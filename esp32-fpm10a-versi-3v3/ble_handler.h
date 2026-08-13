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
extern char bleWifiSsid[33];
extern char bleWifiPass[65];
extern uint8_t bleDeleteId;

extern NimBLEServer *pServer;
extern NimBLECharacteristic *pStatusChar;
extern NimBLECharacteristic *pEventChar;
extern NimBLECharacteristic *pSettingsChar;
extern NimBLECharacteristic *pHistoryChar;

bool settingsSave();
void bleWakeLcd();
void bleWakeUi();
void sensorResumeIdle(const char *reason);
void bleStop();
void bleRestart();
void bleNotifyEvent(const char *json);
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
    if (doc.containsKey("apEnabled")) appSettings.apEnabled = doc["apEnabled"];
    if (doc.containsKey("uploadIntervalMinutes")) {
      int v = doc["uploadIntervalMinutes"] | 120;
      if (v < 5) v = 5; if (v > 1440) v = 1440;
      appSettings.uploadIntervalMinutes = (uint16_t)v;
    }

    if (doc.containsKey("apiBaseUrl") || doc.containsKey("kodeCabang") ||
        doc.containsKey("deviceId") || doc.containsKey("apiKey") ||
        doc.containsKey("irEnabled") || doc.containsKey("scanSchedule") ||
        doc.containsKey("scanStartHour") || doc.containsKey("scanEndHour") ||
        doc.containsKey("apEnabled") || doc.containsKey("uploadIntervalMinutes")) {
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
char bleWifiSsid[33] = {0};
char bleWifiPass[65] = {0};
uint8_t bleDeleteId = 0;

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pStatusChar = nullptr;
NimBLECharacteristic *pEventChar = nullptr;
NimBLECharacteristic *pSettingsChar = nullptr;
NimBLECharacteristic *pHistoryChar = nullptr;

class BLEHistoryCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    (void)pChar;
    (void)connInfo;
    bleUpdateHistory();
  }
};

void bleNotifyEvent(const char *json) {
  if (pEventChar) {
    pEventChar->setValue(json);
    pEventChar->notify();
  }
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
  doc["apEnabled"] = appSettings.apEnabled;
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
  BLEDevice::init("PJTKI-Finger");
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

  pService->start();
  bleUpdateSettings();

  NimBLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setName("PJTKI-Finger");
  pAdv->setMinInterval(160);   // 100ms
  pAdv->setMaxInterval(240);   // 150ms — mudah ditemukan HP
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising started as 'PJTKI-Finger' (always-on)");
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
  }
}

void bleRestart() {
  bleStop();
  bleInit();
}

#endif
