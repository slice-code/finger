import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';
import '../theme/app_theme.dart';
import 'device_scan_screen.dart';

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  bool _busy = false;

  Future<void> _goScan() async {
    await Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => const DeviceScanScreen()),
    );
    if (!mounted) return;
    await context.read<BleService>().readStatus();
  }

  Future<void> _disconnect() async {
    final ble = context.read<BleService>();
    if (ble.isConnecting || ble.isAutoReconnecting) {
      await ble.disconnect(forget: true);
      return;
    }
    if (!await confirmBleDisconnect(context, ble.connectedName)) return;
    await ble.disconnect(forget: true);
  }

  Future<void> _toggleAutoscan(bool value) async {
    final ble = context.read<BleService>();
    setState(() => _busy = true);
    try {
      await ble.writeCommand(value ? 'AUTOSCAN ON' : 'AUTOSCAN OFF');
      await ble.readStatus();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
              content:
                  Text('Gagal: ${e.toString().replaceAll('Exception: ', '')}')),
        );
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();
    final connected = ble.isConnected;
    final connecting = ble.isConnecting || ble.isAutoReconnecting;
    final status = ble.lastStatus;
    final deviceName = ble.connectedName;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Beranda'),
        actions: [
          IconButton(
            icon: const Icon(Icons.swap_horiz),
            tooltip: 'Ganti koneksi',
            onPressed: _goScan,
          ),
          if (connected)
            IconButton(
              icon: const Icon(Icons.bluetooth_disabled),
              tooltip: 'Lepas koneksi',
              onPressed: _disconnect,
            ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          _connectionCard(context, connected, connecting, deviceName),
          const SizedBox(height: 16),
          if (!connected && !connecting)
            AppEmptyState(
              icon: Icons.bluetooth_searching,
              title: 'Belum terhubung ke device',
              subtitle:
                  'Nyalakan ESP32, lalu cari PJTKI-Finger (3V3) atau PJTKI-Finger-5V.',
              actionLabel: 'Cari device',
              onAction: _goScan,
            )
          else if (!connected && connecting)
            const Padding(
              padding: EdgeInsets.only(top: 48),
              child: Column(
                children: [
                  CircularProgressIndicator(),
                  SizedBox(height: 14),
                  Text('Menunggu koneksi Bluetooth…'),
                ],
              ),
            )
          else ...[
            _statusGrid(context, status),
            const SizedBox(height: 16),
            _autoscanCard(context, status, connected),
          ],
        ],
      ),
    );
  }

  Widget _connectionCard(
      BuildContext context, bool connected, bool connecting, String name) {
    final scheme = Theme.of(context).colorScheme;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Container(
                  width: 52,
                  height: 52,
                  decoration: BoxDecoration(
                    color: connected
                        ? AppTheme.success.withValues(alpha: 0.12)
                        : scheme.surfaceContainerHighest,
                    borderRadius: BorderRadius.circular(14),
                  ),
                  child: Icon(
                    connected
                        ? Icons.bluetooth_connected
                        : Icons.bluetooth_disabled,
                    color: connected ? AppTheme.success : scheme.outline,
                  ),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        connected
                            ? name
                            : (connecting
                                ? 'Menghubungkan…'
                                : 'Belum terhubung'),
                        style: const TextStyle(
                            fontSize: 16, fontWeight: FontWeight.w800),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        connected
                            ? 'Siap absensi via Bluetooth'
                            : (connecting
                                ? 'Mencoba device terakhir'
                                : 'Hubungkan ESP32 untuk memulai'),
                        style: TextStyle(
                            color: scheme.onSurfaceVariant, fontSize: 13),
                      ),
                    ],
                  ),
                ),
                if (connected)
                  const StatusBadge(
                      label: 'Online',
                      color: AppTheme.success,
                      icon: Icons.circle)
                else if (connecting)
                  const SizedBox(
                    width: 22,
                    height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                else
                  FilledButton.tonal(
                    onPressed: _goScan,
                    style: FilledButton.styleFrom(
                      minimumSize: const Size(0, 40),
                      padding: const EdgeInsets.symmetric(horizontal: 14),
                    ),
                    child: const Text('Hubungkan'),
                  ),
              ],
            ),
            if (connected || connecting) ...[
              const SizedBox(height: 12),
              Row(
                children: [
                  Expanded(
                    child: FilledButton.tonalIcon(
                      onPressed: _goScan,
                      style: FilledButton.styleFrom(
                        minimumSize: const Size(0, 42),
                      ),
                      icon: const Icon(Icons.swap_horiz, size: 18),
                      label: const Text('Ganti koneksi'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _disconnect,
                      style: OutlinedButton.styleFrom(
                        minimumSize: const Size(0, 42),
                        foregroundColor: AppTheme.danger,
                      ),
                      icon: const Icon(Icons.bluetooth_disabled, size: 18),
                      label: Text(connecting ? 'Batal' : 'Lepas'),
                    ),
                  ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _statusGrid(BuildContext context, DeviceStatus s) {
    final items = <({String label, String value, IconData icon, Color color})>[
      (
        label: 'Suhu',
        value: '${s.temp}°C',
        icon: Icons.thermostat,
        color: s.temp > 55 ? AppTheme.warning : AppTheme.success
      ),
      (
        label: 'Sidik jari',
        value: '${s.count} orang',
        icon: Icons.fingerprint,
        color: Theme.of(context).colorScheme.primary
      ),
      (
        label: 'Sensor',
        value: s.sensorReady ? 'Siap' : 'Error',
        icon: Icons.sensors,
        color: s.sensorReady ? AppTheme.success : AppTheme.danger
      ),
      (
        label: 'WiFi',
        value: s.wifiMode == 'STA' ? 'Terhubung' : 'Mode AP',
        icon: Icons.wifi,
        color: Theme.of(context).colorScheme.primary
      ),
      (
        label: 'Gate sentuh',
        value: s.irEnabled ? 'Aktif' : 'Mati',
        icon: Icons.touch_app_outlined,
        color: const Color(0xFF6A1B9A)
      ),
      (
        label: 'Absensi',
        value: s.autoActive ? 'Berjalan' : 'Dijeda',
        icon: Icons.play_circle_outline,
        color: s.autoActive ? AppTheme.success : AppTheme.warning
      ),
    ];
    return GridView.count(
      crossAxisCount: 2,
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      mainAxisSpacing: 10,
      crossAxisSpacing: 10,
      childAspectRatio: 2.05,
      children: items
          .map((e) => Card(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
                  child: Row(
                    children: [
                      Container(
                        width: 36,
                        height: 36,
                        decoration: BoxDecoration(
                          color: e.color.withValues(alpha: 0.14),
                          borderRadius: BorderRadius.circular(10),
                        ),
                        child: Icon(e.icon, color: e.color, size: 20),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(e.label,
                                style: TextStyle(
                                    color: Theme.of(context)
                                        .colorScheme
                                        .onSurfaceVariant,
                                    fontSize: 12)),
                            Text(e.value,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: const TextStyle(
                                    fontWeight: FontWeight.w800, fontSize: 15)),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ))
          .toList(),
    );
  }

  Widget _autoscanCard(BuildContext context, DeviceStatus s, bool connected) {
    return Card(
      child: SwitchListTile(
        value: s.autoActive,
        onChanged: connected && !_busy ? (v) => _toggleAutoscan(v) : null,
        title: const Text('Pindai otomatis',
            style: TextStyle(fontWeight: FontWeight.w700)),
        subtitle: const Text('Sensor memindai jari tanpa tombol'),
        secondary: Icon(
          Icons.fingerprint,
          color: s.autoActive
              ? AppTheme.success
              : Theme.of(context).colorScheme.outline,
        ),
      ),
    );
  }
}
