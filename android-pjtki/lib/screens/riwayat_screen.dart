import 'package:flutter/material.dart';
import 'package:intl/intl.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/ble_service.dart';

class RiwayatScreen extends StatefulWidget {
  const RiwayatScreen({super.key});

  @override
  State<RiwayatScreen> createState() => _RiwayatScreenState();
}

class _RiwayatScreenState extends State<RiwayatScreen> {
  final _pageController = ScrollController();
  final List<AttendanceRecord> _localRows = [];
  List<AttendanceRecord> _rows = [];
  bool _loading = false;
  bool _hasMore = true;
  int _page = 1;
  String? _error;
  String _status = '';
  bool _localLoaded = false;
  DateTime? _tanggal;

  @override
  void initState() {
    super.initState();
    _pageController.addListener(() {
      if (_pageController.position.pixels >=
          _pageController.position.maxScrollExtent - 200) {
        _loadMore();
      }
    });
    _reload();
  }

  Future<void> _reload() async {
    setState(() {
      _page = 1;
      _rows = [];
      _hasMore = true;
      _error = null;
      _localLoaded = false;
    });
    await _loadLocal();
    await _loadMore();
  }

  /// Baca riwayat absensi lokal dari ESP32 via BLE (char 4fafc208).
  Future<void> _loadLocal() async {
    final ble = context.read<BleService>();
    if (!ble.isConnected) {
      _localRows.clear();
      return;
    }
    final local = await ble.readLocalHistory();
    if (!mounted) return;
    setState(() {
      _localRows
        ..clear()
        ..addAll(local);
      _localLoaded = true;
    });
  }

  List<AttendanceRecord> get _mergedRows {
    // Prioritas: catatan lokal yang belum ter-upload (synced=false) di atas.
    final pendingLocal = _localRows.where((r) => !r.synced).toList();
    final merged = <AttendanceRecord>[...pendingLocal, ..._rows];
    final seen = <String>{};
    final out = <AttendanceRecord>[];
    for (final r in merged) {
      final key = '${r.idBiodata}|${r.tanggal}|${r.jamMasuk}';
      if (r.synced && seen.contains(key)) continue; // duplikat dari server
      if (!r.synced) seen.add(key);
      out.add(r);
    }
    return out;
  }

  Future<void> _loadMore() async {
    if (_loading || !_hasMore) return;
    setState(() => _loading = true);
    try {
      final api = context.read<ApiService>();
      final result = await api.fetchAttendance(
        page: _page,
        status: _status,
        tanggal: _tanggal == null
            ? null
            : DateFormat('yyyy-MM-dd').format(_tanggal!),
      );
      if (!mounted) return;
      setState(() {
        _rows = [..._rows, ...result.rows];
        _hasMore = result.rows.isNotEmpty;
        _page++;
      });
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _pickDate() async {
    final picked = await showDatePicker(
      context: context,
      initialDate: _tanggal ?? DateTime.now(),
      firstDate: DateTime(2024),
      lastDate: DateTime.now(),
    );
    if (picked != null) {
      setState(() => _tanggal = picked);
      await _reload();
    }
  }

  Color _statusColor(String s) {
    switch (s) {
      case 'hadir':
        return Colors.green;
      case 'terlambat':
        return Colors.orange;
      case 'izin':
        return Colors.blue;
      case 'alpha':
        return Colors.red;
      case 'pulang_cepat':
        return Colors.deepOrange;
      case 'pending':
        return Colors.purple;
      default:
        return Colors.grey;
    }
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();
    final merged = _mergedRows;
    final pendingCount = _localRows.where((r) => !r.synced).length;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Riwayat Absensi'),
        actions: [
          if (pendingCount > 0)
            Center(
              child: Padding(
                padding: const EdgeInsets.only(right: 4),
                child: Chip(
                  label: Text('$pendingCount pending sync'),
                  backgroundColor: Colors.purple,
                  labelStyle: const TextStyle(color: Colors.white, fontSize: 10),
                  visualDensity: VisualDensity.compact,
                ),
              ),
            ),
          IconButton(
            icon: const Icon(Icons.sync),
            tooltip: 'Sync ke server',
            onPressed: ble.isConnected
                ? () async {
                    final messenger = ScaffoldMessenger.of(context);
                    await ble.syncNow();
                    messenger.showSnackBar(
                      const SnackBar(content: Text('Sync diminta ke device')),
                    );
                    await Future<void>.delayed(const Duration(milliseconds: 2500));
                    if (mounted) await _loadLocal();
                  }
                : null,
          ),
          IconButton(
            icon: const Icon(Icons.date_range),
            tooltip: _tanggal == null
                ? 'Filter Tanggal'
                : DateFormat('yyyy-MM-dd').format(_tanggal!),
            onPressed: _pickDate,
          ),
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _loading ? null : _reload,
          ),
        ],
      ),
      body: SafeArea(
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 0),
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: Row(
                  children: [
                    for (final s in const [
                      ('', 'Semua'),
                      ('hadir', 'Hadir'),
                      ('terlambat', 'Terlambat'),
                      ('izin', 'Izin'),
                      ('alpha', 'Alpha'),
                      ('pulang_cepat', 'Pulang Cepat'),
                    ])
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 4),
                        child: ChoiceChip(
                          label: Text(s.$2),
                          selected: _status == s.$1,
                          onSelected: (_) async {
                            setState(() => _status = s.$1);
                            await _reload();
                          },
                        ),
                      ),
                  ],
                ),
              ),
            ),
            if (_localLoaded && _localRows.isEmpty && _rows.isEmpty && _error == null)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 8),
                child: Text(
                  'Riwayat lokal kosong — sync dari server di bawah',
                  style: TextStyle(color: Colors.grey, fontSize: 12),
                ),
              ),
            Expanded(
              child: merged.isEmpty && _loading
                  ? const Center(child: CircularProgressIndicator())
                  : _error != null && merged.isEmpty
                      ? Center(
                          child: Padding(
                            padding: const EdgeInsets.all(24),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Text(_error!,
                                    textAlign: TextAlign.center,
                                    style: const TextStyle(color: Colors.red)),
                                const SizedBox(height: 8),
                                TextButton(
                                  onPressed: _reload,
                                  child: const Text('Coba lagi'),
                                ),
                              ],
                            ),
                          ),
                        )
                      : merged.isEmpty
                          ? const Center(child: Text('Belum ada data absensi'))
                          : RefreshIndicator(
                              onRefresh: _reload,
                              child: ListView.separated(
                                controller: _pageController,
                                itemCount: merged.length + (_hasMore ? 1 : 0),
                                separatorBuilder: (_, _) => const Divider(height: 1),
                                itemBuilder: (context, i) {
                                  if (i >= merged.length) {
                                    return const Padding(
                                      padding: EdgeInsets.all(16),
                                      child: Center(
                                        child: CircularProgressIndicator(),
                                      ),
                                    );
                                  }
                                  final r = merged[i];
                                  final isLocal = !r.synced;
                                  return ListTile(
                                    leading: CircleAvatar(
                                      backgroundColor: _statusColor(r.status)
                                          .withValues(alpha: 0.15),
                                      child: Icon(
                                        isLocal
                                            ? Icons.bluetooth
                                            : Icons.badge_outlined,
                                        color: _statusColor(r.status),
                                        size: 20,
                                      ),
                                    ),
                                    title: Text(
                                      r.nama,
                                      maxLines: 1,
                                      overflow: TextOverflow.ellipsis,
                                    ),
                                    subtitle: Text([
                                      if (r.idBiodata != null) r.idBiodata!,
                                      if (r.tanggal.isNotEmpty) r.tanggal,
                                      if (isLocal) 'lokal (belum sync)',
                                    ].join(' • ')),
                                    trailing: Column(
                                      mainAxisAlignment: MainAxisAlignment.center,
                                      crossAxisAlignment: CrossAxisAlignment.end,
                                      children: [
                                        Chip(
                                          label: Text(r.status.toUpperCase()),
                                          backgroundColor: _statusColor(r.status),
                                          labelStyle: const TextStyle(
                                            color: Colors.white,
                                            fontSize: 10,
                                          ),
                                          visualDensity: VisualDensity.compact,
                                        ),
                                        const SizedBox(height: 2),
                                        Text(
                                          'M: ${r.jamMasuk ?? "--"}  P: ${r.jamPulang ?? "--"}',
                                          style: const TextStyle(
                                            fontSize: 11,
                                            color: Colors.grey,
                                          ),
                                        ),
                                      ],
                                    ),
                                  );
                                },
                              ),
                            ),
            ),
          ],
        ),
      ),
    );
  }
}
