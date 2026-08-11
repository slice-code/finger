# AGENTS.md — FPM10A Fingerprint Attendance System

## Project Overview

Two hardware variants exist:

### NodeMCU (ESP8266)
- **FPM10A sensor** via SoftwareSerial (D1=RX, D2=TX)
- **ILI9341 TFT LCD** via SPI (CS=D8, DC=D3, RST=D4, BL=D6, SCK=D5, MOSI=D7)
- **Web UI** (tabs: Dashboard, Daftar, Data, WiFi, Setelan, Akun, Cadangan)
- **LittleFS** for persistent config

### ESP32 (versi 5V)
- **FPM10A sensor** via HardwareSerial2 (RX=GPIO16, TX=GPIO17)
- **ILI9341 TFT LCD** via SPI (CS=GPIO5, DC=GPIO2, RST=GPIO4, BL=GPIO15, SCK=GPIO18, MOSI=GPIO23)
- **Web UI** (same tabs) + Select2 dropdown search for branch/employee
- **LittleFS** for persistent config
- **NTP time** for attendance timestamps

### ESP32 (versi 3V3)
- **FPM10A 3.3V** via HardwareSerial2 (RX=GPIO16/RX2, TX=GPIO17/TX2)
- **ILI9341 TFT LCD** via SPI (pin sama dengan 5V)
- **Adjusted parameters** for weaker 3.3V signal (lihat tabel di bawah)
- Wiring: RX2 → FPM10A TX, TX2 → FPM10A RX, 3V3 → VCC, GND → GND
- **LittleFS** via `#include <LittleFS.h>` pada partisi `littlefs` (jangan alias ke SPIFFS)
- Storage file: `/wifi.json`, `/settings.json`, `/credentials.json`, `/fingerprints.json`, `/errors.log`

## Build & Upload

### NodeMCU (ESP8266)
```bash
# Compile
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 /home/gugus/Documents/Project/pjtki/arduino/nodemcu_bridge/nodemcu_bridge.ino
# Upload (serial port /dev/ttyACM0)
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 --port /dev/ttyACM0 /home/gugus/Documents/Project/pjtki/arduino/nodemcu_bridge/nodemcu_bridge.ino
```

### ESP32
```bash
# Compile (versi 5V)
arduino-cli compile --fqbn esp32:esp32:esp32 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-5v/esp32-fpm10a-versi-5v.ino
# Upload (serial port /dev/ttyUSB0)
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-5v/esp32-fpm10a-versi-5v.ino
```

### ESP32 (versi 3V3)
```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota_lfs /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-3v3/esp32-fpm10a-versi-3v3.ino
# Upload (serial port /dev/ttyUSB0)
arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=no_ota_lfs --port /dev/ttyUSB0 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-3v3/esp32-fpm10a-versi-3v3.ino
```

**Storage/partition rules:**
- Use the sketch-local `partitions.csv`; the data partition is `littlefs, data, littlefs, 0x210000, 0x1E0000`.
- Normal upload writes bootloader, partition table, and app only; it does not write the LittleFS data partition.
- **Never use `erase-flash` for normal firmware updates** because it removes WiFi credentials, settings, fingerprint metadata, and error logs.
- Do not replace `LittleFS.h` with `SPIFFS.h` and do not add `#define LittleFS SPIFFS`. The installed `no_ota_lfs` partition has subtype/label `littlefs`.
- Boot must report `[FS] LittleFS mounted OK`; if storage is unavailable, save handlers must not report success.

## FPM10A Finger Confirm Counter (ESP32 only)

**Problem**: FPM10A sensor on ESP32 can be overly sensitive — `getImage()` returns `FINGERPRINT_OK` (0) even when no finger is touching the sensor, or when a finger is only nearby (not yet pressed). This causes false scan triggers and continuous "NO MATCH" results.

**Solution**: Debounce mechanism using `fingerConfirm` counter in `doAutoScan()`:

```cpp
static int fingerConfirm = 0;
static const int FINGER_CONFIRM_NEEDED = 2;  // current 3V3 firmware; tune only after hardware test

case SCAN_IDLE:
  if (p == FINGERPRINT_OK) {
    fingerConfirm++;
    if (fingerConfirm < FINGER_CONFIRM_NEEDED) {
      scanCooldownUntil = millis() + 30;  // current 3V3 confirmation spacing
      break;  // not enough confirmations yet, keep polling
    }
    fingerConfirm = 0;  // confirmed! proceed to scan
    scanState = SCAN_BUSY;
  }
  else if (p == FINGERPRINT_NOFINGER) {
    fingerConfirm = 0;  // reset counter when finger removed
  }
```

**How it works**:
1. Each `getImage()` returning OK increments `fingerConfirm`
2. Need `FINGER_CONFIRM_NEEDED` (2) consecutive OK reads before processing
3. Between confirmations: 50ms cooldown (~100ms total debounce)
4. Any NOFINGER or error resets the counter to 0
5. Counter also reset after scan completes (WAIT_RELEASE → IDLE)
6. Distant/transient detections (<2 consecutive reads) are silently ignored

**Tuning** (if needed):
- Increase `FINGER_CONFIRM_NEEDED` → more debounce, slower response
- Decrease → faster response, more false triggers
- Adjust `scanCooldownUntil = millis() + 50` → longer = more delay between confirm checks

**DO NOT remove this counter** — the FPM10A has a known hardware sensitivity issue where it detects fingers before contact.

### Touch-Presence Gate (T-OUT FPM10A / IR obstacle) — ESP32 only

**Sumber sinyal gate** untuk `getImage()` — sensor hanya dipoll saat ada jari, memotong chatter UART & false trigger FPM10A over-sensitive:

- **(a) T-OUT FPM10A — REKOMENDASI**: capacitive touch output, aktif HIGH saat jari menyentuh kaca. Wiring: `T-3V3→3V3` (level T-OUT = tegangan T-3V3, wajib 3.3V), `T-OUT→GPIO13` (`TOUCH_PIN`). CMOS push-pull, stabil, tak kena feedback lampu FPM10A. IC Tontek TTP233D: stabilisasi ~0.5s + auto-calibrate ~1s setelah power-on.
- **(b) Modul IR obstacle 3-pin** (VCC/GND/OUT, LM393 + trimpot): `VCC→3V3`, `GND→GND`, `OUT→GPIO13`. Lebih rewel — bisa flicker / feedback dari LED capture FPM10A.

Perilaku & proteksi:
- Polaritas (active-high vs active-low) di-detect **otomatis saat boot** (`irCalibrate()` — sampling 20×25ms, **jangan sentuh sensor selama kalibrasi**). Dipanggil di **akhir** setup setelah deteksi sensor (~20s) supaya TTP233D sudah stabil
- `irUpdateGate()`: debounce — butuh `IR_CONFIRM_MS` (100ms) deteksi kontinu untuk BUKA gate, `IR_RELEASE_MS` (400ms) kosong untuk TUTUP
- `IR_GATE_TIMEOUT_MS` (3s): gate terbuka lama tanpa hasil → tutup sementara (putus feedback LED FPM10A), `IR_GATE_COOLDOWN_MS` (2.5s) sebelum evaluasi ulang
- `FALLBACK_POLL_MS` (1.5s): saat gate tutup, gunakan `getTemplateCount()` sebagai ping tanpa LED; hasil ping harus diperiksa
- Saat gate false (standby) → `getImage()` di-skip total; `lastScanActivity` hanya diperbarui jika ping sensor berhasil
- Toggle via tab Setelan web UI (`ir_enabled` di `/settings.json`, default **true**). Jika nonaktif → gate terbuka, perilaku kembali ke polling normal
- Backlight dinyalakan otomatis saat gate terbuka (jari terkonfirmasi)
- `fingerConfirm` counter tetap dipertahankan sebagai lapisan keamanan kedua

**Rules**:
1. Jangan hapus gate — ini mengurangi false trigger FPM10A over-sensitive & chatter UART di 3.3V
2. Jangan ganti pin GPIO13 ke pin ADC-only (35/34/36/39 tidak punya pull-up) atau pin strapping (0/12)
3. Jika gate di-disable, pastikan fallback polling tetap jalan (gate terbuka = `return true`)
4. T-3V3 wajib 3.3V (bukan 5V) supaya T-OUT tidak melebihi level GPIO ESP32

### FPM10A LED Control (0x50/0x51) — ESP32 only

**Sumber**: Adafruit library PR [#45](https://github.com/adafruit/Adafruit-Fingerprint-Sensor-Library/pull/45) & [#79](https://github.com/adafruit/Adafruit-Fingerprint-Sensor-Library/pull/79) — FPM10A klon **baru** (LED hijau/kuning) **tidak punya kontrol LED otomatis** → LED menyala terus saat idle. Bisa dikontrol via:
- `finger.LEDcontrol(true)` — nyalakan LED (0x50)
- `finger.LEDcontrol(false)` — matikan LED (0x51)

Catatan penting dari diskusi developer:
- Butuh **delay ~100ms setelah nyalakan LED** sebelum `getImage()` — lampu harus stabil dulu
- LED **wajib nyala** saat capture — ini lampu penerangan sidik jari, tanpanya capture gagal
- Tidak semua klon mendukung perintah ini; klon lama mungkin mengabaikan 0x50/0x51

**Implementasi di sini**:
- **Boot**: `finger.LEDcontrol(false)` → lampu mati total
- **Gate terbuka** (jari menyentuh via T-OUT): `LEDcontrol(true)` + `LED_WARMUP_MS` (50ms) → `getImage()`
- **Gate tertutup** (idle / jari diangkat): `LEDcontrol(false)` hanya pada transisi ON→OFF; retry maksimal setiap 2s jika ACK gagal
- **Fallback standby**: `finger.getTemplateCount()` sebagai ping (tanpa LED) — **bukan** `getImage()` supaya lampu tidak kedip setiap 1.5s
- **Enroll**: LED ON sebelum `waitFinger()`, LED OFF di `enrollCleanupResumeScan()`
- **ReinitSensor / AutoOff**: LED dijamin mati saat restart/setop autoscan
- Variabel global `bool ledOn` melacak state LED supaya tidak kirim perintah berulang
- Error LED OFF dicatat ke `/errors.log`; perintah OFF berulang saat LED sudah mati dilarang karena menambah chatter UART

**Peringatan lama AGENTS.md** ("Jangan pakai LEDcontrol() bisa membuat scan berulang") **tidak berlaku** untuk klon ini — root cause crash asli (use-after-free WiFiClientSecure) sudah diperbaiki 2026-08-06, dan pengujian menunjukkan LED control bekerja stabil tanpa memicu re-scan.

### LCD Backlight PWM Dim — ESP32 only

Backlight ILI9341 dikontrol via PWM `ledc` pada GPIO15 **tanpa transistor eksternal** (modul LCD sudah ada driver). Perilaku:

| State | Backlight | `ledcWrite` |
|---|---|---|
| Idle (gate tertutup) | **Redup 12%** | `ledcWrite(LCD_BL, 30)` |
| Jari menyentuh (gate terbuka) | **100%** | `ledcWrite(LCD_BL, 255)` |
| 60 detik tanpa aktivitas | **Mati total** | `ledcWrite(LCD_BL, 0)` |

PWM: `ledcAttach(LCD_BL, 250, 8)` — 250Hz, 8-bit (0–255). Tidak pakai `pinMode(OUTPUT)` — `ledcAttach` sudah menangani konfigurasi pin.

Wiring (lihat `cable.txt`):
```
GPIO15 → pin "LED" modul LCD  (langsung, tanpa resistor/transistor)
VCC LCD → VIN 5V ESP32        (bukan 3.3V — kurangi beban regulator)
```

- Modul ILI9341 punya regulator 3.3V + driver LED sendiri → aman dikasih 5V VCC
- Backlight + chip LCD ambil dari 5V → regulator 3.3V ESP32 lebih adem
- **Tidak perlu TIP41C atau transistor eksternal** — pin "LED" sudah melewati driver di PCB LCD

### FPM10A 3.3V vs 5V Parameter Differences

| Parameter | 5V | 3.3V | Reason |
|---|---|---|---|
| `FINGER_CONFIRM_NEEDED` | 2 | **1** | 3.3V touch sensor weaker, can't sustain 2x OK reads |
| `scanCooldownUntil` | `+50ms` | **+100ms** | More relaxed polling interval |
| `FINGERPRINT_SECURITY_LEVEL` | 3 | **2** | Less detailed images at 3.3V |
| `SCAN_WATCHDOG_MS` | 15000 | **20000** | 3.3V sensor slower, more timeout tolerance |
| `MAX_CONSECUTIVE_ERRORS` | 5 | **8** | 3.3V more transient errors, don't restart too quickly |
| Error throttle `delay()` | 200ms | **300ms** | Give more breathing room |
| Default baud rate | 9600 | **57600** | Chinese 3.3V FPM10A commonly default to 57600 |
| Baud detection order | `{9600, 57600, 19200, 38400, 115200}` | **`{57600, 9600, 19200, 38400}`** | Skip 115200 (too unstable at 3.3V) |
| Power-on stabilization | 2000ms | **3000ms** | 3.3V needs longer to reach stable voltage |
| Inter-baud `delay()` | 150-200ms | **250ms** | More time for UART reinit at 3.3V |

## Serial Debug Monitor

### ESP32 (/dev/ttyUSB0, 9600 baud)
```bash
python3 -c "
import serial, time
ser = serial.Serial('/dev/ttyUSB0', 9600, timeout=0.5)
start = time.time()
while time.time() - start < 120:
    line = ser.readline().decode('utf-8', errors='replace').strip()
    if line: print(f'[{time.time()-start:6.2f}s] {line}')
ser.close()
"
```

### NodeMCU (/dev/ttyACM0, 9600 baud)
```bash
python3 -c "
import serial, time
ser = serial.Serial('/dev/ttyACM0', 9600, timeout=0.5)
start = time.time()
while time.time() - start < 120:
    line = ser.readline().decode('utf-8', errors='replace').strip()
    if line: print(f'[{time.time()-start:6.2f}s] {line}')
ser.close()
"
```

## ⚠️ CRITICAL: WiFi + SoftwareSerial Timing Issue

**Root Cause**: ESP8266 WiFi interrupts disrupt SoftwareSerial RX timing, causing FPM10A sensor detection to fail intermittently. The sensor may fail ALL baud rates (9600, 57600, 19200, 38400, 115200) even though hardware is fine.

**Solution**: WiFi must be turned OFF during sensor detection/reinit:
```cpp
WiFi.disconnect(true);
WiFi.mode(WIFI_OFF);
delay(100);
// ... detect sensor ...
WiFi.mode(WIFI_AP_STA); // re-enable after detection
```

**Symptoms if WiFi is NOT disabled**:
- Serial garbled characters during baud transitions (`roun??2`, `57600??`)
- `verifyPassword()` fails at ALL baud rates
- After successful scan, communication breaks with `FINGERPRINT_PACKETRECIEVEERR` (error code 1)
- Full power cycle (USB disconnect/reconnect) fixes temporarily

**IMPORTANT RULES FOR FUTURE EDITS**:
1. **NEVER remove the WiFi OFF/ON pattern** in `setup()` sensor detection or `reinitSensor()`
2. **Do NOT change the 2s pre-detect delay** — sensor needs power-on stabilization
3. **Do NOT reduce inter-baud delays** below 150ms — ESP8266 needs time to reconfigure SoftwareSerial
4. **Do NOT add blocking loops** during sensor communication — use state machine pattern
5. **If adding new WiFi features**, ensure they don't trigger heavy background tasks during sensor scans

## Recovery Mechanism

The sensor uses WiFi SoftwareSerial which is inherently unreliable on ESP8266. Two failure modes exist:

### Mode 1: Scanning Error → ESP.restart()
When WiFi interrupts corrupt scanning communication:
1. `getImage()` returns error codes → `consecutiveErrors` increments
2. After `MAX_CONSECUTIVE_ERRORS` (5) → **immediate `ESP.restart()`**
3. Boot sequence re-detects sensor with WiFi OFF → works reliably
4. WiFi reconnects after sensor is ready

### Mode 2: Boot Detection Failure → Retry then Restart
When sensor can't be detected at boot:
1. `reinitSensor()` called every 5s with WiFi OFF + 3s cooldown
2. Phase 1: Try 9600 baud 8 times (FPM10A default)
3. Phase 2: Try all 5 bauds × 5 attempts each
4. After `MAX_RECOVERY` (3) failed reinit attempts → **`ESP.restart()`**

### Why reinitSensor() alone can't recover
When sensor communication breaks mid-scan, the FPM10A enters an unrecoverable state. Even with WiFi OFF, `verifyPassword()` fails at ALL baud rates. Only a full power cycle (or ESP restart) resets the sensor properly. This is a known limitation of ESP8266 + SoftwareSerial + FPM10A.

## Key State Variables
- `autoScan` (bool): Enable/disable automatic scanning
- `sensorReady` (bool): Sensor communication is working
- `scanState` (enum): `SCAN_IDLE` → `SCAN_BUSY` → `SCAN_WAIT_RELEASE`
- `consecutiveErrors` (int): Count of consecutive `getImage()` failures (max 5 before ESP.restart())
- `lastScanActivity` (unsigned long): Timestamp of last sensor activity (watchdog 15s)
- `recoveryCount` (int): Recovery attempts before ESP.restart() (max 3)



## Sinkron Template DB → Sensor (ESP32)

Tab Cadangan: user **memilih** anak (filter cabang opsional + search, lintas cabang OK) lalu sync hanya yang dicentang.

1. UI `GET /api/server/templates?kode_cabang=&q=` → device proxy ke `GET {apiBaseUrl}/api/finger/arduino/templates` (meta, hanya yang `has_hex`)
2. UI `POST /api/sync/from-server` body `{ employeeIds: [...] }` (max 30)
3. Device per ID: `GET {apiBaseUrl}/api/finger/arduino/template/:employeeId` → hex → `putTemplateRaw` + `dbAdd` (skip jika sudah di LittleFS)

API batch PJTKI (`POST /api/finger/arduino/templates/batch`) tersedia untuk client lain; ESP pakai GET per-ID agar hemat RAM.

Enroll ESP wajib upload hex (`getTemplateRaw` → `postRegister(..., templateHex)`) supaya template bisa di-sync ke device lain. Marker `ON_DEVICE` tanpa hex **tidak** bisa di-sync.

## PJTKI Backend Sync (ESP32)

Setelah enroll sukses, ESP memanggil API server PJTKI (tanpa JWT):

| Endpoint | Kapan |
|---|---|
| `POST {apiBaseUrl}/api/finger/arduino/register` | Enroll finger sukses (`postRegister`) |
| `POST {apiBaseUrl}/api/finger/arduino/attendance` | Match absensi (`postAttendance`) |

Body register: `{ employeeId, device_id, kode_cabang, finger_id }` → upsert `fingerprint_template` (status TERDAFTAR di menu BLK `#/finger/terdaftar`).

Setting `apiBaseUrl` / `kode_cabang` / `device_id` di LittleFS `/settings.json` (tab Setelan web UI).

**Jangan** pakai `LEDcontrol()` untuk toggle manual — bisa membuat scan berulang pada FPM10A.

## Key Functions
- `doAutoScan()` — Non-blocking state machine for fingerprint scanning
- `reinitSensor()` — WiFi OFF → detect sensor → WiFi AP on (no STA reconnect)
- `wifiReconnect()` — Reconnect to saved WiFi network (called after reinit succeeds)
- **WiFi auto-reconnect** — di housekeeping loop, tiap 30 detik cek koneksi. Kalau putus, otomatis coba reconnect ke semua saved networks (6s timeout per network). Mode WiFi tidak diubah (jangan panggil `WiFi.mode()` — putus web UI).
- `watchdogCheck()` — Triggers reinit after 15s inactivity
- `postAttendance()` — Sends attendance to server with NTP timestamp
- `postRegister()` — Sync enroll ke PJTKI (`/api/finger/arduino/register`)
- `fingerConfirm` — Debounce counter di `doAutoScan()` (ESP32) — **jangan dihapus**

## Libraries
- Adafruit_Fingerprint 2.1.4
- ArduinoJson 7.4.3
- TFT_eSPI 2.5.43
- NTPClient 3.2.1

## FPM10A Error Codes
- `code=0`: `FINGERPRINT_OK` — Image captured successfully (⚠️ can be false positive on sensitive sensors)
- `code=1`: `FINGERPRINT_PACKETRECIEVEERR` — Communication broken (WiFi interference)
- `code=2`: `FINGERPRINT_NOFINGER` — No finger detected
- `code=9`: `FINGERPRINT_NOTFOUND` — Finger detected but no template match
