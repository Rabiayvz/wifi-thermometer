#include <Arduino.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"

// ========= SENSÖR =========
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

// ========= ZAMANLAR =========
static const uint32_t CURRENT_INTERVAL_MS      = 10 * 1000;       // 10 sn
static const uint32_t HISTORY_INTERVAL_MS      = 60 * 1000;       // 60 sn
static const uint32_t WIFI_CHECK_INTERVAL_MS   = 15 * 1000;       // 15 sn
static const uint32_t TIME_SYNC_INTERVAL_MS    = 6 * 60 * 60 * 1000; // 6 saat
static const uint32_t FAILSAFE_RESTART_MS      = 15 * 60 * 1000;  // 15 dk veri gidemezse restart

uint32_t lastCurrentMs = 0;
uint32_t lastHistoryMs = 0;
uint32_t lastWifiCheckMs = 0;
uint32_t lastTimeSyncMs = 0;

uint32_t lastSuccessfulUploadMs = 0;

// ---------- Yardımcılar ----------
bool isTimeValid() {
  time_t now = time(nullptr);
  return now > 1700000000; // yaklaşık 2023 sonrası
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // bağlantıyı daha stabil yapar
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi baglaniyor");
  uint32_t start = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi baglanamadi, modem bekleniyor...");
      WiFi.disconnect(true, true);
      delay(2000);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }

  Serial.println();
  Serial.print("WiFi baglandi. IP: ");
  Serial.println(WiFi.localIP());
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi koptu, yeniden baglaniliyor...");
  WiFi.disconnect(true, true);
  delay(1000);
  connectWiFi();
}

void syncTime() {
  Serial.println("NTP senkronu basliyor...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  uint32_t start = millis();
  while (!isTimeValid()) {
    delay(400);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nNTP uzun surdu, tekrar denenecek...");
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      start = millis();
    }
  }
  Serial.println("\nSaat OK");
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
  fbdo.setResponseSize(2048); // gereksiz büyük buffer olmasın
}

bool ensureFirebaseReady(uint32_t timeoutMs = 10000) {
  uint32_t start = millis();
  while (!Firebase.ready()) {
    delay(100);
    if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

float readTempC() {
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);

  // Dallas sensör hata değerleri
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

  connectWiFi();
  syncTime();
  setupFirebase();

  lastSuccessfulUploadMs = millis();
}

void loop() {
  uint32_t nowMs = millis();

  // WiFi kontrol
  if (nowMs - lastWifiCheckMs >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheckMs = nowMs;
    ensureWiFi();
  }

  // Saati ara sıra tazele
  if (nowMs - lastTimeSyncMs >= TIME_SYNC_INTERVAL_MS) {
    lastTimeSyncMs = nowMs;
    if (WiFi.status() == WL_CONNECTED) {
      syncTime();
    }
  }

  // Zaman geçerli değilse veri gönderme
  if (!isTimeValid()) {
    Serial.println("Saat henuz gecerli degil, NTP bekleniyor...");
    delay(1000);
    return;
  }

  // Firebase hazır değilse biraz bekle
  if (WiFi.status() == WL_CONNECTED && !Firebase.ready()) {
    Serial.println("Firebase hazir degil, bekleniyor...");
    ensureFirebaseReady(3000);
  }

  // current
  if (nowMs - lastCurrentMs >= CURRENT_INTERVAL_MS) {
    lastCurrentMs = nowMs;

    float tempC = readTempC();
    if (isnan(tempC)) {
      Serial.println("Sensor okunamadi, current atlandi.");
    } else if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t nowTs = time(nullptr);

      Serial.print("Temp: ");
      Serial.println(tempC);

      if (writeCurrent(tempC, nowTs)) {
        lastSuccessfulUploadMs = millis();
      }
    } else {
      Serial.println("current yazilamadi: WiFi/Firebase hazir degil.");
    }
  }

  // history
  if (nowMs - lastHistoryMs >= HISTORY_INTERVAL_MS) {
    lastHistoryMs = nowMs;

    float tempC = readTempC();
    if (isnan(tempC)) {
      Serial.println("Sensor okunamadi, history atlandi.");
    } else if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t nowTs = time(nullptr);

      if (writeHistory(tempC, nowTs)) {
        lastSuccessfulUploadMs = millis();
      }
    } else {
      Serial.println("history yazilamadi: WiFi/Firebase hazir degil.");
    }
  }

  maybeRestartIfStuck();
  delay(50);
}