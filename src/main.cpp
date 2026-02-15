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

static const uint32_t CURRENT_INTERVAL_MS = 10 * 1000; // 10 sn
static const uint32_t HISTORY_INTERVAL_MS = 60 * 1000; // 60 sn
static const uint32_t CLEANUP_INTERVAL_MS = 60 * 60 * 1000; // 1 saat

uint32_t lastCurrentMs = 0;
uint32_t lastHistoryMs = 0;
uint32_t lastCleanupMs = 0;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi baglaniyor");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi baglanamadi, tekrar denenecek...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }
  Serial.println();
  Serial.print("WiFi baglandi. IP: ");
  Serial.println(WiFi.localIP());
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Saat senkron bekleniyor");
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 1700000000) {
    delay(400);
    Serial.print(".");
    now = time(nullptr);
    if (millis() - start > 20000) {
      Serial.println("\nNTP gecikiyor, tekrar denenecek...");
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
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
    Serial.printf("Firebase auth HATA: %s\n",
                  config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

float readTempC() {
  sensors.requestTemperatures();
  return sensors.getTempCByIndex(0);
}

bool writeCurrent(float tempC, int nowTs) {
  FirebaseJson current;
  current.set("temp", tempC);
  current.set("ts", nowTs);
  current.set("ms", (int)millis());
  return Firebase.RTDB.setJSON(&fbdo, "/sogukoda/current", &current);
}

bool writeHistory(float tempC, int nowTs) {
  FirebaseJson hist;
  hist.set("temp", tempC);
  hist.set("ts", nowTs);

  // history key = timestamp
  String path = "/sogukoda/history/";
  path += String(nowTs);
  return Firebase.RTDB.setJSON(&fbdo, path.c_str(), &hist);
}

void cleanupOldHistory() {
  time_t now = time(nullptr);
  time_t cutoff = now - (24 * 60 * 60); // 24 saat önce

  Serial.println("History temizleniyor...");

  if (Firebase.RTDB.getJSON(&fbdo, "/sogukoda/history")) {
    FirebaseJson &json = fbdo.to<FirebaseJson>();
    size_t len = json.iteratorBegin();
    String key;
    String value;
    int type = 0;
    int deleted = 0;

    for (size_t i = 0; i < len; i++) {
      json.iteratorGet(i, type, key, value);
      long keyTs = key.toInt();
      
      if (keyTs > 0 && keyTs < cutoff) {
        String path = "/sogukoda/history/";
        path += key;
        if (Firebase.RTDB.deleteNode(&fbdo, path.c_str())) {
          deleted++;
        }
      }
    }
    json.iteratorEnd();

    Serial.print("Temizlenen kayit: ");
    Serial.println(deleted);
  } else {
    Serial.print("History okuma HATA: ");
    Serial.println(fbdo.errorReason());
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  sensors.begin();

  connectWiFi();
  setupTime();
  setupFirebase();
}

void loop() {
  // WiFi koparsa toparla
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  uint32_t nowMs = millis();

  // current (10 sn)
  if (nowMs - lastCurrentMs >= CURRENT_INTERVAL_MS) {
    lastCurrentMs = nowMs;

    float tempC = readTempC();
    time_t nowTs = time(nullptr);

    Serial.print("Temp: ");
    Serial.println(tempC);

    if (!writeCurrent(tempC, (int)nowTs)) {
      Serial.print("current HATA: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // history (60 sn)
  if (nowMs - lastHistoryMs >= HISTORY_INTERVAL_MS) {
    lastHistoryMs = nowMs;

    float tempC = readTempC();
    time_t nowTs = time(nullptr);

    if (!writeHistory(tempC, (int)nowTs)) {
      Serial.print("history HATA: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // cleanup (1 saat)
  if (nowMs - lastCleanupMs >= CLEANUP_INTERVAL_MS) {
    lastCleanupMs = nowMs;
    cleanupOldHistory();
  }
}
