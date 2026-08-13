import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../config/app_config.dart';
import '../models/models.dart';
import 'settings_store.dart';

class BleService extends ChangeNotifier {
  SettingsStore? _settings;

  final _statusController = StreamController<DeviceStatus>.broadcast();
  final _eventController = StreamController<BleEvent>.broadcast();
  final _connectionController = StreamController<bool>.broadcast();

  Stream<DeviceStatus> get statusStream => _statusController.stream;
  Stream<BleEvent> get eventStream => _eventController.stream;
  Stream<bool> get connectionStream => _connectionController.stream;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _statusChar;
  BluetoothCharacteristic? _cmdChar;
  BluetoothCharacteristic? _enrollChar;
  BluetoothCharacteristic? _deleteChar;
  BluetoothCharacteristic? _settingsChar;
  BluetoothCharacteristic? _eventsChar;
  BluetoothCharacteristic? _historyChar;
  StreamSubscription? _connSub;
  StreamSubscription? _statusSub;
  StreamSubscription? _eventsSub;

  bool _connected = false;
  bool _linkEstablished = false;
  bool _connecting = false;
  String _connectedName = '';
  final Map<String, BluetoothDevice> _seenDevices = {};

  // Auto-reconnect: dipakai saat koneksi putus (bukan manual disconnect).
  bool _autoReconnectEnabled = false;
  bool _manualDisconnect = false;
  bool _autoReconnecting = false;
  // true saat connect() dipanggil dari timer auto-reconnect (bukan user).
  // Mencegah connect() me-reset _reconnectAttempts → backoff naik dgn benar.
  bool _autoReconnectCall = false;
  Timer? _reconnectTimer;
  int _reconnectAttempts = 0;
  static const int _reconnectMaxAttempts = 5;
  static const List<Duration> _reconnectBackoff = [
    Duration(seconds: 2),
    Duration(seconds: 5),
    Duration(seconds: 10),
    Duration(seconds: 15),
    Duration(seconds: 20),
  ];

  bool get isAutoReconnecting => _autoReconnecting;

  void bindSettings(SettingsStore settings) {
    _settings = settings;
  }

  BluetoothDevice? get connectedDevice => _device;
  bool get isConnected => _connected;
  bool get isConnecting => _connecting;
  String get connectedName => _connectedName.isNotEmpty
      ? _connectedName
      : (_device?.platformName.trim().isNotEmpty == true
          ? _device!.platformName.trim()
          : kDeviceNamePrefix);

  static bool _uuidEq(String a, String b) {
    final na = a.toLowerCase().replaceAll('-', '');
    final nb = b.toLowerCase().replaceAll('-', '');
    return na == nb;
  }

  static Future<void> requestPermissions() async {
    for (final p in [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ]) {
      if (await p.isDenied || await p.isRestricted) {
        await p.request();
      }
    }
    if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
      try {
        await FlutterBluePlus.turnOn();
      } catch (_) {}
    }
  }

  Future<List<BleDeviceInfo>> scan({int timeoutSec = 8}) async {
    await requestPermissions();
    final seen = <String, BleDeviceInfo>{};
    final sub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final adv = r.advertisementData.advName.trim();
        final platform = r.device.platformName.trim();
        final name = adv.isNotEmpty
            ? adv
            : (platform.isNotEmpty ? platform : '');
        final hasService = r.advertisementData.serviceUuids
            .any((u) => _uuidEq(u.str, kServiceUuid));
        final nameOk =
            name.toUpperCase().contains(kDeviceNamePrefix.toUpperCase());
        if (!nameOk && !hasService) continue;
        final display = name.isNotEmpty ? name : kDeviceNamePrefix;
        _seenDevices[r.device.remoteId.str] = r.device;
        seen[r.device.remoteId.str] = BleDeviceInfo(
          id: r.device.remoteId.str,
          name: display,
          rssi: r.rssi,
        );
      }
    });
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    await FlutterBluePlus.startScan(
      timeout: Duration(seconds: timeoutSec),
      androidUsesFineLocation: true,
    );
    await Future<void>.delayed(Duration(seconds: timeoutSec + 1));
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    await sub.cancel();

    final savedDeviceId = _settings?.lastBleDeviceId ?? '';
    if (savedDeviceId.isNotEmpty && !seen.containsKey(savedDeviceId)) {
      seen[savedDeviceId] = BleDeviceInfo(
        id: savedDeviceId,
        name: _settings?.lastBleDeviceName ?? kDeviceNamePrefix,
        rssi: 0,
      );
    }

    return seen.values.toList();
  }

  void _resetCharacteristics() {
    _statusChar = null;
    _cmdChar = null;
    _enrollChar = null;
    _deleteChar = null;
    _settingsChar = null;
    _eventsChar = null;
    _historyChar = null;
  }

  void _markDisconnected() {
    if (!_connected && !_linkEstablished) return;
    _connected = false;
    _linkEstablished = false;
    _resetCharacteristics();
    _connectionController.add(false);
    notifyListeners();
    _scheduleAutoReconnect();
  }

  /// Auto-reconnect: koneksi putus tiba-tiba (ESP32 reset/interferensi/app
  /// kembali ke foreground). Berhenti jika user manual disconnect atau sudah
  /// berhasil konek lagi. Skip jika masih dalam proses connect manual.
  void _scheduleAutoReconnect() {
    if (!_autoReconnectEnabled || _manualDisconnect) return;
    if (_connecting || _connected) return;
    if (_reconnectAttempts >= _reconnectMaxAttempts) {
      _autoReconnecting = false;
      notifyListeners();
      return;
    }
    final delay = _reconnectBackoff[_reconnectAttempts];
    _reconnectAttempts++;
    _autoReconnecting = true;
    notifyListeners();
    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(delay, () async {
      final savedId = _settings?.lastBleDeviceId ?? '';
      if (savedId.isEmpty) {
        _autoReconnecting = false;
        notifyListeners();
        return;
      }
      _autoReconnecting = false;
      _autoReconnectCall = true;
      notifyListeners();
      try {
        await connect(BleDeviceInfo(
          id: savedId,
          name: _settings?.lastBleDeviceName ?? kDeviceNamePrefix,
        ));
      } catch (_) {
        // Gagal → jadwalkan percobaan berikutnya (backoff naik).
        if (!_manualDisconnect && _autoReconnectEnabled && !_connected) {
          _scheduleAutoReconnect();
        }
      } finally {
        _autoReconnectCall = false;
      }
    });
  }

  void _stopAutoReconnect() {
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    _reconnectAttempts = 0;
    _autoReconnecting = false;
  }

  Future<void> restoreLastDevice() async {
    final savedId = _settings?.lastBleDeviceId ?? '';
    if (savedId.isEmpty) return;
    // Auto-reconnect aktif: app dibuka lagi → coba konek ke device terakhir.
    _autoReconnectEnabled = true;
    _manualDisconnect = false;
    try {
      await connect(BleDeviceInfo(
        id: savedId,
        name: _settings?.lastBleDeviceName ?? kDeviceNamePrefix,
      ));
    } catch (_) {
      // Gagal di cold start (BLE belum siap / stack stale) → retry berkala.
      _scheduleAutoReconnect();
    }
  }

  Future<void> connect(BleDeviceInfo info) async {
    if (_connecting) return;
    _connecting = true;
    if (_autoReconnectCall) {
      // Connect dari timer auto-reconnect: JANGAN reset backoff counter.
      _reconnectTimer?.cancel();
      _reconnectTimer = null;
    } else {
      // Connect eksplisit (dari scan) → hentikan auto-reconnect & mulai baru.
      _stopAutoReconnect();
      _manualDisconnect = false;
      _autoReconnectEnabled = true;
    }
    notifyListeners();

    _settings?.lastBleDeviceId = info.id;
    _settings?.lastBleDeviceName = info.name;

    await requestPermissions();
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    // Android: radio scan harus tenang dulu, kalau tidak klik-1 sering null/gagal.
    await Future<void>.delayed(const Duration(milliseconds: 500));

    await _connSub?.cancel();
    _connSub = null;
    _statusSub?.cancel();
    _eventsSub?.cancel();

    if (_device != null) {
      try {
        await _device!.disconnect();
      } catch (_) {}
      await Future<void>.delayed(const Duration(milliseconds: 300));
    }

    _resetCharacteristics();
    _connected = false;
    _linkEstablished = false;
    _connectedName = info.name;
    notifyListeners();

    _device = _seenDevices[info.id] ?? BluetoothDevice.fromId(info.id);

    // Stream connectionState emit state saat ini saat subscribe.
    // Jangan anggap "disconnected" awal sebagai putus koneksi aktif.
    _connSub = _device!.connectionState.listen((state) {
      if (state == BluetoothConnectionState.connected) {
        _linkEstablished = true;
      } else if (state == BluetoothConnectionState.disconnected) {
        if (_linkEstablished || _connected) {
          _markDisconnected();
        }
      }
    });

    try {
      if (_device!.isConnected) {
        try {
          await _device!.disconnect();
          await Future<void>.delayed(const Duration(milliseconds: 400));
        } catch (_) {}
      }

      try {
        await _device!.clearGattCache();
      } catch (_) {}

      await _connectWithRetry();

      // Tunggu benar-benar connected (bukan cuma Future connect selesai).
      if (!_device!.isConnected) {
        await _device!.connectionState
            .where((s) => s == BluetoothConnectionState.connected)
            .first
            .timeout(const Duration(seconds: 20));
      }
      _linkEstablished = true;

      // Sedikit jeda sebelum discover — Xiaomi sering gagal kalau terlalu cepat.
      await Future<void>.delayed(const Duration(milliseconds: 400));
      await _discoverAndSubscribe();

      if (_statusChar == null || _cmdChar == null) {
        // Retry discover sekali (klik-1 klasik: services belum siap).
        await Future<void>.delayed(const Duration(milliseconds: 600));
        await _discoverAndSubscribe();
      }

      if (_statusChar == null || _cmdChar == null) {
        throw Exception('Karakteristik BLE PJTKI tidak ditemukan');
      }

      _connected = true;
      _reconnectAttempts = 0;
      _autoReconnecting = false;
      _connectionController.add(true);
      notifyListeners();
    } catch (e) {
      _connected = false;
      _linkEstablished = false;
      notifyListeners();
      try {
        await _device?.disconnect();
      } catch (_) {}
      rethrow;
    } finally {
      _connecting = false;
      notifyListeners();
    }
  }

  /// connect + retry untuk kasus "scan tampil tapi tidak bisa konek".
  /// Android BLE stack sering menyimpan connection stale setelah app di-kill:
  /// attempt pertama gagal → disconnect + clear cache → coba lagi sekali.
  Future<void> _connectWithRetry() async {
    final device = _device;
    if (device == null) throw Exception('Device tidak ada');

    const maxAttempts = 2;
    for (var attempt = 0; attempt < maxAttempts; attempt++) {
      try {
        await device.connect(
          license: License.nonprofit,
          autoConnect: false,
          timeout: const Duration(seconds: 20),
        );
        return;
      } catch (e) {
        if (attempt == maxAttempts - 1) rethrow;
        // Stale GATT: tunggu stack release, disconnect + clear cache, coba lagi.
        try {
          await device.disconnect();
        } catch (_) {}
        await Future<void>.delayed(const Duration(milliseconds: 1200));
        try {
          await device.clearGattCache();
        } catch (_) {}
      }
    }
  }

  Future<void> _discoverAndSubscribe() async {
    final device = _device;
    if (device == null) return;

    final services = await device.discoverServices();
    for (final s in services) {
      if (!_uuidEq(s.uuid.str, kServiceUuid)) continue;
      for (final c in s.characteristics) {
        final u = c.uuid.str;
        if (_uuidEq(u, kStatusCharUuid)) _statusChar = c;
        if (_uuidEq(u, kCmdCharUuid)) _cmdChar = c;
        if (_uuidEq(u, kEnrollCharUuid)) _enrollChar = c;
        if (_uuidEq(u, kDeleteCharUuid)) _deleteChar = c;
        if (_uuidEq(u, kSettingsCharUuid)) _settingsChar = c;
        if (_uuidEq(u, kEventsCharUuid)) _eventsChar = c;
        if (_uuidEq(u, kHistoryCharUuid)) _historyChar = c;
      }
    }

    _statusSub?.cancel();
    _eventsSub?.cancel();

    _statusSub = _statusChar?.lastValueStream.listen((bytes) {
      if (bytes.isEmpty) return;
      try {
        final json = jsonDecode(utf8.decode(bytes));
        if (json is Map) {
          _status = DeviceStatus.fromJson(Map<String, dynamic>.from(json));
          _statusController.add(_status);
          notifyListeners();
        }
      } catch (_) {}
    });
    if (_statusChar != null) {
      await _statusChar!.setNotifyValue(true);
      _status = await readStatus();
      notifyListeners();
    }

    _eventsSub = _eventsChar?.lastValueStream.listen((bytes) {
      if (bytes.isEmpty) return;
      final raw = utf8.decode(bytes, allowMalformed: true);
      final ev = BleEvent.parse(raw);
      if (ev.type.isNotEmpty) _eventController.add(ev);
    });
    if (_eventsChar != null) {
      await _eventsChar!.setNotifyValue(true);
    }
  }

  DeviceStatus _status = DeviceStatus();
  DeviceStatus get lastStatus => _status;

  Future<DeviceStatus> readStatus() async {
    final c = _statusChar;
    if (c == null) return _status;
    try {
      final bytes = await c.read();
      if (bytes.isNotEmpty) {
        final json = jsonDecode(utf8.decode(bytes));
        if (json is Map) {
          _status = DeviceStatus.fromJson(Map<String, dynamic>.from(json));
          notifyListeners();
        }
      }
    } catch (_) {}
    return _status;
  }

  Future<void> _writeBytes(BluetoothCharacteristic c, List<int> bytes) async {
    try {
      await c.write(bytes, withoutResponse: false);
    } catch (_) {
      await c.write(bytes, withoutResponse: true);
    }
  }

  Future<void> writeCommand(String cmd) async {
    final c = _cmdChar;
    if (c == null) throw Exception('Karakteristik command tidak ditemukan');
    await _writeBytes(c, utf8.encode(cmd));
  }

  Future<void> requestWifiScan() async {
    await writeCommand('WIFI_SCAN');
  }

  Future<void> syncNow() async {
    await writeCommand('SYNC_NOW');
  }

  Future<void> setUploadInterval(int minutes) async {
    if (minutes <= 0) return;
    await writeCommand('SET_SYNC_INTERVAL $minutes');
  }

  Future<void> cancelEnroll() async {
    await writeCommand('ENROLL_CANCEL');
  }

  Future<void> enroll(String employeeId, String name) async {
    final c = _enrollChar;
    if (c == null) throw Exception('Karakteristik enroll tidak ditemukan');
    try {
      await writeCommand('AUTOSCAN OFF');
      await Future<void>.delayed(const Duration(milliseconds: 300));
    } catch (_) {}
    await _writeBytes(
      c,
      utf8.encode(jsonEncode({'employeeId': employeeId, 'name': name})),
    );
  }

  Future<void> deleteFinger(int id) async {
    final c = _deleteChar;
    if (c == null) throw Exception('Karakteristik delete tidak ditemukan');
    await _writeBytes(c, utf8.encode('$id'));
  }

  Future<Map<String, dynamic>> readSettings() async {
    final c = _settingsChar;
    if (c == null) return {};
    try {
      final bytes = await c.read();
      if (bytes.isEmpty) return {};
      final json = jsonDecode(utf8.decode(bytes));
      if (json is Map) return Map<String, dynamic>.from(json);
    } catch (_) {}
    return {};
  }

  /// Baca riwayat absensi lokal dari ESP32 (BLE char 4fafc208).
  /// Return list AttendanceRecord lokal (synced=false = belum ter-upload server).
  Future<List<AttendanceRecord>> readLocalHistory() async {
    final c = _historyChar;
    if (c == null) return [];
    try {
      final bytes = await c.read();
      if (bytes.isEmpty) return [];
      final raw = utf8.decode(bytes, allowMalformed: true);
      final json = jsonDecode(raw);
      if (json is! List) return [];
      return json
          .map((e) => AttendanceRecord.fromJson(Map<String, dynamic>.from(e)))
          .toList();
    } catch (_) {
      return [];
    }
  }

  Future<void> writeSettings(Map<String, dynamic> data) async {
    final c = _settingsChar;
    if (c == null) throw Exception('Karakteristik settings tidak ditemukan');
    await _writeBytes(c, utf8.encode(jsonEncode(data)));
  }

  Future<void> disconnect() async {
    _manualDisconnect = true;
    _stopAutoReconnect();
    _statusSub?.cancel();
    _eventsSub?.cancel();
    await _connSub?.cancel();
    _connSub = null;
    try {
      await _device?.disconnect();
    } catch (_) {}
    _device = null;
    _connected = false;
    _connectedName = '';
    _resetCharacteristics();
    _connectionController.add(false);
    notifyListeners();
  }

  @override
  void dispose() {
    _statusSub?.cancel();
    _eventsSub?.cancel();
    _connSub?.cancel();
    _statusController.close();
    _eventController.close();
    _connectionController.close();
    super.dispose();
  }
}
