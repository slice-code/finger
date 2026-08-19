import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/settings_store.dart';
import 'home_shell.dart';

class ConnectScreen extends StatefulWidget {
  const ConnectScreen({super.key});

  @override
  State<ConnectScreen> createState() => _ConnectScreenState();
}

class _ConnectScreenState extends State<ConnectScreen> {
  final _apiUrl = TextEditingController();
  final _apiKey = TextEditingController();
  final _kodeCabang = TextEditingController();
  final _deviceId = TextEditingController();
  bool _obscure = true;

  @override
  void initState() {
    super.initState();
    final s = context.read<SettingsStore>();
    _apiUrl.text = s.apiBaseUrl;
    _apiKey.text = s.apiKey;
    _kodeCabang.text = s.kodeCabang;
    _deviceId.text = s.deviceId;
  }

  @override
  void dispose() {
    _apiUrl.dispose();
    _apiKey.dispose();
    _kodeCabang.dispose();
    _deviceId.dispose();
    super.dispose();
  }

  void _enter() {
    final s = context.read<SettingsStore>();
    s.apiBaseUrl = _apiUrl.text.trim();
    s.apiKey = _apiKey.text.trim();
    s.kodeCabang = _kodeCabang.text.trim();
    s.deviceId = _deviceId.text.trim();
    Navigator.of(context).pushReplacement(
      MaterialPageRoute(builder: (_) => const HomeShell()),
    );
  }

  void _save() {
    if (_apiUrl.text.trim().isEmpty || _apiKey.text.trim().isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
            content: Text('API Base URL dan Access Token wajib diisi')),
      );
      return;
    }
    _enter();
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Scaffold(
      body: DecoratedBox(
        decoration: BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [scheme.primary.withValues(alpha: 0.10), scheme.surface],
          ),
        ),
        child: SafeArea(
          child: Center(
            child: SingleChildScrollView(
              padding: const EdgeInsets.all(24),
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 420),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Icon(Icons.hub_outlined, size: 56, color: scheme.primary),
                    const SizedBox(height: 12),
                    const Text(
                      'Hubungkan ke server',
                      textAlign: TextAlign.center,
                      style:
                          TextStyle(fontSize: 24, fontWeight: FontWeight.w800),
                    ),
                    const SizedBox(height: 6),
                    Text(
                      'Isi alamat API dan token device. Bisa dilewati dulu, lalu diatur di Setelan.',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                          color: scheme.onSurfaceVariant, height: 1.35),
                    ),
                    const SizedBox(height: 24),
                    Card(
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(18, 20, 18, 20),
                        child: Column(
                          children: [
                            TextField(
                              controller: _apiUrl,
                              keyboardType: TextInputType.url,
                              decoration: const InputDecoration(
                                labelText: 'API Base URL',
                                helperText: 'Contoh: http://192.168.1.15:3004',
                                prefixIcon: Icon(Icons.dns_outlined),
                              ),
                            ),
                            const SizedBox(height: 14),
                            TextField(
                              controller: _apiKey,
                              obscureText: _obscure,
                              decoration: InputDecoration(
                                labelText: 'Access Token',
                                prefixIcon: const Icon(Icons.key_outlined),
                                suffixIcon: IconButton(
                                  icon: Icon(_obscure
                                      ? Icons.visibility_outlined
                                      : Icons.visibility_off_outlined),
                                  onPressed: () =>
                                      setState(() => _obscure = !_obscure),
                                ),
                              ),
                            ),
                            const SizedBox(height: 14),
                            Row(
                              children: [
                                Expanded(
                                  child: TextField(
                                    controller: _kodeCabang,
                                    decoration: const InputDecoration(
                                      labelText: 'Kode Cabang',
                                      prefixIcon: Icon(Icons.business_outlined),
                                    ),
                                  ),
                                ),
                                const SizedBox(width: 10),
                                Expanded(
                                  child: TextField(
                                    controller: _deviceId,
                                    decoration: const InputDecoration(
                                      labelText: 'Device ID',
                                      prefixIcon: Icon(Icons.memory_outlined),
                                    ),
                                  ),
                                ),
                              ],
                            ),
                            const SizedBox(height: 20),
                            FilledButton(
                              onPressed: _save,
                              child: const Text('Simpan & lanjut'),
                            ),
                            const SizedBox(height: 8),
                            TextButton(
                              onPressed: _enter,
                              child: const Text('Lewati dulu — atur nanti di Setelan'),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
