# Multi Sensor ESP32 (BME280 + DHT22 + GPS)

Sketch ESP32 untuk monitoring lingkungan dengan:
- **BME280** (suhu, kelembaban, tekanan)
- **DHT22** (fallback suhu & kelembaban)
- **GPS** (TinyGPSPlus, auto-baud detect)
- **Dual LCD I2C** (20x4 + 16x2)
- **Web Dashboard** (gauge + chart + status)
- **Daily CSV Logging** ke SPIFFS
- **Configurable log retention** (1..60 hari)
- **Auto reboot harian jam 03:00**
- **AP + STA mode** (tetap bisa akses lokal)

---

## Fitur Utama

- Dashboard real-time via web (`/`)
- API JSON (`/api`)
- Download log CSV (`/download?f=...`)
- View 30 baris terakhir (`/viewlog?f=...`)
- Auto hapus log lama sesuai retention
- Retention bisa diubah dari dashboard
- WiFi credential disimpan di `Preferences`
- Baseline pressure untuk **ALT REL** (kalibrasi 15 detik)

---

## Pin Mapping (ESP32)

### I2C
- `SDA = GPIO21`
- `SCL = GPIO22`

### DHT22
- `DATA = GPIO23`

### GPS (UART2)
- `RX2 = GPIO16` (ESP32 menerima dari TX GPS)
- `TX2 = GPIO17` (ESP32 kirim ke RX GPS, opsional)

---

## I2C Address yang dipakai

- LCD 20x4: `0x27`
- LCD 16x2: `0x26`
- BME280: auto scan `0x76` / `0x77`

> Jika hardware kamu berbeda, ubah di sketch.

---

## Library yang Dibutuhkan

Install via Arduino IDE Library Manager:

- `WiFi` (ESP32 core)
- `WebServer` (ESP32 core)
- `LiquidCrystal_I2C`
- `Adafruit BME280 Library`
- `Adafruit Unified Sensor`
- `DHT sensor library`
- `TinyGPSPlus`

> Pastikan **TinyGPSPlus tidak dobel** di folder libraries.

---

## Konfigurasi AP Default

```cpp
const char* AP_SSID = "MULTI SENSOR";
const char* AP_PASS = "12345678";
```

Saat boot:
- ESP32 selalu membuat AP
- Jika ada kredensial tersimpan, ESP32 mencoba konek STA

---

## Endpoint Web

- `/` : Dashboard utama
- `/api` : Data JSON real-time
- `/listlogs` : Daftar file log
- `/download?f=/log_YYYYMMDD.csv` : Download file CSV
- `/viewlog?f=/log_YYYYMMDD.csv` : Preview 30 baris terakhir
- `/savewifi` (POST) : Simpan SSID/password
- `/clearwifi` (POST) : Hapus WiFi tersimpan
- `/recalibrate` (POST) : Kalibrasi ulang baseline ALT
- `/clearlogtoday` (POST) : Hapus log hari ini
- `/setretention` (POST) : Set retention 1..60 hari

---

## Format JSON `/api` (ringkas)

Contoh field:
- `time`
- `ap_ip`
- `sta_status`, `sta_ip`
- `temperature_c`, `humidity_pct`, `pressure_hpa`
- `alt_rel_m`, `baseline_ready`
- `gps_fix`, `gps_sat`, `gps_lat`, `gps_lon`
- `gps_chars`, `gps_sentences_fix`, `gps_failed_checksum`
- `gps_baud`, `gps_baud_locked`
- `log_retention_days`

---

## Logging

- Nama file: `/log_YYYYMMDD.csv`
- Header otomatis dibuat jika file belum ada
- Interval log default: `5000 ms`
- Retention default: `7 hari` (bisa diubah di dashboard)

---

## Auto Reboot Harian

Device restart otomatis **setiap hari pukul 03:00** (berdasarkan NTP lokal WIB).
Tujuan: menjaga stabilitas perangkat untuk operasi 24/7.

---

## Build & Upload

1. Pilih board: **ESP32 Dev Module**
2. Flash mode default (sesuai board)
3. Upload sketch `.ino`
4. Buka Serial Monitor `115200`
5. Akses dashboard via:
   - AP IP (umumnya `192.168.4.1`), atau
   - IP STA jika terkoneksi router

---

## Troubleshooting

### 1) Compile error `TinyGPSPlus.h` / multiple libraries found
Hapus library duplikat TinyGPSPlus, sisakan satu yang aktif.

### 2) `expected ';' at end of input`
Biasanya ada fungsi yang terpotong (sering di `handleApi()`).
Gunakan sketch final utuh, jangan potong sebagian.

### 3) GPS tidak fix
- Pastikan antena GPS dapat pandangan langit
- Tunggu cold start beberapa menit
- Cek wiring TX/RX

### 4) Waktu masih 1970
NTP belum sinkron. Pastikan STA internet tersedia (atau tunggu retry NTP).

---

## Lisensi

Gunakan bebas untuk riset, edukasi, dan pengembangan lanjutan.
