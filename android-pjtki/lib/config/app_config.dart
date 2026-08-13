const String kServiceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
const String kStatusCharUuid = "4fafc202-1fb5-459e-8fcc-c5c9c331914b";
const String kCmdCharUuid = "4fafc203-1fb5-459e-8fcc-c5c9c331914b";
const String kEnrollCharUuid = "4fafc204-1fb5-459e-8fcc-c5c9c331914b";
const String kDeleteCharUuid = "4fafc205-1fb5-459e-8fcc-c5c9c331914b";
const String kSettingsCharUuid = "4fafc206-1fb5-459e-8fcc-c5c9c331914b";
const String kEventsCharUuid = "4fafc207-1fb5-459e-8fcc-c5c9c331914b";
const String kHistoryCharUuid = "4fafc208-1fb5-459e-8fcc-c5c9c331914b";

const String kDeviceNamePrefix = "PJTKI-Finger";
// Harus http (bukan https) — ESP32 TLS ke Cloudflare sering gagal.
const String kDefaultApiBaseUrl = "http://cks.slice-code.com";
const String kScanTimeoutLabel = "Aktifkan Bluetooth & pastikan device ESP32 menyala";

// Secret key untuk enkripsi HMAC+AES riwayat absensi (POST /api/finger/history).
// HARUS sama dengan FINGERPRINT_SECRET_KEY di server pjtki-bio (api/fingerprint.js).
const String kFingerprintSecretKey = "AbsensiSecureSecret2026!";
