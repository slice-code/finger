import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/ble_service.dart';
import '../services/settings_store.dart';
import '../theme/app_theme.dart';
import 'connect_screen.dart';
import 'device_scan_screen.dart';
import 'login_screen.dart';

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
  final _deviceId = TextEditingController();
  final _apiKey = TextEditingController();
  final _wifiSsid = TextEditingController();
  final _wifiPass = TextEditingController();

  List<Branch> _branches = [];
  String? _kodeCabang;
  bool _loadingBranches = false;

  int _uploadInterval = 120;
  static const _intervalChoices = <int>[5, 15, 30, 60, 120, 240, 360, 720, 1440];

  bool _irEnabled = true;
  bool _obscureKey = true;
  bool _obscureWifi = true;
  bool _loading = false;
  bool _wifiScanning = false;
  String _statusMsg = '';
  StreamSubscription? _bleSub;
  final List<WifiApInfo> _wifiAps = [];

  bool _authBusy = false;

  String _tokenStatusLabel(SettingsStore settings) {
    final token = settings.authToken;
    if (token.isEmpty) return 'Belum login';
    try {
      final parts = token.split('.');
      if (parts.length < 2) return 'Token tidak valid';
      var payload = parts[1];
      payload = base64Url.normalize(payload);
      final map = json.decode(utf8.decode(base64Url.decode(payload)));
      final exp = map['exp'];
      if (exp is! num) return 'Token aktif';
      final expDt = DateTime.fromMillisecondsSinceEpoch(exp.toInt() * 1000);
      if (DateTime.now().isAfter(expDt)) {
        return 'Token kadaluarsa (${_fmtDt(expDt)})';
      }
      return 'Token aktif sampai ${_fmtDt(expDt)}';
    } catch (_) {
      return 'Token aktif';
    }
  }

  String _fmtDt(DateTime dt) {
    final l = dt.toLocal();
    return '${l.day}/${l.month}/${l.year} ${l.hour.toString().padLeft(2, '0')}:${l.minute.toString().padLeft(2, '0')}';
  }

  Future<void> _refreshAuthToken() async {
    setState(() {
      _authBusy = true;
      _statusMsg = 'Memperbarui token...';
    });
    try {
      await context.read<ApiService>().refreshAuthToken();
      if (!mounted) return;
      setState(() => _statusMsg = 'Token login diperbarui');
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Token berhasil diperbarui')),
      );
    } catch (e) {
      if (!mounted) return;
      final msg = e.toString().replaceAll('Exception: ', '');
      setState(() => _statusMsg = 'Gagal refresh token: $msg');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(msg)),
      );
    } finally {
      if (mounted) setState(() => _authBusy = false);
    }
  }

  Future<void> _logout() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Keluar dari akun?'),
        content: const Text(
          'Token login akan dihapus. Anda perlu login ulang untuk akses Staff dan sync.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Batal')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Logout')),
        ],
      ),
    );
    if (ok != true || !mounted) return;
    setState(() => _authBusy = true);
    try {
      await context.read<ApiService>().logout();
      if (!mounted) return;
      Navigator.of(context).pushAndRemoveUntil(
        MaterialPageRoute(builder: (_) => const LoginScreen()),
        (_) => false,
      );
    } finally {
      if (mounted) setState(() => _authBusy = false);
    }
  }

  @override
  void initState() {
    super.initState();
    final settings = context.read<SettingsStore>();
    _appApiUrl.text = settings.apiBaseUrl;
    _deviceApiUrl.text = settings.apiBaseUrl;
    _kodeCabang = settings.kodeCabang.isEmpty ? null : settings.kodeCabang;
    _deviceId.text = settings.deviceId;
    _apiKey.text = settings.apiKey;
    _bleSub = context.read<BleService>().eventStream.listen(_onBleEvent);
    _loadBranches();
    _loadDeviceSettings();
  }

  Future<void> _loadBranches() async {
    setState(() => _loadingBranches = true);
    try {
      final list = await context.read<ApiService>().fetchBranches();
      if (!mounted) return;
      setState(() {
        _branches = list;
        // Pastikan value dropdown valid.
        if (_kodeCabang != null &&
            _kodeCabang!.isNotEmpty &&
            !_branches.any((b) => b.kode == _kodeCabang)) {
          // kode lama tidak ada di list — tetap tampilkan sebagai opsi sementara
          _branches = [
            Branch(kode: _kodeCabang!, nama: _kodeCabang!, kota: ''),
            ..._branches,
          ];
        }
      });
    } catch (_) {
      // Biarkan dropdown kosong; user masih bisa pilih device BLK.
    } finally {
      if (mounted) setState(() => _loadingBranches = false);
    }
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
        setState(() => _statusMsg = 'WiFi terhubung: $ssid ($ip)');
        context.read<BleService>().readStatus();
        break;
      case 'wifi_failed':
        setState(() => _statusMsg = 'WiFi gagal connect — cek SSID/password');
        break;
      case 'settings_saved':
        setState(() => _statusMsg = 'Pengaturan device disimpan');
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
            _statusMsg = 'ESP32 sedang scan WiFi...';
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
    _deviceId.dispose();
    _apiKey.dispose();
    _wifiSsid.dispose();
    _wifiPass.dispose();
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
      if (s['kodeCabang'] != null) {
        final kode = s['kodeCabang'].toString();
        _kodeCabang = kode.isEmpty ? null : kode;
        if (_kodeCabang != null &&
            !_branches.any((b) => b.kode == _kodeCabang)) {
          _branches = [
            Branch(kode: _kodeCabang!, nama: _kodeCabang!, kota: ''),
            ..._branches,
          ];
        }
      }
      if (s['deviceId'] != null) _deviceId.text = s['deviceId'].toString();
      if (s['apiKey'] != null) _apiKey.text = s['apiKey'].toString();
      if (s['wifiSsid'] != null) _wifiSsid.text = s['wifiSsid'].toString();
      if (s['irEnabled'] != null) _irEnabled = s['irEnabled'] == true;
      if (s['uploadIntervalMinutes'] != null) {
        final minutes =
            int.tryParse(s['uploadIntervalMinutes'].toString()) ?? 120;
        _uploadInterval = minutes.clamp(5, 1440);
      }
    });
  }

  Future<void> _saveAppApi() async {
    final url = _appApiUrl.text.trim();
    if (url.isEmpty) return;
    context.read<SettingsStore>().apiBaseUrl = url;
    setState(() => _statusMsg = 'URL API app disimpan');
    await _loadBranches();
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
    if (_appApiUrl.text.trim().isNotEmpty) {
      settings.apiBaseUrl = _appApiUrl.text.trim();
    }
    settings.kodeCabang = _kodeCabang ?? '';
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
        final interval = _uploadInterval.clamp(5, 1440);
        await ble.writeSettings({
          'apiBaseUrl': deviceUrl,
          'kodeCabang': _kodeCabang ?? '',
          'deviceId': _deviceId.text.trim(),
          'apiKey': _apiKey.text.trim(),
          'irEnabled': _irEnabled,
          'uploadIntervalMinutes': interval,
        });
        await ble.setUploadInterval(interval);
        setState(() {
          _statusMsg = httpsRewritten
              ? 'Tersimpan sebagai HTTP: $deviceUrl'
              : 'Pengaturan disimpan';
        });
      } else {
        setState(() => _statusMsg = 'Pengaturan app disimpan (device belum konek)');
      }
    } catch (e) {
      setState(() =>
          _statusMsg = 'Gagal: ${e.toString().replaceAll('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _loginAndPickDevice() async {
    final settings = context.read<SettingsStore>();
    final api = context.read<ApiService>();
    if (!settings.isLoggedIn) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Login user BLK dulu')),
      );
      return;
    }
    setState(() {
      _loading = true;
      _statusMsg = 'Mengambil daftar device...';
    });
    try {
      final devices = await api.fetchDevices();
      if (!mounted) return;
      if (devices.isEmpty) {
        setState(() => _statusMsg = 'Tidak ada device key di BLK');
        return;
      }
      final selected = await showModalBottomSheet<Map<String, dynamic>>(
        context: context,
        showDragHandle: true,
        builder: (ctx) => SafeArea(
          child: ListView(
            shrinkWrap: true,
            children: [
              const Padding(
                padding: EdgeInsets.fromLTRB(16, 8, 16, 8),
                child: Text('Pilih Device BLK',
                    style:
                        TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
              ),
              for (final d in devices)
                ListTile(
                  leading: const Icon(Icons.devices, color: Color(0xFF1E88E5)),
                  title: Text(
                      '${d['kode_cabang']}${(d['device_id']?.toString() ?? '').isNotEmpty ? ' — ${d['device_id']}' : ''}'),
                  subtitle: Text(
                      'Masuk ${d['batas_masuk'] ?? '08:00'} · Pulang ${d['batas_pulang'] ?? '16:00'}'),
                  onTap: () => Navigator.pop(ctx, d),
                ),
            ],
          ),
        ),
      );
      if (selected == null) {
        setState(() => _statusMsg = '');
        return;
      }
      settings.apiKey = selected['device_key']?.toString() ?? '';
      settings.kodeCabang = selected['kode_cabang']?.toString() ?? '';
      settings.deviceId = selected['device_id']?.toString() ?? '';
      _apiKey.text = settings.apiKey;
      _kodeCabang =
          settings.kodeCabang.isEmpty ? null : settings.kodeCabang;
      _deviceId.text = settings.deviceId;
      if (_kodeCabang != null &&
          !_branches.any((b) => b.kode == _kodeCabang)) {
        _branches = [
          Branch(kode: _kodeCabang!, nama: _kodeCabang!, kota: ''),
          ..._branches,
        ];
      }
      setState(() {
        _statusMsg =
            'Device dipilih: ${settings.kodeCabang}. Tekan Simpan untuk kirim ke ESP32.';
      });
    } catch (e) {
      if (mounted) {
        setState(() => _statusMsg =
            'Gagal ambil device: ${e.toString().replaceAll('Exception: ', '')}');
      }
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _cleanFingers() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Bersihkan Fingerprint'),
        content: const Text(
            'Hapus SEMUA sidik jari di sensor ESP32 + data json lokal?\n\n'
            'Gunakan sebelum sinkron ulang dari server.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Batal')),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            style: FilledButton.styleFrom(backgroundColor: Colors.red),
            child: const Text('Bersihkan'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    final ble = context.read<BleService>();
    setState(() {
      _loading = true;
      _statusMsg = 'Membersihkan fingerprint ESP32...';
    });
    try {
      if (!ble.isConnected) {
        setState(() => _statusMsg = 'Hubungkan device ESP32 dulu');
        return;
      }
      await ble.cleanFingers();
      await Future<void>.delayed(const Duration(milliseconds: 1200));
      if (!mounted) return;
      setState(() => _statusMsg = 'Fingerprint ESP32 dibersihkan');
    } catch (e) {
      if (mounted) {
        setState(() => _statusMsg =
            'Gagal: ${e.toString().replaceAll('Exception: ', '')}');
      }
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
          _statusMsg =
              'Gagal scan: ${e.toString().replaceAll('Exception: ', '')}';
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
        const SnackBar(content: Text('Pilih / isi SSID WiFi dulu')),
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
        setState(() => _statusMsg =
            'Gagal: ${e.toString().replaceAll('Exception: ', '')}');
      }
    }
  }

  String _bars(int rssi) {
    if (rssi >= -55) return 'Sangat kuat';
    if (rssi >= -67) return 'Kuat';
    if (rssi >= -80) return 'Sedang';
    return 'Lemah';
  }

  String _intervalLabel(int minutes) {
    if (minutes < 60) return '$minutes menit';
    if (minutes == 60) return '1 jam';
    if (minutes % 60 == 0) return '${minutes ~/ 60} jam';
    return '$minutes menit';
  }

  InputDecoration _fieldDeco({
    required String label,
    String? helper,
    Widget? prefix,
    Widget? suffix,
  }) {
    return InputDecoration(
      labelText: label,
      helperText: helper,
      prefixIcon: prefix,
      suffixIcon: suffix,
      border: const OutlineInputBorder(),
      filled: true,
      fillColor: Colors.grey.shade50,
      isDense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 14),
    );
  }

  Widget _section({
    required IconData icon,
    required String title,
    required String subtitle,
    required Widget child,
    Color? accent,
  }) {
    final color = accent ?? const Color(0xFF1E88E5);
    return Card(
      elevation: 0,
      color: Colors.white,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: Colors.grey.shade200),
      ),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: color.withValues(alpha: 0.12),
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: Icon(icon, color: color, size: 22),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title,
                          style: const TextStyle(
                              fontWeight: FontWeight.w700, fontSize: 16)),
                      const SizedBox(height: 2),
                      Text(subtitle,
                          style: TextStyle(
                              color: Colors.grey.shade600, fontSize: 12)),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            child,
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();
    final settings = context.watch<SettingsStore>();
    final connected = ble.isConnected;
    final intervalValues = {
      ..._intervalChoices,
      if (!_intervalChoices.contains(_uploadInterval)) _uploadInterval,
    }.toList()
      ..sort();

    return Scaffold(
      backgroundColor: const Color(0xFFF5F7FA),
      appBar: AppBar(
        title: const Text('Setelan'),
        backgroundColor: Colors.white,
        surfaceTintColor: Colors.transparent,
        actions: [
          IconButton(
            icon: const Icon(Icons.dns_outlined),
            tooltip: 'Koneksi Server App',
            onPressed: () {
              Navigator.of(context).push(
                MaterialPageRoute(builder: (_) => const ConnectScreen()),
              );
            },
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 28),
        children: [
          // Status strip
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
            decoration: BoxDecoration(
              color: connected
                  ? const Color(0xFFE8F5E9)
                  : const Color(0xFFFFF3E0),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Row(
              children: [
                Icon(
                  connected
                      ? Icons.bluetooth_connected
                      : Icons.bluetooth_disabled,
                  color: connected ? Colors.green.shade700 : Colors.orange.shade800,
                  size: 20,
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(
                    connected
                        ? '${ble.connectedName} — siap ubah setelan'
                        : 'ESP32 belum konek — setelan app tetap bisa disimpan',
                    style: TextStyle(
                      fontSize: 13,
                      color: connected
                          ? Colors.green.shade800
                          : Colors.orange.shade900,
                    ),
                  ),
                ),
                TextButton(
                  onPressed: () {
                    Navigator.of(context).push(
                      MaterialPageRoute(
                          builder: (_) => const DeviceScanScreen()),
                    );
                  },
                  child: Text(connected ? 'Ganti' : 'Hubungkan'),
                ),
                if (connected)
                  TextButton(
                    onPressed: () async {
                      if (!await confirmBleDisconnect(
                          context, ble.connectedName)) {
                        return;
                      }
                      await context
                          .read<BleService>()
                          .disconnect(forget: true);
                    },
                    style: TextButton.styleFrom(
                        foregroundColor: AppTheme.danger),
                    child: const Text('Lepas'),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 14),

          _section(
            icon: Icons.account_circle_outlined,
            title: 'Akun BLK',
            subtitle: settings.authEmail.isNotEmpty
                ? settings.authEmail
                : 'Login untuk akses device & Staff',
            accent: const Color(0xFF00897B),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                if (settings.authName.isNotEmpty)
                  ListTile(
                    contentPadding: EdgeInsets.zero,
                    leading: PersonAvatar(name: settings.authName),
                    title: Text(settings.authName,
                        style: const TextStyle(fontWeight: FontWeight.w700)),
                    subtitle: Text([
                      if (settings.userKodeCabang.isNotEmpty)
                        'Cabang ${settings.userKodeCabang}',
                      _tokenStatusLabel(settings),
                    ].join('\n')),
                  )
                else
                  Text(
                    _tokenStatusLabel(settings),
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: _authBusy || !settings.isLoggedIn
                            ? null
                            : _refreshAuthToken,
                        icon: _authBusy
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(strokeWidth: 2),
                              )
                            : const Icon(Icons.autorenew, size: 20),
                        label: const Text('Refresh token'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.icon(
                        onPressed:
                            _authBusy || !settings.isLoggedIn ? null : _logout,
                        style: FilledButton.styleFrom(
                          backgroundColor: AppTheme.danger,
                        ),
                        icon: const Icon(Icons.logout, size: 20),
                        label: const Text('Logout'),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 14),

          // ── API App ──
          _section(
            icon: Icons.phone_android,
            title: 'API App (HP)',
            subtitle: 'Untuk daftar CPMI & riwayat di aplikasi',
            child: Column(
              children: [
                TextField(
                  controller: _appApiUrl,
                  keyboardType: TextInputType.url,
                  decoration: _fieldDeco(
                    label: 'Base URL',
                    helper: 'Contoh: http://192.168.1.15:3004',
                    prefix: const Icon(Icons.link),
                  ),
                ),
                const SizedBox(height: 10),
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.tonalIcon(
                        onPressed: _saveAppApi,
                        icon: const Icon(Icons.save_outlined),
                        label: const Text('Simpan'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton.icon(
                        onPressed: connected ? _copyDeviceUrlToApp : null,
                        icon: const Icon(Icons.copy_all_outlined),
                        label: const Text('Dari device'),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),

          // ── Device ESP32 ──
          _section(
            icon: Icons.memory,
            title: 'Device ESP32',
            subtitle: connected
                ? 'Disimpan ke ESP32 via BLE'
                : 'Hubungkan BLE untuk kirim ke device',
            child: Column(
              children: [
                TextField(
                  controller: _deviceApiUrl,
                  enabled: connected,
                  keyboardType: TextInputType.url,
                  decoration: _fieldDeco(
                    label: 'API Base URL (device)',
                    helper: 'https:// otomatis jadi http:// di ESP32',
                    prefix: const Icon(Icons.dns_outlined),
                  ),
                ),
                const SizedBox(height: 12),
                DropdownButtonFormField<String>(
                  // ignore: deprecated_member_use
                  value: _kodeCabang,
                  isExpanded: true,
                  decoration: _fieldDeco(
                    label: 'Cabang',
                    prefix: const Icon(Icons.business_outlined),
                    suffix: _loadingBranches
                        ? const Padding(
                            padding: EdgeInsets.all(12),
                            child: SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            ),
                          )
                        : IconButton(
                            tooltip: 'Muat ulang cabang',
                            icon: const Icon(Icons.refresh, size: 20),
                            onPressed: _loadingBranches ? null : _loadBranches,
                          ),
                  ),
                  hint: const Text('Pilih cabang'),
                  items: _branches
                      .map((b) => DropdownMenuItem(
                            value: b.kode,
                            child: Text(
                              b.kota.isEmpty ? b.label : '${b.nama} (${b.kode})',
                              overflow: TextOverflow.ellipsis,
                            ),
                          ))
                      .toList(),
                  onChanged: connected
                      ? (v) => setState(() => _kodeCabang = v)
                      : null,
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _deviceId,
                  enabled: connected,
                  decoration: _fieldDeco(
                    label: 'Device ID',
                    prefix: const Icon(Icons.devices_outlined),
                  ),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _apiKey,
                  enabled: connected,
                  obscureText: _obscureKey,
                  decoration: _fieldDeco(
                    label: 'Device Key',
                    prefix: const Icon(Icons.key_outlined),
                    suffix: IconButton(
                      icon: Icon(
                          _obscureKey ? Icons.visibility_off : Icons.visibility),
                      onPressed: () =>
                          setState(() => _obscureKey = !_obscureKey),
                    ),
                  ),
                ),
                const SizedBox(height: 12),
                DropdownButtonFormField<int>(
                  // ignore: deprecated_member_use
                  value: _uploadInterval,
                  isExpanded: true,
                  decoration: _fieldDeco(
                    label: 'Upload otomatis',
                    helper: 'Kirim pending enroll/absensi ke server',
                    prefix: const Icon(Icons.schedule_outlined),
                  ),
                  items: intervalValues
                      .map((m) => DropdownMenuItem(
                            value: m,
                            child: Text(_intervalLabel(m)),
                          ))
                      .toList(),
                  onChanged: connected
                      ? (v) {
                          if (v != null) setState(() => _uploadInterval = v);
                        }
                      : null,
                ),
                const SizedBox(height: 4),
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  value: _irEnabled,
                  onChanged:
                      connected ? (v) => setState(() => _irEnabled = v) : null,
                  title: const Text('Gate sentuh / IR'),
                  subtitle: const Text('Sensor hanya aktif saat ada jari'),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Expanded(
                      child: FilledButton.tonalIcon(
                        onPressed: connected
                            ? () async {
                                try {
                                  await ble.syncNow();
                                  if (mounted) {
                                    setState(() =>
                                        _statusMsg = 'Sync manual dikirim');
                                  }
                                } catch (e) {
                                  if (mounted) {
                                    setState(() => _statusMsg =
                                        'Gagal sync: ${e.toString().replaceAll('Exception: ', '')}');
                                  }
                                }
                              }
                            : null,
                        icon: const Icon(Icons.sync),
                        label: const Text('Sync sekarang'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: FilledButton.icon(
                        onPressed: _loading ? null : _saveAll,
                        icon: _loading
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child:
                                    CircularProgressIndicator(strokeWidth: 2),
                              )
                            : const Icon(Icons.save),
                        label: const Text('Simpan'),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                OutlinedButton.icon(
                  onPressed: _loading ? null : _loginAndPickDevice,
                  icon: const Icon(Icons.badge_outlined),
                  label: const Text('Pilih Device BLK (auto-isi)'),
                ),
                const SizedBox(height: 8),
                OutlinedButton.icon(
                  onPressed: _loading || !connected ? null : _cleanFingers,
                  style: OutlinedButton.styleFrom(
                    foregroundColor: Colors.red.shade700,
                    side: BorderSide(color: Colors.red.shade300),
                  ),
                  icon: const Icon(Icons.delete_sweep_outlined),
                  label: const Text('Bersihkan fingerprint ESP32'),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),

          // ── WiFi ──
          _section(
            icon: Icons.wifi,
            title: 'WiFi Device',
            subtitle: 'Scan dari antena ESP32, pilih jaringan, kirim password',
            accent: Colors.teal,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                FilledButton.tonalIcon(
                  onPressed: connected && !_wifiScanning ? _scanWifi : null,
                  icon: _wifiScanning
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.wifi_find),
                  label: Text(
                      _wifiScanning ? 'Scanning...' : 'Scan WiFi dari ESP32'),
                ),
                if (_wifiAps.isNotEmpty) ...[
                  const SizedBox(height: 10),
                  Container(
                    constraints: const BoxConstraints(maxHeight: 220),
                    decoration: BoxDecoration(
                      border: Border.all(color: Colors.grey.shade200),
                      borderRadius: BorderRadius.circular(12),
                    ),
                    child: ListView.separated(
                      shrinkWrap: true,
                      itemCount: _wifiAps.length,
                      separatorBuilder: (_, _) =>
                          Divider(height: 1, color: Colors.grey.shade200),
                      itemBuilder: (context, i) {
                        final ap = _wifiAps[i];
                        final selected = ap.ssid == _wifiSsid.text;
                        return ListTile(
                          dense: true,
                          selected: selected,
                          selectedTileColor:
                              const Color(0xFF1E88E5).withValues(alpha: 0.08),
                          leading: Icon(
                            ap.enc ? Icons.lock_outline : Icons.lock_open,
                            size: 20,
                            color: selected
                                ? const Color(0xFF1E88E5)
                                : Colors.grey,
                          ),
                          title: Text(ap.ssid),
                          subtitle: Text('${_bars(ap.rssi)} · ${ap.rssi} dBm'),
                          trailing: selected
                              ? const Icon(Icons.check_circle,
                                  color: Color(0xFF1E88E5), size: 20)
                              : null,
                          onTap: () {
                            HapticFeedback.selectionClick();
                            setState(() => _wifiSsid.text = ap.ssid);
                          },
                        );
                      },
                    ),
                  ),
                ],
                const SizedBox(height: 12),
                TextField(
                  controller: _wifiSsid,
                  decoration: _fieldDeco(
                    label: 'SSID',
                    prefix: const Icon(Icons.wifi),
                  ),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _wifiPass,
                  obscureText: _obscureWifi,
                  decoration: _fieldDeco(
                    label: 'Password WiFi',
                    prefix: const Icon(Icons.lock_outline),
                    suffix: IconButton(
                      icon: Icon(_obscureWifi
                          ? Icons.visibility_off
                          : Icons.visibility),
                      onPressed: () =>
                          setState(() => _obscureWifi = !_obscureWifi),
                    ),
                  ),
                ),
                const SizedBox(height: 10),
                FilledButton.icon(
                  onPressed: connected ? _saveWifi : null,
                  icon: const Icon(Icons.save_alt),
                  label: const Text('Kirim WiFi ke Device'),
                ),
              ],
            ),
          ),

          if (_statusMsg.isNotEmpty) ...[
            const SizedBox(height: 14),
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: const Color(0xFFE3F2FD),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Text(
                _statusMsg,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: Color(0xFF1565C0),
                  fontSize: 13,
                  fontWeight: FontWeight.w500,
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
