# WiFi Thermometer — Soğuk Oda Sıcaklık Takip Sistemi

> **Bu proje aktif olarak bir aile işletmesinde, endüstriyel bir soğuk odanın sıcaklık takibi için üretimde kullanılmaktadır.** Kod üzerinde değişiklik yaparken bunu göz önünde bulundurun; hatalı bir push gerçek bir soğutma sisteminin izlenmesini kesintiye uğratabilir.

ESP32 tabanlı, DS18B20 sıcaklık sensörüyle soğuk oda sıcaklığını okuyup Firebase Realtime Database'e yazan bir izleme sistemi. Sıcaklık, cihaz sağlığı (uptime, WiFi durumu, son yükleme zamanı vb.) ve geçmiş ölçümler buluta aktarılır.

## Donanım

- ESP32 dev board
- DS18B20 sıcaklık sensörü (OneWire, GPIO4)

## Nasıl çalışıyor

1. Cihaz açılışta WiFi'ye bağlanır ve NTP ile saatini senkronize eder.
2. Firebase'e anonim kimlik doğrulaması ile bağlanır.
3. Belirli aralıklarla sıcaklık ölçülüp Firebase Realtime Database'e yazılır:
   - `/sogukoda/current` — anlık sıcaklık (10 sn'de bir)
   - `/sogukoda/history` — zaman damgalı geçmiş kayıtlar (60 sn'de bir)
   - `/sogukoda/health` — uptime, WiFi/RSSI durumu, son yükleme yaşı, reset sebebi (60 sn'de bir)
4. WiFi veya saat senkronizasyonu koparsa cihaz periyodik olarak yeniden dener, veri kaybını asgariye indirmeye çalışır.
5. Eski geçmiş kayıtların temizliği cihaz üzerinde değil, ayrı bir Cloud Functions job'unda yapılır.

## Kurulum

### 1. Gerekli araç

[PlatformIO](https://platformio.org/) (VS Code eklentisi veya CLI).

### 2. Gizli bilgiler (WiFi + Firebase)

WiFi ve Firebase bilgileri kod içinde tutulmaz, `include/secrets.h` dosyasından okunur. Bu dosya `.gitignore` ile repoya dahil edilmez.

```bash
cp include/secrets.h.example include/secrets.h
```

Ardından `include/secrets.h` içine kendi bilgilerinizi girin:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASS "..."
#define API_KEY "..."
#define DATABASE_URL "https://<proje-id>-default-rtdb.firebaseio.com/"
```

`API_KEY` ve `DATABASE_URL`, Firebase projenizin Web API Key ve Realtime Database URL değerleridir.

### 3. Derleme ve yükleme

```bash
pio run -t upload
pio device monitor
```

## Notlar

- ESP32 yalnızca **2.4GHz** WiFi ağlarına bağlanabilir, 5GHz desteklenmez.
- `WIFI_RETRY_INTERVAL_MS`, `HEALTH_INTERVAL_MS` gibi zamanlama sabitleri `src/main.cpp` başında tanımlıdır.
- Elektrik kesintisi / uzun süreli veri gelmemesi bildirimi cihaz üzerinde değil, Cloud Functions tarafında yapılır.
