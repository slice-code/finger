# AGENTS.md — FPM10A Fingerprint Attendance System

## Project Overview
NodeMCU ESP8266-based fingerprint attendance system with:
- **FPM10A sensor** via SoftwareSerial (D1=RX, D2=TX)
- **ILI9341 TFT LCD** via SPI (CS=D8, DC=D3, RST=D4, BL=D6, SCK=D5, MOSI=D7)
- **Web UI** (tabs: Dashboard, Daftar, Data, WiFi, Setelan, Akun, Cadangan)
- **LittleFS** for persistent config: `/credentials.json`, `/wifi.json`, `/db.json`, `/settings.json`

## Build & Upload
```bash
# Compile
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 /home/gugus/Documents/Project/pjtki/arduino/nodemcu_bridge/nodemcu_bridge.ino

# Upload (serial port /dev/ttyACM0)
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 --port /dev/ttyACM0 /home/gugus/Documents/Project/pjtki/arduino/nodemcu_bridge/nodemcu_bridge.ino
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

## Key State Variables
- `autoScan` (bool): Enable/disable automatic scanning
- `sensorReady` (bool): Sensor communication is working
- `scanState` (enum): `SCAN_IDLE` → `SCAN_BUSY` → `SCAN_WAIT_RELEASE`
- `consecutiveErrors` (int): Count of consecutive `getImage()` failures (max 5 before reinit)
- `lastScanActivity` (unsigned long): Timestamp of last sensor activity (watchdog 15s)
- `recoveryCount` (int): Recovery attempts before ESP.restart() (max 3)

## Key Functions
- `doAutoScan()` — Non-blocking state machine for fingerprint scanning
- `reinitSensor()` — WiFi OFF → detect sensor → WiFi ON (used by watchdog)
- `watchdogCheck()` — Triggers reinit after 15s inactivity
- `postAttendance()` — Sends attendance to server with NTP timestamp

## Serial Debug Monitor
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

## Libraries
- Adafruit_Fingerprint 2.1.4
- ArduinoJson 7.4.3
- TFT_eSPI 2.5.43
- NTPClient 3.2.1

## FPM10A Error Codes
- `code=1`: `FINGERPRINT_PACKETRECIEVEERR` — Communication broken (WiFi interference)
- `code=2`: `FINGERPRINT_NOFINGER` — No finger detected
- `code=9`: `FINGERPRINT_NOTFOUND` — Finger detected but no template match
