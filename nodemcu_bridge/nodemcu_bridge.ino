#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdarg.h>

// ── Web Auth ──────────────────────────────────────────────────────
#define WEB_USER "cks"
#define WEB_PASS "gugus$111$"

// ── Pin Mapping ────────────────────────────────────────────────────
// Finger sensor → SoftwareSerial: RX=D1(GPIO5), TX=D2(GPIO4)
// LCD: CS=D8, DC=D3, RST=D4, BL=D6, SCK=D5, MOSI=D7
#define FINGER_RX D1
#define FINGER_TX D2
#define LCD_BL    D6

// ── WiFi Config ────────────────────────────────────────────────────
#define AP_SSID "FPM10A-Bridge"
#define AP_PASS "gugus$111$"
#define WIFI_TIMEOUT_MS 10000
#define WIFI_FILENAME "/wifi.json"

// ── Objects ────────────────────────────────────────────────────────
SoftwareSerial altSerial(FINGER_RX, FINGER_TX);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&altSerial);
TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);

bool requireAuth() {
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
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
bool wifiConnected = false;
String staIP = "";
String staSSID = "";
unsigned long curBaud = 0;
char rxBuf[80];
uint8_t rxLen = 0;

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
};
AppSettings appSettings;

// ── Fingerprint DB (in-memory + LittleFS) ─────────────────────────
#define MAX_FP 100
struct FPEntry { uint8_t id; char name[32]; char empId[16]; };
FPEntry fpDB[MAX_FP];
int fpCount = 0;

// ── SSE ────────────────────────────────────────────────────────────
#define MAX_SSE_CLIENTS 4
WiFiClient sseClients[MAX_SSE_CLIENTS];

// ── Color Palette (LCD) ───────────────────────────────────────────
#define COL_BG        TFT_BLACK
#define COL_PANEL     0x0B12
#define COL_TOPBAR    0x0010
#define COL_TITLE     TFT_WHITE
#define COL_TEXT      TFT_WHITE
#define COL_DIM       0x5AE0
#define COL_DIM2      0x39E7
#define COL_OK        0x07E0
#define COL_OK_DARK   0x0320
#define COL_OK_GLOW   0x0400
#define COL_WARN      0xFFE0
#define COL_ERR       0xF800
#define COL_ERR_DARK  0x6000
#define COL_CYAN      0x07FF
#define COL_CYAN_DIM  0x039F
#define COL_FINGER    0x053F
#define COL_FINGER2   0x03EF
#define COL_SCAN      0x051F
#define COL_GOLD      0xFD20
#define COL_ACCENT    0x881F
#define COL_GRID      0x10A2
#define SCREEN_W 320
#define SCREEN_H 240

// ── Layout constants ──────────────────────────────────────────────
#define TOPBAR_H    28
#define FOOTER_Y    210
#define FOOTER_H    30
#define ICON_CY     85
#define STATUS_Y    150
#define RESULT_Y    145

// ────────────────────────────────────────────────────────────────────
//  LCD PRIMITIVES
// ────────────────────────────────────────────────────────────────────
void lcdProgressBar(int x, int y, int w, int h, int pct, uint16_t fg) {
  tft.fillRoundRect(x, y, w, h, 4, COL_PANEL);
  int fw = (w - 4) * pct / 100;
  if (fw > 0) tft.fillRoundRect(x + 2, y + 2, fw, h - 4, 3, fg);
}

void lcdDrawFingerprint(int cx, int cy, int s, uint16_t col, uint16_t ringCol) {
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
  int bw = tw + 24;
  tft.fillRoundRect(SCREEN_W / 2 - bw / 2, cy - 13, bw, 26, 8, bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.setTextSize(2);
  tft.drawString(txt, SCREEN_W / 2, cy);
}

// ────────────────────────────────────────────────────────────────────
//  TOP BAR
// ────────────────────────────────────────────────────────────────────
void lcdDrawTopbar() {
  tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_CYAN, COL_TOPBAR);
  tft.setTextSize(1);
  tft.drawString("FPM10A", 8, 9);
  tft.setTextDatum(TC_DATUM);
  if (autoScan) {
    tft.setTextColor(COL_OK, COL_TOPBAR);
    tft.drawString("SCAN", SCREEN_W / 2, 9);
  } else {
    tft.setTextColor(COL_DIM, COL_TOPBAR);
    tft.drawString("IDLE", SCREEN_W / 2, 9);
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%d FP", finger.templateCount);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_GOLD, COL_TOPBAR);
  tft.drawString(buf, SCREEN_W - 8, 9);
  tft.drawFastHLine(0, TOPBAR_H, SCREEN_W, COL_CYAN_DIM);
}

// ────────────────────────────────────────────────────────────────────
//  FOOTER (shows WiFi mode)
// ────────────────────────────────────────────────────────────────────
void lcdDrawFooter() {
  tft.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, COL_TOPBAR);
  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_CYAN_DIM);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  if (wifiConnected) {
    tft.setTextColor(COL_OK, COL_TOPBAR);
    tft.drawString("STA:" + staSSID, 8, FOOTER_Y + 4);
    tft.setTextColor(COL_DIM2, COL_TOPBAR);
    tft.drawString("IP:" + staIP, 8, FOOTER_Y + 15);
  } else {
    tft.setTextColor(COL_WARN, COL_TOPBAR);
    tft.drawString("AP:" AP_SSID, 8, FOOTER_Y + 4);
    tft.setTextColor(COL_DIM2, COL_TOPBAR);
    tft.drawString("IP:192.168.4.1", 8, FOOTER_Y + 15);
  }

  tft.setTextDatum(TR_DATUM);
  char buf[20];
  snprintf(buf, sizeof(buf), "Sec:%d", finger.security_level);
  tft.drawString(buf, SCREEN_W - 8, FOOTER_Y + 4);
  tft.drawString("9600 baud", SCREEN_W - 8, FOOTER_Y + 15);
}

// ────────────────────────────────────────────────────────────────────
//  BOOT SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowBoot() {
  tft.fillScreen(COL_BG);
  tft.drawFastHLine(40, 105, SCREEN_W - 80, COL_CYAN_DIM);
  tft.drawFastHLine(40, 107, SCREEN_W - 80, COL_CYAN_DIM);
  lcdDrawFingerprint(SCREEN_W / 2, 60, 20, COL_FINGER, COL_FINGER2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_CYAN, COL_BG);
  tft.setTextSize(2);
  tft.drawString("FPM10A Bridge", SCREEN_W / 2, 115);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.drawString("NodeMCU + WiFi + ILI9341", SCREEN_W / 2, 138);
  lcdProgressBar(60, 160, SCREEN_W - 120, 8, 0, COL_FINGER);
}

void lcdProgress(int pct) {
  lcdProgressBar(60, 160, SCREEN_W - 120, 8, pct, COL_FINGER);
}

// ────────────────────────────────────────────────────────────────────
//  MAIN IDLE SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowIdle() {
  tft.fillScreen(COL_BG);
  lcdDrawTopbar();
  lcdDrawFingerprint(SCREEN_W / 2, ICON_CY, 22, COL_FINGER, COL_FINGER2);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.drawString(autoScan ? "Scan mode - touch sensor" : "Place finger on sensor", SCREEN_W / 2, STATUS_Y);
  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  SCANNING ANIMATION
// ────────────────────────────────────────────────────────────────────
void lcdShowScanning() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  lcdDrawFingerprint(SCREEN_W / 2, ICON_CY, 22, COL_CYAN, COL_CYAN_DIM);
  lcdBadgeCenter(STATUS_Y, "SCANNING", COL_CYAN_DIM, COL_CYAN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Analyzing...", SCREEN_W / 2, STATUS_Y + 20);
}

// ────────────────────────────────────────────────────────────────────
//  MATCH RESULT
// ────────────────────────────────────────────────────────────────────
void lcdShowMatch(int id, int conf, const char *name) {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  tft.fillRoundRect(15, 50, SCREEN_W - 30, 140, 12, COL_OK_DARK);
  tft.drawRoundRect(15, 50, SCREEN_W - 30, 140, 12, COL_OK);
  tft.drawFastHLine(135, 82, 12, COL_OK);
  tft.drawFastHLine(136, 83, 12, COL_OK);
  tft.drawLine(144, 78, 152, 88, COL_OK);
  tft.drawLine(152, 88, 168, 72, COL_OK);
  tft.drawLine(152, 88, 168, 72, COL_OK);
  tft.drawLine(152, 89, 168, 73, COL_OK);
  lcdBadgeCenter(105, "MATCH", COL_OK, COL_BG);
  char buf[20];
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_OK_DARK);
  tft.setTextSize(2);
  snprintf(buf, sizeof(buf), "ID: %d", id);
  tft.drawString(buf, SCREEN_W / 2, 125);
  if (name && name[0]) {
    tft.setTextSize(1);
    tft.setTextColor(COL_CYAN, COL_OK_DARK);
    tft.drawString(name, SCREEN_W / 2, 145);
  }
  int confPct = conf * 100 / 256;
  lcdProgressBar(50, 162, SCREEN_W - 100, 8, confPct, COL_OK);
  snprintf(buf, sizeof(buf), "%d%%", confPct);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_OK, COL_BG);
  tft.setTextSize(1);
  tft.drawString(buf, SCREEN_W / 2, 175);
  lcdDrawTopbar();
  lcdDrawFooter();
}

void lcdShowAttendanceStatus(const char *status) {
  tft.fillRect(20, 180, SCREEN_W - 40, 10, COL_OK_DARK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(1);
  if (strcmp(status, "checkin") == 0) {
    tft.setTextColor(COL_OK, COL_OK_DARK);
    tft.drawString(">> ABSEN MASUK <<", SCREEN_W / 2, 183);
  } else if (strcmp(status, "checkout") == 0) {
    tft.setTextColor(COL_CYAN, COL_OK_DARK);
    tft.drawString(">> ABSEN PULANG <<", SCREEN_W / 2, 183);
  } else if (strcmp(status, "ignored") == 0) {
    tft.setTextColor(COL_WARN, COL_OK_DARK);
    tft.drawString("SUDAH ABSEN", SCREEN_W / 2, 183);
  } else {
    tft.setTextColor(COL_ERR, COL_OK_DARK);
    tft.drawString("GAGAL ABSEN", SCREEN_W / 2, 183);
  }
}

// ────────────────────────────────────────────────────────────────────
//  NO MATCH RESULT
// ────────────────────────────────────────────────────────────────────
void lcdShowNoMatch() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  tft.fillRoundRect(15, 60, SCREEN_W - 30, 110, 12, COL_ERR_DARK);
  tft.drawRoundRect(15, 60, SCREEN_W - 30, 110, 12, COL_ERR);
  tft.drawLine(145, 82, 165, 102, COL_ERR);
  tft.drawLine(165, 82, 145, 102, COL_ERR);
  tft.drawLine(146, 83, 166, 103, COL_ERR);
  tft.drawLine(166, 83, 146, 103, COL_ERR);
  lcdBadgeCenter(120, "NO MATCH", COL_ERR, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WARN, COL_ERR_DARK);
  tft.setTextSize(1);
  tft.drawString("Sidik jari tidak terdaftar", SCREEN_W / 2, 145);
  lcdDrawTopbar();
  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  SENSOR ERROR
// ────────────────────────────────────────────────────────────────────
void lcdShowSensorErr() {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, FOOTER_Y - TOPBAR_H - 1, COL_BG);
  tft.fillRoundRect(15, 70, SCREEN_W - 30, 90, 12, COL_ERR_DARK);
  tft.drawRoundRect(15, 70, SCREEN_W - 30, 90, 12, COL_ERR);
  lcdBadgeCenter(100, "SENSOR ERROR", COL_ERR, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WARN, COL_ERR_DARK);
  tft.setTextSize(1);
  tft.drawString("Periksa koneksi sensor", SCREEN_W / 2, 125);
  lcdDrawTopbar();
  lcdDrawFooter();
}

// ────────────────────────────────────────────────────────────────────
//  ENROLL SCREEN
// ────────────────────────────────────────────────────────────────────
void lcdShowEnrollTitle(uint8_t id) {
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
  tft.drawFastHLine(0, TOPBAR_H, SCREEN_W, COL_ACCENT);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_ACCENT, COL_TOPBAR);
  tft.setTextSize(2);
  char hdr[20];
  snprintf(hdr, sizeof(hdr), "ENROLL ID:%d", id);
  tft.drawString(hdr, SCREEN_W / 2, 9);
}

void lcdEnrollStep(const char *step, int pct, const char *detail, uint16_t col) {
  tft.fillRect(0, TOPBAR_H + 1, SCREEN_W, 100, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col, COL_BG);
  tft.setTextSize(2);
  tft.drawString(step, SCREEN_W / 2, 60);
  if (pct >= 0) lcdProgressBar(40, 90, SCREEN_W - 80, 8, pct, col);
  if (detail) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextSize(1);
    tft.drawString(detail, SCREEN_W / 2, 110);
  }
}

void lcdEnrollOk(const char *msg) {
  tft.fillRoundRect(30, 130, SCREEN_W - 60, 42, 8, COL_OK_DARK);
  tft.drawRoundRect(30, 130, SCREEN_W - 60, 42, 8, COL_OK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_OK, COL_OK_DARK);
  tft.setTextSize(2);
  tft.drawString(msg, SCREEN_W / 2, 148);
}

void lcdEnrollErr(const char *msg) {
  tft.fillRoundRect(30, 130, SCREEN_W - 60, 42, 8, COL_ERR_DARK);
  tft.drawRoundRect(30, 130, SCREEN_W - 60, 42, 8, COL_ERR);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_ERR_DARK);
  tft.setTextSize(2);
  tft.drawString(msg, SCREEN_W / 2, 148);
}

// ────────────────────────────────────────────────────────────────────
//  SERIAL + FINGER SENSOR HELPERS
// ────────────────────────────────────────────────────────────────────
void flushRX() { while (altSerial.available()) altSerial.read(); }

void emit(const __FlashStringHelper *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf_P(buf, sizeof(buf), (const char *)fmt, ap);
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
    flushRX();
    if (finger.getImage() == FINGERPRINT_NOFINGER) return true;
    delay(50); yield();
  }
  return false;
}

bool waitFinger() {
  unsigned long t = millis();
  while (millis() - t < 10000) {
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
      strncpy(savedWiFi[savedWiFiCount].pass, p, 64);
      savedWiFiCount++;
    }
  }
}

void wifiSaveCreds() {
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < savedWiFiCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = savedWiFi[i].ssid;
    obj["pass"] = savedWiFi[i].pass;
  }
  File f = LittleFS.open(WIFI_FILENAME, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

void wifiAddCreds(const char *ssid, const char *pass) {
  if (savedWiFiCount >= MAX_SAVED_WIFI) return;
  for (int i = 0; i < savedWiFiCount; i++) {
    if (strcmp(savedWiFi[i].ssid, ssid) == 0) {
      strncpy(savedWiFi[i].pass, pass, 64);
      wifiSaveCreds();
      return;
    }
  }
  strncpy(savedWiFi[savedWiFiCount].ssid, ssid, 32);
  strncpy(savedWiFi[savedWiFiCount].pass, pass, 64);
  savedWiFiCount++;
  wifiSaveCreds();
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
  LittleFS.remove(WIFI_FILENAME);
}

// ────────────────────────────────────────────────────────────────────
//  App Settings (apiBaseUrl, kodeCabang, deviceId)
// ────────────────────────────────────────────────────────────────────
void settingsLoad() {
  memset(&appSettings, 0, sizeof(appSettings));
  File f = LittleFS.open(SETTINGS_FILENAME, "r");
  if (!f) return;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
  strncpy(appSettings.kodeCabang, doc["kode_cabang"] | "", 15);
  strncpy(appSettings.deviceId, doc["device_id"] | "", 31);
}

void settingsSave() {
  DynamicJsonDocument doc(512);
  doc["apiBaseUrl"] = appSettings.apiBaseUrl;
  doc["kode_cabang"] = appSettings.kodeCabang;
  doc["device_id"] = appSettings.deviceId;
  File f = LittleFS.open(SETTINGS_FILENAME, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// ────────────────────────────────────────────────────────────────────
//  Backend API - POST attendance on fingerprint match
// ────────────────────────────────────────────────────────────────────
String postAttendance(const char *employeeId) {
  if (!appSettings.apiBaseUrl[0] || !employeeId || !employeeId[0]) return "";
  if (WiFi.status() != WL_CONNECTED) return "";

  String url = String(appSettings.apiBaseUrl) + "/api/finger/arduino/attendance";

  DynamicJsonDocument body(256);
  body["employeeId"] = employeeId;
  body["device_id"] = appSettings.deviceId;
  body["kode_cabang"] = appSettings.kodeCabang;
  // time as HH:MM:SS
  char timeBuf[12];
  unsigned long t = millis() / 1000;
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", (t / 3600) % 24, (t / 60) % 60, t % 60);
  body["time"] = timeBuf;

  String json;
  serializeJson(body, json);

  bool isHttps = url.startsWith("https://");
  WiFiClient *client;
  WiFiClientSecure *wc = nullptr;
  if (isHttps) {
    wc = new WiFiClientSecure();
    wc->setInsecure();
    client = wc;
  } else {
    client = new WiFiClient();
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(*client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  String resp = "";
  if (code > 0) {
    resp = http.getString();
  }
  http.end();
  delete client;
  return resp;
}

// ────────────────────────────────────────────────────────────────────
//  WiFi Manager - Init (AP always on, STA if credentials exist)
// ────────────────────────────────────────────────────────────────────
void wifiInit() {
  wifiLoadCreds();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP: %s | AP IP: 192.168.4.1\n", AP_SSID);

  if (savedWiFiCount == 0) {
    Serial.println("[WiFi] No saved credentials, AP only");
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
      Serial.printf("[WiFi] Connected to %s | IP: %s\n", savedWiFi[i].ssid, staIP.c_str());
      return;
    }
    WiFi.disconnect();
  }

  Serial.println("[WiFi] All saved networks failed, AP only");
}

// ────────────────────────────────────────────────────────────────────
//  ENROLLMENT
// ────────────────────────────────────────────────────────────────────
uint8_t enrollFinger(uint8_t id, const char *name, const char *empId) {
  int p = -1;
  enrollActive = true;
  flushRX();

  lcdShowEnrollTitle(id);
  lcdEnrollStep("Remove finger", -1, "Clear sensor first", COL_TEXT);
  emit(F("{\"event\":\"enroll_start\",\"id\":%d}"), id);
  if (!waitNoFinger()) { lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }

  for (int attempt = 0; attempt < 3; attempt++) {
    lcdEnrollStep("Place finger", 10, "Touch sensor gently", COL_WARN);
    emit(F("{\"event\":\"waiting_finger\"}"));
    if (!waitFinger()) { lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }

    lcdEnrollStep("Capturing...", 25, "Reading fingerprint", COL_CYAN);
    flushRX(); delay(200);
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
      lcdEnrollErr("Bad Image #1");
      emit(F("{\"event\":\"bad_image\",\"step\":1,\"code\":%d}"), p);
      if (!waitNoFinger()) { lcdEnrollErr("TIMEOUT"); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }
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
      autoScan = true;
      lcdShowIdle();
      enrollActive = false;
      return 0xFF;
    }

    lcdEnrollStep("Remove finger", 50, "Lift finger off sensor", COL_TEXT);
    emit(F("{\"event\":\"remove\"}"));
    if (!waitNoFinger()) { lcdEnrollErr("TIMEOUT"); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }

    lcdEnrollStep("Place again", 60, "Same finger, same spot", COL_WARN);
    emit(F("{\"event\":\"waiting_finger_2\"}"));
    if (!waitFinger()) { lcdEnrollErr("TIMEOUT"); emit(F("{\"event\":\"enroll_fail\",\"code\":-1}")); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }

    lcdEnrollStep("Capturing...", 75, "Reading fingerprint", COL_CYAN);
    flushRX(); delay(200);
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
      lcdEnrollErr("Bad Image #2");
      emit(F("{\"event\":\"bad_image\",\"step\":2,\"code\":%d}"), p);
      if (!waitNoFinger()) { lcdEnrollErr("TIMEOUT"); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }
      continue;
    }
    lcdEnrollStep("Step 2 OK", 85, "Second scan captured", COL_OK);
    emit(F("{\"event\":\"image_ok_step2\"}"));

    lcdEnrollStep("Building model...", 90, "Matching patterns", COL_CYAN);
    p = finger.createModel();
    if (p == FINGERPRINT_OK) break;

    char msg[24];
    snprintf(msg, sizeof(msg), "Retry %d/3", attempt + 1);
    lcdEnrollErr(msg);
    emit(F("{\"event\":\"retry_create\",\"attempt\":%d}"), attempt + 1);
    if (!waitNoFinger()) { lcdEnrollErr("TIMEOUT"); autoScan = true; lcdShowIdle(); enrollActive = false; return 0xFE; }
    if (attempt == 2) {
      lcdEnrollErr("Model FAILED");
      emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
      autoScan = true;
      lcdShowIdle();
      enrollActive = false;
      return p;
    }
  }

  lcdEnrollStep("Storing...", 95, "Saving to sensor", COL_CYAN);
  delay(100);
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    lcdEnrollErr("Store FAILED");
    emit(F("{\"event\":\"enroll_fail\",\"code\":%d}"), p);
    autoScan = true;
    lcdShowIdle();
    enrollActive = false;
    return p;
  }

  dbAdd(id, name, empId);

  lcdEnrollStep("DONE", 100, NULL, COL_OK);
  char msg[32];
  snprintf(msg, sizeof(msg), "ID:%d Enrolled", id);
  lcdEnrollOk(msg);
  emit(F("{\"event\":\"enrolled\",\"id\":%d,\"name\":\"%s\"}"), id, name);
  delay(2500);

  finger.getTemplateCount();
  fingerDown = false;
  autoScan = true;
  lcdShowIdle();
  enrollActive = false;
  waitNoFinger();
  return p;
}

// ────────────────────────────────────────────────────────────────────
//  AUTO-SCAN
// ────────────────────────────────────────────────────────────────────
void doAutoScan() {
  static uint8_t returnTimer = 0;

  if (returnTimer > 0) {
    returnTimer--;
    if (returnTimer == 0) {
      fingerDown = false;
      lcdShowIdle();
    }
    return;
  }

  flushRX();
  int p = finger.getImage();
  if (p == FINGERPRINT_OK) {
    if (fingerDown) return;
    lcdShowScanning();
    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
      lcdShowSensorErr();
      emit(F("{\"event\":\"autoscan_err\",\"step\":\"image2tz\",\"code\":%d}"), p);
      fingerDown = true; flushRX();
      returnTimer = 60;
      return;
    }
    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
      const char *nm = dbGetName(finger.fingerID);
      const char *eid = dbGetEmpId(finger.fingerID);
      lcdShowMatch(finger.fingerID, finger.confidence, nm);
      emit(F("{\"event\":\"match\",\"id\":%d,\"confidence\":%d,\"name\":\"%s\",\"employeeId\":\"%s\"}"),
           finger.fingerID, finger.confidence, nm, eid ? eid : "");
      // Post attendance to backend API
      if (wifiConnected && eid && eid[0]) {
        String resp = postAttendance(eid);
        if (resp.length() > 0) {
          DynamicJsonDocument doc(512);
          if (!deserializeJson(doc, resp)) {
            const char *st = doc["status"] | "error";
            lcdShowAttendanceStatus(st);
          }
          emit(F("{\"event\":\"attendance\",\"response\":%s}"), resp.c_str());
        } else {
          lcdShowAttendanceStatus("error");
        }
      }
    } else {
      lcdShowNoMatch();
      emit(F("{\"event\":\"nomatch\",\"code\":%d}"), p);
    }
    fingerDown = true; flushRX();
    returnTimer = 60;
  } else if (p == FINGERPRINT_NOFINGER) {
  } else {
    lcdShowSensorErr();
    emit(F("{\"event\":\"autoscan_err\",\"step\":\"getImage\",\"code\":%d}"), p);
    flushRX();
    returnTimer = 60;
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
  server.send_P(200, "text/html", INDEX_HTML);
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
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false");
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
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

  wifiLoadCreds();
  wifiAddCreds(ssid, pass);
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"saved_rebooting\"}");

  delay(500);
  ESP.restart();
}

void handleWifiReset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;

  wifiClearCreds();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"reset_rebooting\"}");

  delay(500);
  ESP.restart();
}

void handleWifiDelete() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;

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
  String json = "{\"apiBaseUrl\":\"" + String(appSettings.apiBaseUrl) + "\"";
  json += ",\"kode_cabang\":\"" + String(appSettings.kodeCabang) + "\"";
  json += ",\"device_id\":\"" + String(appSettings.deviceId) + "\"}";
  server.send(200, "application/json", json);
}

void handleSettingsSave() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (!requireAuth()) return;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }
  if (doc.containsKey("apiBaseUrl")) strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
  if (doc.containsKey("kode_cabang")) strncpy(appSettings.kodeCabang, doc["kode_cabang"] | "", 15);
  if (doc.containsKey("device_id")) strncpy(appSettings.deviceId, doc["device_id"] | "", 31);
  settingsSave();
  server.send(200, "application/json", "{\"ok\":true}");
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

  WiFiClient *client;
  WiFiClientSecure *wc = nullptr;
  if (isHttps) {
    wc = new WiFiClientSecure();
    wc->setInsecure();
    wc->setBufferSizes(1024, 1024);
    client = wc;
  } else {
    client = new WiFiClient();
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(*client, url)) {
    Serial.println("[API] begin() failed");
    delete client;
    return "";
  }
  httpCode = http.GET();
  Serial.print("[API] HTTP code: "); Serial.println(httpCode);
  String resp = "";
  if (httpCode > 0) {
    resp = http.getString();
    Serial.print("[API] Response len: "); Serial.println(resp.length());
  }
  http.end();
  delete client;
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

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  server.send(404, "application/json", "{\"error\":\"not_found\"}");
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
//  SETUP
// ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  delay(100);
  Serial.println("\n\n=== FPM10A Bridge Boot ===");
  Serial.print("Free heap: "); Serial.println(ESP.getFreeHeap());

  // LCD
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  lcdShowBoot();
  lcdProgress(10);

  // LittleFS
  LittleFS.begin();
  dbLoad();
  settingsLoad();

  lcdProgress(25);

  // WiFi Manager (AP always on + STA if saved)
  wifiInit();
  lcdProgress(35);

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
  server.onNotFound(handleNotFound);
  server.begin();

  lcdProgress(45);

  // Fingerprint sensor auto-detect (via SoftwareSerial D1/D2)
  bool ok = false;
  const unsigned long tryBauds[] = {9600, 57600, 19200, 38400, 115200};
  for (int i = 0; i < 5; i++) {
    altSerial.begin(tryBauds[i]);
    delay(200);
    flushRX();
    finger.begin(tryBauds[i]);
    delay(50);
    if (finger.verifyPassword()) { curBaud = tryBauds[i]; ok = true; lcdProgress(50 + i * 8); break; }
    lcdProgress(50 + i * 8);
    delay(50);
  }

  lcdProgress(80);

  finger.getParameters();
  if (ok) finger.setSecurityLevel(FINGERPRINT_SECURITY_LEVEL_3);
  finger.getParameters();

  if (ok && curBaud != 9600) {
    if (finger.setBaudRate(FINGERPRINT_BAUDRATE_9600) == FINGERPRINT_OK) {
      delay(200);
      altSerial.begin(9600);
      finger.begin(9600);
      delay(150);
      if (finger.verifyPassword()) curBaud = 9600;
    }
  }

  sensorReady = ok;
  finger.getTemplateCount();

  Serial.print("[SENSOR] ready=");
  Serial.print(ok ? "YES" : "NO");
  if (ok) { Serial.print(" baud="); Serial.print(curBaud); }
  Serial.print(" templates=");
  Serial.println(finger.templateCount);

  lcdProgress(100);
  delay(400);

  if (ok) {
    autoScan = true;
    emit(F("{\"event\":\"autoscan_on\"}"));
    lcdShowIdle();
  } else {
    tft.fillScreen(COL_BG);
    tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_TOPBAR);
    tft.drawFastHLine(0, TOPBAR_H, SCREEN_W, COL_ERR);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_ERR, COL_TOPBAR);
    tft.setTextSize(2);
    tft.drawString("ERROR", SCREEN_W / 2, 9);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_ERR, COL_BG);
    tft.setTextSize(2);
    tft.drawString("Sensor NOT found", SCREEN_W / 2, 80);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.setTextSize(1);
    tft.drawString("Check wiring!", SCREEN_W / 2, 110);
    lcdDrawFooter();
  }

  emit(F("{\"event\":\"ready\",\"found\":%s,\"baud\":%lu,\"security\":%d}"),
       ok ? "true" : "false", curBaud, finger.security_level);
}

// ────────────────────────────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  char line[80];
  if (nextLine(line, sizeof(line))) {
    broadcastSSE(line);
  }

  if (autoScan) {
    doAutoScan();
    delay(30);
  } else {
    delay(5);
  }
}
