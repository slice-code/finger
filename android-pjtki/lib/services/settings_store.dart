import 'package:shared_preferences/shared_preferences.dart';
import '../config/app_config.dart';

class SettingsStore {
  static const _kApiUrl = 'api_base_url';
  static const _kKodeCabang = 'kode_cabang';
  static const _kDeviceId = 'device_id';
  static const _kApiKey = 'device_key';
  static const _kLastBleDeviceId = 'last_ble_device_id';
  static const _kLastBleDeviceName = 'last_ble_device_name';
  static const _kEnrollCache = 'enroll_cache'; // JSON string daftar finger dari ESP32
  static const _kAuthToken = 'auth_token'; // JWT login BLK (Bearer)
  static const _kAuthEmail = 'auth_email';
  static const _kAuthName = 'auth_name';
  static const _kUserKodeCabang = 'user_kode_cabang'; // cabang dari login BLK (JWT)

  SharedPreferences? _prefs;

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
  }

  SharedPreferences get prefs {
    if (_prefs == null) throw StateError('SettingsStore belum di-init');
    return _prefs!;
  }

  String get apiBaseUrl => prefs.getString(_kApiUrl) ?? kDefaultApiBaseUrl;
  set apiBaseUrl(String v) => prefs.setString(_kApiUrl, v);

  String get kodeCabang => prefs.getString(_kKodeCabang) ?? '';
  set kodeCabang(String v) => prefs.setString(_kKodeCabang, v);

  String get deviceId => prefs.getString(_kDeviceId) ?? '';
  set deviceId(String v) => prefs.setString(_kDeviceId, v);

  String get apiKey => prefs.getString(_kApiKey) ?? '';
  set apiKey(String v) => prefs.setString(_kApiKey, v);

  String get lastBleDeviceId => prefs.getString(_kLastBleDeviceId) ?? '';
  set lastBleDeviceId(String v) => prefs.setString(_kLastBleDeviceId, v);

  String get lastBleDeviceName => prefs.getString(_kLastBleDeviceName) ?? 'PJTKI-Finger';
  set lastBleDeviceName(String v) => prefs.setString(_kLastBleDeviceName, v);

  /// Cache daftar fingerprint dari ESP32 (persist di storage app).
  String get enrollCache => prefs.getString(_kEnrollCache) ?? '';
  set enrollCache(String v) => prefs.setString(_kEnrollCache, v);

  /// Token JWT dari login user BLK (untuk endpoint device/key).
  String get authToken => prefs.getString(_kAuthToken) ?? '';
  set authToken(String v) => prefs.setString(_kAuthToken, v);

  String get authEmail => prefs.getString(_kAuthEmail) ?? '';
  set authEmail(String v) => prefs.setString(_kAuthEmail, v);

  String get authName => prefs.getString(_kAuthName) ?? '';
  set authName(String v) => prefs.setString(_kAuthName, v);

  /// Cabang user BLK dari login (bukan dari device fingerprint).
  String get userKodeCabang => prefs.getString(_kUserKodeCabang) ?? '';
  set userKodeCabang(String v) => prefs.setString(_kUserKodeCabang, v);

  bool get isLoggedIn => authToken.isNotEmpty;

  bool get isConfigured => apiBaseUrl.isNotEmpty && apiKey.isNotEmpty;

  void clearAuth() {
    authToken = '';
    authEmail = '';
    authName = '';
    userKodeCabang = '';
  }
}
