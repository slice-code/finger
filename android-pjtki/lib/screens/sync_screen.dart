import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/ble_service.dart';
import '../services/enroll_store.dart';
import '../services/settings_store.dart';

class SyncScreen extends StatefulWidget {
  const SyncScreen({super.key});

  @override
  State<SyncScreen> createState() => _SyncScreenState();
}

class _SyncScreenState extends State<SyncScreen>
    with SingleTickerProviderStateMixin {
  late TabController _tabController;
  // ── Tab Enroll ──
  List<Map<String, dynamic>> _enrolls = [];
  int _failedEnrollCount = 0;
  bool _loadingEnroll = false;
  bool _loadingMeta = false;
  bool _syncingEnroll = false;
  bool _syncingFromServer = false;
  bool _loadingServer = false;
  String? _syncingEmpId;
  int _lastServerNeedDownload = 0;

  List<Map<String, dynamic>> get _pendingOnDevice =>
      _enrolls.where(_enrollRowNeedsDeviceSync).toList();

  List<Map<String, dynamic>> get _needsHexOnPhone => _enrolls
      .where((e) =>
          (e['employeeId']?.toString() ?? '').isNotEmpty &&
          !_enrollRowHasHex(e))
      .toList();

  List<Map<String, dynamic>> get _readyUploadServer => _enrolls
      .where((e) =>
          _enrollRowHasHex(e) &&
          e['serverSynced'] != true &&
          (e['status']?.toString() ?? '') != 'failed')
      .toList();

  // ── Tab Absensi ──
  List<AttendanceRecord> _local = [];
  bool _loadingAtt = false;
  bool _uploadingAtt = false;

  String? _error;
  String _statusMsg = '';

  @override
  void initState() {
    super.initState();
    _tabController = TabController(length: 4, vsync: this);
    _loadEnrollCache();
  }

  @override
  void dispose() {
    _tabController.dispose();
    super.dispose();
  }

  /// Hilangkan hex 512 dari map UI (hex tetap di EnrollStore).
  List<Map<String, dynamic>> _uiEnrollList(
      List<Map<String, dynamic>> rows) {
    return rows
        .map((e) {
          final m = Map<String, dynamic>.from(e);
          final hex = m['hex']?.toString() ?? '';
          if (hex.length == 512) {
            m['hex'] = '';
            m['hasHex'] = true;
          } else {
            m['hasHex'] = m['hasHex'] == true || hex.isNotEmpty;
          }
          return m;
        })
        .toList();
  }

  bool get _busy =>
      _loadingEnroll ||
      _loadingMeta ||
      _syncingEnroll ||
      _syncingFromServer ||
      _loadingServer ||
      _syncingEmpId != null;

  Future<FingerTemplate?> _downloadTemplateToPhone(FingerTemplate meta) async {
    final store = context.read<EnrollStore>();
    final cached = store.cachedHex(meta.employeeId);
    if (cached != null) {
      return FingerTemplate(
        employeeId: meta.employeeId,
        nama: meta.nama,
        fingerId: meta.fingerId,
        hasHex: true,
        kodeCabang: meta.kodeCabang,
        hex: cached,
      );
    }
    final api = context.read<ApiService>();
    final full = await api.fetchTemplate(meta.employeeId);
    if (full == null || full.hex.length != 512) return null;
    await store.saveServerDownload(
      employeeId: full.employeeId,
      name: full.nama.isNotEmpty ? full.nama : meta.nama,
      hex: full.hex,
      fingerId: full.fingerId > 0 ? full.fingerId : meta.fingerId,
    );
    return full;
  }

  /// Unduh template dari server → penyimpanan HP (tanpa BLE / tanpa ESP32).
  Future<void> _downloadFromServer() async {
    setState(() {
      _loadingServer = true;
      _error = null;
      _statusMsg = 'Membaca daftar template di server...';
      _lastServerNeedDownload = 0;
    });
    try {
      final settings = context.read<SettingsStore>();
      if (settings.apiKey.isEmpty) {
        throw Exception('Device Key belum diisi — atur di Setelan');
      }
      final api = context.read<ApiService>();
      final store = context.read<EnrollStore>();
      final kode = settings.kodeCabang.trim();
      final server = await api.fetchTemplates(
        kodeCabang: kode.isEmpty ? null : kode,
      );
      if (!mounted) return;

      final needDownload = server
          .where((t) =>
              t.hasHex && store.cachedHex(t.employeeId) == null)
          .toList()
        ..sort((a, b) => a.nama.toLowerCase().compareTo(b.nama.toLowerCase()));

      setState(() => _lastServerNeedDownload = needDownload.length);

      if (needDownload.isEmpty) {
        setState(() {
          _statusMsg =
              'Semua template server (${server.length}) sudah ada hex di HP.';
        });
        _loadEnrollCache();
        return;
      }

      var ok = 0, fail = 0;
      var lastDownloadUi = 0;
      for (var i = 0; i < needDownload.length; i++) {
        final meta = needDownload[i];
        if (!mounted) return;
        final now = DateTime.now().millisecondsSinceEpoch;
        if (i == 0 ||
            i == needDownload.length - 1 ||
            now - lastDownloadUi > 450) {
          lastDownloadUi = now;
          setState(() => _statusMsg =
              'Mengunduh ke HP ${i + 1}/${needDownload.length}: ${meta.nama}...');
        }
        try {
          final full = await _downloadTemplateToPhone(meta);
          if (full != null && full.hex.length == 512) {
            ok++;
          } else {
            fail++;
          }
        } catch (e) {
          fail++;
          debugPrint('[SYNC] download ${meta.employeeId}: $e');
        }
      }

      if (!mounted) return;
      setState(() {
        _statusMsg = ok > 0
            ? 'Unduh selesai: $ok template tersimpan di HP'
                '${fail > 0 ? ', $fail gagal' : ''}.'
            : 'Gagal mengunduh template ke HP ($fail).';
      });
      _loadEnrollCache();
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loadingServer = false);
    }
  }

  Future<void> _pushAllToDevice() async {
    final pending = _pendingOnDevice;
    if (pending.isEmpty) {
      setState(() => _statusMsg = 'Tidak ada template di HP yang perlu ke sensor.');
      return;
    }
    final preview = pending
        .take(12)
        .map((e) => e['name']?.toString() ?? e['employeeId'])
        .join(', ');
    final extra = pending.length > 12 ? ' …' : '';
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Kirim semua ke Perangkat'),
        content: Text(
          '${pending.length} template akan dikirim dari HP ke ESP32 via Bluetooth.\n\n'
          '$preview$extra',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Batal'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Kirim semua'),
          ),
        ],
      ),
    );
    if (ok != true || !mounted) return;

    final store = context.read<EnrollStore>();
    final api = context.read<ApiService>();
    final items = <FingerTemplate>[];
    for (final e in pending) {
      final empId = e['employeeId']?.toString() ?? '';
      if (empId.isEmpty) continue;
      try {
        final full = await api.fetchTemplate(empId);
        if (full != null && full.hex.length == 512) {
          items.add(full);
          continue;
        }
      } catch (_) {}
      final hex = store.cachedHex(empId);
      if (hex != null) {
        items.add(FingerTemplate(
          employeeId: empId,
          nama: e['name']?.toString() ?? empId,
          fingerId: (e['id'] as num?)?.toInt() ?? 0,
          hasHex: true,
          hex: hex,
        ));
      }
    }
    if (items.isEmpty) {
      setState(() => _error = 'Tidak ada hex di HP/server untuk dikirim.');
      return;
    }
    await _pushTemplatesToDevice(items);
  }

  /// Kirim satu template dari cache HP → ESP32 (tombol per baris).
  Future<void> _syncOneToDevice(Map<String, dynamic> e) async {
    final empId = e['employeeId']?.toString() ?? '';
    if (empId.isEmpty) return;
    final store = context.read<EnrollStore>();
    final rec = store.findByEmployee(empId);
    final name = rec?.name.isNotEmpty == true
        ? rec!.name
        : (e['name']?.toString() ?? empId);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Sync ke Perangkat'),
        content: Text(
          'Kirim sidik jari $name ($empId) ke sensor ESP32?\n'
          '(Hex diambil dari server jika belum ada di HP)',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Batal'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Sync'),
          ),
        ],
      ),
    );
    if (ok != true || !mounted) return;
    setState(() => _syncingEmpId = empId);
    try {
      final api = context.read<ApiService>();
      final full = await api.fetchTemplate(empId);
      if (full == null || full.hex.length != 512) {
        if (mounted) {
          setState(() =>
              _error = 'Template $name tidak ada / hex kosong di server');
        }
        return;
      }
      await store.saveServerDownload(
        employeeId: full.employeeId,
        name: full.nama.isNotEmpty ? full.nama : name,
        hex: full.hex,
        fingerId: full.fingerId > 0 ? full.fingerId : 0,
      );
      await _pushTemplatesToDevice([full], markBusy: false);
    } finally {
      if (mounted) setState(() => _syncingEmpId = null);
    }
  }

  /// Setelah kirim ke ESP32 — samakan status HP dengan daftar finger di sensor.
  Future<void> _reconcileAfterDevicePush() async {
    final ble = context.read<BleService>();
    final store = context.read<EnrollStore>();
    if (!ble.isConnected) {
      _loadEnrollCache();
      return;
    }
    try {
      final list = await ble.readEnrollList(forceRefresh: true);
      if (!mounted) return;
      await store.reconcileWithDeviceList(list);
      final merged =
          await store.mergeFromDevice(list, metadataOnly: true);
      if (!mounted) return;
      setState(() => _setEnrolls(merged));
    } catch (e) {
      debugPrint('[SYNC] reconcile: $e');
      _loadEnrollCache();
    }
  }

  /// HP → ESP32: kirim hex yang sudah diunduh ke storage Android.
  Future<void> _pushTemplatesToDevice(
    List<FingerTemplate> items, {
    bool markBusy = true,
  }) async {
    final ble = context.read<BleService>();
    final store = context.read<EnrollStore>();
    final api = context.read<ApiService>();
    if (!ble.isConnected) {
      setState(() => _error = 'Hubungkan device ESP32 dulu (Dashboard)');
      return;
    }
    if (items.isEmpty) return;
    if (markBusy) {
      setState(() {
        _syncingFromServer = true;
        _error = null;
      });
    }
    try {
      var restored = 0, skipped = 0, failed = 0;

      for (var i = 0; i < items.length; i++) {
        final t = items[i];
        if (!mounted) return;
        setState(() => _statusMsg =
            'Kirim ke ESP32 ${i + 1}/${items.length}: ${t.nama}...');

        var hex = t.hex.length == 512 ? t.hex : (store.cachedHex(t.employeeId) ?? '');
        // Selalu prefer hex terbaru dari server (Ummi/Sella: hex HP kosong/beda).
        try {
          final full = await api.fetchTemplate(t.employeeId);
          if (full != null && full.hex.length == 512) {
            hex = full.hex;
            await store.updateServerHex(
              employeeId: full.employeeId,
              name: full.nama.isNotEmpty ? full.nama : t.nama,
              hex: hex,
              fingerId: full.fingerId > 0 ? full.fingerId : t.fingerId,
            );
          }
        } catch (e) {
          debugPrint('[SYNC] fetch ${t.employeeId}: $e');
        }
        if (hex.length != 512) {
          failed++;
          if (mounted) {
            setState(() => _statusMsg =
                'Gagal: hex ${t.nama} tidak ada di server');
          }
          continue;
        }

        try {
          final result = await ble.putTemplate(
            employeeId: t.employeeId,
            name: t.nama.isNotEmpty ? t.nama : t.employeeId,
            templateHex: hex,
            fingerId: 0,
          );
          final st = result.status;
          final okOnDevice = (st == 'restored' || st == 'skipped_local') &&
              result.fingerId > 0;
          if (okOnDevice) {
            if (st == 'restored') {
              restored++;
            } else {
              skipped++;
            }
            await store.markOnDevice(t.employeeId, result.fingerId);
          } else if (st == 'skipped_local' || st == 'restored') {
            skipped++;
            if (result.fingerId <= 0) {
              // PUT sukses tapi slot tidak terbaca — reconcile nanti dari LIST.
            }
          } else {
            failed++;
            if (st == 'no_slot' && mounted) {
              setState(() => _error =
                  'Sensor penuh (100 slot). Hapus finger yang tidak dipakai '
                  'atau bersihkan sensor dulu, lalu coba lagi.');
            }
          }
          setState(() => _statusMsg =
              'BLE→ESP32: ${t.nama} ($st)  ok=$restored skip=$skipped gagal=$failed');
        } catch (e) {
          failed++;
          if (mounted) {
            setState(() => _statusMsg =
                'Gagal kirim ${t.nama}: ${e.toString().replaceAll('Exception: ', '')}');
          }
        }
      }

      if (!mounted) return;
      if (restored == 0 && skipped == 0 && failed > 0) {
        setState(() => _error =
            'Gagal mengirim template ke ESP32. Pastikan BLE terhubung & sensor siap.');
      } else {
        setState(() {
          _statusMsg =
              'Kirim selesai: $restored masuk sensor, $skipped sudah ada, $failed gagal.';
        });
      }
      // Jangan panggil _autoSyncEnrolls (baca hex semua finger) — berat &
      // setState berulang bikin TabBarView blank. Cukup refresh metadata.
      await _reconcileAfterDevicePush();
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted && markBusy) setState(() => _syncingFromServer = false);
    }
  }

  /// Update daftar finger di UI dari ESP32 (metadata saja, tanpa unduh hex).
  /// Hanya dipanggil manual — buka layar Sync tidak auto-request BLE.
  Future<void> _refreshEnrollListLight({bool forceRefresh = false}) async {
    final ble = context.read<BleService>();
    final store = context.read<EnrollStore>();
    if (!ble.isConnected) {
      if (mounted) {
        setState(() => _error = 'Hubungkan ESP32 dulu untuk perbarui slot');
      }
      _loadEnrollCache();
      return;
    }
    if (_loadingMeta || _loadingEnroll) return;
    setState(() {
      _loadingMeta = true;
      _error = null;
      _statusMsg = 'Membaca daftar finger dari ESP32...';
    });
    try {
      final list = await ble.readEnrollList(forceRefresh: forceRefresh);
      if (!mounted) return;
      final metaOnly = list
          .map((e) {
            final m = Map<String, dynamic>.from(e);
            m.remove('hex');
            return m;
          })
          .toList();
      final merged =
          await store.mergeFromDevice(metaOnly, metadataOnly: true);
      if (!mounted) return;
      final pending = merged.where(_enrollRowNeedsDeviceSync).length;
      setState(() {
        _setEnrolls(merged);
        _statusMsg = list.isEmpty
            ? 'Sensor kosong — $pending template di HP siap dikirim.'
            : 'Sensor: ${list.length} finger, $pending perlu dikirim dari HP.';
      });
    } catch (e) {
      debugPrint('[SYNC] refresh light: $e');
      if (mounted) {
        setState(() =>
            _error = e.toString().replaceAll('Exception: ', ''));
      }
      _loadEnrollCache();
    } finally {
      if (mounted) setState(() => _loadingMeta = false);
    }
  }

  // ── Enroll: auto-sync bertahap dari ESP32 (list + hex per id) ──
  Future<void> _autoSyncEnrolls() async {
    final ble = context.read<BleService>();
    debugPrint('[SYNC] autoSyncEnrolls called, isConnected=${ble.isConnected}');
    if (!ble.isConnected) {
      debugPrint('[SYNC] not connected, return');
      // Tidak terhubung → biarkan cache lama tampil.
      return;
    }
    if (_loadingEnroll) {
      debugPrint('[SYNC] already loading, return');
      return;
    }
    setState(() {
      _loadingEnroll = true;
      _error = null;
      _statusMsg = 'Menyinkronkan fingerprint dari ESP32...';
    });
    try {
      debugPrint('[SYNC] start syncEnrollsIncremental');
      var lastProgressUi = 0;
      final withHex = await ble.syncEnrollsIncremental(
        onProgress: (done, total) {
          if (!mounted) return;
          final now = DateTime.now().millisecondsSinceEpoch;
          if (done < total && now - lastProgressUi < 450) return;
          lastProgressUi = now;
          setState(() =>
              _statusMsg = 'Menyinkronkan $done/$total finger dari ESP32...');
        },
      );
      final withHexCount =
          withHex.where((e) => (e['hex']?.toString() ?? '').length == 512).length;
      debugPrint(
          '[SYNC] incremental done, got ${withHex.length} items, hexOk=$withHexCount');
      if (!mounted) return;
      final store = context.read<EnrollStore>();
      final merged = await store.mergeFromDevice(withHex);
      final ok =
          merged.where((e) => (e['hex']?.toString() ?? '').length == 512).length;
      setState(() {
        _setEnrolls(_uiEnrollList(merged));
        _statusMsg =
            'Sinkron selesai: $ok/${merged.length} finger punya hex siap upload.';
      });
    } catch (e) {
      debugPrint('[SYNC] error: $e');
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loadingEnroll = false);
    }
  }

  void _recomputeEnrollCounts() {
    _failedEnrollCount =
        _enrolls.where((e) => e['status']?.toString() == 'failed').length;
  }

  void _setEnrolls(List<Map<String, dynamic>> list) {
    _enrolls = list;
    _recomputeEnrollCounts();
  }

  // ── Enroll: baca cache dari storage app ──
  void _loadEnrollCache() {
    if (!mounted) return;
    final store = context.read<EnrollStore>();
    setState(() => _setEnrolls(store.toJsonListForUi()));
  }

  Future<void> _removeOneFailed(String employeeId) async {
    if (employeeId.isEmpty) return;
    final store = context.read<EnrollStore>();
    final rec = store.findByEmployee(employeeId);
    if (rec == null || rec.status != 'failed') return;
    await store.removeByEmployee(employeeId);
    _loadEnrollCache();
    if (mounted) {
      setState(() => _statusMsg = 'Enroll gagal "${rec.name}" dihapus.');
    }
  }

  Future<void> _removeFailed() async {
    final store = context.read<EnrollStore>();
    final failed = store.failedRecords;
    if (failed.isEmpty) return;
    final names = failed.map((e) => e.name).join(', ');
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Hapus enroll gagal'),
        content: Text(
          'Hapus ${failed.length} record enroll gagal dari storage app?\n\n$names\n\n'
          '(Data ini tidak ada di sensor ESP32.)',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Batal'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            style: FilledButton.styleFrom(backgroundColor: Colors.red),
            child: const Text('Hapus'),
          ),
        ],
      ),
    );
    if (ok != true || !mounted) return;
    final n = await store.removeFailed();
    _loadEnrollCache();
    setState(() => _statusMsg = '$n enroll gagal dihapus dari app.');
  }

  // ── Enroll: ambil dari ESP32 → simpan (list + hex) ke storage app ──
  Future<void> _refreshEnrolls() async {
    await _autoSyncEnrolls();
  }

  // ── Enroll: upload dari storage app → server (murni HTTP, tanpa ESP32) ──
  Future<void> _syncEnrolls() async {
    setState(() {
      _syncingEnroll = true;
      _error = null;
      _statusMsg = '';
    });
    try {
      final api = context.read<ApiService>();
      final store = context.read<EnrollStore>();
      int ok = 0, failed = 0, skipped = 0;
      for (final en in _enrolls) {
        final empId = en['employeeId']?.toString() ?? '';
        final id = (en['id'] as num?)?.toInt() ?? 0;
        final hex = store.cachedHex(empId) ?? en['hex']?.toString() ?? '';
        debugPrint('[SYNC] item id=$id emp=$empId hexLen=${hex.length}');
        if (empId.isEmpty || id == 0) {
          debugPrint('[SYNC] SKIP: empId/id kosong');
          skipped++;
          continue;
        }
        if (hex.length != 512) {
          debugPrint('[SYNC] SKIP: hex len=${hex.length} (need 512)');
          skipped++;
          continue;
        }
        try {
          // Upload langsung dari app ke server (hex sudah ada di cache app).
          debugPrint('[SYNC] upload ${en['name'] ?? empId} hexLen=${hex.length}');
          await api.uploadEnroll(
            employeeId: empId,
            fingerId: id,
            templateHex: hex,
          );
          await store.markServerSynced(empId);
          debugPrint('[SYNC] OK ${en['name'] ?? empId}');
          ok++;
        } catch (e) {
          debugPrint('[SYNC] FAIL ${en['name'] ?? empId}: $e');
          failed++;
          if (mounted) {
            setState(() => _error = 'Gagal ${en['name'] ?? empId}: '
                '${e.toString().replaceAll('Exception: ', '')}');
          }
        }
        if (mounted) {
          setState(() =>
              _statusMsg = 'Sync finger: $ok berhasil, $failed gagal, $skipped skip...');
        }
      }
      if (mounted) {
        setState(() => _statusMsg = 'Sync selesai: $ok berhasil, $failed gagal, $skipped skip.');
        _loadEnrollCache();
      }
    } finally {
      if (mounted) setState(() => _syncingEnroll = false);
    }
  }

  // ── Absensi: ambil dari ESP32 ──
  Future<void> _fetchAtt() async {
    setState(() {
      _loadingAtt = true;
      _error = null;
      _statusMsg = '';
    });
    try {
      final ble = context.read<BleService>();
      if (!ble.isConnected) {
        if (!mounted) return;
        setState(() => _error = 'Hubungkan device ESP32 dulu (Dashboard)');
        return;
      }
      final list = await ble.readLocalHistory();
      if (!mounted) return;
      setState(() => _local = list);
      final pending = list.where((e) => !e.synced).length;
      _statusMsg = pending > 0
          ? '$pending absensi belum ter-upload. Ketuk "Upload" untuk mengirim.'
          : 'Semua absensi sudah ter-upload.';
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loadingAtt = false);
    }
  }

  // ── Absensi: upload ke server ──
  Future<void> _uploadAtt() async {
    setState(() {
      _uploadingAtt = true;
      _error = null;
      _statusMsg = '';
    });
    try {
      final api = context.read<ApiService>();
      final ble = context.read<BleService>();
      final pending = _local.where((e) => !e.synced).toList();
      if (pending.isEmpty) {
        setState(() => _statusMsg = 'Tidak ada data yang perlu di-upload.');
        return;
      }

      int ok = 0, failed = 0, ignored = 0;
      final idsKept = <String>[];
      for (final rec in pending) {
        final empId = rec.idBiodata ?? '';
        if (empId.isEmpty) {
          failed++;
          continue;
        }
        final timeIso = '${rec.tanggal}T${rec.jamMasuk ?? ''}';
        try {
          await api.uploadLocalAttendance(employeeId: empId, timeIso: timeIso);
          ok++;
          idsKept.add('$empId|${rec.tanggal}|${rec.jamMasuk}');
        } catch (e) {
          final msg = e.toString();
          if (msg.contains('Absensi sudah') ||
              msg.contains('Belum waktunya') ||
              msg.contains('sudah lengkap')) {
            ignored++;
            idsKept.add('$empId|${rec.tanggal}|${rec.jamMasuk}');
          } else {
            failed++;
          }
        }
      }

      if (idsKept.isNotEmpty && ble.isConnected) {
        final payload = idsKept.join(';');
        try {
          await ble.writeCommand('MARK_SYNCED $payload');
        } catch (_) {}
      }

      if (!mounted) return;
      setState(() {
        _statusMsg = 'Upload selesai: $ok berhasil'
            '${ignored > 0 ? ', $ignored sudah absen' : ''}'
            '${failed > 0 ? ', $failed gagal' : ''}.';
      });
      await _fetchAtt();
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _uploadingAtt = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Sync Data'),
        bottom: TabBar(
          controller: _tabController,
          isScrollable: true,
          tabs: const [
            Tab(text: 'Server → HP'),
            Tab(text: 'HP → Sensor'),
            Tab(text: 'HP → Server'),
            Tab(text: 'Absensi'),
          ],
        ),
      ),
      body: TabBarView(
        controller: _tabController,
        children: [
          _serverToPhoneTab(),
          _phoneToDeviceTab(),
          _phoneToServerTab(),
          _attTab(),
        ],
      ),
    );
  }

  Widget _statusBanner() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (_statusMsg.isNotEmpty) ...[
          Text(_statusMsg,
              style: const TextStyle(color: Colors.green, fontSize: 13)),
          const SizedBox(height: 8),
        ],
        if (_error != null)
          Text(_error!,
              style: const TextStyle(color: Colors.red, fontSize: 13)),
      ],
    );
  }

  Widget _sectionCard({
    required IconData icon,
    required Color iconColor,
    required String title,
    required String description,
  }) {
    return Card(
      color: const Color(0xFFE3F2FD),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(icon, color: iconColor),
                const SizedBox(width: 8),
                Text(title,
                    style: const TextStyle(
                        fontSize: 16, fontWeight: FontWeight.bold)),
              ],
            ),
            const SizedBox(height: 8),
            Text(description,
                style: const TextStyle(fontSize: 13, color: Colors.black87)),
          ],
        ),
      ),
    );
  }

  Widget _emptyListHint(String message) {
    return SliverFillRemaining(
      hasScrollBody: false,
      child: Center(child: Text(message)),
    );
  }

  /// Tab 1: unduh template dari server ke penyimpanan HP (HTTP saja).
  Widget _serverToPhoneTab() {
    final needHex = _needsHexOnPhone;
    return CustomScrollView(
      slivers: [
        SliverPadding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
          sliver: SliverToBoxAdapter(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _sectionCard(
                  icon: Icons.cloud_download,
                  iconColor: const Color(0xFF1E88E5),
                  title: 'Server → HP',
                  description:
                      'Unduh hex sidik jari dari server PJTKI ke penyimpanan HP. '
                      'Tidak perlu Bluetooth atau ESP32.',
                ),
                const SizedBox(height: 12),
                FilledButton.icon(
                  onPressed: _busy ? null : _downloadFromServer,
                  icon: _loadingServer
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(
                            strokeWidth: 2,
                            color: Colors.white,
                          ),
                        )
                      : const Icon(Icons.download),
                  label: Text(_loadingServer
                      ? 'Mengunduh...'
                      : 'Unduh template ke HP'),
                ),
                if (_lastServerNeedDownload > 0) ...[
                  const SizedBox(height: 8),
                  Text(
                    'Terakhir: $_lastServerNeedDownload template perlu diunduh.',
                    style: TextStyle(fontSize: 12, color: Colors.grey.shade700),
                  ),
                ],
                const SizedBox(height: 12),
                _statusBanner(),
                const SizedBox(height: 12),
                Text(
                  needHex.isEmpty
                      ? 'Semua finger di app sudah punya hex di HP (${_enrolls.length})'
                      : 'Belum punya hex di HP (${needHex.length})',
                  style: const TextStyle(fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 8),
              ],
            ),
          ),
        ),
        if (needHex.isEmpty)
          _emptyListHint('Hex lengkap di HP. Buka tab HP → Sensor untuk kirim ke ESP32.')
        else
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
            sliver: SliverFixedExtentList(
              itemExtent: 56,
              delegate: SliverChildBuilderDelegate(
                (context, i) {
                  final e = needHex[i];
                  return ListTile(
                    dense: true,
                    leading: const Icon(Icons.person_outline, size: 22),
                    title: Text(
                      e['name']?.toString() ?? '-',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(fontSize: 14),
                    ),
                    subtitle: Text(
                      e['employeeId']?.toString() ?? '',
                      style: const TextStyle(fontSize: 12),
                    ),
                    trailing: const Text(
                      'Hex kurang',
                      style: TextStyle(fontSize: 11, color: Colors.orange),
                    ),
                  );
                },
                childCount: needHex.length,
                addAutomaticKeepAlives: false,
                addRepaintBoundaries: true,
              ),
            ),
          ),
      ],
    );
  }

  /// Tab 2: kirim hex dari HP ke sensor ESP32 via BLE.
  Widget _phoneToDeviceTab() {
    final pending = _pendingOnDevice;
    return CustomScrollView(
      slivers: [
        SliverPadding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
          sliver: SliverToBoxAdapter(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _sectionCard(
                  icon: Icons.bluetooth,
                  iconColor: Colors.deepPurple,
                  title: 'HP → Sensor',
                  description:
                      'Kirim template yang sudah ada di HP ke ESP32 via Bluetooth. '
                      'Hubungkan device dulu dari Dashboard.',
                ),
                const SizedBox(height: 12),
                Row(
                  children: [
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: _busy ? null : () => _refreshEnrollListLight(),
                        icon: _loadingMeta
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(strokeWidth: 2),
                              )
                            : const Icon(Icons.refresh),
                        label: Text(
                            _loadingMeta ? 'Membaca...' : 'Perbarui slot'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: _busy || pending.isEmpty
                            ? null
                            : _pushAllToDevice,
                        icon: _syncingFromServer
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(
                                  strokeWidth: 2,
                                  color: Colors.white,
                                ),
                              )
                            : const Icon(Icons.sync),
                        label: Text(_syncingFromServer
                            ? 'Mengirim...'
                            : pending.isEmpty
                                ? 'Kirim semua'
                                : 'Kirim semua (${pending.length})'),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                _statusBanner(),
                const SizedBox(height: 12),
                Text(
                  pending.isEmpty
                      ? 'Semua template sudah di sensor'
                      : 'Menunggu ke sensor (${pending.length})',
                  style: const TextStyle(fontWeight: FontWeight.bold),
                ),
                if (pending.isNotEmpty) ...[
                  const SizedBox(height: 4),
                  Text(
                    'Ganti device (3V3↔5V)? Ketuk Perbarui slot dulu.',
                    style: TextStyle(fontSize: 12, color: Colors.grey.shade700),
                  ),
                ],
                const SizedBox(height: 8),
              ],
            ),
          ),
        ),
        if (pending.isEmpty)
          _emptyListHint(
              'Tidak ada antrean. Unduh dulu di tab Server → HP jika perlu.')
        else
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
            sliver: SliverFixedExtentList(
              itemExtent: 72,
              delegate: SliverChildBuilderDelegate(
                (context, i) => _buildEnrollTile(pending[i]),
                childCount: pending.length,
                addAutomaticKeepAlives: false,
                addRepaintBoundaries: true,
              ),
            ),
          ),
      ],
    );
  }

  /// Tab 3: ambil hex dari ESP32 & upload ke server PJTKI.
  Widget _phoneToServerTab() {
    final ready = _readyUploadServer;
    return CustomScrollView(
      slivers: [
        SliverPadding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
          sliver: SliverToBoxAdapter(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _sectionCard(
                  icon: Icons.cloud_upload,
                  iconColor: Colors.green.shade700,
                  title: 'HP → Server',
                  description:
                      'Ambil hex dari ESP32 ke HP, lalu upload ke server PJTKI. '
                      'Untuk enroll baru yang dilakukan langsung di sensor.',
                ),
                const SizedBox(height: 12),
                if (_failedEnrollCount > 0)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 8),
                    child: OutlinedButton.icon(
                      onPressed: _busy ? null : _removeFailed,
                      icon: const Icon(Icons.delete_outline, color: Colors.red),
                      label: Text(
                        'Hapus enroll gagal ($_failedEnrollCount)',
                        style: const TextStyle(color: Colors.red),
                      ),
                    ),
                  ),
                Row(
                  children: [
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: _busy ? null : () => _refreshEnrollListLight(),
                        icon: _loadingMeta
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(strokeWidth: 2),
                              )
                            : const Icon(Icons.refresh),
                        label: Text(
                            _loadingMeta ? 'Membaca...' : 'Perbarui slot'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: _busy ? null : _refreshEnrolls,
                        icon: const Icon(Icons.download),
                        label: Text(
                            _loadingEnroll ? 'Mengambil...' : 'Ambil hex'),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                FilledButton.tonalIcon(
                  onPressed: _busy || ready.isEmpty ? null : _syncEnrolls,
                  icon: _syncingEnroll
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.cloud_upload),
                  label: Text(_syncingEnroll
                      ? 'Mengupload...'
                      : ready.isEmpty
                          ? 'Upload ke Server'
                          : 'Upload ke Server (${ready.length})'),
                ),
                const SizedBox(height: 12),
                _statusBanner(),
                const SizedBox(height: 12),
                Text('Semua finger di app (${_enrolls.length})',
                    style: const TextStyle(fontWeight: FontWeight.bold)),
                if (ready.isNotEmpty) ...[
                  const SizedBox(height: 4),
                  Text(
                    '${ready.length} siap upload ke server',
                    style: TextStyle(fontSize: 12, color: Colors.green.shade700),
                  ),
                ],
                const SizedBox(height: 8),
              ],
            ),
          ),
        ),
        if (_enrolls.isEmpty && !_loadingEnroll)
          _emptyListHint('Belum ada data. Ketuk "Ambil hex" setelah hubung ESP32.')
        else
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
            sliver: SliverFixedExtentList(
              itemExtent: 72,
              delegate: SliverChildBuilderDelegate(
                (context, i) => _buildEnrollTile(_enrolls[i], showSync: false),
                childCount: _enrolls.length,
                addAutomaticKeepAlives: false,
                addRepaintBoundaries: true,
              ),
            ),
          ),
      ],
    );
  }
  Widget _buildEnrollTile(Map<String, dynamic> e, {bool showSync = true}) {
    final empId = e['employeeId']?.toString() ?? '';
    return _EnrollRowItem(
      key: ValueKey(empId.isEmpty ? e.hashCode : empId),
      data: e,
      busy: _busy,
      syncing: _syncingEmpId == empId,
      showSync: showSync,
      onSync: showSync ? () => _syncOneToDevice(e) : null,
      onRemoveFailed:
          empId.isEmpty ? null : () => _removeOneFailed(empId),
    );
  }

  // ── Tab Absensi ──
  Widget _attTab() {
    final pendingCount = _local.where((e) => !e.synced).length;
    return CustomScrollView(
      slivers: [
        SliverPadding(
          padding: const EdgeInsets.all(16),
          sliver: SliverToBoxAdapter(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.icon(
                        onPressed:
                            _loadingAtt || _uploadingAtt ? null : _fetchAtt,
                        icon: const Icon(Icons.download),
                        label: Text(_loadingAtt
                            ? 'Mengambil...'
                            : 'Ambil dari ESP32'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.tonalIcon(
                        onPressed: _loadingAtt ||
                                _uploadingAtt ||
                                pendingCount == 0
                            ? null
                            : _uploadAtt,
                        icon: _uploadingAtt
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(
                                    strokeWidth: 2),
                              )
                            : const Icon(Icons.cloud_upload),
                        label: Text(_uploadingAtt
                            ? 'Mengupload...'
                            : 'Upload ke Server'),
                      ),
                    ),
                  ],
                ),
                if (_statusMsg.isNotEmpty) ...[
                  const SizedBox(height: 12),
                  Text(_statusMsg,
                      style: const TextStyle(
                          color: Colors.green, fontSize: 13)),
                ],
                if (_error != null) ...[
                  const SizedBox(height: 12),
                  Text(_error!,
                      style:
                          const TextStyle(color: Colors.red, fontSize: 13)),
                ],
                const SizedBox(height: 16),
                Text('Absensi lokal ESP32 (${_local.length})',
                    style: const TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 8),
              ],
            ),
          ),
        ),
        if (_local.isEmpty && !_loadingAtt)
          const SliverFillRemaining(
            hasScrollBody: false,
            child: Center(
              child: Text('Belum ada data. Ketuk "Ambil dari ESP32".'),
            ),
          )
        else
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
            sliver: SliverFixedExtentList(
              itemExtent: 72,
              delegate: SliverChildBuilderDelegate(
                (context, i) => _itemTile(_local[i]),
                childCount: _local.length,
                addAutomaticKeepAlives: false,
                addRepaintBoundaries: true,
              ),
            ),
          ),
      ],
    );
  }

  Widget _itemTile(AttendanceRecord e) {
    final empId = e.idBiodata ?? '-';
    final waktu = e.jamMasuk ?? '-';
    return RepaintBoundary(
      child: ListTile(
        dense: true,
        leading: Icon(
          e.synced ? Icons.cloud_done : Icons.cloud_upload,
          color: e.synced ? Colors.green : Colors.orange,
        ),
        title: Text(e.nama, style: const TextStyle(fontSize: 14)),
        subtitle: Text('$empId • ${e.tanggal} $waktu'),
        trailing: Text(
          e.synced ? 'Terkirim' : 'Pending',
          style: TextStyle(
            fontSize: 12,
            color: e.synced ? Colors.green : Colors.orange,
            fontWeight: FontWeight.bold,
          ),
        ),
      ),
    );
  }
}

bool _enrollRowHasHex(Map<String, dynamic> e) =>
    e['hasHex'] == true || (e['hex']?.toString() ?? '').length == 512;

bool _enrollRowNeedsDeviceSync(Map<String, dynamic> e) {
  final status = e['status']?.toString() ?? '';
  if (status == 'failed' || status == 'pending' || status == 'enrolling') {
    return false;
  }
  if (status == 'enrolled') {
    final fingerId = (e['id'] as num?)?.toInt() ?? 0;
    return fingerId <= 0;
  }
  if (status == 'server_cached') return true;
  if (!_enrollRowHasHex(e)) return true;
  final fingerId = (e['id'] as num?)?.toInt() ?? 0;
  return fingerId <= 0;
}

/// Baris finger — widget terpisah supaya scroll tidak rebuild seluruh layar.
class _EnrollRowItem extends StatelessWidget {
  final Map<String, dynamic> data;
  final bool busy;
  final bool syncing;
  final bool showSync;
  final VoidCallback? onSync;
  final VoidCallback? onRemoveFailed;

  const _EnrollRowItem({
    super.key,
    required this.data,
    required this.busy,
    required this.syncing,
    this.showSync = true,
    this.onSync,
    this.onRemoveFailed,
  });

  @override
  Widget build(BuildContext context) {
    final status = data['status']?.toString() ?? 'enrolled';
    final synced = data['serverSynced'] == true;
    final hasHex = _enrollRowHasHex(data);
    final needsDevice = _enrollRowNeedsDeviceSync(data);
    final empId = data['employeeId']?.toString() ?? '';
    final fingerId = (data['id'] as num?)?.toInt() ?? 0;

    String trailing;
    Color trailingColor;
    if (status == 'failed') {
      trailing = 'Gagal';
      trailingColor = Colors.red;
    } else if (synced) {
      trailing = 'Tersinkron';
      trailingColor = Colors.green;
    } else if (status == 'pending' || status == 'enrolling') {
      trailing = 'Menunggu device';
      trailingColor = Colors.orange;
    } else if (status == 'server_cached') {
      trailing = 'Di HP';
      trailingColor = Colors.deepPurple;
    } else if (!hasHex) {
      trailing = 'Hex kurang';
      trailingColor = Colors.orange;
    } else {
      trailing = 'Siap upload';
      trailingColor = Colors.orange;
    }

    final subtitleParts = <String>[
      if (fingerId > 0) 'slot $fingerId',
      empId,
      if (needsDevice) 'belum ke sensor',
    ];

    return RepaintBoundary(
      child: ListTile(
        dense: true,
        visualDensity: VisualDensity.compact,
        leading: Icon(
          status == 'failed'
              ? Icons.error_outline
              : synced
                  ? Icons.cloud_done
                  : (status == 'pending' || status == 'enrolling'
                      ? Icons.phone_android
                      : needsDevice
                          ? Icons.phone_android_outlined
                          : Icons.fingerprint),
          color: status == 'failed'
              ? Colors.red
              : synced
                  ? Colors.green
                  : needsDevice
                      ? Colors.deepPurple
                      : const Color(0xFF1E88E5),
          size: 22,
        ),
        title: Text(
          data['name']?.toString() ?? '-',
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontSize: 14),
        ),
        subtitle: Text(
          subtitleParts.join(' • '),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontSize: 12),
        ),
        trailing: status == 'failed'
            ? IconButton(
                tooltip: 'Hapus',
                icon: const Icon(Icons.delete_outline,
                    color: Colors.red, size: 20),
                onPressed: busy ? null : onRemoveFailed,
              )
            : needsDevice && showSync
                ? syncing
                    ? const SizedBox(
                        width: 22,
                        height: 22,
                        child:
                            CircularProgressIndicator(strokeWidth: 2),
                      )
                    : IconButton(
                        tooltip: 'Sync ke sensor',
                        icon: const Icon(Icons.sync, size: 22),
                        color: Colors.deepPurple,
                        onPressed: busy ? null : onSync,
                      )
                : Text(
                    trailing,
                    style: TextStyle(fontSize: 11, color: trailingColor),
                  ),
      ),
    );
  }
}
