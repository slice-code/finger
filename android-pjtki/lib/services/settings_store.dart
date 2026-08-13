import 'package:shared_preferences/shared_preferences.dart';
import '../config/app_config.dart';

class SettingsStore {
  static const _kApiUrl = 'api_base_url';
  static const _kKodeCabang = 'kode_cabang';
  static const _kDeviceId = 'device_id';
  static const _kApiKey = 'device_key';
  static const _kLastBleDeviceId = 'last_ble_device_id';
  static const _kLastBleDeviceName = 'last_ble_device_name';

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

  bool get isConfigured => apiBaseUrl.isNotEmpty && apiKey.isNotEmpty;
}
