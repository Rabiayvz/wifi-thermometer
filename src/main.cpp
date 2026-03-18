#include <Arduino.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <esp_system.h>

#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"

// ========= SENSOR =========
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ========= WIFI =========
#define WIFI_SSID "REDACTED_WIFI_SSID"
#define WIFI_PASS "REDACTED_WIFI_PASS"

// ========= FIREBASE =========
#define API_KEY "REDACTED_API_KEY"
#define DATABASE_URL "https://redacted-project-default-rtdb.firebaseio.com/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ========= INTERVALS =========
static const uint32_t CURRENT_INTERVAL_MS        = 10 * 1000;
static const uint32_t HISTORY_INTERVAL_MS        = 60 * 1000;
static const uint32_t WIFI_RETRY_INTERVAL_MS     = 15 * 1000;
static const uint32_t TIME_SYNC_RETRY_INTERVAL_MS= 30 * 1000;
static const uint32_t TIME_SYNC_INTERVAL_MS      = 6 * 60 * 60 * 1000;
static const uint32_t HEALTH_INTERVAL_MS         = 60 * 1000;
static const uint32_t FAILSAFE_RESTART_MS        = 15 * 60 * 1000;

uint32_t lastCurrentMs = 0;
uint32_t lastHistoryMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastTimeSyncAttemptMs = 0;
uint32_t lastTimeSyncOkMs = 0;
uint32_t lastHealthMs = 0;
uint32_t lastSuccessfulUploadMs = 0;

String resetReasonText() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    default:                return "unknown";
  }
}

bool isTimeValid() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

void startWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi baglanti denemesi baslatiliyor...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (nowMs - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) return;

  lastWifiRetryMs = nowMs;

  Serial.println("WiFi bagli degil, yeniden deneniyor...");
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void trySyncTime() {
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t nowMs = millis();
  bool needPeriodicSync = lastTimeSyncOkMs == 0 || (nowMs - lastTimeSyncOkMs >= TIME_SYNC_INTERVAL_MS);
  bool canRetryNow = nowMs - lastTimeSyncAttemptMs >= TIME_SYNC_RETRY_INTERVAL_MS;

  if (!needPeriodicSync || !canRetryNow) return;

  lastTimeSyncAttemptMs = nowMs;

  Serial.println("NTP senkron denemesi...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  if (isTimeValid()) {
    lastTimeSyncOkMs = nowMs;
    Serial.println("Saat OK");
  } else {
    Serial.println("Saat henuz alinmadi, sonra tekrar denenecek.");
  }
}

void setupFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase anonymous auth OK");
  } else {
    Serial.printf("Firebase auth HATA: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(2048);
}

float readTempC() {
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);

  if (t == DEVICE_DISCONNECTED_C || t < -100 || t > 150) {
    return NAN;
  }
  return t;
}

bool writeCurrent(float tempC, time_t nowTs) {
  FirebaseJson current;
  current.set("temp", tempC);
  current.set("ts", (int)nowTs);
  current.set("ms", (int)millis());

  bool ok = Firebase.RTDB.setJSON(&fbdo, "/sogukoda/current", &current);
  if (!ok) {
    Serial.print("current HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}

bool writeHistory(float tempC, time_t nowTs) {
  FirebaseJson hist;
  hist.set("temp", tempC);
  hist.set("ts", (int)nowTs);

  String path = "/sogukoda/history/";
  path += String((uint32_t)nowTs);

  bool ok = Firebase.RTDB.setJSON(&fbdo, path.c_str(), &hist);
  if (!ok) {
    Serial.print("history HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}

bool writeHealth() {
  FirebaseJson health;
  health.set("uptimeSec", (int)(millis() / 1000));
  health.set("wifiConnected", WiFi.status() == WL_CONNECTED);
  health.set("rssi", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -999);
  health.set("timeValid", isTimeValid());
  health.set("resetReason", resetReasonText());
  health.set("lastUploadAgeSec", lastSuccessfulUploadMs == 0 ? -1 : (int)((millis() - lastSuccessfulUploadMs) / 1000));

  bool ok = Firebase.RTDB.setJSON(&fbdo, "/sogukoda/health", &health);
  if (!ok) {
    Serial.print("health HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}

void maybeRestartIfStuck() {
  if (lastSuccessfulUploadMs == 0) return;

  uint32_t nowMs = millis();
  if (nowMs - lastSuccessfulUploadMs > FAILSAFE_RESTART_MS) {
    Serial.println("Uzun suredir veri gonderilemiyor. ESP yeniden baslatiliyor...");
    delay(1000);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  sensors.begin();

  Serial.print("Reset reason: ");
  Serial.println(resetReasonText());

  startWiFiIfNeeded();
  setupFirebase();

  lastSuccessfulUploadMs = millis();
}

void loop() {
  uint32_t nowMs = millis();

  ensureWiFi();
  trySyncTime();

  if (WiFi.status() == WL_CONNECTED && !Firebase.ready()) {
    Serial.println("Firebase hazir degil, bekleniyor...");
  }

  bool shouldWriteCurrent = nowMs - lastCurrentMs >= CURRENT_INTERVAL_MS;
  bool shouldWriteHistory = nowMs - lastHistoryMs >= HISTORY_INTERVAL_MS;

  if (shouldWriteCurrent || shouldWriteHistory) {
    float tempC = readTempC();

    if (isnan(tempC)) {
      Serial.println("Sensor okunamadi, olcum atlandi.");
    } else if (!isTimeValid()) {
      Serial.println("Saat gecerli degil, veri gonderimi ertelendi.");
    } else if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t nowTs = time(nullptr);

      if (shouldWriteCurrent) {
        lastCurrentMs = nowMs;
        Serial.print("Temp: ");
        Serial.println(tempC);

        if (writeCurrent(tempC, nowTs)) {
          lastSuccessfulUploadMs = millis();
        }
      }

      if (shouldWriteHistory) {
        lastHistoryMs = nowMs;

        if (writeHistory(tempC, nowTs)) {
          lastSuccessfulUploadMs = millis();
        }
      }
    } else {
      Serial.println("Veri yazilamadi: WiFi/Firebase hazir degil.");
    }
  }

  if (nowMs - lastHealthMs >= HEALTH_INTERVAL_MS) {
    lastHealthMs = nowMs;

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      if (writeHealth()) {
        lastSuccessfulUploadMs = millis();
      }
    }
  }

  maybeRestartIfStuck();
  delay(50);
}