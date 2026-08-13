import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';
import '../services/settings_store.dart';
import 'connect_screen.dart';

class WifiApInfo {
  final String ssid;
  final int rssi;
  final bool enc;
  WifiApInfo({required this.ssid, required this.rssi, this.enc = true});
}

class SetelanScreen extends StatefulWidget {
  const SetelanScreen({super.key});

  @override
  State<SetelanScreen> createState() => _SetelanScreenState();
}

class _SetelanScreenState extends State<SetelanScreen> {
  final _appApiUrl = TextEditingController();
  final _deviceApiUrl = TextEditingController();
  final _kodeCabang = TextEditingController();
  final _deviceId = TextEditingController();
  final _apiKey = TextEditingController();
  final _wifiSsid = TextEditingController();
  final _wifiPass = TextEditingController();
  final _uploadInterval = TextEditingController(text: '120');
  bool _irEnabled = true;
  bool _apEnabled = false;
  bool _loading = false;
  bool _wifiScanning = false;
  String _statusMsg = '';
  StreamSubscription? _bleSub;
  final List<WifiApInfo> _wifiAps = [];

  @override
  void initState() {
    super.initState();
    final settings = context.read<SettingsStore>();
    _appApiUrl.text = settings.apiBaseUrl;
    _deviceApiUrl.text = settings.apiBaseUrl;
    _kodeCabang.text = settings.kodeCabang;
    _deviceId.text = settings.deviceId;
    _apiKey.text = settings.apiKey;
    _bleSub = context.read<BleService>().eventStream.listen(_onBleEvent);
    _loadDeviceSettings();
  }

  void _onBleEvent(BleEvent ev) {
    if (!mounted) return;
    switch (ev.type) {
      case 'wifi_queued':
      case 'wifi_connecting':
        setState(() => _statusMsg = 'Device mencoba connect WiFi...');
        break;
      case 'wifi_saved':
        if (ev.data['ok'] != true) {
          setState(() => _statusMsg = 'Gagal menyimpan WiFi ke device');
        }
        break;
      case 'wifi_connected':
        final ssid = ev.data['ssid']?.toString() ?? '';
        final ip = ev.data['ip']?.toString() ?? '';
        setState(() => _statusMsg = 'WiFi terhubung: $ssid ($ip) ✓');
        context.read<BleService>().readStatus();
        break;
      case 'wifi_failed':
        setState(() {
          _statusMsg = 'WiFi gagal connect — cek SSID/password ✗';
        });
        break;
      case 'settings_saved':
        setState(() => _statusMsg = 'Pengaturan device disimpan ✓');
        break;
      case 'settings_fail':
        setState(() {
          _statusMsg = 'Gagal simpan: ${ev.data['reason'] ?? 'error'}';
        });
        break;
      case 'wifi_scan':
        final st = ev.data['status']?.toString() ?? '';
        if (st == 'scanning' || st == 'queued') {
          setState(() {
            _wifiScanning = true;
            _statusMsg = 'ESP32 sedang scan WiFi (bisa 20–40 detik)...';
          });
        } else if (st == 'busy') {
          setState(() {
            _wifiScanning = false;
            _statusMsg = 'Scan sibuk — coba lagi sebentar';
          });
        } else if (st == 'done') {
          setState(() {
            _wifiScanning = false;
            _statusMsg = _wifiAps.isEmpty
                ? 'Scan selesai — tidak ada jaringan'
                : 'Scan selesai: ${_wifiAps.length} jaringan';
          });
        }
        break;
      case 'wifi_scan_ap':
        final ssid = ev.data['ssid']?.toString() ?? '';
        if (ssid.isEmpty) return;
        final rssi = (ev.data['rssi'] as num?)?.toInt() ?? -100;
        final enc = ev.data['enc'] != false;
        setState(() {
          final i = _wifiAps.indexWhere((a) => a.ssid == ssid);
          if (i < 0) {
            _wifiAps.add(WifiApInfo(ssid: ssid, rssi: rssi, enc: enc));
          } else if (rssi > _wifiAps[i].rssi) {
            _wifiAps[i] = WifiApInfo(ssid: ssid, rssi: rssi, enc: enc);
          }
          _wifiAps.sort((a, b) => b.rssi.compareTo(a.rssi));
        });
        break;
    }
  }

  @override
  void dispose() {
    _bleSub?.cancel();
    _appApiUrl.dispose();
    _deviceApiUrl.dispose();
    _kodeCabang.dispose();
    _deviceId.dispose();
    _apiKey.dispose();
    _wifiSsid.dispose();
    _wifiPass.dispose();
    _uploadInterval.dispose();
    super.dispose();
  }

  Future<void> _loadDeviceSettings() async {
    final ble = context.read<BleService>();
    if (!ble.isConnected) return;
    final s = await ble.readSettings();
    if (!mounted) return;
    setState(() {
      if (s['apiBaseUrl'] != null) {
        _deviceApiUrl.text = s['apiBaseUrl'].toString();
      }
      if (s['kodeCabang'] != null) _kodeCabang.text = s['kodeCabang'].toString();
      if (s['deviceId'] != null) _deviceId.text = s['deviceId'].toString();
      if (s['apiKey'] != null) _apiKey.text = s['apiKey'].toString();
      if (s['wifiSsid'] != null) _wifiSsid.text = s['wifiSsid'].toString();
      if (s['irEnabled'] != null) _irEnabled = s['irEnabled'] == true;
      if (s['apEnabled'] != null) _apEnabled = s['apEnabled'] == true;
      if (s['uploadIntervalMinutes'] != null) {
        final minutes = int.tryParse(s['uploadIntervalMinutes'].toString()) ?? 120;
        _uploadInterval.text = minutes.toString();
      }
    });
  }

  Future<void> _saveAppApi() async {
    final settings = context.read<SettingsStore>();
    final url = _appApiUrl.text.trim();
    if (url.isEmpty) return;
    settings.apiBaseUrl = url;
    setState(() => _statusMsg = 'URL API app (HP) disimpan ✓');
  }

  Future<void> _copyDeviceUrlToApp() async {
    _appApiUrl.text = _deviceApiUrl.text.trim();
    await _saveAppApi();
  }

  Future<void> _saveAll() async {
    setState(() {
      _loading = true;
      _statusMsg = '';
    });
    final settings = context.read<SettingsStore>();
    // App API tetap dari field app — jangan ditimpa field device.
    if (_appApiUrl.text.trim().isNotEmpty) {
      settings.apiBaseUrl = _appApiUrl.text.trim();
    }
    settings.kodeCabang = _kodeCabang.text.trim();
    settings.deviceId = _deviceId.text.trim();
    settings.apiKey = _apiKey.text.trim();

    final ble = context.read<BleService>();
    try {
      if (ble.isConnected) {
        final rawUrl = _deviceApiUrl.text.trim();
        var deviceUrl = rawUrl;
        var httpsRewritten = false;
        if (deviceUrl.toLowerCase().startsWith('https://')) {
          deviceUrl = 'http://${deviceUrl.substring(8)}';
          _deviceApiUrl.text = deviceUrl;
          httpsRewritten = true;
        }
        final intervalMinutes = int.tryParse(_uploadInterval.text.trim()) ?? 120;
        await ble.writeSettings({
          'apiBaseUrl': deviceUrl,
          'kodeCabang': _kodeCabang.text.trim(),
          'deviceId': _deviceId.text.trim(),
          'apiKey': _apiKey.text.trim(),
          'irEnabled': _irEnabled,
          'apEnabled': _apEnabled,
          'uploadIntervalMinutes': intervalMinutes.clamp(5, 1440),
        });
        await ble.setUploadInterval(intervalMinutes.clamp(5, 1440));
        setState(() {
          _statusMsg = httpsRewritten
              ? 'Tersimpan sebagai HTTP (HTTPS tidak stabil di ESP32): $deviceUrl'
              : 'Pengaturan disimpan ✓';
        });
      } else {
        setState(() => _statusMsg = 'Pengaturan app disimpan ✓');
      }
    } catch (e) {
      setState(() => _statusMsg = 'Gagal: ${e.toString().replaceAll('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _scanWifi() async {
    final ble = context.read<BleService>();
    if (!ble.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Hubungkan device ESP32 dulu')),
      );
      return;
    }
    setState(() {
      _wifiAps.clear();
      _wifiScanning = true;
      _statusMsg = 'Meminta ESP32 scan WiFi...';
    });
    try {
      await ble.requestWifiScan();
    } catch (e) {
      if (mounted) {
        setState(() {
          _wifiScanning = false;
          _statusMsg = 'Gagal scan: ${e.toString().replaceAll('Exception: ', '')}';
        });
      }
    }
  }

  Future<void> _saveWifi() async {
    final ble = context.read<BleService>();
    if (!ble.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Hubungkan device ESP32 dulu')),
      );
      return;
    }
    if (_wifiSsid.text.trim().isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('SSID WiFi wajib diisi')),
      );
      return;
    }
    try {
      await ble.writeSettings({
        'wifiSsid': _wifiSsid.text.trim(),
        'wifiPass': _wifiPass.text,
      });
      if (mounted) {
        setState(() => _statusMsg = 'WiFi dikirim ke device — menunggu connect...');
      }
    } catch (e) {
      if (mounted) {
        setState(() => _statusMsg = 'Gagal: ${e.toString().replaceAll('Exception: ', '')}');
      }
    }
  }

  void _editConnection() {
    Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => const ConnectScreen()),
    );
  }

  String _bars(int rssi) {
    if (rssi >= -55) return 'Sangat kuat';
    if (rssi >= -67) return 'Kuat';
    if (rssi >= -80) return 'Sedang';
    return 'Lemah';
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Setelan'),
        actions: [
          IconButton(
            icon: const Icon(Icons.dns),
            tooltip: 'Ubah Koneksi Server App',
            onPressed: _editConnection,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _card(
            title: 'API App (HP)',
            subtitle:
                'Dipakai app untuk daftar karyawan / riwayat. Bisa http:// atau https://cks.slice-code.com',
            child: Column(
              children: [
                TextField(
                  controller: _appApiUrl,
                  decoration: const InputDecoration(
                    labelText: 'API Base URL (app)',
                    prefixIcon: Icon(Icons.phone_android),
                    border: OutlineInputBorder(),
                    isDense: true,
                    helperText: 'Contoh: https://cks.slice-code.com atau http://...',
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.tonalIcon(
                        onPressed: _saveAppApi,
                        icon: const Icon(Icons.save),
                        label: const Text('Simpan URL App'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: ble.isConnected ? _copyDeviceUrlToApp : null,
                        icon: const Icon(Icons.copy_all),
                        label: const Text('Samakan dgn Device'),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          _card(
            title: 'Pengaturan Device (ESP32)',
            subtitle: ble.isConnected
                ? 'BLE terhubung — URL ini dipakai ESP untuk absensi/enroll'
                : 'Hubungkan device untuk mengubah',
            child: Column(
              children: [
                TextField(
                  controller: _deviceApiUrl,
                  enabled: ble.isConnected,
                  decoration: const InputDecoration(
                    labelText: 'API Base URL (device)',
                    prefixIcon: Icon(Icons.dns_outlined),
                    border: OutlineInputBorder(),
                    isDense: true,
                    helperText: 'Isi https:// juga boleh — otomatis jadi http:// di device',
                  ),
                ),
                const SizedBox(height: 12),
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        controller: _kodeCabang,
                        enabled: ble.isConnected,
                        decoration: const InputDecoration(
                          labelText: 'Kode Cabang',
                          prefixIcon: Icon(Icons.business),
                          border: OutlineInputBorder(),
                          isDense: true,
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: TextField(
                        controller: _deviceId,
                        enabled: ble.isConnected,
                        decoration: const InputDecoration(
                          labelText: 'Device ID',
                          prefixIcon: Icon(Icons.devices),
                          border: OutlineInputBorder(),
                          isDense: true,
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _apiKey,
                  enabled: ble.isConnected,
                  obscureText: true,
                  decoration: const InputDecoration(
                    labelText: 'Device Key (X-Device-Key)',
                    prefixIcon: Icon(Icons.key),
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _uploadInterval,
                  enabled: ble.isConnected,
                  keyboardType: TextInputType.number,
                  decoration: const InputDecoration(
                    labelText: 'Jadwal upload otomatis (menit)',
                    prefixIcon: Icon(Icons.schedule),
                    border: OutlineInputBorder(),
                    isDense: true,
                    helperText: 'Default 120 menit (2 jam).',
                  ),
                ),
                const SizedBox(height: 8),
                SwitchListTile(
                  value: _irEnabled,
                  onChanged: ble.isConnected
                      ? (v) => setState(() => _irEnabled = v)
                      : null,
                  title: const Text('Gate IR / Touch presence'),
                  subtitle: const Text('Sensor hanya dipoll saat ada jari'),
                  contentPadding: EdgeInsets.zero,
                ),
                SwitchListTile(
                  value: _apEnabled,
                  onChanged: ble.isConnected
                      ? (v) => setState(() => _apEnabled = v)
                      : null,
                  title: const Text('Mode SoftAP (FPM10A-Bridge)'),
                  subtitle: const Text(
                      'Default OFF — setup lewat BLE. ON hanya jika butuh web UI 192.168.4.1'),
                  contentPadding: EdgeInsets.zero,
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.tonalIcon(
                        onPressed: ble.isConnected ? () async {
                          try {
                            await ble.syncNow();
                            if (mounted) {
                              setState(() => _statusMsg = 'Sync manual dikirim ke ESP32 ✓');
                            }
                          } catch (e) {
                            if (mounted) {
                              setState(() => _statusMsg = 'Gagal sync: ${e.toString().replaceAll('Exception: ', '')}');
                            }
                          }
                        } : null,
                        icon: const Icon(Icons.sync),
                        label: const Text('Sync Sekarang'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: _loading || !ble.isConnected ? null : _saveAll,
                        icon: const Icon(Icons.save),
                        label: const Text('Simpan'),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          _card(
            title: 'WiFi Device',
            subtitle: 'Scan jaringan dari antena ESP32, lalu pilih & kirim password',
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                FilledButton.tonalIcon(
                  onPressed: ble.isConnected && !_wifiScanning ? _scanWifi : null,
                  icon: _wifiScanning
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.wifi_find),
                  label: Text(_wifiScanning ? 'Scanning...' : 'Scan WiFi dari ESP32'),
                ),
                if (_wifiAps.isNotEmpty) ...[
                  const SizedBox(height: 8),
                  ConstrainedBox(
                    constraints: const BoxConstraints(maxHeight: 220),
                    child: ListView.separated(
                      shrinkWrap: true,
                      itemCount: _wifiAps.length,
                      separatorBuilder: (context, index) => const Divider(height: 1),
                      itemBuilder: (context, i) {
                        final ap = _wifiAps[i];
                        final selected = ap.ssid == _wifiSsid.text;
                        return ListTile(
                          dense: true,
                          selected: selected,
                          leading: Icon(
                            ap.enc ? Icons.lock : Icons.lock_open,
                            size: 20,
                            color: selected ? const Color(0xFF1E88E5) : null,
                          ),
                          title: Text(ap.ssid),
                          subtitle: Text('${_bars(ap.rssi)} (${ap.rssi} dBm)'),
                          trailing: selected
                              ? const Icon(Icons.check, color: Color(0xFF1E88E5))
                              : null,
                          onTap: () => setState(() => _wifiSsid.text = ap.ssid),
                        );
                      },
                    ),
                  ),
                ],
                const SizedBox(height: 12),
                TextField(
                  controller: _wifiSsid,
                  decoration: const InputDecoration(
                    labelText: 'SSID WiFi',
                    prefixIcon: Icon(Icons.wifi),
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _wifiPass,
                  obscureText: true,
                  decoration: const InputDecoration(
                    labelText: 'Password WiFi',
                    prefixIcon: Icon(Icons.lock_outline),
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
                const SizedBox(height: 8),
                FilledButton.icon(
                  onPressed: ble.isConnected ? _saveWifi : null,
                  icon: const Icon(Icons.save_alt),
                  label: const Text('Kirim WiFi ke Device'),
                ),
              ],
            ),
          ),
          if (_statusMsg.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(
                _statusMsg,
                textAlign: TextAlign.center,
                style: const TextStyle(color: Color(0xFF1E88E5), fontSize: 13),
              ),
            ),
        ],
      ),
    );
  }

  Widget _card({
    required String title,
    required String subtitle,
    required Widget child,
  }) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            const SizedBox(height: 2),
            Text(subtitle, style: const TextStyle(color: Colors.grey, fontSize: 12)),
            const SizedBox(height: 12),
            child,
          ],
        ),
      ),
    );
  }
}
