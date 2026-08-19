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
  BluetoothCharacteristic? _enrollListChar;
  StreamSubscription? _connSub;
  StreamSubscription? _statusSub;
  StreamSubscription? _eventsSub;

  /// Cache metadata LIST PAGE — hindari baca BLE berulang antar layar.
  List<Map<String, dynamic>>? _enrollListCache;
  DateTime? _enrollListCacheAt;
  static const _enrollListTtl = Duration(seconds: 45);

  void invalidateEnrollListCache() {
    _enrollListCache = null;
    _enrollListCacheAt = null;
  }

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
  String get connectedDeviceId => _device?.remoteId.str ?? '';
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
    _enrollListChar = null;
    invalidateEnrollListCache();
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

      // MTU tinggi WAJIB sebelum baca template hex 512 byte. Default Android ~23
      // → read characteristic panjang gagal/kosong → enroll_cache tanpa hex → Sync skip.
      try {
        final mtu = await _device!.requestMtu(517);
        debugPrint('[BLE] MTU negotiated=$mtu');
      } catch (e) {
        debugPrint('[BLE] requestMtu failed: $e');
      }

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
          // Default FBP mtu=512 → ATT payload max 511 → hex 512 char terpotong/gagal parse.
          // 517 memungkinkan Read Response muat 512 byte penuh.
          mtu: 517,
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
        if (_uuidEq(u, kEnrollListCharUuid)) _enrollListChar = c;
      }
    }

    _statusSub?.cancel();
    _eventsSub?.cancel();

    _statusSub = _statusChar?.lastValueStream.listen((bytes) {
      if (bytes.isEmpty) return;
      try {
        final json = jsonDecode(utf8.decode(bytes));
        if (json is Map) {
          final next =
              DeviceStatus.fromJson(Map<String, dynamic>.from(json));
          if (next == _status) return;
          _status = next;
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
          final next =
              DeviceStatus.fromJson(Map<String, dynamic>.from(json));
          if (next != _status) {
            _status = next;
            notifyListeners();
          }
        }
      }
    } catch (_) {}
    return _status;
  }

  Future<void> _writeBytes(
    BluetoothCharacteristic c,
    List<int> bytes, {
    bool longWrite = false,
  }) async {
    final timeout = longWrite ? 45 : 15;
    try {
      await c.write(
        bytes,
        withoutResponse: false,
        allowLongWrite: longWrite,
        timeout: timeout,
      );
    } catch (_) {
      if (longWrite) {
        // Hex 512B wajib long-write — jangan fallback ke write pendek.
        rethrow;
      }
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

  /// Sync template dari server → sensor. Kirim daftar employeeId (max 30).
  /// Firmware baca JSON `{"employeeIds":[...]}` dari /ble_sync.json.
  /// @deprecated Prefer [putTemplate] — unduh hex di HP dulu, kirim via BLE.
  Future<void> syncTemplates(List<String> employeeIds) async {
    if (employeeIds.isEmpty) return;
    final ids = employeeIds.take(30).toList();
    final payload = jsonEncode({'employeeIds': ids});
    await writeCommand('SYNC_TEMPLATES $payload');
  }

  /// Kirim template hex dari HP → ESP32 (PUT_TEMPLATE meta + hex 512 via char 209).
  Future<({String status, int fingerId})> putTemplate({
    required String employeeId,
    required String name,
    required String templateHex,
    int fingerId = 0,
  }) async {
    if (employeeId.isEmpty) throw Exception('employeeId kosong');
    if (templateHex.length != 512) {
      throw Exception('Hex template harus 512 karakter');
    }
    final enrollChar = _enrollListChar;
    if (enrollChar == null) {
      throw Exception('Karakteristik enroll list tidak ditemukan');
    }

    try {
      final mtu = _device?.mtuNow ?? 0;
      if (mtu > 0 && mtu < 520) {
        await _device!.requestMtu(517);
      }
    } catch (_) {}

    final completer = Completer<({String status, int fingerId})>();
    StreamSubscription? sub;
    var readyReceived = false;
    sub = _eventController.stream.listen((ev) {
      final evEmp = ev.data['employeeId']?.toString() ?? '';
      if (evEmp.isNotEmpty && evEmp != employeeId) return;

      if (ev.type == 'put_template_ready') {
        readyReceived = true;
        return;
      }

      // Tunggu put_template_done — sync_progress skipped_local bawa id slot.
      if (ev.type == 'put_template_done') {
        if (completer.isCompleted) return;
        final ok = ev.data['ok'] == true;
        final slot = (ev.data['id'] as num?)?.toInt() ?? 0;
        final st = ev.data['status']?.toString() ??
            (ok ? 'restored' : 'write_fail');
        completer.complete((status: st, fingerId: slot));
      } else if (ev.type == 'sync_progress') {
        final st = ev.data['status']?.toString() ?? '';
        final slot = (ev.data['id'] as num?)?.toInt() ?? 0;
        if (!completer.isCompleted &&
            slot > 0 &&
            (st == 'skipped_local' || st == 'restored')) {
          completer.complete((status: st, fingerId: slot));
        } else if (!completer.isCompleted &&
            (st == 'write_fail' ||
                st == 'bad_hex' ||
                st == 'no_slot')) {
          completer.complete((status: st, fingerId: 0));
        }
      } else if (ev.type == 'put_template_fail') {
        if (!completer.isCompleted) {
          completer.completeError(Exception(
              ev.data['reason']?.toString() ?? 'put_template gagal'));
        }
      }
    });

    try {
      await writeCommand(
        'PUT_TEMPLATE ${jsonEncode({
          'employeeId': employeeId,
          'name': name,
          'fingerId': fingerId,
        })}',
      );
      // Tunggu ESP32 siap terima hex (event put_template_ready).
      final readyDeadline = DateTime.now().add(const Duration(seconds: 8));
      while (!readyReceived && DateTime.now().isBefore(readyDeadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 50));
      }
      if (!readyReceived) {
        throw Exception('ESP32 tidak siap terima hex (put_template_ready timeout)');
      }
      await Future<void>.delayed(const Duration(milliseconds: 150));
      // Hex 512B melebihi default ATT MTU — wajib allowLongWrite.
      await _writeBytes(
        enrollChar,
        utf8.encode(templateHex),
        longWrite: true,
      );
      return await completer.future.timeout(const Duration(seconds: 60));
    } finally {
      await sub.cancel();
      invalidateEnrollListCache();
    }
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
    invalidateEnrollListCache();
  }

  Future<void> deleteFinger(int id) async {
    final c = _deleteChar;
    if (c == null) throw Exception('Karakteristik delete tidak ditemukan');
    await _writeBytes(c, utf8.encode('$id'));
    invalidateEnrollListCache();
  }

  /// Hapus finger by employeeId (ESP32 hapus di sensor + DB, server via API).
  Future<void> deleteFingerByEmployee(String employeeId) async {
    if (employeeId.isEmpty) return;
    await writeCommand('DELETE_EMP $employeeId');
    invalidateEnrollListCache();
  }

  /// Bersihkan SEMUA fingerprint di sensor ESP32 + DB lokal.
  /// Dipakai sebelum sinkron ulang dari server (data app/server vs sensor).
  Future<void> cleanFingers() async {
    await writeCommand('CLEAN_FINGERS');
    invalidateEnrollListCache();
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

  /// Minta daftar fingerprint terdaftar dari ESP32 (BLE char 4fafc209).
  /// Return list {id, name, employeeId}.
  /// Minta daftar fingerprint terdaftar dari ESP32 (BLE char 4fafc209).
  /// Return list {id, name, employeeId} — METADATA SAJA (kecil), bukan hex.
  /// Hex diambil per-id via readEnrollTemplate (bertahap, hindari 1 file besar).
  Future<Map<String, dynamic>?> _readEnrollListPage(int page) async {
    final c = _enrollListChar;
    if (c == null) return null;
    await c.write(utf8.encode('LIST PAGE $page'), withoutResponse: true);
    for (var attempt = 0; attempt < 8; attempt++) {
      await Future<void>.delayed(
          Duration(milliseconds: attempt == 0 ? 450 : 250));
      final bytes = await c.read();
      if (bytes.isEmpty) continue;
      final raw = utf8.decode(bytes, allowMalformed: true);
      try {
        final json = jsonDecode(raw);
        if (json is Map && json['items'] is List) {
          return Map<String, dynamic>.from(json);
        }
      } catch (_) {}
    }
    return null;
  }

  Future<List<Map<String, dynamic>>> readEnrollList({
    bool forceRefresh = false,
  }) async {
    if (!forceRefresh &&
        _enrollListCache != null &&
        _enrollListCacheAt != null &&
        DateTime.now().difference(_enrollListCacheAt!) < _enrollListTtl) {
      return _enrollListCache!
          .map((e) => Map<String, dynamic>.from(e))
          .toList();
    }

    final c = _enrollListChar;
    if (c == null) throw Exception('Karakteristik enroll list tidak ditemukan');

    // LIST penuh sering > MTU (~500B) → gagal parse. Pakai LIST PAGE (≤4 item).
    final first = await _readEnrollListPage(0);
    if (first != null) {
      final all = <Map<String, dynamic>>[];
      void addItems(Map<String, dynamic> page) {
        for (final e in page['items'] as List) {
          if (e is Map) all.add(Map<String, dynamic>.from(e));
        }
      }

      addItems(first);
      final pages = (first['pages'] as num?)?.toInt() ?? 1;
      for (var p = 1; p < pages; p++) {
        final pg = await _readEnrollListPage(p);
        if (pg != null) addItems(pg);
      }
      _enrollListCache =
          all.map((e) => Map<String, dynamic>.from(e)).toList();
      _enrollListCacheAt = DateTime.now();
      return all;
    }

    await c.write(utf8.encode('LIST'), withoutResponse: true);
    for (var attempt = 0; attempt < 5; attempt++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      final bytes = await c.read();
      if (bytes.isEmpty) continue;
      final raw = utf8.decode(bytes, allowMalformed: true);
      try {
        final json = jsonDecode(raw);
        if (json is! List) continue;
        final all = json
            .map((e) => Map<String, dynamic>.from(e as Map))
            .toList();
        _enrollListCache =
            all.map((e) => Map<String, dynamic>.from(e)).toList();
        _enrollListCacheAt = DateTime.now();
        return all;
      } catch (_) {
        continue;
      }
    }
    return [];
  }

  /// Parse value karakteristik enroll-list → hex 512 atau null (belum siap / error).
  Map<String, dynamic>? _parseEnrollTemplateValue(List<int> bytes, int fingerId) {
    if (bytes.isEmpty) return null;
    final raw = utf8.decode(bytes, allowMalformed: true).trim();
    if (raw.isEmpty) return null;
    // Masih value LIST lama — belum diganti ESP32.
    if (raw.startsWith('[')) return null;

    // Sukses format baru: exact 512 hex digit (tanpa JSON).
    if (!raw.startsWith('{') && raw.length >= 512) {
      final hex = raw.substring(0, 512);
      if (RegExp(r'^[0-9a-fA-F]{512}$').hasMatch(hex)) {
        return {'id': fingerId, 'hex': hex, 'ok': true};
      }
    }

    if (raw.startsWith('{')) {
      try {
        final json = jsonDecode(raw);
        if (json is! Map) return null;
        final m = Map<String, dynamic>.from(json);
        if (m['ok'] == false) return {'id': fingerId, 'ok': false};
        final respId = (m['id'] as num?)?.toInt();
        if (respId != null && respId != fingerId) return null;
        final hex = m['hex']?.toString() ?? '';
        if (hex.length >= 512) {
          return {
            'id': fingerId,
            'hex': hex.substring(0, 512),
            'ok': true,
          };
        }
      } catch (_) {
        return null;
      }
    }
    return null;
  }

  /// Baca hex yang baru saja di-push ESP32 setelah enroll (tanpa GET_TEMPLATE).
  /// Characteristic 4fafc209 berisi 512 hex mentah.
  Future<String?> readPushedEnrollHex() async {
    final c = _enrollListChar;
    if (c == null) return null;
    try {
      final mtu = _device?.mtuNow ?? 0;
      if (mtu > 0 && mtu < 520) {
        await _device!.requestMtu(517);
      }
    } catch (_) {}
    for (var i = 0; i < 6; i++) {
      if (i > 0) {
        await Future<void>.delayed(Duration(milliseconds: 80 * i));
      }
      try {
        final bytes = await c.read();
        final parsed = _parseEnrollTemplateValue(bytes, 0);
        final hex = parsed?['hex']?.toString() ?? '';
        if (hex.length == 512) {
          debugPrint('[BLE] pushed enroll hex OK');
          return hex;
        }
      } catch (e) {
        debugPrint('[BLE] pushed hex read: $e');
      }
    }
    return null;
  }

  /// Minta template hex dari ESP32 (BLE char 4fafc209) per id finger.
  /// Return Map {id, hex, ok:true} atau null jika tidak ada.
  /// Firmware baru: sukses = raw 512 hex digit (tanpa JSON).
  Future<Map<String, dynamic>?> readEnrollTemplate(int fingerId) async {
    final c = _enrollListChar;
    if (c == null) throw Exception('Karakteristik enroll list tidak ditemukan');

    // Pastikan MTU cukup sebelum baca 512 byte.
    try {
      final mtu = _device?.mtuNow ?? 0;
      if (mtu > 0 && mtu < 520) {
        final n = await _device!.requestMtu(517);
        debugPrint('[BLE] GET_TEMPLATE bump MTU $mtu → $n');
      }
    } catch (_) {}

    // Tulis ulang + poll; beberapa HP butuh write-with-response.
    for (var round = 0; round < 3; round++) {
      try {
        await c.write(utf8.encode('GET_TEMPLATE $fingerId'),
            withoutResponse: false);
      } catch (_) {
        await c.write(utf8.encode('GET_TEMPLATE $fingerId'),
            withoutResponse: true);
      }
      debugPrint('[BLE] GET_TEMPLATE id=$fingerId round=${round + 1}');

      for (var attempt = 0; attempt < 10; attempt++) {
        await Future<void>.delayed(
            Duration(milliseconds: attempt == 0 ? 350 : 300));
        List<int> bytes;
        try {
          bytes = await c.read();
        } catch (e) {
          debugPrint('[BLE] GET_TEMPLATE read err: $e');
          continue;
        }
        debugPrint(
            '[BLE] GET_TEMPLATE read#$attempt len=${bytes.length}');
        final parsed = _parseEnrollTemplateValue(bytes, fingerId);
        if (parsed == null) continue;
        if (parsed['ok'] == false) {
          debugPrint('[BLE] GET_TEMPLATE id=$fingerId → ok:false');
          return null;
        }
        final hex = parsed['hex']?.toString() ?? '';
        if (hex.length == 512) {
          debugPrint('[BLE] GET_TEMPLATE id=$fingerId HEX OK');
          return parsed;
        }
      }
    }
    debugPrint('[BLE] GET_TEMPLATE id=$fingerId FAILED');
    return null;
  }

  /// Sync daftar enroll dari ESP32 secara BERTAHAP (per-id, bukan 1 file besar).
  /// Return list {id, name, employeeId, hex?} — hex hanya jika berhasil dibaca (512).
  Future<List<Map<String, dynamic>>> syncEnrollsIncremental({
    void Function(int done, int total)? onProgress,
  }) async {
    final list = await readEnrollList(forceRefresh: true);
    final total = list.length;
    final result = <Map<String, dynamic>>[];
    for (var i = 0; i < list.length; i++) {
      final en = list[i];
      final id = (en['id'] as num?)?.toInt() ?? 0;
      final empId = en['employeeId']?.toString() ?? '';
      final item = Map<String, dynamic>.from(en);
      item.remove('hex'); // jangan bawa hex lama/kosong dari LIST
      if (id > 0 && empId.isNotEmpty) {
        try {
          final tpl = await readEnrollTemplate(id);
          final hex = tpl?['hex']?.toString() ?? '';
          if (hex.length == 512) {
            item['hex'] = hex;
          } else {
            debugPrint('[BLE] sync skip hex id=$id len=${hex.length}');
          }
        } catch (e) {
          debugPrint('[BLE] sync template err id=$id: $e');
        }
      }
      result.add(item);
      onProgress?.call(i + 1, total);
    }
    return result;
  }

  Future<void> writeSettings(Map<String, dynamic> data) async {
    final c = _settingsChar;
    if (c == null) throw Exception('Karakteristik settings tidak ditemukan');
    await _writeBytes(c, utf8.encode(jsonEncode(data)));
  }

  /// Lepas koneksi BLE. [forget] = true jika tombol Lepas: device terakhir
  /// dihapus supaya app tidak auto-reconnect saat dibuka lagi.
  Future<void> disconnect({bool forget = false}) async {
    _manualDisconnect = true;
    _connecting = false;
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
    _linkEstablished = false;
    _connectedName = '';
    _resetCharacteristics();
    if (forget) {
      _settings?.lastBleDeviceId = '';
      _settings?.lastBleDeviceName = '';
    }
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
