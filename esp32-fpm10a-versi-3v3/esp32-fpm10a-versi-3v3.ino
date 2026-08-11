#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_Fingerprint.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <stdio.h>
#include <stdarg.h>
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Web Auth ──────────────────────────────────────────────────────
#define WEB_USER "cks"
#define WEB_PASS "gugus$111$"

// ── Pin Mapping (ESP32 + FPM10A 3.3V) ──────────────────────────────────
// Finger sensor → HardwareSerial2: RX=GPIO16 (RX2), TX=GPIO17 (TX2)
// FPM10A 3.3V: weaker IR LED, lower signal margin — adjusted debounce/security
// LCD (ILI9341 SPI): CS=GPIO5, DC=GPIO2, RST=GPIO4, BL=GPIO15, SCK=GPIO18, MOSI=GPIO23
#define FINGER_RX 16
#define FINGER_TX 17
#define LCD_BL    15
// Finger-presence gate: T-OUT (touch output FPM10A, aktif HIGH saat jari
// menyentuh kaca) → GPIO13. Kompatibel juga dengan modul IR obstacle 3-pin
// (VCC→3V3, GND→GND, OUT→GPIO13) karena polaritas di-auto-detect saat boot.
#define TOUCH_PIN 13

// ── WiFi Config ────────────────────────────────────────────────────
#define AP_SSID "FPM10A-Bridge"
#define AP_PASS "gugus$111$"
#define WIFI_TIMEOUT_MS 10000
#define WIFI_FILENAME "/wifi.json"
#define STORAGE_BASE_PATH "/littlefs"
#define STORAGE_PARTITION_LABEL "littlefs"
bool storageReady = false;
void logError(const char *fmt, ...);

// ── Objects ────────────────────────────────────────────────────────
HardwareSerial altSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&altSerial);
TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "id.pool.ntp.org", 25200, 60000); // WIB (UTC+7)

// ── Runtime Credentials (loaded from LittleFS / defaults) ──────────
#define CRED_FILENAME "/credentials.json"
struct CredConfig {
  char webUser[32];
  char webPass[64];
  char apPass[65];
  char ntpServer[64];
  long utcOffset; // seconds
};
CredConfig cred;

void credLoad() {
  memset(&cred, 0, sizeof(cred));
  strncpy(cred.webUser, WEB_USER, 31);
  strncpy(cred.webPass, WEB_PASS, 63);
  strncpy(cred.apPass, AP_PASS, 64);
  strncpy(cred.ntpServer, "id.pool.ntp.org", 63);
  cred.utcOffset = 25200; // WIB

  if (!storageReady) return;
  File f = LittleFS.open(CRED_FILENAME, "r");
  if (!f) return;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  if (doc.containsKey("webUser")) { strncpy(cred.webUser, doc["webUser"] | "", 31); cred.webUser[31] = 0; }
  if (doc.containsKey("webPass")) { strncpy(cred.webPass, doc["webPass"] | "", 63); cred.webPass[63] = 0; }
  if (doc.containsKey("apPass")) { strncpy(cred.apPass, doc["apPass"] | "", 64); cred.apPass[64] = 0; }
  if (doc.containsKey("ntpServer")) { strncpy(cred.ntpServer, doc["ntpServer"] | "", 63); cred.ntpServer[63] = 0; }
  if (doc.containsKey("utcOffset")) cred.utcOffset = doc["utcOffset"] | 25200;
  Serial.printf("[CRED] Loaded from %s\n", CRED_FILENAME);
}

bool credSave() {
  if (!storageReady) return false;
  DynamicJsonDocument doc(512);
  doc["webUser"] = cred.webUser;
  doc["webPass"] = cred.webPass;
  doc["apPass"] = cred.apPass;
  doc["ntpServer"] = cred.ntpServer;
  doc["utcOffset"] = cred.utcOffset;
  File f = LittleFS.open(CRED_FILENAME, "w");
  if (!f) { logError("credentials write open failed"); return false; }
  serializeJson(doc, f);
  f.close();
  return true;
}

bool requireAuth() {
  if (!server.authenticate(cred.webUser, cred.webPass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ── State ──────────────────────────────────────────────────────────
bool autoScan = false;
bool fingerDown = false;
bool sensorReady = false;
bool enrollActive = false;
bool restoreActive = false;
bool wifiConnected = false;
bool scanSleeping = false;
bool ledOn = false;           // LED FPM10A terkontrol via 0x50/0x51 (sensor klon baru)
bool lcdBacklightOn = true;
unsigned long lastLcdActivity = 0;
#define LCD_IDLE_TIMEOUT_MS 60000
String staIP = "";
String staSSID = "";
unsigned long curBaud = 0;
char rxBuf[80];
uint8_t rxLen = 0;

enum ScanState { SCAN_IDLE, SCAN_BUSY, SCAN_WAIT_RELEASE };
ScanState scanState = SCAN_IDLE;
unsigned long scanResultTime = 0;
unsigned long scanCooldownUntil = 0;
int consecutiveErrors = 0;
uint8_t fallbackErrors = 0;
#define SCAN_RESULT_HOLD_MS 4000
#define MAX_CONSECUTIVE_ERRORS 8   // 3.3V: lebih toleran transient error
#define SCAN_SOFT_RECOVER_MS 1000  // jeda setelah error burst sebelum scan ulang
#define LED_WARMUP_MS       50    // jeda setelah LED ON sebelum getImage()
#define LED_OFF_RETRY_MS  2000    // retry hanya jika perintah LED OFF gagal

// ── Watchdog / Auto-Recovery ──────────────────────────────────────
unsigned long lastScanActivity = 0;  // millis terakhir sensor merespons
unsigned long lastRecoveryAttempt = 0;
uint8_t recoveryCount = 0;
#define SCAN_WATCHDOG_MS   20000  // 20 detik tanpa aktivitas = recovery (3.3V lebih lambat)
#define RECOVERY_COOLDOWN  5000   // jeda antar recovery attempt
#define MAX_RECOVERY       3      // max recovery sebelum ESP.restart()

// ── WiFi credentials ──────────────────────────────────────────────
#define MAX_SAVED_WIFI 5
struct WiFiCreds { char ssid[33]; char pass[65]; };
WiFiCreds savedWiFi[MAX_SAVED_WIFI];
int savedWiFiCount = 0;

// ── Backend API Settings ──────────────────────────────────────────
#define SETTINGS_FILENAME "/settings.json"
struct AppSettings {
  char apiBaseUrl[128];
  char kodeCabang[16];
  char deviceId[32];
  char apiKey[65];         // X-Device-Key untuk auth ke PJTKI server
  uint8_t scanStartHour;  // jam mulai scan (0-23)
  uint8_t scanEndHour;    // jam selesai scan (0-23)
  bool scanSchedule;      // true = pakai jadwal
  bool irEnabled;         // true = IR obstacle gate aktif (default)
};
AppSettings appSettings;

bool storageInit() {
  if (LittleFS.begin(false, STORAGE_BASE_PATH, 10, STORAGE_PARTITION_LABEL)) {
    storageReady = true;
    Serial.printf("[FS] LittleFS mounted OK | total=%u used=%u\n",
      LittleFS.totalBytes(), LittleFS.usedBytes());
    return true;
  }

  Serial.println("[FS] Mount failed; formatting LittleFS partition...");
  if (!LittleFS.format()) {
    Serial.println("[FS] Format failed; storage unavailable");
    return false;
  }

  if (!LittleFS.begin(false, STORAGE_BASE_PATH, 10, STORAGE_PARTITION_LABEL)) {
    Serial.println("[FS] Mount after format failed; storage unavailable");
    return false;
  }

  storageReady = true;
  Serial.printf("[FS] Formatted and mounted | total=%u used=%u\n",
    LittleFS.totalBytes(), LittleFS.usedBytes());
  return true;
}

// ── Fingerprint DB (in-memory + LittleFS) ─────────────────────────
#define MAX_FP 100
struct FPEntry { uint8_t id; char name[32]; char empId[16]; };
FPEntry fpDB[MAX_FP];
int fpCount = 0;

#include "ble_handler.h"

// ── Debug Log (ring-buffer LittleFS, max ~8KB) ───────────────────────
#define DEBUG_LOG "/debug.log"
#define DEBUG_LOG_MAX 8192
#define ERROR_LOG "/errors.log"
#define ERROR_LOG_MAX 32768
#define ERROR_LOG_MIN_INTERVAL_MS 5000

void logDebug(const char *fmt, ...) {
  if (!storageReady) return;
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  unsigned long ts = millis();
  float suhu = temperatureRead();
  char line[160];
  snprintf(line, sizeof(line), "[%lum %ds %dC] %s\n", ts / 60000, (ts / 1000) % 60, (int)suhu, buf);

  File f = LittleFS.open(DEBUG_LOG, "a");
  if (f) {
    long sz = f.size();
    f.print(line);
    f.close();
    // Ring: hapus separuh awal kalau terlalu besar
    if (sz > DEBUG_LOG_MAX) {
      File fr = LittleFS.open(DEBUG_LOG, "r");
      if (fr) {
        fr.seek(sz / 2);  // skip separuh awal
        String keep = fr.readString();
        fr.close();
        File fw = LittleFS.open(DEBUG_LOG, "w");
        if (fw) { fw.print(keep); fw.close(); }
      }
    }
  }
}

uint32_t suppressedErrorCount = 0;
unsigned long lastErrorLogAt = 0;

void logError(const char *fmt, ...) {
  if (!storageReady) return;

  unsigned long now = millis();
  if (lastErrorLogAt && now - lastErrorLogAt < ERROR_LOG_MIN_INTERVAL_MS) {
    suppressedErrorCount++;
    return;
  }

  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char line[240];
  float suhu = temperatureRead();
  if (suppressedErrorCount) {
    snprintf(line, sizeof(line), "[%lum %ds %dC heap=%u wifi=%d suppressed=%lu] %s\n",
      now / 60000, (now / 1000) % 60, (int)suhu, ESP.getFreeHeap(),
      (int)WiFi.status(), (unsigned long)suppressedErrorCount, buf);
  } else {
    snprintf(line, sizeof(line), "[%lum %ds %dC heap=%u wifi=%d] %s\n",
      now / 60000, (now / 1000) % 60, (int)suhu, ESP.getFreeHeap(),
      (int)WiFi.status(), buf);
  }

  File f = LittleFS.open(ERROR_LOG, "a");
  if (!f) return;
  long sz = f.size();
  f.print(line);
  f.close();
  lastErrorLogAt = now;
  suppressedErrorCount = 0;

  if (sz + (long)strlen(line) > ERROR_LOG_MAX) {
    File fr = LittleFS.open(ERROR_LOG, "r");
    if (fr) {
      fr.seek(sz / 2);
      String keep = fr.readString();
      fr.close();
      File fw = LittleFS.open(ERROR_LOG, "w");
      if (fw) { fw.print(keep); fw.close(); }
    }
  }
}

void handleDebugLog() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  File f = LittleFS.open(DEBUG_LOG, "r");
  if (!f) { server.send(200, "text/plain", "(empty)\n"); return; }
  server.send(200, "text/plain", f.readString());
  f.close();
}

void handleErrorLog() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Content-Disposition", "attachment; filename=errors.log");
  File f = LittleFS.open(ERROR_LOG, "r");
  if (!f) { server.send(200, "text/plain", "(empty)\n"); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line += '\n';
    server.sendContent(line);
  }
  f.close();
}

// ── SSE ────────────────────────────────────────────────────────────
#define MAX_SSE_CLIENTS 4
WiFiClient sseClients[MAX_SSE_CLIENTS];

// ── PJTKI Finger Theme (match webpage.h cyan/teal) ───────────────────
// RGB565 ≈ HTML: #0b1220 bg, #0f172a topbar, #132337 card,
//                #38bdf8 cyan, #0e7490 teal, #a5e1ec teal-light
#define COL_BG        0x0884   // #0b1220
#define COL_SIDEBAR   0x3DFF   // #38bdf8 cyan
#define COL_PANEL     0x1106   // #132337 card
#define COL_TOPBAR    0x08A5   // #0f172a
#define COL_ACCENT    0x3DFF   // #38bdf8 cyan
#define COL_ACCENT2   0x0BB2   // #0e7490 teal
#define COL_ACCENT3   0xA71D   // #a5e1ec teal-light
#define COL_BORDER    0x2A6F   // soft cyan border
#define COL_TEXT      0xE75E   // #e2e8f0
#define COL_DIM       0x9517   // #94a3b8
#define COL_DIM2      0x63B1   // #64748b
#define COL_DIM3      0x2126   // dark slate inset
#define COL_OK        0x262B   // #22c55e green
#define COL_OK_DARK   0x1306   // dark green panel
#define COL_ERR       0xEA08   // #ef4444
#define COL_ERR_DARK  0x5800   // dark red panel
#define COL_WARN      0xF4E1   // #f59e0b
#define COL_GOLD      0x3DFF   // cyan (ex-gold → brand cyan)
#define SIDEBAR_W     4
#define SCREEN_W 320
#define SCREEN_H 240

// ── Layout constants ──────────────────────────────────────────────
#define TOPBAR_H    32
#define FOOTER_Y    208
#define FOOTER_H    32
#define ICON_CY     88
#define STATUS_Y    152
#define RESULT_Y    145

// ────────────────────────────────────────────────────────────────────
//  LCD PRIMITIVES
// ────────────────────────────────────────────────────────────────────
void lcdSidebar() {
  tft.fillRect(0, TOPBAR_H, SIDEBAR_W, FOOTER_Y - TOPBAR_H, COL_ACCENT2);
  tft.fillRect(SIDEBAR_W - 1, TOPBAR_H, 1, FOOTER_Y - TOPBAR_H, COL_ACCENT);
}

void lcdProgressBar(int x, int y, int w, int h, int pct, uint16_t fg) {
  tft.fillRoundRect(x, y, w, h, 4, COL_DIM3);
  tft.drawRoundRect(x, y, w, h, 4, COL_BORDER);
  int fw = (w - 4) * pct / 100;
  if (fw > 0) tft.fillRoundRect(x + 2, y + 2, fw, h - 4, 3, fg);
}

void lcdDrawFingerprint(int cx, int cy, int s, uint16_t col, uint16_t ringCol) {
  // outer glow rings (web-like cyan halo)
  tft.drawCircle(cx, cy, 32 * s / 10, COL_DIM3);
  tft.drawCircle(cx, cy, 28 * s / 10, ringCol);
  tft.drawCircle(cx, cy, 26 * s / 10, ringCol);
  tft.drawCircle(cx, cy, 22 * s / 10, col);
  tft.drawCircle(cx, cy, 18 * s / 10, col);
  for (int i = -14; i <= 14; i += 4)
    tft.drawLine(cx + i, cy - 16, cx + i, cy + 16, col);
  tft.drawArc(cx, cy, 17 * s / 10, 11 * s / 10, -50, 50, col, COL_BG);
  tft.drawArc(cx, cy, 15 * s / 10, 9 * s / 10, 130, 230, col, COL_BG);
}

void lcdBadgeCenter(int cy, const char *txt, uint16_t bg, uint16_t fg) {
  int tw = tft.textWidth(txt, 2);
  int bw = tw + 28;
  tft.fillRoundRect(SCREEN_W / 2 - bw / 2, cy - 14, bw, 28, 12, bg);
  tft.drawRoundRect(SCREEN_W / 2 - bw / 2, cy - 14, bw, 28, 12, fg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.setTextSize(2);
  tft.drawString(txt, SCREEN_W / 2, cy);
}

void lcdCard(int y, int h, uint16_t col) {
  tft.fillRoundRect(12, y, SCREEN_W - 24, h, 12, COL_PANEL);
  tft.drawRoundRect(12, y, SCREEN_W - 24, h, 12, COL_BORDER);
  tft.fillRoundRect(14, y + 8, 4, h - 16, 2, col);
}

void lcdBrandMark(int x, int y) {
  // mini brand square like web .brand-mark
  tft.fillRoundRect(x, y, 18, 18, 4, COL_ACCENT2);
  tft.fillRoundRect(x + 1, y + 1, 16, 16, 3, COL_ACCENT);
  tft.drawCircle(x + 9, y + 9, 5, COL_TOPBAR);
  tft.drawCircle(x + 9, y + 9, 3, COL_TOPBAR);
}

// ────────────────────────────────────────────────────────────────────
//  TOP BAR
// ────────────────────────────────────────────────────────────────────
void lcdDrawTopbar() {
  tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
  tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, COL_BORDER);
  lcdBrandMark(8, 7);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_ACCENT, COL_TOPBAR);
  tft.setTextSize(1);
  tft.drawString("PJTKI", 30, 6);
  tft.setTextColor(COL_DIM, COL_TOPBAR);
  tft.drawString("Finger", 30, 16);

  // status pill center
  const char *st = "IDLE";
  uint16_t sc = COL_DIM;
  if (autoScan) {
    if (scanSleeping) { st = "SLEEP"; sc = COL_DIM2; }
    else { st = "SCAN"; sc = COL_ACCENT; }
  }
  int tw = tft.textWidth(st, 1);
  int pw = tw + 16;
  int px = SCREEN_W / 2 - pw / 2;
  tft.fillRoundRect(px, 8, pw, 16, 8, COL_DIM3);
  tft.drawRoundRect(px, 8, pw, 16, 8, sc);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(sc, COL_DIM3);
  tft.drawString(st, SCREEN_W / 2, 16);

  char buf[12];
  snprintf(buf, sizeof(buf), "%d FP", finger.templateCount);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_ACCENT3, COL_TOPBAR);
  tft.drawString(buf, SCREEN_W - 8, 12);
}

// ────────────────────────────────────────────────────────────────────
//  FOOTER (shows WiFi mode)
// ────────────────────────────────────────────────────────────────────
void lcdDrawFooter() {
  tft.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, COL_TOPBAR);
  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_BORDER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  if (wifiConnected) {
    tft.fillCircle(SIDEBAR_W + 10, FOOTER_Y + 10, 3, COL_OK);
    tft.setTextColor(COL_TEXT, COL_TOPBAR);
    tft.drawString(staSSID, SIDEBAR_W + 18, FOOTER_Y + 5);
    tft.setTextColor(COL_DIM2, COL_TOPBAR);
    tft.drawString(staIP, SIDEBAR_W + 18, FOOTER_Y + 17);
  } else {
    tft.fillCircle(SIDEBAR_W + 10, FOOTER_Y + 10, 3, COL_WARN);
    tft.setTextColor(COL_WARN, COL_TOPBAR);
    tft.drawString("AP:" AP_SSID, SIDEBAR_W + 18, FOOTER_Y + 5);
    tft.setTextColor(COL_DIM2, COL_TOPBAR);
    tft.drawString("192.168.4.1", SIDEBAR_W + 18, FOOTER_Y + 17);
  }

  tft.setTextDatum(TR_DATUM);
  char buf[20];
  snprintf(buf, sizeof(buf), "S:%d", finger.security_level);
  tft.setTextColor(COL_DIM, COL_TOPBAR);
  tft.drawString(buf, SCREEN_W - 8, FOOTER_Y + 5);

  float suhu = temperatureRead();
  snprintf(buf, sizeof(buf), "%.0f C", suhu);
  tft.setTextColor(COL_DIM2, COL_TOPBAR);
  tft.drawString(buf, SCREEN_W - 96, FOOTER_Y + 16);

  tft.setTextColor(COL_ACCENT3, COL_TOPBAR);
  if (timeClient.isTimeSet()) {
    String t = timeClient.getFormattedTime();
    tft.drawString(t, SCREEN_W - 8, FOOTER_Y + 17);
  } else {
    tft.drawString("--:--:--", SCREEN_W - 8, FOOTER_Y + 17);
  }
}

// ────────────────────────────────────────────────────────────────────
//  BOOT SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowBoot() {
  tft.fillScreen(COL_BG);
  // soft teal band behind icon
  tft.fillRoundRect(40, 18, SCREEN_W - 80, 78, 14, COL_PANEL);
  tft.drawRoundRect(40, 18, SCREEN_W - 80, 78, 14, COL_BORDER);
  lcdDrawFingerprint(SCREEN_W / 2, 55, 16, COL_ACCENT, COL_ACCENT2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setTextSize(2);
  tft.drawString("PJTKI Finger", SCREEN_W / 2, 112);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Attendance System", SCREEN_W / 2, 134);
  tft.drawFastHLine(70, 148, SCREEN_W - 140, COL_BORDER);
  lcdProgressBar(50, 162, SCREEN_W - 100, 10, 0, COL_ACCENT);
}

void lcdProgress(int pct) {
  lcdProgressBar(50, 162, SCREEN_W - 100, 10, pct, COL_ACCENT);
}

// ────────────────────────────────────────────────────────────────────
//  MAIN IDLE SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowIdle() {
  tft.fillScreen(COL_BG);
  lcdSidebar();
  lcdDrawTopbar();

  // hero card
  tft.fillRoundRect(16, 44, SCREEN_W - 32, 148, 14, COL_PANEL);
  tft.drawRoundRect(16, 44, SCREEN_W - 32, 148, 14, COL_BORDER);
  tft.fillRoundRect(16, 44, SCREEN_W - 32, 6, 3, COL_ACCENT2);
  tft.fillRect(16, 47, SCREEN_W - 32, 3, COL_ACCENT);

  lcdDrawFingerprint(SCREEN_W / 2, ICON_CY, 18, COL_ACCENT, COL_ACCENT2);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString(autoScan ? "Siap memindai sidik jari" : "Mode idle — aktifkan scan", SCREEN_W / 2, STATUS_Y - 4);
  tft.setTextColor(COL_DIM2, COL_PANEL);
  tft.drawString(autoScan ? "Tempelkan jari ke sensor" : "Buka web UI untuk enroll", SCREEN_W / 2, STATUS_Y + 12);

  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  SCANNING ANIMATION
// ────────────────────────────────────────────────────────────────────
void lcdShowScanning() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  lcdSidebar();
  tft.fillRoundRect(16, 44, SCREEN_W - 32, 148, 14, COL_PANEL);
  tft.drawRoundRect(16, 44, SCREEN_W - 32, 148, 14, COL_ACCENT);
  lcdDrawFingerprint(SCREEN_W / 2, ICON_CY - 6, 20, COL_ACCENT, COL_ACCENT3);
  lcdBadgeCenter(STATUS_Y - 6, "SCANNING", COL_ACCENT2, COL_ACCENT3);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("Menganalisis sidik jari...", SCREEN_W / 2, STATUS_Y + 22);
}

// ────────────────────────────────────────────────────────────────────
//  MATCH RESULT
// ────────────────────────────────────────────────────────────────────
void lcdShowMatch(int id, int conf, const char *name) {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  lcdSidebar();
  lcdCard(42, 152, COL_OK);
  tft.fillCircle(SCREEN_W / 2, 70, 16, COL_OK);
  tft.drawCircle(SCREEN_W / 2, 70, 18, COL_OK);
  tft.drawLine(SCREEN_W / 2 - 9, 70, SCREEN_W / 2 - 3, 78, COL_BG);
  tft.drawLine(SCREEN_W / 2 - 3, 78, SCREEN_W / 2 + 8, 62, COL_BG);
  tft.drawLine(SCREEN_W / 2 - 8, 70, SCREEN_W / 2 - 2, 78, COL_BG);
  tft.drawLine(SCREEN_W / 2 - 2, 78, SCREEN_W / 2 + 9, 63, COL_BG);
  lcdBadgeCenter(98, "MATCH", COL_OK_DARK, COL_OK);
  char buf[24];
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.setTextSize(2);
  snprintf(buf, sizeof(buf), "ID %d", id);
  tft.drawString(buf, SCREEN_W / 2, 122);
  if (name && name[0]) {
    tft.setTextSize(1);
    tft.setTextColor(COL_ACCENT3, COL_PANEL);
    tft.drawString(name, SCREEN_W / 2, 142);
  }
  int confPct = conf * 100 / 256;
  lcdProgressBar(40, 158, SCREEN_W - 80, 10, confPct, COL_OK);
  snprintf(buf, sizeof(buf), "Confidence %d%%", confPct);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_OK, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString(buf, SCREEN_W / 2, 178);
  lcdDrawTopbar();
  lcdDrawFooter();
}

void lcdShowAttendanceStatus(const char *status) {
  tft.fillRoundRect(28, 176, SCREEN_W - 56, 14, 4, COL_DIM3);
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(1);
  if (strcmp(status, "checkin") == 0) {
    tft.setTextColor(COL_OK, COL_DIM3);
    tft.drawString("CHECK IN", SCREEN_W / 2, 183);
  } else if (strcmp(status, "checkout") == 0) {
    tft.setTextColor(COL_ACCENT, COL_DIM3);
    tft.drawString("CHECK OUT", SCREEN_W / 2, 183);
  } else if (strcmp(status, "ignored") == 0) {
    tft.setTextColor(COL_WARN, COL_DIM3);
    tft.drawString("SUDAH ABSEN", SCREEN_W / 2, 183);
  } else {
    tft.setTextColor(COL_ERR, COL_DIM3);
    tft.drawString("GAGAL KIRIM", SCREEN_W / 2, 183);
  }
}

// ────────────────────────────────────────────────────────────────────
//  NO MATCH RESULT
// ────────────────────────────────────────────────────────────────────
void lcdShowNoMatch() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  lcdSidebar();
  lcdCard(52, 128, COL_ERR);
  tft.fillCircle(SCREEN_W / 2, 78, 15, COL_ERR);
  tft.drawCircle(SCREEN_W / 2, 78, 17, COL_ERR);
  tft.drawLine(SCREEN_W / 2 - 7, 78, SCREEN_W / 2 + 7, 78, COL_BG);
  tft.drawLine(SCREEN_W / 2 - 6, 78, SCREEN_W / 2 + 6, 78, COL_BG);
  lcdBadgeCenter(112, "NO MATCH", COL_ERR_DARK, COL_ERR);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("Sidik jari belum terdaftar", SCREEN_W / 2, 148);
  lcdDrawTopbar();
  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  SENSOR ERROR
// ────────────────────────────────────────────────────────────────────
void lcdShowSensorErr() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  lcdSidebar();
  lcdCard(58, 110, COL_ERR);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_ERR, COL_PANEL);
  tft.setTextSize(2);
  tft.drawString("SENSOR", SCREEN_W / 2, 88);
  tft.drawString("ERROR", SCREEN_W / 2, 112);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("Periksa kabel & daya", SCREEN_W / 2, 140);
  lcdDrawTopbar();
  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  ENROLL SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowEnrollTitle(uint8_t id) {
  tft.fillScreen(COL_BG);
  lcdSidebar();
  tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
  tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, COL_BORDER);
  lcdBrandMark(8, 7);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_ACCENT, COL_TOPBAR);
  tft.setTextSize(1);
  tft.drawString("ENROLL", 30, 6);
  char hdr[20];
  snprintf(hdr, sizeof(hdr), "Slot ID %d", id);
  tft.setTextColor(COL_DIM, COL_TOPBAR);
  tft.drawString(hdr, 30, 16);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_ACCENT3, COL_TOPBAR);
  tft.drawString("PJTKI", SCREEN_W - 8, 12);
}

void lcdEnrollStep(const char *step, int pct, const char *detail, uint16_t col) {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, 110, COL_BG);
  lcdSidebar();
  tft.fillRoundRect(16, TOPBAR_H + 8, SCREEN_W - 32, 96, 12, COL_PANEL);
  tft.drawRoundRect(16, TOPBAR_H + 8, SCREEN_W - 32, 96, 12, COL_BORDER);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col, COL_PANEL);
  tft.setTextSize(2);
  tft.drawString(step, SCREEN_W / 2, TOPBAR_H + 36);
  if (pct >= 0) lcdProgressBar(40, TOPBAR_H + 58, SCREEN_W - 80, 10, pct, col);
  if (detail) {
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.setTextSize(1);
    tft.drawString(detail, SCREEN_W / 2, TOPBAR_H + 84);
  }
}

void lcdEnrollOk(const char *msg) {
  tft.fillRoundRect(28, 128, SCREEN_W - 56, 42, 12, COL_OK_DARK);
  tft.drawRoundRect(28, 128, SCREEN_W - 56, 42, 12, COL_OK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_OK, COL_OK_DARK);
  tft.setTextSize(2);
  tft.drawString(msg, SCREEN_W / 2, 149);
}

void lcdEnrollErr(const char *msg) {
  tft.fillRoundRect(28, 128, SCREEN_W - 56, 42, 12, COL_ERR_DARK);
  tft.drawRoundRect(28, 128, SCREEN_W - 56, 42, 12, COL_ERR);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_TEXT, COL_ERR_DARK);
  tft.setTextSize(2);
  tft.drawString(msg, SCREEN_W / 2, 149);
}

// ────────────────────────────────────────────────────────────────────
//  SERIAL + FINGER SENSOR HELPERS
// ────────────────────────────────────────────────────────────────────
void flushRX() { while (altSerial.available()) altSerial.read(); }

void emit(const __FlashStringHelper *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), (const char *)fmt, ap);
  va_end(ap);
  Serial.println(buf);
  Serial.flush();
  broadcastSSE(buf);
}

bool nextLine(char *out, size_t n) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      while (rxLen > 0 && (rxBuf[rxLen - 1] == ' ' || rxBuf[rxLen - 1] == '\r')) rxBuf[--rxLen] = 0;
      rxBuf[rxLen] = 0; rxLen = 0;
      strncpy(out, rxBuf, n); out[n - 1] = 0;
      return out[0] != 0;
    }
    if (c != '\r' && rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c;
  }
  return false;
}

bool waitNoFinger() {
  unsigned long t = millis();
  while (millis() - t < 10000) {
    lastScanActivity = millis();
    flushRX();
    if (finger.getImage() == FINGERPRINT_NOFINGER) return true;
    delay(50); yield();
  }
  return false;
}

bool waitFinger() {
  unsigned long t = millis();
  while (millis() - t < 10000) {
    lastScanActivity = millis();
    flushRX();
    if (finger.getImage() == FINGERPRINT_OK) return true;
    delay(50); yield();
  }
  return false;
}

// ────────────────────────────────────────────────────────────────────
//  FINGERPRINT DB (LittleFS)
// ────────────────────────────────────────────────────────────────────
void dbLoad() {
  fpCount = 0;
  if (!storageReady) return;
  File f = LittleFS.open("/fingerprints.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  for (JsonPair p : doc.as<JsonObject>()) {
    if (fpCount >= MAX_FP) break;
    fpDB[fpCount].id = atoi(p.key().c_str());
    strncpy(fpDB[fpCount].name, p.value()["name"] | "", 31);
    strncpy(fpDB[fpCount].empId, p.value()["employeeId"] | "", 15);
    fpCount++;
  }
}

void dbSave() {
  if (!storageReady) return;
  DynamicJsonDocument doc(4096);
  for (int i = 0; i < fpCount; i++) {
    JsonObject o = doc.createNestedObject(String(fpDB[i].id));
    o["name"] = fpDB[i].name;
    if (fpDB[i].empId[0]) o["employeeId"] = fpDB[i].empId;
  }
  File f = LittleFS.open("/fingerprints.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

void dbAdd(uint8_t id, const char *name, const char *empId) {
  for (int i = 0; i < fpCount; i++) {
    if (fpDB[i].id == id) {
      strncpy(fpDB[i].name, name, 31);
      strncpy(fpDB[i].empId, empId ? empId : "", 15);
      dbSave(); return;
    }
  }
  if (fpCount < MAX_FP) {
    fpDB[fpCount].id = id;
    strncpy(fpDB[fpCount].name, name, 31);
    strncpy(fpDB[fpCount].empId, empId ? empId : "", 15);
    fpCount++;
    dbSave();
  }
}

void dbRemove(uint8_t id) {
  for (int i = 0; i < fpCount; i++) {
    if (fpDB[i].id == id) {
      for (int j = i; j < fpCount - 1; j++) fpDB[j] = fpDB[j + 1];
      fpCount--; dbSave(); return;
    }
  }
}

void dbClear() { fpCount = 0; dbSave(); }

const char* dbGetName(uint8_t id) {
  for (int i = 0; i < fpCount; i++) if (fpDB[i].id == id) return fpDB[i].name;
  return "";
}

const char* dbGetEmpId(uint8_t id) {
  for (int i = 0; i < fpCount; i++) if (fpDB[i].id == id) return fpDB[i].empId;
  return "";
}

// ────────────────────────────────────────────────────────────────────
//  SSE (Server-Sent Events)
// ────────────────────────────────────────────────────────────────────
void broadcastSSE(const char *msg) {
  String data = "data: ";
  data += msg;
  data += "\n\n";
  for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
    if (sseClients[i]) {
      sseClients[i].print(data);
      if (!sseClients[i].connected()) { sseClients[i].stop(); sseClients[i] = WiFiClient(); }
    }
  }
}

void handleSSE() {
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println("Access-Control-Allow-Origin: *");
  client.println();
  client.print(": connected\n\n");

  for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
    if (!sseClients[i] || !sseClients[i].connected()) {
      sseClients[i] = client;
      return;
    }
  }
  client.stop();
}

// ────────────────────────────────────────────────────────────────────
//  WiFi Manager - Load/Save credentials
// ────────────────────────────────────────────────────────────────────
void wifiLoadCreds() {
  savedWiFiCount = 0;
  if (!storageReady) return;
  File f = LittleFS.open(WIFI_FILENAME, "r");
  if (!f) return;
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (savedWiFiCount >= MAX_SAVED_WIFI) break;
    const char *s = obj["ssid"] | "";
    const char *p = obj["pass"] | "";
    if (s[0]) {
      strncpy(savedWiFi[savedWiFiCount].ssid, s, 32);
      savedWiFi[savedWiFiCount].ssid[32] = 0;
      strncpy(savedWiFi[savedWiFiCount].pass, p, 64);
      savedWiFi[savedWiFiCount].pass[64] = 0;
      savedWiFiCount++;
    }
  }
  Serial.printf("[WiFi] Loaded %d saved credential(s)\n", savedWiFiCount);
}

bool wifiSaveCreds() {
  if (!storageReady) return false;
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < savedWiFiCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = savedWiFi[i].ssid;
    obj["pass"] = savedWiFi[i].pass;
  }
  File f = LittleFS.open(WIFI_FILENAME, "w");
  if (!f) { logError("wifi credentials write open failed"); return false; }
  serializeJson(doc, f);
  f.close();
  return true;
}

bool wifiAddCreds(const char *ssid, const char *pass) {
  if (!storageReady) return false;
  for (int i = 0; i < savedWiFiCount; i++) {
    if (strcmp(savedWiFi[i].ssid, ssid) == 0) {
      char oldPass[65];
      strncpy(oldPass, savedWiFi[i].pass, sizeof(oldPass) - 1);
      oldPass[sizeof(oldPass) - 1] = 0;
      strncpy(savedWiFi[i].pass, pass, 64);
      savedWiFi[i].pass[64] = 0;
      if (wifiSaveCreds()) return true;
      strncpy(savedWiFi[i].pass, oldPass, sizeof(savedWiFi[i].pass) - 1);
      savedWiFi[i].pass[sizeof(savedWiFi[i].pass) - 1] = 0;
      return false;
    }
  }
  if (savedWiFiCount >= MAX_SAVED_WIFI) return false;
  strncpy(savedWiFi[savedWiFiCount].ssid, ssid, 32);
  savedWiFi[savedWiFiCount].ssid[32] = 0;
  strncpy(savedWiFi[savedWiFiCount].pass, pass, 64);
  savedWiFi[savedWiFiCount].pass[64] = 0;
  savedWiFiCount++;
  if (wifiSaveCreds()) return true;
  savedWiFiCount--;
  memset(&savedWiFi[savedWiFiCount], 0, sizeof(savedWiFi[savedWiFiCount]));
  return false;
}

void wifiRemoveCreds(const char *ssid) {
  for (int i = 0; i < savedWiFiCount; i++) {
    if (strcmp(savedWiFi[i].ssid, ssid) == 0) {
      for (int j = i; j < savedWiFiCount - 1; j++) savedWiFi[j] = savedWiFi[j + 1];
      savedWiFiCount--;
      wifiSaveCreds();
      return;
    }
  }
}

void wifiClearCreds() {
  savedWiFiCount = 0;
  if (!storageReady) return;
  LittleFS.remove(WIFI_FILENAME);
}

// ────────────────────────────────────────────────────────────────────
//  App Settings (apiBaseUrl, kodeCabang, deviceId)
// ────────────────────────────────────────────────────────────────────
void settingsLoad() {
  memset(&appSettings, 0, sizeof(appSettings));
  appSettings.irEnabled = true;  // default aktif
  if (!storageReady) return;
  File f = LittleFS.open(SETTINGS_FILENAME, "r");
  if (!f) return;
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127); appSettings.apiBaseUrl[127] = 0;
  strncpy(appSettings.kodeCabang, doc["kode_cabang"] | "", 15); appSettings.kodeCabang[15] = 0;
  strncpy(appSettings.deviceId, doc["device_id"] | "", 31); appSettings.deviceId[31] = 0;
  strncpy(appSettings.apiKey, doc["api_key"] | "", 64); appSettings.apiKey[64] = 0;
  appSettings.irEnabled = doc["ir_enabled"] | true;
}

bool settingsSave() {
  if (!storageReady) return false;
  DynamicJsonDocument doc(768);
  doc["apiBaseUrl"] = appSettings.apiBaseUrl;
  doc["kode_cabang"] = appSettings.kodeCabang;
  doc["device_id"] = appSettings.deviceId;
  doc["api_key"] = appSettings.apiKey;
  doc["ir_enabled"] = appSettings.irEnabled;
  File f = LittleFS.open(SETTINGS_FILENAME, "w");
  if (!f) { logError("settings write open failed"); return false; }
  serializeJson(doc, f);
  f.close();
  return true;
}

// ────────────────────────────────────────────────────────────────────
//  Backend API - POST attendance on fingerprint match
// ────────────────────────────────────────────────────────────────────
String postAttendance(const char *employeeId) {
  if (!appSettings.apiBaseUrl[0] || !employeeId || !employeeId[0]) {
    logError("attendance skipped: missing API URL or employee ID");
    return "";
  }
  if (WiFi.status() != WL_CONNECTED) {
    logError("attendance skipped: WiFi disconnected");
    return "";
  }

  String url = String(appSettings.apiBaseUrl) + "/api/finger/arduino/attendance";

  DynamicJsonDocument body(256);
  body["employeeId"] = employeeId;
  body["device_id"] = appSettings.deviceId;
  body["kode_cabang"] = appSettings.kodeCabang;
  // NTP time as YYYY-MM-DDTHH:MM:SS
  String ntpTime = timeClient.getFormattedTime();
  unsigned long epoch = timeClient.getEpochTime();
  struct tm *ti = localtime((time_t *)&epoch);
  char timeBuf[24];
  snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%s",
           ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ntpTime.c_str());
  body["time"] = timeBuf;

  String json;
  serializeJson(body, json);

  bool isHttps = url.startsWith("https://");
  // Static/reused client — JANGAN new+delete tiap request. `http.end()`
  // menutup koneksi tapi paket FIN/TLS close_notify masih diproses ASYNC
  // oleh task WiFi driver (ppTask). Kalau objek client langsung di-delete
  // (memori dibebaskan) sebelum proses async itu selesai, ppTask nanti
  // baca memori yang sudah bebas → use-after-free → Guru Meditation
  // LoadProhibited persis di ppTask/lmac* (root cause crash nyata
  // 2026-08-06, konsisten dgn laporan komunitas arduino-esp32#3659).
  static WiFiClientSecure attnSecureClient;
  static WiFiClient attnPlainClient;
  static bool attnSecureInit = false;
  WiFiClient *client;
  if (isHttps) {
    if (!attnSecureInit) { attnSecureClient.setInsecure(); attnSecureInit = true; }
    client = &attnSecureClient;
  } else {
    client = &attnPlainClient;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.begin(*client, url);
  http.addHeader("Content-Type", "application/json");
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  lastScanActivity = millis();  // cegah watchdog selama HTTP
  int code = http.POST(json);
  String resp = "";
  if (code > 0) {
    resp = http.getString();
    if (code < 200 || code >= 300) logError("attendance HTTP status=%d", code);
  } else {
    logError("attendance HTTP failed code=%d", code);
  }
  http.end();
  lastScanActivity = millis();
  return resp;
}


// Register enroll ke server PJTKI (POST /api/finger/arduino/register)
String postRegister(const char *employeeId, uint8_t fingerId, const char *templateHex) {
  if (!appSettings.apiBaseUrl[0] || !employeeId || !employeeId[0]) {
    logError("register skipped: missing API URL or employee ID");
    return "";
  }
  if (WiFi.status() != WL_CONNECTED) {
    logError("register skipped: WiFi disconnected");
    return "";
  }

  String url = String(appSettings.apiBaseUrl) + "/api/finger/arduino/register";

  DynamicJsonDocument body(1536);
  body["employeeId"] = employeeId;
  body["device_id"] = appSettings.deviceId;
  body["kode_cabang"] = appSettings.kodeCabang;
  body["finger_id"] = fingerId;
  if (templateHex && templateHex[0]) body["templateHex"] = templateHex;

  String json;
  serializeJson(body, json);

  bool isHttps = url.startsWith("https://");
  // Static/reused client — lihat catatan di postAttendance() soal
  // use-after-free saat client di-new/delete tiap request.
  static WiFiClientSecure regSecureClient;
  static WiFiClient regPlainClient;
  static bool regSecureInit = false;
  WiFiClient *client;
  if (isHttps) {
    if (!regSecureInit) { regSecureClient.setInsecure(); regSecureInit = true; }
    client = &regSecureClient;
  } else {
    client = &regPlainClient;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.begin(*client, url);
  http.addHeader("Content-Type", "application/json");
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  int code = http.POST(json);
  String resp = "";
  if (code > 0) {
    resp = http.getString();
    Serial.printf("[API] register HTTP %d: %s\n", code, resp.c_str());
    if (code < 200 || code >= 300) logError("register HTTP status=%d", code);
  } else {
    Serial.printf("[API] register fail code=%d\n", code);
    logError("register HTTP failed code=%d", code);
  }
  http.end();
  return resp;
}

// ────────────────────────────────────────────────────────────────────
//  WiFi Manager - power policy (3.3V panas jika radio selalu full)
// ────────────────────────────────────────────────────────────────────
// Hanya TX power + flag. JANGAN ganti mode/AP di sini berkala —
// itu memutus web UI (kesan "reboot") saat buka tab Setelan/WiFi.
void wifiApplyPowerPolicy() {
  WiFi.setTxPower(WIFI_POWER_11dBm);
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    staIP = WiFi.localIP().toString();
    // Modem sleep MATI: bug lwIP → assert saat TCP aktif di beberapa core.
    WiFi.setSleep(false);
  } else {
    wifiConnected = false;
    WiFi.setSleep(false);
  }
}

// Pause autoscan + refresh watchdog selama operasi WiFi blocking
void wifiOpsBegin() {
  autoScan = false;
  lastScanActivity = millis();
  lastRecoveryAttempt = millis();
  consecutiveErrors = 0;
}

void wifiOpsEnd(bool resumeAuto) {
  // 3.3V: RF scan mengganggu UART FPM10A — flush + cooldown sebelum autoscan
  flushRX();
  delay(250);
  flushRX();
  lastScanActivity = millis();
  lastRecoveryAttempt = millis();
  consecutiveErrors = 0;
  recoveryCount = 0;
  scanState = SCAN_IDLE;
  scanCooldownUntil = millis() + 5000;
  if (resumeAuto) autoScan = true;
}

// ────────────────────────────────────────────────────────────────────
//  WiFi Manager - Init (AP for setup, STA if credentials exist)
// ────────────────────────────────────────────────────────────────────
void wifiInit() {
  wifiLoadCreds();

  // Boot: mulai hemat CPU (240MHz default = panas + drain).
  // 80MHz cukup untuk polling sensor 20Hz + web UI + HTTP; UART2 hardware
  // tidak terpengaruh clock CPU.
  setCpuFrequencyMhz(80);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // sementara AP aktif untuk setup
  WiFi.softAP(AP_SSID, cred.apPass);
  Serial.printf("[WiFi] AP: %s | AP IP: 192.168.4.1\n", AP_SSID);

  if (savedWiFiCount == 0) {
    Serial.println("[WiFi] No saved credentials, AP only");
    wifiApplyPowerPolicy();
    return;
  }

  for (int i = 0; i < savedWiFiCount; i++) {
    Serial.printf("[WiFi] Trying saved: %s\n", savedWiFi[i].ssid);
    WiFi.begin(savedWiFi[i].ssid, savedWiFi[i].pass);

    unsigned long start = millis();
    while (millis() - start < WIFI_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      staIP = WiFi.localIP().toString();
      staSSID = String(savedWiFi[i].ssid);
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
      Serial.printf("[WiFi] Connected to %s | IP: %s | STA\n", savedWiFi[i].ssid, staIP.c_str());
      return;
    }
    WiFi.disconnect();
  }

  Serial.println("[WiFi] All saved networks failed, AP only");
  logError("WiFi init: all saved networks failed");
  wifiApplyPowerPolicy();
}


bool getTemplateRaw(uint16_t id, uint8_t *buf);
String toHex(const uint8_t *buf, size_t len);
bool putTemplateRaw(uint16_t id, const uint8_t *buf);
bool fromHex(const char *hex, uint8_t *buf, size_t maxLen);
String apiProxyGet(const char *path, int &httpCode);
String apiProxyPost(const char *path, const String &body, int &httpCode);

// ────────────────────────────────────────────────────────────────────
//  ENROLLMENT
// ────────────────────────────────────────────────────────────────────
// Reset state agar watchdog/scan error tidak ESP.restart() tepat setelah enroll
void enrollCleanupResumeScan() {
  if (ledOn) { finger.LEDcontrol(false); ledOn = false; }
  flushRX();
  delay(80);
  lastScanActivity = millis();
  lastRecoveryAttempt = millis();
  recoveryCount = 0;
  consecutiveErrors = 0;
  scanState = SCAN_IDLE;
  scanCooldownUntil = millis() + 1500;
  fingerDown = false;
  autoScan = true;
  enrollActive = false;
  lcdShowIdle();
}

uint8_t enrollFinger(uint8_t id, const char *name, const char *empId) {
  int p = -1;
  enrollActive = true;
  autoScan = false;
  lastScanActivity = millis();  // cegah false watchdog selama enroll (bisa >20s)
  flushRX();

  lcdShowEnrollTitle(id);
  lcdEnrollStep("Remove finger", -1, "Clear sensor first", COL_TEXT);
  emit(F("{\"event\":\"enroll_start\",\"id\":%d}"), id);
  if (!waitNoFinger()) { logError("enroll timeout waiting for no finger"); lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); enrollCleanupResumeScan(); return 0xFE; }

  for (int attempt = 0; attempt < 3; attempt++) {
    lastScanActivity = millis();
    lcdEnrollStep("Place finger", 10, "Touch sensor gently", COL_WARN);
    emit(F("{\"event\":\"waiting_finger\"}"));
    finger.LEDcontrol(true); delay(200); ledOn = true;
    if (!waitFinger()) { logError("enroll timeout waiting for finger step=1"); finger.LEDcontrol(false); ledOn = false; lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); enrollCleanupResumeScan(); return 0xFE; }

    lcdEnrollStep("Capturing...", 25, "Reading fingerprint", COL_ACCENT);
    flushRX(); delay(200);
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
      logError("enroll image2Tz step=1 failed code=%d", p);
      lcdEnrollErr("Bad Image #1");
      emit(F("{\"event\":\"bad_image\",\"step\":1,\"code\":%d}"), p);
      if (!waitNoFinger()) { logError("enroll timeout after bad image step=1"); lcdEnrollErr("TIMEOUT"); enrollCleanupResumeScan(); return 0xFE; }
      continue;
    }
    lcdEnrollStep("Step 1 OK", 40, "First scan captured", COL_OK);
    emit(F("{\"event\":\"image_ok_step1\"}"));

    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
      char msg[32];
      snprintf(msg, sizeof(msg), "ID:%d exists", finger.fingerID);
      lcdEnrollErr(msg);
      emit(F("{\"event\":\"already_registered\",\"id\":%d}"), finger.fingerID);
      waitNoFinger();
      enrollCleanupResumeScan();
      return 0xFF;
    }

    lcdEnrollStep("Remove finger", 50, "Lift finger off sensor", COL_TEXT);
    emit(F("{\"event\":\"remove\"}"));
    if (!waitNoFinger()) { logError("enroll timeout removing finger step=1"); lcdEnrollErr("TIMEOUT"); enrollCleanupResumeScan(); return 0xFE; }

    lcdEnrollStep("Place again", 60, "Same finger, same spot", COL_WARN);
    emit(F("{\"event\":\"waiting_finger_2\"}"));
    finger.LEDcontrol(true); delay(200); ledOn = true;
    if (!waitFinger()) { logError("enroll timeout waiting for finger step=2"); finger.LEDcontrol(false); ledOn = false; lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); enrollCleanupResumeScan(); return 0xFE; }

    lcdEnrollStep("Capturing...", 75, "Reading fingerprint", COL_ACCENT);
    flushRX(); delay(200);
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
      logError("enroll image2Tz step=2 failed code=%d", p);
      lcdEnrollErr("Bad Image #2");
      emit(F("{\"event\":\"bad_image\",\"step\":2,\"code\":%d}"), p);
      if (!waitNoFinger()) { logError("enroll timeout after bad image step=2"); lcdEnrollErr("TIMEOUT"); enrollCleanupResumeScan(); return 0xFE; }
      continue;
    }
    lcdEnrollStep("Step 2 OK", 85, "Second scan captured", COL_OK);
    emit(F("{\"event\":\"image_ok_step2\"}"));

    lcdEnrollStep("Building model...", 90, "Matching patterns", COL_ACCENT);
    p = finger.createModel();
    if (p == FINGERPRINT_OK) break;

    logError("enroll createModel failed code=%d attempt=%d", p, attempt + 1);
    char msg[24];
    snprintf(msg, sizeof(msg), "Retry %d/3", attempt + 1);
    lcdEnrollErr(msg);
    emit(F("{\"event\":\"retry_create\",\"attempt\":%d}"), attempt + 1);
    if (!waitNoFinger()) { logError("enroll timeout after createModel failure"); lcdEnrollErr("TIMEOUT"); enrollCleanupResumeScan(); return 0xFE; }
    if (attempt == 2) {
      lcdEnrollErr("Model FAILED");
      emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
      enrollCleanupResumeScan();
      return p;
    }
  }

  lcdEnrollStep("Storing...", 95, "Saving to sensor", COL_ACCENT);
  delay(100);
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    logError("enroll storeModel failed id=%d code=%d", id, p);
    lcdEnrollErr("Store FAILED");
    emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
    enrollCleanupResumeScan();
    return p;
  }

  dbAdd(id, name, empId);

  // Upload template hex ke server supaya bisa di-sync ke device lain.
  // Sebelumnya di-skip di 3.3V karena diduga heap/brownout — root cause
  // crash asli ternyata bug use-after-free WiFiClientSecure di HTTP client
  // (sudah diperbaiki 2026-08-06), BUKAN soal ukuran payload. Sekarang aman
  // diaktifkan, konsisten dengan versi 5V.
  if (wifiConnected && empId && empId[0]) {
    uint8_t tplBuf[256];
    String hexStr = "";
    if (getTemplateRaw(id, tplBuf)) {
      hexStr = toHex(tplBuf, 256);
    } else {
      Serial.println("[API] getTemplateRaw failed — register tanpa hex");
    }
    String regResp = postRegister(empId, id, hexStr.length() ? hexStr.c_str() : "");
    if (regResp.length() > 0) {
      emit(F("{\"event\":\"register_server\",\"ok\":true}"));
      Serial.printf("[API] register resp: %s\n", regResp.c_str());
    } else {
      emit(F("{\"event\":\"register_server\",\"response\":{\"status\":\"error\",\"message\":\"backend_unreachable\"}}"));
    }
  }

  lcdEnrollStep("DONE", 100, NULL, COL_OK);
  char msg[32];
  snprintf(msg, sizeof(msg), "ID:%d Enrolled", id);
  lcdEnrollOk(msg);
  emit(F("{\"event\":\"enrolled\",\"id\":%d,\"name\":\"%s\"}"), id, name);
  delay(2000);

  finger.getTemplateCount();
  waitNoFinger();
  enrollCleanupResumeScan();
  return p;
}

// ────────────────────────────────────────────────────────────────────
//  AUTO-SCAN (fully non-blocking state machine)
// ────────────────────────────────────────────────────────────────────
// ────────────────────────────────────────────────────────────────────
//  TOUCH-PRESENCE GATE (T-OUT FPM10A / IR obstacle)
// ────────────────────────────────────────────────────────────────────
// Gate kehadiran jari untuk getImage() — sensor hanya dipoll saat ada jari,
// memotong chatter UART terus-menerus & mengurangi false trigger FPM10A
// yang over-sensitive.
//
// Sumber sinyal (pilih salah satu, samakan ke TOUCH_PIN):
//   a) T-OUT FPM10A (REKOMENDASI): output capacitive touch, aktif HIGH saat
//      jari menyentuh kaca. Wiring: T-3V3→3V3, T-OUT→GPIO13. CMOS push-pull,
//      stabil, tidak kena feedback lampu FPM10A.
//   b) Modul IR obstacle 3-pin (VCC/GND/OUT): VCC→3V3, GND→GND, OUT→GPIO13.
//      Lebih rewel — bisa flicker karena feedback LED capture FPM10A.
//
// Polaritas (active-high vs active-low) dideteksi otomatis saat boot.
// Jangan sentuh sensor selama kalibrasi boot.
//
// Anti-flicker (2026-08-06): keluaran bisa tidak stabil (trimpot terlalu
// sensitif / feedback LED FPM10A). Solusi 3 lapis:
//   1. irUpdateGate(): debounce — butuh IR_CONFIRM_MS deteksi kontinu untuk
//      BUKA gate, IR_RELEASE_MS kosong kontinu untuk TUTUP gate.
//   2. IR_GATE_TIMEOUT_MS: gate terbuka lama tanpa hasil → tutup sementara
//      (putus feedback), lalu IR_GATE_COOLDOWN_MS sebelum evaluasi ulang.
//   3. FALLBACK_POLL_MS: saat gate tutup, tetap poll getImage pelan-pelan
//      sebagai jaring pengaman — scan tetap jalan walau gate gagal.
bool irPolarityHigh = true;  // true = jari terdeteksi saat pin HIGH
bool irCalibrated = false;

void irCalibrate() {
  int hi = 0, lo = 0;
  for (int i = 0; i < 20; i++) {
    if (digitalRead(TOUCH_PIN) == HIGH) hi++; else lo++;
    delay(25); yield();
  }
  irPolarityHigh = (lo > hi);  // idle LOW → finger = HIGH, dan sebaliknya
  irCalibrated = true;
  Serial.printf("[TOUCH] idle=%s → finger=%s\n", (lo > hi) ? "LOW" : "HIGH",
                irPolarityHigh ? "HIGH" : "LOW");
}

bool irRawDetected() {
  if (!irCalibrated) return true;
  bool lvl = (digitalRead(TOUCH_PIN) == HIGH);
  return irPolarityHigh ? lvl : !lvl;
}

// ── Gate state machine ────────────────────────────────────────────
bool irGateOpen = false;
unsigned long irDetSince = 0;        // sejak kapan IR terdeteksi kontinu
unsigned long irClearSince = 0;      // sejak kapan IR kosong kontinu
unsigned long irGateOpenedAt = 0;    // sejak kapan gate terbuka
unsigned long irGateCooldownUntil = 0;
unsigned long lastFallbackPoll = 0;
const unsigned long IR_CONFIRM_MS = 100;
const unsigned long IR_RELEASE_MS = 400;
const unsigned long IR_GATE_TIMEOUT_MS = 3000;
const unsigned long IR_GATE_COOLDOWN_MS = 2500;
const unsigned long FALLBACK_POLL_MS = 1500;

void irUpdateGate() {
  if (!appSettings.irEnabled) {
    irGateOpen = true;  // IR nonaktif → gate selalu terbuka (perilaku lama)
    irDetSince = irClearSince = 0;
    return;
  }
  bool raw = irRawDetected();
  if (raw) {
    irClearSince = 0;
    if (!irGateOpen) {
      if (!irDetSince) irDetSince = millis();
      else if (millis() - irDetSince >= IR_CONFIRM_MS) {
        irGateOpen = true;
        irGateOpenedAt = millis();
        logDebug("GATE open");
      }
    }
  } else {
    irDetSince = 0;
    if (irGateOpen) {
      if (!irClearSince) irClearSince = millis();
      else if (millis() - irClearSince >= IR_RELEASE_MS) {
        irGateOpen = false;
        irClearSince = 0;
        logDebug("GATE close (release)");
      }
    }
  }
  // Runaway timeout: gate terbuka >3s tanpa hasil → tutup sementara
  if (irGateOpen && millis() - irGateOpenedAt > IR_GATE_TIMEOUT_MS) {
    irGateOpen = false;
    irGateCooldownUntil = millis() + IR_GATE_COOLDOWN_MS;
    irGateOpenedAt = irDetSince = irClearSince = 0;
    logDebug("GATE timeout");
  }
  // Cooldown: jangan buka gate lagi selama periode cooldown
  if (!irGateOpen && millis() < irGateCooldownUntil) irDetSince = 0;
}

bool irGateActive() { return irGateOpen; }

bool irShouldPoll() {
  irUpdateGate();
  return irGateOpen;
  // Fallback & cooldown sudah ditangani langsung di SCAN_IDLE; di sini
  // hanya kembalikan gate state — supaya bisa dipakai sebagai gate scan.
}

void checkAutoSleep() {
  if (!timeClient.isTimeSet()) { scanSleeping = false; return; }
  int h = timeClient.getHours();
  scanSleeping = (h >= 0 && h < 5);
}

void doAutoScan() {
  static int fingerConfirm = 0;  // debounce: butuh 2x OK berturut
  static const int FINGER_CONFIRM_NEEDED = 2;

  switch (scanState) {

    case SCAN_IDLE: {
      yield();
      if (scanSleeping) break;
      if (millis() < scanCooldownUntil) break;

      irUpdateGate();

      if (irGateOpen) {
        // Gate terbuka — jari terdeteksi. Nyalakan LED + polling getImage.
        lastLcdActivity = millis();
        lcdBacklightOn = true;
        ledcWrite(LCD_BL, 255);
        if (!ledOn) {
          finger.LEDcontrol(true);
          ledOn = true;
          logDebug("LED ON");
          scanCooldownUntil = millis() + LED_WARMUP_MS;
          break;  // non-blocking: skip getImage sampai LED stabil
        }
      } else {
        // Gate tertutup — standby. Skip getImage, hanya fallback ping.
        static unsigned long lastLedOffAttempt = 0;
        if (ledOn && (lastLedOffAttempt == 0 || millis() - lastLedOffAttempt >= LED_OFF_RETRY_MS)) {
          lastLedOffAttempt = millis();
          uint8_t ledResult = finger.LEDcontrol(false);
          if (ledResult == FINGERPRINT_OK) {
            logDebug("LED OFF (standby)");
            ledOn = false;
          } else {
            Serial.printf("[SCAN] LED OFF err: %d\n", ledResult);
            logError("LED OFF failed code=%d", ledResult);
          }
        }
        if (lcdBacklightOn) ledcWrite(LCD_BL, 30);
        if (millis() - lastFallbackPoll >= FALLBACK_POLL_MS) {
          lastFallbackPoll = millis();
          int ping = finger.getTemplateCount();
          if (ping == FINGERPRINT_OK) {
            fallbackErrors = 0;
            lastScanActivity = millis();
          } else {
            fallbackErrors++;
            if (fallbackErrors <= 3) {
              Serial.printf("[SCAN] idle ping err: %d (x%d)\n", ping, fallbackErrors);
            }
            logError("idle template ping failed code=%d consecutive=%d", ping, fallbackErrors);
            flushRX();
          }
        }
        return;
      }

      int p = finger.getImage();
      if (p == FINGERPRINT_OK || p == FINGERPRINT_NOFINGER) {
        lastScanActivity = millis();
      }

      if (p == FINGERPRINT_OK) {
        fingerConfirm++;
        if (fingerConfirm < FINGER_CONFIRM_NEEDED) {
          scanCooldownUntil = millis() + 30;
          break;
        }
        fingerConfirm = 0;
        logDebug("SCAN image OK (debounce %d)", FINGER_CONFIRM_NEEDED);
        ledcWrite(LCD_BL, 255);
        scanState = SCAN_BUSY;
        scanResultTime = millis();  // set awal: timeout safety jika image2Tz/fingerSearch hang
        lcdShowScanning();

        p = finger.image2Tz();
        if (p != FINGERPRINT_OK) {
          Serial.printf("[SCAN] image2Tz fail: %d\n", p);
          logError("image2Tz failed code=%d", p);
          scanResultTime = millis();
          scanCooldownUntil = millis() + 500;
          scanState = SCAN_WAIT_RELEASE;
          return;
        }

        p = finger.fingerSearch();
        if (p == FINGERPRINT_OK) {
          logDebug("SCAN match id=%d conf=%d", finger.fingerID, finger.confidence);
          const char *nm = dbGetName(finger.fingerID);
          const char *eid = dbGetEmpId(finger.fingerID);
          lcdShowMatch(finger.fingerID, finger.confidence, nm);
          emit(F("{\"event\":\"match\",\"id\":%d,\"confidence\":%d,\"name\":\"%s\",\"employeeId\":\"%s\"}"),
               finger.fingerID, finger.confidence, nm, eid ? eid : "");
          { char bbuf[128]; snprintf(bbuf, sizeof(bbuf),
              "{\"event\":\"match\",\"id\":%d,\"confidence\":%d,\"name\":\"%s\",\"employeeId\":\"%s\"}",
              finger.fingerID, finger.confidence, nm, eid ? eid : "");
            bleNotifyEvent(bbuf); }
          if (wifiConnected && eid && eid[0]) {
            // JANGAN panggil WiFi.setTxPower() di sini — mengubah register
            // PHY TX power sementara driver WiFi (task "ppTask") sedang
            // aktif proses TX-queue/AMPDU/retry di packet lain menyebabkan
            // race → korupsi state internal driver → crash LoadProhibited
            // di ppTask (root cause bug nyata 2026-08-06 — jangan tambahkan
            // lagi). Root cause crash asli sudah diperbaiki di level
            // WiFiClientSecure (reuse client, tidak new/delete tiap
            // request) — jadi cooldown di bawah cukup singkat.
            String resp = postAttendance(eid);
            if (resp.length() > 0) {
              DynamicJsonDocument doc(512);
              if (!deserializeJson(doc, resp)) {
                const char *st = doc["status"] | "error";
                lcdShowAttendanceStatus(st);
              }
              emit(F("{\"event\":\"attendance\",\"ok\":true}"));
              Serial.printf("[API] attendance: %s\n", resp.c_str());
            } else {
              lcdShowAttendanceStatus("error");
            }
            flushRX();
          }
        } else {
          logDebug("SCAN nomatch code=%d", p);
          lcdShowNoMatch();
          emit(F("{\"event\":\"nomatch\",\"code\":%d}"), p);
          { char bbuf[64]; snprintf(bbuf, sizeof(bbuf),
              "{\"event\":\"nomatch\",\"code\":%d}", p);
            bleNotifyEvent(bbuf); }
        }

        scanResultTime = millis();
        lastScanActivity = millis();
        consecutiveErrors = 0;
        recoveryCount = 0;
        scanCooldownUntil = millis() + 500;
        scanState = SCAN_WAIT_RELEASE;
      } else if (p == FINGERPRINT_NOFINGER) {
        consecutiveErrors = 0;
        fingerConfirm = 0;
      } else {
        // Transient UART error (sering setelah WiFi scan) — JANGAN ESP.restart()
        consecutiveErrors++;
        if (consecutiveErrors <= 3) {
          Serial.printf("[SCAN] getImage err: %d (x%d)\n", p, consecutiveErrors);
        }
        logError("getImage failed code=%d consecutive=%d", p, consecutiveErrors);
        flushRX();
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          Serial.printf("[SCAN] %d errors → soft recover (no reboot)\n", consecutiveErrors);
          logError("soft recover after %d getImage errors", consecutiveErrors);
          emit(F("{\"event\":\"sensor_soft_recover\"}"));
          // Putus feedback LED dan reset gate segera, jangan tunggu timeout 3s.
          uint8_t ledResult = finger.LEDcontrol(false);
          ledOn = (ledResult != FINGERPRINT_OK);
          if (ledOn) {
            Serial.printf("[SCAN] LED OFF err: %d\n", ledResult);
            logError("LED OFF during soft recover failed code=%d", ledResult);
          }
          ledcWrite(LCD_BL, 30);
          irGateOpen = false;
          irGateCooldownUntil = millis() + SCAN_SOFT_RECOVER_MS;
          irGateOpenedAt = irDetSince = irClearSince = 0;
          fingerConfirm = 0;
          flushRX();
          delay(300);
          flushRX();
          consecutiveErrors = 0;
          scanCooldownUntil = millis() + SCAN_SOFT_RECOVER_MS;
          lastScanActivity = millis();
        }
        delay(150);
      }
      break;
    }

    case SCAN_BUSY:
      // Safety: jika image2Tz/fingerSearch hang di 3.3V (UART stuck)
      if (millis() - scanResultTime > 5000) {
        logDebug("SCAN BUSY timeout — force reset");
        flushRX();
        finger.LEDcontrol(false); ledOn = false;
        scanState = SCAN_IDLE;
        consecutiveErrors = 0;
        fingerConfirm = 0;
        lcdShowIdle();
      }
      break;

    case SCAN_WAIT_RELEASE: {
      // Timer 1.5 detik langsung lepas — jangan cek getImage() karena false positive
      // di 3.3V sering return OK walau jari sudah diangkat
      if (millis() - scanResultTime > 500) {
        flushRX();
        finger.LEDcontrol(false); ledOn = false;
        logDebug("LED OFF (post-scan)");
        irGateOpen = false;
        irGateCooldownUntil = millis() + 200;
        irGateOpenedAt = irDetSince = irClearSince = 0;
        scanState = SCAN_IDLE;
        consecutiveErrors = 0;
        fingerConfirm = 0;
        lcdShowIdle();
      }
      break;
    }
  }
}

// ────────────────────────────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ────────────────────────────────────────────────────────────────────
extern const char INDEX_HTML[] PROGMEM;

// ────────────────────────────────────────────────────────────────────
//  WEB API HANDLERS
// ────────────────────────────────────────────────────────────────────
void handleRoot() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  finger.getTemplateCount();
  String json = "{\"ready\":" + String(sensorReady ? "true" : "false");
  json += ",\"autoActive\":" + String(autoScan ? "true" : "false");
  json += ",\"count\":" + String(finger.templateCount);
  json += ",\"baud\":" + String(curBaud);
  json += ",\"security\":" + String(finger.security_level);
  json += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  json += ",\"wifiMode\":\"" + String(wifiConnected ? "STA" : "AP") + "\"";
  json += ",\"staIP\":\"" + staIP + "\"";
  json += ",\"staSSID\":\"" + staSSID + "\"";
  json += ",\"apSSID\":\"" + String(AP_SSID) + "\"";
  json += ",\"ntpSynced\":" + String(timeClient.isTimeSet() ? "true" : "false");
  json += ",\"ntpTime\":\"" + (timeClient.isTimeSet() ? timeClient.getFormattedTime() : "--:--:--") + "\"";
  json += ",\"irEnabled\":" + String(appSettings.irEnabled ? "true" : "false");
  json += ",\"irFinger\":" + String(irRawDetected() ? "true" : "false");
  json += ",\"irGate\":" + String(irGateOpen ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleCount() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  finger.getTemplateCount();
  server.send(200, "application/json", "{\"ok\":true,\"count\":" + String(finger.templateCount) + "}");
}

void handleList() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{";
  for (int i = 0; i < fpCount; i++) {
    if (i > 0) json += ",";
    json += "\"" + String(fpDB[i].id) + "\":{\"name\":\"" + fpDB[i].name + "\"";
    if (fpDB[i].empId[0]) json += ",\"employeeId\":\"" + String(fpDB[i].empId) + "\"";
    json += "}";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleEnroll() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (enrollActive) { server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}"); return; }

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  const char *name = doc["name"] | "";
  const char *empId = doc["employeeId"] | "";
  if (strlen(name) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"name_empty\"}");
    return;
  }

  finger.getTemplateCount();
  uint8_t id = finger.templateCount + 1;
  if (id > MAX_FP) {
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"full\"}");
    return;
  }

  server.send(202, "application/json", "{\"ok\":true,\"id\":" + String(id) + ",\"name\":\"" + String(name) + "\"}");

  autoScan = false;
  enrollFinger(id, name, empId);
}

void handleDelete() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  uint8_t id = doc["id"] | 0;
  if (id == 0) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"id_invalid\"}"); return; }

  int p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) dbRemove(id);

  server.send(200, "application/json",
    "{\"ok\":" + String(p == FINGERPRINT_OK ? "true" : "false") + ",\"id\":" + String(id) + "}");
  finger.getTemplateCount();
  lcdShowIdle();
}

void handleEmpty() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  int p = finger.emptyDatabase();
  if (p == FINGERPRINT_OK) dbClear();
  server.send(200, "application/json",
    "{\"ok\":" + String(p == FINGERPRINT_OK ? "true" : "false") + "}");
  finger.getTemplateCount();
  lcdShowIdle();
}

void handleAutoOn() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  autoScan = true;
  fingerDown = false;
  lcdShowIdle();
  server.send(200, "application/json", "{\"ok\":true,\"autoActive\":true}");
  emit(F("{\"event\":\"autoscan_on\"}"));
}

void handleAutoOff() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  autoScan = false;
  if (ledOn) { finger.LEDcontrol(false); ledOn = false; }
  finger.getTemplateCount();
  lcdShowIdle();
  server.send(200, "application/json", "{\"ok\":true,\"autoActive\":false}");
  emit(F("{\"event\":\"autoscan_off\"}"));
}

// ────────────────────────────────────────────────────────────────────
//  WiFi API Handlers
// ────────────────────────────────────────────────────────────────────
void handleWifiStatus() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{";
  json += "\"mode\":\"" + String(wifiConnected ? "STA" : "AP") + "\"";
  json += ",\"connected\":" + String(wifiConnected ? "true" : "false");
  json += ",\"staIP\":\"" + staIP + "\"";
  json += ",\"staSSID\":\"" + staSSID + "\"";
  json += ",\"apSSID\":\"" + String(AP_SSID) + "\"";
  json += ",\"apIP\":\"192.168.4.1\"";
  json += ",\"savedCount\":" + String(savedWiFiCount);
  json += ",\"saved\":[";
  for (int i = 0; i < savedWiFiCount; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + String(savedWiFi[i].ssid) + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleWifiScan() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");

  // Scan RF blocking — pause autoscan, jangan langsung resume tanpa cooldown
  bool wasAuto = autoScan;
  wifiOpsBegin();

  wifi_mode_t prevMode = WiFi.getMode();
  // ESP32: scan lebih stabil di AP_STA; WIFI_STA-only sering putus + error sensor
  if (prevMode == WIFI_STA) {
    WiFi.mode(WIFI_AP_STA);
    delay(50);
  }

  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  if (n < 0) n = 0;
  if (n > 15) n = 15;  // batasi heap JSON

  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();

  if (prevMode == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
  }
  wifiApplyPowerPolicy();
  wifiOpsEnd(wasAuto);
  server.send(200, "application/json", json);
}

void handleWifiSave() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  const char *ssid = doc["ssid"] | "";
  const char *pass = doc["pass"] | "";
  if (strlen(ssid) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid_empty\"}");
    return;
  }
  if (strlen(ssid) > 32 || strlen(pass) > 64) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"credential_too_long\"}");
    return;
  }
  if (!storageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_unavailable\"}");
    return;
  }

  wifiLoadCreds();
  if (!wifiAddCreds(ssid, pass)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"wifi_save_failed\"}");
    return;
  }

  // Soft connect — TANPA ESP.restart() (reboot bikin kesan module setting rusak)
  bool wasAuto = autoScan;
  wifiOpsBegin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  // Pastikan AP tetap hidup selama connect supaya response ke browser (via AP) sampai
  WiFi.softAP(AP_SSID, cred.apPass);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (millis() - start < 12000) {
    lastScanActivity = millis();
    if (WiFi.status() == WL_CONNECTED) break;
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    staIP = WiFi.localIP().toString();
    staSSID = String(ssid);
    server.send(200, "application/json",
      "{\"ok\":true,\"msg\":\"connected\",\"staIP\":\"" + staIP + "\",\"staSSID\":\"" + staSSID + "\"}");
    delay(300);  // biar TCP kirim response dulu
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiApplyPowerPolicy();
    Serial.printf("[WiFi] Saved+connected %s | %s (no reboot)\n", ssid, staIP.c_str());
  } else {
    wifiConnected = false;
    server.send(200, "application/json",
      "{\"ok\":true,\"msg\":\"saved_connect_failed\",\"mode\":\"AP\"}");
    logError("WiFi save: connect failed for saved network");
    wifiApplyPowerPolicy();
    Serial.printf("[WiFi] Saved %s but connect failed — tetap AP\n", ssid);
  }

  wifiOpsEnd(wasAuto);
}

void handleWifiReset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (!storageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_unavailable\"}");
    return;
  }

  bool wasAuto = autoScan;
  wifiOpsBegin();
  wifiClearCreds();
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, cred.apPass);
  wifiConnected = false;
  staIP = "";
  staSSID = "";
  wifiApplyPowerPolicy();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"reset_ap_mode\"}");
  wifiOpsEnd(wasAuto);
  // Tidak ESP.restart() — cukup balik ke AP
}

void handleWifiDelete() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (!storageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_unavailable\"}");
    return;
  }

  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  const char *ssid = doc["ssid"] | "";
  if (strlen(ssid) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid_empty\"}");
    return;
  }

  wifiLoadCreds();
  wifiRemoveCreds(ssid);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ────────────────────────────────────────────────────────────────────
//  Settings API
// ────────────────────────────────────────────────────────────────────
void handleSettingsGet() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  lastScanActivity = millis();  // buka tab Setelan jangan picu watchdog
  DynamicJsonDocument doc(768);
  doc["apiBaseUrl"] = appSettings.apiBaseUrl;
  doc["kode_cabang"] = appSettings.kodeCabang;
  doc["device_id"] = appSettings.deviceId;
  doc["api_key"] = appSettings.apiKey;
  doc["ir_enabled"] = appSettings.irEnabled;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSettingsSave() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  lastScanActivity = millis();
  if (!storageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_unavailable\"}");
    return;
  }
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  if (doc.containsKey("apiBaseUrl")) strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
  if (doc.containsKey("kode_cabang")) strncpy(appSettings.kodeCabang, doc["kode_cabang"] | "", 15);
  if (doc.containsKey("device_id")) strncpy(appSettings.deviceId, doc["device_id"] | "", 31);
  if (doc.containsKey("api_key")) strncpy(appSettings.apiKey, doc["api_key"] | "", 64);
  if (doc.containsKey("ir_enabled")) appSettings.irEnabled = doc["ir_enabled"] | true;
  if (!settingsSave()) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"settings_save_failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"saved\"}");
}

// ────────────────────────────────────────────────────────────────────
//  API Proxy - Branches & Employees from backend
// ────────────────────────────────────────────────────────────────────
String apiProxyGet(const char *path, int &httpCode) {
  httpCode = -1;
  if (!appSettings.apiBaseUrl[0]) return "";
  if (WiFi.status() != WL_CONNECTED) return "";

  String url = String(appSettings.apiBaseUrl) + path;
  Serial.print("[API] GET "); Serial.println(url);
  Serial.print("[API] Free heap: "); Serial.println(ESP.getFreeHeap());

  bool isHttps = url.startsWith("https://");

  // Static/reused client — hindari use-after-free async TX (lihat
  // catatan detail di postAttendance()).
  static WiFiClientSecure getSecureClient;
  static WiFiClient getPlainClient;
  static bool getSecureInit = false;
  WiFiClient *client;
  if (isHttps) {
    if (!getSecureInit) { getSecureClient.setInsecure(); getSecureInit = true; }
    client = &getSecureClient;
  } else {
    client = &getPlainClient;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(*client, url)) {
    Serial.println("[API] begin() failed");
    logError("API GET begin failed path=%s", path);
    return "";
  }
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  httpCode = http.GET();
  Serial.print("[API] HTTP code: "); Serial.println(httpCode);
  if (httpCode < 200 || httpCode >= 300) logError("API GET status=%d path=%s", httpCode, path);
  String resp = "";
  if (httpCode > 0) {
    resp = http.getString();
    Serial.print("[API] Response len: "); Serial.println(resp.length());
  }
  http.end();
  return resp;
}


String apiProxyPost(const char *path, const String &body, int &httpCode) {
  httpCode = -1;
  if (!appSettings.apiBaseUrl[0]) return "";
  if (WiFi.status() != WL_CONNECTED) return "";

  String url = String(appSettings.apiBaseUrl) + path;
  Serial.print("[API] POST "); Serial.println(url);

  bool isHttps = url.startsWith("https://");
  // Static/reused client — hindari use-after-free async TX (lihat
  // catatan detail di postAttendance()).
  static WiFiClientSecure postSecureClient;
  static WiFiClient postPlainClient;
  static bool postSecureInit = false;
  WiFiClient *client;
  if (isHttps) {
    if (!postSecureInit) { postSecureClient.setInsecure(); postSecureInit = true; }
    client = &postSecureClient;
  } else {
    client = &postPlainClient;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(*client, url)) {
    logError("API POST begin failed path=%s", path);
    return "";
  }
  http.addHeader("Content-Type", "application/json");
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  httpCode = http.POST(body);
  if (httpCode < 200 || httpCode >= 300) logError("API POST status=%d path=%s", httpCode, path);
  String resp = "";
  if (httpCode > 0) resp = http.getString();
  http.end();
  return resp;
}

void handleBranches() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (WiFi.status() != WL_CONNECTED) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }
  int httpCode = 0;
  String resp = apiProxyGet("/api/finger/branches", httpCode);
  if (resp.length() == 0) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\",\"httpCode\":" + String(httpCode) + ",\"heap\":" + String(ESP.getFreeHeap()) + "}");
    return;
  }
  server.send(200, "application/json", resp);
}

void handleEmployees() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (WiFi.status() != WL_CONNECTED) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }
  String kode = server.arg("kode_cabang");
  String path = "/api/finger/employees";
  if (kode.length() > 0) path += "?kode_cabang=" + kode;
  int httpCode = 0;
  String resp = apiProxyGet(path.c_str(), httpCode);
  if (resp.length() == 0) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\",\"httpCode\":" + String(httpCode) + ",\"heap\":" + String(ESP.getFreeHeap()) + "}");
    return;
  }
  server.send(200, "application/json", resp);
}

// ────────────────────────────────────────────────────────────────────
//  Fingerprint Template I/O (raw serial for 256-byte template data)
// ────────────────────────────────────────────────────────────────────
#define FINGERPRINT_DOWNLOAD 0x09

// Fingerprint Command/Response helpers (raw serial, bypasses library)
bool sendFingerCmd(const uint8_t *payload, uint8_t payloadLen, uint8_t *outType, uint8_t *outData, uint16_t *outDataLen) {
  uint8_t hdr[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00};
  uint16_t wLen = payloadLen + 2;
  hdr[7] = wLen >> 8; hdr[8] = wLen & 0xFF;
  uint16_t sum = 0;
  for (int i = 6; i < 9; i++) sum += hdr[i];
  for (int i = 0; i < payloadLen; i++) sum += payload[i];

  flushRX();
  altSerial.write(hdr, 9);
  altSerial.write(payload, payloadLen);
  altSerial.write((uint8_t)(sum >> 8));
  altSerial.write((uint8_t)(sum & 0xFF));
  altSerial.flush();

  unsigned long start = millis();
  uint8_t buf[300];
  uint16_t idx = 0;
  while (millis() - start < 2000 && idx < sizeof(buf)) {
    if (altSerial.available()) {
      uint8_t b = altSerial.read();
      if (idx == 0 && b != 0xEF) continue;
      buf[idx++] = b;
      if (idx >= 9) {
        uint16_t pLen = 9 + ((uint16_t)buf[7] << 8 | buf[8]);
        if (idx >= pLen) {
          if (outType) *outType = buf[6];
          if (outDataLen) *outDataLen = pLen - 9;
          if (outData) memcpy(outData, buf + 9, (pLen - 9 > 256 ? 256 : pLen - 9));
          return true;
        }
      }
    }
    delay(1);
  }
  return false;
}

// Read 256-byte template from sensor for given ID
// Read 256-byte template from sensor for given ID (sent as two 128-byte DATA packets)
bool getTemplateRaw(uint16_t id, uint8_t *buf) {
  if (finger.loadModel(id) != FINGERPRINT_OK) return false;
  delay(30);
  flushRX();

  uint8_t upHdr[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x04};
  uint16_t upSum = 0;
  for (int i=6;i<9;i++) upSum += upHdr[i];
  upSum += 0x08 + 0x01;
  altSerial.write(upHdr,9);
  altSerial.write((uint8_t)0x08); altSerial.write((uint8_t)0x01);
  altSerial.write((uint8_t)(upSum>>8));
  altSerial.write((uint8_t)(upSum&0xFF));
  altSerial.flush();

  unsigned long start = millis();
  uint8_t pkt[500];
  uint16_t idx = 0;
  while (millis()-start < 3000 && idx < sizeof(pkt)) {
    if (altSerial.available()) {
      pkt[idx++] = altSerial.read();
    }
    delay(1);
  }
  if (idx < 9) return false;

  // Scan for DATA packets (type 0x02, wire_len=130 = 128 data + 2 chk)
  uint8_t chunks[2][128];
  uint8_t chunkCount = 0;
  for (uint16_t i = 0; i <= idx - 9 && chunkCount < 2; i++) {
    if (pkt[i]==0xEF && pkt[i+1]==0x01 && pkt[i+6]==0x02) {
      uint16_t wLen = ((uint16_t)pkt[i+7] << 8) | pkt[i+8];
      if (wLen >= 130 && i + 9 + 128 <= idx) {
        memcpy(chunks[chunkCount], pkt + i + 9, 128);
        chunkCount++;
      }
    }
  }

  if (chunkCount == 0) return false;
  memcpy(buf, chunks[0], 128);
  if (chunkCount >= 2) memcpy(buf + 128, chunks[1], 128);
  else memset(buf + 128, 0, 128);
  return true;
}

// Write 256-byte template to sensor (two 128-byte DownChar calls, then storeModel)
bool putTemplateRaw(uint16_t id, const uint8_t *buf) {
  // 1. DownChar buffer 1 with first 128 bytes
  for (uint8_t buffer = 1; buffer <= 2; buffer++) {
    flushRX();
    uint8_t hdr[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x00};
    uint16_t wLen = 2 + 128 + 2; // cmd + param + data(128) + chk(2)
    hdr[7] = wLen >> 8; hdr[8] = wLen & 0xFF;
    uint16_t sum = 0;
    for (int i = 6; i < 9; i++) sum += hdr[i];
    sum += 0x09 + buffer;
    const uint8_t *chunk = buf + (buffer - 1) * 128;
    for (int i = 0; i < 128; i++) sum += chunk[i];

    altSerial.write(hdr, 9);
    altSerial.write((uint8_t)0x09);
    altSerial.write(buffer);
    altSerial.write(chunk, 128);
    altSerial.write((uint8_t)(sum >> 8));
    altSerial.write((uint8_t)(sum & 0xFF));
    altSerial.flush();

    // Read ACK
    uint8_t resp[12];
    uint16_t ri = 0;
    unsigned long start = millis();
    while (ri < 11 && millis() - start < 2000) {
      if (altSerial.available()) {
        uint8_t b = altSerial.read();
        if (ri == 0 && b != 0xEF) continue;
        resp[ri++] = b;
      }
      delay(1);
    }
    if (ri < 11 || resp[9] != 0x00) return false;
    delay(20);
  }

  // 2. Store buffer 1+2 to flash using library
  return finger.storeModel(id) == FINGERPRINT_OK;
}

String toHex(const uint8_t *buf, size_t len) {
  String r;
  for (size_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) r += '0';
    r += String(buf[i], HEX);
  }
  return r;
}

bool fromHex(const char *hex, uint8_t *buf, size_t maxLen) {
  size_t hexLen = strlen(hex);
  if (hexLen % 2 != 0 || hexLen / 2 > maxLen) return false;
  for (size_t i = 0; i < hexLen / 2; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    buf[i] = ((hi >= 'a' ? hi - 'a' + 10 : hi >= 'A' ? hi - 'A' + 10 : hi - '0') << 4) |
              (lo >= 'a' ? lo - 'a' + 10 : lo >= 'A' ? lo - 'A' + 10 : lo - '0');
  }
  return true;
}

// ────────────────────────────────────────────────────────────────────
//  Backup (metadata only)
// ────────────────────────────────────────────────────────────────────
String jsonEscape(const char *s) {
  String r;
  for (size_t i = 0; s[i]; i++) {
    char c = s[i];
    if (c == '"') r += "\\\"";
    else if (c == '\\') r += "\\\\";
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if (c == '\t') r += "\\t";
    else if (c >= 0x20) r += c;
  }
  return r;
}

void handleBackup() {
  if (!requireAuth()) return;
  if (enrollActive || restoreActive) { server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}"); return; }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Content-Disposition", "attachment; filename=fpm10a-backup.json");

  finger.getTemplateCount();

  String json = "{\"version\":1";
  json += ",\"deviceInfo\":{";
  json += "\"templateCount\":" + String(finger.templateCount);
  json += ",\"securityLevel\":" + String(finger.security_level);
  json += ",\"baud\":" + String(curBaud);
  json += "}";
  json += ",\"fingerprints\":{";
  for (int i = 0; i < fpCount; i++) {
    if (i > 0) json += ",";
    json += "\"" + String(fpDB[i].id) + "\":{";
    json += "\"name\":\"" + jsonEscape(fpDB[i].name) + "\"";
    if (fpDB[i].empId[0]) json += ",\"employeeId\":\"" + jsonEscape(fpDB[i].empId) + "\"";
    json += "}";
  }
  json += "}}";
  server.send(200, "application/json", json);
}

void handleRestore() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (enrollActive || restoreActive) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }

  // Support both new format (with "fingerprints" key) and flat format
  JsonObject fps;
  if (doc.containsKey("fingerprints")) {
    fps = doc["fingerprints"].as<JsonObject>();
  } else {
    fps = doc.as<JsonObject>();
  }

  restoreActive = true;
  bool oldAuto = autoScan;
  autoScan = false;

  int restored = 0, missing = 0;
  String detail = "[";
  bool first = true;

  for (JsonPair p : fps) {
    uint8_t id = atoi(p.key().c_str());
    if (id == 0) continue;

    JsonObject info = p.value().as<JsonObject>();
    const char *name = info["name"] | "";
    const char *empId = info["employeeId"] | "";

    // Check if template exists on sensor (NEVER delete or overwrite sensor data)
    flushRX();
    delay(30);
    bool templateExists = (finger.loadModel(id) == FINGERPRINT_OK);

    // Always restore metadata to LittleFS
    dbAdd(id, name, empId);

    if (!first) detail += ",";
    first = false;
    detail += "{\"id\":" + String(id);
    detail += ",\"template\":" + String(templateExists ? "true" : "false");
    detail += ",\"name\":\"" + jsonEscape(name) + "\"";
    if (empId[0]) detail += ",\"employeeId\":\"" + jsonEscape(empId) + "\"";
    detail += "}";

    if (templateExists) restored++;
    else missing++;

    emit(F("{\"event\":\"restore_progress\",\"id\":%d,\"template\":%s}"), id, templateExists ? "true" : "false");

    delay(10);
    yield();
  }

  detail += "]";

  finger.getTemplateCount();

  if (oldAuto) { autoScan = true; lcdShowIdle(); }
  restoreActive = false;

  String resp = "{\"ok\":true,\"restored\":" + String(restored);
  resp += ",\"missing\":" + String(missing);
  resp += ",\"detail\":" + detail + "}";
  server.send(200, "application/json", resp);
  emit(F("{\"event\":\"restore_complete\",\"restored\":%d,\"missing\":%d}"), restored, missing);
}

// ────────────────────────────────────────────────────────────────────
//  Full Backup (metadata + template biometric as hex)
// ────────────────────────────────────────────────────────────────────
void handleBackupFull() {
  if (!requireAuth()) return;
  if (enrollActive || restoreActive) { server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}"); return; }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Content-Disposition", "attachment; filename=fpm10a-backup-full.json");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "{\"version\":1");

  finger.getTemplateCount();
  String s;
  s = ",\"deviceInfo\":{";
  s += "\"templateCount\":" + String(finger.templateCount);
  s += ",\"securityLevel\":" + String(finger.security_level);
  s += ",\"baud\":" + String(curBaud);
  s += "}";
  server.sendContent(s);

  s = ",\"fingerprints\":{";
  for (int i = 0; i < fpCount; i++) {
    if (i > 0) s += ",";
    s += "\"" + String(fpDB[i].id) + "\":{";
    s += "\"name\":\"" + jsonEscape(fpDB[i].name) + "\"";
    if (fpDB[i].empId[0]) s += ",\"employeeId\":\"" + jsonEscape(fpDB[i].empId) + "\"";
    s += "}";
  }
  s += "}";
  server.sendContent(s);

  s = ",\"templates\":{";
  server.sendContent(s);
  for (int i = 0; i < fpCount; i++) {
    uint8_t tpl[256];
    bool ok = getTemplateRaw(fpDB[i].id, tpl);
    s = "\"" + String(fpDB[i].id) + "\":\"";
    if (ok) s += toHex(tpl, 256);
    s += "\"";
    if (i < fpCount - 1) s += ",";
    server.sendContent(s);
    delay(5);
    yield();
  }
  server.sendContent("}}");
}

// ────────────────────────────────────────────────────────────────────
//  Restore Single Template
// ────────────────────────────────────────────────────────────────────
void handleRestoreTemplate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (enrollActive || restoreActive) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }

  uint16_t id = doc["id"] | 0;
  const char *hex = doc["data"] | "";

  if (id == 0 || strlen(hex) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"id_or_data_empty\"}");
    return;
  }

  restoreActive = true;
  bool oldAuto = autoScan;
  autoScan = false;

  uint8_t tpl[256];
  bool hexOk = fromHex(hex, tpl, 256);
  if (!hexOk) {
    if (oldAuto) { autoScan = true; }
    restoreActive = false;
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_hex\"}");
    return;
  }

  // Check if template already exists on sensor
  flushRX();
  delay(30);
  bool exists = (finger.loadModel(id) == FINGERPRINT_OK);

  if (exists) {
    // Skip: template already on sensor
    if (oldAuto) { autoScan = true; }
    restoreActive = false;
    server.send(200, "application/json", "{\"ok\":true,\"id\":" + String(id) + ",\"status\":\"skipped\"}");
    return;
  }

  // Write template to sensor
  bool ok = putTemplateRaw(id, tpl);
  if (ok) {
    finger.getTemplateCount();
    emit(F("{\"event\":\"template_restored\",\"id\":%d}"), id);
  }

  if (oldAuto) { autoScan = true; }
  restoreActive = false;

  String resp = "{\"ok\":" + String(ok ? "true" : "false");
  resp += ",\"id\":" + String(id);
  resp += ",\"status\":\"" + String(ok ? "restored" : "failed") + "\"}";
  server.send(ok ? 200 : 500, "application/json", resp);
}


// ────────────────────────────────────────────────────────────────────
uint8_t nextFreeFingerId() {
  for (uint8_t id = 1; id <= (uint8_t)MAX_FP; id++) {
    bool used = false;
    for (int i = 0; i < fpCount; i++) {
      if (fpDB[i].id == id) { used = true; break; }
    }
    if (!used) return id;
  }
  return 0;
}

int findDbByEmpId(const char *empId) {
  if (!empId || !empId[0]) return -1;
  for (int i = 0; i < fpCount; i++) {
    if (strcmp(fpDB[i].empId, empId) == 0) return i;
  }
  return -1;
}

// Sinkron template dari server PJTKI → sensor FPM10A (hanya employeeIds yang dipilih)
void handleSyncFromServer() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (enrollActive || restoreActive) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  if (!appSettings.apiBaseUrl[0]) {
    server.send(422, "application/json", "{\"ok\":false,\"error\":\"apiBaseUrl_empty\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }

  DynamicJsonDocument reqDoc(4096);
  if (deserializeJson(reqDoc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  JsonArray want = reqDoc["employeeIds"].as<JsonArray>();
  if (want.isNull() || want.size() == 0) {
    server.send(422, "application/json", "{\"ok\":false,\"error\":\"employeeIds_required\"}");
    return;
  }
  if (want.size() > 30) {
    server.send(422, "application/json", "{\"ok\":false,\"error\":\"max_30_ids\"}");
    return;
  }

  restoreActive = true;
  bool oldAuto = autoScan;
  autoScan = false;

  int restored = 0, skipped = 0, failed = 0, noHex = 0;

  for (JsonVariant v : want) {
    const char *empId = v.as<const char*>();
    if (!empId || !empId[0]) continue;

    if (findDbByEmpId(empId) >= 0) {
      skipped++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"skipped_local\"}"), empId);
      continue;
    }

    String onePath = String("/api/finger/arduino/template/") + empId;
    int code2 = 0;
    String oneResp = apiProxyGet(onePath.c_str(), code2);
    if (oneResp.length() == 0 || code2 < 200 || code2 >= 300) {
      if (code2 == 422) noHex++;
      else failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"fetch_fail\"}"), empId);
      continue;
    }

    DynamicJsonDocument oneDoc(3072);
    if (deserializeJson(oneDoc, oneResp) || !oneDoc["success"]) {
      if (code2 == 422) noHex++;
      else failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_hex\"}"), empId);
      continue;
    }

    const char *hex = oneDoc["data"]["template_hex"] | "";
    const char *nm = oneDoc["data"]["nama"] | "";
    int preferId = oneDoc["data"]["finger_id"] | 0;

    uint8_t tpl[256];
    if (!hex[0] || !fromHex(hex, tpl, 256)) {
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"bad_hex\"}"), empId);
      continue;
    }

    uint8_t id = 0;
    if (preferId > 0 && preferId <= MAX_FP) {
      bool used = false;
      for (int i = 0; i < fpCount; i++) if (fpDB[i].id == (uint8_t)preferId) { used = true; break; }
      if (!used) id = (uint8_t)preferId;
    }
    if (!id) id = nextFreeFingerId();
    if (!id) {
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_slot\"}"), empId);
      break;
    }

    flushRX();
    delay(20);
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      uint8_t found = 0;
      for (uint8_t cand = 1; cand <= (uint8_t)MAX_FP; cand++) {
        bool usedDb = false;
        for (int i = 0; i < fpCount; i++) {
          if (fpDB[i].id == cand) { usedDb = true; break; }
        }
        if (usedDb) continue;
        flushRX();
        delay(10);
        if (finger.loadModel(cand) != FINGERPRINT_OK) { found = cand; break; }
      }
      id = found;
      if (!id) { failed++; emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_slot\"}"), empId); break; }
    }

    bool ok = putTemplateRaw(id, tpl);
    if (ok) {
      dbAdd(id, nm && nm[0] ? nm : empId, empId);
      restored++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"id\":%d,\"status\":\"restored\"}"), empId, id);
    } else {
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"write_fail\"}"), empId);
    }
    delay(50);
    yield();
  }

  finger.getTemplateCount();
  if (oldAuto) { autoScan = true; lcdShowIdle(); }
  restoreActive = false;

  String resp = "{\"ok\":true";
  resp += ",\"restored\":" + String(restored);
  resp += ",\"skipped\":" + String(skipped);
  resp += ",\"failed\":" + String(failed);
  resp += ",\"noHex\":" + String(noHex);
  resp += "}";
  emit(F("{\"event\":\"sync_complete\",\"restored\":%d,\"skipped\":%d,\"failed\":%d}"), restored, skipped, failed);
  server.send(200, "application/json", resp);
}

// Proxy daftar template server (untuk UI pilih anak, lintas cabang)
static String urlEncodeParam(const String &s) {
  String out;
  const char *hx = "0123456789ABCDEF";
  for (unsigned i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hx[(c >> 4) & 0xF];
      out += hx[c & 0xF];
    }
  }
  return out;
}

void handleServerTemplates() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (WiFi.status() != WL_CONNECTED) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }
  String path = "/api/finger/arduino/templates";
  String qs = "";
  if (server.hasArg("kode_cabang")) {
    qs += (qs.length() ? "&" : "?");
    qs += "kode_cabang=";
    qs += urlEncodeParam(server.arg("kode_cabang"));
  }
  if (server.hasArg("q")) {
    qs += (qs.length() ? "&" : "?");
    qs += "q=";
    qs += urlEncodeParam(server.arg("q"));
  }
  path += qs;
  int httpCode = 0;
  String resp = apiProxyGet(path.c_str(), httpCode);
  if (resp.length() == 0) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\",\"httpCode\":" + String(httpCode) + "}");
    return;
  }
  server.send(200, "application/json", resp);
}

void handleStorage() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{\"ok\":true,\"storageReady\":" + String(storageReady ? "true" : "false");
  json += ",\"total\":" + String(LittleFS.totalBytes());
  json += ",\"used\":" + String(LittleFS.usedBytes());
  json += ",\"free\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes());
  json += ",\"sketchSize\":" + String(ESP.getSketchSize());
  json += ",\"freeSketchSpace\":" + String(ESP.getFreeSketchSpace());
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += "}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  server.send(404, "application/json", "{\"error\":\"not_found\"}");
}

// ────────────────────────────────────────────────────────────────────
//  Credentials API
// ────────────────────────────────────────────────────────────────────
void handleCredGet() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{\"webUser\":\"" + String(cred.webUser) + "\"";
  json += ",\"apSSID\":\"" + String(AP_SSID) + "\"";
  json += ",\"ntpServer\":\"" + String(cred.ntpServer) + "\"";
  json += ",\"utcOffset\":" + String(cred.utcOffset);
  json += ",\"ntpSynced\":" + String(timeClient.isTimeSet() ? "true" : "false");
  json += ",\"ntpTime\":\"" + (timeClient.isTimeSet() ? timeClient.getFormattedTime() : "--:--:--") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleCredSave() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  if (!storageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_unavailable\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }

  if (doc.containsKey("webUser")) strncpy(cred.webUser, doc["webUser"] | "", 31);
  if (doc.containsKey("webPass") && strlen(doc["webPass"] | "") > 0) strncpy(cred.webPass, doc["webPass"] | "", 63);
  if (doc.containsKey("apPass") && strlen(doc["apPass"] | "") > 0) strncpy(cred.apPass, doc["apPass"] | "", 64);
  if (doc.containsKey("ntpServer")) strncpy(cred.ntpServer, doc["ntpServer"] | "", 63);
  if (doc.containsKey("utcOffset")) cred.utcOffset = doc["utcOffset"] | 25200;
  if (!credSave()) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"credentials_save_failed\"}");
    return;
  }

  // Apply NTP changes immediately
  timeClient.setPoolServerName(cred.ntpServer);
  timeClient.setTimeOffset(cred.utcOffset);
  timeClient.forceUpdate();

  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"saved_rebooting\"}");
  delay(500);
  ESP.restart();
}

// ────────────────────────────────────────────────────────────────────
//  EMBEDDED HTML WEB UI (in webpage.h to avoid Arduino preprocessor
//  forward-declaration issues with JS function keywords)
// ────────────────────────────────────────────────────────────────────
#include "webpage.h"
/* -- Moved to webpage.h --
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FPM10A Console</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0e14;--card:#141b24;--border:#1e2a38;--cyan:#00e5ff;--green:#00e676;--red:#ff1744;--yellow:#ffd600;--dim:#6b7280;--text:#e2e8f0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
.topbar{background:#0d1520;border-bottom:1px solid var(--border);padding:12px 16px;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;z-index:100}
.topbar h1{font-size:18px;color:var(--cyan)}
.pill{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:12px;font-size:11px;background:var(--card);border:1px solid var(--border)}
.dot{width:7px;height:7px;border-radius:50%}
.dot-g{background:var(--green)}.dot-r{background:var(--red)}.dot-y{background:var(--yellow)}
.tabs{display:flex;background:var(--card);border-bottom:1px solid var(--border);overflow-x:auto}
.tab{flex:1;padding:12px 8px;text-align:center;cursor:pointer;font-size:13px;color:var(--dim);border-bottom:2px solid transparent;transition:.2s;white-space:nowrap}
.tab:hover{color:var(--text)}.tab.on{color:var(--cyan);border-color:var(--cyan)}
.page{display:none;padding:16px;max-width:600px;margin:0 auto}.page.on{display:block}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:12px}
.card h3{font-size:13px;color:var(--dim);margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}
.stat{font-size:28px;font-weight:700}
.stat-c{color:var(--cyan)}.stat-g{color:var(--green)}.stat-r{color:var(--red)}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:10px 20px;border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;transition:.15s;width:100%;margin-top:8px}
.btn-c{background:var(--cyan);color:#000}.btn-c:hover{filter:brightness(1.15)}
.btn-g{background:var(--green);color:#000}.btn-g:hover{filter:brightness(1.15)}
.btn-r{background:var(--red);color:#fff}.btn-r:hover{filter:brightness(1.15)}
.btn-o{background:transparent;border:1px solid var(--border);color:var(--text)}.btn-o:hover{background:var(--border)}
.btn:disabled{opacity:.4;cursor:not-allowed}
input,select{width:100%;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:14px;margin-top:6px;outline:none}
input:focus,select:focus{border-color:var(--cyan)}
label{font-size:13px;color:var(--dim);margin-top:10px;display:block}
.scan-box{text-align:center;padding:30px 16px;border-radius:16px;border:2px solid var(--border);transition:.3s}
.scan-box.active{border-color:var(--cyan);box-shadow:0 0 30px rgba(0,229,255,.15)}
.scan-box.ok{border-color:var(--green);box-shadow:0 0 30px rgba(0,230,118,.2)}
.scan-box.fail{border-color:var(--red);box-shadow:0 0 30px rgba(255,23,68,.2)}
.scan-icon{font-size:48px;margin-bottom:12px;animation:pulse 1.5s infinite}
@keyframes pulse{0%,100%{opacity:.6}50%{opacity:1}}
.scan-badge{display:inline-block;padding:4px 14px;border-radius:16px;font-size:12px;font-weight:700;margin:8px 0}
.badge-scan{background:rgba(0,229,255,.15);color:var(--cyan)}
.badge-ok{background:rgba(0,230,118,.15);color:var(--green)}
.badge-fail{background:rgba(255,23,68,.15);color:var(--red)}
.badge-idle{background:rgba(107,114,128,.15);color:var(--dim)}
.scan-name{font-size:20px;font-weight:700;margin:4px 0}
.log{max-height:200px;overflow-y:auto;font-family:'SF Mono',monospace;font-size:11px;background:var(--bg);border-radius:8px;padding:8px;margin-top:8px}
.log div{padding:2px 0;border-bottom:1px solid var(--border)}
.log .t{color:var(--dim)}.log .ok{color:var(--green)}.log .er{color:var(--red)}.log .cy{color:var(--cyan)}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;padding:8px;color:var(--dim);border-bottom:1px solid var(--border);font-size:11px;text-transform:uppercase}
td{padding:8px;border-bottom:1px solid var(--border)}
.del-btn{background:none;border:none;color:var(--red);cursor:pointer;font-size:16px;padding:4px 8px}
.empty-state{text-align:center;padding:40px;color:var(--dim)}
.wifi-item{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;margin-bottom:6px;cursor:pointer;transition:.15s}
.wifi-item:hover{border-color:var(--cyan)}
.wifi-item.selected{border-color:var(--cyan);background:rgba(0,229,255,.05)}
.wifi-ssid{font-weight:600;font-size:14px}
.wifi-signal{font-size:12px;color:var(--dim)}
.wifi-lock{color:var(--yellow);font-size:12px}
</style>
</head>
<body>
<div class="topbar">
  <h1>FPM10A</h1>
  <div style="display:flex;gap:6px">
    <span class="pill"><span class="dot" id="sdot"></span><span id="stxt">OFFLINE</span></span>
    <span class="pill" id="tpill">0 templates</span>
  </div>
</div>
<div class="tabs">
  <div class="tab on" onclick="go('dash')">Dashboard</div>
  <div class="tab" onclick="go('enroll')">Daftar</div>
  <div class="tab" onclick="go('scan')">Scan</div>
  <div class="tab" onclick="go('data')">Data</div>
  <div class="tab" onclick="go('wifi')">WiFi</div>
  <div class="tab" onclick="go('setel')">Setelan</div>
</div>

<div class="page on" id="p-dash">
  <div class="card"><h3>Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat stat-c" id="dcnt">-</div><div style="font-size:12px;color:var(--dim)">Templates</div></div>
      <div style="flex:1"><div class="stat stat-g" id="dscan">IDLE</div><div style="font-size:12px;color:var(--dim)">Scan Mode</div></div>
    </div>
  </div>
  <div class="card"><h3>WiFi Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat" id="dwmode" style="font-size:18px">AP</div><div style="font-size:12px;color:var(--dim)">Mode</div></div>
      <div style="flex:1"><div style="font-size:14px;color:var(--dim)" id="dwip">192.168.4.1</div><div style="font-size:12px;color:var(--dim)">IP</div></div>
    </div>
  </div>
  <div class="card"><h3>Quick Actions</h3>
    <button class="btn btn-c" onclick="go('enroll')">Daftar Sidik Jari Baru</button>
    <button class="btn btn-g" onclick="go('scan')">Mulai Scan</button>
  </div>
  <div class="card"><h3>Activity Log</h3><div class="log" id="elog"></div></div>
</div>

<div class="page" id="p-enroll">
  <div class="card"><h3>Daftar Sidik Jari</h3>
    <label>Nama</label><input id="ename" placeholder="Nama karyawan">
    <label>ID Karyawan (opsional)</label><input id="eemp" placeholder="EMP001">
    <button class="btn btn-c" id="enrollBtn" onclick="startEnroll()">Mulai Daftar</button>
  </div>
  <div class="card"><h3>Progress</h3>
    <div id="eprog" style="text-align:center;padding:20px;color:var(--dim)">Menunggu...</div>
  </div>
  <div class="card"><h3>Log</h3><div class="log" id="elog2"></div></div>
</div>

<div class="page" id="p-scan">
  <div class="card">
    <div class="scan-box" id="sbox">
      <div class="scan-icon" id="sicon">&#x1f463;</div>
      <div class="scan-badge badge-idle" id="sbadge">IDLE</div>
      <div class="scan-name" id="sname">Tekan tombol untuk mulai</div>
      <div style="font-size:12px;color:var(--dim)" id="sconf"></div>
    </div>
    <button class="btn btn-g" id="scanBtn" onclick="toggleScan()">Mulai Scan</button>
  </div>
  <div class="card"><h3>Scan Log</h3><div class="log" id="slog"></div></div>
</div>

<div class="page" id="p-data">
  <div class="card"><h3>Data Terdaftar (<span id="dcnt2">0</span>)</h3>
    <input id="dsearch" placeholder="Cari..." oninput="filterData()">
    <div style="overflow-x:auto;margin-top:8px">
      <table><thead><tr><th>ID</th><th>Nama</th><th>Karyawan</th><th></th></tr></thead>
      <tbody id="dtbody"></tbody></table>
    </div>
    <div class="empty-state" id="dempty">Belum ada data</div>
    <button class="btn btn-r" onclick="emptyAll()">Hapus Semua</button>
  </div>
</div>

<div class="page" id="p-wifi">
  <div class="card"><h3>WiFi Status</h3>
    <div style="display:flex;gap:12px">
      <div style="flex:1"><div class="stat" id="wmode" style="font-size:18px">AP</div><div style="font-size:12px;color:var(--dim)">Mode</div></div>
      <div style="flex:1"><div style="font-size:14px;color:var(--dim)" id="wsta">-</div><div style="font-size:12px;color:var(--dim)">Connected</div></div>
    </div>
  </div>
  <div class="card"><h3>Scan Network</h3>
    <button class="btn btn-o" onclick="scanWifi()">Scan</button>
    <div id="wlist" style="margin-top:8px"></div>
  </div>
  <div class="card"><h3>Connect to WiFi</h3>
    <label>SSID</label><input id="wssid" placeholder="Network name" readonly>
    <label>Password</label><input id="wpass" type="password" placeholder="Password">
    <button class="btn btn-c" onclick="saveWifi()">Simpan & Reboot</button>
    <button class="btn btn-r" id="wresetBtn" onclick="resetWifi()" style="display:none">Hapus WiFi & Reboot</button>
  </div>
</div>

<div class="page" id="p-setel">
  <div class="card"><h3>Pengaturan API</h3>
    <label>API Server URL</label><input id="sapi" placeholder="http://192.168.1.15:3004">
    <label>Kode Cabang</label><input id="scab" placeholder="CKS">
    <label>Device ID</label><input id="sdev" placeholder="arduino-001">
    <button class="btn btn-c" onclick="saveSettings()">Simpan Setelan</button>
  </div>
  <div class="card"><h3>Status</h3>
    <div style="font-size:13px;color:var(--dim)">
      <div>WiFi Mode: <span id="sfg-wmode" style="color:var(--text)">-</span></div>
      <div>IP: <span id="sfg-ip" style="color:var(--text)">-</span></div>
      <div>Fingerprint: <span id="sfg-fp" style="color:var(--text)">-</span></div>
    </div>
  </div>
</div>

<script>
var autoOn=false,scanRunning=false;
function go(s){document.querySelectorAll('.page').forEach(p=>p.classList.remove('on'));
document.getElementById('p-'+s).classList.add('on');
document.querySelectorAll('.tab').forEach((t,i)=>{t.classList.toggle('on',['dash','enroll','scan','data','wifi','setel'][i]===s)});
if(s==='data')loadData();if(s==='wifi')loadWifiStatus();if(s==='setel')loadSettings()}
function addLog(el,cls,txt){var d=document.getElementById(el);var m=document.createElement('div');
m.innerHTML='<span class="t">'+new Date().toLocaleTimeString()+'</span> <span class="'+cls+'">'+txt+'</span>';
d.prepend(m);if(d.children.length>50)d.lastChild.remove()}
function api(path,method,body){
return fetch(path,{method:method||'GET',headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined}).then(r=>r.json())}

function updStatus(){
api('/api/status').then(d=>{
document.getElementById('sdot').className='dot '+(d.ready?'dot-g':'dot-r');
document.getElementById('stxt').textContent=d.ready?(d.autoActive?'SCANNING':'SIAP'):'OFFLINE';
document.getElementById('tpill').textContent=d.count+' templates';
document.getElementById('dcnt').textContent=d.count;
document.getElementById('dscan').textContent=d.autoActive?'ACTIVE':'IDLE';
document.getElementById('dscan').className='stat '+(d.autoActive?'stat-g':'stat-r');
document.getElementById('dwmode').textContent=d.wifiMode;
document.getElementById('dwmode').style.color=d.wifiMode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('dwip').textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-wmode');if(e)e.textContent=d.wifiMode;
var e=document.getElementById('sfg-ip');if(e)e.textContent=d.wifiMode==='STA'?d.staIP:'192.168.4.1';
var e=document.getElementById('sfg-fp');if(e)e.textContent=d.count+' templates | baud:'+d.baud;
autoOn=d.autoOn;if(d.autoActive)go('scan');
}).catch(()=>{})}

function startEnroll(){
var name=document.getElementById('ename').value.trim();
var emp=document.getElementById('eemp').value.trim();
if(!name){alert('Nama wajib diisi');return}
document.getElementById('enrollBtn').disabled=true;
document.getElementById('eprog').innerHTML='<span style="color:var(--cyan)">Memulai...</span>';
addLog('elog2','cy','Mulai daftar: '+name);
api('/api/enroll','POST',{name:name,employeeId:emp}).then(d=>{
if(!d.ok){document.getElementById('enrollBtn').disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">'+d.error+'</span>';
addLog('elog2','er','Error: '+d.error)}
}).catch(e=>{document.getElementById('enrollBtn').disabled=false;
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal</span>';
addLog('elog2','er','Network error')})}

function toggleScan(){
if(!scanRunning){api('/api/autoscan/on','POST').then(d=>{if(d.ok){scanRunning=true;
document.getElementById('scanBtn').textContent='Stop Scan';
document.getElementById('scanBtn').className='btn btn-r';
setScanState('active','MENUNGGU','Menempelkan jari...')}})}
else{api('/api/autoscan/off','POST').then(()=>{scanRunning=false;
document.getElementById('scanBtn').textContent='Mulai Scan';
document.getElementById('scanBtn').className='btn btn-g';
setScanState('','IDLE','Tekan tombol untuk mulai')})}}

function setScanState(cls,badge,name){
var b=document.getElementById('sbox');b.className='scan-box '+(cls||'');
document.getElementById('sbadge').className='scan-badge badge-'+(cls==='ok'?'ok':cls==='fail'?'fail':cls==='active'?'scan':'idle');
document.getElementById('sbadge').textContent=badge;
document.getElementById('sname').textContent=name;
document.getElementById('sconf').textContent=''}

function loadData(){
api('/api/list').then(d=>{
var keys=Object.keys(d);document.getElementById('dcnt2').textContent=keys.length;
var tb=document.getElementById('dtbody');tb.innerHTML='';
document.getElementById('dempty').style.display=keys.length?'none':'block';
keys.forEach(k=>{var e=d[k];var tr=document.createElement('tr');
tr.innerHTML='<td>'+k+'</td><td>'+e.name+'</td><td>'+(e.employeeId||'-')+'</td><td><button class="del-btn" onclick="delFP('+k+')">&times;</button></td>';
tb.appendChild(tr)})})
document.getElementById('dsearch').value='';filterData()}

function filterData(){
var q=document.getElementById('dsearch').value.toLowerCase();
document.querySelectorAll('#dtbody tr').forEach(r=>{r.style.display=r.textContent.toLowerCase().includes(q)?'':'none'})}

function delFP(id){if(!confirm('Hapus ID '+id+'?'))return;
api('/api/delete','POST',{id:id}).then(d=>{if(d.ok)loadData()})}

function emptyAll(){if(!confirm('Hapus SEMUA data?'))return;
api('/api/empty','POST').then(d=>{if(d.ok)loadData()})}

// WiFi functions
function loadWifiStatus(){
api('/api/wifi').then(d=>{
document.getElementById('wmode').textContent=d.mode;
document.getElementById('wmode').style.color=d.mode==='STA'?'var(--green)':'var(--yellow)';
document.getElementById('wsta').textContent=d.connected?d.staSSID+' ('+d.staIP+')':'Tidak terhubung';
document.getElementById('wresetBtn').style.display=d.hasSaved?'block':'none';
}).catch(()=>{})}

function scanWifi(){
document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--dim)">Scanning...</div>';
api('/api/wifi/scan').then(networks=>{
var html='';
networks.forEach(n=>{
var signal=n.rssi>-50?'Excellent':n.rssi>-70?'Good':'Weak';
html+='<div class="wifi-item" onclick="selectWifi(\''+n.ssid.replace(/'/g,"\\'")+'\')">';
html+='<div><div class="wifi-ssid">'+n.ssid+'</div>';
html+='<div class="wifi-signal">'+signal+' ('+n.rssi+' dBm) '+(n.enc?'Secured':'Open')+'</div></div>';
html+='<div>'+(n.enc?'<span class="wifi-lock">&#x1f512;</span>':'')+'</div>';
html+='</div>';
});
document.getElementById('wlist').innerHTML=html||'<div style="text-align:center;padding:12px;color:var(--dim)">Tidak ada jaringan</div>';
}).catch(()=>{document.getElementById('wlist').innerHTML='<div style="text-align:center;padding:12px;color:var(--red)">Gagal scan</div>'})}

function selectWifi(ssid){
document.getElementById('wssid').value=ssid;
document.querySelectorAll('.wifi-item').forEach(el=>el.classList.remove('selected'));
event.currentTarget.classList.add('selected');
}

function saveWifi(){
var ssid=document.getElementById('wssid').value.trim();
var pass=document.getElementById('wpass').value;
if(!ssid){alert('Pilih jaringan WiFi');return}
if(!confirm('Simpan WiFi "'+ssid+'" dan reboot?'))return;
api('/api/wifi','POST',{ssid:ssid,pass:pass}).then(d=>{
if(d.ok){alert('Tersimpan! Device akan reboot...');}
}).catch(()=>alert('Gagal menyimpan'))}

function resetWifi(){
if(!confirm('Hapus WiFi credentials dan reboot ke AP mode?'))return;
api('/api/wifi/reset','POST').then(d=>{
if(d.ok){alert('Dihapus! Device akan reboot ke AP mode...');}
}).catch(()=>alert('Gagal'))}

function loadSettings(){
api('/api/settings').then(d=>{
document.getElementById('sapi').value=d.apiBaseUrl||'';
document.getElementById('scab').value=d.kode_cabang||'';
document.getElementById('sdev').value=d.device_id||'';
}).catch(()=>{})
updStatus()}

function saveSettings(){
var u=document.getElementById('sapi').value.trim();
var c=document.getElementById('scab').value.trim();
var dv=document.getElementById('sdev').value.trim();
if(!u){alert('API URL wajib diisi');return}
api('/api/settings','POST',{apiBaseUrl:u,kode_cabang:c,device_id:dv}).then(d=>{
if(d.ok){alert('Setelan tersimpan!');}
}).catch(()=>alert('Gagal menyimpan'))}

var es=new EventSource('/api/events');
es.onmessage=function(e){
try{var o=JSON.parse(e.data);handleEvent(o)}catch(x){}};

function handleEvent(o){
var t=o.event||o.type;
if(t==='enroll_start'){
document.getElementById('enrollBtn').disabled=true;
document.getElementById('eprog').innerHTML='<span style="color:var(--cyan)">ID: '+o.id+' | Letakkan jari...</span>';
addLog('elog2','cy','Enroll ID:'+o.id+' dimulai')}
else if(t==='waiting_finger')
document.getElementById('eprog').innerHTML='<span style="color:var(--yellow)">Letakkan jari di sensor...</span>';
else if(t==='image_ok_step1')
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Scan 1 OK</span>';
else if(t==='remove')
document.getElementById('eprog').innerHTML='Angkat jari...';
else if(t==='waiting_finger_2')
document.getElementById('eprog').innerHTML='<span style="color:var(--yellow)">Letakkan jari SAMA lagi...</span>';
else if(t==='image_ok_step2')
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Scan 2 OK | Membuat model...</span>';
else if(t==='enrolled'){
document.getElementById('eprog').innerHTML='<span style="color:var(--green)">Berhasil! ID: '+o.id+'</span>';
document.getElementById('enrollBtn').disabled=false;
addLog('elog2','ok','Enrolled: '+o.name+' (ID:'+o.id+')');
document.getElementById('ename').value='';document.getElementById('eemp').value='';
setTimeout(function(){document.getElementById('eprog').innerHTML='Menunggu...';},3000);
updStatus();loadData()}
else if(t==='enroll_fail'||t==='already_registered'){
document.getElementById('eprog').innerHTML='<span style="color:var(--red)">Gagal: '+(o.id?'sudah ada ID:'+o.id:'')+'</span>';
document.getElementById('enrollBtn').disabled=false;
addLog('elog2','er','Enroll gagal')}
else if(t==='bad_image')
addLog('elog2','er','Gambar jelek step '+(o.step||'?'));
else if(t==='retry_create')
addLog('elog2','er','Create gagal, percobaan '+(o.attempt||'?'));
else if(t==='match'){
setScanState('ok','TERDETEKSI',o.name||'ID: '+o.id);
document.getElementById('sconf').textContent='Confidence: '+Math.round(o.confidence*100/256)+'%';
addLog('slog','ok','MATCH: '+(o.name||'?')+' ID:'+o.id)}
else if(t==='nomatch'){
setScanState('fail','TIDAK DIKENALI','Sidik jari tidak terdaftar');
addLog('slog','er','No match (code:'+o.code+')')}
else if(t==='autoscan_on'){
scanRunning=true;setScanState('active','MENUNGGU','Menempelkan jari...');
document.getElementById('scanBtn').textContent='Stop Scan';
document.getElementById('scanBtn').className='btn btn-r'}
else if(t==='autoscan_off'){
scanRunning=false;setScanState('','IDLE','Tekan tombol untuk mulai');
document.getElementById('scanBtn').textContent='Mulai Scan';
document.getElementById('scanBtn').className='btn btn-g'}
else if(t==='autoscan_err')
addLog('slog','er','Scan error: '+(o.step||'')+' code:'+(o.code||''));
else if(t==='attendance'){
var st=o.response||{};
var msg=st.status||'unknown';
var cols={checkin:'ok',checkout:'cy',not_found:'er',ignored:'t',error:'er'};
var labels={checkin:'ABSEN MASUK',checkout:'ABSEN PULANG',not_found:'TIDAK DIKENALI',ignored:'SUDAH ABSEN',error:'ERROR'};
setScanState(msg==='checkin'||msg==='checkout'?'ok':'fail',labels[msg]||msg.toUpperCase(),'');
addLog('slog',cols[msg]||'t','Attendance: '+msg+(o.response?' '+JSON.stringify(o.response):''))}
updStatus()}

updStatus();setInterval(updStatus,5000);
</script>
</body>
</html>
)rawliteral";
*/ // end moved-to-webpage.h

// ────────────────────────────────────────────────────────────────────
//  AUTO-RECOVERY: re-init sensor & restart auto-scan
// ────────────────────────────────────────────────────────────────────
bool reinitSensor() {
  Serial.println("[SENSOR] Re-init sensor (WiFi OFF)...");
  autoScan = false;
  sensorReady = false;

  // Matikan WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  yield();

  // Tunggu sensor stabil setelah WiFi OFF (serial line clear)
  delay(3000);

  bool ok = false;
  curBaud = 0;

  // Phase 1: Fokus di 57600 (default FPM10A 3.3V) — coba 8x
  Serial.println("[SENSOR] Phase1: 57600 x8...");
  altSerial.end();
  delay(250);
  altSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  delay(400);
  flushRX();
  finger.begin(57600);
  delay(300);

  for (int attempt = 0; attempt < 8 && !ok; attempt++) {
    Serial.printf("[SENSOR] 57600 try %d/8\n", attempt + 1);
    if (finger.verifyPassword()) {
      curBaud = 57600;
      ok = true;
    } else {
      delay(200);
      flushRX();
    }
  }

  // Phase 2: Coba semua baud
  if (!ok) {
    const unsigned long tryBauds[] = {57600, 9600, 19200, 38400};  // 3.3V: skip 115200
    for (int i = 0; i < 4 && !ok; i++) {
      unsigned long baud = tryBauds[i];
      Serial.printf("[SENSOR] Phase2: Baud %lu...\n", baud);
      altSerial.end();
      delay(250);
      altSerial.begin(baud, SERIAL_8N1, FINGER_RX, FINGER_TX);
      delay(400);
      flushRX();
      finger.begin(baud);
      delay(300);
      for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        if (finger.verifyPassword()) {
          curBaud = baud;
          ok = true;
        } else {
          delay(150);
          flushRX();
        }
      }
    }
  }

  // Hidupkan WiFi kembali
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, cred.apPass);
  WiFi.setSleep(false);

  if (ok) {
    delay(200);
    finger.getParameters();
    finger.setSecurityLevel(FINGERPRINT_SECURITY_LEVEL_2);  // 3.3V: gambar kurang detail
    // 3.3V: ESP32 HardwareSerial UART2 stabil, FPM10A bekerja di baud terdeteksi
    finger.getTemplateCount();
    sensorReady = true;
    autoScan = true;
    scanState = SCAN_IDLE;
    lastScanActivity = millis();
    consecutiveErrors = 0;
    recoveryCount = 0;
    ledOn = false;
    finger.LEDcontrol(false);
    lcdShowIdle();
    emit(F("{\"event\":\"autoscan_on\"}"));
    Serial.printf("[SENSOR] Recovered baud=%lu templates=%d\n", curBaud, finger.templateCount);
  } else {
    Serial.println("[SENSOR] Re-init FAILED");
    logError("sensor reinit failed");
  }
  return ok;
}

// ── WiFi reconnect (dipanggil setelah reinitSensor) ──
void wifiReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiApplyPowerPolicy();
    return;
  }
  if (savedWiFiCount == 0) {
    wifiApplyPowerPolicy();
    return;
  }

  for (int i = 0; i < savedWiFiCount; i++) {
    Serial.printf("[WiFi] Reconnect to %s...\n", savedWiFi[i].ssid);
    WiFi.begin(savedWiFi[i].ssid, savedWiFi[i].pass);
    unsigned long start = millis();
    while (millis() - start < 8000) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      staIP = WiFi.localIP().toString();
      staSSID = String(savedWiFi[i].ssid);
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
      Serial.printf("[WiFi] Connected to %s | IP: %s | sleep ON\n", staSSID.c_str(), staIP.c_str());
      return;
    }
    WiFi.disconnect();
  }
  Serial.println("[WiFi] All reconnect attempts failed, AP mode");
  logError("WiFi reconnect: all saved networks failed");
  wifiApplyPowerPolicy();
}

void watchdogCheck() {
  if (!autoScan || enrollActive || restoreActive) return;
  if (scanState != SCAN_IDLE) return; // jangan ganggu saat scan/proses berjalan
  if (millis() - lastScanActivity < SCAN_WATCHDOG_MS) return;

  Serial.printf("[WATCHDOG] No activity for %lu ms\n", millis() - lastScanActivity);
  logError("watchdog: no sensor activity for %lu ms", millis() - lastScanActivity);

  if (millis() - lastRecoveryAttempt < RECOVERY_COOLDOWN) return;
  lastRecoveryAttempt = millis();
  recoveryCount++;

  if (recoveryCount > MAX_RECOVERY) {
    // Soft reset state — jangan reboot tiap kali sensor transient (sering setelah scan)
    Serial.println("[WATCHDOG] Max recovery → soft reset (no reboot)");
    emit(F("{\"event\":\"watchdog_soft_reset\"}"));
    flushRX();
    delay(500);
    flushRX();
    recoveryCount = 0;
    consecutiveErrors = 0;
    lastScanActivity = millis();
    scanCooldownUntil = millis() + 8000;
    reinitSensor();
    return;
  }

  Serial.printf("[WATCHDOG] Recovery attempt %d/%d\n", recoveryCount, MAX_RECOVERY);
  if (!reinitSensor()) {
    Serial.println("[WATCHDOG] Will retry next cycle");
    logError("watchdog sensor recovery attempt failed count=%d", recoveryCount);
  }
}

// ────────────────────────────────────────────────────────────────────
//  RESET REASON DIAGNOSTIC (3.3V board: reset paling sering brownout
//  hardware — arus spike LCD+TFT+WiFi TX bareng, BUKAN ESP.restart()
//  software. Register BOD di-disable di bawah supaya rail 3.3V yang
//  sedikit ngedip tidak langsung reset chip; log ini untuk konfirmasi.)
// ────────────────────────────────────────────────────────────────────
const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (power-on normal)";
    case ESP_RST_EXT:       return "EXT (external pin reset)";
    case ESP_RST_SW:        return "SW (ESP.restart() dipanggil di kode)";
    case ESP_RST_PANIC:     return "PANIC (crash/exception)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog timeout)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wakeup";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (rail 3.3V ngedip — HARDWARE, bukan software)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// ────────────────────────────────────────────────────────────────────
//  SETUP
// ────────────────────────────────────────────────────────────────────
void setup() {
  // Disable hardware brownout detector SEDINI mungkin (sebelum apapun lain).
  // 3.3V FPM10A + LCD backlight + TFT SPI + WiFi TX bareng bisa bikin rail
  // ngedip sesaat → BOD reset chip instan, TIDAK lewat ESP.restart() kode
  // manapun (makanya semua fix software sebelumnya tidak ngefek).
  // Root fix sebenarnya = perkuat power supply (cap besar di rail 3.3V),
  // ini hanya mitigasi supaya dip sesaat tidak langsung mereset device.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(9600);
  delay(100);
  Serial.println("\n\n=== FPM10A Bridge Boot ===");
  Serial.printf("[BOOT] Reset reason: %s\n", resetReasonStr(esp_reset_reason()));
  Serial.print("Free heap: "); Serial.println(ESP.getFreeHeap());

  // LCD
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);
  ledcAttach(LCD_BL, 250, 8);  // pin, 250Hz, 8-bit (0-255)
  ledcWrite(LCD_BL, 255);      // backlight penuh saat boot
  lcdBacklightOn = true;
  lastLcdActivity = millis();

  lcdShowBoot();
  lcdProgress(10);

  // LittleFS: gunakan partisi data bertipe/berlabel "littlefs".
  storageInit();
  esp_reset_reason_t bootReason = esp_reset_reason();
  if (bootReason != ESP_RST_POWERON) {
    logError("boot reset reason code=%d", (int)bootReason);
  }
  credLoad();
  dbLoad();
  settingsLoad();

  // Gate kehadiran jari (T-OUT / IR) — pinMode + kalibrasi dijalankan di
  // AKHIR setup (lihat di bawah) setelah TTP233D stabilisasi.

  lcdProgress(25);

  // WiFi Manager (AP always on + STA if saved)
  wifiInit();
  lcdProgress(35);

  // NTP - start after WiFi
  timeClient.begin();
  timeClient.setUpdateInterval(60000);
  timeClient.update();

  // Web server
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/count", handleCount);
  server.on("/api/list", handleList);
  server.on("/api/enroll", HTTP_POST, handleEnroll);
  server.on("/api/enroll", HTTP_OPTIONS, handleEnroll);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/api/delete", HTTP_OPTIONS, handleDelete);
  server.on("/api/empty", HTTP_POST, handleEmpty);
  server.on("/api/empty", HTTP_OPTIONS, handleEmpty);
  server.on("/api/autoscan/on", HTTP_POST, handleAutoOn);
  server.on("/api/autoscan/on", HTTP_OPTIONS, handleAutoOn);
  server.on("/api/autoscan/off", HTTP_POST, handleAutoOff);
  server.on("/api/autoscan/off", HTTP_OPTIONS, handleAutoOff);
  server.on("/api/events", handleSSE);
  // WiFi API
  server.on("/api/wifi", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/wifi", HTTP_OPTIONS, handleWifiSave);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/api/wifi/reset", HTTP_OPTIONS, handleWifiReset);
  server.on("/api/wifi/delete", HTTP_POST, handleWifiDelete);
  server.on("/api/wifi/delete", HTTP_OPTIONS, handleWifiDelete);
  // Settings API
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/settings", HTTP_OPTIONS, handleSettingsSave);
  // Branch/Employee proxy API
  server.on("/api/branches", HTTP_GET, handleBranches);
  server.on("/api/branches", HTTP_OPTIONS, handleBranches);
  server.on("/api/employees", HTTP_GET, handleEmployees);
  server.on("/api/employees", HTTP_OPTIONS, handleEmployees);
  // Backup / Restore API
  server.on("/api/backup", HTTP_GET, handleBackup);
  server.on("/api/backup/full", HTTP_GET, handleBackupFull);
  server.on("/api/restore", HTTP_POST, handleRestore);
  server.on("/api/restore", HTTP_OPTIONS, handleRestore);
  server.on("/api/restore/template", HTTP_POST, handleRestoreTemplate);
  server.on("/api/restore/template", HTTP_OPTIONS, handleRestoreTemplate);
  server.on("/api/sync/from-server", HTTP_POST, handleSyncFromServer);
  server.on("/api/sync/from-server", HTTP_OPTIONS, handleSyncFromServer);
  server.on("/api/server/templates", HTTP_GET, handleServerTemplates);
  server.on("/api/server/templates", HTTP_OPTIONS, handleServerTemplates);
  server.on("/api/storage", HTTP_GET, handleStorage);
  // Credentials API
  server.on("/api/credentials", HTTP_GET, handleCredGet);
  server.on("/api/credentials", HTTP_POST, handleCredSave);
  server.on("/api/credentials", HTTP_OPTIONS, handleCredSave);

  server.on("/api/debug/log", HTTP_GET, handleDebugLog);
  server.on("/api/debug/errors", HTTP_GET, handleErrorLog);
  server.onNotFound(handleNotFound);
  server.begin();

  // ── BLE ──
  bleInit();

  lcdProgress(45);

  // ── Fingerprint sensor auto-detect ──
  // ESP32 HardwareSerial lebih stabil dari ESP8266 SoftwareSerial.
  // WiFi OFF saat deteksi untuk keandalan maksimal.
  Serial.println("[SENSOR] Detecting sensor (WiFi OFF)...");
  lcdEnrollStep("Init Sensor", -1, "Deteksi sensor...", COL_ACCENT);

  // Simpan status WiFi lalu matikan
  bool wasWifiConnected = wifiConnected;
  String wasStaIP = staIP;
  String wasStaSSID = staSSID;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  Serial.println("[SENSOR] WiFi OFF for sensor detection");
  yield();

  // Tunggu sensor stabilisasi (power-on reset, 3.3V perlu lebih lama)
  delay(3000);

  bool ok = false;
  curBaud = 0;
  const unsigned long tryBauds[] = {57600, 9600, 19200, 38400};  // 3.3V: 57600 default, skip 115200

  for (int i = 0; i < 4 && !ok; i++) {
    unsigned long baud = tryBauds[i];
    Serial.printf("[SENSOR] Baud %lu...\n", baud);

    altSerial.end();
    delay(250);
    altSerial.begin(baud, SERIAL_8N1, FINGER_RX, FINGER_TX);
    delay(400);
    flushRX();
    finger.begin(baud);
    delay(300);

    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
      if (finger.verifyPassword()) {
        curBaud = baud;
        ok = true;
        Serial.printf("[SENSOR] FOUND at %lu (attempt %d)\n", curBaud, attempt + 1);
      } else {
        delay(100);
        flushRX();
      }
    }
  }

  // Hidupkan WiFi kembali
  Serial.println("[SENSOR] Re-enabling WiFi...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, cred.apPass);
  if (wasWifiConnected && wasStaSSID.length() > 0) {
    for (int i = 0; i < savedWiFiCount; i++) {
      if (String(savedWiFi[i].ssid) == wasStaSSID) {
        WiFi.begin(savedWiFi[i].ssid, savedWiFi[i].pass);
        break;
      }
    }
    unsigned long start = millis();
    while (millis() - start < 10000) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      staIP = WiFi.localIP().toString();
      staSSID = wasStaSSID;
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
      Serial.printf("[WiFi] Reconnected to %s | IP: %s | sleep ON\n", staSSID.c_str(), staIP.c_str());
    } else {
      wifiConnected = false;
      wifiApplyPowerPolicy();
      Serial.println("[WiFi] Reconnect failed, AP mode");
    }
  } else {
    wifiApplyPowerPolicy();
    Serial.println("[WiFi] AP mode active");
  }

  lcdProgress(80);

  finger.getParameters();
  if (ok) finger.setSecurityLevel(FINGERPRINT_SECURITY_LEVEL_2);  // 3.3V: gambar kurang detail
  finger.getParameters();

  // ESP32 HardwareSerial UART2 stabil — FPM10A 3.3V dapat di baud mana saja
  sensorReady = ok;
  finger.getTemplateCount();

  Serial.print("[SENSOR] ready=");
  Serial.print(ok ? "YES" : "NO");
  if (ok) { Serial.print(" baud="); Serial.print(curBaud); }
  Serial.print(" templates=");
  Serial.println(finger.templateCount);

  lcdProgress(100);
  delay(400);

  // Gate kehadiran jari (T-OUT / IR) diinisialisasi sekarang — sudah ~20s
  // sejak power-on, TTP233D sudah lewat stabilisasi & auto-calibrate 1s.
  // Jangan sentuh sensor selama irCalibrate() berlangsung.
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  irCalibrate();

  if (ok) {
    autoScan = true;
    scanState = SCAN_IDLE;
    lastScanActivity = millis();
    ledOn = false;
    finger.LEDcontrol(false);
    Serial.println("[LED] Off — controlled via 0x51 (klon baru)");
    emit(F("{\"event\":\"autoscan_on\"}"));
    lcdShowIdle();
  } else {
    tft.fillScreen(COL_BG);
    tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
    tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, COL_WARN);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.setTextSize(2);
    tft.drawString("RECONNECT", SCREEN_W / 2, 60);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextSize(1);
    tft.drawString("Menunggu sensor...", SCREEN_W / 2, 90);
    lcdDrawFooter();
  }

  emit(F("{\"event\":\"ready\",\"found\":%s,\"baud\":%lu,\"security\":%d}"),
       ok ? "true" : "false", curBaud, finger.security_level);
}

// ────────────────────────────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────────────────────────────
void loop() {
  // BLE WiFi save — simpan + langsung coba connect
  if (bleWifiSaveRequested) {
    bleWifiSaveRequested = false;
    bleSaveWifi(bleWifiSsid, bleWifiPass);
    // Trigger connect langsung (jangan tunggu 30s auto-reconnect)
    WiFi.begin(bleWifiSsid, bleWifiPass);
  }

  // BLE Enroll — blocking seperti web UI enroll, jalan di main loop
  if (bleEnrollRequested) {
    bleEnrollRequested = false;
    File f = LittleFS.open("/ble_enroll.json", "r");
    if (f) {
      String data = f.readString();
      f.close();
      LittleFS.remove("/ble_enroll.json");
      DynamicJsonDocument doc(256);
      if (!deserializeJson(doc, data)) {
        const char *eid = doc["employeeId"] | "";
        const char *nm  = doc["name"] | "";
        if (eid[0] && nm[0]) {
          bool wasAuto = autoScan;
          autoScan = false;
          enrollFinger(0, nm, eid);  // id=0 → auto-assign
          autoScan = wasAuto;
          if (autoScan) { ledOn = false; finger.LEDcontrol(false); scanState = SCAN_IDLE; }
        }
      }
    }
  }

  // Poll sensor ~20 Hz (bukan ~500 Hz). delay(2) + WiFi no-sleep = ESP32 panas.
  if (autoScan) {
    doAutoScan();
    watchdogCheck();
    delay(40);
  } else {
    // auto reconnect sensor
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 5000) {
      lastRetry = millis();
      Serial.println("[LOOP] Trying sensor reconnect...");
      if (reinitSensor()) {
        Serial.println("[LOOP] Sensor reconnected!");
        recoveryCount = 0;
        wifiReconnect();
      } else {
        recoveryCount++;
        Serial.printf("[LOOP] Reconnect failed (%d/%d)\n", recoveryCount, MAX_RECOVERY);
        logError("loop sensor reconnect failed count=%d", recoveryCount);
        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);
        WiFi.softAP(AP_SSID, cred.apPass);
        if (recoveryCount >= MAX_RECOVERY) {
          Serial.println("[LOOP] Max recovery → soft wait (no reboot)");
          logError("loop sensor max recovery reached");
          emit(F("{\"event\":\"sensor_wait\"}"));
          recoveryCount = 0;
          lastScanActivity = millis();
          delay(10000);
        }
      }
    }
    delay(50);
  }

  // Housekeeping
  static uint8_t hkTick = 0;
  hkTick++;
  if (hkTick >= 2) {
    hkTick = 0;
    server.handleClient();
    timeClient.update();
    checkAutoSleep();

    // BLE status update setiap 5 detik
    static unsigned long lastBleStatus = 0;
    if (millis() - lastBleStatus > 5000) {
      lastBleStatus = millis();
      bleUpdateStatus();
    }

    // TX power saja — jangan ganti mode WiFi berkala (putus web UI)
    static unsigned long lastPowerPolicy = 0;
    if (millis() - lastPowerPolicy > 30000) {
      lastPowerPolicy = millis();
      wifiApplyPowerPolicy();

      // Auto-reconnect saat WiFi drop (tiap 60 detik coba lagi)
      if (!wifiConnected && savedWiFiCount > 0) {
        static unsigned long lastWifiRetry = 0;
        if (millis() - lastWifiRetry > 30000) {
          lastWifiRetry = millis();
          Serial.println("[WiFi] Auto-reconnect attempt...");
          for (int i = 0; i < savedWiFiCount; i++) {
            WiFi.begin(savedWiFi[i].ssid, savedWiFi[i].pass);
            unsigned long st = millis();
            while (millis() - st < 6000)
              if (WiFi.status() == WL_CONNECTED) break; else delay(100);
            if (WiFi.status() == WL_CONNECTED) {
              wifiConnected = true; staIP = WiFi.localIP().toString();
              staSSID = String(savedWiFi[i].ssid);
              Serial.printf("[WiFi] Reconnected %s | %s\n", staSSID.c_str(), staIP.c_str());
              break;
            }
            WiFi.disconnect(); delay(200);
          }
        }
      }
    }

    // Matikan backlight dan hentikan refresh LCD setelah lama tanpa sentuhan.
    if (lcdBacklightOn && millis() - lastLcdActivity > LCD_IDLE_TIMEOUT_MS) {
      ledcWrite(LCD_BL, 0);
      lcdBacklightOn = false;
    }

    // Update jam & suhu hanya saat LCD masih aktif.
    static unsigned long lastClock = 0;
    if (lcdBacklightOn && millis() - lastClock > 1000) {
      lastClock = millis();
      tft.setTextDatum(TR_DATUM);
      tft.setTextSize(1);

      tft.fillRect(SCREEN_W - 88, FOOTER_Y + 16, 80, 10, COL_TOPBAR);
      String t = timeClient.isTimeSet() ? timeClient.getFormattedTime() : "--:--:--";
      tft.setTextColor(COL_DIM2, COL_TOPBAR);
      tft.drawString(t, SCREEN_W - 8, FOOTER_Y + 16);

      tft.fillRect(SCREEN_W - 168, FOOTER_Y + 16, 68, 10, COL_TOPBAR);
      float suhu = temperatureRead();
      char tmpBuf[10];
      snprintf(tmpBuf, sizeof(tmpBuf), "%.0f C", suhu);
      tft.setTextColor(COL_DIM2, COL_TOPBAR);
      tft.drawString(tmpBuf, SCREEN_W - 96, FOOTER_Y + 16);
    }

    char line[80];
    if (nextLine(line, sizeof(line))) {
      broadcastSSE(line);
    }
  }
}
