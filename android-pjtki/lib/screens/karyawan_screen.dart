import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/ble_service.dart';
import '../services/enroll_store.dart';
import '../services/settings_store.dart';
import '../theme/app_theme.dart';
import 'enroll_screen.dart';

/// Finger gagal di tab CPMI: orphan sensor (tanpa nama) atau enroll gagal di app.
class _FailedFinger {
  final int fingerId;
  final String employeeId;
  final String name;
  final bool fromDevice;

  const _FailedFinger({
    required this.fingerId,
    required this.employeeId,
    required this.name,
    required this.fromDevice,
  });

  String get key =>
      fingerId > 0 ? 'id:$fingerId' : 'emp:$employeeId';

  String get displayName {
    if (name.isNotEmpty) return name;
    if (employeeId.isNotEmpty) return employeeId;
    return 'Tanpa nama';
  }
}

class KaryawanScreen extends StatefulWidget {
  const KaryawanScreen({super.key});

  @override
  State<KaryawanScreen> createState() => _KaryawanScreenState();
}

class _KaryawanScreenState extends State<KaryawanScreen>
    with SingleTickerProviderStateMixin {
  List<Employee> _employees = [];
  List<Branch> _branches = [];
  /// employeeId yang sudah ada di ESP32 (local).
  final Set<String> _localIds = {};
  /// finger id di ESP32 per employeeId (untuk hapus).
  final Map<String, int> _localFingerIds = {};
  /// Template di sensor tanpa nama / employeeId.
  List<_FailedFinger> _orphanFingers = [];

  bool _loading = false;
  String? _error;
  String? _deleting;
  String _q = '';
  late final TabController _tabs;
  StreamSubscription? _connSub;

  @override
  void initState() {
    super.initState();
    _tabs = TabController(length: 3, vsync: this);
    _tabs.addListener(() {
      if (mounted) setState(() {});
    });
    _loadLocalFromCache();
    _load();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final ble = context.read<BleService>();
      if (ble.isConnected) _refreshLocalFromBle();
      _connSub = ble.connectionStream.listen((ok) {
        if (ok && mounted) _refreshLocalFromBle();
      });
    });
  }

  @override
  void dispose() {
    _connSub?.cancel();
    _tabs.dispose();
    super.dispose();
  }

  void _loadLocalFromCache() {
    _applyLocalList(context.read<EnrollStore>().toJsonList());
  }

  void _applyLocalList(List<Map<String, dynamic>> list) {
    _localIds.clear();
    _localFingerIds.clear();
    for (final e in list) {
      final empId = e['employeeId']?.toString() ?? '';
      if (empId.isEmpty) continue;
      final status = e['status']?.toString() ?? 'enrolled';
      // Tampilkan sebagai local jika sudah enrolled di device ATAU masih pending/enrolling.
      if (status == 'failed') continue;
      _localIds.add(empId);
      final id = (e['id'] as num?)?.toInt() ?? 0;
      if (id > 0) _localFingerIds[empId] = id;
    }
  }

  bool _isLocal(Employee e) => _localIds.contains(e.id);
  bool _isServer(Employee e) => e.fingerTerdaftar;

  void _applyOrphans(List<Map<String, dynamic>> list) {
    final orphans = <_FailedFinger>[];
    for (final e in list) {
      final id = (e['id'] as num?)?.toInt() ?? 0;
      final empId = e['employeeId']?.toString() ?? '';
      final name = e['name']?.toString() ?? '';
      if (id > 0 && empId.isEmpty && name.isEmpty) {
        orphans.add(_FailedFinger(
          fingerId: id,
          employeeId: '',
          name: '',
          fromDevice: true,
        ));
      }
    }
    _orphanFingers = orphans;
  }

  Future<void> _refreshLocalFromBle() async {
    final ble = context.read<BleService>();
    if (!ble.isConnected) return;
    try {
      final list = await ble.readEnrollList();
      if (!mounted) return;
      final store = context.read<EnrollStore>();
      final merged = await store.mergeFromDevice(list);
      setState(() {
        _applyOrphans(list);
        _applyLocalList(merged);
      });
    } catch (_) {
      // Cache lokal tetap dipakai.
    }
  }

  List<_FailedFinger> _failedItems() {
    final store = context.read<EnrollStore>();
    final items = <_FailedFinger>[
      ..._orphanFingers,
      ...store.failedRecords.map(
        (r) => _FailedFinger(
          fingerId: r.fingerId,
          employeeId: r.employeeId,
          name: r.name,
          fromDevice: false,
        ),
      ),
    ];
    if (_q.isEmpty) return items;
    final q = _q.toLowerCase();
    return items.where((f) {
      return f.displayName.toLowerCase().contains(q) ||
          f.employeeId.toLowerCase().contains(q) ||
          (f.fingerId > 0 && '${f.fingerId}'.contains(q));
    }).toList();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final api = context.read<ApiService>();
      final settings = context.read<SettingsStore>();
      _loadLocalFromCache();
      List<Branch> branches = [];
      try {
        branches = await api.fetchBranches();
      } catch (_) {}
      final employees = await api.fetchEmployees(
        kodeCabang: settings.kodeCabang.isEmpty ? null : settings.kodeCabang,
      );
      if (!mounted) return;
      setState(() {
        _branches = branches;
        _employees = employees;
      });
      // Refresh daftar local ESP32 di background.
      unawaited(_refreshLocalFromBle());
    } catch (e) {
      if (mounted) setState(() => _error = e.toString());
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _selectBranch() async {
    final settings = context.read<SettingsStore>();
    final selected = await showModalBottomSheet<Branch>(
      context: context,
      builder: (ctx) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const Padding(
              padding: EdgeInsets.all(16),
              child: Text('Pilih Cabang',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            for (final b in _branches)
              ListTile(
                title: Text(b.label),
                onTap: () => Navigator.pop(ctx, b),
              ),
          ],
        ),
      ),
    );
    if (selected != null) {
      settings.kodeCabang = selected.kode;
      await _load();
    }
  }

  Future<void> _enroll(Employee emp) async {
    final store = context.read<EnrollStore>();
    final existing = store.findByEmployee(emp.id);
    if (existing != null && existing.isEnrolledOnDevice) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
            content: Text('CPMI ini sudah terdaftar di device (local)')),
      );
      _tabs.animateTo(1);
      return;
    }
    final ble = context.read<BleService>();
    if (!ble.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
            content: Text('Hubungkan device ESP32 dulu (Dashboard)')),
      );
      return;
    }
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => EnrollScreen(employee: emp)),
    );
    if (!mounted) return;
    if (ok == true) {
      setState(() => _localIds.add(emp.id));
      _tabs.animateTo(1);
    }
    await _load();
  }

  Future<void> _delete(Employee emp) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Hapus sidik jari'),
        content: Text(
            'Hapus registrasi sidik jari ${emp.nama} (${emp.id})?\n'
            'Sidik jari dihapus dari sensor ESP32 dan server.'),
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
    if (confirmed != true || !mounted) return;

    final ble = context.read<BleService>();
    final api = context.read<ApiService>();
    final enrollStore = context.read<EnrollStore>();
    setState(() => _deleting = emp.id);
    try {
      if (ble.isConnected) {
        await ble.deleteFingerByEmployee(emp.id);
        await Future<void>.delayed(const Duration(milliseconds: 800));
      }
      try {
        await api.unregisterFinger(emp.id);
      } catch (_) {}
      if (!mounted) return;
      await enrollStore.removeByEmployee(emp.id);
      setState(() => _applyLocalList(enrollStore.toJsonList()));
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Sidik jari dihapus')),
      );
      await _load();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
              content: Text(
                  'Gagal hapus: ${e.toString().replaceAll('Exception: ', '')}')),
        );
      }
    } finally {
      if (mounted) setState(() => _deleting = null);
    }
  }

  Future<void> _deleteFailed(_FailedFinger item) async {
    final label = item.fingerId > 0
        ? 'finger ID ${item.fingerId} (${item.displayName})'
        : item.displayName;
    final from = item.fromDevice ? 'dari sensor ESP32' : 'dari storage app';
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Hapus finger gagal'),
        content: Text('Hapus $label $from?'),
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
    if (confirmed != true || !mounted) return;
    try {
      await _deleteFailedItem(item);
    } catch (_) {
      return;
    }
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('$label dihapus')),
    );
  }

  Future<void> _deleteFailedItem(_FailedFinger item) async {
    final ble = context.read<BleService>();
    final enrollStore = context.read<EnrollStore>();
    setState(() => _deleting = item.key);
    try {
      if (item.fingerId > 0) {
        if (!ble.isConnected) {
          throw Exception('Hubungkan device ESP32 dulu (Dashboard)');
        }
        await ble.deleteFinger(item.fingerId);
        await Future<void>.delayed(const Duration(milliseconds: 800));
      }
      if (item.employeeId.isNotEmpty) {
        await enrollStore.removeByEmployee(item.employeeId);
      }
      if (item.fromDevice) {
        _orphanFingers =
            _orphanFingers.where((e) => e.fingerId != item.fingerId).toList();
      }
      if (!mounted) return;
      setState(() => _applyLocalList(enrollStore.toJsonList()));
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
              content: Text(
                  'Gagal hapus: ${e.toString().replaceAll('Exception: ', '')}')),
        );
      }
      rethrow;
    } finally {
      if (mounted) setState(() => _deleting = null);
    }
  }

  Future<void> _deleteAllFailed() async {
    final items = _failedItems();
    if (items.isEmpty) return;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Hapus semua finger gagal'),
        content: Text(
          'Hapus ${items.length} finger gagal dari sensor/app?\n'
          'Finger tanpa nama dihapus dari ESP32. Enroll gagal dihapus dari storage app.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Batal'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            style: FilledButton.styleFrom(backgroundColor: Colors.red),
            child: const Text('Hapus semua'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;

    int ok = 0;
    for (final item in List<_FailedFinger>.from(items)) {
      if (!mounted) return;
      try {
        await _deleteFailedItem(item);
        ok++;
      } catch (_) {}
    }
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('$ok finger gagal dihapus')),
    );
    await _refreshLocalFromBle();
  }

  List<Employee> _filtered({required bool localOnly}) {
    return _employees.where((e) {
      final isLocal = _isLocal(e);
      if (localOnly && !isLocal) return false;
      if (!localOnly && isLocal) return false;
      if (_q.isNotEmpty &&
          !e.nama.toLowerCase().contains(_q.toLowerCase()) &&
          !e.id.toLowerCase().contains(_q.toLowerCase())) {
        return false;
      }
      return true;
    }).toList();
  }

  Widget _statusChips(Employee e, {required bool showLocal}) {
    final chips = <Widget>[];
    final localRec = context.read<EnrollStore>().findByEmployee(e.id);
    if (showLocal && _isLocal(e)) {
      if (localRec != null &&
          (localRec.status == 'pending' || localRec.status == 'enrolling')) {
        chips.add(_chip('Pending', AppTheme.warning));
      } else if (localRec?.status == 'failed') {
        chips.add(_chip('Gagal', AppTheme.danger));
      } else {
        chips.add(_chip('Di device', Theme.of(context).colorScheme.primary));
      }
    }
    if (_isServer(e)) {
      chips.add(_chip('Server', AppTheme.success));
    }
    if (chips.isEmpty) return const SizedBox.shrink();
    return Wrap(spacing: 6, runSpacing: 4, children: chips);
  }

  Widget _chip(String label, Color color) {
    return StatusBadge(label: label, color: color);
  }

  Widget _listBody({required bool localOnly}) {
    if (_loading) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_error != null) {
      return AppEmptyState(
        icon: Icons.error_outline,
        title: 'Gagal memuat data',
        subtitle: _error,
        actionLabel: 'Coba lagi',
        onAction: _load,
      );
    }
    final items = _filtered(localOnly: localOnly);
    if (items.isEmpty) {
      return AppEmptyState(
        icon: localOnly ? Icons.fingerprint : Icons.person_search_outlined,
        title: localOnly
            ? 'Belum ada CPMI di device'
            : 'Tidak ada yang perlu enroll',
        subtitle: localOnly
            ? 'Daftarkan sidik jari dari tab Belum.'
            : 'Semua CPMI cabang ini sudah di device, atau daftar server kosong.',
      );
    }
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 20),
      itemCount: items.length,
      separatorBuilder: (_, _) => const SizedBox(height: 8),
      itemBuilder: (context, i) {
        final e = items[i];
        final local = _isLocal(e);
        final server = _isServer(e);
        return Card(
          child: ListTile(
            contentPadding:
                const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
            leading: PersonAvatar(
              name: e.nama,
              color: local
                  ? Theme.of(context).colorScheme.primary
                  : (server ? AppTheme.success : null),
            ),
            title: Text(e.nama,
                style: const TextStyle(fontWeight: FontWeight.w700)),
            subtitle: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const SizedBox(height: 2),
                Text(e.id),
                const SizedBox(height: 6),
                _statusChips(e, showLocal: localOnly || local),
                if (!localOnly && server && !local)
                  const Padding(
                    padding: EdgeInsets.only(top: 4),
                    child: Text(
                      'Ada di server, belum di device ini',
                      style: TextStyle(fontSize: 12, color: AppTheme.warning),
                    ),
                  ),
              ],
            ),
            isThreeLine: true,
            trailing: local
                ? (_deleting == e.id
                    ? const SizedBox(
                        width: 20,
                        height: 20,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : IconButton(
                        tooltip: 'Hapus sidik jari',
                        icon: const Icon(Icons.delete_outline,
                            color: AppTheme.danger),
                        onPressed: () => _delete(e),
                      ))
                : FilledButton.tonal(
                    onPressed: () => _enroll(e),
                    style: FilledButton.styleFrom(
                      minimumSize: const Size(0, 40),
                      padding: const EdgeInsets.symmetric(horizontal: 14),
                    ),
                    child: const Text('Enroll'),
                  ),
            onTap: local ? null : () => _enroll(e),
          ),
        );
      },
    );
  }

  Widget _gagalBody() {
    if (_loading) {
      return const Center(child: CircularProgressIndicator());
    }
    final items = _failedItems();
    if (items.isEmpty) {
      return const AppEmptyState(
        icon: Icons.verified_outlined,
        title: 'Tidak ada finger gagal',
        subtitle: 'Enroll yang bermasalah atau tanpa nama akan tampil di sini.',
      );
    }
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 20),
      itemCount: items.length,
      separatorBuilder: (_, _) => const SizedBox(height: 8),
      itemBuilder: (context, i) {
        final f = items[i];
        final source = f.fromDevice ? 'Tanpa nama di sensor' : 'Enroll gagal';
        final details = <String>[
          if (f.fingerId > 0) 'ID ${f.fingerId}',
          if (f.employeeId.isNotEmpty) f.employeeId,
          source,
        ];
        return Card(
          child: ListTile(
            leading: PersonAvatar(name: f.displayName, color: AppTheme.danger),
            title: Text(f.displayName,
                style: const TextStyle(fontWeight: FontWeight.w700)),
            subtitle: Text(details.join(' • ')),
            trailing: _deleting == f.key
                ? const SizedBox(
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : IconButton(
                    tooltip: 'Hapus',
                    icon:
                        const Icon(Icons.delete_outline, color: AppTheme.danger),
                    onPressed: () => _deleteFailed(f),
                  ),
          ),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final belumCount = _filtered(localOnly: false).length;
    final terdaftarCount = _filtered(localOnly: true).length;
    final gagalCount = _failedItems().length;
    final onGagalTab = _tabs.index == 2;

    return Scaffold(
      appBar: AppBar(
        title: const Text('CPMI'),
        actions: [
          if (onGagalTab && gagalCount > 0)
            IconButton(
              icon: const Icon(Icons.delete_sweep, color: Colors.red),
              tooltip: 'Hapus semua finger gagal',
              onPressed: _deleting != null ? null : _deleteAllFailed,
            ),
          IconButton(
            icon: const Icon(Icons.business),
            tooltip: 'Pilih Cabang',
            onPressed: _branches.isEmpty ? null : _selectBranch,
          ),
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Muat Ulang',
            onPressed: _loading
                ? null
                : () async {
                    await _load();
                    await _refreshLocalFromBle();
                  },
          ),
        ],
        bottom: TabBar(
          controller: _tabs,
          tabs: [
            Tab(text: 'Belum  $belumCount'),
            Tab(text: 'Device  $terdaftarCount'),
            Tab(text: 'Gagal  $gagalCount'),
          ],
        ),
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
            child: TextField(
              onChanged: (v) => setState(() => _q = v),
              decoration: InputDecoration(
                hintText: onGagalTab
                    ? 'Cari nama, ID finger, atau employee'
                    : 'Cari nama atau ID CPMI',
                prefixIcon: const Icon(Icons.search),
              ),
            ),
          ),
          Expanded(
            child: TabBarView(
              controller: _tabs,
              children: [
                _listBody(localOnly: false),
                _listBody(localOnly: true),
                _gagalBody(),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
