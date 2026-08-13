import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/ble_service.dart';
import '../services/settings_store.dart';
import 'enroll_screen.dart';

class KaryawanScreen extends StatefulWidget {
  const KaryawanScreen({super.key});

  @override
  State<KaryawanScreen> createState() => _KaryawanScreenState();
}

class _KaryawanScreenState extends State<KaryawanScreen> {
  List<Employee> _employees = [];
  List<Branch> _branches = [];
  bool _loading = false;
  String? _error;
  String _filter = 'semua';
  String _q = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final api = context.read<ApiService>();
      final settings = context.read<SettingsStore>();
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
    if (emp.fingerTerdaftar) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Karyawan ini sudah terdaftar sidik jari')),
      );
      return;
    }
    final ble = context.read<BleService>();
    if (!ble.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Hubungkan device ESP32 dulu (Dashboard)')),
      );
      return;
    }
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => EnrollScreen(employee: emp)),
    );
    if (ok == true) _load();
  }

  @override
  Widget build(BuildContext context) {
    final filtered = _employees.where((e) {
      if (_filter == 'terdaftar' && !e.fingerTerdaftar) return false;
      if (_filter == 'belum' && e.fingerTerdaftar) return false;
      if (_q.isNotEmpty &&
          !e.nama.toLowerCase().contains(_q.toLowerCase()) &&
          !e.id.toLowerCase().contains(_q.toLowerCase())) {
        return false;
      }
      return true;
    }).toList();

    return Scaffold(
      appBar: AppBar(
        title: const Text('Karyawan'),
        actions: [
          IconButton(
            icon: const Icon(Icons.business),
            tooltip: 'Pilih Cabang',
            onPressed: _branches.isEmpty ? null : _selectBranch,
          ),
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Muat Ulang',
            onPressed: _loading ? null : _load,
          ),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
            child: TextField(
              onChanged: (v) => setState(() => _q = v),
              decoration: const InputDecoration(
                hintText: 'Cari nama / ID',
                prefixIcon: Icon(Icons.search),
                border: OutlineInputBorder(),
                isDense: true,
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(
              children: [
                ChoiceChip(
                  label: const Text('Semua'),
                  selected: _filter == 'semua',
                  onSelected: (_) => setState(() => _filter = 'semua'),
                ),
                const SizedBox(width: 8),
                ChoiceChip(
                  label: const Text('Terdaftar'),
                  selected: _filter == 'terdaftar',
                  onSelected: (_) => setState(() => _filter = 'terdaftar'),
                ),
                const SizedBox(width: 8),
                ChoiceChip(
                  label: const Text('Belum'),
                  selected: _filter == 'belum',
                  onSelected: (_) => setState(() => _filter = 'belum'),
                ),
              ],
            ),
          ),
          const SizedBox(height: 8),
          Expanded(
            child: _loading
                ? const Center(child: CircularProgressIndicator())
                : _error != null
                    ? Center(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            const Icon(Icons.error_outline,
                                size: 48, color: Colors.red),
                            const SizedBox(height: 8),
                            Text(_error!,
                                textAlign: TextAlign.center,
                                style: const TextStyle(color: Colors.red)),
                            const SizedBox(height: 8),
                            TextButton(onPressed: _load, child: const Text('Coba lagi')),
                          ],
                        ),
                      )
                    : filtered.isEmpty
                        ? const Center(child: Text('Tidak ada data'))
                        : ListView.separated(
                            itemCount: filtered.length,
                            separatorBuilder: (_, _) => const Divider(height: 1),
                            itemBuilder: (context, i) {
                              final e = filtered[i];
                              return ListTile(
                                enabled: !e.fingerTerdaftar,
                                leading: CircleAvatar(
                                  backgroundColor: e.fingerTerdaftar
                                      ? Colors.green.shade100
                                      : Colors.grey.shade200,
                                  child: Icon(
                                    Icons.person,
                                    color: e.fingerTerdaftar
                                        ? Colors.green.shade800
                                        : Colors.grey,
                                  ),
                                ),
                                title: Text(e.nama),
                                subtitle: Text(e.id),
                                trailing: e.fingerTerdaftar
                                    ? const Chip(
                                        label: Text('TERDAFTAR'),
                                        backgroundColor: Colors.green,
                                        labelStyle: TextStyle(
                                            color: Colors.white, fontSize: 11),
                                        visualDensity: VisualDensity.compact,
                                      )
                                    : FilledButton.tonal(
                                        onPressed: () => _enroll(e),
                                        child: const Text('Enroll'),
                                      ),
                                onTap: e.fingerTerdaftar ? null : () => _enroll(e),
                              );
                            },
                          ),
          ),
        ],
      ),
    );
  }
}
