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
# Compile
arduino-cli compile --fqbn esp32:esp32:esp32 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-5v/esp32-fpm10a-versi-5v.ino
# Upload (serial port /dev/ttyUSB0)
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 /home/gugus/Documents/Project/pjtki/arduino/esp32-fpm10a-versi-5v/esp32-fpm10a-versi-5v.ino
```

## FPM10A Finger Confirm Counter (ESP32 only)

**Problem**: FPM10A sensor on ESP32 can be overly sensitive — `getImage()` returns `FINGERPRINT_OK` (0) even when no finger is touching the sensor, or when a finger is only nearby (not yet pressed). This causes false scan triggers and continuous "NO MATCH" results.

**Solution**: Debounce mechanism using `fingerConfirm` counter in `doAutoScan()`:

```cpp
static int fingerConfirm = 0;
static const int FINGER_CONFIRM_NEEDED = 2;  // need 2 consecutive OK reads

case SCAN_IDLE:
  if (p == FINGERPRINT_OK) {
    fingerConfirm++;
    if (fingerConfirm < FINGER_CONFIRM_NEEDED) {
      scanCooldownUntil = millis() + 50;  // 50ms between confirm checks
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
