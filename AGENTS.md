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
- **BLE-first**: softAP default OFF (`apEnabled=false`), konfigurasi WiFi & settings lewat app BLE (`ble_app`/`android-pjtki` Flutter) — lihat bagian "BLE Android App"

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
# Compile (cek kode setelah update — harus sukses tanpa error)
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota_lfs /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-3v3/esp32-fpm10a-versi-3v3.ino
# Upload (serial port /dev/ttyUSB0)
arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=no_ota_lfs --port /dev/ttyUSB0 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-3v3/esp32-fpm10a-versi-3v3.ino
```

Verifikasi hasil compile (2026-08-13): `Sketch uses 1609487 bytes (76%) of program storage space` / `Global variables use 71260 bytes (21%)` — jika angka ini melonjak signifikan atau muncul warning/error, periksa perubahan terakhir.

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
static const int FINGER_CONFIRM_NEEDED = 1;  // 3V3: sensor lemah, tak sanggup 2x OK berturut

case SCAN_IDLE:
  if (p == FINGERPRINT_OK) {
    fingerConfirm++;
    if (fingerConfirm < FINGER_CONFIRM_NEEDED) {
      scanCooldownUntil = millis() + 30;
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
2. Need `FINGER_CONFIRM_NEEDED` consecutive OK reads before processing (3V3 = **1**, 5V = **2**)
3. Between confirmations (jika N>1): cooldown `+30ms`
4. Any NOFINGER or error resets the counter to 0
5. Counter also reset after scan completes (WAIT_RELEASE → IDLE)
6. Distant/transient detections (<N consecutive reads) are silently ignored

**Tuning** (if needed):
- Increase `FINGER_CONFIRM_NEEDED` → more debounce, slower response
- Decrease → faster response, more false triggers
- Adjust `scanCooldownUntil` spacing → longer = more delay between confirm checks

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

> **⚠️ Aturan state `ledOn` (bug 2026-08-13, lihat § BLE Connection & LED Stability):**
> - **WAJIB** verifikasi hasil `LEDcontrol(true) == FINGERPRINT_OK` sebelum set `ledOn = true`. Jika gagal → `ledOn = false` + `ledOnSince = 0`.
> - Cleanup (`enrollCleanupResumeScan`, `sensorResumeIdle`, `handleAutoOff`) **WAJIB** reset `ledOn = false` walau `LEDcontrol(false)` gagal — mempertahankan `ledOn = true` saat gagal membuat gate scan tidak pernah menyalakan LED lagi (LED stuck mati walau jari disentuh).

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
| `scanCooldownUntil` (confirm spacing) | `+50ms` | **`+30ms`** | Current 3V3 firmware spacing |
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

> ⚠️ **BERLAKU HANYA UNTUK NodeMCU/ESP8266 (SoftwareSerial).** ESP32 (5V & 3V3) memakai
> **HardwareSerial UART2** untuk FPM10A, sehingga interrupt WiFi TIDAK mengganggu UART —
> **JANGAN** matikan WiFi (`WiFi.mode(WIFI_OFF)`) saat deteksi/reinit sensor di ESP32.
> Mematikan WiFi di ESP32 membuat AP hilang 12–25 detik tiap siklus recovery sensor
> → AP tidak stabil & device tidak bisa connect ke WiFi dari web UI via AP. Sudah dihapus
> dari `detectSensor()` dan `reinitSensor()` di versi 3V3 (2026-08-12).

## Recovery Mechanism

### NodeMCU / ESP8266 (SoftwareSerial)
The sensor uses SoftwareSerial which is inherently unreliable with WiFi. Two failure modes exist:

#### Mode 1: Scanning Error → ESP.restart()
When WiFi interrupts corrupt scanning communication:
1. `getImage()` returns error codes → `consecutiveErrors` increments
2. After `MAX_CONSECUTIVE_ERRORS` → **immediate `ESP.restart()`**
3. Boot sequence re-detects sensor with WiFi OFF → works reliably
4. WiFi reconnects after sensor is ready

#### Mode 2: Boot Detection Failure → Retry then Restart
When sensor can't be detected at boot:
1. `reinitSensor()` called with WiFi OFF + cooldown
2. Phase baud retries, then after `MAX_RECOVERY` → **`ESP.restart()`**

Only a full power cycle (or ESP restart) reliably resets a wedged FPM10A on ESP8266.

### ESP32 versi 3V3 (HardwareSerial UART2) — current firmware
- Scan errors → **soft recover** (LED off, gate reset, cooldown) — **jangan** `ESP.restart()` pada burst `getImage` error
- Sensor reconnect di loop: interval ~30s, AP tetap hidup; setelah `MAX_RECOVERY` → idle + keep serving AP (bukan reboot paksa)
- `reinitSensor()` **tidak** mematikan WiFi (AP/STA tetap)
- Absensi HTTP di task `attnHttp` (queue) supaya match tidak nunggu TLS
- Cache cabang/karyawan di task `cacheHttp`; Select2 AJAX + slim cache; HTTPS di-serialize (`httpsMutex`)
- Web UI: tanpa HTTP Basic Auth; STA pakai IP LAN (softAP off saat connected)
- Jadwal sleep: `scan_schedule` + `scan_start_hour` / `scan_end_hour` di `/settings.json` (default aktif 05→00 = tidur 00–05)

## Key State Variables
- `autoScan` (bool): Enable/disable automatic scanning
- `sensorReady` (bool): Sensor communication is working
- `scanState` (enum): `SCAN_IDLE` → `SCAN_BUSY` → `SCAN_WAIT_RELEASE`
- `consecutiveErrors` (int): Count of consecutive `getImage()` failures (3V3 soft-recover at `MAX_CONSECUTIVE_ERRORS`=8)
- `lastScanActivity` (unsigned long): Timestamp of last sensor activity (watchdog 20s on 3V3)
- `recoveryCount` (int): Recovery attempts (3V3: idle AP setelah max, bukan reboot paksa)



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

Setting `apiBaseUrl` / `kode_cabang` / `device_id` / `api_key` / `ir_enabled` / jadwal scan di LittleFS `/settings.json` (tab Setelan web UI).

**LED FPM10A**: pada klon baru, `LEDcontrol(true/false)` **dipakai** (gate terbuka = ON, idle = OFF). Jangan toggle LED dari UI manual secara agresif; kontrol lewat gate/enroll saja.

## BLE Android App (`ble_app` / `android-pjtki` Flutter)

Koneksi HP → ESP32 via **NimBLE** (tanpa WiFi/softAP). Config WiFi & settings dari app BLE.

### GATT Service (UUID wajib konsisten dengan `lib/config/app_config.dart` Flutter)

| Karakteristik | UUID | Property | Fungsi |
|---|---|---|---|
| Service | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` | — | — |
| Status | `4fafc202-...` | READ+NOTIFY | JSON status device (`ready`, `autoActive`, `count`, `wifiMode`, `temp`, `sensorReady`, `irEnabled`) |
| Command | `4fafc203-...` | WRITE / WRITE_NR | Perintah teks: `AUTOSCAN ON/OFF`, `WIFI_SCAN`, `ENROLL_CANCEL` |
| Enroll | `4fafc204-...` | WRITE / WRITE_NR | JSON `{"employeeId","name"}` → tulis ke `/ble_enroll.json` |
| Delete | `4fafc205-...` | WRITE / WRITE_NR | ID finger (1–100) |
| Settings | `4fafc206-...` | READ+WRITE / WRITE_NR | JSON config; `onRead` → `bleUpdateSettings()` |
| Events | `4fafc207-...` | READ+NOTIFY | Notifikasi event (`wifi_saved`, `wifi_connecting`, `wifi_failed`, `wifi_scan`, `enroll_*`, `settings_saved`, `autoscan`) |

Catatan NimBLE 2.x: `onWrite`/`onRead` **wajib** signature `(NimBLECharacteristic*, NimBLEConnInfo&)` — tanpa `ConnInfo&` handler TIDAK di-override dan write dari HP diabaikan diam-diam.

### Pending Sync Offline → Server (ESP32 3V3, perbaikan 2026-08-14)

**Daftar (enroll) & absensi kini disimpan dulu di storage ESP32, lalu di-upload**
**ke server secara berkala / manual — tidak lagi langsung-cepat hanya saat online.**

- File storage: `/attendance.json` (max 200 catatan absensi, field `synced`),
  `/pending_register.json` (max 20 enroll belum ter-upload, simpan `hex`).
- `pendingAttAdd()` selalu dipanggil saat match (offline atau online). Gagal
  upload → `synced=false` → di-retry oleh sync worker. Dedup 4s menandai
  duplikat sebagai `synced=true` supaya tidak bikin absensi ganda.
- Enroll (`enrollFinger`): `dbAdd` + `pendingRegAdd` SELALU, lalu coba
  `postRegister` langsung jika online; sukses → hapus dari pending, gagal →
  tetap di antrean.
- `syncWorker` (task `syncHttp`): flush pending register + attendance belum
  `synced`. Dipicu oleh timer `uploadIntervalMinutes` (default **120 menit**)
  di loop housekeeping & perintah BLE `SYNC_NOW`.
- `AppSettings.uploadIntervalMinutes` (5–1440, `/settings.json` key
  `upload_interval_minutes`). Diubah via tab Setelan web, BLE settings write
  (`uploadIntervalMinutes`), atau command `SET_SYNC_INTERVAL <menit>`.
- BLE command handler sekarang handle: `SYNC_NOW`, `SET_SYNC_INTERVAL <n>`.
- BLE karakteristik baru **`4fafc208`** (READ) = riwayat absensi lokal
  (`bleUpdateHistory()`, JSON array `{employeeId,nama,tanggal,jam,synced}`).
  UUID ditambah di `app_config.dart` (`kHistoryCharUuid`).
- Android `RiwayatScreen`: gabungkan riwayat lokal (BLE `readLocalHistory()`,
  item `synced=false` ditandai "lokal (belum sync)" berikon Bluetooth) + riwayat
  server via `POST /api/finger/history`. Tombol sync di riwayat & Setelan.
- `pendingMutex` melindungi akses `pendingAtt`/`pendingReg` antar task
  (attnWorker, syncWorker, loop, BLE).

### Riwayat Server API (HMAC+AES, pjtki-bio)

Riwayat absensi yang dibaca app Android **tidak** pakai `/api/blk_absensi`
(butuh JWT — app tidak bisa). Gunakan endpoint baru yang memakai **enkripsi
sama persis seperti upload finger** (`/api/finger/register` &
`/api/finger/attendance`):

- `POST /api/finger/history` — body `{encryptedPayload, signature}` (HMAC+AES
  `FINGERPRINT_SECRET_KEY`). Payload: `{page, perPage, tanggal?, status?,
  search?, device_id?}` → query `blk_absensi` → `{success, data, total}`.
- Android encrypt dengan `FingerprintCrypto` (AES-256-ECB key=SHA256(secret),
  HMAC-SHA256 signature) — paket `pointycastle` + `crypto`; secret
  `kFingerprintSecretKey` di `app_config.dart` (harus sama dengan server).
- Server route terpasang di `api/fingerprint.js` (pjtki-bio) dan sudah teruji
  via pm2 (port 3004).

### BLE Connection & LED Stability (perbaikan 2026-08-13)

Tiga bug nyata yang sudah diperbaiki (masing-masing punya pelajaran penting — jangan regress):

#### 1. ⚠️ JANGAN turunkan supervision timeout — bikin churn & reboot

Dulu dicoba `updateConnParams(handle, 24, 48, 0, **100**)` (1s) supaya detect link loss cepat. **TERNYATA kontraproduktif**:

- Setiap jeda BLE >1s → ESP32 putus link → `onDisconnect` → re-advertise
- App auto-reconnect (backoff) → connect → putus lagi → **churn tak berujung**
- `bleEnsureAdvertising()` tight-loop (ribuan `[BLE] advertising resumed`/detik di serial) → **task watchdog → ESP32 restart** (gejala "enroll gagal lalu tiba-tiba reboot")

**Keputusan: tetap `400` (4s)** — kompromi stabil, sama seperti firmware 5V yang sudah teruji. Jangan turunkan.

#### 2. Cooldown di `bleEnsureAdvertising()` — anti-tight-loop

```cpp
if (!pAdv->isAdvertising()) {
  static unsigned long lastEnsure = 0;
  unsigned long now = millis();
  if (now - lastEnsure < 2000) return;   // maksimal 1×/2 detik
  lastEnsure = now;
  BLEDevice::startAdvertising();
}
```

Tanpa cooldown ini, jika `isAdvertising()` tidak pulih (NimBLE stuck saat churn), loop utama bisa memicu watchdog. **Jangan hapus cooldown.**

#### 3. LED FPM10A stuck mati setelah enroll gagal (state `ledOn` tidak sinkron)

**Gejala:** enroll gagal sensor → LED tidak mau menyala walau jari disentuh.

**Akar:** `ledOn` di-set `true` tanpa verifikasi hasil `LEDcontrol(true)`. Enroll gagal → `LEDcontrol(false)` juga gagal → `ledOn` tetap `true` (padahal LED fisik mati). Kembali ke mode scan, blok `if (!ledOn)` di gate **tidak pernah dieksekusi** → LED tidak pernah dinyalakan ulang.

**Aturan (wajib):**
- **Setiap set `ledOn = true` WAJIB verifikasi hasil** `finger.LEDcontrol(true) == FINGERPRINT_OK`; jika gagal set `ledOn = false` + `ledOnSince = 0`. Berlaku di: `enrollLedKeepOn()`, `bleWakeUi()`, gate `SCAN_IDLE`.
- **Setiap cleanup WAJIB reset `ledOn = false`** walau `LEDcontrol(false)` gagal — jangan pertahankan state salah. Berlaku di: `enrollCleanupResumeScan()`, `sensorResumeIdle()`, `handleAutoOff()`.
- Jika `LEDcontrol(true)` gagal saat gate terbuka → `scanCooldownUntil = millis() + 500` (jangan loop keras).

#### 4. Enroll: error sensor beruntun → batal cepat + reinit

- `waitFinger()` / `waitNoFinger()`: jika `getImage()` error (bukan `OK`/`NOFINGER`) **≥5 beruntun** → return false (abort). Sebelumnya loop menunggu sampai `ENROLL_WAIT_MS` (180 detik) → tampak menggantung saat sensor macet.
- Setelah `enrollFinger()` return error (bukan `0xFD` batal), loop panggil `reinitSensor()` + `sensorResumeIdle("post-enroll-reinit")` supaya sensor & LED pulih otomatis.
- Operasi blocking sensor di `enrollFinger()` (`image2Tz`, `fingerSearch`, `createModel`, `storeModel`) **wajib** di-wrap `disableLoopWDT()`/`enableLoopWDT()` + `esp_task_wdt_reset()` — sama seperti path scan (`doAutoScan`). Tanpa wrap, sensor lambat >5s → task WDT → reboot.

#### 5. Flutter app: auto-reconnect

`lib/services/ble_service.dart`:
- `restoreLastDevice()` dipindah dari `main()` (sebelum `runApp`) ke `HomeShell.initState()` via `addPostFrameCallback` — permission dialog BLE bisa tampil benar & Android stack siap.
- Koneksi putus tiba-tiba → `_scheduleAutoReconnect()` dengan backoff 2s→5s→10s→15s→20s (max 5). Stop saat user manual `disconnect()`.
- `_connectWithRetry()`: attempt 1 gagal → disconnect + clearGattCache + 1.2s → coba sekali lagi (kasus "scan tampil tapi tidak konek" — Android stale GATT setelah app di-kill).
- Flag `_autoReconnectCall` membedakan connect dari timer vs manual, supaya backoff counter tidak di-reset tiap percobaan (jangan kembali ke loop "menunggu koneksi terus").

### ⚠️ Android app: 2 device adb / duplikat

`adb devices` bisa tampil **2 koneksi untuk device yang sama** (satu `IP:port`, satu `adb-s...tcp` mDNS). Ini bukan 2 device fisik. Gunakan `adb -s <serial>` untuk meng-arahkan perintah (mis. `adb -s 192.168.1.5:37005 install ...`), atau `adb disconnect` untuk menghapus yang duplikat.

## Key Functions
- `doAutoScan()` — Non-blocking state machine for fingerprint scanning
- `reinitSensor()` — ESP32: detect sensor **dengan WiFi tetap ON** (jangan WIFI_OFF); softAP/BLE tetap
- `wifiReconnect()` — Reconnect to saved WiFi network (called after reinit succeeds)
- **WiFi auto-reconnect** — di housekeeping loop, tiap 30 detik cek koneksi. Kalau putus, otomatis coba reconnect ke semua saved networks. Mode WiFi tidak diubah sembarangan (jangan putus web UI).
- `watchdogCheck()` — Triggers reinit after inactivity (`SCAN_WATCHDOG_MS`)
- `postAttendance()` / `attnEnqueue` — Sends attendance to server (async task)
- `postRegister()` — Sync enroll ke PJTKI (`/api/finger/arduino/register`)
- `httpsLock` / `cacheWorker` — serialize TLS; unduh cache cabang/karyawan di background
- `fingerConfirm` — Debounce counter di `doAutoScan()` (ESP32) — **jangan dihapus**
- `checkAutoSleep()` — Hormati `scan_schedule` + jam mulai/selesai
- `bleInit()` — setup BLE server ("PJTKI-Finger", MTU 517, TX +9dBm); `bleEnsureAdvertising()` re-advertise setelah disconnect
- `bleUpdateStatus()` / `bleUpdateSettings()` / `bleNotifyEvent()` — publish status/settings/events ke app BLE
- `bleWakeLcd()` / `bleWakeUi()` — bangunkan LCD saat konek BLE (jangan nyalakan LED FPM10A)
- `sensorResumeIdle()` — reset state scan/LED/gate sebelum enroll atau setelah gagal (UART bersih)
- `irCalibrate()` / `irUpdateGate()` / `irGateActive()` / `irShouldPoll()` — kalibrasi & gate T-OUT/IR
- `ledForceOff()` / `enrollLedKeepOn()` / `enrollKeepAliveUi()` — kontrol LED FPM10A (0x50/0x51); **wajib** verifikasi hasil sebelum set `ledOn=true` (lihat § BLE Connection & LED Stability)
- `waitFinger()` / `waitNoFinger()` — polling jari saat enroll; error sensor ≥5 beruntun → abort cepat (jangan tunggu 180s)
- `attnEnqueue()` / `attnWorker()` / `attnServiceUi()` / `attnPublishResult()` — upload absensi di task terpisah
- `cacheInitWorker()` / `cacheWorker()` / `cacheBuildEmpSlim()` — cache cabang/karyawan slim di background
- `wifiScanLaunchChannel()` / `wifiScanMergeResults()` / `wifiScanService()` — progressive WiFi scan

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
