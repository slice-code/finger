import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';

class DeviceScanScreen extends StatefulWidget {
  const DeviceScanScreen({super.key});

  @override
  State<DeviceScanScreen> createState() => _DeviceScanScreenState();
}

class _DeviceScanScreenState extends State<DeviceScanScreen> {
  List<BleDeviceInfo> _devices = [];
  bool _scanning = false;
  String? _error;

  Future<void> _scan() async {
    setState(() {
      _scanning = true;
      _error = null;
    });
    try {
      final ble = context.read<BleService>();
      final devices = await ble.scan(timeoutSec: 8);
      if (!mounted) return;
      setState(() => _devices = devices);
      if (devices.isEmpty) {
        _error = 'Tidak ada device PJTKI-Finger ditemukan.\n'
            'Pastikan ESP32 menyala & sensor aktif.';
      }
    } catch (e) {
      if (mounted) setState(() => _error = e.toString());
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  Future<void> _connect(BleDeviceInfo info) async {
    if (_scanning) return;
    setState(() {
      _scanning = true;
      _error = null;
    });
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    try {
      final ble = context.read<BleService>();
      await ble.connect(info);
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } catch (e) {
      if (mounted) {
        setState(() {
          _error = 'Gagal konek: ${e.toString().replaceAll('Exception: ', '')}';
        });
      }
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _scan());
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Cari Device ESP32')),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Row(
              children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: _scanning ? null : _scan,
                    icon: _scanning
                        ? const SizedBox(
                            width: 18,
                            height: 18,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.search),
                    label: Text(_scanning ? 'Mencari...' : 'Scan Ulang'),
                  ),
                ),
              ],
            ),
          ),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Text(
                _error!,
                textAlign: TextAlign.center,
                style: const TextStyle(color: Colors.red),
              ),
            ),
          const SizedBox(height: 8),
          Expanded(
            child: _devices.isEmpty
                ? const Center(child: Text('Belum ada device ditemukan'))
                : ListView.separated(
                    itemCount: _devices.length,
                    separatorBuilder: (_, _) => const Divider(height: 1),
                    itemBuilder: (context, i) {
                      final d = _devices[i];
                      return ListTile(
                        leading: const Icon(Icons.devices, color: Color(0xFF1E88E5)),
                        title: Text(d.name),
                        subtitle: Text(_scanning
                            ? 'Menghubungkan...'
                            : (d.rssi == 0
                                ? 'Device ditemukan'
                                : 'Kekuatan sinyal: ${d.rssi} dBm')),
                        trailing: _scanning
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(strokeWidth: 2),
                              )
                            : const Icon(Icons.chevron_right),
                        onTap: _scanning ? null : () => _connect(d),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
