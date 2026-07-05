# API Server — Absensi Fingerprint

## Tech stack

- **Server**: Node.js (http murni, tanpa Express) via `server.js`
- **Database**: PostgreSQL / SQLite (sql.js) via `database.js`
- **Enkripsi**: AES-256-ECB + HMAC-SHA256
- **Transport**: HTTPS REST (JSON)

## Client Types

Ada 2 tipe client yang menggunakan API ini:

1. **Android App** - Menggunakan hash-based fingerprint matching (simulasi biometric)
2. **Arduino + Electron App** - Menggunakan server-side template matching (FPM10A sensor)

**Arsitektur Arduino + Electron (Server-Side Matching):**
- Arduino: Hanya sebagai fingerprint reader, kirim template hex ke Electron
- Electron: Sync data karyawan dari API, kirim template ke server saat enroll/scan
- Server: Simpan template di PostgreSQL, lakukan matching di server-side

Dokumentasi ini mencakup kedua tipe client.

---

## Table of contents

1. [Complete API flow](#complete-api-flow)
2. [Auto-detect scan mechanism](#auto-detect-scan-mechanism)
3. [Arduino + Electron Integration](#arduino--electron-integration)
4. [`GET /api/finger/branches`](#1-get-apifingerbranches)
5. [`GET /api/finger/employees`](#2-get-apifingeremployees)
6. [`POST /api/finger/register`](#3-post-apifingerregister)
7. [`POST /api/finger/attendance`](#4-post-apifingerattendance)
8. [`GET /api/finger/arduino/sync`](#5-get-apifingerarduinossync)
9. [`POST /api/finger/arduino/template`](#6-post-apifingerarduinotemplate)
10. [`POST /api/finger/arduino/attendance`](#7-post-apifingerarduinoattendance)
11. [Encryption & decryption](#encryption--decryption-flow)
12. [Server-side template matching](#server-side-template-matching)
13. [Database schema](#database-schema)
14. [Aturan 2x fingerprint per hari](#aturan-2x-fingerprint-per-hari)
15. [Client defaults](#client-defaults)
16. [Security notes](#security-notes)

---

## Complete API flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    ANDROID APP                                   │
│                                                                  │
│  REGISTRASI:                                                     │
│    GET /api/finger/branches  ─────────────────►  Server returns  │
│    (ambil daftar cabang aktif)               └── [{kode, nama}]  │
│         │                                                       │
│    User pilih cabang                                             │
│         │                                                       │
│    GET /api/finger/employees?kode_cabang=X ─►  Server returns   │
│    (ambil daftar TKI per cabang)            └── [{id, nama,      │
│                                                 finger_terdaftar}]│
│         │                                                       │
│    User pilih TKI + biometric                                    │
│         │                                                       │
│    POST /api/finger/register  ────────────►  Server stores       │
│    AES({employeeId, fingerprintHash})      └── fingerprint_hash  │
│                                            └── ke personal table │
│         │                                                       │
│    ◄── {status: "ok", message: "registered"}                    │
│    ◄── {status: "already_registered", ...}                       │
│                                                                  │
│  SCAN/ABSENSI (auto-detect):                                     │
│    GET /api/finger/employees?kode_cabang=X ─►  Dapat daftar TKI  │
│    (hanya yang finger_terdaftar = true)      └── yg terdaftar    │
│         │                                                       │
│    User letakkan jari (BiometricPrompt)                          │
│         │                                                       │
│    App iterate semua TKI terdaftar:                              │
│      FOR EACH employee IN registered:                            │
│        hash = SHA-256("FINGERPRINT_TEMPLATE_V1|{employeeId}")    │
│        POST /api/finger/attendance  ──────►  Server matches      │
│        AES({fingerprintHash})              └── hash → personal   │
│        IF response != "not_found": STOP    └── record ke         │
│                                              blk_absensi         │
│         │                                                       │
│    ◄── {status: "checkin", type: "masuk", ...}                  │
│    ◄── {status: "checkout", type: "pulang", ...}                │
│    ◄── {status: "ignored", ...}  (jika sudah 2x)                │
│    ◌── "Sidik jari tidak dikenal" (setelah coba semua)          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Auto-detect scan mechanism

App Android menggunakan mekanisme **auto-detect**: user cukup letakkan jari, app akan mencoba mencocokkan hash fingerprint **satu per satu** ke setiap TKI yang sudah terdaftar di cabang tersebut.

### Cara kerja

```
1. App ambil daftar TKI dari GET /api/finger/employees?kode_cabang=X
2. Filter hanya yang finger_terdaftar = true  →  registeredEmployees[]
3. User letakkan jari → BiometricPrompt verifikasi device-level
4. App loop melalui setiap registeredEmployees:
   a. Generate hash = SHA-256("FINGERPRINT_TEMPLATE_V1|{employeeId}")
   b. POST /api/finger/attendance dengan hash tersebut
   c. Server respons:
      - "checkin"/"checkout" → BERHASIL, stop loop
      - "ignored"            → SUDAH LENGKAP, stop loop
      - "not_found"          → lanjut ke employee berikutnya
5. Jika semua employee menghasilkan "not_found" → "Sidik jari tidak dikenal"
```

### Implikasi untuk server

- **Setiap scan menghasilkan 1 hingga N request** `POST /api/finger/attendance` (N = jumlah TKI terdaftar di cabang)
- Server tidak perlu endpoint baru — logic yang sama, hanya dipanggil berkali-kali
- **PENTING**: Pastikan server bisa menangani **burst request** (contoh: 10 TKI terdaftar = 10 request berturut-turut dalam 1-2 detik)
- **Rate limiting**: jangan batasi berdasarkan IP karena request datang dari device yang sama
- Response `not_found` HARUS cepat (langsung return, jangan INSERT apa pun)

### Optimasi yang disarankan (optional)

Untuk performa lebih baik, bisa tambahkan endpoint batch:

```
POST /api/finger/attendance/batch
Body decrypted: { "fingerprintHashes": ["hash1", "hash2", ...] }
Response: match pertama yang ditemukan (atau not_found)
```

Tapi **tidak wajib** — flow satu-per-satu di atas sudah berfungsi dengan baik.

---

## Arduino + Electron Integration

Arduino Uno + FPM10A fingerprint sensor digunakan sebagai hardware fingerprint reader. Electron app berjalan di PC/laptop dan berkomunikasi dengan Arduino via USB serial.

### Architecture (Server-Side Template Matching)

```
┌─────────────────────────────────────────────────────────────────┐
│                    ARDUINO + ELECTRON APP                       │
│                                                                  │
│  HARDWARE:                                                       │
│    Arduino Uno + FPM10A Sensor                                   │
│    - Hanya sebagai fingerprint reader                           │
│    - Kirim template hex ke Electron saat jari ditempel          │
│    - Tidak melakukan matching on-sensor                         │
│                                                                  │
│  ELECTRON APP:                                                   │
│    - Sync data karyawan dari API server                         │
│    - Terima template hex dari Arduino                           │
│    - Kirim template ke server untuk registrasi/attendance         │
│    - Local SQLite untuk cache (optional)                         │
│                                                                  │
│  SERVER (Node.js + PostgreSQL):                                  │
│    - Simpan template fingerprint di database                    │
│    - Lakukan matching server-side                               │
│    - Catat attendance ke blk_absensi                            │
│                                                                  │
│  SYNC DATA KARYAWAN:                                             │
│    GET /api/finger/arduino/sync  ─────────────►  Dapat daftar   │
│    {kode_cabang}                              karyawan dari      │
│                                             server              │
│         │                                                       │
│    Electron simpan ke local SQLite                             │
│         │                                                       │
│  REGISTRASI FINGERPRINT:                                        │
│    User pilih TKI dari local list                               │
│    STORE:<id> ke Arduino  ───────────────────►  Ambil template  │
│    Arduino kirim T:<hex_template>                              │
│    POST /api/finger/arduino/template  ──────►  Simpan template  │
│    {employeeId, templateHex}                 ke PostgreSQL      │
│         │                                                       │
│    ◄── {status: "ok", message: "template saved"}               │
│                                                                  │
│  SCAN/ABSENSI:                                                   │
│    Jari ditempel → Arduino kirim T:<hex_template>              │
│    Electron terima template                                     │
│    POST /api/finger/arduino/attendance  ─────►  Server match     │
│    {templateHex, device_id, kode_cabang}       template & record│
│         │                                                       │
│    ◄── {status: "checkin", type: "masuk", employeeId, name}     │
│    ◄── {status: "checkout", type: "pulang", employeeId, name}    │
│    ◄── {status: "not_found"}  (tidak ada match)                  │
└─────────────────────────────────────────────────────────────────┘
```

### Arduino Serial Protocol

| Format | Keterangan |
|--------|------------|
| `RDY:<count>` | Sensor siap, count = jumlah template tersimpan di Arduino (untuk debugging) |
| `T:<hex_template>` | Template fingerprint hex (512 byte = 1024 hex chars) - auto-push saat jari ditempel |
| `OK:<id>:<count>` | Operasi berhasil (STORE/DELETE), count = total template |
| `ERR:<code>` | Error (SENSOR, TIMEOUT, BAD_ID, dll) |
| `WAIT_FINGER` | Menunggu jari untuk enrollment |

### Wiring (FPM10A → Arduino Uno)

| Kabel FPM10A (warna) | Pin Arduino Uno |
|-----------------------|-------------------|
| VCC (Merah/Red)       | **3.3V**          |
| TX  (Kuning/Yellow)   | **D2**            |
| RX  (Hijau/Green)     | **D3**            |
| GND (Hitam/Black)     | **GND**           |

### Local Database (Electron - Optional Cache)

Electron app menggunakan SQLite untuk cache data karyawan lokal (untuk offline capability):

```sql
CREATE TABLE employees (
  id TEXT PRIMARY KEY,  -- id_biodata dari server
  name TEXT NOT NULL,
  kode_cabang TEXT,
  kode_sektor TEXT,
  has_template BOOLEAN DEFAULT false,
  synced_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE attendance_local (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  employee_id TEXT NOT NULL,
  name TEXT NOT NULL,
  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
  synced BOOLEAN DEFAULT false,
  FOREIGN KEY (employee_id) REFERENCES employees(id)
);
```

### Sync dengan Server Utama

Electron app sync data karyawan dan kirim template ke server via API:

- **Sync employees**: GET `/api/finger/arduino/sync?kode_cabang=X`
- **Register template**: POST `/api/finger/arduino/template`
- **Attendance**: POST `/api/finger/arduino/attendance`

### Kelebihan Server-Side Matching

- **Unlimited templates**: Tidak terbatas kapasitas sensor (disimpan di PostgreSQL)
- **Centralized data**: Data fingerprint terpusat di server
- **Scalable**: Bisa tambah lokasi tanpa batasan hardware
- **Backup**: Data fingerprint terbackup di server
- **Analytics**: Bisa analisis data fingerprint di server

### Keterbatasan

- **Requires internet**: Butuh koneksi internet untuk sync dan attendance
- **Server dependency**: Server harus selalu available
- **Template extraction**: Butuh library khusus untuk ekstrak template dari FPM10A

---

## 1. `GET /api/finger/branches`

Mengembalikan daftar cabang aktif untuk dipilih di app sebelum registrasi.

| Item | Value |
|---|---|
| Method | `GET` |
| Full path | `/api/finger/branches` |
| Auth | Tidak |

### Response `200 OK`

```json
{
  "success": true,
  "data": [
    { "kode_cabang": "CKS",  "nama_cabang": "Malang",     "kota": "Malang" },
    { "kode_cabang": "CKSTA","nama_cabang": "Tulungagung", "kota": "Tulungagung" }
  ]
}
```

### Flow app Android

1. App memanggil `GET /api/finger/branches` → dapat daftar cabang
2. User memilih cabang dari dropdown
3. App memanggil `GET /api/finger/employees?kode_cabang=CKS` → dapat daftar TKI cabang tersebut
4. User memilih TKI dan scan sidik jari
5. App memanggil `POST /api/finger/register` → daftar fingerprint

---

## 2. `GET /api/finger/employees`

Mengembalikan daftar TKI aktif untuk layar Registrasi fingerprint.

| Item | Value |
|---|---|
| Method | `GET` |
| Full path | `/api/finger/employees` |
| Auth | Tidak (endpoint publik dalam handler fingerprint) |

### Query parameters

| Param | Required | Default | Description |
|---|---|---|---|
| `kode_cabang` | Optional | — | Filter TKI berdasarkan kode cabang. Jika kosong, tampilkan semua cabang. |

Contoh: `GET /api/finger/employees?kode_cabang=CKSTA` → hanya TKI cabang CKSTA.

### Response `200 OK`

```json
[
  {
    "id": "MLG-FF-0001",
    "nama": "Ani Rahayu",
    "finger_terdaftar": true
  },
  {
    "id": "MLG-FF-0002",
    "nama": "Budi Santoso",
    "finger_terdaftar": false
  }
]
```

`finger_terdaftar`: `true` jika TKI sudah registrasi fingerprint, `false` jika belum.

### Server logic

```sql
SELECT id_biodata AS id, nama,
       (fingerprint_hash IS NOT NULL AND fingerprint_hash != '') AS finger_terdaftar
FROM personal
WHERE is_active = 1 AND arsip_status = 'active'
ORDER BY nama
```

---

## 3. `POST /api/finger/register`

Mendaftarkan hash fingerprint untuk seorang TKI.

### Request

| Header | Value |
|---|---|
| `Content-Type` | `application/json` |

**Body:**

```json
{
  "encryptedPayload": "r7Yx...base64...==",
  "signature": "aB3d...base64...=="
}
```

**Decrypted payload:**

```json
{
  "employeeId": "MLG-FF-0001",
  "fingerprintHash": "a1b2c3d4e5f6..."
}
```

### Responses

| Status | Meaning |
|---|---|
| `200 OK` | Fingerprint registered |
| `200 OK` `already_registered` | Fingerprint sudah terdaftar untuk TKI ini |
| `200 OK` `used_by_other` | Fingerprint sudah dipakai TKI lain (tidak bisa daftar ganda) |
| `401 Unauthorized` | HMAC signature invalid |
| `422 Unprocessable` | Missing or invalid fields |

**Success `200` (baru terdaftar):**
```json
{ "status": "ok", "message": "registered" }
```

**Already registered `200` (sudah terdaftar untuk TKI yang sama):**
```json
{
  "status": "already_registered",
  "message": "Fingerprint sudah terdaftar untuk TKI ini",
  "employeeId": "MLG-FF-0001",
  "nama": "Ani Rahayu"
}
```

**Used by other `200` (hash sudah dipakai TKI lain — ditolak):**
```json
{
  "status": "used_by_other",
  "message": "Fingerprint sudah digunakan oleh TKI lain: Budi Santoso",
  "employeeId": "MLG-FF-0001",
  "nama": "Ani Rahayu",
  "existingEmployeeId": "MLG-FF-0002",
  "existingNama": "Budi Santoso"
}
```

### Server logic

```
1. Verify HMAC signature
2. Decrypt encryptedPayload dengan AES-256
3. Parse JSON → {employeeId, fingerprintHash}
4. Cek apakah fingerprintHash sudah dipakai TKI LAIN:
   SELECT id_biodata, nama FROM personal
   WHERE fingerprint_hash = ? AND id_biodata != ?
   Jika ditemukan → return {status: "used_by_other", existingEmployeeId, existingNama, ...}
5. Cek apakah TKI ini sudah punya fingerprint:
   SELECT fingerprint_hash FROM personal WHERE id_biodata = ?
   Jika sudah ada → return {status: "already_registered", ...}
6. UPDATE personal SET fingerprint_hash = ? WHERE id_biodata = ?
7. Return {status: "ok", message: "registered"}
```

---

## 4. `POST /api/finger/attendance`

Endpoint utama absensi. Menerima hash fingerprint, mencocokkan, dan mencatat kehadiran.

**Aturan 2x per hari:**
- Fingerprint ke-1 → check-in (`jam_masuk`)
- Fingerprint ke-2 → check-out (`jam_pulang`)
- Fingerprint ke-3+ → diabaikan (tidak disimpan)

### Request

```json
{
  "encryptedPayload": "sD8f...base64...==",
  "signature": "xYz1...base64...=="
}
```

**Decrypted payload:**

```json
{
  "fingerprintHash": "a1b2c3d4e5f6..."
}
```

### Responses

**✅ Check-in berhasil `200` (fingerprint ke-1):**
```json
{
  "status": "checkin",
  "type": "masuk",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "nama": "Ani Rahayu",
  "time": "07:45:00",
  "message": "Absen masuk berhasil"
}
```

**✅ Check-out berhasil `200` (fingerprint ke-2):**
```json
{
  "status": "checkout",
  "type": "pulang",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "nama": "Ani Rahayu",
  "time": "16:05:00",
  "message": "Absen pulang berhasil"
}
```

**⚠️ Diabaikan `200` (fingerprint ke-3+):**
```json
{
  "status": "ignored",
  "type": "already_complete",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "message": "Absensi hari ini sudah lengkap (masuk & pulang)"
}
```

**❌ Sidik jari tidak dikenal `200`:**
```json
{
  "status": "not_found",
  "message": "Sidik jari tidak dikenal"
}
```

**Error responses:**

| Code | Body |
|---|---|
| `401` | `{"status": "error", "message": "invalid signature"}` |
| `422` | `{"status": "error", "message": "fingerprintHash required"}` |
| `500` | `{"status": "error", "message": "internal error"}` |

### Server logic

```
1. Verify HMAC signature
2. Decrypt encryptedPayload dengan AES-256
3. Parse JSON → {fingerprintHash}
4. SELECT * FROM personal WHERE fingerprint_hash = ?
5. If not found:
   a. Return {status: "not_found", message: "Sidik jari tidak dikenal"}
6. If found:
   a. Cek record blk_absensi hari ini untuk TKI tersebut
   b. Jika belum ada:
      - Batas terlambat: 08:00
      - Status = "hadir" jika ≤ 08:00, "terlambat" jika > 08:00
      - INSERT ke blk_absensi (jam_masuk, status, ...)
      - Return {status: "checkin", type: "masuk", ...}
   c. Jika sudah ada 1 record dan jam_pulang masih null:
      - UPDATE jam_pulang
      - Return {status: "checkout", type: "pulang", ...}
   d. Jika sudah ada 1 record dan jam_pulang sudah terisi:
      - Jangan simpan apa-apa
      - Return {status: "ignored", type: "already_complete", ...}
```

---

## 5. `GET /api/finger/arduino/sync`

Sync data karyawan dari server ke Electron app untuk local cache.

| Item | Value |
|---|---|
| Method | `GET` |
| Full path | `/api/finger/arduino/sync` |
| Auth | Tidak (atau API key jika diperlukan) |

### Query parameters

| Param | Required | Default | Description |
|---|---|---|---|
| `kode_cabang` | Optional | — | Filter karyawan berdasarkan kode cabang. Jika kosong, tampilkan semua cabang. |

Contoh: `GET /api/finger/arduino/sync?kode_cabang=CKS` → hanya karyawan cabang CKS.

### Response `200 OK`

```json
{
  "success": true,
  "data": [
    {
      "id_biodata": "MLG-FF-0001",
      "nama": "Ani Rahayu",
      "kode_sektor": "FF",
      "kode_cabang": "CKS",
      "has_template": true
    },
    {
      "id_biodata": "MLG-FF-0002",
      "nama": "Budi Santoso",
      "kode_sektor": "FF",
      "kode_cabang": "CKS",
      "has_template": false
    }
  ]
}
```

`has_template`: `true` jika TKI sudah punya template fingerprint di server, `false` jika belum.

### Server logic

```sql
SELECT id_biodata, nama, kode_sektor, kode_cabang,
       (fingerprint_template IS NOT NULL AND fingerprint_template != '') AS has_template
FROM personal
WHERE is_active = 1 AND arsip_status = 'active'
ORDER BY nama
```

---

## 6. `POST /api/finger/arduino/template`

Mendaftarkan template fingerprint untuk seorang TKI ke server.

### Request

| Header | Value |
|---|---|
| `Content-Type` | `application/json` |

**Body:**

```json
{
  "employeeId": "MLG-FF-0001",
  "templateHex": "a1b2c3d4e5f6...1024 hex chars...",
  "device_id": "arduino-001"
}
```

`templateHex`: 1024 hex characters (512 bytes) dari FPM10A sensor.

### Responses

| Status | Meaning |
|---|---|
| `200 OK` | Template registered |
| `200 OK` `already_registered` | Template sudah terdaftar untuk TKI ini |
| `200 OK` `used_by_other` | Template sudah dipakai TKI lain (tidak bisa daftar ganda) |
| `422 Unprocessable` | Missing or invalid fields |

**Success `200` (baru terdaftar):**
```json
{ "status": "ok", "message": "template saved" }
```

**Already registered `200` (sudah terdaftar untuk TKI yang sama):**
```json
{
  "status": "already_registered",
  "message": "Template sudah terdaftar untuk TKI ini",
  "employeeId": "MLG-FF-0001",
  "nama": "Ani Rahayu"
}
```

**Used by other `200` (template sudah dipakai TKI lain — ditolak):**
```json
{
  "status": "used_by_other",
  "message": "Template sudah digunakan oleh TKI lain: Budi Santoso",
  "employeeId": "MLG-FF-0001",
  "nama": "Ani Rahayu",
  "existingEmployeeId": "MLG-FF-0002",
  "existingNama": "Budi Santoso"
}
```

### Server logic

```
1. Parse JSON → {employeeId, templateHex, device_id}
2. Cek apakah templateHex sudah dipakai TKI LAIN:
   SELECT id_biodata, nama FROM personal
   WHERE fingerprint_template = ? AND id_biodata != ?
   Jika ditemukan → return {status: "used_by_other", existingEmployeeId, existingNama, ...}
3. Cek apakah TKI ini sudah punya template:
   SELECT fingerprint_template FROM personal WHERE id_biodata = ?
   Jika sudah ada → return {status: "already_registered", ...}
4. UPDATE personal SET fingerprint_template = ? WHERE id_biodata = ?
5. Return {status: "ok", message: "template saved"}
```

---

## 7. `POST /api/finger/arduino/attendance`

Endpoint utama absensi untuk Arduino + Electron. Menerima template fingerprint, melakukan matching server-side, dan mencatat kehadiran.

**Aturan 2x per hari:**
- Fingerprint ke-1 → check-in (`jam_masuk`)
- Fingerprint ke-2 → check-out (`jam_pulang`)
- Fingerprint ke-3+ → diabaikan (tidak disimpan)

### Request

| Header | Value |
|---|---|
| `Content-Type` | `application/json` |

**Body:**

```json
{
  "templateHex": "a1b2c3d4e5f6...1024 hex chars...",
  "device_id": "arduino-001",
  "kode_cabang": "CKS"
}
```

### Responses

**✅ Check-in berhasil `200` (fingerprint ke-1):**
```json
{
  "status": "checkin",
  "type": "masuk",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "nama": "Ani Rahayu",
  "time": "07:45:00",
  "message": "Absen masuk berhasil"
}
```

**✅ Check-out berhasil `200` (fingerprint ke-2):**
```json
{
  "status": "checkout",
  "type": "pulang",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "nama": "Ani Rahayu",
  "time": "16:05:00",
  "message": "Absen pulang berhasil"
}
```

**⚠️ Diabaikan `200` (fingerprint ke-3+):**
```json
{
  "status": "ignored",
  "type": "already_complete",
  "employeeId": "MLG-FF-0001",
  "name": "Ani Rahayu",
  "message": "Absensi hari ini sudah lengkap (masuk & pulang)"
}
```

**❌ Sidik jari tidak dikenal `200`:**
```json
{
  "status": "not_found",
  "message": "Sidik jari tidak dikenal"
}
```

**Error responses:**

| Code | Body |
|---|---|
| `422` | `{"status": "error", "message": "templateHex required"}` |
| `500` | `{"status": "error", "message": "internal error"}` |

### Server logic

```
1. Parse JSON → {templateHex, device_id, kode_cabang}
2. Cari template di database:
   SELECT * FROM personal WHERE fingerprint_template = ?
3. If not found:
   a. Return {status: "not_found", message: "Sidik jari tidak dikenal"}
4. If found:
   a. Cek record blk_absensi hari ini untuk TKI tersebut
   b. Jika belum ada:
      - Batas terlambat: 08:00
      - Status = "hadir" jika ≤ 08:00, "terlambat" jika > 08:00
      - INSERT ke blk_absensi (jam_masuk, status, nama, device_id, kode_cabang, metode_absen='fingerprint')
      - Return {status: "checkin", type: "masuk", ...}
   c. Jika sudah ada 1 record dan jam_pulang masih null:
      - UPDATE jam_pulang
      - Return {status: "checkout", type: "pulang", ...}
   d. Jika sudah ada 1 record dan jam_pulang sudah terisi:
      - Jangan simpan apa-apa
      - Return {status: "ignored", type: "already_complete", ...}
```

---

## Encryption & decryption flow

### Key derivation

```
secret_key = "AbsensiSecureSecret2026!"       ← UTF-8 string, configurable via env

AES-256 key = SHA-256(secret_key)            → 32 bytes
HMAC key    = secret_key.getBytes("UTF-8")   → raw bytes, NOT hashed
```

### HMAC signature verification

```
signature = Base64( HMAC-SHA256(encryptedPayload, raw_secret_key_bytes) )
```

### AES-256 decryption

Algorithm: `AES/ECB/PKCS5Padding`

```javascript
const crypto = require('crypto');

function decryptPayload(encryptedBase64, secretKey) {
  const aesKey = crypto.createHash('sha256').update(secretKey).digest();
  const decipher = crypto.createDecipheriv('aes-256-ecb', aesKey, null);
  decipher.setAutoPadding(true);
  const encrypted = Buffer.from(encryptedBase64, 'base64');
  const decrypted = Buffer.concat([decipher.update(encrypted), decipher.final()]);
  return JSON.parse(decrypted.toString('utf-8'));
}
```

---

## Server-side template matching

Server melakukan matching template fingerprint yang dikirim dari Arduino dengan template yang tersimpan di database PostgreSQL.

### Template format

Template dari FPM10A sensor adalah 512 byte data, dikirim sebagai 1024 hex characters.

### Matching algorithm

Server menggunakan algoritma byte-level similarity comparison:

```javascript
function compareTemplates(hex1, hex2) {
  if (!hex1 || !hex2 || hex1.length !== hex2.length) return 0;
  
  const b1 = hexToBuffer(hex1);
  const b2 = hexToBuffer(hex2);
  
  let exact = 0, close = 0;
  const len = b1.length;
  
  for (let i = 0; i < len; i++) {
    const diff = Math.abs(b1[i] - b2[i]);
    if (diff === 0) exact++;
    else if (diff <= 2) close++;
  }
  
  const score = (exact + close * 0.5) / len;
  return score; // 0.0 - 1.0
}
```

### Threshold

- **Match threshold**: 0.60 (60% similarity)
- Jika score >= 0.60 → match ditemukan
- Jika score < 0.60 → tidak ada match

### Performance considerations

- Template matching dilakukan di server, bisa berat jika banyak template
- Gunakan index pada kolom `fingerprint_template` untuk query cepat
- Pertimbangkan menggunakan database yang support similarity search (PostgreSQL pg_trgm)

### Alternative: Fingerprint library

Untuk matching yang lebih akurat, bisa menggunakan library fingerprint matching di server:

```javascript
const Fingerprint = require('fingerprintjs'); // Example library

// Load template from database
const storedTemplate = loadTemplateFromDB(employeeId);

// Compare with incoming template
const score = Fingerprint.compare(incomingTemplate, storedTemplate);
```

---

## Database schema

### Table: `personal` (data TKI)

Fingerprint template disimpan di kolom `fingerprint_template` pada tabel `personal`.

```sql
CREATE TABLE personal (
    id                  SERIAL PRIMARY KEY,
    id_biodata          VARCHAR(50) UNIQUE NOT NULL,
    nama                VARCHAR(200) NOT NULL,
    kode_sektor         VARCHAR(10),
    kode_cabang         VARCHAR(10),
    fingerprint_template TEXT,        -- 1024 hex chars (512 bytes), NULL until registered
    is_active           INTEGER DEFAULT 1,
    arsip_status        VARCHAR(20) DEFAULT 'active',
    ...
);

CREATE INDEX idx_personal_fingerprint ON personal(fingerprint_template);
```

### Table: `blk_absensi` (log absensi)

```sql
CREATE TABLE blk_absensi (
    id                SERIAL PRIMARY KEY,
    id_tki            INTEGER NOT NULL REFERENCES personal(id_tki),
    id_biodata        VARCHAR(50),
    nama              VARCHAR(200),
    sektor            VARCHAR(10),
    tanggal           DATE NOT NULL,
    jam_masuk         TIME,
    jam_pulang        TIME,
    status            VARCHAR(20) NOT NULL,    -- hadir, terlambat, izin, alpha, pulang_cepat
    keterangan        TEXT,
    metode_absen      VARCHAR(20),             -- fingerprint, manual, centang
    fingerprint_hash  VARCHAR(128),
    device_id         VARCHAR(100),
    kode_cabang       VARCHAR(10),
    created_at        TIMESTAMP DEFAULT NOW(),
    updated_at        TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_absensi_tgl ON blk_absensi(tanggal);
CREATE INDEX idx_absensi_tki_tgl ON blk_absensi(id_tki, tanggal);
```

---

## Aturan 2x fingerprint per hari

| Urutan | Aksi | Kolom diisi | Status |
|---|---|---|---|
| Fingerprint ke-1 | Check-in | `jam_masuk` | `hadir` (≤ 08:00) / `terlambat` (> 08:00) |
| Fingerprint ke-2 | Check-out | `jam_pulang` | Status tetap dari check-in |
| Fingerprint ke-3+ | Diabaikan | Tidak ada perubahan | Response `ignored` |

Detail implementasi di `server.js` — `POST /api/finger/attendance`:

```javascript
// Cek absensi hari ini untuk TKI ini
const existingRows = await database.queryAll(
  "SELECT id, jam_masuk, jam_pulang FROM blk_absensi WHERE id_tki = ? AND tanggal = ? ORDER BY id ASC LIMIT 2",
  tki.id_tki, today
);

if (!existingRows || existingRows.length === 0) {
  // Check-in: INSERT dengan jam_masuk
} else if (existingRows.length === 1 && !existingRows[0].jam_pulang) {
  // Check-out: UPDATE jam_pulang
} else {
  // Sudah lengkap: abaikan
}
```

---

## Client defaults

| Setting | Default value |
|---|---|
| API Base URL | `http://192.168.1.15:3004/api/finger` |
| Branches endpoint | `/api/finger/branches` |
| Employees endpoint | `/api/finger/employees?kode_cabang={kode}` |
| Register endpoint | `/api/finger/register` |
| Attendance endpoint | `/api/finger/attendance` |
| Secret key | `AbsensiSecureSecret2026!` |
| Hash algorithm | `SHA-256("FINGERPRINT_TEMPLATE_V1|{employeeId}")` |

---

## Security notes

---

## Server Implementation Guide

### Prerequisites

- Node.js (v14+)
- PostgreSQL (v12+)
- npm packages:
  - `pg` - PostgreSQL client
  - `http` atau `express` - HTTP server

### Database Setup

#### 1. Create Database

```sql
CREATE DATABASE absensi_db;
```

#### 2. Create Tables

```sql
-- Table personal (data TKI)
CREATE TABLE personal (
    id                  SERIAL PRIMARY KEY,
    id_biodata          VARCHAR(50) UNIQUE NOT NULL,
    nama                VARCHAR(200) NOT NULL,
    kode_sektor         VARCHAR(10),
    kode_cabang         VARCHAR(10),
    fingerprint_template TEXT,        -- 1024 hex chars (512 bytes), NULL until registered
    is_active           INTEGER DEFAULT 1,
    arsip_status        VARCHAR(20) DEFAULT 'active',
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_personal_fingerprint ON personal(fingerprint_template);
CREATE INDEX idx_personal_id_biodata ON personal(id_biodata);
CREATE INDEX idx_personal_cabang ON personal(kode_cabang);

-- Table blk_absensi (log absensi)
CREATE TABLE blk_absensi (
    id                SERIAL PRIMARY KEY,
    id_tki            INTEGER NOT NULL REFERENCES personal(id),
    id_biodata        VARCHAR(50),
    nama              VARCHAR(200),
    tanggal           DATE DEFAULT CURRENT_DATE,
    jam_masuk         TIME,
    jam_pulang        TIME,
    status            VARCHAR(20), -- 'hadir', 'terlambat', 'izin', 'sakit'
    device_id         VARCHAR(50),
    kode_cabang       VARCHAR(10),
    metode_absen      VARCHAR(20) DEFAULT 'fingerprint',
    created_at        TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_blk_absensi_tanggal ON blk_absensi(tanggal);
CREATE INDEX idx_blk_absensi_id_biodata ON blk_absensi(id_biodata);
```

#### 3. Migration from Old Schema

Jika sudah ada table `personal` dengan `fingerprint_hash`:

```sql
-- Drop old index
DROP INDEX IF EXISTS idx_personal_fingerprint_hash;

-- Add new column
ALTER TABLE personal ADD COLUMN fingerprint_template TEXT;

-- Migrate data (optional, jika ingin convert hash ke template)
-- UPDATE personal SET fingerprint_template = fingerprint_hash WHERE fingerprint_hash IS NOT NULL;

-- Drop old column (setelah migrasi selesai)
-- ALTER TABLE personal DROP COLUMN fingerprint_hash;

-- Create new index
CREATE INDEX idx_personal_fingerprint ON personal(fingerprint_template);
```

### Node.js Server Implementation

#### Basic Server Structure

```javascript
const http = require('http');
const { Pool } = require('pg');

// PostgreSQL connection
const pool = new Pool({
  host: 'localhost',
  port: 5432,
  database: 'absensi_db',
  user: 'postgres',
  password: 'your_password',
});

const PORT = 3004;

// Helper: parse JSON body
function parseBody(req) {
  return new Promise((resolve) => {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => resolve(JSON.parse(body || '{}')));
  });
}

// Helper: send JSON response
function sendJson(res, data, statusCode = 200) {
  res.writeHead(statusCode, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(data));
}

// 1. GET /api/finger/arduino/sync
async function handleSyncEmployees(req, res) {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const kodeCabang = url.searchParams.get('kode_cabang');

  let query = `
    SELECT id_biodata, nama, kode_sektor, kode_cabang,
           (fingerprint_template IS NOT NULL AND fingerprint_template != '') AS has_template
    FROM personal
    WHERE is_active = 1 AND arsip_status = 'active'
  `;
  const params = [];

  if (kodeCabang) {
    query += ' AND kode_cabang = $1';
    params.push(kodeCabang);
  }

  query += ' ORDER BY nama';

  try {
    const result = await pool.query(query, params);
    sendJson(res, { success: true, data: result.rows });
  } catch (err) {
    console.error('Sync error:', err);
    sendJson(res, { success: false, message: err.message }, 500);
  }
}

// 2. POST /api/finger/arduino/template
async function handleRegisterTemplate(req, res) {
  const body = await parseBody(req);
  const { employeeId, templateHex, device_id } = body;

  if (!employeeId || !templateHex) {
    return sendJson(res, { status: 'error', message: 'employeeId and templateHex required' }, 422);
  }

  try {
    // Check if template is used by other employee
    const checkOther = await pool.query(
      'SELECT id_biodata, nama FROM personal WHERE fingerprint_template = $1 AND id_biodata != $2',
      [templateHex, employeeId]
    );

    if (checkOther.rows.length > 0) {
      return sendJson(res, {
        status: 'used_by_other',
        message: `Template sudah digunakan oleh TKI lain: ${checkOther.rows[0].nama}`,
        employeeId,
        existingEmployeeId: checkOther.rows[0].id_biodata,
        existingNama: checkOther.rows[0].nama
      });
    }

    // Check if employee already has template
    const checkExisting = await pool.query(
      'SELECT fingerprint_template FROM personal WHERE id_biodata = $1',
      [employeeId]
    );

    if (checkExisting.rows.length > 0 && checkExisting.rows[0].fingerprint_template) {
      return sendJson(res, {
        status: 'already_registered',
        message: 'Template sudah terdaftar untuk TKI ini',
        employeeId,
        nama: checkExisting.rows[0].nama
      });
    }

    // Update template
    await pool.query(
      'UPDATE personal SET fingerprint_template = $1, updated_at = CURRENT_TIMESTAMP WHERE id_biodata = $2',
      [templateHex, employeeId]
    );

    sendJson(res, { status: 'ok', message: 'template saved' });
  } catch (err) {
    console.error('Template registration error:', err);
    sendJson(res, { status: 'error', message: err.message }, 500);
  }
}

// 3. POST /api/finger/arduino/attendance
async function handleAttendance(req, res) {
  const body = await parseBody(req);
  const { templateHex, device_id, kode_cabang } = body;

  if (!templateHex) {
    return sendJson(res, { status: 'error', message: 'templateHex required' }, 422);
  }

  try {
    // Find employee by template
    const employee = await pool.query(
      'SELECT * FROM personal WHERE fingerprint_template = $1',
      [templateHex]
    );

    if (employee.rows.length === 0) {
      return sendJson(res, { status: 'not_found', message: 'Sidik jari tidak dikenal' });
    }

    const emp = employee.rows[0];
    const today = new Date().toISOString().split('T')[0];

    // Check today's attendance
    const todayAttendance = await pool.query(
      `SELECT * FROM blk_absensi 
       WHERE id_biodata = $1 AND tanggal = $2 
       ORDER BY created_at DESC LIMIT 1`,
      [emp.id_biodata, today]
    );

    if (todayAttendance.rows.length === 0) {
      // Check-in
      const now = new Date();
      const timeStr = now.toTimeString().split(' ')[0];
      const isLate = timeStr > '08:00:00';
      const status = isLate ? 'terlambat' : 'hadir';

      await pool.query(
        `INSERT INTO blk_absensi (id_tki, id_biodata, nama, tanggal, jam_masuk, status, device_id, kode_cabang, metode_absen)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 'fingerprint')`,
        [emp.id, emp.id_biodata, emp.nama, today, timeStr, status, device_id, kode_cabang]
      );

      return sendJson(res, {
        status: 'checkin',
        type: 'masuk',
        employeeId: emp.id_biodata,
        name: emp.nama,
        nama: emp.nama,
        time: timeStr,
        message: 'Absen masuk berhasil'
      });
    } else if (!todayAttendance.rows[0].jam_pulang) {
      // Check-out
      const now = new Date();
      const timeStr = now.toTimeString().split(' ')[0];

      await pool.query(
        'UPDATE blk_absensi SET jam_pulang = $1 WHERE id = $2',
        [timeStr, todayAttendance.rows[0].id]
      );

      return sendJson(res, {
        status: 'checkout',
        type: 'pulang',
        employeeId: emp.id_biodata,
        name: emp.nama,
        nama: emp.nama,
        time: timeStr,
        message: 'Absen pulang berhasil'
      });
    } else {
      // Already complete
      return sendJson(res, {
        status: 'ignored',
        type: 'already_complete',
        employeeId: emp.id_biodata,
        name: emp.nama,
        message: 'Absensi hari ini sudah lengkap (masuk & pulang)'
      });
    }
  } catch (err) {
    console.error('Attendance error:', err);
    sendJson(res, { status: 'error', message: err.message }, 500);
  }
}

// HTTP Server
const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const path = url.pathname;

  // CORS headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(200);
    res.end();
    return;
  }

  if (path === '/api/finger/arduino/sync' && req.method === 'GET') {
    await handleSyncEmployees(req, res);
  } else if (path === '/api/finger/arduino/template' && req.method === 'POST') {
    await handleRegisterTemplate(req, res);
  } else if (path === '/api/finger/arduino/attendance' && req.method === 'POST') {
    await handleAttendance(req, res);
  } else {
    sendJson(res, { message: 'Not Found' }, 404);
  }
});

server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
});
```

### Package.json

```json
{
  "name": "absensi-api-server",
  "version": "1.0.0",
  "main": "server.js",
  "dependencies": {
    "pg": "^8.11.0"
  }
}
```

### Running the Server

```bash
# Install dependencies
npm install

# Set environment variables (optional)
export PGHOST=localhost
export PGPORT=5432
export PGDATABASE=absensi_db
export PGUSER=postgres
export PGPASSWORD=your_password

# Start server
node server.js
```

### Testing the Endpoints

#### 1. Test Sync Employees

```bash
curl "http://localhost:3004/api/finger/arduino/sync?kode_cabang=CKS"
```

#### 2. Test Register Template

```bash
curl -X POST http://localhost:3004/api/finger/arduino/template \
  -H "Content-Type: application/json" \
  -d '{
    "employeeId": "MLG-FF-0001",
    "templateHex": "a1b2c3d4e5f6...",
    "device_id": "arduino-001"
  }'
```

#### 3. Test Attendance

```bash
curl -X POST http://localhost:3004/api/finger/arduino/attendance \
  -H "Content-Type: application/json" \
  -d '{
    "templateHex": "a1b2c3d4e5f6...",
    "device_id": "arduino-001",
    "kode_cabang": "CKS"
  }'
```

### Production Considerations

1. **Security**: Add authentication/authorization to API endpoints
2. **HTTPS**: Use HTTPS for production deployment
3. **Rate Limiting**: Add rate limiting to prevent abuse
4. **Logging**: Implement proper logging for debugging
5. **Error Handling**: Add comprehensive error handling
6. **Database Connection Pooling**: Configure connection pool size
7. **Environment Variables**: Use `.env` file for configuration
8. **Validation**: Add input validation for all endpoints
9. **Backup**: Regular database backups
10. **Monitoring**: Add health check endpoint

### Troubleshooting

#### Template Not Matching

- Verify template hex format (1024 hex characters)
- Check database encoding (UTF-8)
- Ensure template is stored correctly in database

#### Connection Issues

- Check PostgreSQL is running
- Verify connection parameters
- Check firewall settings

#### Performance Issues

- Add index on `fingerprint_template` column
- Consider using connection pooling
- Monitor database query performance

### Hash algorithm

```
fingerprintHash = SHA-256("FINGERPRINT_TEMPLATE_V1|{employeeId}")
```

- Deterministic — same `employeeId` always produces the same hash.
- The hash is NOT a real biometric template; it is a simulated identifier.

### Error responses summary

| HTTP Status | Meaning | Client handling |
|---|---|---|
| `200` `checkin` | Check-in berhasil | Tampilkan "Absen masuk berhasil" |
| `200` `checkout` | Check-out berhasil | Tampilkan "Absen pulang berhasil" |
| `200` `ignored` | Sudah absen lengkap | Tampilkan "Sudah absen masuk & pulang" |
| `200` `not_found` | Sidik jari tidak dikenal | Tampilkan "Sidik jari tidak dikenal" |
| `401` | HMAC signature mismatch | Log error, contact admin |
| `422` | Missing field in payload | Log error, contact admin |
| `500` | Server error | Tampilkan "Server error, coba lagi" |

### Security recommendations

1. **Rotate the secret key** periodically.
2. **Use HTTPS only**.
3. **Timestamp validation** — reject requests older than ±5 minutes to prevent replay attacks.
4. **Rate limiting** — apply per-IP or per-fingerprint rate limiting.
5. **Never log the secret key** or decrypted `fingerprintHash` in plain text.

---

## Implementation status (catatan untuk developer app)

### Server

| Item | Status | Keterangan |
|---|---|---|
| `GET /api/finger/branches` | ✅ Implemented | `server.js` — daftar cabang aktif |
| `GET /api/finger/employees` | ✅ Implemented | `server.js` — ambil TKI aktif, support `?kode_cabang=` |
| `POST /api/finger/register` | ✅ Implemented | `server.js` — simpan hash, return `already_registered` jika sudah ada |
| `POST /api/finger/attendance` | ✅ Implemented | `server.js` — check-in/out ke `blk_absensi` |
| AES-256-ECB encryption | ✅ Implemented | `server.js` — decrypt payload |
| HMAC-SHA256 signature | ✅ Implemented | `server.js` — verify signature |
| Aturan 2x scan/hari | ✅ Implemented | Check-in → check-out → abaikan ke-3+ |
| Batas terlambat 08:00 | ✅ Implemented | `<=08:00` = hadir, `>08:00` = terlambat |
| Auto-detect matching | ✅ Implemented | App iterate semua TKI terdaftar, server cocokkan satu per satu |

### App Android

| Item | Status | Keterangan |
|---|---|---|
| Pilih cabang (dropdown) | ✅ | `GET /branches` → pilih → `GET /employees?kode_cabang=` |
| Registrasi fingerprint | ✅ | Pilih TKI + biometric → `POST /register` |
| Auto-detect scan | ✅ | User letakkan jari → app coba semua hash TKI → server match |
| List refresh setelah register | ✅ | Auto fetch ulang employees setelah register sukses |
| Dua tab filter (Terdaftar/Belum) | ✅ | Filter berdasarkan `finger_terdaftar` dari server |
| Indikator ✓ TERDAFTAR | ✅ | Badge hijau pada TKI yang sudah register |

### Database

| Item | Status | File |
|---|---|---|
| `personal.fingerprint_hash` (TEXT) | ✅ | `schema/personal.json:201` |
| `blk_absensi.fingerprint_hash` (TEXT) | ✅ | `schema/blk_absensi.json` |
| `blk_absensi.device_id` (TEXT) | ✅ | `schema/blk_absensi.json` |
| `blk_absensi.metode_absen` (TEXT) | ✅ | `schema/blk_absensi.json` — nilai: `fingerprint` |
| `blk_absensi.id_tki` → TEXT | ✅ Fixed | `schema/blk_absensi.json:17` + migrasi otomatis (`database.js`) |
| Index `idx_personal_fingerprint` | ✅ | `database.js:ensureIndexes()` |

### Konfigurasi app biometrik

| Setting | Value |
|---|---|
| API Base URL | `http://192.168.1.15:3004/api/finger` |
| Secret key | `AbsensiSecureSecret2026!` |
| Port server | `3004` |

---

## Catatan integrasi Arduino bridge

Arduino bridge (FPM10A + Node.js bridge) tidak mengirim `templateHex` karena sensor
FPM10A melakukan matching on-sensor (fingerSearch) dan hanya mengembalikan ID hasil
pencocokan. Bridge mengirim format berikut ke endpoint ini:

### `POST /api/finger/arduino/attendance` — format alternatif

**Body:**
```json
{
  "employeeId": "CKS-MI-0001",
  "device_id": "arduino-001",
  "kode_cabang": "CKS"
}
```

Server API HARUS mendukung matching berdasarkan `employeeId` (`id_biodata`), bukan
`fingerprint_template`. Response tetap mengikuti format yang sama seperti dokumentasi
di atas (checkin/checkout/ignored/not_found/error).
