import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';

class EnrollScreen extends StatefulWidget {
  final Employee employee;
  const EnrollScreen({super.key, required this.employee});

  @override
  State<EnrollScreen> createState() => _EnrollScreenState();
}

class _EnrollScreenState extends State<EnrollScreen> {
  int _step = 0;
  double _progress = 0;
  String _statusText = 'Mengirim perintah enroll...';
  bool _done = false;
  bool _error = false;
  bool _cancelling = false;
  StreamSubscription? _sub;
  Timer? _timeout;

  static const _steps = [
    'Mengirim perintah enroll...',
    'Letakkan jari di sensor',
    'Scan ke-1 berhasil — angkat jari',
    'Letakkan jari lagi (posisi sama)',
    'Mencocokkan pola...',
  ];

  @override
  void initState() {
    super.initState();
    _sub = context.read<BleService>().eventStream.listen(_onEvent);
    _sendEnroll();
  }

  Future<void> _sendEnroll() async {
    try {
      await context.read<BleService>().enroll(widget.employee.id, widget.employee.nama);
      _armTimeout();
    } catch (e) {
      _fail('Gagal mengirim perintah: ${e.toString().replaceAll('Exception: ', '')}');
    }
  }

  void _armTimeout() {
    _timeout?.cancel();
    // ESP menunggu hingga ~3 menit per tahap — timeout app sedikit lebih longgar.
    _timeout = Timer(const Duration(minutes: 4), () {
      if (!_done && !_error) {
        setState(() {
          _error = true;
          _statusText = 'Timeout — tidak ada aktivitas sensor';
        });
      }
    });
  }

  void _onEvent(BleEvent ev) {
    if (!mounted || _done) return;
    if (_error && ev.type != 'enroll_cancelled') return;
    setState(() {
      switch (ev.type) {
        case 'enroll_queued':
          _statusText = 'Perintah diterima device — siapkan jari...';
          _progress = 8;
          break;
        case 'enroll_start':
          _step = 0;
          _progress = 5;
          _statusText = 'Mode enroll aktif — LCD & lampu sensor menyala';
          break;
        case 'waiting_finger':
          _step = 1;
          _progress = 15;
          _statusText = _steps[1];
          break;
        case 'image_ok_step1':
          _step = 2;
          _progress = 40;
          _statusText = _steps[2];
          break;
        case 'remove':
          _step = 2;
          _progress = 50;
          _statusText = _steps[2];
          break;
        case 'waiting_finger_2':
          _step = 3;
          _progress = 60;
          _statusText = _steps[3];
          break;
        case 'image_ok_step2':
          _step = 4;
          _progress = 80;
          _statusText = _steps[4];
          break;
        case 'retry_create':
          _step = 1;
          _progress = 30;
          _statusText = 'Model gagal (percobaan ${ev.data['attempt'] ?? '?'}/3) — coba lagi';
          break;
        case 'bad_image':
          _step = 1;
          _progress = 10;
          _statusText = 'Gambar buruk, letakkan jari lebih rapat';
          break;
        case 'already_registered':
          _error = true;
          _cancelling = false;
          _statusText = 'Sidik jari sudah terdaftar (ID ${ev.data['id'] ?? '?'})';
          break;
        case 'enrolled':
          _done = true;
          _cancelling = false;
          _progress = 100;
          _statusText = 'Berhasil terdaftar (ID ${ev.data['id'] ?? '?'})';
          _timeout?.cancel();
          Timer(const Duration(seconds: 2), () {
            if (mounted) Navigator.of(context).pop(true);
          });
          break;
        case 'enroll_cancelled':
          _error = true;
          _cancelling = false;
          _statusText = 'Enroll dibatalkan';
          _timeout?.cancel();
          Timer(const Duration(milliseconds: 800), () {
            if (mounted) Navigator.of(context).pop(false);
          });
          break;
        case 'enroll_cancel_queued':
          _statusText = 'Membatalkan enroll di device...';
          break;
        case 'enroll_fail':
          final reason = ev.data['reason']?.toString();
          final code = ev.data['code'];
          _cancelling = false;
          _fail(reason != null && reason.isNotEmpty
              ? 'Enroll gagal: $reason${code != null ? ' ($code)' : ''}'
              : 'Enroll gagal (code ${code ?? '?'})');
          break;
        case 'register_server':
          if (ev.data['ok'] == true) {
            _statusText = 'Berhasil terdaftar + tersinkron ke server';
          }
          break;
      }
    });
  }

  void _fail(String msg) {
    setState(() {
      _error = true;
      _statusText = msg;
    });
  }

  Future<void> _cancel() async {
    if (_done || _cancelling) return;
    setState(() {
      _cancelling = true;
      _statusText = 'Membatalkan...';
    });
    try {
      await context.read<BleService>().cancelEnroll();
    } catch (e) {
      if (mounted) {
        setState(() {
          _cancelling = false;
          _statusText = 'Gagal batalkan: ${e.toString().replaceAll('Exception: ', '')}';
        });
      }
      return;
    }
    // Fallback jika device tidak balas event (mis. belum mulai enroll).
    Timer(const Duration(seconds: 3), () {
      if (mounted && !_done && _cancelling) {
        Navigator.of(context).pop(false);
      }
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    _timeout?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final emp = widget.employee;
    final busy = !_done && !_error;
    return PopScope(
      canPop: !busy || _cancelling,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop && busy && !_cancelling) _cancel();
      },
      child: Scaffold(
        appBar: AppBar(
          title: const Text('Enroll Fingerprint'),
          automaticallyImplyLeading: !busy,
        ),
        body: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Card(
                child: ListTile(
                  leading: const Icon(Icons.person, size: 36, color: Color(0xFF1E88E5)),
                  title: Text(emp.nama,
                      style: const TextStyle(fontWeight: FontWeight.bold)),
                  subtitle: Text(emp.id),
                ),
              ),
              const SizedBox(height: 32),
              Icon(
                _done
                    ? Icons.check_circle
                    : _error
                        ? Icons.error
                        : Icons.fingerprint,
                size: 80,
                color: _done
                    ? Colors.green
                    : _error
                        ? Colors.red
                        : const Color(0xFF1E88E5),
              ),
              const SizedBox(height: 16),
              Text(
                _statusText,
                textAlign: TextAlign.center,
                style: TextStyle(
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                  color: _error ? Colors.red : null,
                ),
              ),
              const SizedBox(height: 24),
              LinearProgressIndicator(value: _progress / 100, minHeight: 8),
              const SizedBox(height: 12),
              Text(
                _step < 2 && !_done && !_error
                    ? 'Letakkan satu jari pada sensor FPM10A, tekan pelan.\nLampu sensor & LCD tetap menyala sampai 2x scan selesai.'
                    : _step == 2 && !_done && !_error
                        ? 'Angkat jari, lalu tempel lagi di posisi yang sama.'
                        : _step == 4 && !_done && !_error
                            ? 'Tahan jari — jangan diangkat sampai selesai.'
                            : '',
                textAlign: TextAlign.center,
                style: const TextStyle(color: Colors.grey, fontSize: 13),
              ),
              const Spacer(),
              if (busy)
                FilledButton.icon(
                  onPressed: _cancelling ? null : _cancel,
                  style: FilledButton.styleFrom(
                    backgroundColor: Colors.red.shade600,
                    foregroundColor: Colors.white,
                  ),
                  icon: _cancelling
                      ? const SizedBox(
                          width: 18,
                          height: 18,
                          child: CircularProgressIndicator(
                            strokeWidth: 2,
                            color: Colors.white,
                          ),
                        )
                      : const Icon(Icons.cancel),
                  label: Text(_cancelling ? 'Membatalkan...' : 'Batalkan Enroll'),
                )
              else
                FilledButton.tonal(
                  onPressed: () => Navigator.of(context).pop(_done),
                  child: const Text('Tutup'),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
