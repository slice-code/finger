#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// ── BLE UUID Definitions ───────────────────────────────────────────
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_STATUS_UUID    "4fafc202-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_COMMAND_UUID   "4fafc203-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_ENROLL_UUID    "4fafc204-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_DELETE_UUID    "4fafc205-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_SETTINGS_UUID  "4fafc206-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_EVENTS_UUID    "4fafc207-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_HISTORY_UUID   "4fafc208-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_ENROLL_LIST_UUID "4fafc209-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_DEVICE_NAME         "PJTKI-Finger-5V"

struct AppSettings;

extern bool autoScan;
extern bool sensorReady;
extern bool wifiConnected;
extern bool storageReady;
extern int fpCount;
extern AppSettings appSettings;
extern String staSSID;

extern bool bleEnrollRequested;
extern bool bleDeleteRequested;
extern bool bleWifiSaveRequested;
extern bool bleWifiScanRequested;
extern bool bleEnrollCancelRequested;
extern bool bleSyncTemplatesRequested;
extern bool bleDeleteEmpRequested;
extern char bleDeleteEmpId[40];
extern bool bleMarkSyncedRequested;
extern char bleMarkSyncedPayload[2048];
extern bool bleEnrollListPending;
extern uint8_t bleEnrollListMode;
extern uint8_t bleEnrollListId;
extern int16_t bleEnrollListPage;  // -1=LIST penuh, >=0=halaman LIST PAGE n
extern bool bleCleanFingersRequested;
extern char bleWifiSsid[33];
extern char bleWifiPass[65];
extern uint8_t bleDeleteId;
extern bool blePutTemplateWaitingHex;
extern bool blePutTemplateProcess;
extern char blePutTemplateEmpId[40];
extern char blePutTemplateName[64];
extern uint8_t blePutTemplatePreferId;
extern char blePutTemplateHex[513];
extern uint16_t blePutTemplateHexRecv;

extern NimBLEServer *pServer;
extern NimBLECharacteristic *pStatusChar;
extern NimBLECharacteristic *pEventChar;
extern NimBLECharacteristic *pSettingsChar;
extern NimBLECharacteristic *pEnrollListChar;
extern NimBLECharacteristic *pHistoryChar;

bool settingsSave();
void bleWakeLcd();
void bleWakeUi();
void sensorResumeIdle(const char *reason);
void bleStop();
void bleRestart();
void bleNotifyEvent(const char *json);
bool blePushEnrollHex(const char *hex);
void bleUpdateStatus();
void bleUpdateSettings();
void bleUpdateHistory();
void bleInit();
void bleEnsureAdvertising();
void syncRequestNow();

// NimBLE 2.x WAJIB: onWrite/onRead pakai (Characteristic*, ConnInfo&).
// Signature lama tanpa ConnInfo TIDAK override → write dari HP diabaikan diam-diam.

class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pSrv, NimBLEConnInfo &connInfo) override {
    Serial.printf("[BLE] client connected handle=%u\n", connInfo.getConnHandle());
    // Supervision timeout 400 (4s): kompromi stabil. Nilai 100 (1s) TERNYATA
    // membuat link churn parah — setiap jeda >1s ESP32 putus link, app
    // auto-reconnect, connect-putus berulang → tight loop advertising →
    // watchdog restart. 4s membuat deteksi link loss lebih lambat tapi jauh
    // lebih stabil (sama seperti firmware 5V yang sudah teruji).
    pSrv->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 400);
    // Hanya LCD — jangan nyalakan LED FPM10A (bikin scan macet).
    bleWakeLcd();
    bleUpdateSettings();
    bleUpdateStatus();
  }
  void onDisconnect(NimBLEServer *pSrv, NimBLEConnInfo &connInfo, int reason) override {
    (void)pSrv; (void)connInfo;
    Serial.printf("[BLE] client disconnected reason=%d — resume advertising\n", reason);
    delay(100);
    bleEnsureAdvertising();
  }
};

class BLECmdCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    String cmd = pChar->getValue();
    Serial.printf("[BLE] command: %s\n", cmd.c_str());
    cmd.trim();
    if (cmd.equalsIgnoreCase("AUTOSCAN ON")) {
      autoScan = true;
      bleNotifyEvent("{\"event\":\"autoscan\",\"active\":true}");
    } else if (cmd.equalsIgnoreCase("AUTOSCAN OFF")) {
      autoScan = false;
      bleNotifyEvent("{\"event\":\"autoscan\",\"active\":false}");
    } else if (cmd.equalsIgnoreCase("WIFI_SCAN") || cmd.equalsIgnoreCase("WIFI SCAN")) {
      bleWifiScanRequested = true;
      bleNotifyEvent("{\"event\":\"wifi_scan\",\"status\":\"queued\"}");
    } else if (cmd.equalsIgnoreCase("ENROLL_CANCEL") || cmd.equalsIgnoreCase("ENROLL CANCEL")) {
      bleEnrollCancelRequested = true;
      bleNotifyEvent("{\"event\":\"enroll_cancel_queued\"}");
      Serial.println("[BLE] enroll cancel requested");
    } else if (cmd.startsWith("SYNC_TEMPLATES") || cmd.startsWith("SYNC TEMPLATES")) {
      // Format: SYNC_TEMPLATES {"employeeIds":["A","B",...]} (max 30)
      // atau SYNC_TEMPLATES id1,id2,id3 (CSV).
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        String payload = cmd.substring(sp + 1);
        payload.trim();
        DynamicJsonDocument doc(4096);
        bool ok = false;
        if (payload.startsWith("{")) {
          if (!deserializeJson(doc, payload)) ok = true;
        } else {
          JsonArray arr = doc.to<JsonArray>();
          int start = 0;
          while (start < (int)payload.length()) {
            int comma = payload.indexOf(',', start);
            String tok = (comma >= 0) ? payload.substring(start, comma) : payload.substring(start);
            tok.trim();
            if (tok.length()) arr.add(tok);
            if (comma < 0) break;
            start = comma + 1;
          }
          ok = arr.size() > 0;
        }
        if (ok) {
          File bf = LittleFS.open("/ble_sync.json", "w");
          if (bf) {
            serializeJson(doc, bf);
            bf.close();
            bleSyncTemplatesRequested = true;
            bleNotifyEvent("{\"event\":\"sync_templates_queued\"}");
            Serial.println("[BLE] sync templates queued");
          } else {
            bleNotifyEvent("{\"event\":\"sync_templates_fail\",\"reason\":\"file\"}");
          }
        } else {
          bleNotifyEvent("{\"event\":\"sync_templates_fail\",\"reason\":\"bad_payload\"}");
        }
      } else {
        bleNotifyEvent("{\"event\":\"sync_templates_fail\",\"reason\":\"no_ids\"}");
      }
    } else if (cmd.startsWith("PUT_TEMPLATE ") || cmd.startsWith("PUT_TEMPLATE")) {
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        String payload = cmd.substring(sp + 1);
        payload.trim();
        if (payload.startsWith("{")) {
          DynamicJsonDocument doc(384);
          if (!deserializeJson(doc, payload)) {
            const char *emp = doc["employeeId"] | "";
            const char *nm = doc["name"] | "";
            int fid = doc["fingerId"] | 0;
            if (emp[0]) {
              strncpy(blePutTemplateEmpId, emp, sizeof(blePutTemplateEmpId) - 1);
              blePutTemplateEmpId[sizeof(blePutTemplateEmpId) - 1] = 0;
              strncpy(blePutTemplateName, nm, sizeof(blePutTemplateName) - 1);
              blePutTemplateName[sizeof(blePutTemplateName) - 1] = 0;
              blePutTemplatePreferId =
                  (fid > 0 && fid <= 100) ? (uint8_t)fid : 0;
              blePutTemplateHex[0] = 0;
              blePutTemplateHexRecv = 0;
              blePutTemplateWaitingHex = true;
              bleNotifyEvent("{\"event\":\"put_template_ready\"}");
              Serial.printf("[BLE] put_template meta emp=%s\n", blePutTemplateEmpId);
            } else {
              bleNotifyEvent("{\"event\":\"put_template_fail\",\"reason\":\"no_emp\"}");
            }
          } else {
            bleNotifyEvent("{\"event\":\"put_template_fail\",\"reason\":\"json\"}");
          }
        } else {
          bleNotifyEvent("{\"event\":\"put_template_fail\",\"reason\":\"bad_payload\"}");
        }
      } else {
        bleNotifyEvent("{\"event\":\"put_template_fail\",\"reason\":\"no_meta\"}");
      }
    } else if (cmd.equalsIgnoreCase("CLEAN_FINGERS") || cmd.equalsIgnoreCase("CLEAN FINGERS")) {
      bleCleanFingersRequested = true;
      bleNotifyEvent("{\"event\":\"clean_fingers_queued\"}");
      Serial.println("[BLE] clean fingers requested");
    } else if (cmd.startsWith("DELETE_EMP") || cmd.startsWith("DELETE EMP")) {
      // Format: DELETE_EMP <employeeId> — hapus finger by employeeId dari
      // sensor + DB lokal + server (hapus juga fingerprint_template).
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        String emp = cmd.substring(sp + 1);
        emp.trim();
        if (emp.length() && emp.length() < 40) {
          strncpy(bleDeleteEmpId, emp.c_str(), sizeof(bleDeleteEmpId) - 1);
          bleDeleteEmpId[sizeof(bleDeleteEmpId) - 1] = 0;
          bleDeleteEmpRequested = true;
          bleNotifyEvent("{\"event\":\"delete_emp_queued\"}");
          Serial.printf("[BLE] delete emp requested: %s\n", bleDeleteEmpId);
        } else {
          bleNotifyEvent("{\"event\":\"delete_emp_fail\",\"reason\":\"bad_emp\"}");
        }
      }
    } else if (cmd.startsWith("MARK_SYNCED") || cmd.startsWith("MARK SYNCED")) {
      // Format: MARK_SYNCED <employeeId>|<tanggal>|<jam>;<employeeId>|...;...
      // Tandai item riwayat lokal sebagai sudah ter-upload oleh app.
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        String payload = cmd.substring(sp + 1);
        payload.trim();
        if (payload.length()) {
          bleMarkSyncedRequested = true;
          strncpy(bleMarkSyncedPayload, payload.c_str(), sizeof(bleMarkSyncedPayload) - 1);
          bleMarkSyncedPayload[sizeof(bleMarkSyncedPayload) - 1] = 0;
          bleNotifyEvent("{\"event\":\"mark_synced_queued\"}");
          Serial.printf("[BLE] mark synced payload len=%u\n", (unsigned)payload.length());
        }
      }
    } else if (cmd.equalsIgnoreCase("SYNC_NOW") || cmd.equalsIgnoreCase("SYNC NOW")) {
      syncRequestNow();
      bleNotifyEvent("{\"event\":\"sync_now\",\"status\":\"queued\"}");
      Serial.println("[BLE] sync now requested");
    } else if (cmd.startsWith("SET_SYNC_INTERVAL") || cmd.startsWith("SET SYNC INTERVAL")) {
      int sp = cmd.lastIndexOf(' ');
      if (sp >= 0) {
        int v = atoi(cmd.c_str() + sp + 1);
        if (v < 5) v = 5; if (v > 1440) v = 1440;
        appSettings.uploadIntervalMinutes = (uint16_t)v;
        settingsSave();
        bleUpdateSettings();
        char evBuf[96];
        snprintf(evBuf, sizeof(evBuf), "{\"event\":\"sync_interval\",\"minutes\":%d}", v);
        bleNotifyEvent(evBuf);
        Serial.printf("[BLE] sync interval set to %d minutes\n", v);
      }
    }
  }
};

class BLEEnrollCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    String data = pChar->getValue();
    Serial.printf("[BLE] enroll: %s\n", data.c_str());
    if (!storageReady) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-2,\"reason\":\"storage\"}");
      return;
    }
    if (!sensorReady) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-6,\"reason\":\"sensor\"}");
      return;
    }
    // ⚠️ Jangan tumpuk enroll: kalau masih ada enroll berjalan (enrollActive)
    // atau restore/sync template sedang jalan, tolak yang baru — mencegah
    // dua operasi sensor bersamaan → UART bentrok → sensor error/macet.
    if (enrollActive || restoreActive || bleEnrollRequested) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-7,\"reason\":\"busy\"}");
      return;
    }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, data)) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-3,\"reason\":\"json\"}");
      return;
    }
    const char *eid = doc["employeeId"] | "";
    const char *nm = doc["name"] | "";
    if (!eid[0] || !nm[0]) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-4,\"reason\":\"missing_fields\"}");
      return;
    }
    File f = LittleFS.open("/ble_enroll.json", "w");
    if (!f) {
      bleNotifyEvent("{\"event\":\"enroll_fail\",\"code\":-5,\"reason\":\"file\"}");
      return;
    }
    f.print(data);
    f.close();
    autoScan = false;
    bleEnrollRequested = true;
    bleWakeLcd(); // LCD dulu; LED dinyalakan di enrollFinger setelah sensor di-reset
    bleNotifyEvent("{\"event\":\"enroll_queued\"}");
  }
};

class BLEDeleteCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    String data = pChar->getValue();
    Serial.printf("[BLE] delete: %s\n", data.c_str());
    int id = data.toInt();
    if (id > 0 && id <= 100) {
      bleDeleteId = (uint8_t)id;
      bleDeleteRequested = true;
    }
  }
};

class BLESettingsCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)pChar;
    (void)connInfo;
    bleUpdateSettings();
  }

  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    String data = pChar->getValue();
    Serial.printf("[BLE] settings: %s\n", data.c_str());
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, data)) {
      bleNotifyEvent("{\"event\":\"settings_fail\",\"reason\":\"json\"}");
      return;
    }

    if (doc.containsKey("wifiSsid")) {
      const char *sid = doc["wifiSsid"] | "";
      const char *pw = doc["wifiPass"] | "";
      if (sid[0]) {
        strncpy(bleWifiSsid, sid, 32);
        bleWifiSsid[32] = 0;
        strncpy(bleWifiPass, pw, 63);
        bleWifiPass[63] = 0;
        bleWifiSaveRequested = true;
      }
    }

    if (!storageReady) {
      if (bleWifiSaveRequested) {
        bleNotifyEvent("{\"event\":\"wifi_queued\"}");
      } else {
        bleNotifyEvent("{\"event\":\"settings_fail\",\"reason\":\"storage\"}");
      }
      return;
    }

    if (doc.containsKey("apiBaseUrl")) {
      strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
      appSettings.apiBaseUrl[127] = 0;
      // https:// → http:// (TLS di ESP32+BLE tidak stabil / sering GAGAL KIRIM)
      if (strncmp(appSettings.apiBaseUrl, "https://", 8) == 0 ||
          strncmp(appSettings.apiBaseUrl, "HTTPS://", 8) == 0) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "http://%s", appSettings.apiBaseUrl + 8);
        strncpy(appSettings.apiBaseUrl, tmp, 127);
        appSettings.apiBaseUrl[127] = 0;
        Serial.printf("[BLE] apiBaseUrl https->http: %s\n", appSettings.apiBaseUrl);
      }
    }
    if (doc.containsKey("kodeCabang")) {
      strncpy(appSettings.kodeCabang, doc["kodeCabang"] | "", 15);
      appSettings.kodeCabang[15] = 0;
    }
    if (doc.containsKey("deviceId")) {
      strncpy(appSettings.deviceId, doc["deviceId"] | "", 31);
      appSettings.deviceId[31] = 0;
    }
    if (doc.containsKey("apiKey")) {
      strncpy(appSettings.apiKey, doc["apiKey"] | "", 64);
      appSettings.apiKey[64] = 0;
    }
    if (doc.containsKey("irEnabled")) appSettings.irEnabled = doc["irEnabled"];
    if (doc.containsKey("scanSchedule")) appSettings.scanSchedule = doc["scanSchedule"];
    if (doc.containsKey("scanStartHour")) appSettings.scanStartHour = doc["scanStartHour"];
    if (doc.containsKey("scanEndHour")) appSettings.scanEndHour = doc["scanEndHour"];
    if (doc.containsKey("uploadIntervalMinutes")) {
      int v = doc["uploadIntervalMinutes"] | 120;
      if (v < 5) v = 5; if (v > 1440) v = 1440;
      appSettings.uploadIntervalMinutes = (uint16_t)v;
    }

    if (doc.containsKey("apiBaseUrl") || doc.containsKey("kodeCabang") ||
        doc.containsKey("deviceId") || doc.containsKey("apiKey") ||
        doc.containsKey("irEnabled") || doc.containsKey("scanSchedule") ||
        doc.containsKey("scanStartHour") || doc.containsKey("scanEndHour") ||
        doc.containsKey("uploadIntervalMinutes")) {
      settingsSave();
      bleNotifyEvent("{\"event\":\"settings_saved\",\"ok\":true}");
    }

    bleUpdateSettings();
  }
};

bool bleEnrollRequested = false;
bool bleDeleteRequested = false;
bool bleWifiSaveRequested = false;
bool bleWifiScanRequested = false;
bool bleEnrollCancelRequested = false;
bool bleSyncTemplatesRequested = false;
bool bleDeleteEmpRequested = false;
char bleDeleteEmpId[40] = {0};
bool bleMarkSyncedRequested = false;
char bleMarkSyncedPayload[2048] = {0};
bool bleEnrollListPending = false;
uint8_t bleEnrollListMode = 0;   // 0=list, 1=template, 2=pending_reg
uint8_t bleEnrollListId = 0;
int16_t bleEnrollListPage = -1;
bool bleCleanFingersRequested = false;
char bleWifiSsid[33] = {0};
char bleWifiPass[65] = {0};
uint8_t bleDeleteId = 0;
bool blePutTemplateWaitingHex = false;
bool blePutTemplateProcess = false;
char blePutTemplateEmpId[40] = {0};
char blePutTemplateName[64] = {0};
uint8_t blePutTemplatePreferId = 0;
char blePutTemplateHex[513] = {0};
uint16_t blePutTemplateHexRecv = 0;

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pStatusChar = nullptr;
NimBLECharacteristic *pEventChar = nullptr;
NimBLECharacteristic *pSettingsChar = nullptr;
NimBLECharacteristic *pEnrollListChar = nullptr;
NimBLECharacteristic *pHistoryChar = nullptr;

class BLEHistoryCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)pChar;
    (void)connInfo;
    bleUpdateHistory();
  }
};

// Enroll list char: WRITE "LIST" atau "GET_TEMPLATE <id>" → isi ke pEnrollListChar.
// READ mengembalikan hasil terakhir (daftar fingerprint atau hex template).
class BLEEnrollListCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    String cmd = pChar->getValue();
    // Hex 512 digit mentah setelah PUT_TEMPLATE meta (app Android).
    // Terima sekali (512B) atau bertahap (long-write BLE).
    if (blePutTemplateWaitingHex && cmd.length() > 0) {
      for (unsigned i = 0; i < cmd.length() && blePutTemplateHexRecv < 512; i++) {
        char c = cmd[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
          continue;
        }
        blePutTemplateHex[blePutTemplateHexRecv++] = c;
      }
      if (blePutTemplateHexRecv >= 512) {
        blePutTemplateHex[512] = 0;
        blePutTemplateWaitingHex = false;
        blePutTemplateHexRecv = 0;
        blePutTemplateProcess = true;
        bleNotifyEvent("{\"event\":\"put_template_queued\"}");
        Serial.println("[BLE] put_template hex received 512");
        return;
      }
      Serial.printf("[BLE] put_template hex partial %u/512\n",
                    (unsigned)blePutTemplateHexRecv);
      return;
    }
    cmd.trim();
    // Anti-tight-loop: lewati write yang sama berulang dalam 500ms (app
    // polling read bisa memicu onWrite dobel → proses berulang → banjir serial).
    static unsigned long lastCmdAt = 0;
    static String lastCmd = "";
    unsigned long now = millis();
    if (cmd == lastCmd && now - lastCmdAt < 500) {
      return;
    }
    lastCmd = cmd;
    lastCmdAt = now;
    Serial.printf("[BLE] enrolllist: %s\n", cmd.c_str());
    if (cmd.equalsIgnoreCase("LIST")) {
      bleEnrollListPage = -1;
      bleEnrollListPending = true;
      bleEnrollListMode = 0;
      bleNotifyEvent("{\"event\":\"enroll_list_queued\"}");
    } else if (cmd.startsWith("LIST PAGE ") || cmd.startsWith("LIST_PAGE ")) {
      // LIST PAGE n — metadata per halaman (≤4 item), muat batas BLE 512B.
      int sp = cmd.indexOf(' ');
      int sp2 = cmd.indexOf(' ', sp + 1);
      bleEnrollListPage = (sp2 >= 0) ? atoi(cmd.c_str() + sp2 + 1) : 0;
      if (bleEnrollListPage < 0) bleEnrollListPage = 0;
      bleEnrollListPending = true;
      bleEnrollListMode = 0;
      bleNotifyEvent("{\"event\":\"enroll_list_page_queued\"}");
    } else if (cmd.equalsIgnoreCase("PENDING_REG") || cmd.equalsIgnoreCase("PENDING REG")) {
      bleEnrollListPage = 0;
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        String rest = cmd.substring(sp + 1);
        rest.trim();
        if (rest.startsWith("PAGE ")) bleEnrollListPage = atoi(rest.c_str() + 5);
      }
      bleEnrollListPending = true;
      bleEnrollListMode = 2;
      bleNotifyEvent("{\"event\":\"pending_reg_queued\"}");
    } else if (cmd.startsWith("GET_TEMPLATE") || cmd.startsWith("GET TEMPLATE")) {
      int sp = cmd.indexOf(' ');
      if (sp >= 0) {
        bleEnrollListId = (uint8_t)atoi(cmd.c_str() + sp + 1);
        bleEnrollListPending = true;
        bleEnrollListMode = 1;
        bleNotifyEvent("{\"event\":\"enroll_template_queued\"}");
      }
    }
  }
};

void bleNotifyEvent(const char *json) {
  if (pEventChar) {
    pEventChar->setValue(json);
    pEventChar->notify();
  }
}

// Hex 512 char MENTAH ke char enroll-list — jangan bungkus JSON (ATT max 512).
// App baca characteristic ini saat event "enrolled", tanpa GET_TEMPLATE.
bool blePushEnrollHex(const char *hex) {
  if (!pEnrollListChar || !hex || strlen(hex) != 512) return false;
  pEnrollListChar->setValue((const uint8_t *)hex, 512);
  const bool connected = pServer && pServer->getConnectedCount() > 0;
  Serial.printf("[BLE] enroll hex pushed 512 connected=%d\n", connected ? 1 : 0);
  return connected;
}

void bleUpdateSettings() {
  if (!pSettingsChar) return;
  DynamicJsonDocument doc(768);
  doc["apiBaseUrl"] = appSettings.apiBaseUrl;
  doc["kodeCabang"] = appSettings.kodeCabang;
  doc["deviceId"] = appSettings.deviceId;
  doc["apiKey"] = appSettings.apiKey;
  doc["irEnabled"] = appSettings.irEnabled;
  doc["scanSchedule"] = appSettings.scanSchedule;
  doc["scanStartHour"] = appSettings.scanStartHour;
  doc["scanEndHour"] = appSettings.scanEndHour;
  doc["uploadIntervalMinutes"] = appSettings.uploadIntervalMinutes;
  if (staSSID.length()) doc["wifiSsid"] = staSSID;
  String out;
  serializeJson(doc, out);
  pSettingsChar->setValue(out.c_str());
}

void bleEnsureAdvertising() {
  if (!pServer) return;
  NimBLEAdvertising *pAdv = BLEDevice::getAdvertising();
  if (!pAdv) return;
  if (!pAdv->isAdvertising()) {
    // Cooldown anti-tight-loop: jangan panggil startAdvertising lebih dari
    // 1×/2 detik. Tanpa ini, jika isAdvertising() tidak pulih (mis. NimBLE
    // stuck saat churn), loop utama bisa memicu watchdog → ESP32 restart.
    static unsigned long lastEnsure = 0;
    unsigned long now = millis();
    if (now - lastEnsure < 2000) return;
    lastEnsure = now;
    BLEDevice::startAdvertising();
    Serial.println("[BLE] advertising resumed");
  }
}

void bleInit() {
  BLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  // Power TX tinggi agar koneksi HP stabil (WiFi+BLE coexist).
  NimBLEDevice::setPower(9);  // ~+9 dBm

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BleServerCallbacks());
  // Setelah disconnect, NimBLE otomatis advertise lagi — BLE tetap "menyala".
  pServer->advertiseOnDisconnect(true);

  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  pStatusChar = pService->createCharacteristic(
    BLE_CHAR_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  NimBLECharacteristic *pCmdChar = pService->createCharacteristic(
    BLE_CHAR_COMMAND_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pCmdChar->setCallbacks(new BLECmdCallback());

  NimBLECharacteristic *pEnrollChar = pService->createCharacteristic(
    BLE_CHAR_ENROLL_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pEnrollChar->setCallbacks(new BLEEnrollCallback());

  NimBLECharacteristic *pDelChar = pService->createCharacteristic(
    BLE_CHAR_DELETE_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pDelChar->setCallbacks(new BLEDeleteCallback());

  pSettingsChar = pService->createCharacteristic(
    BLE_CHAR_SETTINGS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pSettingsChar->setCallbacks(new BLESettingsCallback());

  pEventChar = pService->createCharacteristic(
    BLE_CHAR_EVENTS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  // Riwayat absensi lokal (baca dari app Android via BLE)
  pHistoryChar = pService->createCharacteristic(
    BLE_CHAR_HISTORY_UUID,
    NIMBLE_PROPERTY::READ
  );
  pHistoryChar->setCallbacks(new BLEHistoryCallback());

  // Daftar enroll + template hex (baca dari app Android via BLE)
  pEnrollListChar = pService->createCharacteristic(
    BLE_CHAR_ENROLL_LIST_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pEnrollListChar->setCallbacks(new BLEEnrollListCallback());

  pService->start();
  bleUpdateSettings();

  NimBLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setName(BLE_DEVICE_NAME);
  pAdv->setMinInterval(160);   // 100ms
  pAdv->setMaxInterval(240);   // 150ms — mudah ditemukan HP
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] Advertising started as '%s' (always-on)\n", BLE_DEVICE_NAME);
}

void bleUpdateStatus() {
  if (!pStatusChar) return;
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ready\":%s,\"autoActive\":%s,\"count\":%d,\"wifiMode\":\"%s\","
    "\"temp\":%d,\"sensorReady\":%s,\"irEnabled\":%s}",
    (fpCount > 0 || sensorReady) ? "true" : "false",
    autoScan ? "true" : "false",
    fpCount,
    wifiConnected ? "STA" : "AP",
    (int)temperatureRead(),
    sensorReady ? "true" : "false",
    appSettings.irEnabled ? "true" : "false"
  );
  pStatusChar->setValue(buf);
  pStatusChar->notify();
}

void bleStop() {
  if (pServer) {
    BLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    pServer = nullptr;
    pStatusChar = nullptr;
    pEventChar = nullptr;
    pSettingsChar = nullptr;
    pHistoryChar = nullptr;
    pEnrollListChar = nullptr;
  }
}

void bleRestart() {
  bleStop();
  bleInit();
}

#endif
