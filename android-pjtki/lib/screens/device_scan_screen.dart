import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';
import '../theme/app_theme.dart';

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
        _error = 'Tidak ada PJTKI-Finger di sekitar.\n'
            'Pastikan ESP32 menyala dan tidak sedang terhubung HP lain.';
      }
    } catch (e) {
      if (mounted) setState(() => _error = e.toString());
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  Future<void> _connect(BleDeviceInfo info) async {
    if (_scanning) return;
    final ble = context.read<BleService>();
    if (ble.isConnected && ble.connectedDeviceId == info.id) {
      Navigator.of(context).pop(true);
      return;
    }
    if (ble.isConnected && ble.connectedDeviceId != info.id) {
      final switchOk = await showDialog<bool>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: const Text('Ganti koneksi'),
          content: Text(
            'Lepas ${ble.connectedName} lalu hubungkan ke ${info.name}?',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Batal'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Ganti'),
            ),
          ],
        ),
      );
      if (switchOk != true) return;
    }
    setState(() {
      _scanning = true;
      _error = null;
    });
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    try {
      await ble.connect(info);
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } catch (e) {
      if (mounted) {
        setState(() {
          _error =
              'Gagal konek: ${e.toString().replaceAll('Exception: ', '')}';
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

  Widget _rssiBars(int rssi) {
    final level = rssi == 0
        ? 0
        : rssi > -60
            ? 3
            : rssi > -75
                ? 2
                : 1;
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.end,
      children: List.generate(3, (i) {
        return Container(
          width: 4,
          height: 6.0 + (i * 5),
          margin: const EdgeInsets.only(left: 2),
          decoration: BoxDecoration(
            color: i < level
                ? AppTheme.success
                : Theme.of(context).colorScheme.outlineVariant,
            borderRadius: BorderRadius.circular(1),
          ),
        );
      }),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();
    final currentId = ble.connectedDeviceId;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Cari device'),
        actions: [
          if (ble.isConnected)
            TextButton(
              onPressed: () async {
                if (!await confirmBleDisconnect(context, ble.connectedName)) {
                  return;
                }
                await ble.disconnect(forget: true);
                if (mounted) setState(() {});
              },
              child: const Text('Lepas'),
            ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
        child: Column(
          children: [
            FilledButton.icon(
              onPressed: _scanning ? null : _scan,
              icon: _scanning
                  ? const SizedBox(
                      width: 18,
                      height: 18,
                      child: CircularProgressIndicator(
                          strokeWidth: 2, color: Colors.white),
                    )
                  : const Icon(Icons.search),
              label: Text(_scanning ? 'Mencari…' : 'Scan ulang'),
            ),
            const SizedBox(height: 12),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Text(
                  _error!,
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: AppTheme.danger, height: 1.35),
                ),
              ),
            Expanded(
              child: _devices.isEmpty && !_scanning
                  ? AppEmptyState(
                      icon: Icons.sensors_off,
                      title: 'Device belum ditemukan',
                      subtitle:
                          'Dekatkan HP ke ESP32 lalu ketuk Scan ulang.',
                      actionLabel: 'Scan ulang',
                      onAction: _scan,
                    )
                  : ListView.separated(
                      itemCount: _devices.length,
                      separatorBuilder: (_, _) => const SizedBox(height: 8),
                      itemBuilder: (context, i) {
                        final d = _devices[i];
                        final isCurrent =
                            ble.isConnected && currentId == d.id;
                        return Card(
                          child: ListTile(
                            contentPadding: const EdgeInsets.symmetric(
                                horizontal: 14, vertical: 6),
                            leading: CircleAvatar(
                              backgroundColor: Theme.of(context)
                                  .colorScheme
                                  .primary
                                  .withValues(alpha: 0.12),
                              child: Icon(
                                  isCurrent
                                      ? Icons.bluetooth_connected
                                      : Icons.memory,
                                  color: Theme.of(context).colorScheme.primary),
                            ),
                            title: Text(d.name,
                                style: const TextStyle(
                                    fontWeight: FontWeight.w800)),
                            subtitle: Text(_scanning
                                ? 'Menghubungkan…'
                                : (isCurrent
                                    ? 'Sedang terhubung'
                                    : (d.rssi == 0
                                        ? 'Ketuk untuk ganti ke device ini'
                                        : 'Sinyal ${d.rssi} dBm · ketuk untuk ganti'))),
                            trailing: isCurrent
                                ? const StatusBadge(
                                    label: 'Aktif',
                                    color: AppTheme.success,
                                    icon: Icons.circle)
                                : (_scanning
                                    ? const SizedBox(
                                        width: 18,
                                        height: 18,
                                        child: CircularProgressIndicator(
                                            strokeWidth: 2),
                                      )
                                    : _rssiBars(d.rssi)),
                            onTap: _scanning ? null : () => _connect(d),
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}
