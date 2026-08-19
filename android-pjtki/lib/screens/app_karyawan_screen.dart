import 'package:flutter/material.dart';
import 'package:intl/intl.dart';
import 'package:provider/provider.dart';

import '../models/models.dart';
import '../services/api_service.dart';
import '../services/settings_store.dart';
import '../theme/app_theme.dart';

/// Manajemen akun karyawan app (enroll + absensi) — staff BLK via JWT.
class AppKaryawanScreen extends StatefulWidget {
  const AppKaryawanScreen({super.key});

  @override
  State<AppKaryawanScreen> createState() => _AppKaryawanScreenState();
}

class _AppKaryawanScreenState extends State<AppKaryawanScreen>
    with SingleTickerProviderStateMixin {
  late final TabController _tabs;
  final _daftarKey = GlobalKey<_DaftarKaryawanTabState>();
  final _absensiKey = GlobalKey<_AbsensiKaryawanTabState>();

  @override
  void initState() {
    super.initState();
    _tabs = TabController(length: 2, vsync: this);
    _tabs.addListener(() {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _tabs.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final settings = context.watch<SettingsStore>();
    final cabangKode = settings.userKodeCabang;
    return Scaffold(
      appBar: AppBar(
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('Karyawan App'),
            if (cabangKode.isNotEmpty)
              Text(
                'Cabang $cabangKode',
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
              ),
          ],
        ),
        bottom: TabBar(
          controller: _tabs,
          tabs: const [
            Tab(text: 'Daftar', icon: Icon(Icons.person_add_outlined, size: 20)),
            Tab(text: 'Absensi', icon: Icon(Icons.fact_check_outlined, size: 20)),
          ],
        ),
      ),
      body: TabBarView(
        controller: _tabs,
        children: [
          _DaftarKaryawanTab(key: _daftarKey),
          _AbsensiKaryawanTab(key: _absensiKey),
        ],
      ),
      floatingActionButton: _tabs.index == 0
          ? FloatingActionButton.extended(
              onPressed: () => _daftarKey.currentState?._openEnroll(),
              icon: const Icon(Icons.person_add),
              label: const Text('Enroll'),
            )
          : FloatingActionButton.extended(
              onPressed: () => _absensiKey.currentState?._addManual(),
              icon: const Icon(Icons.add),
              label: const Text('Catat'),
            ),
    );
  }
}

class _DaftarKaryawanTab extends StatefulWidget {
  const _DaftarKaryawanTab({super.key});

  @override
  State<_DaftarKaryawanTab> createState() => _DaftarKaryawanTabState();
}

class _DaftarKaryawanTabState extends State<_DaftarKaryawanTab> {
  List<AppKaryawan> _rows = [];
  List<Branch> _branches = [];
  bool _loading = false;
  String? _error;
  String _q = '';
  String _statusFilter = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  List<AppKaryawan> get _filtered {
    if (_q.isEmpty) return _rows;
    final q = _q.toLowerCase();
    return _rows.where((k) {
      return k.nama.toLowerCase().contains(q) ||
          k.email.toLowerCase().contains(q) ||
          (k.kodeKaryawan ?? '').toLowerCase().contains(q) ||
          (k.jabatan ?? '').toLowerCase().contains(q);
    }).toList();
  }

  Future<void> _load() async {
    final settings = context.read<SettingsStore>();
    if (settings.authToken.isEmpty) {
      setState(() {
        _loading = false;
        _error = 'Belum login BLK — logout lalu masuk ulang.';
      });
      return;
    }
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final api = context.read<ApiService>();
      final settings = context.read<SettingsStore>();
      if (settings.userKodeCabang.isEmpty) {
        try {
          final me = await api.fetchAuthMe();
          final kode = me['kode_cabang']?.toString() ?? '';
          if (kode.isNotEmpty) settings.userKodeCabang = kode;
        } catch (_) {}
      }
      List<Branch> branches = [];
      try {
        branches = await api.fetchBranches();
      } catch (_) {}
      final result = await api.fetchAppKaryawanList(
        perPage: 200,
        status: _statusFilter.isEmpty ? null : _statusFilter,
      );
      if (!mounted) return;
      setState(() {
        _branches = branches;
        _rows = result.rows;
      });
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _openEnroll({AppKaryawan? edit}) async {
    final saved = await showModalBottomSheet<bool>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      builder: (ctx) => _EnrollKaryawanSheet(
        branches: _branches,
        edit: edit,
      ),
    );
    if (saved == true) await _load();
  }

  Future<void> _toggleStatus(AppKaryawan k) async {
    final next = k.isActive ? 'inactive' : 'active';
    final label = k.isActive ? 'nonaktifkan' : 'aktifkan';
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('${label[0].toUpperCase()}${label.substring(1)} akun?'),
        content: Text('${k.nama} (${k.email}) akan di-$label.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Batal')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Ya')),
        ],
      ),
    );
    if (ok != true) return;
    try {
      final api = context.read<ApiService>();
      await api.setAppKaryawanStatus(k.id, next);
      await _load();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Akun ${k.nama} di-$label')),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(e.toString().replaceAll('Exception: ', ''))),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading && _rows.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_error != null && _rows.isEmpty) {
      return AppEmptyState(
        icon: Icons.cloud_off_outlined,
        title: 'Gagal memuat data',
        subtitle: _error,
        actionLabel: 'Coba lagi',
        onAction: _load,
      );
    }

    final filtered = _filtered;
    return RefreshIndicator(
      onRefresh: _load,
      child: CustomScrollView(
        slivers: [
          SliverToBoxAdapter(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
              child: Column(
                children: [
                  TextField(
                    decoration: const InputDecoration(
                      hintText: 'Cari nama, email, jabatan…',
                      prefixIcon: Icon(Icons.search),
                    ),
                    onChanged: (v) => setState(() => _q = v.trim()),
                  ),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      FilterChip(
                        label: const Text('Semua'),
                        selected: _statusFilter.isEmpty,
                        onSelected: (_) {
                          setState(() => _statusFilter = '');
                          _load();
                        },
                      ),
                      const SizedBox(width: 6),
                      FilterChip(
                        label: const Text('Aktif'),
                        selected: _statusFilter == 'active',
                        onSelected: (_) {
                          setState(() => _statusFilter = 'active');
                          _load();
                        },
                      ),
                      const SizedBox(width: 6),
                      FilterChip(
                        label: const Text('Nonaktif'),
                        selected: _statusFilter == 'inactive',
                        onSelected: (_) {
                          setState(() => _statusFilter = 'inactive');
                          _load();
                        },
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          if (filtered.isEmpty)
            const SliverFillRemaining(
              hasScrollBody: false,
              child: AppEmptyState(
                icon: Icons.people_outline,
                title: 'Belum ada karyawan',
                subtitle:
                    'User sistem (bio, blk, …) muncul otomatis setelah sync. Tambah manual untuk satpam tanpa akses app.',
              ),
            )
          else
            SliverList(
              delegate: SliverChildBuilderDelegate(
                (context, i) {
                  final k = filtered[i];
                  return Card(
                    margin: const EdgeInsets.fromLTRB(16, 0, 16, 10),
                    child: ListTile(
                      leading: PersonAvatar(
                        name: k.nama,
                        color: k.isActive ? AppTheme.success : AppTheme.danger,
                      ),
                      title: Text(k.nama,
                          style: const TextStyle(fontWeight: FontWeight.w700)),
                      subtitle: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          if (k.hasAppAccess && k.email.isNotEmpty)
                            Text(k.email)
                          else if (k.isSatpam)
                            Text('Tanpa akses app',
                                style: TextStyle(
                                    color: Theme.of(context)
                                        .colorScheme
                                        .onSurfaceVariant)),
                          if (k.jabatan != null && k.jabatan!.isNotEmpty)
                            Text(k.jabatan!,
                                style: TextStyle(
                                    color: Theme.of(context)
                                        .colorScheme
                                        .onSurfaceVariant)),
                          if (k.isSyncedUser)
                            Text('User sistem',
                                style: TextStyle(
                                    fontSize: 12,
                                    color: Theme.of(context).colorScheme.primary)),
                          if (k.kodeCabang != null && k.kodeCabang!.isNotEmpty)
                            Text('Cabang: ${k.kodeCabang}',
                                style: const TextStyle(fontSize: 12)),
                        ],
                      ),
                      isThreeLine: true,
                      trailing: PopupMenuButton<String>(
                        onSelected: (v) {
                          if (v == 'edit' && !k.isSyncedUser) _openEnroll(edit: k);
                          if (v == 'toggle') _toggleStatus(k);
                        },
                        itemBuilder: (_) => [
                          if (!k.isSyncedUser)
                            const PopupMenuItem(value: 'edit', child: Text('Edit')),
                          PopupMenuItem(
                            value: 'toggle',
                            child: Text(k.isActive ? 'Nonaktifkan' : 'Aktifkan'),
                          ),
                        ],
                      ),
                      onTap: () {
                        if (!k.isSyncedUser) _openEnroll(edit: k);
                      },
                    ),
                  );
                },
                childCount: filtered.length,
              ),
            ),
          const SliverToBoxAdapter(child: SizedBox(height: 88)),
        ],
      ),
    );
  }
}

class _EnrollKaryawanSheet extends StatefulWidget {
  final List<Branch> branches;
  final AppKaryawan? edit;

  const _EnrollKaryawanSheet({required this.branches, this.edit});

  @override
  State<_EnrollKaryawanSheet> createState() => _EnrollKaryawanSheetState();
}

class _EnrollKaryawanSheetState extends State<_EnrollKaryawanSheet> {
  final _nama = TextEditingController();
  final _email = TextEditingController();
  final _password = TextEditingController();
  final _phone = TextEditingController();
  final _kode = TextEditingController();
  final _jabatan = TextEditingController();
  final _departemen = TextEditingController();
  final _cabangManual = TextEditingController();
  String? _cabang;
  bool _obscure = true;
  bool _saving = false;
  bool _hasAppAccess = true;
  String? _error;

  bool get _isEdit => widget.edit != null;

  @override
  void initState() {
    super.initState();
    final e = widget.edit;
    if (e != null) {
      _nama.text = e.nama;
      _email.text = e.email;
      _phone.text = e.phone ?? '';
      _kode.text = e.kodeKaryawan ?? '';
      _jabatan.text = e.jabatan ?? '';
      _departemen.text = e.departemen ?? '';
      _cabang = e.kodeCabang;
      _hasAppAccess = e.hasAppAccess;
      if (e.kodeCabang != null) _cabangManual.text = e.kodeCabang!;
    } else {
      final settings = context.read<SettingsStore>();
      if (settings.userKodeCabang.isNotEmpty) {
        _cabang = settings.userKodeCabang;
        _cabangManual.text = settings.userKodeCabang;
      }
    }
  }

  @override
  void dispose() {
    _nama.dispose();
    _email.dispose();
    _password.dispose();
    _phone.dispose();
    _kode.dispose();
    _jabatan.dispose();
    _departemen.dispose();
    _cabangManual.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final nama = _nama.text.trim();
    final email = _email.text.trim();
    final pass = _password.text;
    if (nama.isEmpty) {
      setState(() => _error = 'Nama wajib');
      return;
    }
    if (_hasAppAccess) {
      if (email.isEmpty) {
        setState(() => _error = 'Email wajib jika punya akses app');
        return;
      }
      if (!_isEdit && pass.isEmpty) {
        setState(() => _error = 'Password wajib untuk akun baru');
        return;
      }
    }
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      final api = context.read<ApiService>();
      if (_isEdit) {
        await api.updateAppKaryawan(
          widget.edit!.id,
          nama: nama,
          email: email,
          password: pass.isEmpty ? null : pass,
          phone: _phone.text.trim(),
          kodeCabang: _cabang,
          jabatan: _jabatan.text.trim(),
          departemen: _departemen.text.trim(),
          kodeKaryawan: _kode.text.trim(),
        );
      } else {
        await api.createAppKaryawan(
          nama: nama,
          email: email.isEmpty ? null : email,
          password: pass.isEmpty ? null : pass,
          hasAppAccess: _hasAppAccess,
          phone: _phone.text.trim(),
          kodeCabang: _cabang,
          jabatan: _jabatan.text.trim(),
          departemen: _departemen.text.trim(),
          kodeKaryawan: _kode.text.trim(),
        );
      }
      if (mounted) Navigator.pop(context, true);
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.viewInsetsOf(context).bottom;
    return Padding(
      padding: EdgeInsets.fromLTRB(20, 16, 20, 16 + bottom),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              _isEdit ? 'Edit Karyawan' : 'Daftar Karyawan Baru',
              style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w800),
            ),
            const SizedBox(height: 4),
            Text(
              _isEdit
                  ? 'Kosongkan password jika tidak ingin mengubah.'
                  : _hasAppAccess
                      ? 'Karyawan dengan akses app bisa login mobile.'
                      : 'Contoh satpam — hanya untuk absensi manual, tanpa login app.',
              style: TextStyle(color: Theme.of(context).colorScheme.onSurfaceVariant),
            ),
            if (!_isEdit) ...[
              const SizedBox(height: 12),
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Punya akses app'),
                subtitle: const Text('Nonaktifkan untuk satpam / tanpa login'),
                value: _hasAppAccess,
                onChanged: (v) => setState(() => _hasAppAccess = v),
              ),
            ],
            const SizedBox(height: 16),
            TextField(
              controller: _nama,
              textInputAction: TextInputAction.next,
              decoration: const InputDecoration(labelText: 'Nama *'),
            ),
            const SizedBox(height: 12),
            if (_hasAppAccess) ...[
              TextField(
                controller: _email,
                keyboardType: TextInputType.emailAddress,
                textInputAction: TextInputAction.next,
                decoration: const InputDecoration(labelText: 'Email login *'),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: _password,
                obscureText: _obscure,
                decoration: InputDecoration(
                  labelText: _isEdit ? 'Password baru' : 'Password *',
                  suffixIcon: IconButton(
                    icon: Icon(_obscure ? Icons.visibility_outlined : Icons.visibility_off_outlined),
                    onPressed: () => setState(() => _obscure = !_obscure),
                  ),
                ),
              ),
              const SizedBox(height: 12),
            ],
            TextField(
              controller: _phone,
              keyboardType: TextInputType.phone,
              decoration: const InputDecoration(labelText: 'Telepon'),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _kode,
              decoration: const InputDecoration(labelText: 'Kode karyawan'),
            ),
            const SizedBox(height: 12),
            if (widget.branches.isNotEmpty)
              DropdownButtonFormField<String>(
                value: _cabang != null &&
                        widget.branches.any((b) => b.kode == _cabang)
                    ? _cabang
                    : null,
                decoration: const InputDecoration(labelText: 'Cabang'),
                items: widget.branches
                    .map((b) => DropdownMenuItem(value: b.kode, child: Text(b.label)))
                    .toList(),
                onChanged: (v) => setState(() => _cabang = v),
              )
            else
              TextField(
                controller: _cabangManual,
                onChanged: (v) => _cabang = v.trim(),
                decoration: const InputDecoration(labelText: 'Kode cabang'),
              ),
            const SizedBox(height: 12),
            TextField(
              controller: _jabatan,
              decoration: const InputDecoration(labelText: 'Jabatan'),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _departemen,
              decoration: const InputDecoration(labelText: 'Departemen'),
            ),
            if (_error != null) ...[
              const SizedBox(height: 12),
              Text(_error!, style: const TextStyle(color: AppTheme.danger)),
            ],
            const SizedBox(height: 20),
            FilledButton(
              onPressed: _saving ? null : _save,
              child: _saving
                  ? const SizedBox(
                      width: 22,
                      height: 22,
                      child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white),
                    )
                  : Text(_isEdit ? 'Simpan' : 'Daftarkan'),
            ),
          ],
        ),
      ),
    );
  }
}

class _AbsensiKaryawanTab extends StatefulWidget {
  const _AbsensiKaryawanTab({super.key});

  @override
  State<_AbsensiKaryawanTab> createState() => _AbsensiKaryawanTabState();
}

class _AbsensiKaryawanTabState extends State<_AbsensiKaryawanTab> {
  final _scroll = ScrollController();
  List<KaryawanAbsensi> _rows = [];
  List<AppKaryawan> _karyawan = [];
  bool _loading = false;
  bool _hasMore = true;
  int _page = 1;
  String? _error;
  String _status = '';
  DateTime? _tanggal;
  String _q = '';

  @override
  void initState() {
    super.initState();
    _scroll.addListener(() {
      if (_scroll.position.pixels >= _scroll.position.maxScrollExtent - 200) {
        _loadMore();
      }
    });
    _reload();
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  Future<void> _reload() async {
    setState(() {
      _page = 1;
      _rows = [];
      _hasMore = true;
      _error = null;
    });
    try {
      final api = context.read<ApiService>();
      final kList = await api.fetchAppKaryawanList(perPage: 200);
      if (mounted) _karyawan = kList.rows;
    } catch (_) {}
    await _loadMore();
  }

  Future<void> _loadMore() async {
    if (_loading || !_hasMore) return;
    setState(() => _loading = true);
    try {
      final api = context.read<ApiService>();
      final result = await api.fetchKaryawanAbsensiList(
        page: _page,
        status: _status.isEmpty ? null : _status,
        tanggal: _tanggal == null
            ? null
            : DateFormat('yyyy-MM-dd').format(_tanggal!),
        search: _q.isEmpty ? null : _q,
      );
      if (!mounted) return;
      setState(() {
        _rows = [..._rows, ...result.rows];
        _hasMore = result.rows.isNotEmpty;
        _page++;
      });
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  Future<void> _pickDate() async {
    final picked = await showDatePicker(
      context: context,
      initialDate: _tanggal ?? DateTime.now(),
      firstDate: DateTime(2020),
      lastDate: DateTime.now().add(const Duration(days: 365)),
    );
    if (picked != null) {
      setState(() => _tanggal = picked);
      await _reload();
    }
  }

  Future<void> _addManual() async {
    if (_karyawan.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Daftar karyawan kosong — enroll dulu di tab Daftar')),
      );
      return;
    }
    final saved = await showModalBottomSheet<bool>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      builder: (ctx) => _ManualAbsensiSheet(karyawan: _karyawan),
    );
    if (saved == true) await _reload();
  }

  Color _statusColor(String s) {
    switch (s.toLowerCase()) {
      case 'hadir':
        return AppTheme.success;
      case 'izin':
      case 'sakit':
        return AppTheme.warning;
      case 'alpha':
      case 'tidak hadir':
        return AppTheme.danger;
      default:
        return AppTheme.seed;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
          child: Column(
            children: [
              TextField(
                decoration: const InputDecoration(
                  hintText: 'Cari nama…',
                  prefixIcon: Icon(Icons.search),
                ),
                onSubmitted: (v) {
                  setState(() => _q = v.trim());
                  _reload();
                },
              ),
              const SizedBox(height: 8),
              SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: Row(
                  children: [
                    ActionChip(
                      avatar: const Icon(Icons.calendar_today, size: 16),
                      label: Text(_tanggal == null
                          ? 'Semua tanggal'
                          : DateFormat('dd MMM yyyy').format(_tanggal!)),
                      onPressed: _pickDate,
                    ),
                    if (_tanggal != null)
                      IconButton(
                        tooltip: 'Hapus filter tanggal',
                        onPressed: () {
                          setState(() => _tanggal = null);
                          _reload();
                        },
                        icon: const Icon(Icons.close, size: 18),
                      ),
                    const SizedBox(width: 4),
                    for (final s in ['', 'hadir', 'izin', 'sakit', 'alpha'])
                      Padding(
                        padding: const EdgeInsets.only(right: 6),
                        child: FilterChip(
                          label: Text(s.isEmpty ? 'Semua' : s),
                          selected: _status == s,
                          onSelected: (_) {
                            setState(() => _status = s);
                            _reload();
                          },
                        ),
                      ),
                  ],
                ),
              ),
            ],
          ),
        ),
        if (_error != null && _rows.isEmpty)
          Expanded(
            child: AppEmptyState(
              icon: Icons.cloud_off_outlined,
              title: 'Gagal memuat absensi',
              subtitle: _error,
              actionLabel: 'Coba lagi',
              onAction: _reload,
            ),
          )
        else if (_rows.isEmpty && !_loading)
          const Expanded(
            child: AppEmptyState(
              icon: Icons.event_busy_outlined,
              title: 'Belum ada absensi',
              subtitle: 'Catatan absensi karyawan app akan muncul di sini.',
            ),
          )
        else
          Expanded(
            child: RefreshIndicator(
              onRefresh: _reload,
              child: ListView.builder(
                controller: _scroll,
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 88),
                itemCount: _rows.length + (_loading ? 1 : 0),
                itemBuilder: (context, i) {
                  if (i >= _rows.length) {
                    return const Padding(
                      padding: EdgeInsets.all(16),
                      child: Center(child: CircularProgressIndicator()),
                    );
                  }
                  final r = _rows[i];
                  return Card(
                    margin: const EdgeInsets.only(bottom: 10),
                    child: ListTile(
                      leading: PersonAvatar(name: r.nama),
                      title: Text(r.nama,
                          style: const TextStyle(fontWeight: FontWeight.w700)),
                      subtitle: Text(
                        '${r.tanggal}'
                        '${r.jamMasuk != null ? ' · masuk ${r.jamMasuk}' : ''}'
                        '${r.jamPulang != null ? ' · pulang ${r.jamPulang}' : ''}'
                        '${r.metodeAbsen != null ? '\n${r.metodeAbsen}' : ''}',
                      ),
                      isThreeLine: true,
                      trailing: StatusBadge(
                        label: r.status,
                        color: _statusColor(r.status),
                      ),
                    ),
                  );
                },
              ),
            ),
          ),
      ],
    );
  }
}

class _ManualAbsensiSheet extends StatefulWidget {
  final List<AppKaryawan> karyawan;

  const _ManualAbsensiSheet({required this.karyawan});

  @override
  State<_ManualAbsensiSheet> createState() => _ManualAbsensiSheetState();
}

class _ManualAbsensiSheetState extends State<_ManualAbsensiSheet> {
  AppKaryawan? _selected;
  DateTime _tanggal = DateTime.now();
  TimeOfDay? _masuk;
  TimeOfDay? _pulang;
  String _status = 'hadir';
  final _ket = TextEditingController();
  bool _saving = false;
  String? _error;

  @override
  void dispose() {
    _ket.dispose();
    super.dispose();
  }

  String _fmtTime(TimeOfDay? t) {
    if (t == null) return '';
    return '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}:00';
  }

  Future<void> _save() async {
    if (_selected == null) {
      setState(() => _error = 'Pilih karyawan');
      return;
    }
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      await context.read<ApiService>().createKaryawanAbsensi(
            karyawanId: _selected!.id,
            nama: _selected!.nama,
            kodeCabang: _selected!.kodeCabang,
            tanggal: DateFormat('yyyy-MM-dd').format(_tanggal),
            status: _status,
            jamMasuk: _fmtTime(_masuk),
            jamPulang: _fmtTime(_pulang),
            keterangan: _ket.text.trim(),
          );
      if (mounted) Navigator.pop(context, true);
    } catch (e) {
      if (mounted) {
        setState(() => _error = e.toString().replaceAll('Exception: ', ''));
      }
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.viewInsetsOf(context).bottom;
    return Padding(
      padding: EdgeInsets.fromLTRB(20, 16, 20, 16 + bottom),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text(
              'Catat Absensi Manual',
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w800),
            ),
            const SizedBox(height: 16),
            DropdownButtonFormField<AppKaryawan>(
              value: _selected,
              decoration: const InputDecoration(labelText: 'Karyawan *'),
              items: widget.karyawan
                  .map((k) => DropdownMenuItem(value: k, child: Text(k.nama)))
                  .toList(),
              onChanged: (v) => setState(() => _selected = v),
            ),
            const SizedBox(height: 12),
            ListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Tanggal'),
              subtitle: Text(DateFormat('yyyy-MM-dd').format(_tanggal)),
              trailing: const Icon(Icons.calendar_today),
              onTap: () async {
                final d = await showDatePicker(
                  context: context,
                  initialDate: _tanggal,
                  firstDate: DateTime(2020),
                  lastDate: DateTime.now().add(const Duration(days: 30)),
                );
                if (d != null) setState(() => _tanggal = d);
              },
            ),
            Row(
              children: [
                Expanded(
                  child: ListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Jam masuk'),
                    subtitle: Text(_masuk?.format(context) ?? '-'),
                    onTap: () async {
                      final t = await showTimePicker(
                        context: context,
                        initialTime: _masuk ?? TimeOfDay.now(),
                      );
                      if (t != null) setState(() => _masuk = t);
                    },
                  ),
                ),
                Expanded(
                  child: ListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Jam pulang'),
                    subtitle: Text(_pulang?.format(context) ?? '-'),
                    onTap: () async {
                      final t = await showTimePicker(
                        context: context,
                        initialTime: _pulang ?? TimeOfDay.now(),
                      );
                      if (t != null) setState(() => _pulang = t);
                    },
                  ),
                ),
              ],
            ),
            DropdownButtonFormField<String>(
              value: _status,
              decoration: const InputDecoration(labelText: 'Status'),
              items: const [
                DropdownMenuItem(value: 'hadir', child: Text('Hadir')),
                DropdownMenuItem(value: 'izin', child: Text('Izin')),
                DropdownMenuItem(value: 'sakit', child: Text('Sakit')),
                DropdownMenuItem(value: 'alpha', child: Text('Alpha')),
              ],
              onChanged: (v) => setState(() => _status = v ?? 'hadir'),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _ket,
              decoration: const InputDecoration(labelText: 'Keterangan'),
            ),
            if (_error != null) ...[
              const SizedBox(height: 12),
              Text(_error!, style: const TextStyle(color: AppTheme.danger)),
            ],
            const SizedBox(height: 20),
            FilledButton(
              onPressed: _saving ? null : _save,
              child: _saving
                  ? const SizedBox(
                      width: 22,
                      height: 22,
                      child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white),
                    )
                  : const Text('Simpan'),
            ),
          ],
        ),
      ),
    );
  }
}
