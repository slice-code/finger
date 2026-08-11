#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <NimBLEDevice.h>

// ── BLE UUID Definitions ───────────────────────────────────────────
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_STATUS_UUID    "4fafc202-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_COMMAND_UUID   "4fafc203-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_ENROLL_UUID    "4fafc204-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_DELETE_UUID    "4fafc205-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_SETTINGS_UUID  "4fafc206-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_EVENTS_UUID    "4fafc207-1fb5-459e-8fcc-c5c9c331914b"

struct AppSettings;
struct FPEntry;

extern bool autoScan;
extern bool sensorReady;
extern bool wifiConnected;
extern bool storageReady;
extern int fpCount;
extern AppSettings appSettings;
extern FPEntry fpDB[100];
extern int savedWiFiCount;
extern WiFiCreds savedWiFi[5];
extern bool wifiSaveCreds();

bool bleEnrollRequested = false;
bool bleDeleteRequested = false;
bool bleWifiSaveRequested = false;
char bleWifiSsid[33] = {0};
char bleWifiPass[65] = {0};
uint8_t bleDeleteId = 0;

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pStatusChar = nullptr;
NimBLECharacteristic *pEventChar = nullptr;
NimBLECharacteristic *pSettingsChar = nullptr;

bool settingsSave();
void bleStop();
void bleRestart();
void bleSaveWifi(const char *ssid, const char *pass);

void bleNotifyEvent(const char *json) {
  if (pEventChar) {
    pEventChar->setValue(json);
    pEventChar->notify();
  }
}

class BLECmdCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar) {
    String cmd = pChar->getValue().c_str();
    Serial.printf("[BLE] command: %s\n", cmd.c_str());
    cmd.trim();
    if (cmd.equalsIgnoreCase("AUTOSCAN ON")) {
      autoScan = true;
      bleNotifyEvent("{\"event\":\"autoscan\",\"active\":true}");
    } else if (cmd.equalsIgnoreCase("AUTOSCAN OFF")) {
      autoScan = false;
      bleNotifyEvent("{\"event\":\"autoscan\",\"active\":false}");
    }
  }
};

class BLEEnrollCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    Serial.printf("[BLE] enroll: %s\n", data.c_str());
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, data)) {
      const char *eid = doc["employeeId"] | "";
      const char *nm = doc["name"] | "";
      if (eid[0] && nm[0]) {
        File f = LittleFS.open("/ble_enroll.json", "w");
        if (f) { f.print(data); f.close(); }
        bleEnrollRequested = true;
      }
    }
  }
};

class BLEDeleteCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    Serial.printf("[BLE] delete: %s\n", data.c_str());
    int id = data.toInt();
    if (id > 0 && id <= 100) {
      bleDeleteId = (uint8_t)id;
      bleDeleteRequested = true;
    }
  }
};

class BLESettingsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar) {
    String data = pChar->getValue().c_str();
    Serial.printf("[BLE] settings: %s\n", data.c_str());
    if (!storageReady) return;
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, data)) {
      if (doc.containsKey("apiBaseUrl"))     strncpy(appSettings.apiBaseUrl, doc["apiBaseUrl"] | "", 127);
      if (doc.containsKey("kodeCabang"))     strncpy(appSettings.kodeCabang, doc["kodeCabang"] | "", 15);
      if (doc.containsKey("deviceId"))       strncpy(appSettings.deviceId, doc["deviceId"] | "", 31);
      if (doc.containsKey("apiKey"))         strncpy(appSettings.apiKey, doc["apiKey"] | "", 64);
      if (doc.containsKey("irEnabled"))      appSettings.irEnabled = doc["irEnabled"];
      if (doc.containsKey("scanSchedule"))   appSettings.scanSchedule = doc["scanSchedule"];
      if (doc.containsKey("scanStartHour"))  appSettings.scanStartHour = doc["scanStartHour"];
      if (doc.containsKey("scanEndHour"))    appSettings.scanEndHour = doc["scanEndHour"];
      // WiFi credentials via BLE
      if (doc.containsKey("wifiSsid") && doc.containsKey("wifiPass")) {
        const char *sid = doc["wifiSsid"] | "";
        const char *pw  = doc["wifiPass"] | "";
        if (sid[0]) {
          strncpy(bleWifiSsid, sid, 32);
          strncpy(bleWifiPass, pw, 63);
          bleWifiSaveRequested = true;
        }
      }
      settingsSave();
      if (pSettingsChar) {
        String out;
        serializeJson(doc, out);
        pSettingsChar->setValue(out.c_str());
      }
    }
  }
};

void bleInit() {
  BLEDevice::init("PJTKI-Finger");
  pServer = BLEDevice::createServer();

  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  // Status (read + notify) — NimBLE auto-adds CCCD
  pStatusChar = pService->createCharacteristic(
    BLE_CHAR_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  // Command (write)
  NimBLECharacteristic *pCmdChar = pService->createCharacteristic(
    BLE_CHAR_COMMAND_UUID, NIMBLE_PROPERTY::WRITE
  );
  pCmdChar->setCallbacks(new BLECmdCallback());

  // Enroll (write)
  NimBLECharacteristic *pEnrollChar = pService->createCharacteristic(
    BLE_CHAR_ENROLL_UUID, NIMBLE_PROPERTY::WRITE
  );
  pEnrollChar->setCallbacks(new BLEEnrollCallback());

  // Delete (write)
  NimBLECharacteristic *pDelChar = pService->createCharacteristic(
    BLE_CHAR_DELETE_UUID, NIMBLE_PROPERTY::WRITE
  );
  pDelChar->setCallbacks(new BLEDeleteCallback());

  // Settings (read + write)
  pSettingsChar = pService->createCharacteristic(
    BLE_CHAR_SETTINGS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  pSettingsChar->setCallbacks(new BLESettingsCallback());

  // Events (notify)
  pEventChar = pService->createCharacteristic(
    BLE_CHAR_EVENTS_UUID, NIMBLE_PROPERTY::NOTIFY
  );

  pService->start();

  NimBLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising started as 'PJTKI-Finger'");
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
  }
}

void bleRestart() {
  bleStop();
  bleInit();
}

void bleSaveWifi(const char *ssid, const char *pass) {
  if (!storageReady) return;
  if (savedWiFiCount >= 5) return;
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
  Serial.printf("[BLE] WiFi saved: %s\n", ssid);
}

#endif
