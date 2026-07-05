#include <Adafruit_Fingerprint.h>
#include <AltSoftSerial.h>
#include <stdio.h>
#include <stdarg.h>

// AltSoftSerial pakai Timer1 hardware untuk RX (Input Capture).
// Pin FIX di Uno: RX = D8 (ICP1), TX = D9 (OC1A).
//   Sensor TX -> Arduino D8
//   Sensor RX -> Arduino D9
AltSoftSerial altSerial;
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&altSerial);

bool autoScan = false;
bool fingerDown = false;

// buffer baris perintah masuk (dinamis tanpa String)
char rxBuf[80];
uint8_t rxLen = 0;

void flushRX() {
  while (altSerial.available()) altSerial.read();
}

// printf-style emit; format di flash (F()) -> RAM global minim.
void emit(const __FlashStringHelper *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf_P(buf, sizeof(buf), (const char *)fmt, ap);
  va_end(ap);
  Serial.println(buf);
  Serial.flush();
}

// kumpul baris dari Serial hingga '\n'. true jika selesai (disalin ke out).
bool nextLine(char *out, size_t n) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      while (rxLen > 0 && (rxBuf[rxLen - 1] == ' ' || rxBuf[rxLen - 1] == '\r')) rxBuf[--rxLen] = 0;
      rxBuf[rxLen] = 0;
      rxLen = 0;
      strncpy(out, rxBuf, n);
      out[n - 1] = 0;
      return out[0] != 0;
    }
    if (c != '\r' && rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c;
  }
  return false;
}

void waitNoFinger() {
  while (true) {
    flushRX();
    int p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) break;
    delay(50);
  }
}

void waitFinger() {
  while (true) {
    flushRX();
    int p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    delay(50);
  }
}

uint8_t enrollFinger(uint8_t id) {
  int p = -1;
  flushRX();

  emit(F("{\"event\":\"waiting_clear\"}"));
  waitNoFinger();

  for (int attempt = 0; attempt < 3; attempt++) {
    emit(F("{\"event\":\"waiting_finger\"}"));
    waitFinger();

    delay(20);
    p = finger.image2Tz(1);
    emit(F("{\"event\":\"diag\",\"step\":\"image2Tz1\",\"code\":%d}"), p);
    if (p != FINGERPRINT_OK) {
      emit(F("{\"event\":\"bad_image\",\"step\":1,\"code\":%d}"), p);
      waitNoFinger();
      continue;
    }
    emit(F("{\"event\":\"image_ok_step1\"}"));

    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
      emit(F("{\"event\":\"already_registered\",\"id\":%d,\"confidence\":%d}"), finger.fingerID, finger.confidence);
      waitNoFinger();
      return 0xFF;
    }

    emit(F("{\"event\":\"remove\"}"));
    waitNoFinger();

    emit(F("{\"event\":\"waiting_finger_2\"}"));
    waitFinger();

    delay(20);
    p = finger.image2Tz(2);
    emit(F("{\"event\":\"diag\",\"step\":\"image2Tz2\",\"code\":%d}"), p);
    if (p != FINGERPRINT_OK) {
      emit(F("{\"event\":\"bad_image\",\"step\":2,\"code\":%d}"), p);
      waitNoFinger();
      continue;
    }
    emit(F("{\"event\":\"image_ok_step2\"}"));

    p = finger.createModel();
    emit(F("{\"event\":\"diag\",\"step\":\"createModel\",\"code\":%d}"), p);
    if (p == FINGERPRINT_OK) break;

    emit(F("{\"event\":\"retry_create\",\"attempt\":%d,\"code\":%d}"), attempt + 1, p);
    waitNoFinger();
    if (attempt == 2) {
      emit(F("{\"event\":\"err\",\"step\":\"create\",\"code\":%d}"), p);
      return p;
    }
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) { emit(F("{\"event\":\"err\",\"step\":\"store\",\"code\":%d}"), p); return p; }

  emit(F("{\"event\":\"enrolled\",\"id\":%d}"), id);
  waitNoFinger();
  return p;
}

void setup() {
  Serial.begin(9600);
  while (!Serial);
  delay(100);

  bool ok = false;
  const unsigned long tryBauds[] = {9600, 57600, 19200, 38400, 115200};
  unsigned long curBaud = 0;
  for (int i = 0; i < 5; i++) {
    altSerial.begin(tryBauds[i]);
    delay(200);
    flushRX();
    finger.begin(tryBauds[i]);
    delay(50);
    if (finger.verifyPassword()) { curBaud = tryBauds[i]; ok = true; break; }
    delay(50);
  }

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
  emit(F("{\"event\":\"ready\",\"found\":%s,\"baud\":%lu,\"security\":%d,\"uart\":\"altsoft\"}"),
       ok ? "true" : "false", curBaud, finger.security_level);
}

void doAutoScan() {
  flushRX();
  int p = finger.getImage();
  if (p == FINGERPRINT_OK) {
    if (fingerDown) return;
    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
      emit(F("{\"event\":\"autoscan_err\",\"step\":\"image2tz\",\"code\":%d}"), p);
      fingerDown = true;
      flushRX();
      return;
    }
    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
      emit(F("{\"event\":\"match\",\"id\":%d,\"confidence\":%d}"), finger.fingerID, finger.confidence);
    } else {
      emit(F("{\"event\":\"nomatch\",\"code\":%d}"), p);
    }
    fingerDown = true;
    flushRX();
  } else if (p == FINGERPRINT_NOFINGER) {
    fingerDown = false;
  } else {
    emit(F("{\"event\":\"autoscan_err\",\"step\":\"getImage\",\"code\":%d}"), p);
    flushRX();
  }
}

void handleCmd(const char *line) {
  if (!line[0]) return;
  if (strcmp_P(line, PSTR("PING")) == 0) {
    emit(F("{\"ok\":true,\"cmd\":\"ping\"}"));
  } else if (strcmp_P(line, PSTR("COUNT")) == 0) {
    finger.getTemplateCount();
    emit(F("{\"ok\":true,\"count\":%d}"), finger.templateCount);
  } else if (strcmp_P(line, PSTR("EMPTY")) == 0) {
    int p = finger.emptyDatabase();
    emit(F("{\"ok\":%s,\"code\":%d}"), p == FINGERPRINT_OK ? "true" : "false", p);
  } else if (strncmp_P(line, PSTR("ENROLL "), 7) == 0) {
    uint8_t id = (uint8_t)atoi(line + 7);
    if (id == 0) { emit(F("{\"ok\":false,\"error\":\"id_invalid\"}")); return; }
    enrollFinger(id);
  } else if (strcmp_P(line, PSTR("VERIFY")) == 0) {
    emit(F("{\"event\":\"place_for_verify\"}"));
    int p = -1;
    while (p != FINGERPRINT_OK) {
      flushRX();
      p = finger.getImage();
      if (p != FINGERPRINT_OK) { delay(50); continue; }
      p = finger.image2Tz();
      if (p != FINGERPRINT_OK && p != FINGERPRINT_NOFINGER) {
        emit(F("{\"ok\":false,\"error\":\"img\",\"code\":%d}"), p);
        return;
      }
    }
    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
      emit(F("{\"ok\":true,\"id\":%d,\"confidence\":%d}"), finger.fingerID, finger.confidence);
    } else {
      emit(F("{\"ok\":false,\"error\":\"no_match\",\"code\":%d}"), p);
    }
    while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);
  } else if (strncmp_P(line, PSTR("DELETE "), 7) == 0) {
    uint8_t id = (uint8_t)atoi(line + 7);
    int p = finger.deleteModel(id);
    emit(F("{\"ok\":%s,\"id\":%d,\"code\":%d}"), p == FINGERPRINT_OK ? "true" : "false", id, p);
  } else if (strcmp_P(line, PSTR("DETECT")) == 0) {
    int p = finger.getImage();
    emit(F("{\"ok\":true,\"placed\":%s}"), p == FINGERPRINT_OK ? "true" : "false");
  } else if (strcmp_P(line, PSTR("AUTO ON")) == 0) {
    autoScan = true;
    fingerDown = false;
    emit(F("{\"ok\":true,\"autoscan\":true}"));
  } else if (strcmp_P(line, PSTR("AUTO OFF")) == 0) {
    autoScan = false;
    emit(F("{\"ok\":true,\"autoscan\":false}"));
  } else {
    emit(F("{\"ok\":false,\"error\":\"unknown_cmd\"}"));
  }
}

void loop() {
  char line[80];
  if (nextLine(line, sizeof(line))) handleCmd(line);
  if (autoScan) {
    doAutoScan();
    delay(30);
  } else {
    delay(5); // hindari busy-wait (mencegah starvation ISR)
  }
}
