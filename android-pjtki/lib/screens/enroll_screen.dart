import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/ble_service.dart';
import '../services/enroll_store.dart';
import '../theme/app_theme.dart';

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
    _startEnroll();
  }

  /// Simpan nama + employeeId di storage Android dulu, baru kirim ke ESP32.
  Future<void> _startEnroll() async {
    final store = context.read<EnrollStore>();
    await store.savePending(widget.employee.id, widget.employee.nama);
    await _sendEnroll();
  }

  Future<void> _sendEnroll() async {
    try {
      await context.read<EnrollStore>().markEnrolling(widget.employee.id);
      await context.read<BleService>().enroll(widget.employee.id, widget.employee.nama);
      _armTimeout();
    } catch (e) {
      await context.read<EnrollStore>().markFailed(widget.employee.id);
      _fail('Gagal mengirim perintah: ${e.toString().replaceAll('Exception: ', '')}');
    }
  }

  void _armTimeout() {
    _timeout?.cancel();
    // ESP menunggu hingga ~3 menit per tahap — timeout app sedikit lebih longgar.
    _timeout = Timer(const Duration(minutes: 4), () {
      if (!_done && !_error) {
        unawaited(context.read<EnrollStore>().markFailed(widget.employee.id));
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
          unawaited(context.read<EnrollStore>().markFailed(widget.employee.id));
          break;
        case 'enrolled':
          _done = true;
          _cancelling = false;
          _progress = 95;
          final hasHex = ev.data['hex'] == true;
          final fingerId = (ev.data['id'] as num?)?.toInt() ?? 0;
          _statusText = 'Menyimpan template ke HP...';
          _timeout?.cancel();
          unawaited(_finishEnrolled(fingerId, hexReady: hasHex));
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
          unawaited(context.read<EnrollStore>().markFailed(widget.employee.id));
          _fail(reason != null && reason.isNotEmpty
              ? 'Enroll gagal: $reason${code != null ? ' ($code)' : ''}'
              : 'Enroll gagal (code ${code ?? '?'})');
          break;
        case 'register_server':
          break;
      }
    });
  }

  void _fail(String msg) {
    unawaited(context.read<EnrollStore>().markFailed(widget.employee.id));
    setState(() {
      _error = true;
      _statusText = msg;
    });
  }

  Future<void> _finishEnrolled(int fingerId, {required bool hexReady}) async {
    await _onEnrolled(fingerId, hexReady: hexReady);
    if (!mounted) return;
    await Future<void>.delayed(const Duration(milliseconds: 400));
    if (mounted) Navigator.of(context).pop(true);
  }

  /// Hex dari ESP32 sudah di-push ke characteristic — baca langsung, simpan di HP.
  /// GET_TEMPLATE hanya fallback (lambat: UART sensor).
  Future<void> _onEnrolled(int fingerId, {required bool hexReady}) async {
    final store = context.read<EnrollStore>();
    final ble = context.read<BleService>();
    String hex = '';
    if (fingerId > 0) {
      try {
        hex = await ble.readPushedEnrollHex() ?? '';
        if (hex.length != 512 && hexReady) {
          final tpl = await ble.readEnrollTemplate(fingerId);
          hex = tpl?['hex']?.toString() ?? '';
        }
      } catch (e) {
        debugPrint('[ENROLL] fetch hex failed: $e');
      }
    }
    await store.markEnrolled(
      widget.employee.id,
      fingerId,
      hex: hex.length == 512 ? hex : null,
    );
    if (!mounted) return;
    setState(() {
      _progress = 100;
      _statusText = hex.length == 512
          ? 'Berhasil (ID $fingerId) — hex tersimpan di HP'
          : 'Terdaftar (ID $fingerId) — hex belum lengkap di HP';
    });
    debugPrint('[ENROLL] local saved id=$fingerId hex=${hex.length}');
  }

  String _hintText() {
    if (_done || _error) return '';
    if (_step < 2) {
      return 'Tekan pelan satu jari di kaca sensor.\nLampu dan LCD menyala sampai 2 kali scan selesai.';
    }
    if (_step == 2) return 'Angkat jari, lalu tempel lagi di posisi yang sama.';
    if (_step == 4) return 'Tahan jari — jangan diangkat sampai selesai.';
    return '';
  }

  Widget _stepRow() {
    const labels = ['Siap', 'Scan 1', 'Angkat', 'Scan 2', 'Simpan'];
    return Row(
      children: [
        for (var i = 0; i < labels.length; i++) ...[
          if (i > 0)
            Expanded(
              child: Container(
                height: 2,
                color: (_done || _step > i)
                    ? AppTheme.success
                    : Theme.of(context).dividerColor,
              ),
            ),
          Column(
            children: [
              CircleAvatar(
                radius: 14,
                backgroundColor: (_done || _step >= i)
                    ? (_error && !_done
                        ? AppTheme.danger
                        : Theme.of(context).colorScheme.primary)
                    : Theme.of(context).colorScheme.surfaceContainerHighest,
                foregroundColor: (_done || _step >= i)
                    ? Colors.white
                    : Theme.of(context).colorScheme.onSurfaceVariant,
                child: Text('${i + 1}',
                    style: const TextStyle(
                        fontSize: 12, fontWeight: FontWeight.w800)),
              ),
              const SizedBox(height: 4),
              Text(labels[i],
                  style: const TextStyle(fontSize: 10, fontWeight: FontWeight.w600)),
            ],
          ),
        ],
      ],
    );
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
          padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Card(
                child: ListTile(
                  leading: PersonAvatar(name: emp.nama),
                  title: Text(emp.nama,
                      style: const TextStyle(fontWeight: FontWeight.w800)),
                  subtitle: Text(emp.id),
                ),
              ),
              const SizedBox(height: 28),
              _stepRow(),
              const SizedBox(height: 28),
              Icon(
                _done
                    ? Icons.check_circle_rounded
                    : _error
                        ? Icons.error_outline
                        : Icons.fingerprint,
                size: 92,
                color: _done
                    ? AppTheme.success
                    : _error
                        ? AppTheme.danger
                        : Theme.of(context).colorScheme.primary,
              ),
              const SizedBox(height: 16),
              Text(
                _statusText,
                textAlign: TextAlign.center,
                style: TextStyle(
                  fontSize: 18,
                  fontWeight: FontWeight.w800,
                  height: 1.3,
                  color: _error ? AppTheme.danger : null,
                ),
              ),
              const SizedBox(height: 20),
              ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: LinearProgressIndicator(
                  value: _progress / 100,
                  minHeight: 10,
                ),
              ),
              const SizedBox(height: 14),
              Text(
                _hintText(),
                textAlign: TextAlign.center,
                style: TextStyle(
                  color: Theme.of(context).colorScheme.onSurfaceVariant,
                  fontSize: 14,
                  height: 1.4,
                ),
              ),
              const Spacer(),
              if (busy)
                FilledButton.icon(
                  onPressed: _cancelling ? null : _cancel,
                  style: FilledButton.styleFrom(
                    backgroundColor: AppTheme.danger,
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
                      : const Icon(Icons.close),
                  label: Text(_cancelling ? 'Membatalkan…' : 'Batalkan'),
                )
              else
                FilledButton.tonal(
                  onPressed: () => Navigator.of(context).pop(_done),
                  child: const Text('Selesai'),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
