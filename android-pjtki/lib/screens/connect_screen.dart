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
        const SnackBar(content: Text('API Base URL dan Access Token wajib diisi')),
      );
      return;
    }
    _enter();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  const Icon(Icons.fingerprint, size: 72, color: Color(0xFF1E88E5)),
                  const SizedBox(height: 8),
                  const Text(
                    'PJTKI Absensi',
                    textAlign: TextAlign.center,
                    style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 4),
                  const Text(
                    'Koneksi ke server pakai access token (X-Device-Key)',
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.grey),
                  ),
                  const SizedBox(height: 32),
                  TextField(
                    controller: _apiUrl,
                    keyboardType: TextInputType.url,
                    decoration: const InputDecoration(
                      labelText: 'API Base URL',
                      helperText: 'Contoh: http://192.168.1.15:3004',
                      prefixIcon: Icon(Icons.dns_outlined),
                      border: OutlineInputBorder(),
                    ),
                  ),
                  const SizedBox(height: 16),
                  TextField(
                    controller: _apiKey,
                    obscureText: _obscure,
                    decoration: InputDecoration(
                      labelText: 'Access Token (X-Device-Key)',
                      prefixIcon: const Icon(Icons.key),
                      border: const OutlineInputBorder(),
                      suffixIcon: IconButton(
                        icon: Icon(_obscure ? Icons.visibility_off : Icons.visibility),
                        onPressed: () => setState(() => _obscure = !_obscure),
                      ),
                    ),
                  ),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Expanded(
                        child: TextField(
                          controller: _kodeCabang,
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
                  const SizedBox(height: 24),
                  FilledButton(
                    onPressed: _save,
                    style: FilledButton.styleFrom(
                      padding: const EdgeInsets.symmetric(vertical: 16),
                    ),
                    child: const Text('Simpan & Masuk'),
                  ),
                  const SizedBox(height: 12),
                  TextButton(
                    onPressed: _enter,
                    child: const Text('Lewati — masuk tanpa server'),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
