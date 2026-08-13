import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';
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

  Future<void> _toggleAutoscan(bool value) async {
    final ble = context.read<BleService>();
    setState(() => _busy = true);
    try {
      await ble.writeCommand(value ? 'AUTOSCAN ON' : 'AUTOSCAN OFF');
      await ble.readStatus();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Gagal: ${e.toString().replaceAll('Exception: ', '')}')),
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
        title: const Text('Dashboard'),
        actions: [
          IconButton(
            icon: const Icon(Icons.bluetooth_searching),
            tooltip: 'Cari Device',
            onPressed: _goScan,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _connectionCard(context, connected, connecting, deviceName),
          const SizedBox(height: 12),
          if (!connected && !connecting)
            _emptyState()
          else if (!connected && connecting)
            _connectingState()
          else
            Column(
              children: [
                _statusGrid(context, status),
                const SizedBox(height: 12),
                _autoscanCard(context, status, connected),
              ],
            ),
        ],
      ),
    );
  }

  Widget _connectionCard(
      BuildContext context, bool connected, bool connecting, String name) {
    final active = connected || connecting;
    return Card(
      color: connected ? const Color(0xFFE3F2FD) : Colors.grey.shade100,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Icon(
              connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
              size: 40,
              color: active ? const Color(0xFF1E88E5) : Colors.grey,
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    connected
                        ? name
                        : (connecting ? 'Menghubungkan...' : 'Belum terhubung'),
                    style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                  ),
                  Text(
                    connected
                        ? 'Terhubung via BLE'
                        : (connecting
                            ? 'Mencoba konek ke device terakhir'
                            : 'Ketuk ikon bluetooth untuk mencari'),
                    style: const TextStyle(color: Colors.grey, fontSize: 13),
                  ),
                ],
              ),
            ),
            if (connecting)
              const SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
          ],
        ),
      ),
    );
  }

  Widget _connectingState() {
    return const Padding(
      padding: EdgeInsets.only(top: 48),
      child: Column(
        children: [
          CircularProgressIndicator(),
          SizedBox(height: 12),
          Text('Menunggu koneksi BLE...'),
        ],
      ),
    );
  }

  Widget _emptyState() {
    return const Padding(
      padding: EdgeInsets.only(top: 48),
      child: Column(
        children: [
          Icon(Icons.wifi_tethering_error, size: 64, color: Colors.grey),
          SizedBox(height: 12),
          Text('Hubungkan ke device ESP32 dulu'),
        ],
      ),
    );
  }

  Widget _statusGrid(BuildContext context, DeviceStatus s) {
    final items = <({String label, String value, IconData icon, Color color})>[
      (label: 'Suhu', value: '${s.temp}°C', icon: Icons.thermostat, color: s.temp > 55 ? Colors.orange : Colors.green),
      (label: 'Fingerprint', value: '${s.count}', icon: Icons.fingerprint, color: const Color(0xFF1E88E5)),
      (label: 'Sensor', value: s.sensorReady ? 'Siap' : 'Error', icon: Icons.sensors, color: s.sensorReady ? Colors.green : Colors.red),
      (label: 'WiFi', value: s.wifiMode == 'STA' ? 'STA' : 'AP', icon: Icons.wifi, color: const Color(0xFF1E88E5)),
      (label: 'Gate IR', value: s.irEnabled ? 'Aktif' : 'Nonaktif', icon: Icons.gesture, color: Colors.purple),
      (label: 'Mode', value: s.autoActive ? 'Scan Aktif' : 'Paused', icon: Icons.play_circle,
          color: s.autoActive ? Colors.green : Colors.orange),
    ];
    return GridView.count(
      crossAxisCount: 2,
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      mainAxisSpacing: 10,
      crossAxisSpacing: 10,
      mainAxisExtent: 72,
      children: items
          .map((e) => Card(
                color: e.color,
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Row(
                    children: [
                      Icon(e.icon, color: Colors.white),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(e.label,
                                style: const TextStyle(color: Colors.white70, fontSize: 12)),
                            Text(e.value,
                                style: const TextStyle(
                                    color: Colors.white,
                                    fontWeight: FontWeight.bold,
                                    fontSize: 16)),
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
        title: const Text('Auto Scan Fingerprint'),
        subtitle: const Text('Sensor otomatis memindai jari'),
        secondary: const Icon(Icons.autorenew),
      ),
    );
  }
}
