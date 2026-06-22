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
static const uint32_t CURRENT_INTERVAL_MS         = 10 * 1000;
static const uint32_t HISTORY_INTERVAL_MS         = 60 * 1000;
static const uint32_t WIFI_RETRY_INTERVAL_MS      = 15 * 1000;
static const uint32_t TIME_SYNC_RETRY_INTERVAL_MS = 30 * 1000;
static const uint32_t TIME_SYNC_INTERVAL_MS       = 6 * 60 * 60 * 1000;
static const uint32_t HEALTH_INTERVAL_MS          = 60 * 1000;
static const uint32_t FAILSAFE_RESTART_MS         = 30 * 60 * 1000; // 15'ten 30 dk'ya çıkarıldı
// Cleanup Cloud Functions'a taşındı.

uint32_t lastCurrentMs          = 0;
uint32_t lastHistoryMs          = 0;
uint32_t lastWifiRetryMs        = 0;
uint32_t lastTimeSyncAttemptMs  = 0;
uint32_t lastTimeSyncOkMs       = 0;
uint32_t lastHealthMs           = 0;
uint32_t lastSuccessfulUploadMs = 0;


// =========================================================
// Yardimci fonksiyonlar
// =========================================================

String resetReasonText()
{
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason)
  {
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

bool isTimeValid()
{
  time_t now = time(nullptr);
  return now > 1700000000;
}

// =========================================================
// WiFi
// =========================================================

void connectWiFiBlocking()
{
  Serial.println("WiFi baglaniyor...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");

    if (millis() - start > 20000)
    {
      Serial.println("\nYeniden deneniyor...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }

  Serial.println();
  Serial.print("WiFi baglandi. IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
}

void ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  uint32_t nowMs = millis();
  if (nowMs - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS)
    return;

  lastWifiRetryMs = nowMs;
  Serial.println("WiFi koptu, yeniden baglaniyor...");
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// =========================================================
// NTP
// =========================================================

void trySyncTime()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  uint32_t nowMs = millis();
  bool needPeriodicSync = (lastTimeSyncOkMs == 0) ||
                          (nowMs - lastTimeSyncOkMs >= TIME_SYNC_INTERVAL_MS);
  bool canRetryNow = (nowMs - lastTimeSyncAttemptMs >= TIME_SYNC_RETRY_INTERVAL_MS);

  if (!needPeriodicSync || !canRetryNow)
    return;

  lastTimeSyncAttemptMs = nowMs;
  Serial.println("NTP senkron denemesi...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  uint32_t ntpStart = millis();
  while (!isTimeValid() && millis() - ntpStart < 5000)
  {
    delay(200);
  }

  if (isTimeValid())
  {
    lastTimeSyncOkMs = nowMs;
    Serial.println("Saat OK");
  }
  else
  {
    Serial.println("Saat alinamadi, sonra tekrar denenecek.");
  }
}

// =========================================================
// Firebase
// FIX: signUp kaldırıldı — her restart'ta yeni kullanıcı
//      açılmasına ve token döngüsüne neden oluyordu.
// =========================================================

void setupFirebase()
{
  config.api_key        = API_KEY;
  config.database_url   = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  if (Firebase.signUp(&config, &auth, "", ""))
  {
    Serial.println("Firebase anonymous auth OK");
  }
  else
  {
    Serial.printf("Firebase auth HATA: %s\n",
                  config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(2048);

  Serial.println("Firebase baslatildi.");
}

// =========================================================
// Sensor
// =========================================================

float readTempC()
{
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C || t < -100 || t > 150)
    return NAN;
  return t;
}

// =========================================================
// Firebase yazma
// =========================================================

bool writeCurrent(float tempC, time_t nowTs)
{
  FirebaseJson current;
  current.set("temp", tempC);
  current.set("ts",   (int)nowTs);
  current.set("ms",   (int)millis());

  bool ok = Firebase.RTDB.setJSON(&fbdo, "/sogukoda/current", &current);
  if (!ok)
  {
    Serial.print("current HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}

bool writeHistory(float tempC, time_t nowTs)
{
  FirebaseJson hist;
  hist.set("temp", tempC);
  hist.set("ts",   (int)nowTs);

  String path = "/sogukoda/history/";
  path += String((uint32_t)nowTs);

  bool ok = Firebase.RTDB.setJSON(&fbdo, path.c_str(), &hist);
  if (!ok)
  {
    Serial.print("history HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}

bool writeHealth()
{
  FirebaseJson health;
  health.set("uptimeSec",       (int)(millis() / 1000));
  health.set("wifiConnected",   WiFi.status() == WL_CONNECTED);
  health.set("rssi",            WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -999);
  health.set("timeValid",       isTimeValid());
  health.set("resetReason",     resetReasonText());
  health.set("lastUploadAgeSec",
    lastSuccessfulUploadMs == 0 ? -1 : (int)((millis() - lastSuccessfulUploadMs) / 1000));

  bool ok = Firebase.RTDB.setJSON(&fbdo, "/sogukoda/health", &health);
  if (!ok)
  {
    Serial.print("health HATA: ");
    Serial.println(fbdo.errorReason());
  }
  return ok;
}



// =========================================================
// Failsafe
// FIX: WiFi bağlıyken bile restart atıyordu. Artık sadece
//      WiFi de bağlı olduğu halde upload yoksa restart atar.
//      Süre 15 dk'dan 30 dk'ya çıkarıldı.
// =========================================================

void maybeRestartIfStuck()
{
  // DEVRE DIŞI — signUp döngüsüne neden oluyordu.
  // Elektrik kesintisi bildirimi Cloud Functions üzerinden yapılıyor,
  // ESP32'nin kendini restart atmasına gerek yok.
}

// =========================================================
// SETUP
// =========================================================

void setup()
{
  Serial.begin(115200);
  delay(500);

  sensors.begin();

  Serial.print("Reset reason: ");
  Serial.println(resetReasonText());

  connectWiFiBlocking();

  Serial.println("NTP baslatiliyor...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  uint32_t ntpStart = millis();
  while (!isTimeValid() && millis() - ntpStart < 10000)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (isTimeValid())
  {
    lastTimeSyncOkMs = millis();
    Serial.println("NTP OK");
  }
  else
  {
    Serial.println("NTP alinamadi, loop'ta tekrar denenecek.");
  }

  setupFirebase();

  lastSuccessfulUploadMs = millis();
  lastWifiRetryMs        = millis();
  lastTimeSyncAttemptMs  = millis();
}

// =========================================================
// LOOP
// =========================================================

void loop()
{
  uint32_t nowMs = millis();

  ensureWiFi();
  trySyncTime();

  if (WiFi.status() == WL_CONNECTED && !Firebase.ready())
  {
    Serial.println("Firebase hazir degil, bekleniyor...");
  }

  bool shouldWriteCurrent = (nowMs - lastCurrentMs >= CURRENT_INTERVAL_MS);
  bool shouldWriteHistory = (nowMs - lastHistoryMs  >= HISTORY_INTERVAL_MS);

  if (shouldWriteCurrent || shouldWriteHistory)
  {
    float tempC = readTempC();

    if (isnan(tempC))
    {
      Serial.println("Sensor okunamadi, olcum atlandi.");
      if (shouldWriteCurrent) lastCurrentMs = nowMs;
      if (shouldWriteHistory) lastHistoryMs  = nowMs;
    }
    else if (!isTimeValid())
    {
      Serial.println("Saat gecerli degil, veri gonderimi ertelendi.");
    }
    else if (WiFi.status() == WL_CONNECTED && Firebase.ready())
    {
      time_t nowTs = time(nullptr);

      if (shouldWriteCurrent)
      {
        lastCurrentMs = nowMs;
        Serial.print("Temp: ");
        Serial.println(tempC);

        if (writeCurrent(tempC, nowTs))
          lastSuccessfulUploadMs = millis();
      }

      if (shouldWriteHistory)
      {
        lastHistoryMs = nowMs;
        if (writeHistory(tempC, nowTs))
          lastSuccessfulUploadMs = millis();
      }
    }
    else
    {
      Serial.println("Veri yazilamadi: WiFi/Firebase hazir degil.");
    }
  }

  if (nowMs - lastHealthMs >= HEALTH_INTERVAL_MS)
  {
    lastHealthMs = nowMs;
    if (WiFi.status() == WL_CONNECTED && Firebase.ready())
    {
      if (writeHealth())
        lastSuccessfulUploadMs = millis();
    }
  }

  maybeRestartIfStuck();

  delay(50);
}