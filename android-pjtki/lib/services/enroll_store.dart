import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

/// Rekord enroll lokal di Android — disimpan SEBELUM kirim ke ESP32.
class LocalEnrollRecord {
  final String employeeId;
  final String name;
  final int fingerId;
  final String hex;
  /// pending | enrolling | enrolled | failed
  final String status;
  final bool serverSynced;
  final String savedAt;
  final String updatedAt;

  const LocalEnrollRecord({
    required this.employeeId,
    required this.name,
    this.fingerId = 0,
    this.hex = '',
    this.status = 'pending',
    this.serverSynced = false,
    required this.savedAt,
    required this.updatedAt,
  });

  bool get hasHex => hex.length == 512;
  bool get isEnrolledOnDevice => fingerId > 0 && status == 'enrolled';

  LocalEnrollRecord copyWith({
    String? name,
    int? fingerId,
    String? hex,
    String? status,
    bool? serverSynced,
    String? updatedAt,
  }) {
    return LocalEnrollRecord(
      employeeId: employeeId,
      name: name ?? this.name,
      fingerId: fingerId ?? this.fingerId,
      hex: hex ?? this.hex,
      status: status ?? this.status,
      serverSynced: serverSynced ?? this.serverSynced,
      savedAt: savedAt,
      updatedAt: updatedAt ?? this.updatedAt,
    );
  }

  Map<String, dynamic> toJson() => {
        'employeeId': employeeId,
        'name': name,
        'id': fingerId,
        'hex': hex,
        'status': status,
        'serverSynced': serverSynced,
        'savedAt': savedAt,
        'updatedAt': updatedAt,
      };

  /// Ringkas untuk ListView — hex disimpan di EnrollStore, bukan di widget tree.
  Map<String, dynamic> toJsonForUi() => {
        'employeeId': employeeId,
        'name': name,
        'id': fingerId,
        'hex': hex.length == 512 ? '' : hex,
        'hasHex': hex.length == 512,
        'status': status,
        'serverSynced': serverSynced,
        'savedAt': savedAt,
        'updatedAt': updatedAt,
      };

  factory LocalEnrollRecord.fromJson(Map<String, dynamic> json) {
    final now = DateTime.now().toIso8601String();
    return LocalEnrollRecord(
      employeeId: json['employeeId']?.toString() ?? '',
      name: json['name']?.toString() ?? '',
      fingerId: (json['id'] as num?)?.toInt() ?? 0,
      hex: json['hex']?.toString() ?? '',
      status: json['status']?.toString() ?? 'enrolled',
      serverSynced: json['serverSynced'] == true,
      savedAt: json['savedAt']?.toString() ?? now,
      updatedAt: json['updatedAt']?.toString() ?? now,
    );
  }
}

/// Penyimpanan lokal-first untuk data enroll (nama + employeeId wajib ada sebelum BLE).
class EnrollStore {
  static const _kEnrollLocal = 'enroll_local_v1';
  static const _kEnrollCacheLegacy = 'enroll_cache';

  SharedPreferences? _prefs;
  List<LocalEnrollRecord> _records = [];

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    await _load();
    await _migrateLegacyCache();
  }

  SharedPreferences get prefs {
    if (_prefs == null) throw StateError('EnrollStore belum di-init');
    return _prefs!;
  }

  List<LocalEnrollRecord> get all => List.unmodifiable(_records);

  Set<String> get employeeIds =>
      _records.map((e) => e.employeeId).where((id) => id.isNotEmpty).toSet();

  LocalEnrollRecord? findByEmployee(String employeeId) {
    try {
      return _records.firstWhere((e) => e.employeeId == employeeId);
    } catch (_) {
      return null;
    }
  }

  /// Simpan metadata enroll SEBELUM kirim perintah ke ESP32.
  Future<void> savePending(String employeeId, String name) async {
    if (employeeId.isEmpty || name.isEmpty) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      final prev = _records[idx];
      _records[idx] = prev.copyWith(
        name: name,
        status: 'pending',
        updatedAt: now,
      );
    } else {
      _records.add(LocalEnrollRecord(
        employeeId: employeeId,
        name: name,
        status: 'pending',
        savedAt: now,
        updatedAt: now,
      ));
    }
    await _persist();
  }

  Future<void> markEnrolling(String employeeId) async {
    await _updateStatus(employeeId, 'enrolling');
  }

  Future<void> markEnrolled(String employeeId, int fingerId, {String? hex}) async {
    if (employeeId.isEmpty) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      final prev = _records[idx];
      _records[idx] = prev.copyWith(
        fingerId: fingerId > 0 ? fingerId : prev.fingerId,
        hex: hex ?? prev.hex,
        status: 'enrolled',
        updatedAt: now,
      );
    } else {
      _records.add(LocalEnrollRecord(
        employeeId: employeeId,
        name: employeeId,
        fingerId: fingerId,
        hex: hex ?? '',
        status: 'enrolled',
        savedAt: now,
        updatedAt: now,
      ));
    }
    await _persist();
  }

  Future<void> markFailed(String employeeId) async {
    await _updateStatus(employeeId, 'failed');
  }

  Future<void> setHex(String employeeId, int fingerId, String hex) async {
    if (employeeId.isEmpty || hex.length != 512) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      final prev = _records[idx];
      _records[idx] = prev.copyWith(
        fingerId: fingerId > 0 ? fingerId : prev.fingerId,
        hex: hex,
        status: prev.status == 'pending' || prev.status == 'enrolling'
            ? 'enrolled'
            : prev.status,
        updatedAt: now,
      );
    } else {
      _records.add(LocalEnrollRecord(
        employeeId: employeeId,
        name: employeeId,
        fingerId: fingerId,
        hex: hex,
        status: 'enrolled',
        savedAt: now,
        updatedAt: now,
      ));
    }
    await _persist();
  }

  Future<void> markServerSynced(String employeeId) async {
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx < 0) return;
    _records[idx] = _records[idx].copyWith(
      serverSynced: true,
      updatedAt: DateTime.now().toIso8601String(),
    );
    await _persist();
  }

  /// Simpan template unduhan server di HP (belum / sudah siap kirim ke ESP32).
  Future<void> saveServerDownload({
    required String employeeId,
    required String name,
    required String hex,
    int fingerId = 0,
  }) async {
    if (employeeId.isEmpty || hex.length != 512) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      final prev = _records[idx];
      _records[idx] = prev.copyWith(
        name: name.isNotEmpty ? name : prev.name,
        fingerId: fingerId > 0 ? fingerId : prev.fingerId,
        hex: hex,
        status: 'server_cached',
        updatedAt: now,
      );
    } else {
      _records.add(LocalEnrollRecord(
        employeeId: employeeId,
        name: name.isNotEmpty ? name : employeeId,
        fingerId: fingerId,
        hex: hex,
        status: 'server_cached',
        savedAt: now,
        updatedAt: now,
      ));
    }
    await _persist();
  }

  /// Update hex dari server tanpa menurunkan status `enrolled` (sudah di sensor).
  Future<void> updateServerHex({
    required String employeeId,
    required String name,
    required String hex,
    int fingerId = 0,
  }) async {
    if (employeeId.isEmpty || hex.length != 512) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      final prev = _records[idx];
      final keepEnrolled = prev.status == 'enrolled' && prev.fingerId > 0;
      _records[idx] = prev.copyWith(
        name: name.isNotEmpty ? name : prev.name,
        fingerId: fingerId > 0 ? fingerId : prev.fingerId,
        hex: hex,
        status: keepEnrolled ? 'enrolled' : 'server_cached',
        updatedAt: now,
      );
    } else {
      await saveServerDownload(
        employeeId: employeeId,
        name: name,
        hex: hex,
        fingerId: fingerId,
      );
      return;
    }
    await _persist();
  }

  /// Cocokkan status lokal dengan daftar finger di ESP32 (setelah PUT_TEMPLATE).
  Future<int> reconcileWithDeviceList(
      List<Map<String, dynamic>> deviceList) async {
    var updated = 0;
    final now = DateTime.now().toIso8601String();
    for (final en in deviceList) {
      final empId = en['employeeId']?.toString() ?? '';
      final id = (en['id'] as num?)?.toInt() ?? 0;
      if (id <= 0) continue;

      var idx = empId.isNotEmpty
          ? _records.indexWhere((e) => e.employeeId == empId)
          : -1;
      if (idx < 0) {
        idx = _records.indexWhere((e) =>
            e.fingerId == id &&
            (e.status == 'server_cached' || e.status == 'enrolled'));
      }
      if (idx < 0) continue;
      final prev = _records[idx];
      if (prev.status == 'enrolled' && prev.fingerId == id) continue;
      _records[idx] = prev.copyWith(
        fingerId: id,
        status: 'enrolled',
        name: (en['name']?.toString().isNotEmpty == true)
            ? en['name'].toString()
            : prev.name,
        updatedAt: now,
      );
      updated++;
    }
    if (updated > 0) await _persist();
    return updated;
  }

  /// Tandai template sudah masuk sensor ESP32 setelah PUT_TEMPLATE BLE.
  Future<void> markOnDevice(String employeeId, int fingerId) async {
    if (employeeId.isEmpty || fingerId <= 0) return;
    final now = DateTime.now().toIso8601String();
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx >= 0) {
      _records[idx] = _records[idx].copyWith(
        fingerId: fingerId,
        status: 'enrolled',
        updatedAt: now,
      );
    }
    await _persist();
  }

  String? cachedHex(String employeeId) {
    final rec = findByEmployee(employeeId);
    if (rec == null) return null;
    return rec.hex.length == 512 ? rec.hex : null;
  }

  Future<void> removeByEmployee(String employeeId) async {
    _records.removeWhere((e) => e.employeeId == employeeId);
    await _persist();
  }

  /// Hapus semua record status `failed` (enroll gagal, tidak ada di sensor).
  Future<int> removeFailed() async {
    final before = _records.length;
    _records.removeWhere((e) => e.status == 'failed');
    final removed = before - _records.length;
    if (removed > 0) await _persist();
    return removed;
  }

  List<LocalEnrollRecord> get failedRecords =>
      _records.where((e) => e.status == 'failed').toList();

  /// Gabungkan metadata dari ESP32 — Android tetap sumber nama jika ESP kosong.
  /// [metadataOnly]: tidak menulis ulang hex ke disk jika tidak berubah (refresh cepat).
  Future<List<Map<String, dynamic>>> mergeFromDevice(
    List<Map<String, dynamic>> deviceList, {
    bool metadataOnly = false,
  }) async {
    final byEmp = <String, LocalEnrollRecord>{
      for (final r in _records) r.employeeId: r,
    };
    final byFinger = <int, LocalEnrollRecord>{
      for (final r in _records)
        if (r.fingerId > 0) r.fingerId: r,
    };
    final seenEmp = <String>{};
    final mergedMaps = <Map<String, dynamic>>[];

    for (final en in deviceList) {
      final id = (en['id'] as num?)?.toInt() ?? 0;
      var empId = en['employeeId']?.toString() ?? '';
      var name = en['name']?.toString() ?? '';
      final hexDev = en['hex']?.toString() ?? '';

      LocalEnrollRecord? local = empId.isNotEmpty ? byEmp[empId] : null;
      local ??= id > 0 ? byFinger[id] : null;

      if (local != null) {
        if (name.isEmpty) name = local.name;
        if (empId.isEmpty) empId = local.employeeId;
      }

      if (empId.isEmpty && id <= 0) continue;
      seenEmp.add(empId);

      final hex = hexDev.length == 512
          ? hexDev
          : (local?.hasHex == true ? local!.hex : '');

      final now = DateTime.now().toIso8601String();
      final record = LocalEnrollRecord(
        employeeId: empId.isNotEmpty ? empId : (local?.employeeId ?? ''),
        name: name.isNotEmpty ? name : (local?.name ?? empId),
        fingerId: id > 0 ? id : (local?.fingerId ?? 0),
        hex: hex,
        status: id > 0 ? 'enrolled' : (local?.status ?? 'pending'),
        serverSynced: local?.serverSynced ?? false,
        savedAt: local?.savedAt ?? now,
        updatedAt: now,
      );

      if (record.employeeId.isNotEmpty) {
        byEmp[record.employeeId] = record;
        if (record.fingerId > 0) byFinger[record.fingerId] = record;
      }
      mergedMaps.add(record.toJson());
    }

    // Entri lokal yang belum ada di sensor terhubung → kembalikan ke antrean kirim.
    final nowOff = DateTime.now().toIso8601String();
    for (final r in _records) {
      if (r.employeeId.isEmpty || seenEmp.contains(r.employeeId)) continue;
      var kept = r;
      // Pernah marked enrolled dari device lain (3V3) — sensor ini (5V) kosong.
      if ((r.status == 'enrolled' || r.status == 'server_cached') && r.hasHex) {
        kept = r.copyWith(
          status: 'server_cached',
          fingerId: 0,
          updatedAt: nowOff,
        );
      }
      mergedMaps.add(kept.toJson());
      seenEmp.add(r.employeeId);
    }

    final newRecords = mergedMaps
        .map((m) => LocalEnrollRecord.fromJson(m))
        .where((r) => r.employeeId.isNotEmpty)
        .toList();

    var changed = newRecords.length != _records.length;
    if (!changed) {
      final byEmp = {for (final r in _records) r.employeeId: r};
      for (final r in newRecords) {
        final prev = byEmp[r.employeeId];
        if (prev == null ||
            prev.fingerId != r.fingerId ||
            prev.name != r.name ||
            prev.status != r.status ||
            prev.serverSynced != r.serverSynced ||
            (!metadataOnly && prev.hex != r.hex)) {
          changed = true;
          break;
        }
      }
    }

    _records = newRecords;
    if (changed) await _persist();

    if (metadataOnly) {
      return toJsonListForUi();
    }
    return mergedMaps;
  }

  List<Map<String, dynamic>> toJsonList() =>
      _records.map((e) => e.toJson()).toList();

  /// Daftar untuk UI — tanpa hex 512 char (hemat memori & rebuild ListView).
  List<Map<String, dynamic>> toJsonListForUi() =>
      _records.map((e) => e.toJsonForUi()).toList();

  Future<void> _updateStatus(String employeeId, String status) async {
    final idx = _records.indexWhere((e) => e.employeeId == employeeId);
    if (idx < 0) return;
    _records[idx] = _records[idx].copyWith(
      status: status,
      updatedAt: DateTime.now().toIso8601String(),
    );
    await _persist();
  }

  Future<void> _load() async {
    final raw = prefs.getString(_kEnrollLocal) ?? '';
    if (raw.isEmpty) {
      _records = [];
      return;
    }
    try {
      final json = jsonDecode(raw);
      if (json is! List) {
        _records = [];
        return;
      }
      _records = json
          .map((e) => LocalEnrollRecord.fromJson(Map<String, dynamic>.from(e)))
          .where((r) => r.employeeId.isNotEmpty)
          .toList();
    } catch (_) {
      _records = [];
    }
  }

  Future<void> _persist() async {
    await prefs.setString(
      _kEnrollLocal,
      jsonEncode(_records.map((e) => e.toJson()).toList()),
    );
  }

  Future<void> _migrateLegacyCache() async {
    if (_records.isNotEmpty) return;
    final legacy = prefs.getString(_kEnrollCacheLegacy) ?? '';
    if (legacy.isEmpty) return;
    try {
      final json = jsonDecode(legacy);
      if (json is! List) return;
      final now = DateTime.now().toIso8601String();
      _records = json
          .map((e) {
            final m = Map<String, dynamic>.from(e as Map);
            m['status'] ??= 'enrolled';
            m['savedAt'] ??= now;
            m['updatedAt'] ??= now;
            return LocalEnrollRecord.fromJson(m);
          })
          .where((r) => r.employeeId.isNotEmpty)
          .toList();
      await _persist();
    } catch (_) {}
  }
}
