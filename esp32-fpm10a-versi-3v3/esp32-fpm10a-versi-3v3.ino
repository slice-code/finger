#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <StreamString.h>
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
#include <esp_task_wdt.h>
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
DNSServer dnsServer;
bool dnsServerActive = false;
#define DNS_PORT 53
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

// Harus di atas requireAuth() — flag mode AP setup (bukan auth).
bool wifiApSetupMode = true;

// Satu HTTPS/TLS pada satu waktu (attnHttp + cache + register) — cegah OOM NimBLE.
static SemaphoreHandle_t httpsMutex = nullptr;
static volatile bool httpsBusy = false;
bool cacheSyncBusy = false;  // forward — dipakai WiFi scan guard sebelum blok cache

bool httpsLock(uint32_t waitMs) {
  if (!httpsMutex) return true;
  if (xSemaphoreTake(httpsMutex, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
  httpsBusy = true;
  return true;
}
void httpsUnlock() {
  httpsBusy = false;
  if (httpsMutex) xSemaphoreGive(httpsMutex);
}

// ── Shared outbound HTTP(S) client ─────────────────────────────────
// Satu WiFiClientSecure untuk semua API — 4× static client sebelumnya
// boros ~15–25KB RAM/tls context dan HTTPS ke Cloudflare sering gagal (OOM).
static WiFiClientSecure apiSecureClient;
static WiFiClient apiPlainClient;
static bool apiTlsConfigured = false;

// TLS mbedTLS + Cloudflare butuh ~60–80KB free. Di bawah ini HTTPS sering
// jadi "connection refused" palsu. HTTP ke host yang sama tetap jalan.
#ifndef API_HTTPS_MIN_HEAP
#define API_HTTPS_MIN_HEAP 70000u
#endif

static void apiTlsConfigure() {
  if (apiTlsConfigured) return;
  apiSecureClient.setInsecure();
  apiSecureClient.setHandshakeTimeout(30000);  // TLS handshake Cloudflare butuh waktu
  apiTlsConfigured = true;
}

static void apiTlsReset() {
  apiTlsConfigure();
  apiSecureClient.setInsecure();  // ulang tiap request — state bisa kotor setelah stop()
  apiSecureClient.stop();  // reset sesi TLS, jangan delete objek (use-after-free WiFi)
}

static bool apiUrlIsHttps(const String &url) {
  return url.startsWith("https://") || url.startsWith("HTTPS://");
}

// https://host/path → http://host/path (untuk fallback RAM / retry).
static String apiToPlainHttp(const String &url) {
  if (url.startsWith("https://")) return String("http://") + url.substring(8);
  if (url.startsWith("HTTPS://")) return String("http://") + url.substring(8);
  return url;
}

// Pilih URL efektif: paksa HTTP jika heap ketat (HTTPS hampir pasti gagal).
static String apiEffectiveUrl(const String &url) {
  if (!apiUrlIsHttps(url)) return url;
  uint32_t heap = ESP.getFreeHeap();
  if (heap < API_HTTPS_MIN_HEAP) {
    String plain = apiToPlainHttp(url);
    Serial.printf("[API] HTTPS->HTTP (heap=%u < %u) %s\n",
                  heap, (unsigned)API_HTTPS_MIN_HEAP, plain.c_str());
    return plain;
  }
  return url;
}

static void apiHttpLogError(const char *tag, HTTPClient &http, int code) {
  if (code > 0) return;
  const String err = http.errorToString(code);
  Serial.printf("[%s] HTTP err %d (%s) heap=%u\n", tag, code, err.c_str(), ESP.getFreeHeap());
  logError("%s HTTP err %d (%s) heap=%u", tag, code, err.c_str(), ESP.getFreeHeap());
}

// Begin HTTPClient — HTTPS: no reuse, stop() dulu, timeout lebih panjang.
// Pemanggil boleh kirim URL https; di sini otomatis turun ke http jika heap ketat.
static bool apiHttpBegin(HTTPClient &http, WiFiClient *&client, const String &urlIn, uint32_t timeoutMs = 0) {
  String url = apiEffectiveUrl(urlIn);
  bool tls = apiUrlIsHttps(url);
  if (tls) {
    apiTlsReset();
    client = &apiSecureClient;
    http.setReuse(false);
    if (!timeoutMs) timeoutMs = 30000;
  } else {
    // Jangan stop() agresif di plain TCP — bisa ganggu lwIP async close.
    client = &apiPlainClient;
    http.setReuse(false);
    if (!timeoutMs) timeoutMs = 12000;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  Serial.printf("[API] begin %s heap=%u\n", url.c_str(), ESP.getFreeHeap());
  if (!http.begin(*client, url)) {
    Serial.printf("[API] begin failed url=%s tls=%d heap=%u\n", url.c_str(), tls ? 1 : 0, ESP.getFreeHeap());
    logError("API begin failed tls=%d heap=%u", tls ? 1 : 0, ESP.getFreeHeap());
    return false;
  }
  return true;
}

bool requireAuth() {
  // Web UI lokal: tanpa HTTP Basic Auth di AP maupun STA.
  // - AP: dilindungi password softAP; Basic Auth bikin HP stuck 401.
  // - STA (WiFi connected): akses lewat IP LAN, bukan Access Point —
  //   Basic Auth juga sering 401 di browser HP.
  // Kredensial web tetap di tab Akun (disimpan), tidak dipakai gate HTTP.
  return true;
}

// ── State ──────────────────────────────────────────────────────────
bool autoScan = false;
bool fingerDown = false;
bool sensorReady = false;
bool enrollActive = false;
bool restoreActive = false;
bool wifiConnected = false;
// wifiApSetupMode dideklarasikan di atas requireAuth().
// true = AP setup → jangan auto WiFi.begin berkala (AP goyang / client putus).
bool wifiStaEverOk = false;  // pernah sukses STA di sesi boot ini
String staIP = "";
String staSSID = "";
bool scanSleeping = false;
bool ledOn = false;           // LED FPM10A terkontrol via 0x50/0x51 (sensor klon baru)
unsigned long ledOnSince = 0; // sejak kapan LED nyala (auto-off 5 detik)
#define LED_AUTO_OFF_MS 5000  // LED nyala >5s tanpa hasil scan → mati paksa
bool irPolarityHigh = true;   // true = jari terdeteksi saat pin HIGH
bool irCalibrated = false;
bool irFallbackMode = false;  // gate tidak pernah berubah → pakai fallback polling normal
bool irGateOpen = false;
unsigned long irDetSince = 0;
unsigned long irClearSince = 0;
unsigned long irGateOpenedAt = 0;
unsigned long irGateCooldownUntil = 0;
unsigned long lastFallbackPoll = 0;
bool lcdBacklightOn = true;
unsigned long lastLcdActivity = 0;
#define LCD_IDLE_TIMEOUT_MS 60000
unsigned long curBaud = 0;
char rxBuf[80];
uint8_t rxLen = 0;

enum ScanState { SCAN_IDLE, SCAN_BUSY, SCAN_WAIT_RELEASE };
ScanState scanState = SCAN_IDLE;
unsigned long scanResultTime = 0;
unsigned long scanCooldownUntil = 0;
int consecutiveErrors = 0;
uint8_t fallbackErrors = 0;
bool fingerMustRelease = false;  // setelah scan: wajib angkat jari dulu sebelum scan/enroll ulang
#define SCAN_RESULT_HOLD_MS 800   // tampil hasil singkat — jangan bikin terasa lambat
#define MAX_CONSECUTIVE_ERRORS 8   // 3.3V: lebih toleran transient error
#define SCAN_SOFT_RECOVER_MS 1000  // jeda setelah error burst sebelum scan ulang
#define LED_WARMUP_MS      120    // jeda setelah LED ON sebelum getImage() (3.3V butuh lebih lama)
#define LED_OFF_RETRY_MS  2000    // retry hanya jika perintah LED OFF gagal

// ── Watchdog / Auto-Recovery ──────────────────────────────────────
unsigned long lastScanActivity = 0;  // millis terakhir sensor merespons
unsigned long lastRecoveryAttempt = 0;
uint8_t recoveryCount = 0;
#define SCAN_WATCHDOG_MS   20000  // 20 detik tanpa aktivitas = recovery (3.3V lebih lambat)
#define RECOVERY_COOLDOWN  5000   // jeda antar recovery attempt
#define MAX_RECOVERY       3      // max soft recovery; lalu idle+AP (bukan ESP.restart)

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
  bool apEnabled;         // false = BLE-only setup (softAP mati)
  uint16_t uploadIntervalMinutes; // jadwal auto-sync pending (default 120 = 2 jam)
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
    if (appSettings.apEnabled) {
      tft.drawString("AP:" AP_SSID, SIDEBAR_W + 18, FOOTER_Y + 5);
      tft.setTextColor(COL_DIM2, COL_TOPBAR);
      tft.drawString("192.168.4.1", SIDEBAR_W + 18, FOOTER_Y + 17);
    } else {
      tft.drawString("BLE:PJTKI-Finger", SIDEBAR_W + 18, FOOTER_Y + 5);
      tft.setTextColor(COL_DIM2, COL_TOPBAR);
      tft.drawString("AP off — setup via app", SIDEBAR_W + 18, FOOTER_Y + 17);
    }
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
  if (strcmp(status, "sending") == 0) {
    tft.setTextColor(COL_ACCENT, COL_DIM3);
    tft.drawString("MENGIRIM ABSENSI...", SCREEN_W / 2, 183);
  } else if (strcmp(status, "checkin") == 0) {
    tft.setTextColor(COL_OK, COL_DIM3);
    tft.drawString("CHECK IN", SCREEN_W / 2, 183);
  } else if (strcmp(status, "checkout") == 0) {
    tft.setTextColor(COL_ACCENT, COL_DIM3);
    tft.drawString("CHECK OUT", SCREEN_W / 2, 183);
  } else if (strcmp(status, "ignored") == 0) {
    tft.setTextColor(COL_WARN, COL_DIM3);
    tft.drawString("SUDAH ABSEN", SCREEN_W / 2, 183);
  } else if (strcmp(status, "offline") == 0) {
    tft.setTextColor(COL_WARN, COL_DIM3);
    tft.drawString("OFFLINE — absensi lokal", SCREEN_W / 2, 183);
  } else if (strcmp(status, "ok") == 0 || strcmp(status, "success") == 0 ||
             strcmp(status, "berhasil") == 0) {
    tft.setTextColor(COL_OK, COL_DIM3);
    tft.drawString("BERHASIL", SCREEN_W / 2, 183);
  } else if (strcmp(status, "not_found") == 0) {
    tft.setTextColor(COL_ERR, COL_DIM3);
    tft.drawString("TIDAK DIKENALI", SCREEN_W / 2, 183);
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
  bleNotifyEvent(buf);
}

// Escape string untuk disisipkan ke payload JSON (SSE/BLE). Mencegah
// injection yang merusak JSON semua client saat nama/employeeId berisi
// tanda kutip atau backslash.
const char *jsonEscape(const char *in) {
  static char esc[256];
  int j = 0;
  for (const char *s = in; *s && j < (int)sizeof(esc) - 2; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"':  esc[j++] = '\\'; esc[j++] = '"';  break;
      case '\\': esc[j++] = '\\'; esc[j++] = '\\'; break;
      case '\n': esc[j++] = '\\'; esc[j++] = 'n';  break;
      case '\r': esc[j++] = '\\'; esc[j++] = 'r';  break;
      case '\t': esc[j++] = '\\'; esc[j++] = 't';  break;
      default:
        if (c < 0x20) {
          j += snprintf(esc + j, sizeof(esc) - (size_t)j, "\\u%04x", c);
        } else {
          esc[j++] = (char)c;
        }
    }
  }
  esc[j] = 0;
  return esc;
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

// Enroll butuh LED tetap nyala di antara 2 scan. Jangan pakai getImage()
// untuk deteksi jari diangkat: GenImg + NOFINGER pada klon baru mematikan
// lampu penerangan, sehingga scan ke-2 tidak pernah dapat gambar.
bool irRawDetected();
static void enrollLedKeepOn() {
  // Jangan set ledOn=true tanpa verifikasi — jika LEDcontrol gagal (sensor
  // error), LED fisik mati tapi ledOn=true → scan tidak pernah menyalakan
  // ulang → LED stuck mati walau jari disentuh.
  uint8_t r = finger.LEDcontrol(true);
  ledOn = (r == FINGERPRINT_OK);
  if (!ledOn) {
    ledOnSince = 0;
  }
}

static void enrollKeepAliveUi() {
  lastScanActivity = millis();
  lastLcdActivity = millis();
  if (!lcdBacklightOn) {
    lcdBacklightOn = true;
    ledcWrite(LCD_BL, 255);
  } else {
    ledcWrite(LCD_BL, 255);
  }
  enrollLedKeepOn();
}

// Bangunkan LCD saja (konek BLE) — JANGAN nyalakan LED FPM10A di sini.
// LED tanpa ledOnSince bikin doAutoScan langsung masuk LED_AUTO_OFF → scan macet
// sampai layar redup.
void bleWakeLcd() {
  lastScanActivity = millis();
  lastLcdActivity = millis();
  lcdBacklightOn = true;
  ledcWrite(LCD_BL, 255);
}

// Bangunkan LCD + LED untuk enroll.
void bleWakeUi() {
  bleWakeLcd();
  uint8_t r = finger.LEDcontrol(true);
  bool ok = (r == FINGERPRINT_OK);
  ledOn = ok;
  ledOnSince = ok ? millis() : 0;
  if (!ok) {
    logError("LED ON (bleWakeUi) failed code=%d", r);
  }
}

// Reset state scan/LED/gate sebelum enroll atau setelah gagal — supaya UART bersih.
void sensorResumeIdle(const char *reason) {
  Serial.printf("[SENSOR] resume idle (%s)\n", reason ? reason : "-");
  enrollActive = false;
  if (ledOn) {
    uint8_t ledResult = finger.LEDcontrol(false);
    if (ledResult != FINGERPRINT_OK) {
      logError("LED OFF (resume idle) failed code=%d", ledResult);
    }
    // SELALU reset false — walau gagal, biarkan gate scan mencoba LED ON lagi.
    ledOn = false;
  }
  ledOnSince = 0;
  flushRX();
  delay(30);
  flushRX();
  scanState = SCAN_IDLE;
  scanCooldownUntil = 0;
  fingerDown = false;
  fingerMustRelease = false;
  consecutiveErrors = 0;
  irGateOpen = false;
  irDetSince = irClearSince = 0;
  irGateCooldownUntil = 0;
  lastScanActivity = millis();
  lastLcdActivity = millis();
  autoScan = true;
}

// Timeout panjang: mode enroll menunggu jari sampai 2 scan selesai / dibatalkan.
static const unsigned long ENROLL_WAIT_MS = 180000UL;

bool waitNoFinger() {
  unsigned long t = millis();
  unsigned long clearSince = 0;
  unsigned long lastLedRefresh = 0;
  int errStreak = 0;
  while (millis() - t < ENROLL_WAIT_MS) {
    if (bleEnrollCancelRequested) return false;
    enrollKeepAliveUi();
    yield();
    if (millis() - lastLedRefresh > 800) {
      lastLedRefresh = millis();
      enrollLedKeepOn();
    }
    // Enroll: getImage adalah otoritas utama. IR hanya bantuan —
    // jangan blokir jika T-OUT/IR belum deteksi (sering saat jari hanya di kaca optik).
    flushRX();
    disableLoopWDT();
    uint8_t p = finger.getImage();
    enableLoopWDT();
    esp_task_wdt_reset();
    enrollLedKeepOn();
    delay(LED_WARMUP_MS);
    if (p == FINGERPRINT_NOFINGER) {
      if (appSettings.irEnabled && irCalibrated && irRawDetected()) {
        // Sensor bilang kosong tapi IR masih “ada” — tunggu IR clear sebentar.
        if (!clearSince) clearSince = millis();
        else if (millis() - clearSince >= 120) return true;
      } else {
        return true;
      }
    } else if (p == FINGERPRINT_OK) {
      // Ada jari masih nempel — tunggu sampai diangkat.
      clearSince = 0;
      errStreak = 0;
    } else {
      // Error komunikasi beruntun → sensor macet. Batalkan enroll cepat,
      // jangan tunggu 180s (membuat "gagal sensor" menggantung).
      if (++errStreak >= 5) {
        logError("enroll waitNoFinger sensor error x%d (code=%d) — abort", errStreak, p);
        return false;
      }
      clearSince = 0;
    }
    delay(40);
  }
  return false;
}

bool waitFinger() {
  unsigned long t = millis();
  unsigned long lastLedRefresh = 0;
  int errStreak = 0;
  enrollKeepAliveUi();
  delay(200);
  while (millis() - t < ENROLL_WAIT_MS) {
    if (bleEnrollCancelRequested) return false;
    enrollKeepAliveUi();
    yield();
    if (millis() - lastLedRefresh > 800) {
      lastLedRefresh = millis();
      enrollLedKeepOn();
    }
    // Selalu poll getImage saat enroll — jangan menunggu IR dulu.
    flushRX();
    disableLoopWDT();
    uint8_t p = finger.getImage();
    enableLoopWDT();
    esp_task_wdt_reset();
    if (p == FINGERPRINT_OK) return true;
    if (p != FINGERPRINT_NOFINGER) {
      // Error komunikasi beruntun → sensor macet. Jangan tunggu 180s.
      if (++errStreak >= 5) {
        logError("enroll waitFinger sensor error x%d (code=%d) — abort", errStreak, p);
        return false;
      }
    } else {
      errStreak = 0;
    }
    enrollLedKeepOn();
    delay(40);
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

void cacheSetEmployeeRegistered(const char *empId, bool registered);
void cacheInvalidateAllEmployees();

void dbAdd(uint8_t id, const char *name, const char *empId) {
  for (int i = 0; i < fpCount; i++) {
    if (fpDB[i].id == id) {
      strncpy(fpDB[i].name, name, 31);
      strncpy(fpDB[i].empId, empId ? empId : "", 15);
      dbSave();
      if (empId && empId[0]) cacheSetEmployeeRegistered(empId, true);
      return;
    }
  }
  if (fpCount < MAX_FP) {
    fpDB[fpCount].id = id;
    strncpy(fpDB[fpCount].name, name, 31);
    strncpy(fpDB[fpCount].empId, empId ? empId : "", 15);
    fpCount++;
    dbSave();
    if (empId && empId[0]) cacheSetEmployeeRegistered(empId, true);
  }
}

void dbRemove(uint8_t id) {
  for (int i = 0; i < fpCount; i++) {
    if (fpDB[i].id == id) {
      char empId[16];
      strncpy(empId, fpDB[i].empId, 15);
      empId[15] = 0;
      for (int j = i; j < fpCount - 1; j++) fpDB[j] = fpDB[j + 1];
      fpCount--;
      dbSave();
      if (empId[0]) cacheSetEmployeeRegistered(empId, false);
      return;
    }
  }
}

void dbClear() {
  fpCount = 0;
  dbSave();
  cacheInvalidateAllEmployees();
}

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
// ESP32 + BLE: HTTPS ke Cloudflare sering gagal (RAM/TLS). Simpan sebagai http://.
bool settingsNormalizeApiUrl(char *url, size_t cap) {
  if (!url || !url[0] || cap < 10) return false;
  bool https = (strncmp(url, "https://", 8) == 0) || (strncmp(url, "HTTPS://", 8) == 0);
  if (!https) return false;
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "http://%s", url + 8);
  strncpy(url, tmp, cap - 1);
  url[cap - 1] = 0;
  return true;
}

bool settingsSave();  // forward — dipanggil saat migrasi https→http di load

void settingsLoad() {
  memset(&appSettings, 0, sizeof(appSettings));
  appSettings.irEnabled = true;  // default aktif
  // Default jadwal: aktif 05:00–00:00 (tidur 00–05) — sama perilaku lama hardcoded
  appSettings.scanSchedule = true;
  appSettings.scanStartHour = 5;
  appSettings.scanEndHour = 0;
  // BLE-first: softAP default OFF — setup WiFi/settings lewat app BLE
  appSettings.apEnabled = false;
  appSettings.uploadIntervalMinutes = 120; // default 2 jam
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
  if (doc.containsKey("scan_schedule")) appSettings.scanSchedule = doc["scan_schedule"] | true;
  if (doc.containsKey("scan_start_hour")) {
    int h = doc["scan_start_hour"] | 5;
    if (h < 0) h = 0; if (h > 23) h = 23;
    appSettings.scanStartHour = (uint8_t)h;
  }
  if (doc.containsKey("scan_end_hour")) {
    int h = doc["scan_end_hour"] | 0;
    if (h < 0) h = 0; if (h > 23) h = 23;
    appSettings.scanEndHour = (uint8_t)h;
  }
  // Tanpa key di JSON lama → tetap false (BLE-first). Explicit true untuk aktifkan AP.
  if (doc.containsKey("ap_enabled")) appSettings.apEnabled = doc["ap_enabled"] | false;
  if (doc.containsKey("upload_interval_minutes")) {
    int v = doc["upload_interval_minutes"] | 120;
    if (v < 5) v = 5; if (v > 1440) v = 1440;
    appSettings.uploadIntervalMinutes = (uint16_t)v;
  }
  // Migrasi: https di LittleFS → http (ESP32+BLE tidak sanggup TLS stabil).
  if (settingsNormalizeApiUrl(appSettings.apiBaseUrl, sizeof(appSettings.apiBaseUrl))) {
    Serial.printf("[SET] migrated apiBaseUrl -> %s\n", appSettings.apiBaseUrl);
    settingsSave();
  }
}

bool settingsSave() {
  if (!storageReady) return false;
  DynamicJsonDocument doc(768);
  doc["apiBaseUrl"] = appSettings.apiBaseUrl;
  doc["kode_cabang"] = appSettings.kodeCabang;
  doc["device_id"] = appSettings.deviceId;
  doc["api_key"] = appSettings.apiKey;
  doc["ir_enabled"] = appSettings.irEnabled;
  doc["scan_schedule"] = appSettings.scanSchedule;
  doc["scan_start_hour"] = appSettings.scanStartHour;
  doc["scan_end_hour"] = appSettings.scanEndHour;
  doc["ap_enabled"] = appSettings.apEnabled;
  doc["upload_interval_minutes"] = appSettings.uploadIntervalMinutes;
  File f = LittleFS.open(SETTINGS_FILENAME, "w");
  if (!f) { logError("settings write open failed"); return false; }
  serializeJson(doc, f);
  f.close();
  return true;
}

// ────────────────────────────────────────────────────────────────────
//  Offline attendance log + pending register queue (LittleFS)
// ────────────────────────────────────────────────────────────────────
// Absensi selalu disimpan dulu ke storage lokal, lalu di-upload ke server
// secara berkala (uploadIntervalMinutes) / manual (SYNC_NOW via BLE).
// Yang belum ter-upload ditandai synced=false dan akan di-retry saat sync.
// ────────────────────────────────────────────────────────────────────
#define ATTENDANCE_LOG "/attendance.json"
#define ATTENDANCE_LOG_MAX 200
#define PENDING_REGISTER_FILE "/pending_register.json"
#define PENDING_REGISTER_MAX 20

struct PendingAttendance {
  char employeeId[40];
  char nama[32];
  char tanggal[12];   // YYYY-MM-DD
  char jam[10];       // HH:MM:SS
  bool synced;
};
PendingAttendance pendingAtt[ATTENDANCE_LOG_MAX];
int pendingAttCount = 0;

// Mutex: pendingAtt/pendingReg diakses dari attnWorker, syncWorker, loop,
// dan task BLE — LittleFS tidak aman diakses bersamaan antar task.
SemaphoreHandle_t pendingMutex = nullptr;

static inline bool pendingLock() {
  return pendingMutex ? xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50)) == pdTRUE : true;
}
static inline void pendingUnlock() {
  if (pendingMutex) xSemaphoreGive(pendingMutex);
}

struct PendingRegister {
  char employeeId[40];
  uint8_t fingerId;
  char hex[512];
};
PendingRegister pendingReg[PENDING_REGISTER_MAX];
int pendingRegCount = 0;

void pendingAttLoad() {
  pendingLock();
  pendingAttCount = 0;
  if (!storageReady) { pendingUnlock(); return; }
  File f = LittleFS.open(ATTENDANCE_LOG, "r");
  if (!f) { pendingUnlock(); return; }
  DynamicJsonDocument doc(ATTENDANCE_LOG_MAX * 120);
  if (deserializeJson(doc, f)) { f.close(); pendingUnlock(); return; }
  f.close();
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject o : arr) {
    if (pendingAttCount >= ATTENDANCE_LOG_MAX) break;
    PendingAttendance &p = pendingAtt[pendingAttCount];
    memset(&p, 0, sizeof(p));
    strncpy(p.employeeId, o["employeeId"] | "", sizeof(p.employeeId) - 1);
    strncpy(p.nama, o["nama"] | "", sizeof(p.nama) - 1);
    strncpy(p.tanggal, o["tanggal"] | "", sizeof(p.tanggal) - 1);
    strncpy(p.jam, o["jam"] | "", sizeof(p.jam) - 1);
    p.synced = o["synced"] | false;
    pendingAttCount++;
  }
  pendingUnlock();
}

void pendingAttSave() {
  if (!storageReady) return;
  pendingLock();
  DynamicJsonDocument doc(ATTENDANCE_LOG_MAX * 120);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < pendingAttCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["employeeId"] = pendingAtt[i].employeeId;
    o["nama"] = pendingAtt[i].nama;
    o["tanggal"] = pendingAtt[i].tanggal;
    o["jam"] = pendingAtt[i].jam;
    o["synced"] = pendingAtt[i].synced;
  }
  File f = LittleFS.open(ATTENDANCE_LOG, "w");
  if (!f) { pendingUnlock(); return; }
  serializeJson(doc, f);
  f.close();
  pendingUnlock();
}

void pendingRegLoad() {
  pendingLock();
  pendingRegCount = 0;
  if (!storageReady) { pendingUnlock(); return; }
  File f = LittleFS.open(PENDING_REGISTER_FILE, "r");
  if (!f) { pendingUnlock(); return; }
  DynamicJsonDocument doc(PENDING_REGISTER_MAX * 600);
  if (deserializeJson(doc, f)) { f.close(); pendingUnlock(); return; }
  f.close();
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject o : arr) {
    if (pendingRegCount >= PENDING_REGISTER_MAX) break;
    PendingRegister &p = pendingReg[pendingRegCount];
    memset(&p, 0, sizeof(p));
    strncpy(p.employeeId, o["employeeId"] | "", sizeof(p.employeeId) - 1);
    p.fingerId = (uint8_t)(o["fingerId"] | 0);
    strncpy(p.hex, o["hex"] | "", sizeof(p.hex) - 1);
    pendingRegCount++;
  }
  pendingUnlock();
}

void pendingRegSave() {
  if (!storageReady) return;
  pendingLock();
  DynamicJsonDocument doc(PENDING_REGISTER_MAX * 600);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < pendingRegCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["employeeId"] = pendingReg[i].employeeId;
    o["fingerId"] = pendingReg[i].fingerId;
    o["hex"] = pendingReg[i].hex;
  }
  File f = LittleFS.open(PENDING_REGISTER_FILE, "w");
  if (!f) { pendingUnlock(); return; }
  serializeJson(doc, f);
  f.close();
  pendingUnlock();
}

// Tambah catatan absensi (selalu, walau offline). Return index baru.
int pendingAttAdd(const char *employeeId, const char *nama) {
  if (!employeeId || !employeeId[0]) return -1;
  pendingLock();
  if (pendingAttCount >= ATTENDANCE_LOG_MAX) {
    memmove(&pendingAtt[0], &pendingAtt[1], (ATTENDANCE_LOG_MAX - 1) * sizeof(PendingAttendance));
    pendingAttCount--;
  }
  int idx = pendingAttCount;
  PendingAttendance &p = pendingAtt[idx];
  memset(&p, 0, sizeof(p));
  strncpy(p.employeeId, employeeId, sizeof(p.employeeId) - 1);
  strncpy(p.nama, nama ? nama : "", sizeof(p.nama) - 1);
  time_t et = (time_t)timeClient.getEpochTime();
  struct tm *ti = localtime(&et);
  if (ti) {
    snprintf(p.tanggal, sizeof(p.tanggal), "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
    snprintf(p.jam, sizeof(p.jam), "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
  } else {
    strncpy(p.tanggal, "0000-00-00", sizeof(p.tanggal) - 1);
    strncpy(p.jam, "00:00:00", sizeof(p.jam) - 1);
  }
  p.synced = false;
  pendingAttCount++;
  pendingUnlock();
  pendingAttSave();
  return idx;
}

// Tambah register finger yang belum ter-upload server (offline). Return true jika masuk antrean.
bool pendingRegAdd(const char *employeeId, uint8_t fingerId, const char *hex) {
  if (!employeeId || !employeeId[0]) return false;
  pendingLock();
  // dedup: update jika sudah ada employeeId+fingerId
  for (int i = 0; i < pendingRegCount; i++) {
    if (strcmp(pendingReg[i].employeeId, employeeId) == 0 && pendingReg[i].fingerId == fingerId) {
      if (hex && hex[0]) strncpy(pendingReg[i].hex, hex, sizeof(pendingReg[i].hex) - 1);
      pendingUnlock();
      pendingRegSave();
      return true;
    }
  }
  if (pendingRegCount >= PENDING_REGISTER_MAX) { pendingUnlock(); return false; }
  PendingRegister &p = pendingReg[pendingRegCount++];
  memset(&p, 0, sizeof(p));
  strncpy(p.employeeId, employeeId, sizeof(p.employeeId) - 1);
  p.fingerId = fingerId;
  if (hex && hex[0]) strncpy(p.hex, hex, sizeof(p.hex) - 1);
  pendingUnlock();
  pendingRegSave();
  return true;
}

bool pendingRegRemove(int idx) {
  pendingLock();
  if (idx < 0 || idx >= pendingRegCount) { pendingUnlock(); return false; }
  for (int i = idx; i < pendingRegCount - 1; i++) pendingReg[i] = pendingReg[i + 1];
  pendingRegCount--;
  pendingUnlock();
  pendingRegSave();
  return true;
}

// ────────────────────────────────────────────────────────────────────
//  Backend API - POST attendance on fingerprint match
// ────────────────────────────────────────────────────────────────────
// Upload absensi di task terpisah.
// ATURAN KERAS: gagal koneksi/HTTP → tampil "error" di LCD. JANGAN reboot.
// ────────────────────────────────────────────────────────────────────
struct AttnJob {
  char employeeId[40];
  char timeBuf[24];
  int pendingIdx;   // index di pendingAtt (-1 jika tidak ada)
};
static QueueHandle_t attnQueue = nullptr;
static volatile bool attnResultPending = false;
static char attnResultStatus[24] = "error";
static volatile bool attnUploading = false;
static unsigned long attnUiClearAt = 0;
static unsigned long attnSendingSince = 0;  // 0 = tidak sedang "MENGIRIM"
static char attnLastEmpId[40] = "";
static unsigned long attnLastQueuedAt = 0;
static WiFiClient attnClient;  // static — jangan WiFiClient di stack task (bisa panic)

#define ATTN_DEDUP_MS 4000u
#define ATTN_MIN_HEAP 26000u
#define ATTN_RESULT_HOLD_MS 1100u
#define ATTN_FAIL_HOLD_MS 1200u
#define ATTN_HTTP_TIMEOUT_MS 3000u
#define ATTN_SENDING_TIMEOUT_MS 4500u  // UI: MENGIRIM max segini → GAGAL (tanpa reboot)

static void attnPublishResult(const char *st) {
  strncpy(attnResultStatus, st && st[0] ? st : "error", sizeof(attnResultStatus) - 1);
  attnResultStatus[sizeof(attnResultStatus) - 1] = 0;
  attnResultPending = true;
}

// Parse http://host[:port]/path → buffer C (tanpa String besar).
static bool attnParseUrl(const char *full, char *host, size_t hostCap,
                         uint16_t *port, char *path, size_t pathCap) {
  if (!full || !host || !port || !path || hostCap < 2 || pathCap < 2) return false;
  host[0] = 0;
  path[0] = '/';
  path[1] = 0;
  *port = 80;
  const char *p = full;
  if (strncmp(p, "https://", 8) == 0) p += 8;  // tetap pakai port 80 (http only)
  else if (strncmp(p, "http://", 7) == 0) p += 7;
  else return false;
  const char *slash = strchr(p, '/');
  size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
  if (hlen == 0 || hlen >= hostCap) return false;
  memcpy(host, p, hlen);
  host[hlen] = 0;
  if (slash) {
    strncpy(path, slash, pathCap - 1);
    path[pathCap - 1] = 0;
  }
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = 0;
    int pr = atoi(colon + 1);
    if (pr > 0 && pr < 65536) *port = (uint16_t)pr;
  }
  return host[0] != 0;
}

// Baca 1 baris header (tanpa String) — timeout total di loop luar.
static bool attnReadLine(WiFiClient &c, char *buf, size_t cap, unsigned long deadline) {
  size_t n = 0;
  while (millis() < deadline && n + 1 < cap) {
    if (!c.available()) {
      if (!c.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int ch = c.read();
    if (ch < 0) break;
    if (ch == '\r') continue;
    if (ch == '\n') {
      buf[n] = 0;
      return true;
    }
    buf[n++] = (char)ch;
  }
  buf[n] = 0;
  return n > 0;
}

// Return: true + status server di outStatus, atau false (gagal jaringan/HTTP).
// TIDAK BOLEH melempar/abort — semua error return false.
static bool postAttendanceSafe(const char *employeeId, const char *timeIso,
                               char *outStatus, size_t outCap) {
  if (outStatus && outCap) {
    strncpy(outStatus, "error", outCap - 1);
    outStatus[outCap - 1] = 0;
  }
  if (!appSettings.apiBaseUrl[0] || !employeeId || !employeeId[0]) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ESP.getFreeHeap() < ATTN_MIN_HEAP) {
    Serial.printf("[API] attn skip low heap=%u\n", ESP.getFreeHeap());
    return false;
  }

  // Paksa http:// — HTTPS di ESP32+BLE sering panic/reboot.
  char urlNorm[192];
  if (strncmp(appSettings.apiBaseUrl, "https://", 8) == 0) {
    snprintf(urlNorm, sizeof(urlNorm), "http://%s/api/finger/arduino/attendance",
             appSettings.apiBaseUrl + 8);
  } else if (strncmp(appSettings.apiBaseUrl, "http://", 7) == 0) {
    snprintf(urlNorm, sizeof(urlNorm), "%s/api/finger/arduino/attendance", appSettings.apiBaseUrl);
  } else {
    snprintf(urlNorm, sizeof(urlNorm), "http://%s/api/finger/arduino/attendance",
             appSettings.apiBaseUrl);
  }

  char host[80], path[120];
  uint16_t port = 80;
  if (!attnParseUrl(urlNorm, host, sizeof(host), &port, path, sizeof(path))) {
    Serial.println("[API] attn bad url");
    return false;
  }

  char timeField[40] = "";
  if (timeIso && timeIso[0]) {
    strncpy(timeField, timeIso, sizeof(timeField) - 1);
  } else {
    snprintf(timeField, sizeof(timeField), "%s", "1970-01-01T00:00:00");
  }

  char body[220];
  snprintf(body, sizeof(body),
           "{\"employeeId\":\"%.36s\",\"device_id\":\"%.30s\",\"kode_cabang\":\"%.14s\",\"time\":\"%.36s\"}",
           employeeId, appSettings.deviceId, appSettings.kodeCabang, timeField);
  size_t bodyLen = strlen(body);

  if (!httpsLock(1500)) {
    Serial.println("[API] attn lock busy");
    return false;
  }

  bool ok = false;
  int httpCode = -1;
  char respBuf[280];
  respBuf[0] = 0;

  // Pastikan socket lama tertutup sebelum connect baru.
  if (attnClient.connected()) attnClient.stop();
  vTaskDelay(pdMS_TO_TICKS(20));

  Serial.printf("[API] attn DNS/connect %s:%u heap=%u\n", host, port, ESP.getFreeHeap());

  IPAddress ip;
  // hostByName bisa gagal — jangan biarkan hang tanpa batas.
  unsigned long dnsT0 = millis();
  bool dnsOk = WiFi.hostByName(host, ip);
  if (!dnsOk || (millis() - dnsT0) > 2500) {
    Serial.println("[API] attn DNS FAIL");
    httpsUnlock();
    return false;
  }

  attnClient.setTimeout(2);  // detik untuk read ops
  if (!attnClient.connect(ip, port)) {
    Serial.println("[API] attn connect FAIL");
    attnClient.stop();
    httpsUnlock();
    return false;
  }

  // Tulis request — cek write gagal.
  int w = 0;
  w += attnClient.printf("POST %s HTTP/1.0\r\n", path);
  w += attnClient.printf("Host: %s\r\n", host);
  w += attnClient.print("Content-Type: application/json\r\n");
  w += attnClient.printf("Content-Length: %u\r\n", (unsigned)bodyLen);
  if (appSettings.apiKey[0]) {
    w += attnClient.printf("X-Device-Key: %s\r\n", appSettings.apiKey);
  }
  w += attnClient.print("Connection: close\r\n\r\n");
  w += attnClient.print(body);
  if (w <= 0) {
    Serial.println("[API] attn write FAIL");
    attnClient.stop();
    httpsUnlock();
    return false;
  }

  unsigned long deadline = millis() + ATTN_HTTP_TIMEOUT_MS;
  bool headersDone = false;
  size_t respLen = 0;
  char line[160];

  while (millis() < deadline) {
    if (!headersDone) {
      if (!attnClient.available() && !attnClient.connected()) break;
      if (!attnClient.available()) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      if (!attnReadLine(attnClient, line, sizeof(line), deadline)) break;
      if (httpCode < 0 && strncmp(line, "HTTP/", 5) == 0) {
        const char *sp = strchr(line, ' ');
        if (sp) httpCode = atoi(sp + 1);
      }
      if (line[0] == 0) headersDone = true;
      continue;
    }
    while (attnClient.available() && respLen + 1 < sizeof(respBuf)) {
      int b = attnClient.read();
      if (b < 0) break;
      respBuf[respLen++] = (char)b;
      respBuf[respLen] = 0;
    }
    if (!attnClient.available() && !attnClient.connected()) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  attnClient.stop();
  httpsUnlock();

  Serial.printf("[API] attn HTTP %d len=%u heap=%u\n",
                httpCode, (unsigned)respLen, ESP.getFreeHeap());

  if (httpCode < 200 || httpCode >= 300 || respLen == 0) return false;

  // Ambil status dari JSON sederhana.
  const char *key = strstr(respBuf, "\"status\"");
  if (key) {
    const char *q1 = strchr(key + 8, '"');
    if (q1) {
      const char *q2 = strchr(q1 + 1, '"');
      if (q2 && q2 > q1 + 1 && outStatus && outCap > 1) {
        size_t n = (size_t)(q2 - (q1 + 1));
        if (n >= outCap) n = outCap - 1;
        memcpy(outStatus, q1 + 1, n);
        outStatus[n] = 0;
        ok = true;
      }
    }
  }
  if (!ok && outStatus && outCap) {
    strncpy(outStatus, "ok", outCap - 1);
    ok = true;
  }
  return ok;
}

bool attnEnqueue(const char *employeeId, int pendingIdx) {
  if (!attnQueue || !employeeId || !employeeId[0]) return false;
  if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) return false;
  if (ESP.getFreeHeap() < ATTN_MIN_HEAP) return false;
  if (attnUploading || uxQueueMessagesWaiting(attnQueue) > 0) {
    Serial.println("[API] attendance skip: busy");
    return false;
  }
  if (attnLastEmpId[0] && strcmp(attnLastEmpId, employeeId) == 0 &&
      (millis() - attnLastQueuedAt) < ATTN_DEDUP_MS) {
    // Duplikat 4s → tandai catatan lokal sebagai synced (sudah tercatat),
    // supaya sync berkala TIDAK re-upload dan bikin absensi ganda di server.
    if (pendingIdx >= 0 && pendingIdx < pendingAttCount) {
      pendingAtt[pendingIdx].synced = true;
      pendingAttSave();
    }
    attnPublishResult("ignored");
    return true;
  }

  AttnJob job;
  memset(&job, 0, sizeof(job));
  strncpy(job.employeeId, employeeId, sizeof(job.employeeId) - 1);
  job.pendingIdx = pendingIdx;
  // waktu dari caller worker jika kosong — isi di sini
  String ntpTime = timeClient.getFormattedTime();
  unsigned long epoch = timeClient.getEpochTime();
  time_t et = (time_t)epoch;
  struct tm *ti = localtime(&et);
  if (ti) {
    snprintf(job.timeBuf, sizeof(job.timeBuf), "%04d-%02d-%02dT%s",
             ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ntpTime.c_str());
  }

  if (xQueueSend(attnQueue, &job, 0) != pdTRUE) return false;
  strncpy(attnLastEmpId, employeeId, sizeof(attnLastEmpId) - 1);
  attnLastEmpId[sizeof(attnLastEmpId) - 1] = 0;
  attnLastQueuedAt = millis();
  Serial.printf("[API] attendance queued %s\n", employeeId);
  return true;
}

void attnWorker(void *param) {
  (void)param;
  AttnJob job;
  for (;;) {
    if (xQueueReceive(attnQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    attnUploading = true;
    Serial.printf("[API] attendance upload %s...\n", job.employeeId);
    vTaskDelay(pdMS_TO_TICKS(100));

    char st[24] = "error";
    bool ok = postAttendanceSafe(job.employeeId, job.timeBuf, st, sizeof(st));
    if (!ok) {
      strncpy(st, "error", sizeof(st) - 1);
      Serial.println("[API] attendance FAIL -> show error (no reboot)");
    } else {
      Serial.printf("[API] attendance OK status=%s\n", st);
    }

    // Update catatan lokal: sukses → synced=true (tidak di-retry).
    // Gagal → tetap pending (di-retry sync berkala/manual).
    if (job.pendingIdx >= 0 && job.pendingIdx < pendingAttCount) {
      PendingAttendance &p = pendingAtt[job.pendingIdx];
      p.synced = ok;
      pendingAttSave();
    }

    // SELALU publish hasil — sukses maupun gagal. Jangan biarkan UI stuck MENGIRIM.
    attnPublishResult(st);
    attnUploading = false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void attnServiceUi() {
  // Timeout MENGIRIM: tampilkan GAGAL, jangan reboot.
  if (attnSendingSince && (millis() - attnSendingSince) > ATTN_SENDING_TIMEOUT_MS &&
      !attnResultPending) {
    Serial.println("[API] sending UI timeout -> GAGAL KIRIM");
    attnSendingSince = 0;
    lcdShowAttendanceStatus("error");
    attnUiClearAt = millis() + ATTN_FAIL_HOLD_MS;
    // Jangan paksa attnUploading=false jika task masih jalan — biarkan selesai sendiri.
  }

  if (attnResultPending) {
    noInterrupts();
    attnResultPending = false;
    char st[24];
    strncpy(st, attnResultStatus, sizeof(st) - 1);
    st[sizeof(st) - 1] = 0;
    interrupts();

    attnSendingSince = 0;
    lcdShowAttendanceStatus(st);
    ledcWrite(LCD_BL, 120);
    bool fail = (strcmp(st, "error") == 0 || strcmp(st, "offline") == 0 ||
                 strcmp(st, "not_found") == 0);
    attnUiClearAt = millis() + (fail ? ATTN_FAIL_HOLD_MS : ATTN_RESULT_HOLD_MS);
    if (!fail) emit(F("{\"event\":\"attendance\",\"ok\":true,\"status\":\"%s\"}"), st);
    else emit(F("{\"event\":\"attendance\",\"ok\":false,\"status\":\"%s\"}"), st);
  }

  if (attnUiClearAt && millis() >= attnUiClearAt) {
    attnUiClearAt = 0;
    if (!enrollActive && scanState == SCAN_IDLE) lcdShowIdle();
  }
}

void attnInit() {
  if (!httpsMutex) httpsMutex = xSemaphoreCreateMutex();
  if (attnQueue) return;
  attnQueue = xQueueCreate(1, sizeof(AttnJob));
  xTaskCreatePinnedToCore(attnWorker, "attnHttp", 12288, nullptr, 1, nullptr, 0);
  Serial.println("[API] attendance upload task ready");
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

  // JsonDocument (ArduinoJson 7) tumbuh di heap — jangan pakai pool 1536:
  // template 256 byte = 512 hex, kalau pool penuh templateHex di-drop diam-diam
  // dan server menyimpan marker ON_DEVICE (tidak bisa di-sync).
  JsonDocument body;
  body["employeeId"] = employeeId;
  body["device_id"] = appSettings.deviceId;
  body["kode_cabang"] = appSettings.kodeCabang;
  body["finger_id"] = fingerId;
  size_t hexLen = (templateHex && templateHex[0]) ? strlen(templateHex) : 0;
  if (hexLen) body["templateHex"] = templateHex;
  else logError("register without templateHex employee=%s", employeeId);

  String json;
  serializeJson(body, json);
  if (hexLen && json.indexOf("templateHex") < 0) {
    logError("register JSON omitted templateHex (len=%u)", (unsigned)hexLen);
    return "";
  }
  Serial.printf("[API] register hex_len=%u json_len=%u\n", (unsigned)hexLen, (unsigned)json.length());

  WiFiClient *client = nullptr;
  HTTPClient http;
  if (!httpsLock(15000)) {
    logError("register skipped: https busy");
    return "";
  }
  if (!apiHttpBegin(http, client, url, 25000)) {
    httpsUnlock();
    return "";
  }
  http.addHeader("Content-Type", "application/json");
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  int code = http.POST(json);
  String resp = "";
  if (code > 0) {
    resp = http.getString();
    Serial.printf("[API] register HTTP %d: %s\n", code, resp.c_str());
    if (code < 200 || code >= 300) logError("register HTTP status=%d", code);
  } else {
    apiHttpLogError("register", http, code);
  }
  http.end();
  httpsUnlock();
  return resp;
}

// ────────────────────────────────────────────────────────────────────
//  Sync worker — flush pending register + attendance belum ter-upload
// ────────────────────────────────────────────────────────────────────
// Dipicu oleh: timer berkala (uploadIntervalMinutes) & perintah SYNC_NOW
// (BLE). Diproses di task terpisah supaya tidak memblokir loop utama.
static QueueHandle_t syncQueue = nullptr;
static volatile bool syncBusy = false;
static volatile bool syncRequested = false;

// Tandai ada pekerjaan sync (aman dipanggil dari task BLE / loop).
void syncRequestNow() {
  syncRequested = true;
  if (syncQueue) {
    uint8_t dummy = 1;
    xQueueSend(syncQueue, &dummy, 0);
  }
}

void syncWorker(void *param) {
  (void)param;
  for (;;) {
    uint8_t dummy;
    if (syncQueue && xQueueReceive(syncQueue, &dummy, portMAX_DELAY) != pdTRUE) continue;
    if (!syncRequested) continue;
    syncRequested = false;
    if (syncBusy) continue;
    syncBusy = true;
    Serial.println("[SYNC] worker start");

    if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) {
      Serial.println("[SYNC] offline — tetap simpan pending");
      syncBusy = false;
      continue;
    }

    // 1) Pending register (enroll yang belum ter-upload)
    for (int i = 0; i < pendingRegCount && !enrollActive && !restoreActive; i++) {
      PendingRegister &r = pendingReg[i];
      if (!r.employeeId[0]) continue;
      Serial.printf("[SYNC] register pending %s fid=%u\n", r.employeeId, r.fingerId);
      String resp = postRegister(r.employeeId, r.fingerId, r.hex[0] ? r.hex : "");
      bool ok = resp.length() > 0 && (resp.indexOf("\"status\":\"ok\"") >= 0 ||
                                      resp.indexOf("\"status\":\"updated\"") >= 0);
      if (ok) {
        Serial.printf("[SYNC] register OK, remove pending %s\n", r.employeeId);
        pendingRegRemove(i);
        i--;
        bleUpdateStatus();
      } else {
        Serial.printf("[SYNC] register fail — keep pending %s\n", r.employeeId);
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 2) Pending attendance (synced=false)
    if (!enrollActive && !restoreActive) {
      for (int i = 0; i < pendingAttCount; i++) {
        PendingAttendance &p = pendingAtt[i];
        if (p.synced || !p.employeeId[0]) continue;
        char timeIso[32];
        snprintf(timeIso, sizeof(timeIso), "%sT%s", p.tanggal, p.jam);
        Serial.printf("[SYNC] attendance pending %s @%s\n", p.employeeId, timeIso);
        char st[24] = "error";
        bool ok = postAttendanceSafe(p.employeeId, timeIso, st, sizeof(st));
        if (ok) {
          p.synced = true;
          pendingAttSave();
          Serial.printf("[SYNC] attendance OK status=%s\n", st);
        } else {
          Serial.printf("[SYNC] attendance fail — keep pending %s\n", p.employeeId);
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }

    Serial.println("[SYNC] worker done");
    bleUpdateStatus();
    syncBusy = false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void syncInit() {
  if (!httpsMutex) httpsMutex = xSemaphoreCreateMutex();
  if (!pendingMutex) pendingMutex = xSemaphoreCreateMutex();
  if (syncQueue) return;
  syncQueue = xQueueCreate(1, sizeof(uint8_t));
  xTaskCreatePinnedToCore(syncWorker, "syncHttp", 12288, nullptr, 1, nullptr, 0);
  Serial.println("[SYNC] pending sync worker ready");
}

// Bangun JSON riwayat absensi lokal untuk BLE history char (4fafc208).
void bleUpdateHistory() {
  if (!pHistoryChar) return;
  DynamicJsonDocument doc(ATTENDANCE_LOG_MAX * 120);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < pendingAttCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["employeeId"] = pendingAtt[i].employeeId;
    o["nama"] = pendingAtt[i].nama;
    o["tanggal"] = pendingAtt[i].tanggal;
    o["jam"] = pendingAtt[i].jam;
    o["synced"] = pendingAtt[i].synced;
  }
  String out;
  serializeJson(doc, out);
  pHistoryChar->setValue(out.c_str());
}

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

void wifiDnsStart() {
  if (dnsServerActive) return;
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  dnsServerActive = true;
  Serial.printf("[WiFi] Captive DNS ON → %s\n", WiFi.softAPIP().toString().c_str());
}

void wifiDnsStop() {
  if (!dnsServerActive) return;
  dnsServer.stop();
  dnsServerActive = false;
  Serial.println("[WiFi] Captive DNS OFF");
}

// Tetap layani web/DNS saat operasi blocking (reinit sensor, dll) supaya AP
// tidak "mati" dari sisi HP.
void wifiServicePump() {
  if (dnsServerActive) dnsServer.processNextRequest();
  server.handleClient();
  yield();
}

void pumpDelay(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) {
    wifiServicePump();
    delay(1);
  }
}

// Pastikan AP setup hidup TANPA restart softAP (restart = putus client).
void wifiEnsureApAlive() {
  if (!wifiApSetupMode || !appSettings.apEnabled) return;
  wifi_mode_t m = WiFi.getMode();
  if (m != WIFI_AP_STA && m != WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
    pumpDelay(80);
  }
  if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.setSleep(false);
    WiFi.softAP(AP_SSID, cred.apPass);
    pumpDelay(50);
    wifiDnsStart();
    Serial.println("[WiFi] AP softAP restored (IP was 0)");
  }
}

// Idle setup tanpa softAP — konfigurasi lewat BLE.
void wifiEnterBleOnly(const char *reason) {
  Serial.printf("[WiFi] BLE-only setup (%s) — softAP OFF\n", reason ? reason : "-");
  wifiConnected = false;
  wifiApSetupMode = true;
  staIP = "";
  wifiDnsStop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  delay(80);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  wifiApplyPowerPolicy();
}

// Mode AP setup — AP hidup untuk web UI, STA idle (tanpa WiFi.begin).
// Pakai WIFI_AP_STA (bukan WIFI_AP murni) supaya scan jaringan bisa jalan.
void wifiEnterApOnly(const char *reason) {
  if (!appSettings.apEnabled) {
    wifiEnterBleOnly(reason);
    return;
  }
  Serial.printf("[WiFi] AP-setup mode (%s)\n", reason ? reason : "-");
  wifiConnected = false;
  wifiApSetupMode = true;
  staIP = "";
  // Putus usaha STA tanpa hapus cred tersimpan
  WiFi.disconnect(true);
  delay(80);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.setSleep(false);
  // softAP wajib setelah ganti mode — kalau tidak, client HP putus / scan gagal
  WiFi.softAP(AP_SSID, cred.apPass);
  delay(50);
  wifiApplyPowerPolicy();
  wifiDnsStart();
  Serial.printf("[WiFi] AP: %s | IP %s — STA idle, siap scan/setup\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

void wifiMarkStaConnected(const char *ssid) {
  wifiConnected = true;
  wifiApSetupMode = false;
  wifiStaEverOk = true;
  staIP = WiFi.localIP().toString();
  if (ssid && ssid[0]) staSSID = String(ssid);
  wifiDnsStop();
  // Mode server = STA di LAN WiFi. Matikan softAP supaya tidak campur portal.
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  wifiApplyPowerPolicy();
  Serial.printf("[WiFi] STA server mode SSID=%s IP=%s (AP off)\n",
                staSSID.c_str(), staIP.c_str());
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
  // Setelah WiFi ops: cooldown singkat saja (dulu 5s — terasa “mati” setelah scan/BLE).
  scanCooldownUntil = millis() + 800;
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
  WiFi.setSleep(false);

  // BLE-first: jangan nyalakan softAP kecuali ap_enabled=true.
  if (!appSettings.apEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.softAPdisconnect(true);
    Serial.println("[WiFi] softAP disabled (BLE setup mode)");
  } else {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, cred.apPass);
    Serial.printf("[WiFi] AP: %s | AP IP: 192.168.4.1\n", AP_SSID);
  }

  if (savedWiFiCount == 0) {
    Serial.println("[WiFi] No saved credentials — setup mode");
    wifiEnterApOnly("no-credentials");
    return;
  }

  if (appSettings.apEnabled && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
  } else if (!appSettings.apEnabled) {
    WiFi.mode(WIFI_STA);
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
      wifiMarkStaConnected(savedWiFi[i].ssid);
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
      Serial.printf("[WiFi] Connected to %s | IP: %s | STA\n", savedWiFi[i].ssid, staIP.c_str());
      return;
    }
    WiFi.disconnect(false);
    delay(100);
  }

  Serial.println("[WiFi] All saved networks failed — setup mode (no background retry storm)");
  logError("WiFi init: all saved networks failed");
  wifiEnterApOnly("boot-connect-failed");
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
  bleEnrollCancelRequested = false;
  if (ledOn) {
    uint8_t ledResult = finger.LEDcontrol(false);
    if (ledResult != FINGERPRINT_OK) {
      logError("LED OFF (enroll cleanup) failed code=%d", ledResult);
    }
    // SELALU reset ke false — walau perintah gagal, gate scan berikutnya
    // harus bisa mencoba LEDcontrol(true) lagi. ledOn=true yang dipertahankan
    // membuat scan tidak pernah menyalakan LED lagi (bug LED stuck).
    ledOn = false;
  }
  ledOnSince = 0;
  flushRX();
  delay(50);
  lastScanActivity = millis();
  lastRecoveryAttempt = millis();
  lastLcdActivity = millis();
  recoveryCount = 0;
  consecutiveErrors = 0;
  scanState = SCAN_IDLE;
  // Langsung siap scan — jangan cooldown panjang setelah batal/selesai enroll
  scanCooldownUntil = millis() + 200;
  fingerDown = false;
  autoScan = true;
  enrollActive = false;
  fingerMustRelease = false;
  // Reset gate supaya jari berikutnya langsung bisa scan
  irGateOpen = false;
  irDetSince = irClearSince = 0;
  irGateCooldownUntil = 0;
  if (lcdBacklightOn) ledcWrite(LCD_BL, 255);
  lcdShowIdle();
  bleUpdateStatus();
}

// Batalkan enroll aktif (dari BLE). Return true jika dibatalkan.
static bool enrollAbortIfCancelled(const char *where) {
  if (!bleEnrollCancelRequested) return false;
  bleEnrollCancelRequested = false;
  Serial.printf("[ENROLL] cancelled at %s — resume autoscan\n", where ? where : "?");
  lcdEnrollErr("DIBATALKAN");
  emit(F("{\"event\":\"enroll_cancelled\"}"));
  enrollCleanupResumeScan();
  return true;
}

uint8_t enrollFinger(uint8_t id, const char *name, const char *empId) {
  int p = -1;
  if (bleEnrollCancelRequested) {
    bleEnrollCancelRequested = false;
    emit(F("{\"event\":\"enroll_cancelled\"}"));
    enrollCleanupResumeScan();
    return 0xFD;
  }
  enrollActive = true;
  autoScan = false;
  lastScanActivity = millis();
  // Mode enroll: LCD + LED FPM10A menyala terus sampai 2 scan selesai / batal.
  enrollKeepAliveUi();
  flushRX();

  // id=0 → auto-assign slot kosong (dipakai jalur BLE). Harus dialokasikan
  // sebelum waitNoFinger supaya storeModel(id) tidak menulis ke slot 0.
  if (id == 0) {
    id = nextFreeFingerId();
    if (id == 0) {
      logError("enroll auto-assign failed: full");
      enrollCleanupResumeScan();
      return 0xFE;
    }
  }

  lcdShowEnrollTitle(id);
  lcdEnrollStep("Remove finger", -1, "Clear sensor first", COL_TEXT);
  emit(F("{\"event\":\"enroll_start\",\"id\":%d}"), id);
  enrollKeepAliveUi();
  if (!waitNoFinger()) {
    if (enrollAbortIfCancelled("wait_clear")) return 0xFD;
    logError("enroll timeout waiting for no finger");
    lcdEnrollErr("TIMEOUT");
    emit(F("{\"event\":\"enroll_fail\",\"code\":-1}"));
    enrollCleanupResumeScan();
    return 0xFE;
  }

  enrollLedKeepOn();
  delay(200);

  for (int attempt = 0; attempt < 3; attempt++) {
    if (enrollAbortIfCancelled("loop")) return 0xFD;
    lastScanActivity = millis();
    lcdEnrollStep("Place finger", 10, "Touch sensor gently", COL_WARN);
    emit(F("{\"event\":\"waiting_finger\"}"));
    enrollKeepAliveUi();
    if (!waitFinger()) {
      if (enrollAbortIfCancelled("finger1")) return 0xFD;
      logError("enroll timeout waiting for finger step=1");
      lcdEnrollErr("TIMEOUT");
      emit(F("{\"event\":\"enroll_fail\",\"code\":-1}"));
      enrollCleanupResumeScan();
      return 0xFE;
    }

    lcdEnrollStep("Capturing...", 25, "Reading fingerprint", COL_ACCENT);
    enrollKeepAliveUi();
    flushRX(); delay(200);
    // image2Tz/fingerSearch/createModel/storeModel bisa blocking >5s saat
    // sensor lambat/macet → task WDT loop = reboot. Sama seperti scan path.
    disableLoopWDT();
    p = finger.image2Tz(1);
    enableLoopWDT();
    esp_task_wdt_reset();
    if (p != FINGERPRINT_OK) {
      logError("enroll image2Tz step=1 failed code=%d", p);
      lcdEnrollErr("Bad Image #1");
      emit(F("{\"event\":\"bad_image\",\"step\":1,\"code\":%d}"), p);
      if (!waitNoFinger()) {
        if (enrollAbortIfCancelled("bad1")) return 0xFD;
        lcdEnrollErr("TIMEOUT");
        enrollCleanupResumeScan();
        return 0xFE;
      }
      continue;
    }
    lcdEnrollStep("Step 1 OK", 40, "First scan captured", COL_OK);
    emit(F("{\"event\":\"image_ok_step1\"}"));

    disableLoopWDT();
    p = finger.fingerSearch();
    enableLoopWDT();
    esp_task_wdt_reset();
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
    enrollKeepAliveUi();
    if (!waitNoFinger()) {
      if (enrollAbortIfCancelled("remove1")) return 0xFD;
      logError("enroll timeout removing finger step=1");
      lcdEnrollErr("TIMEOUT");
      enrollCleanupResumeScan();
      return 0xFE;
    }

    lcdEnrollStep("Place again", 60, "Same finger, same spot", COL_WARN);
    emit(F("{\"event\":\"waiting_finger_2\"}"));
    enrollKeepAliveUi();
    delay(LED_WARMUP_MS);
    if (!waitFinger()) {
      if (enrollAbortIfCancelled("finger2")) return 0xFD;
      logError("enroll timeout waiting for finger step=2");
      lcdEnrollErr("TIMEOUT");
      emit(F("{\"event\":\"enroll_fail\",\"code\":-1}"));
      enrollCleanupResumeScan();
      return 0xFE;
    }

    lcdEnrollStep("Capturing...", 75, "Reading fingerprint", COL_ACCENT);
    enrollKeepAliveUi();
    flushRX(); delay(200);
    disableLoopWDT();
    p = finger.image2Tz(2);
    enableLoopWDT();
    esp_task_wdt_reset();
    if (p != FINGERPRINT_OK) {
      logError("enroll image2Tz step=2 failed code=%d", p);
      lcdEnrollErr("Bad Image #2");
      emit(F("{\"event\":\"bad_image\",\"step\":2,\"code\":%d}"), p);
      if (!waitNoFinger()) {
        if (enrollAbortIfCancelled("bad2")) return 0xFD;
        lcdEnrollErr("TIMEOUT");
        enrollCleanupResumeScan();
        return 0xFE;
      }
      continue;
    }
    lcdEnrollStep("Step 2 OK", 85, "Second scan captured", COL_OK);
    emit(F("{\"event\":\"image_ok_step2\"}"));

    lcdEnrollStep("Building model...", 90, "Matching patterns", COL_ACCENT);
    disableLoopWDT();
    p = finger.createModel();
    enableLoopWDT();
    esp_task_wdt_reset();
    if (p == FINGERPRINT_OK) break;

    logError("enroll createModel failed code=%d attempt=%d", p, attempt + 1);
    char msg[24];
    snprintf(msg, sizeof(msg), "Retry %d/3", attempt + 1);
    lcdEnrollErr(msg);
    emit(F("{\"event\":\"retry_create\",\"attempt\":%d}"), attempt + 1);
    if (!waitNoFinger()) {
      if (enrollAbortIfCancelled("retry")) return 0xFD;
      lcdEnrollErr("TIMEOUT");
      enrollCleanupResumeScan();
      return 0xFE;
    }
    if (attempt == 2) {
      lcdEnrollErr("Model FAILED");
      emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
      enrollCleanupResumeScan();
      return p;
    }
  }

  if (enrollAbortIfCancelled("store")) return 0xFD;

  lcdEnrollStep("Storing...", 95, "Saving to sensor", COL_ACCENT);
  delay(100);
  disableLoopWDT();
  p = finger.storeModel(id);
  enableLoopWDT();
  esp_task_wdt_reset();
  if (p != FINGERPRINT_OK) {
    logError("enroll storeModel failed id=%d code=%d", id, p);
    lcdEnrollErr("Store FAILED");
    emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
    enrollCleanupResumeScan();
    return p;
  }

  dbAdd(id, name, empId);

  // Ambil template hex supaya bisa di-upload ke server (sync ke device lain).
  // SELALU simpan ke antrean pending_register.json — walau offline, akan
  // di-upload saat sync berkala/manual (SYNC_NOW). Jika online langsung upload.
  uint8_t tplBuf[256];
  String hexStr = "";
  if (empId && empId[0]) {
    for (int t = 0; t < 3 && hexStr.length() < 512; t++) {
      delay(t == 0 ? 80 : 200);
      flushRX();
      if (getTemplateRaw(id, tplBuf)) hexStr = toHex(tplBuf, 256);
      Serial.printf("[API] getTemplateRaw try %d hex_len=%u\n", t + 1, (unsigned)hexStr.length());
    }
    if (hexStr.length() < 512) {
      logError("enroll register: template hex unavailable id=%d", id);
    }
    const char *hexC = hexStr.length() >= 512 ? hexStr.c_str() : "";
    bool queued = pendingRegAdd(empId, id, hexC);
    if (queued) {
      Serial.println("[API] register queued pending (sync akan retry)");
    }
    if (wifiConnected) {
      String regResp = postRegister(empId, id, hexC);
      if (regResp.length() > 0) {
        emit(F("{\"event\":\"register_server\",\"ok\":true,\"hex\":%s,\"status\":\"ok\"}"),
             hexStr.length() >= 512 ? "true" : "false");
        Serial.printf("[API] register resp: %s\n", regResp.c_str());
        // sukses → hapus dari antrean pending
        for (int i = 0; i < pendingRegCount; i++) {
          if (strcmp(pendingReg[i].employeeId, empId) == 0 && pendingReg[i].fingerId == id) {
            pendingRegRemove(i);
            break;
          }
        }
      } else {
        emit(F("{\"event\":\"register_server\",\"ok\":false,\"hex\":%s,\"status\":\"error\"}"),
             hexStr.length() >= 512 ? "true" : "false");
      }
    } else {
      emit(F("{\"event\":\"register_server\",\"ok\":false,\"hex\":%s,\"status\":\"offline\"}"),
           hexStr.length() >= 512 ? "true" : "false");
    }
  }

  lcdEnrollStep("DONE", 100, NULL, COL_OK);
  char msg[32];
  snprintf(msg, sizeof(msg), "ID:%d Enrolled", id);
  lcdEnrollOk(msg);
  emit(F("{\"event\":\"enrolled\",\"id\":%d,\"name\":\"%s\"}"), id, jsonEscape(name));
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

void irCalibrate() {
  int hi = 0, lo = 0;
  for (int i = 0; i < 20; i++) {
    if (digitalRead(TOUCH_PIN) == HIGH) hi++; else lo++;
    delay(25); yield();
  }

  // Infer polaritas dari level idle. Boot sampling tanpa jari = stabil:
  //  - idle HIGH (hi=20)  → sensor active-LOW (jari menarik pin ke LOW)
  //  - idle LOW  (lo=20)  → sensor active-HIGH (jari menaikkan pin ke HIGH)
  // Jangan anggap level idle stabil sebagai "sensor rusak" — itu NORMAL.
  // Gate tetap dipakai; tanpa jari gate tertutup (LED mati), saat jari
  // menyentuh pin berubah level → irRawDetected() true → LED nyala.
  irFallbackMode = false;
  irPolarityHigh = (lo > hi);  // idle LOW → finger = HIGH, dan sebaliknya
  irCalibrated = true;
  irGateOpen = false;
  irDetSince = irClearSince = 0;
  irGateCooldownUntil = 0;
  if (hi == 0 || lo == 0) {
    // Sinyal tidak berubah saat boot — tetap pakai gate dengan polaritas
    // ter-infer. Gate terbuka hanya saat level pin berubah (jari disentuh).
    Serial.printf("[TOUCH] stable idle (hi=%d lo=%d) → finger=%s\n", hi, lo,
                  irPolarityHigh ? "HIGH" : "LOW");
  } else {
    Serial.printf("[TOUCH] idle=%s → finger=%s\n", (lo > hi) ? "LOW" : "HIGH",
                  irPolarityHigh ? "HIGH" : "LOW");
  }
}

bool irRawDetected() {
  if (!irCalibrated) return false;  // belum kalibrasi → jangan klaim ada jari
  bool lvl = (digitalRead(TOUCH_PIN) == HIGH);
  return irPolarityHigh ? lvl : !lvl;
}

// ── Gate state machine ────────────────────────────────────────────
const unsigned long IR_CONFIRM_MS = 100;
const unsigned long IR_RELEASE_MS = 400;
const unsigned long IR_GATE_TIMEOUT_MS = 3000;
const unsigned long IR_GATE_COOLDOWN_MS = 2500;
const unsigned long FALLBACK_POLL_MS = 1500;

void irUpdateGate() {
  if (!appSettings.irEnabled) {
    irFallbackMode = false;
    irGateOpen = true;  // IR nonaktif → gate selalu terbuka (perilaku lama)
    irDetSince = irClearSince = 0;
    return;
  }
  if (irFallbackMode) {
    // Jangan pernah membiarkan gate terbuka permanen saat boot tidak ada sinyal.
    // Ini menyebabkan LED FPM10A tetap nyala terus tanpa jari.
    irGateOpen = false;
    irDetSince = 0;
    irClearSince = 0;
    irGateCooldownUntil = 0;
    return;
  }
  bool raw = irRawDetected();
  // Bangunkan LCD segera saat sentuhan mentah — jangan tunggu debounce gate
  // (dulu layar bisa mati total sampai jari >5 detik karena cooldown gate).
  if (raw) {
    lastLcdActivity = millis();
    lcdBacklightOn = true;
    ledcWrite(LCD_BL, 255);
    // Saat ada sentuhan valid, nyalakan LED segera — jangan menunggu gate
    // sepenuhnya terbuka dulu, supaya lampu sensor menyala saat jari disentuh.
    if (!ledOn && sensorReady) {
      uint8_t r = finger.LEDcontrol(true);
      bool ok = (r == FINGERPRINT_OK);
      ledOn = ok;
      ledOnSince = ok ? millis() : 0;
      if (!ok) {
        logError("LED ON (raw touch) failed code=%d", r);
      }
    }
  }
  if (raw) {
    irClearSince = 0;
    if (!irGateOpen) {
      // Jari masih menempel saat cooldown → batalkan cooldown (hindari tunggu >5s).
      if (millis() < irGateCooldownUntil) irGateCooldownUntil = 0;
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
}

bool irGateActive() { return irGateOpen; }

bool irShouldPoll() {
  irUpdateGate();
  return irGateOpen;
  // Fallback & cooldown sudah ditangani langsung di SCAN_IDLE; di sini
  // hanya kembalikan gate state — supaya bisa dipakai sebagai gate scan.
}

void checkAutoSleep() {
  static bool prevSleeping = false;
  if (!timeClient.isTimeSet()) { scanSleeping = false; return; }
  if (!appSettings.scanSchedule) { scanSleeping = false; return; }
  int h = timeClient.getHours();
  uint8_t a = appSettings.scanStartHour;
  uint8_t b = appSettings.scanEndHour;
  bool active;
  if (a == b) {
    active = true;  // start==end → selalu scan
  } else if (a < b) {
    active = (h >= (int)a && h < (int)b);
  } else {
    // wrap midnight, mis. 5→0 = aktif 05–23
    active = (h >= (int)a || h < (int)b);
  }
  scanSleeping = !active;
  // Transisi tidur → bangun: reset lastScanActivity supaya watchdog tidak
  // langsung menganggap sensor mati (lastScanActivity basi sejak sebelum tidur).
  if (prevSleeping && !scanSleeping) {
    lastScanActivity = millis();
    scanCooldownUntil = 0;
    Serial.println("[SLEEP] Wake up — resume scan");
    lcdShowIdle();
  }
  prevSleeping = scanSleeping;
}

static void ledForceOff(const char *reason) {
  uint8_t ledResult = finger.LEDcontrol(false);
  // Selalu reset state internal — walau perintah gagal, jangan mempertahankan
  // ledOn=true yang membuat gate scan tidak pernah menyalakan LED lagi.
  ledOn = false;
  ledOnSince = 0;
  if (ledResult == FINGERPRINT_OK) {
    if (reason) logDebug("LED OFF (%s)", reason);
  } else {
    logError("LED OFF (%s) failed code=%d", reason ? reason : "?", ledResult);
  }
}

void doAutoScan() {
  static int fingerConfirm = 0;  // debounce counter (jangan dihapus — AGENTS.md)
  static const int FINGER_CONFIRM_NEEDED = 1;  // 3V3: sensor lemah, tak sanggup 2x OK berturut

  switch (scanState) {

    case SCAN_IDLE: {
      yield();
      if (scanSleeping) break;
      if (millis() < scanCooldownUntil) break;

      irUpdateGate();

      // Setelah scan: wajib angkat jari dulu. Scan ulang terlalu cepat (jari
      // masih nempel / false OK) sering bikin image2Tz hang ~5s → loop macet
      // (BLE enroll ikut tidak respons).
      if (fingerMustRelease) {
        bool clear = false;
        if (appSettings.irEnabled && irCalibrated) {
          clear = !irRawDetected();
        } else {
          clear = !irGateOpen;
        }
        if (clear) {
          fingerMustRelease = false;
          fingerConfirm = 0;
          ledForceOff("release-ok");
          irGateOpen = false;
          irDetSince = irClearSince = 0;
          irGateCooldownUntil = 0;
          logDebug("SCAN ready — finger released");
          // Jangan hapus layar MENGIRIM / hasil absensi.
          if (!attnSendingSince && !attnUiClearAt) lcdShowIdle();
        } else {
          if (lcdBacklightOn) ledcWrite(LCD_BL, 90);  // jangan full 255 saat tunggu angkat jari
          break;
        }
      }

      // Jangan mulai scan baru saat MENGIRIM di layar.
      // Timeout attnServiceUi → GAGAL KIRIM + buka kunci (tanpa reboot).
      if (attnSendingSince) {
        if (lcdBacklightOn) ledcWrite(LCD_BL, 90);
        break;
      }

      if (irGateOpen) {
        lastLcdActivity = millis();
        lcdBacklightOn = true;
        ledcWrite(LCD_BL, 255);
        if (!ledOn) {
          uint8_t r = finger.LEDcontrol(true);
          ledOn = (r == FINGERPRINT_OK);
          ledOnSince = ledOn ? millis() : 0;
          if (!ledOn) {
            // Sensor masih error — jangan loop keras; tunggu gate cooldown.
            logError("LED ON (gate) failed code=%d", r);
            scanCooldownUntil = millis() + 500;
            break;
          }
          logDebug("LED ON");
          scanCooldownUntil = millis() + LED_WARMUP_MS;
          break;
        } else if (ledOnSince == 0) {
          ledOnSince = millis();
          scanCooldownUntil = millis() + LED_WARMUP_MS;
          break;
        } else if (appSettings.irEnabled && millis() - ledOnSince > LED_AUTO_OFF_MS) {
          // Auto-off hanya untuk mode gate IR: gate terbuka lama tanpa hasil →
          // tutup sementara. Saat ir_enabled=false (fallback polling) LED harus
          // nyala stabil — mematikannya justru bikin kedip (gate langsung buka lagi).
          ledForceOff("auto");
          fingerConfirm = 0;
          fingerMustRelease = true;
          irGateOpen = false;
          irDetSince = irClearSince = 0;
          irGateCooldownUntil = 0;
          scanCooldownUntil = millis() + 200;
          break;
        }
      } else {
        static unsigned long lastLedOffAttempt = 0;
        if (ledOn && (lastLedOffAttempt == 0 || millis() - lastLedOffAttempt >= LED_OFF_RETRY_MS)) {
          lastLedOffAttempt = millis();
          ledForceOff("standby");
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

      if (p == FINGERPRINT_NOFINGER) {
        // Saat gate IR aktif: jari lepas → matikan LED & tutup gate (hemat).
        // Saat ir_enabled=false (fallback polling): jangan sentuh LED/gate —
        // kalau dimatikan di sini lalu irUpdateGate() buka lagi tiap loop,
        // LED jadi KEDIP terus. Biarkan LED nyala stabil selama polling.
        if (appSettings.irEnabled && irCalibrated) {
          ledForceOff("nofinger");
          irGateOpen = false;
          irDetSince = irClearSince = 0;
          irGateCooldownUntil = millis() + 150;
        }
        fingerConfirm = 0;
        fingerMustRelease = false;
        break;
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
        scanResultTime = millis();
        lcdShowScanning();

        flushRX();
        // image2Tz/fingerSearch bisa blocking >5s → task WDT loop = reboot.
        disableLoopWDT();
        p = finger.image2Tz();
        if (p == FINGERPRINT_OK) p = finger.fingerSearch();
        enableLoopWDT();
        esp_task_wdt_reset();

        if (p != FINGERPRINT_OK && p != FINGERPRINT_NOTFOUND) {
          Serial.printf("[SCAN] image2Tz/search fail: %d\n", p);
          logError("image2Tz/search failed code=%d", p);
          scanResultTime = millis();
          fingerMustRelease = true;
          scanCooldownUntil = millis() + 200;
          ledForceOff("img-fail");
          scanState = SCAN_WAIT_RELEASE;
          return;
        }

        if (p == FINGERPRINT_OK) {
          logDebug("SCAN match id=%d conf=%d", finger.fingerID, finger.confidence);
          const char *nm = dbGetName(finger.fingerID);
          const char *eid = dbGetEmpId(finger.fingerID);
          lcdShowMatch(finger.fingerID, finger.confidence, nm);
          const char *nmE = jsonEscape(nm ? nm : "");
          const char *eidE = jsonEscape(eid ? eid : "");
          emit(F("{\"event\":\"match\",\"id\":%d,\"confidence\":%d,\"name\":\"%s\",\"employeeId\":\"%s\"}"),
               finger.fingerID, finger.confidence, nmE, eidE);
          if (eid && eid[0]) {
            ledForceOff("pre-attn");
            ledcWrite(LCD_BL, 90);
            // SELALU simpan dulu ke storage lokal (riwayat offline).
            int paIdx = pendingAttAdd(eid, nm);
            // Enqueue dulu. Gagal WiFi/queue → tampil OFFLINE/error, JANGAN reboot.
            // Catatan tetap tersimpan (synced=false) dan di-upload saat sync.
            if (WiFi.status() == WL_CONNECTED && attnEnqueue(eid, paIdx)) {
              lcdShowAttendanceStatus("sending");
              attnSendingSince = millis();
            } else {
              lcdShowAttendanceStatus(WiFi.status() == WL_CONNECTED ? "error" : "offline");
              attnSendingSince = 0;
              attnUiClearAt = millis() + ATTN_FAIL_HOLD_MS;
            }
          } else {
            ledForceOff("pre-attn");
          }
        } else {
          logDebug("SCAN nomatch code=%d", p);
          lcdShowNoMatch();
          emit(F("{\"event\":\"nomatch\",\"code\":%d}"), p);
          ledForceOff("nomatch");
        }

        scanResultTime = millis();
        lastScanActivity = millis();
        consecutiveErrors = 0;
        recoveryCount = 0;
        fingerMustRelease = true;
        scanCooldownUntil = millis() + 200;
        scanState = SCAN_WAIT_RELEASE;
      } else {
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
          ledForceOff("soft-recover");
          ledcWrite(LCD_BL, 30);
          irGateOpen = false;
          irGateCooldownUntil = millis() + SCAN_SOFT_RECOVER_MS;
          irGateOpenedAt = irDetSince = irClearSince = 0;
          fingerConfirm = 0;
          fingerMustRelease = false;
          flushRX();
          delay(200);
          flushRX();
          consecutiveErrors = 0;
          scanCooldownUntil = millis() + SCAN_SOFT_RECOVER_MS;
          lastScanActivity = millis();
        }
        delay(80);
      }
      break;
    }

    case SCAN_BUSY:
      if (millis() - scanResultTime > 3000) {
        logDebug("SCAN BUSY timeout — force reset");
        flushRX();
        ledForceOff("busy-timeout");
        fingerMustRelease = true;
        scanState = SCAN_IDLE;
        consecutiveErrors = 0;
        fingerConfirm = 0;
        lcdShowIdle();
      }
      break;

    case SCAN_WAIT_RELEASE: {
      // Saat MENGIRIM: pantau timeout di attnServiceUi (→ GAGAL), jangan reboot.
      if (attnSendingSince) {
        if (lcdBacklightOn) ledcWrite(LCD_BL, 90);
        lastScanActivity = millis();  // jangan picu watchdog
        break;
      }
      // Selalu kembali ke UI idle setelah hold — jangan stuck di MATCH/CHECK IN.
      if (millis() - scanResultTime < SCAN_RESULT_HOLD_MS) break;

      bool fingerGone = true;
      if (appSettings.irEnabled && irCalibrated) {
        fingerGone = !irRawDetected();
      }
      flushRX();
      ledForceOff("post-scan");
      irGateOpen = false;
      irGateCooldownUntil = millis() + 150;
      irGateOpenedAt = irDetSince = irClearSince = 0;
      fingerMustRelease = !fingerGone;
      scanState = SCAN_IDLE;
      consecutiveErrors = 0;
      fingerConfirm = 0;
      if (!attnUiClearAt) lcdShowIdle();
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
  // HTML portal: di softAP / offline jangan Basic Auth (HP stuck 401).
  // STA LAN tetap auth via requireAuth().
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  // Jangan pakai server.send(const char*) — WebServer cast ke String; HTML ~47KB
  // + heap tertekan NimBLE sering gagal ("String cast failed") → halaman blank.
  // send_P stream dari flash tanpa alokasi full-copy.
  server.send_P(200, PSTR("text/html"), INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Jangan sentuh UART sensor saat autoscan/enroll — bentrok getImage → PACKETRECIEVEERR.
  static uint16_t cachedTplCount = 0;
  if (!enrollActive && !restoreActive && scanState == SCAN_IDLE && !ledOn) {
    if (finger.getTemplateCount() == FINGERPRINT_OK) {
      cachedTplCount = finger.templateCount;
    }
  } else if (finger.templateCount > 0) {
    cachedTplCount = finger.templateCount;
  }
  bool staOk = (WiFi.status() == WL_CONNECTED);
  if (staOk) {
    wifiConnected = true;
    staIP = WiFi.localIP().toString();
  }
  String json = "{\"ready\":" + String(sensorReady ? "true" : "false");
  json += ",\"autoActive\":" + String(autoScan ? "true" : "false");
  json += ",\"count\":" + String(cachedTplCount);
  json += ",\"baud\":" + String(curBaud);
  json += ",\"security\":" + String(finger.security_level);
  json += ",\"ip\":\"" + String(staOk ? staIP : WiFi.softAPIP().toString()) + "\"";
  json += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  json += ",\"wifiMode\":\"" + String(staOk ? "STA" : "AP") + "\"";
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
  uint16_t cnt = finger.templateCount;
  if (!enrollActive && !restoreActive && scanState == SCAN_IDLE && !ledOn) {
    if (finger.getTemplateCount() == FINGERPRINT_OK) cnt = finger.templateCount;
  }
  server.send(200, "application/json", "{\"ok\":true,\"count\":" + String(cnt) + "}");
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

  uint8_t id = nextFreeFingerId();
  if (id == 0) {
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
  if (ledOn) {
    uint8_t ledResult = finger.LEDcontrol(false);
    if (ledResult != FINGERPRINT_OK) logError("LED OFF (autooff) failed code=%d", ledResult);
    ledOn = false;  // reset — walau gagal, jangan pertahankan state salah
  }
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
  bool staOk = (WiFi.status() == WL_CONNECTED);
  if (staOk) {
    wifiConnected = true;
    staIP = WiFi.localIP().toString();
  }
  String json = "{";
  json += "\"mode\":\"" + String(staOk ? "STA" : "AP") + "\"";
  json += ",\"connected\":" + String(staOk ? "true" : "false");
  json += ",\"staIP\":\"" + staIP + "\"";
  json += ",\"staSSID\":\"" + staSSID + "\"";
  json += ",\"apSSID\":\"" + String(AP_SSID) + "\"";
  json += ",\"apIP\":\"192.168.4.1\"";
  json += ",\"serverURL\":\"" + String(staOk ? ("http://" + staIP + "/") : "http://192.168.4.1/") + "\"";
  json += ",\"savedCount\":" + String(savedWiFiCount);
  json += ",\"saved\":[";
  for (int i = 0; i < savedWiFiCount; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + String(savedWiFi[i].ssid) + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Progressive scan: channel 1→13, merge ke list, UI poll dapat update live.
// Area padat: 2 pass. Jangan softAP() ulang / scan blocking di HTTP handler.
#define WIFI_SCAN_MAX 64
#define WIFI_SCAN_PASSES 2
struct WifiScanAp {
  char ssid[33];
  char bssid[18];
  int8_t rssi;
  uint8_t channel;
  bool hidden;
  bool enc;
};
static WifiScanAp wifiScanAps[WIFI_SCAN_MAX];
static int wifiScanCount = 0;
static uint8_t wifiScanPhase = 0; // 0 idle, 1 running, 2 done
static uint8_t wifiScanNextCh = 1;
static uint8_t wifiScanPass = 0;
static unsigned long wifiScanStartMs = 0;
static unsigned long wifiScanChStartMs = 0;
static bool wifiScanWasAuto = false;

static void wifiScanJsonEscape(const char *in, String &out) {
  for (const char *p = in; *p; p++) {
    if (*p == '\\' || *p == '"') out += '\\';
    out += *p;
  }
}

static void wifiScanMergeResults(int n) {
  for (int i = 0; i < n; i++) {
    String bssid = WiFi.BSSIDstr(i);
    if (bssid.length() < 11) continue;
    int idx = -1;
    for (int j = 0; j < wifiScanCount; j++) {
      if (bssid.equals(wifiScanAps[j].bssid)) {
        idx = j;
        break;
      }
    }
    if (idx < 0) {
      if (wifiScanCount >= WIFI_SCAN_MAX) continue;
      idx = wifiScanCount++;
      strncpy(wifiScanAps[idx].bssid, bssid.c_str(), sizeof(wifiScanAps[idx].bssid) - 1);
      wifiScanAps[idx].bssid[sizeof(wifiScanAps[idx].bssid) - 1] = 0;
      wifiScanAps[idx].rssi = -127;
    }
    String ssid = WiFi.SSID(i);
    strncpy(wifiScanAps[idx].ssid, ssid.c_str(), sizeof(wifiScanAps[idx].ssid) - 1);
    wifiScanAps[idx].ssid[sizeof(wifiScanAps[idx].ssid) - 1] = 0;
    wifiScanAps[idx].hidden = (ssid.length() == 0);
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (rssi > wifiScanAps[idx].rssi) wifiScanAps[idx].rssi = rssi;
    wifiScanAps[idx].channel = (uint8_t)WiFi.channel(i);
    wifiScanAps[idx].enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
}

static void wifiScanSortByRssi() {
  for (int i = 0; i < wifiScanCount; i++) {
    for (int j = i + 1; j < wifiScanCount; j++) {
      if (wifiScanAps[j].rssi > wifiScanAps[i].rssi) {
        WifiScanAp t = wifiScanAps[i];
        wifiScanAps[i] = wifiScanAps[j];
        wifiScanAps[j] = t;
      }
    }
  }
}

static String wifiScanBuildResponse(const char *status) {
  wifiScanSortByRssi();
  String json;
  json.reserve((size_t)wifiScanCount * 140 + 80);
  json += "{\"status\":\"";
  json += status;
  json += "\",\"channel\":";
  json += String(wifiScanNextCh);
  json += ",\"pass\":";
  json += String(wifiScanPass + 1);
  json += ",\"count\":";
  json += String(wifiScanCount);
  json += ",\"networks\":[";
  for (int i = 0; i < wifiScanCount; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    wifiScanJsonEscape(wifiScanAps[i].ssid, json);
    json += "\",\"hidden\":";
    json += wifiScanAps[i].hidden ? "true" : "false";
    json += ",\"rssi\":";
    json += String(wifiScanAps[i].rssi);
    json += ",\"channel\":";
    json += String(wifiScanAps[i].channel);
    json += ",\"bssid\":\"";
    json += wifiScanAps[i].bssid;
    json += "\",\"enc\":";
    json += wifiScanAps[i].enc ? "true" : "false";
    json += "}";
  }
  json += "]}";
  return json;
}

static bool wifiScanLaunchChannel(uint8_t ch) {
  WiFi.scanDelete();
  // async, show_hidden, active, dwell ms, single channel
  int16_t r = WiFi.scanNetworks(true, true, false, 260, ch);
  wifiScanChStartMs = millis();
  Serial.printf("[WiFi] scan ch%d pass%d r=%d known=%d\n",
                (int)ch, (int)wifiScanPass + 1, (int)r, wifiScanCount);
  return r != WIFI_SCAN_FAILED;
}

static void wifiScanPrepareRadio() {
  WiFi.setSleep(false);
  if (appSettings.apEnabled) {
    WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
      WiFi.softAP(AP_SSID, cred.apPass);
      delay(60);
    }
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.softAPdisconnect(true);
  }
  WiFi.disconnect(false);
  delay(20);
  wifi_country_t country = {};
  strncpy(country.cc, "ID", sizeof(country.cc));
  country.schan = 1;
  country.nchan = 13;
  country.max_tx_power = 78;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
}

static void wifiScanFinish() {
  if (wifiApSetupMode) WiFi.disconnect(false);
  wifiEnsureApAlive();
  wifiApplyPowerPolicy();
  wifiOpsEnd(wifiScanWasAuto);
  wifiScanPhase = 2;
  Serial.printf("[WiFi] scan done total=%d\n", wifiScanCount);
}

// Mulai progressive scan (dipakai web UI + BLE). Return false jika busy/gagal.
static bool wifiScanStart(const char *reason) {
  if (wifiScanPhase == 1) return false;
  if (cacheSyncBusy || attnUploading || httpsBusy) return false;
  Serial.printf("[WiFi] progressive scan start (%s) mode=%d apSetup=%d\n",
                reason ? reason : "?", (int)WiFi.getMode(), wifiApSetupMode ? 1 : 0);
  wifiScanWasAuto = autoScan;
  wifiOpsBegin();
  wifiScanPrepareRadio();
  wifiScanCount = 0;
  wifiScanNextCh = 1;
  wifiScanPass = 0;
  wifiScanStartMs = millis();
  wifiScanPhase = 1;
  if (!wifiScanLaunchChannel(1)) {
    wifiScanFinish();
    return true; // selesai segera (kosong/gagal)
  }
  return true;
}

void wifiScanService() {
  if (wifiScanPhase != 1) return;
  int16_t st = WiFi.scanComplete();
  if (st == WIFI_SCAN_RUNNING) {
    if (millis() - wifiScanChStartMs > 4000) {
      Serial.printf("[WiFi] scan ch%d timeout — skip\n", (int)wifiScanNextCh);
      WiFi.scanDelete();
      st = 0;
    } else {
      return;
    }
  }
  if (st < 0 && st != WIFI_SCAN_FAILED) {
    // unexpected
    return;
  }
  if (st == WIFI_SCAN_FAILED) {
    Serial.printf("[WiFi] scan ch%d failed — lanjut\n", (int)wifiScanNextCh);
    WiFi.scanDelete();
  } else if (st > 0) {
    wifiScanMergeResults(st);
    WiFi.scanDelete();
  } else {
    WiFi.scanDelete();
  }

  wifiScanNextCh++;
  if (wifiScanNextCh > 13) {
    wifiScanPass++;
    if (wifiScanPass < WIFI_SCAN_PASSES) {
      wifiScanNextCh = 1;
      Serial.printf("[WiFi] scan pass %d/%d\n", wifiScanPass + 1, WIFI_SCAN_PASSES);
    } else {
      wifiScanFinish();
      return;
    }
  }
  if (millis() - wifiScanStartMs > 45000) {
    Serial.println("[WiFi] scan global timeout");
    wifiScanFinish();
    return;
  }
  if (!wifiScanLaunchChannel(wifiScanNextCh)) {
    // gagal start — coba channel berikutnya di loop berikutnya
    wifiScanChStartMs = millis() - 3500;
  }
}

void handleWifiScan() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");

  if (cacheSyncBusy || attnUploading || httpsBusy) {
    server.send(200, "application/json",
                "{\"status\":\"busy\",\"count\":0,\"networks\":[],\"error\":\"https_busy\"}");
    return;
  }

  wifiScanService();

  // Live progress: selalu kirim networks yang sudah terkumpul
  if (wifiScanPhase == 1) {
    server.send(200, "application/json", wifiScanBuildResponse("scanning"));
    return;
  }
  if (wifiScanPhase == 2) {
    server.send(200, "application/json", wifiScanBuildResponse("done"));
    wifiScanPhase = 0;
    return;
  }

  if (!wifiScanStart("web")) {
    server.send(200, "application/json",
                "{\"status\":\"busy\",\"count\":0,\"networks\":[],\"error\":\"busy\"}");
    return;
  }
  if (wifiScanPhase == 2) {
    server.send(200, "application/json", wifiScanBuildResponse("done"));
    wifiScanPhase = 0;
    return;
  }
  server.send(200, "application/json", wifiScanBuildResponse("scanning"));
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

  if (appSettings.apEnabled) {
    // AP sudah aktif (mode AP) — jangan panggil softAP() lagi, itu merestart AP
    // dan memutus client browser. Hanya aktifkan AP kalau saat ini STA-only.
    if (WiFi.getMode() == WIFI_STA) {
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(AP_SSID, cred.apPass);
    }
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.softAPdisconnect(true);
  }
  WiFi.setSleep(false);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (millis() - start < 12000) {
    lastScanActivity = millis();
    if (WiFi.status() == WL_CONNECTED) break;
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiMarkStaConnected(ssid);
    server.send(200, "application/json",
      "{\"ok\":true,\"msg\":\"connected\",\"staIP\":\"" + staIP + "\",\"staSSID\":\"" + staSSID + "\"}");
    delay(300);  // biar TCP kirim response dulu
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiApplyPowerPolicy();
    Serial.printf("[WiFi] Saved+connected %s | %s (no reboot)\n", ssid, staIP.c_str());
  } else {
    // Gagal connect → tetap AP stabil, JANGAN biarkan STA terus scan
    logError("WiFi save: connect failed for saved network");
    wifiEnterApOnly("save-connect-failed");
    server.send(200, "application/json",
      "{\"ok\":true,\"msg\":\"saved_connect_failed\",\"mode\":\"AP\"}");
    Serial.printf("[WiFi] Saved %s but connect failed — tetap AP-only\n", ssid);
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
  wifiStaEverOk = false;
  wifiEnterApOnly("wifi-reset");
  staSSID = "";
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
  doc["scan_schedule"] = appSettings.scanSchedule;
  doc["scan_start_hour"] = appSettings.scanStartHour;
  doc["scan_end_hour"] = appSettings.scanEndHour;
  doc["ap_enabled"] = appSettings.apEnabled;
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
  bool httpsRewritten = false;
  if (doc.containsKey("apiBaseUrl")) {
    strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
    appSettings.apiBaseUrl[127] = 0;
    httpsRewritten = settingsNormalizeApiUrl(appSettings.apiBaseUrl, sizeof(appSettings.apiBaseUrl));
  }
  if (doc.containsKey("kode_cabang")) { strncpy(appSettings.kodeCabang, doc["kode_cabang"] | "", 15); appSettings.kodeCabang[15] = 0; }
  if (doc.containsKey("device_id")) { strncpy(appSettings.deviceId, doc["device_id"] | "", 31); appSettings.deviceId[31] = 0; }
  if (doc.containsKey("api_key")) { strncpy(appSettings.apiKey, doc["api_key"] | "", 64); appSettings.apiKey[64] = 0; }
  if (doc.containsKey("ir_enabled")) appSettings.irEnabled = doc["ir_enabled"] | true;
  if (doc.containsKey("scan_schedule")) appSettings.scanSchedule = doc["scan_schedule"] | true;
  if (doc.containsKey("scan_start_hour")) {
    int h = doc["scan_start_hour"] | 5;
    if (h < 0) h = 0; if (h > 23) h = 23;
    appSettings.scanStartHour = (uint8_t)h;
  }
  if (doc.containsKey("scan_end_hour")) {
    int h = doc["scan_end_hour"] | 0;
    if (h < 0) h = 0; if (h > 23) h = 23;
    appSettings.scanEndHour = (uint8_t)h;
  }
  if (doc.containsKey("ap_enabled")) appSettings.apEnabled = doc["ap_enabled"] | false;
  if (doc.containsKey("upload_interval_minutes")) {
    int v = doc["upload_interval_minutes"] | 120;
    if (v < 5) v = 5; if (v > 1440) v = 1440;
    appSettings.uploadIntervalMinutes = (uint16_t)v;
  }
  if (!settingsSave()) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"settings_save_failed\"}");
    return;
  }
  if (httpsRewritten) {
    Serial.printf("[SET] apiBaseUrl https->http: %s\n", appSettings.apiBaseUrl);
  }
  DynamicJsonDocument out(384);
  out["ok"] = true;
  out["msg"] = httpsRewritten ? "saved_http_rewrite" : "saved";
  out["https_rewritten"] = httpsRewritten;
  out["apiBaseUrl"] = appSettings.apiBaseUrl;
  String json;
  serializeJson(out, json);
  server.send(200, "application/json", json);
}

// ────────────────────────────────────────────────────────────────────
//  API Proxy - Branches & Employees from backend
// ────────────────────────────────────────────────────────────────────
// Baca body lewat decoder bawaan HTTPClient (writeToStream).
// JANGAN baca raw getStreamPtr() sendiri: tanpa collectHeaders(), header
// Transfer-Encoding tidak terlihat → framing chunk (mis. "22f\\n{...}\\n0")
// bocor ke JSON dan bikin parse gagal di web UI.
String apiReadBody(HTTPClient &http) {
  StreamString body;
  int sz = http.getSize();  // -1 jika chunked / tanpa Content-Length
  if (sz > 0) body.reserve((size_t)sz + 1);
  else body.reserve(56000);  // CKS employees ~49KB
  int n = http.writeToStream(&body);
  if (n < 0) {
    Serial.printf("[API] writeToStream error %d\n", n);
    logError("API body read error=%d", n);
    return "";
  }
  return body;
}

// Stream body backend → browser tanpa full-buffer di RAM (penting untuk
// /employees cabang besar seperti CKS/Malang ~49KB + NimBLE).
class WebForwardStream : public Stream {
public:
  size_t write(uint8_t c) override {
    char ch = (char)c;
    server.sendContent(&ch, 1);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (!buffer || !size) return 0;
    server.sendContent((const char *)buffer, size);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Tulis respons HTTP langsung ke File (cache LittleFS) — hemat RAM.
class FileWriteStream : public Stream {
  File *f;
public:
  explicit FileWriteStream(File *file) : f(file) {}
  size_t write(uint8_t c) override { return f ? f->write(c) : 0; }
  size_t write(const uint8_t *buffer, size_t size) override {
    return (f && buffer && size) ? f->write(buffer, size) : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { if (f) f->flush(); }
};

// ── Cache daftar cabang/karyawan di LittleFS ─────────────────────────
// Enroll UI baca dari cache (cepat). Sync API berkala / tombol Refresh.
#define CACHE_DIR "/cache"
#define CACHE_BRANCHES "/cache/branches.json"
#define CACHE_META "/cache/meta.json"
#define CACHE_REFRESH_MS (30UL * 60UL * 1000UL)  // 30 menit
// cacheSyncBusy dideklarasikan di atas (WiFi scan guard)
unsigned long lastCacheSyncMs = 0;

bool cacheEnsureDir() {
  if (!storageReady) return false;
  if (LittleFS.exists(CACHE_DIR)) return true;
  return LittleFS.mkdir(CACHE_DIR);
}

String cacheEmpPath(const String &kode) {
  String safe;
  for (size_t i = 0; i < kode.length(); i++) {
    char c = kode.charAt(i);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      safe += c;
  }
  if (!safe.length()) safe = "ALL";
  return String(CACHE_DIR) + "/emp_" + safe + ".json";
}

String cacheEmpSlimPath(const String &kode) {
  String p = cacheEmpPath(kode);
  p.replace(".json", ".slim.json");
  return p;
}

// Job unduh karyawan async — cabang besar (Malang/CKS) jangan block HTTP 20–45s.
char cacheEmpJobKode[24] = {0};
volatile bool cacheEmpJobWanted = false;
volatile bool cacheEmpJobFail = false;
volatile bool cacheBranchesWanted = false;

void cacheRequestEmployees(const String &kode) {
  if (!kode.length() || kode == "__all__") return;
  if (kode.length() >= (int)sizeof(cacheEmpJobKode)) return;
  strncpy(cacheEmpJobKode, kode.c_str(), sizeof(cacheEmpJobKode) - 1);
  cacheEmpJobKode[sizeof(cacheEmpJobKode) - 1] = 0;
  cacheEmpJobFail = false;
  cacheEmpJobWanted = true;
  Serial.printf("[CACHE] emp job queued %s\n", cacheEmpJobKode);
}

bool cacheRefreshBranches();
bool cacheRefreshEmployees(const String &kode);

void cacheWorker(void *param) {
  (void)param;
  for (;;) {
    if (enrollActive || restoreActive) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (cacheBranchesWanted && WiFi.status() == WL_CONNECTED && appSettings.apiBaseUrl[0]) {
      cacheSyncBusy = true;
      Serial.println("[CACHE] worker branches fetch");
      cacheRefreshBranches();
      cacheBranchesWanted = false;
      cacheSyncBusy = false;
      lastCacheSyncMs = millis();
    }

    if (cacheEmpJobWanted && WiFi.status() == WL_CONNECTED && appSettings.apiBaseUrl[0]) {
      char kode[24];
      strncpy(kode, cacheEmpJobKode, sizeof(kode) - 1);
      kode[sizeof(kode) - 1] = 0;
      cacheSyncBusy = true;
      Serial.printf("[CACHE] worker emp start %s\n", kode);
      bool ok = cacheRefreshEmployees(String(kode));
      // Jangan clear wanted jika user sudah antre kode lain.
      if (strncmp(cacheEmpJobKode, kode, sizeof(cacheEmpJobKode)) == 0) {
        cacheEmpJobWanted = false;
        cacheEmpJobFail = !ok;
      }
      cacheSyncBusy = false;
      lastCacheSyncMs = millis();
      Serial.printf("[CACHE] worker emp done %s ok=%d\n", kode, ok ? 1 : 0);
    } else if (cacheEmpJobWanted && WiFi.status() != WL_CONNECTED) {
      cacheEmpJobFail = true;
      cacheEmpJobWanted = false;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void cacheInitWorker() {
  if (!httpsMutex) httpsMutex = xSemaphoreCreateMutex();
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(cacheWorker, "cacheHttp", 12288, nullptr, 1, nullptr, 0);
  Serial.println("[CACHE] download worker ready");
}

// Tick di loop utama: JANGAN fetch di sini (blocking). Worker yang jalan.
void cacheEmpJobTick() {
  // no-op — dibiarkan agar call site lama aman
}

void cacheBackgroundTick() {
  if (cacheSyncBusy || cacheEmpJobWanted || cacheBranchesWanted) return;
  if (enrollActive || restoreActive || attnUploading || httpsBusy) return;
  if (!storageReady || !appSettings.apiBaseUrl[0]) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastCacheSyncMs != 0 && (millis() - lastCacheSyncMs) < CACHE_REFRESH_MS) return;
  cacheBranchesWanted = true;
  if (appSettings.kodeCabang[0]) {
    cacheRequestEmployees(String(appSettings.kodeCabang));
  }
  Serial.println("[CACHE] background queued (worker)");
}

// Ambil array karyawan dari root [] atau {"data":[]}.
static JsonArray empJsonArray(JsonDocument &doc) {
  if (doc.is<JsonArray>()) return doc.as<JsonArray>();
  if (doc["data"].is<JsonArray>()) return doc["data"].as<JsonArray>();
  return JsonArray(); // invalid/empty
}

static void empJsonFilter(JsonDocument &filter) {
  // Root array
  filter[0]["id"] = true;
  filter[0]["nama"] = true;
  filter[0]["name"] = true;
  filter[0]["finger_terdaftar"] = true;
  // Wrapped { data: [ ... ] }
  filter["data"][0]["id"] = true;
  filter["data"][0]["nama"] = true;
  filter["data"][0]["name"] = true;
  filter["data"][0]["finger_terdaftar"] = true;
}

static const char *empNamaOf(JsonObject emp) {
  const char *n = emp["nama"] | "";
  if (n && n[0]) return n;
  return emp["name"] | "";
}

// Buat file slim (id+nama, skip finger_terdaftar) supaya UI enroll cepat.
bool cacheBuildEmpSlim(const String &kode) {
  String src = cacheEmpPath(kode);
  String dst = cacheEmpSlimPath(kode);
  if (!LittleFS.exists(src)) return false;
  File f = LittleFS.open(src, "r");
  if (!f) return false;
  size_t sz = f.size();
  Serial.printf("[CACHE] slim build %s src=%u bytes heap=%u\n",
                kode.c_str(), (unsigned)sz, (unsigned)ESP.getFreeHeap());

  JsonDocument filter;
  empJsonFilter(filter);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  if (err) {
    Serial.printf("[CACHE] slim parse fail %s (%s) heap=%u\n",
                  kode.c_str(), err.c_str(), (unsigned)ESP.getFreeHeap());
    LittleFS.remove(dst);
    return false;
  }
  JsonArray arr = empJsonArray(doc);
  if (arr.isNull()) {
    Serial.printf("[CACHE] slim no array %s\n", kode.c_str());
    LittleFS.remove(dst);
    return false;
  }

  String tmp = dst + ".tmp";
  File o = LittleFS.open(tmp, "w");
  if (!o) return false;
  o.print('[');
  bool first = true;
  int kept = 0;
  for (JsonObject emp : arr) {
    if (emp["finger_terdaftar"] | false) continue;
    if (!first) o.print(',');
    first = false;
    o.print("{\"id\":");
    serializeJson(emp["id"], o);
    o.print(",\"nama\":");
    if (emp["nama"].isNull() && !emp["name"].isNull()) serializeJson(emp["name"], o);
    else serializeJson(emp["nama"], o);
    o.print('}');
    kept++;
    if ((kept & 31) == 0) yield();
  }
  o.print(']');
  o.close();
  LittleFS.remove(dst);
  if (!LittleFS.rename(tmp, dst)) {
    File a = LittleFS.open(tmp, "r");
    File b = LittleFS.open(dst, "w");
    if (a && b) {
      uint8_t buf[512];
      while (a.available()) {
        int n = a.read(buf, sizeof(buf));
        if (n > 0) b.write(buf, n);
      }
    }
    if (a) a.close();
    if (b) b.close();
    LittleFS.remove(tmp);
  }
  Serial.printf("[CACHE] slim %s kept=%d\n", dst.c_str(), kept);
  return LittleFS.exists(dst);
}

void cacheInvalidateEmpSlim(const String &kode) {
  if (!kode.length()) return;
  LittleFS.remove(cacheEmpSlimPath(kode));
}

unsigned long cacheEpochNow() {
  if (timeClient.isTimeSet()) return timeClient.getEpochTime();
  return millis() / 1000UL;
}

void cacheMetaSet(const char *key, unsigned long ts) {
  if (!cacheEnsureDir() || !key || !key[0]) return;
  JsonDocument doc;
  File f = LittleFS.open(CACHE_META, "r");
  if (f) { deserializeJson(doc, f); f.close(); }
  doc[key] = ts;
  f = LittleFS.open(CACHE_META, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

unsigned long cacheMetaGet(const char *key) {
  if (!storageReady || !LittleFS.exists(CACHE_META) || !key) return 0;
  JsonDocument doc;
  File f = LittleFS.open(CACHE_META, "r");
  if (!f) return 0;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return 0;
  return (unsigned long)(doc[key] | 0);
}

bool sendLittleFSFile(const char *path, const char *cacheState) {
  if (!storageReady || !LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return false; }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  if (cacheState && cacheState[0]) server.sendHeader("X-Cache", cacheState);
  // streamFile lebih andal untuk file besar (Malang ~49KB) vs sendContent loop
  size_t n = server.streamFile(f, "application/json");
  Serial.printf("[CACHE] stream %s bytes=%u sent=%u %s\n",
                path, (unsigned)sz, (unsigned)n, cacheState ? cacheState : "");
  return n > 0;
}

// Select2 async: baca slim/full cache, filter q, page/limit — respons kecil.
bool sendEmployeesSelect2(const String &kode, const String &q, int page, int limit) {
  if (!storageReady || !kode.length()) return false;
  String spath = cacheEmpSlimPath(kode);
  String cpath = cacheEmpPath(kode);
  // Slim dulu (array kecil). Kalau belum ada, parse full + buat slim di belakang.
  String path = LittleFS.exists(spath) ? spath : cpath;
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  Serial.printf("[S2] open %s bytes=%u heap=%u\n",
                path.c_str(), (unsigned)sz, (unsigned)ESP.getFreeHeap());

  JsonDocument filter;
  empJsonFilter(filter);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  if (err) {
    Serial.printf("[S2] parse fail %s (%s) heap=%u\n",
                  kode.c_str(), err.c_str(), (unsigned)ESP.getFreeHeap());
    // File corrupt / OOM — antre ulang cache
    if (WiFi.status() == WL_CONNECTED && appSettings.apiBaseUrl[0]) {
      cacheRequestEmployees(kode);
    }
    return false;
  }
  JsonArray arr = empJsonArray(doc);
  if (arr.isNull()) {
    Serial.printf("[S2] no employee array in %s\n", path.c_str());
    // Mungkin format tak dikenal — coba rebuild slim dari full
    if (path != cpath && LittleFS.exists(cpath)) {
      // fall through by reopening full below
    } else if (WiFi.status() == WL_CONNECTED) {
      cacheRequestEmployees(kode);
    }
    // Retry once on full file if we opened slim that is wrong
    if (path == spath && LittleFS.exists(cpath)) {
      path = cpath;
      f = LittleFS.open(path, "r");
      if (!f) return false;
      JsonDocument filter2;
      empJsonFilter(filter2);
      doc.clear();
      err = deserializeJson(doc, f, DeserializationOption::Filter(filter2));
      f.close();
      if (err) {
        Serial.printf("[S2] full parse fail %s (%s)\n", kode.c_str(), err.c_str());
        return false;
      }
      arr = empJsonArray(doc);
      if (arr.isNull()) return false;
      // rebuild slim in background next worker cycle
      cacheBuildEmpSlim(kode);
    } else {
      return false;
    }
  }

  String qLower = q;
  qLower.toLowerCase();
  qLower.trim();
  if (page < 1) page = 1;
  if (limit < 1) limit = 30;
  if (limit > 50) limit = 50;
  int skip = (page - 1) * limit;

  JsonDocument out;
  out["ok"] = true;
  JsonArray results = out["results"].to<JsonArray>();
  int total = 0;
  int matched = 0;
  int sent = 0;
  bool more = false;

  for (JsonObject emp : arr) {
    total++;
    if (emp["finger_terdaftar"] | false) continue;
    const char *idC = emp["id"] | "";
    const char *namaC = empNamaOf(emp);
    if (!idC[0]) continue;
    if (qLower.length()) {
      String idL = String(idC); idL.toLowerCase();
      String namaL = String(namaC); namaL.toLowerCase();
      if (idL.indexOf(qLower) < 0 && namaL.indexOf(qLower) < 0) continue;
    }
    if (matched++ < skip) continue;
    if (sent >= limit) { more = true; break; }

    JsonObject row = results.add<JsonObject>();
    row["id"] = idC;
    String text = String(namaC);
    if (!text.length()) text = idC;
    text += " (";
    text += idC;
    text += ")";
    row["text"] = text;
    row["nama"] = namaC;
    sent++;
    yield();
  }

  out["more"] = more;
  out["page"] = page;
  out["limit"] = limit;
  out["matched"] = matched;
  out["total"] = total;
  out["kode_cabang"] = kode;
  server.sendHeader("X-Cache", path.endsWith(".slim.json") ? "S2-SLIM" : "S2-FULL");
  String json;
  serializeJson(out, json);
  server.send(200, "application/json", json);
  Serial.printf("[S2] %s q='%s' page=%d sent=%d more=%d total=%d\n",
                kode.c_str(), q.c_str(), page, sent, more ? 1 : 0, total);
  return true;
}

bool apiProxyBeginGet(HTTPClient &http, WiFiClient *&client, const String &url, uint32_t timeoutMs) {
  return apiHttpBegin(http, client, url, timeoutMs);
}

// Encode query component untuk proxy Select2 (nama/ID bisa spasi/non-ASCII).
String urlEncodeComponent(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  static const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Select2 live: proxy page kecil ke PJTKI (tanpa unduh full cache cabang).
bool proxyEmployeesSelect2Live(const String &kode, const String &q, int page, int limit) {
  if (!kode.length() || kode == "__all__") return false;
  if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) return false;
  if (!httpsLock(15000)) {
    server.send(503, "application/json",
                "{\"ok\":false,\"error\":\"https_busy\",\"retry\":true,\"results\":[],\"more\":false}");
    return true;
  }

  if (page < 1) page = 1;
  if (limit < 1) limit = 30;
  if (limit > 50) limit = 50;

  String url = String(appSettings.apiBaseUrl) + "/api/finger/employees?select2=1"
             + "&kode_cabang=" + urlEncodeComponent(kode)
             + "&page=" + String(page)
             + "&limit=" + String(limit)
             + "&q=" + urlEncodeComponent(q);

  WiFiClient *client = nullptr;
  HTTPClient http;
  if (!apiHttpBegin(http, client, url, 15000)) {
    httpsUnlock();
    return false;
  }
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("[S2] live HTTP %d\n", httpCode);
    logError("employees select2 live status=%d kode=%s", httpCode, kode.c_str());
    http.end();
    httpsUnlock();
    return false;
  }

  String body = apiReadBody(http);
  http.end();
  httpsUnlock();

  // Validasi minimal: harus punya results (format server baru)
  if (!body.length() || body.indexOf("\"results\"") < 0) {
    Serial.println("[S2] live bad body");
    return false;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Cache", "LIVE");
  server.send(200, "application/json", body);
  Serial.printf("[S2] LIVE %s q='%s' page=%d bytes=%u\n",
                kode.c_str(), q.c_str(), page, (unsigned)body.length());
  return true;
}

// Unduh URL API → file LittleFS (stream, tanpa buffer penuh di RAM).
bool cacheFetchToFile(const String &url, const String &path, int &httpCode) {
  httpCode = -1;
  if (!cacheEnsureDir() || !url.length() || !path.length()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!httpsLock(60000)) {
    logError("cache fetch https busy");
    return false;
  }

  String tmp = path + ".tmp";
  WiFiClient *client = nullptr;
  HTTPClient http;
  if (!apiHttpBegin(http, client, url, 45000)) {
    httpsUnlock();
    logError("cache fetch begin failed");
    return false;
  }
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    logError("cache fetch HTTP %d", httpCode);
    http.end();
    httpsUnlock();
    return false;
  }

  File f = LittleFS.open(tmp, "w");
  if (!f) {
    http.end();
    httpsUnlock();
    logError("cache open tmp failed");
    return false;
  }
  FileWriteStream out(&f);
  int n = http.writeToStream(&out);
  f.close();
  http.end();
  httpsUnlock();
  if (n <= 0) {
    LittleFS.remove(tmp);
    logError("cache write failed n=%d", n);
    return false;
  }
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) {
    // fallback copy
    File a = LittleFS.open(tmp, "r");
    File b = LittleFS.open(path, "w");
    if (a && b) {
      uint8_t buf[512];
      while (a.available()) {
        int r = a.read(buf, sizeof(buf));
        if (r > 0) b.write(buf, r);
      }
    }
    if (a) a.close();
    if (b) b.close();
    LittleFS.remove(tmp);
    if (!LittleFS.exists(path)) return false;
  }
  Serial.printf("[CACHE] saved %s (%d bytes)\n", path.c_str(), n);
  return true;
}

bool cacheRefreshBranches() {
  if (!appSettings.apiBaseUrl[0]) return false;
  String url = String(appSettings.apiBaseUrl) + "/api/finger/branches";
  int code = 0;
  if (!cacheFetchToFile(url, CACHE_BRANCHES, code)) return false;
  cacheMetaSet("branches", cacheEpochNow());
  return true;
}

bool cacheRefreshEmployees(const String &kode) {
  if (!appSettings.apiBaseUrl[0]) return false;
  // Jangan cache "semua cabang" — payload bisa sangat besar.
  if (!kode.length() || kode == "__all__") return false;
  String path = cacheEmpPath(kode);
  String url = String(appSettings.apiBaseUrl) + "/api/finger/employees?kode_cabang=" + kode;
  int code = 0;
  if (!cacheFetchToFile(url, path, code)) return false;
  String metaKey = "emp_" + kode;
  cacheMetaSet(metaKey.c_str(), cacheEpochNow());
  cacheBuildEmpSlim(kode);
  return true;
}

// Patch flag finger_terdaftar di file cache tanpa hit API (setelah enroll/hapus).
bool cachePatchEmpFlagInFile(const String &path, const char *empId, bool registered) {
  if (!storageReady || !empId || !empId[0] || !LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0 || sz > 70000) { f.close(); return false; }
  String data;
  data.reserve(sz + 16);
  uint8_t buf[512];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n > 0) data += String((const char *)buf, n);
  }
  f.close();

  String idKey = String("\"id\":\"") + empId + "\"";
  int idPos = data.indexOf(idKey);
  if (idPos < 0) return false;
  int nextId = data.indexOf("\"id\":\"", idPos + idKey.length());
  int flagPos = data.indexOf("\"finger_terdaftar\":", idPos);
  if (flagPos < 0) return false;
  if (nextId >= 0 && flagPos > nextId) return false;

  int valStart = flagPos + 19;  // length of "finger_terdaftar":
  while (valStart < (int)data.length() && (data.charAt(valStart) == ' ' || data.charAt(valStart) == '\t'))
    valStart++;
  int valEnd = valStart;
  if (data.substring(valStart, valStart + 4) == "true") valEnd = valStart + 4;
  else if (data.substring(valStart, valStart + 5) == "false") valEnd = valStart + 5;
  else return false;

  const char *newVal = registered ? "true" : "false";
  if (data.substring(valStart, valEnd) == newVal) return true;  // sudah sesuai

  String out = data.substring(0, valStart) + newVal + data.substring(valEnd);
  String tmp = path + ".tmp";
  File w = LittleFS.open(tmp, "w");
  if (!w) return false;
  w.print(out);
  w.close();
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) {
    File a = LittleFS.open(tmp, "r");
    File b = LittleFS.open(path, "w");
    if (a && b) {
      while (a.available()) {
        int n = a.read(buf, sizeof(buf));
        if (n > 0) b.write(buf, n);
      }
    }
    if (a) a.close();
    if (b) b.close();
    LittleFS.remove(tmp);
  }
  return LittleFS.exists(path);
}

void cacheSetEmployeeRegistered(const char *empId, bool registered) {
  if (!empId || !empId[0] || !storageReady) return;

  // Coba file cabang dari prefix ID (CKS-HK-0001 → CKS)
  String kode;
  for (const char *p = empId; *p && *p != '-'; p++) kode += *p;
  if (kode.length()) {
    String path = cacheEmpPath(kode);
    if (cachePatchEmpFlagInFile(path, empId, registered)) {
      Serial.printf("[CACHE] %s finger_terdaftar=%s (%s)\n",
                    empId, registered ? "true" : "false", path.c_str());
      cacheInvalidateEmpSlim(kode);
      cacheBuildEmpSlim(kode);
      return;
    }
  }

  // Fallback: scan semua emp_*.json
  File root = LittleFS.open(CACHE_DIR);
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    file.close();
    String base = name;
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.substring(slash + 1);
    if (base.startsWith("emp_") && base.endsWith(".json") &&
        base.indexOf(".tmp") < 0 && base.indexOf(".slim.") < 0) {
      String path = String(CACHE_DIR) + "/" + base;
      if (cachePatchEmpFlagInFile(path, empId, registered)) {
        Serial.printf("[CACHE] %s finger_terdaftar=%s (%s)\n",
                      empId, registered ? "true" : "false", path.c_str());
        String k = base.substring(4);
        if (k.endsWith(".json")) k.remove(k.length() - 5);
        cacheInvalidateEmpSlim(k);
        cacheBuildEmpSlim(k);
        break;
      }
    }
    file = root.openNextFile();
  }
}

void cacheInvalidateAllEmployees() {
  if (!storageReady || !LittleFS.exists(CACHE_DIR)) return;
  File root = LittleFS.open(CACHE_DIR);
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    file.close();
    String base = name;
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.substring(slash + 1);
    if (base.startsWith("emp_") && base.endsWith(".json")) {
      String path = String(CACHE_DIR) + "/" + base;
      LittleFS.remove(path);
      Serial.printf("[CACHE] invalidate %s\n", path.c_str());
    }
    file = root.openNextFile();
  }
}

String apiProxyGet(const char *path, int &httpCode) {
  httpCode = -1;
  if (!appSettings.apiBaseUrl[0]) return "";
  if (WiFi.status() != WL_CONNECTED) return "";
  if (!httpsLock(30000)) {
    logError("API GET https busy path=%s", path);
    return "";
  }

  String url = String(appSettings.apiBaseUrl) + path;
  Serial.print("[API] GET "); Serial.println(url);
  Serial.print("[API] Free heap: "); Serial.println(ESP.getFreeHeap());

  WiFiClient *client = nullptr;
  HTTPClient http;
  if (!apiHttpBegin(http, client, url, 30000)) {
    Serial.println("[API] begin() failed");
    logError("API GET begin failed path=%s", path);
    httpsUnlock();
    return "";
  }
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  httpCode = http.GET();
  Serial.print("[API] HTTP code: "); Serial.println(httpCode);
  if (httpCode < 200 || httpCode >= 300) {
    if (httpCode <= 0) apiHttpLogError("API-GET", http, httpCode);
    else logError("API GET status=%d path=%s", httpCode, path);
  }
  String resp = "";
  if (httpCode > 0) {
    resp = apiReadBody(http);
    Serial.print("[API] Response len: "); Serial.println(resp.length());
  }
  http.end();
  httpsUnlock();
  return resp;
}


String apiProxyPost(const char *path, const String &body, int &httpCode) {
  httpCode = -1;
  if (!appSettings.apiBaseUrl[0]) return "";
  if (WiFi.status() != WL_CONNECTED) return "";
  if (!httpsLock(20000)) {
    logError("API POST https busy path=%s", path);
    return "";
  }

  String url = String(appSettings.apiBaseUrl) + path;
  Serial.print("[API] POST "); Serial.println(url);

  WiFiClient *client = nullptr;
  HTTPClient http;
  if (!apiHttpBegin(http, client, url, 20000)) {
    httpsUnlock();
    return "";
  }
  http.addHeader("Content-Type", "application/json");
  if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
  httpCode = http.POST(body);
  if (httpCode < 200 || httpCode >= 300) {
    if (httpCode <= 0) apiHttpLogError("API-POST", http, httpCode);
    else logError("API POST status=%d path=%s", httpCode, path);
  }
  String resp = "";
  if (httpCode > 0) resp = apiReadBody(http);
  http.end();
  httpsUnlock();
  return resp;
}

void handleBranches() {
  if (!requireAuth()) return;
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  bool refresh = server.hasArg("refresh") && server.arg("refresh") == "1";
  bool haveCache = storageReady && LittleFS.exists(CACHE_BRANCHES);

  // Default: sajikan cache lokal (cepat, tanpa hit API).
  if (!refresh && haveCache) {
    if (sendLittleFSFile(CACHE_BRANCHES, "HIT")) return;
  }

  if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) {
    if (haveCache && sendLittleFSFile(CACHE_BRANCHES, "STALE")) return;
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(502, "application/json",
                "{\"ok\":false,\"error\":\"" +
                String(!appSettings.apiBaseUrl[0] ? "apiBaseUrl_empty" : "wifi_not_connected") +
                "\"}");
    return;
  }

  // Jangan block HTTP thread — unduh di cacheHttp worker.
  if (refresh || !haveCache) {
    cacheBranchesWanted = true;
  }

  if (haveCache) {
    if (sendLittleFSFile(CACHE_BRANCHES, refresh ? "REFRESHING" : "BUSY")) return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json",
              "{\"ok\":false,\"error\":\"caching\",\"retry\":true}");
}

void handleEmployees() {
  if (!requireAuth()) return;
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");

  String kode = server.arg("kode_cabang");
  bool refresh = server.hasArg("refresh") && server.arg("refresh") == "1";
  bool select2 = server.hasArg("select2") && server.arg("select2") == "1";
  String q = server.hasArg("q") ? server.arg("q") : "";
  int page = server.hasArg("page") ? server.arg("page").toInt() : 1;
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 30;
  if (page < 1) page = 1;
  if (limit < 1) limit = 30;
  if (limit > 50) limit = 50;

  // "Semua cabang" tidak di-cache (payload besar) — stream langsung bila online.
  if (!kode.length() || kode == "__all__") {
    if (select2) {
      server.send(422, "application/json",
                  "{\"ok\":false,\"error\":\"pilih_cabang\",\"results\":[],\"more\":false}");
      return;
    }
    if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) {
      server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_or_api_required_for_all\"}");
      return;
    }
    if (!httpsLock(45000)) {
      server.send(503, "application/json", "{\"ok\":false,\"error\":\"https_busy\",\"retry\":true}");
      return;
    }
    String url = String(appSettings.apiBaseUrl) + "/api/finger/employees";
    if (kode.length() && kode != "__all__") url += "?kode_cabang=" + kode;
    WiFiClient *client = nullptr;
    HTTPClient http;
    if (!apiHttpBegin(http, client, url, 45000)) {
      httpsUnlock();
      server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\"}");
      return;
    }
    if (appSettings.apiKey[0]) http.addHeader("X-Device-Key", appSettings.apiKey);
    int httpCode = http.GET();
    if (httpCode < 200 || httpCode >= 300) {
      http.end();
      httpsUnlock();
      server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\",\"httpCode\":" + String(httpCode) + "}");
      return;
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("X-Cache", "BYPASS");
    server.send(200, "application/json", "");
    WebForwardStream fwd;
    http.writeToStream(&fwd);
    server.sendContent("");
    http.end();
    httpsUnlock();
    return;
  }

  String cpath = cacheEmpPath(kode);
  String spath = cacheEmpSlimPath(kode);
  bool haveCache = storageReady && LittleFS.exists(cpath);
  bool haveSlim = storageReady && LittleFS.exists(spath);
  bool jobThis = cacheEmpJobWanted && String(cacheEmpJobKode) == kode;

  // Select2 async: LIVE ke server PJTKI (page kecil). Cache lokal hanya fallback offline.
  if (select2) {
    if (wifiScanPhase == 1) {
      server.send(200, "application/json",
                  "{\"ok\":false,\"error\":\"wifi_scanning\",\"retry\":true,\"results\":[],\"more\":false}");
      return;
    }
    if (WiFi.status() == WL_CONNECTED && appSettings.apiBaseUrl[0]) {
      if (proxyEmployeesSelect2Live(kode, q, page, limit)) return;
      // Live gagal → coba cache lokal bila ada
      if (sendEmployeesSelect2(kode, q, page, limit)) return;
      server.send(502, "application/json",
                  "{\"ok\":false,\"error\":\"backend_unreachable\",\"results\":[],\"more\":false}");
      return;
    }
    // Offline: cache LittleFS
    if (sendEmployeesSelect2(kode, q, page, limit)) return;
    server.send(502, "application/json",
                "{\"ok\":false,\"error\":\"wifi_or_cache_missing\",\"results\":[],\"more\":false}");
    return;
  }

  // Serve cepat: slim dulu (Malang jauh lebih kecil), lalu full cache.
  if (!refresh) {
    if (wifiScanPhase == 1) {
      server.send(200, "application/json",
                  "{\"ok\":false,\"error\":\"wifi_scanning\",\"retry\":true}");
      return;
    }
    if (haveSlim && sendLittleFSFile(spath.c_str(), "HIT-SLIM")) return;
    if (haveCache && sendLittleFSFile(cpath.c_str(), "HIT")) return;
  }

  // Offline: pakai cache apa adanya
  if (WiFi.status() != WL_CONNECTED || !appSettings.apiBaseUrl[0]) {
    if (haveSlim && sendLittleFSFile(spath.c_str(), "STALE-SLIM")) return;
    if (haveCache && sendLittleFSFile(cpath.c_str(), "STALE")) return;
    server.send(502, "application/json",
                "{\"ok\":false,\"error\":\"" +
                String(!appSettings.apiBaseUrl[0] ? "apiBaseUrl_empty" : "wifi_not_connected") +
                "\"}");
    return;
  }

  // Jangan block HTTP 30–45 detik saat unduh Malang — async + UI poll.
  if (cacheEmpJobFail && String(cacheEmpJobKode) == kode && !cacheEmpJobWanted && !haveCache) {
    cacheEmpJobFail = false;
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\"}");
    return;
  }

  if (refresh || !haveCache) {
    if (!jobThis && !(cacheSyncBusy && String(cacheEmpJobKode) == kode)) {
      cacheRequestEmployees(kode);
    }
    // Saat refresh, tetap sajikan cache lama jika ada (UI tidak kosong).
    if (refresh) {
      if (haveSlim && sendLittleFSFile(spath.c_str(), "REFRESHING-SLIM")) return;
      if (haveCache && sendLittleFSFile(cpath.c_str(), "REFRESHING")) return;
    }
    server.send(200, "application/json",
                "{\"ok\":false,\"error\":\"caching\",\"retry\":true,\"kode_cabang\":\"" + kode + "\"}");
    return;
  }

  server.send(502, "application/json", "{\"ok\":false,\"error\":\"backend_unreachable\"}");
}

void handleCacheStatus() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  JsonDocument doc;
  doc["ok"] = true;
  doc["storageReady"] = storageReady;
  doc["syncBusy"] = cacheSyncBusy;
  doc["refreshMinutes"] = (int)(CACHE_REFRESH_MS / 60000UL);
  unsigned long now = cacheEpochNow();
  unsigned long bts = cacheMetaGet("branches");
  doc["branchesCached"] = storageReady && LittleFS.exists(CACHE_BRANCHES);
  doc["branchesAgeSec"] = (bts && now >= bts) ? (now - bts) : -1;
  if (storageReady && LittleFS.exists(CACHE_BRANCHES)) {
    File f = LittleFS.open(CACHE_BRANCHES, "r");
    doc["branchesBytes"] = f ? (int)f.size() : 0;
    if (f) f.close();
  }
  String kode = server.hasArg("kode_cabang") ? server.arg("kode_cabang") : String(appSettings.kodeCabang);
  if (kode.length() && kode != "__all__") {
    String p = cacheEmpPath(kode);
    String mk = "emp_" + kode;
    unsigned long ets = cacheMetaGet(mk.c_str());
    doc["kode_cabang"] = kode;
    doc["employeesCached"] = LittleFS.exists(p);
    doc["employeesAgeSec"] = (ets && now >= ets) ? (now - ets) : -1;
    if (LittleFS.exists(p)) {
      File f = LittleFS.open(p, "r");
      doc["employeesBytes"] = f ? (int)f.size() : 0;
      if (f) f.close();
    }
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleCacheRefresh() {
  if (!requireAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }
  if (!appSettings.apiBaseUrl[0]) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"apiBaseUrl_empty\"}");
    return;
  }
  if (cacheSyncBusy || enrollActive || restoreActive) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }

  String kode = "";
  if (server.hasArg("plain") && server.arg("plain").length()) {
    JsonDocument body;
    if (!deserializeJson(body, server.arg("plain"))) {
      kode = String((const char *)(body["kode_cabang"] | ""));
    }
  }
  if (!kode.length() && server.hasArg("kode_cabang")) kode = server.arg("kode_cabang");
  if (!kode.length() && appSettings.kodeCabang[0]) kode = String(appSettings.kodeCabang);

  cacheBranchesWanted = true;
  if (kode.length() && kode != "__all__") cacheRequestEmployees(kode);
  else if (appSettings.kodeCabang[0]) cacheRequestEmployees(String(appSettings.kodeCabang));

  // Tunggu worker selesai sambil tetap layani AP/web (jangan block radio).
  unsigned long t0 = millis();
  while ((cacheBranchesWanted || cacheEmpJobWanted || cacheSyncBusy) &&
         (millis() - t0 < 90000UL)) {
    wifiServicePump();
    delay(40);
  }
  bool okB = storageReady && LittleFS.exists(CACHE_BRANCHES);
  bool okE = false;
  if (kode.length() && kode != "__all__") okE = LittleFS.exists(cacheEmpPath(kode));
  lastCacheSyncMs = millis();

  String json = "{\"ok\":true,\"branches\":";
  json += okB ? "true" : "false";
  json += ",\"employees\":";
  json += okE ? "true" : "false";
  json += ",\"kode_cabang\":\"";
  json += kode;
  json += "\"}";
  server.send(200, "application/json", json);
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

// Read 256-byte template from sensor for given ID (DATA 0x02 + END 0x08 packets)
bool getTemplateRaw(uint16_t id, uint8_t *buf) {
  if (finger.loadModel(id) != FINGERPRINT_OK) return false;
  delay(50);
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
  unsigned long lastByte = 0;
  uint8_t pkt[700];
  uint16_t idx = 0;
  while (millis() - start < 2500 && idx < sizeof(pkt)) {
    if (altSerial.available()) {
      pkt[idx++] = altSerial.read();
      lastByte = millis();
    } else if (lastByte && millis() - lastByte > 100) {
      break;
    } else {
      delay(1);
    }
  }
  if (idx < 9) return false;

  memset(buf, 0, 256);
  uint16_t copied = 0;
  uint16_t i = 0;
  while (i + 9 <= idx && copied < 256) {
    if (!(pkt[i] == 0xEF && pkt[i + 1] == 0x01)) { i++; continue; }
    uint8_t ptype = pkt[i + 6];
    uint16_t wLen = ((uint16_t)pkt[i + 7] << 8) | pkt[i + 8];
    uint16_t total = 9 + wLen;
    if (wLen < 2 || i + total > idx) break;
    uint16_t dataLen = wLen - 2;
    if ((ptype == 0x02 || ptype == 0x08) && dataLen >= 128) {
      uint16_t n = dataLen;
      if (n > 256 - copied) n = 256 - copied;
      memcpy(buf + copied, pkt + i + 9, n);
      copied += n;
    }
    i += total;
  }
  return copied >= 128;
}

// Write 256-byte template: DownChar buffer 1, lalu 2 paket DATA 128-byte, lalu storeModel
bool putTemplateRaw(uint16_t id, const uint8_t *buf) {
  flushRX();
  delay(40);
  uint8_t payload[2] = {0x09, 0x01};
  uint8_t ackType = 0, ackBuf[16];
  uint16_t ackLen = 0;
  if (!sendFingerCmd(payload, 2, &ackType, ackBuf, &ackLen)) {
    logError("putTemplate DownChar cmd fail id=%d", id);
    return false;
  }
  if (ackType != 0x07 || ackBuf[0] != 0x00) {
    logError("putTemplate DownChar ack type=%d code=%d id=%d", ackType, ackBuf[0], id);
    return false;
  }

  for (uint8_t n = 0; n < 2; n++) {
    uint8_t ptype = (n == 1) ? 0x08 : 0x02;
    const uint8_t *chunk = buf + n * 128;
    uint8_t hdr[9] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, ptype, 0x00, 0x82};
    uint16_t sum = ptype + 0x00 + 0x82;
    for (int i = 0; i < 128; i++) sum += chunk[i];
    altSerial.write(hdr, 9);
    altSerial.write(chunk, 128);
    altSerial.write((uint8_t)(sum >> 8));
    altSerial.write((uint8_t)(sum & 0xFF));
    altSerial.flush();
    delay(30);
  }
  delay(80);
  uint8_t st = finger.storeModel(id);
  if (st != FINGERPRINT_OK) {
    logError("putTemplate storeModel id=%d code=%d", id, st);
    return false;
  }
  return true;
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
  if (!hex || !buf || !maxLen) return false;
  size_t hexLen = strlen(hex);
  if (hexLen < 256 || (hexLen % 2) != 0) return false;
  size_t n = hexLen / 2;
  if (n > maxLen) n = maxLen;
  memset(buf, 0, maxLen);
  for (size_t i = 0; i < n; i++) {
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
    json += "\"name\":\"" + String(jsonEscape(fpDB[i].name)) + "\"";
    if (fpDB[i].empId[0]) json += ",\"employeeId\":\"" + String(jsonEscape(fpDB[i].empId)) + "\"";
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
    detail += ",\"name\":\"" + String(jsonEscape(name)) + "\"";
    if (empId[0]) detail += ",\"employeeId\":\"" + String(jsonEscape(empId)) + "\"";
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
    s += "\"name\":\"" + String(jsonEscape(fpDB[i].name)) + "\"";
    if (fpDB[i].empId[0]) s += ",\"employeeId\":\"" + String(jsonEscape(fpDB[i].empId)) + "\"";
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
    if (used) continue;
    // Cek slot fisik di sensor juga — fpDB bisa tidak sinkron (mis. sensor
    // di-restore lewat Cadangan). loadModel() return FINGERPRINT_OK jika
    // template sudah ada di slot tsb.
    if (sensorReady) {
      uint8_t p = finger.loadModel(id);
      if (p == FINGERPRINT_OK) continue;
      if (p == FINGERPRINT_BADLOCATION) return id;
      // PACKETRECIEVEERR/lain: komunikasi gagal — abaikan cek sensor
      // supaya enroll tetap bisa jalan (ID bisa dicek lagi oleh sensor saat
      // fingerSearch di enrollFinger).
    }
    return id;
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

    int dbIdx = findDbByEmpId(empId);
    bool onSensor = false;
    if (dbIdx >= 0) {
      // fpDB ada entry, tapi pastikan template beneran ada di sensor.
      // Kalau sensor pernah di-reset/empty tanpa update fpDB, entry tsb
      // harus direstore, bukan di-skip.
      flushRX();
      delay(10);
      onSensor = (finger.loadModel(fpDB[dbIdx].id) == FINGERPRINT_OK);
    }
    if (dbIdx >= 0 && onSensor) {
      skipped++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"skipped_local\"}"), jsonEscape(empId));
      continue;
    }

    String onePath = String("/api/finger/arduino/template/") + empId;
    int code2 = 0;
    String oneResp = apiProxyGet(onePath.c_str(), code2);
    if (oneResp.length() == 0 || code2 < 200 || code2 >= 300) {
      logError("sync fetch fail emp=%s http=%d len=%u", empId, code2, (unsigned)oneResp.length());
      if (code2 == 422) noHex++;
      else failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"fetch_fail\"}"), jsonEscape(empId));
      continue;
    }

    JsonDocument oneDoc;
    DeserializationError perr = deserializeJson(oneDoc, oneResp);
    if (perr || !oneDoc["success"]) {
      logError("sync json fail emp=%s err=%s len=%u", empId, perr.c_str(), (unsigned)oneResp.length());
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_hex\"}"), jsonEscape(empId));
      continue;
    }

    String hexStr = oneDoc["data"]["template_hex"] | "";
    String nmStr = oneDoc["data"]["nama"] | "";
    int preferId = oneDoc["data"]["finger_id"] | 0;
    const char *hex = hexStr.c_str();
    const char *nm = nmStr.c_str();

    uint8_t tpl[256];
    if (!hex[0] || !fromHex(hex, tpl, 256)) {
      logError("sync bad_hex emp=%s hex_len=%u", empId, (unsigned)hexStr.length());
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"bad_hex\"}"), jsonEscape(empId));
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
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_slot\"}"), jsonEscape(empId));
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
      if (!id) { failed++; emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"no_slot\"}"), jsonEscape(empId)); break; }
    }

    bool ok = putTemplateRaw(id, tpl);
    if (ok) {
      dbAdd(id, nm && nm[0] ? nm : empId, empId);
      restored++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"id\":%d,\"status\":\"restored\"}"), jsonEscape(empId), id);
    } else {
      logError("sync write_fail emp=%s slot=%d", empId, id);
      failed++;
      emit(F("{\"event\":\"sync_progress\",\"employeeId\":\"%s\",\"status\":\"write_fail\"}"), jsonEscape(empId));
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

void handleCaptiveProbe() {
  // Probe OS (Android/iOS/Windows) → redirect ke portal UI, bukan 404 JSON.
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }

  String uri = server.uri();
  Serial.printf("[HTTP] notfound %s\n", uri.c_str());

  // Unknown API → JSON
  if (uri.startsWith("/api/")) {
    server.send(404, "application/json", "{\"error\":\"not_found\"}");
    return;
  }

  // Mode AP / captive portal: HP sering buka path acak → jangan blank 404.
  if (wifiApSetupMode || !wifiConnected) {
    if (server.method() == HTTP_GET) {
      handleCaptiveProbe();
      return;
    }
  }

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

  if (doc.containsKey("webUser")) { strncpy(cred.webUser, doc["webUser"] | "", 31); cred.webUser[31] = 0; }
  if (doc.containsKey("webPass") && strlen(doc["webPass"] | "") > 0) { strncpy(cred.webPass, doc["webPass"] | "", 63); cred.webPass[63] = 0; }
  if (doc.containsKey("apPass") && strlen(doc["apPass"] | "") > 0) { strncpy(cred.apPass, doc["apPass"] | "", 64); cred.apPass[64] = 0; }
  if (doc.containsKey("ntpServer")) { strncpy(cred.ntpServer, doc["ntpServer"] | "", 63); cred.ntpServer[63] = 0; }
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
  Serial.println("[SENSOR] Re-init sensor (WiFi ON, hardware UART)...");
  autoScan = false;
  sensorReady = false;

  // JANGAN bleStop()/bleInit() di sini — NimBLE deinit mengganggu coexist
  // WiFi/AP → HP kehilangan AP saat finger mati. Sensor di UART2, WiFi tetap ON.

  // Tunggu sensor stabil — tetap pump web supaya AP hidup
  pumpDelay(1500);

  bool ok = false;
  curBaud = 0;

  // ⚠️ reinitSensor dipanggil dari loopTask (watchdogCheck / loop reconnect /
  // post-enroll). Deteksi baud blokir loop berdetik-detik (5×57600 + 4×baud,
  // tiap verifyPassword + pumpDelay). TANPA disableLoopWDT → task watchdog
  // triggered → reboot (lihat log: "task_wdt ... loopTask (CPU 1) ... Aborting").
  // Sama seperti enrollFinger: blocking sensor WAJIB di-wrap WDT.
  disableLoopWDT();

  // Phase 1: Fokus di 57600 (default FPM10A 3.3V)
  Serial.println("[SENSOR] Phase1: 57600 x5...");
  altSerial.end();
  pumpDelay(200);
  altSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  pumpDelay(300);
  flushRX();
  finger.begin(57600);
  pumpDelay(200);

  for (int attempt = 0; attempt < 5 && !ok; attempt++) {
    wifiServicePump();
    Serial.printf("[SENSOR] 57600 try %d/5\n", attempt + 1);
    if (finger.verifyPassword()) {
      curBaud = 57600;
      ok = true;
    } else {
      pumpDelay(150);
      flushRX();
    }
  }

  // Phase 2: Coba semua baud (lebih pendek — jangan blok AP terlalu lama)
  if (!ok) {
    const unsigned long tryBauds[] = {57600, 9600, 19200, 38400};
    for (int i = 0; i < 4 && !ok; i++) {
      unsigned long baud = tryBauds[i];
      Serial.printf("[SENSOR] Phase2: Baud %lu...\n", baud);
      altSerial.end();
      pumpDelay(150);
      altSerial.begin(baud, SERIAL_8N1, FINGER_RX, FINGER_TX);
      pumpDelay(250);
      flushRX();
      finger.begin(baud);
      pumpDelay(150);
      for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        wifiServicePump();
        if (finger.verifyPassword()) {
          curBaud = baud;
          ok = true;
        } else {
          pumpDelay(100);
          flushRX();
        }
      }
    }
  }

  enableLoopWDT();
  esp_task_wdt_reset();

  wifiEnsureApAlive();

  if (ok) {
    pumpDelay(100);
    finger.getParameters();
    finger.setSecurityLevel(FINGERPRINT_SECURITY_LEVEL_2);
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
    Serial.println("[SENSOR] Re-init FAILED — AP tetap dilayani");
    logError("sensor reinit failed");
    sensorReady = false;
    autoScan = false;
  }
  return ok;
}

// ── WiFi reconnect (setelah reinitSensor / drop STA) ──
// JANGAN paksa semua SSID saat AP setup — itu merusak kestabilan AP.
void wifiReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiMarkStaConnected(staSSID.c_str());
    return;
  }

  // Mode setup AP: JANGAN wifiEnterApOnly() (softAP ulang = putus client).
  if (wifiApSetupMode || !wifiStaEverOk || savedWiFiCount == 0) {
    wifiEnsureApAlive();
    return;
  }

  // Soft reconnect hanya ke SSID terakhir yang pernah sukses.
  const char *target = staSSID.length() ? staSSID.c_str() : savedWiFi[0].ssid;
  const char *pass = nullptr;
  for (int i = 0; i < savedWiFiCount; i++) {
    if (strcmp(savedWiFi[i].ssid, target) == 0) {
      pass = savedWiFi[i].pass;
      break;
    }
  }
  if (!pass) {
    wifiApSetupMode = true;
    wifiStaEverOk = false;
    wifiEnsureApAlive();
    return;
  }

  Serial.printf("[WiFi] Soft reconnect to %s...\n", target);
  if (WiFi.getMode() == WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
    pumpDelay(50);
  }
  WiFi.begin(target, pass);
  unsigned long start = millis();
  while (millis() - start < 8000) {
    wifiServicePump();
    if (WiFi.status() == WL_CONNECTED) break;
    delay(20);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiMarkStaConnected(target);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiApplyPowerPolicy();
    Serial.printf("[WiFi] Reconnected %s | %s\n", staSSID.c_str(), staIP.c_str());
    return;
  }

  Serial.println("[WiFi] Soft reconnect failed — AP setup tanpa restart softAP");
  logError("WiFi soft reconnect failed");
  wifiStaEverOk = false;
  wifiApSetupMode = true;
  WiFi.disconnect(true);
  pumpDelay(50);
  wifiEnsureApAlive();
}

void watchdogCheck() {
  if (!autoScan || enrollActive || restoreActive) return;
  if (scanSleeping) return; // jadwal tidur (mis. 00:00–05:00): scan sengaja mati — jangan reinit
  if (attnUploading || cacheSyncBusy || httpsBusy) return; // jangan reinit saat HTTPS
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
  pendingAttLoad();
  pendingRegLoad();

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
  server.on("/api/cache/status", HTTP_GET, handleCacheStatus);
  server.on("/api/cache/refresh", HTTP_POST, handleCacheRefresh);
  server.on("/api/cache/refresh", HTTP_OPTIONS, handleCacheRefresh);
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
  // Captive portal probes (Android / iOS / Windows / Kindle)
  server.on("/generate_204", handleCaptiveProbe);
  server.on("/gen_204", handleCaptiveProbe);
  server.on("/hotspot-detect.html", handleCaptiveProbe);
  server.on("/library/test/success.html", handleCaptiveProbe);
  server.on("/ncsi.txt", handleCaptiveProbe);
  server.on("/connecttest.txt", handleCaptiveProbe);
  server.on("/canonical.html", handleCaptiveProbe);
  server.on("/success.txt", handleCaptiveProbe);
  server.on("/chrome-variations/seed", handleCaptiveProbe);
  server.onNotFound(handleNotFound);
  server.begin();

  attnInit();
  cacheInitWorker();
  syncInit();

  // Kalau boot langsung AP-only, nyalakan captive DNS
  if (wifiApSetupMode) wifiDnsStart();

  lcdProgress(45);

  // ── Fingerprint sensor auto-detect ──
  // ESP32 HardwareSerial lebih stabil dari ESP8266 SoftwareSerial.
  Serial.println("[SENSOR] Detecting sensor (WiFi ON)...");
  lcdEnrollStep("Init Sensor", -1, "Deteksi sensor...", COL_ACCENT);

  // Simpan status WiFi
  bool wasWifiConnected = wifiConnected;
  String wasStaIP = staIP;
  String wasStaSSID = staSSID;

  // WiFi TIDAK dimatikan — sensor di HardwareSerial UART2, tidak terganggu
  // interrupt WiFi (beda dengan NodeMCU/SoftwareSerial). AP tetap hidup agar
  // web UI tetap bisa diakses selama boot.
  Serial.println("[SENSOR] Detecting sensor (WiFi ON)...");

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
        // Klon FPM10A LED menyala dari power-on sampai dikontrol 0x50/0x51.
        // Matikan SEGERA setelah sensor berespon, supaya boot tidak tampak
        // "LED nyala terus". Ini penting: setup masih panjang (BLE, gate
        // calibration) yang bisa makan waktu detik — LED harus mati sekarang.
        ledOn = false;
        uint8_t ledR = finger.LEDcontrol(false);
        if (ledR != FINGERPRINT_OK) {
          Serial.printf("[LED] boot OFF failed code=%d (retry nanti)\n", ledR);
        } else {
          Serial.println("[LED] boot OFF");
        }
      } else {
        delay(100);
        flushRX();
      }
    }
  }

  // WiFi tetap hidup sepanjang deteksi — tidak perlu re-enable.
  if (wasWifiConnected && wasStaSSID.length() > 0 && wifiStaEverOk) {
    // Sudah STA sebelum deteksi sensor — pastikan masih connect; kalau putus, soft reconnect sekali.
    if (WiFi.status() != WL_CONNECTED) {
      wifiReconnect();
    } else {
      wifiMarkStaConnected(wasStaSSID.c_str());
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
    }
  } else if (!wifiConnected) {
    // Boot tanpa STA / gagal connect → kunci AP-only (jangan tinggal AP_STA scan).
    wifiEnterApOnly("post-sensor-ap");
  }

  lcdProgress(80);

  // ── BLE ──
  // Inisialisasi BLE SETELAH WiFi stabil. Jangan init BLE lalu matikan
  // WiFi (WiFi.mode(WIFI_OFF) untuk deteksi sensor): BLE & WiFi berbagi
  // radio yang sama → coexist rusak → advertising mati diam-diam meski
  // log "Advertising started" muncul.
  bleInit();

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

  // Tunda sync cache pertama ±30 menit — populate on-demand saat buka tab Daftar
  // atau lewat tombol Refresh (hindari blok boot dengan unduhan 49KB).
  lastCacheSyncMs = millis();
  cacheEnsureDir();
}

// ────────────────────────────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────────────────────────────
void loop() {
  // BLE WiFi — simpan kredensial + connect non-blocking (sama logika web UI)
  static bool bleWifiConnectActive = false;
  static unsigned long bleWifiConnectStart = 0;
  static bool bleWifiWasAuto = false;

  if (bleWifiSaveRequested) {
    bleWifiSaveRequested = false;
    wifiLoadCreds();
    if (!storageReady || !wifiAddCreds(bleWifiSsid, bleWifiPass)) {
      bleNotifyEvent("{\"event\":\"wifi_saved\",\"ok\":false}");
    } else {
      bleNotifyEvent("{\"event\":\"wifi_saved\",\"ok\":true}");
      bleWifiWasAuto = autoScan;
      wifiOpsBegin();
      if (appSettings.apEnabled) {
        if (WiFi.getMode() == WIFI_STA) {
          WiFi.mode(WIFI_AP_STA);
          WiFi.softAP(AP_SSID, cred.apPass);
        }
      } else {
        WiFi.mode(WIFI_STA);
        WiFi.softAPdisconnect(true);
      }
      WiFi.setSleep(false);
      WiFi.begin(bleWifiSsid, bleWifiPass);
      bleWifiConnectActive = true;
      bleWifiConnectStart = millis();
      bleNotifyEvent("{\"event\":\"wifi_connecting\"}");
      Serial.printf("[BLE] WiFi connect start: %s\n", bleWifiSsid);
    }
  }

  if (bleWifiConnectActive) {
    lastScanActivity = millis();
    if (WiFi.status() == WL_CONNECTED) {
      bleWifiConnectActive = false;
      wifiMarkStaConnected(bleWifiSsid);
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifiApplyPowerPolicy();
      bleUpdateStatus();
      bleUpdateSettings();
      char ev[160];
      snprintf(ev, sizeof(ev),
        "{\"event\":\"wifi_connected\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
        staSSID.c_str(), staIP.c_str());
      bleNotifyEvent(ev);
      wifiOpsEnd(bleWifiWasAuto);
      Serial.printf("[BLE] WiFi connected %s | %s\n", staSSID.c_str(), staIP.c_str());
    } else if (millis() - bleWifiConnectStart > 15000) {
      bleWifiConnectActive = false;
      logError("BLE WiFi connect timeout ssid=%s", bleWifiSsid);
      wifiEnterApOnly("ble-connect-failed");
      bleNotifyEvent("{\"event\":\"wifi_failed\",\"reason\":\"timeout\"}");
      wifiOpsEnd(bleWifiWasAuto);
      Serial.printf("[BLE] WiFi connect failed: %s\n", bleWifiSsid);
    }
  }

  // BLE WiFi scan — progressive scan lalu kirim SSID satu-per-satu via notify
  static bool bleWifiScanPendingDone = false;
  static bool bleWifiScanStreaming = false;
  static int bleWifiScanSendIdx = 0;

  if (bleWifiScanRequested) {
    bleWifiScanRequested = false;
    if (wifiScanPhase == 1 || bleWifiScanStreaming) {
      bleNotifyEvent("{\"event\":\"wifi_scan\",\"status\":\"busy\"}");
    } else if (cacheSyncBusy || attnUploading || httpsBusy) {
      bleNotifyEvent("{\"event\":\"wifi_scan\",\"status\":\"busy\",\"reason\":\"https\"}");
    } else if (!wifiScanStart("ble")) {
      bleNotifyEvent("{\"event\":\"wifi_scan\",\"status\":\"busy\"}");
    } else {
      bleWifiScanPendingDone = true;
      bleNotifyEvent("{\"event\":\"wifi_scan\",\"status\":\"scanning\"}");
      Serial.println("[BLE] WiFi scan started");
    }
  }

  if (bleWifiScanPendingDone && wifiScanPhase == 2) {
    bleWifiScanPendingDone = false;
    wifiScanSortByRssi();
    bleWifiScanSendIdx = 0;
    bleWifiScanStreaming = true;
  }

  if (bleWifiScanStreaming) {
    // Kirim max 1 AP per loop agar notify BLE tidak drop.
    while (bleWifiScanSendIdx < wifiScanCount) {
      const char *ssid = wifiScanAps[bleWifiScanSendIdx].ssid;
      int rssi = wifiScanAps[bleWifiScanSendIdx].rssi;
      bool enc = wifiScanAps[bleWifiScanSendIdx].enc;
      uint8_t ch = wifiScanAps[bleWifiScanSendIdx].channel;
      int idx = bleWifiScanSendIdx++;
      if (!ssid[0] || wifiScanAps[idx].hidden) continue;
      // Skip duplikat SSID yang sudah dikirim (ambil RSSI terbaik karena sudah sorted)
      bool dup = false;
      for (int j = 0; j < idx; j++) {
        if (wifiScanAps[j].ssid[0] && strcmp(wifiScanAps[j].ssid, ssid) == 0) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      char esc[40];
      int e = 0;
      for (const char *s = ssid; *s && e < (int)sizeof(esc) - 1; s++) {
        char c = *s;
        if (c == '"' || c == '\\') {
          if (e < (int)sizeof(esc) - 2) esc[e++] = '\\';
        }
        if (c >= 32 && c != 127) esc[e++] = c;
      }
      esc[e] = 0;
      char ev[128];
      snprintf(ev, sizeof(ev),
        "{\"event\":\"wifi_scan_ap\",\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%s,\"ch\":%u}",
        esc, rssi, enc ? "true" : "false", (unsigned)ch);
      bleNotifyEvent(ev);
      break; // satu per iterasi
    }
    if (bleWifiScanSendIdx >= wifiScanCount) {
      bleWifiScanStreaming = false;
      wifiScanPhase = 0;
      char done[80];
      snprintf(done, sizeof(done),
        "{\"event\":\"wifi_scan\",\"status\":\"done\",\"count\":%d}", wifiScanCount);
      bleNotifyEvent(done);
      Serial.printf("[BLE] WiFi scan sent done count=%d\n", wifiScanCount);
    }
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
          // Bersihkan state scan/LED yang mungkin macet setelah uji sentuh + BLE connect.
          sensorResumeIdle("pre-enroll");
          autoScan = false;
          bleWakeUi();
          Serial.printf("[BLE] starting enroll name=%s emp=%s\n", nm, eid);
          uint8_t enrollRes = enrollFinger(0, nm, eid);  // id=0 → auto-assign
          // Pastikan kembali siap scan absensi — jangan biarkan LED/gate tersangkut.
          sensorResumeIdle("post-enroll");
          lcdShowIdle();
          bleUpdateStatus();
          Serial.printf("[BLE] enroll finished res=%u — autoscan ON\n", enrollRes);
          // Jika enroll gagal karena sensor error (bukan batal 0xFD), coba
          // reinit sensor supaya LED & scan pulih — jangan biarkan macet.
          if (enrollRes != 0xFD && enrollRes != FINGERPRINT_OK) {
            Serial.printf("[BLE] enroll failed res=%u → sensor reinit\n", enrollRes);
            logError("enroll failed res=%u — reinit sensor", enrollRes);
            delay(300);
            flushRX();
            reinitSensor();
            sensorResumeIdle("post-enroll-reinit");
            bleUpdateStatus();
          }
        }
      }
    }
  }

  // BLE Delete — hapus sidik jari dari sensor + DB lokal
  if (bleDeleteRequested) {
    bleDeleteRequested = false;
    uint8_t id = bleDeleteId;
    Serial.printf("[BLE] delete id=%u\n", id);
    if (id > 0 && id <= 100) {
      int p = finger.deleteModel(id);
      if (p == FINGERPRINT_OK) {
        dbRemove(id);
        emit(F("{\"event\":\"deleted\",\"id\":%u,\"ok\":true}"), id);
      } else {
        Serial.printf("[BLE] deleteModel failed code=%d\n", p);
        emit(F("{\"event\":\"deleted\",\"id\":%u,\"ok\":false,\"code\":%d}"), id, p);
      }
      finger.getTemplateCount();
      bleUpdateStatus();
      lcdShowIdle();
    }
  }

  // Poll sensor ~20 Hz (bukan ~500 Hz). delay(2) + WiFi no-sleep = ESP32 panas.
  if (autoScan) {
    doAutoScan();
    watchdogCheck();
    delay(40);
  } else {
    // autoScan off. Dua kasus berbeda:
    //   a) User/app sengaja matikan autoscan (mis. sebelum enroll BLE) — sensor
    //      SEHAT, jangan reinit! reinitSensor() mem-blokir loop ~15 detik dan
    //      menahan permintaan enroll yang masuk → "enroll tidak merespon / delay 5s".
    //   b) Sensor benar-benar mati (sensorReady=false) — baru reinit berkala.
    // Guard dengan !sensorReady supaya mode enroll tidak memicu reconnect.
    wifiEnsureApAlive();
    if (sensorReady) {
      // Sensor sehat, hanya autoscan dimatikan (enroll/uji). Jangan reinit.
      delay(20);
    } else {
      static unsigned long lastRetry = 0;
      // Jangan reinit tiap 5s (blokir AP). Coba tiap 30s saja.
      if (millis() - lastRetry > 30000) {
        lastRetry = millis();
        Serial.println("[LOOP] Trying sensor reconnect...");
        if (reinitSensor()) {
          Serial.println("[LOOP] Sensor reconnected!");
          recoveryCount = 0;
          // Hanya soft-reconnect STA jika memang pernah STA; AP setup: no-op aman
          if (!wifiApSetupMode) wifiReconnect();
        } else {
          recoveryCount++;
          Serial.printf("[LOOP] Sensor still down (%d) — AP tetap aktif\n", recoveryCount);
          logError("loop sensor reconnect failed count=%d", recoveryCount);
          wifiEnsureApAlive();
          if (recoveryCount >= MAX_RECOVERY) {
            Serial.println("[LOOP] Max sensor recovery — idle, keep serving AP");
            logError("loop sensor max recovery reached");
            emit(F("{\"event\":\"sensor_wait\"}"));
            recoveryCount = 0;
            lastScanActivity = millis();
            // Jangan delay(10000) blocking — cukup perpanjang interval via lastRetry
            lastRetry = millis();
          }
        }
      }
      delay(20);
    }
  }

  // Web client tiap iterasi — jangan di-throttle (request HTML besar butuh
  // banyak chunk TCP; kalau jarang di-poll, koneksi timeout / blank).
  wifiScanService();
  attnServiceUi();
  cacheEmpJobTick();
  if (dnsServerActive) dnsServer.processNextRequest();
  server.handleClient();

  // Housekeeping (lebih jarang)
  static uint8_t hkTick = 0;
  hkTick++;
  if (hkTick >= 2) {
    hkTick = 0;
    timeClient.update();
    checkAutoSleep();
    cacheBackgroundTick();

    // Auto-sync pending (register/attendance) sesuai uploadIntervalMinutes.
    static unsigned long lastAutoSync = 0;
    if (appSettings.uploadIntervalMinutes > 0 &&
        (lastAutoSync == 0 || millis() - lastAutoSync >= (unsigned long)appSettings.uploadIntervalMinutes * 60000UL)) {
      lastAutoSync = millis();
      if (pendingRegCount > 0 || pendingAttCount > 0) {
        Serial.printf("[SYNC] periodic trigger (pendingReg=%d pendingAtt=%d)\n",
                      pendingRegCount, pendingAttCount);
        syncRequestNow();
      }
    }

    // BLE status + pastikan advertising tetap hidup (selalu siap di-scan)
    static unsigned long lastBleStatus = 0;
    if (millis() - lastBleStatus > 5000) {
      lastBleStatus = millis();
      bleUpdateStatus();
      bleEnsureAdvertising();
    }

    // TX power saja — jangan ganti mode WiFi berkala (putus web UI)
    static unsigned long lastPowerPolicy = 0;
    if (millis() - lastPowerPolicy > 30000) {
      lastPowerPolicy = millis();
      wifiApplyPowerPolicy();

      // Sync flag jika STA putus sendiri (router mati, dll).
      if (wifiStaEverOk && !wifiApSetupMode && WiFi.status() != WL_CONNECTED) {
        wifiConnected = false;
        // JANGAN set wifiApSetupMode=true di sini — itu mematikan soft reconnect
        // (blok di bawah butuh !wifiApSetupMode). Soft reconnect / AP-only
        // setelah retry yang menata ulang mode.
      }

      // Soft reconnect HANYA jika sebelumnya sudah pernah STA sukses lalu putus.
      // Saat AP setup (belum pernah connect / boot gagal) → JANGAN paksa WiFi.begin
      // (itu yang bikin AP tidak stabil).
      if (wifiStaEverOk && !wifiApSetupMode && !wifiConnected && savedWiFiCount > 0) {
        static unsigned long lastWifiRetry = 0;
        static uint8_t wifiDropRetries = 0;
        if (millis() - lastWifiRetry > 60000) {
          lastWifiRetry = millis();
          wifiDropRetries++;
          Serial.printf("[WiFi] Soft reconnect after drop (%u/3)...\n", wifiDropRetries);
          wifiReconnect();
          if (wifiConnected) {
            wifiDropRetries = 0;
          } else if (wifiDropRetries >= 3) {
            // Setelah 3x gagal → kunci AP-only, biar user set ulang dari web
            wifiDropRetries = 0;
            wifiStaEverOk = false;
            wifiEnterApOnly("drop-retries-exhausted");
          }
        }
      } else if (wifiApSetupMode) {
        // Setup mode harus AP_STA (AP hidup + STA idle). WIFI_OFF/STA-only = rusak.
        // Pastikan tidak ada STA reconnect hang yang hop channel.
        if (WiFi.status() != WL_CONNECTED) {
          WiFi.disconnect(false);
        }
        wifi_mode_t m = WiFi.getMode();
        if (m != WIFI_AP_STA && m != WIFI_AP) {
          Serial.println("[WiFi] AP-setup guard: restore without client kick");
          wifiEnsureApAlive();
        } else if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
          WiFi.softAP(AP_SSID, cred.apPass);
        }
      }
    }

    // Mode scan idle > 60s: matikan backlight total.
    // Mode enroll / daftar / restore wajib tetap hidup sampai proses selesai atau dibatalkan.
    if (lcdBacklightOn && !enrollActive && !restoreActive && millis() - lastLcdActivity > LCD_IDLE_TIMEOUT_MS) {
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
