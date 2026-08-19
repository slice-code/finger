import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:dio/dio.dart';
import 'package:pointycastle/api.dart' as pc;
import 'package:pointycastle/block/aes.dart';
import 'package:pointycastle/block/modes/ecb.dart';
import 'package:pointycastle/paddings/pkcs7.dart';

import '../config/app_config.dart';
import '../models/models.dart';
import 'settings_store.dart';

/// Enkripsi payload ala server pjtki-bio (api/fingerprint.js):
///  - AES-256-ECB, key = SHA256(secret), padding PKCS7 → base64
///  - signature = HMAC-SHA256(encryptedPayload, secret) → base64
class FingerprintCrypto {
  final String secret;

  FingerprintCrypto(this.secret);

  static Uint8List _sha256(List<int> data) =>
      Uint8List.fromList(crypto.sha256.convert(data).bytes);

  /// Encrypt plaintext string → base64 AES-256-ECB(PKCS7)
  String encryptPayload(String plaintext) {
    final key = pc.KeyParameter(_sha256(utf8.encode(secret)));
    final cipher = ECBBlockCipher(AESEngine())..init(true, key);
    final data = Uint8List.fromList(utf8.encode(plaintext));
    final blockSize = cipher.blockSize;
    final paddedLen = blockSize * ((data.length ~/ blockSize) + 1);
    final padded = Uint8List(paddedLen);
    padded.setRange(0, data.length, data);
    PKCS7Padding().addPadding(padded, data.length);
    final out = Uint8List(paddedLen);
    var off = 0;
    final block = Uint8List(blockSize);
    while (off < paddedLen) {
      cipher.processBlock(padded, off, block, 0);
      out.setRange(off, off + block.length, block);
      off += blockSize;
    }
    return base64Encode(out);
  }

  /// signature HMAC-SHA256 base64
  String sign(String encryptedBase64) {
    final hmac = crypto.Hmac(crypto.sha256, utf8.encode(secret));
    return base64Encode(hmac.convert(utf8.encode(encryptedBase64)).bytes);
  }
}

class ApiService {
  final SettingsStore settings;
  final Dio _dio = Dio(BaseOptions(
    connectTimeout: const Duration(seconds: 8),
    receiveTimeout: const Duration(seconds: 15),
    headers: {'Content-Type': 'application/json'},
  ));

  ApiService(this.settings);

  FingerprintCrypto get _crypto => FingerprintCrypto(kFingerprintSecretKey);

  Map<String, String> _headers() {
    final h = <String, String>{'Content-Type': 'application/json'};
    if (settings.apiKey.isNotEmpty) {
      h['X-Device-Key'] = settings.apiKey;
    }
    return h;
  }

  String _url(String path) => '${settings.apiBaseUrl}$path';

  /// Header dengan Authorization Bearer (jika login user BLK) + X-Device-Key.
  Map<String, String> _authHeaders() {
    final h = _headers();
    if (settings.authToken.isNotEmpty) {
      h['Authorization'] = 'Bearer ${settings.authToken}';
    }
    return h;
  }

  /// Login user BLK (email+password) → simpan token JWT.
  Future<Map<String, dynamic>> login(String email, String password) async {
    final res = await _dio.post(
      _url('/api/auth/login'),
      data: {'email': email, 'password': password},
      options: Options(headers: _headers()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    if (body['success'] != true) {
      throw Exception(body['error'] ?? 'Login gagal');
    }
    final token = body['token']?.toString() ?? '';
    if (token.isEmpty) throw Exception('Token tidak ditemukan');
    settings.authToken = token;
    settings.authEmail = email;
    final data = Map<String, dynamic>.from(body['data'] as Map? ?? {});
    settings.authName = data['name']?.toString() ?? '';
    settings.userKodeCabang = data['kode_cabang']?.toString() ?? '';
    return data;
  }

  /// Profil user BLK saat ini (cabang dari JWT).
  Future<Map<String, dynamic>> fetchAuthMe() async {
    final body = await _staffJson(_dio.get(
      _url('/api/auth/me'),
      options: Options(headers: _authHeaders()),
    ));
    return Map<String, dynamic>.from(body['data'] as Map? ?? body);
  }

  /// Ambil daftar device key BLK (untuk user login).
  /// Return list {kode_cabang, device_id, device_key, batas_masuk, batas_pulang}.
  Future<List<Map<String, dynamic>>> fetchDevices() async {
    final res = await _dio.get(
      _url('/api/finger/devices'),
      options: Options(headers: _authHeaders()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    final list = body['data'] as List? ?? [];
    return list.map((e) => Map<String, dynamic>.from(e as Map)).toList();
  }

  Future<List<Branch>> fetchBranches() async {
    final res = await _dio.get(
      _url('/api/finger/branches'),
      options: Options(headers: _headers()),
    );
    final body = res.data;
    final list = body['data'] as List? ?? [];
    return list.map((e) => Branch.fromJson(Map<String, dynamic>.from(e))).toList();
  }

  Future<List<Employee>> fetchEmployees({String? kodeCabang}) async {
    final res = await _dio.get(
      _url('/api/finger/employees'),
      queryParameters: kodeCabang == null || kodeCabang.isEmpty
          ? null
          : {'kode_cabang': kodeCabang},
      options: Options(headers: _headers()),
    );
    final list = res.data as List? ?? [];
    return list.map((e) => Employee.fromJson(Map<String, dynamic>.from(e))).toList();
  }

  Future<({List<AttendanceRecord> rows, int total})> fetchAttendance({
    int page = 1,
    int perPage = 50,
    String? tanggal,
    String? status,
    String? search,
  }) async {
    if (settings.apiBaseUrl.trim().isEmpty) {
      throw Exception('API server belum dikonfigurasi');
    }

    final payload = <String, dynamic>{
      'page': page,
      'perPage': perPage,
    };
    if (tanggal != null && tanggal.isNotEmpty) payload['tanggal'] = tanggal;
    if (status != null && status.isNotEmpty) payload['status'] = status;
    if (search != null && search.isNotEmpty) payload['search'] = search;
    if (settings.deviceId.isNotEmpty) payload['device_id'] = settings.deviceId;
    if (settings.kodeCabang.isNotEmpty) payload['kode_cabang'] = settings.kodeCabang;

    final encryptedPayload = _crypto.encryptPayload(jsonEncode(payload));
    final signature = _crypto.sign(encryptedPayload);

    try {
      final res = await _dio.post(
        _url('/api/finger/history'),
        data: {'encryptedPayload': encryptedPayload, 'signature': signature},
        options: Options(headers: _headers()),
      );

      final data = res.data;
      final Map<String, dynamic> body = data is Map ? Map<String, dynamic>.from(data) : {};
      final dynamic rawList = body['data'] ?? body['rows'] ?? data;
      final list = rawList is List ? rawList : const <dynamic>[];

      if (body.containsKey('success') && body['success'] != true) {
        throw Exception(body['error'] ?? 'Gagal memuat riwayat');
      }

      final rows = list
          .map((e) => AttendanceRecord.fromJson(Map<String, dynamic>.from(e)))
          .toList();

      return (
        rows: rows,
        total: (body['total'] as num?)?.toInt() ?? rows.length,
      );
    } on DioException catch (e) {
      final msg = e.response?.data is Map
          ? (e.response?.data['message'] ?? e.response?.data['error'])
          : null;
      throw Exception(msg ?? _dioErrorMessage(e));
    }
  }

  /// Daftar template fingerprint di server (yang punya hex, siap sync ke device).
  Future<List<FingerTemplate>> fetchTemplates({String? kodeCabang}) async {
    final res = await _dio.get(
      _url('/api/finger/arduino/templates'),
      queryParameters: kodeCabang == null || kodeCabang.isEmpty
          ? null
          : {'kode_cabang': kodeCabang},
      options: Options(headers: _headers()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    if (body['success'] == false) {
      throw Exception(body['message'] ?? 'Gagal mengambil template server');
    }
    final list = body['data'] as List? ?? [];
    return list
        .map((e) => FingerTemplate.fromJson(Map<String, dynamic>.from(e as Map)))
        .where((t) => t.employeeId.isNotEmpty && t.hasHex)
        .toList();
  }

  /// Unduh satu template (hex 512) dari server ke app Android.
  Future<FingerTemplate?> fetchTemplate(String employeeId) async {
    if (employeeId.isEmpty) return null;
    final res = await _dio.get(
      _url('/api/finger/arduino/template/${Uri.encodeComponent(employeeId)}'),
      options: Options(headers: _headers()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    if (body['success'] != true) {
      throw Exception(body['message'] ?? 'Template tidak ditemukan');
    }
    final data = body['data'];
    if (data is! Map) return null;
    final m = Map<String, dynamic>.from(data);
    m['employeeId'] ??= employeeId;
    final tpl = FingerTemplate.fromJson(m);
    if (tpl.hex.length != 512) {
      throw Exception('Template $employeeId tidak punya hex lengkap');
    }
    return tpl;
  }

  /// Hapus registrasi fingerprint di server (DELETE by employeeId, X-Device-Key).
  Future<void> unregisterFinger(String employeeId) async {
    if (employeeId.isEmpty) return;
    await _dio.delete(
      _url('/api/finger/arduino/template/${Uri.encodeComponent(employeeId)}'),
      options: Options(headers: _headers()),
    );
  }

  /// Upload satu absensi lokal ESP32 ke server (sama seperti jalur ESP32,
  /// memakai logika 2x/hari + batas jam di server). Pakai X-Device-Key.
  /// Return status server: checkin / checkout / ignored.
  Future<String> uploadLocalAttendance({
    required String employeeId,
    required String timeIso, // YYYY-MM-DDTHH:MM:SS (WIB)
  }) async {
    final res = await _dio.post(
      _url('/api/finger/arduino/attendance'),
      data: {
        'employeeId': employeeId,
        'device_id': settings.deviceId,
        'kode_cabang': settings.kodeCabang,
        'time': timeIso,
      },
      options: Options(headers: _headers()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    final status = body['status']?.toString() ?? 'ok';
    final msg = body['message']?.toString() ?? '';
    if (status == 'ignored') {
      throw Exception(msg.isEmpty ? 'Absensi sudah tercatat / belum waktunya' : msg);
    }
    return status;
  }

  // ── Karyawan App (staff BLK — /api/finger/app-karyawan, JWT sama seperti devices) ──

  Future<({List<AppKaryawan> rows, int total})> fetchAppKaryawanList({
    int page = 1,
    int perPage = 50,
    String? status,
    String? search,
  }) async {
    final params = <String, dynamic>{
      'page': page,
      'perPage': perPage,
    };
    if (status != null && status.isNotEmpty) params['status'] = status;
    if (search != null && search.isNotEmpty) params['search'] = search;
    // Cabang difilter server dari JWT user BLK — jangan kirim kode device.

    final body = await _staffJson(_dio.get(
      _url('/api/finger/app-karyawan'),
      queryParameters: params,
      options: Options(headers: _authHeaders()),
    ));
    final list = body['data'] as List? ?? [];
    return (
      rows: list
          .map((e) => AppKaryawan.fromJson(Map<String, dynamic>.from(e as Map)))
          .toList(),
      total: (body['total'] as num?)?.toInt() ?? list.length,
    );
  }

  Future<AppKaryawan> createAppKaryawan({
    required String nama,
    String? email,
    String? password,
    bool hasAppAccess = true,
    String? kodeKaryawan,
    String? phone,
    String? kodeCabang,
    String? jabatan,
    String? departemen,
  }) async {
    final body = await _staffJson(_dio.post(
      _url('/api/finger/app-karyawan'),
      data: {
        'nama': nama,
        'has_app_access': hasAppAccess ? 'yes' : 'no',
        'status': 'active',
        if (email != null && email.isNotEmpty) 'email': email,
        if (password != null && password.isNotEmpty) 'password': password,
        if (kodeKaryawan != null && kodeKaryawan.isNotEmpty)
          'kode_karyawan': kodeKaryawan,
        if (phone != null && phone.isNotEmpty) 'phone': phone,
        if (kodeCabang != null && kodeCabang.isNotEmpty)
          'kode_cabang': kodeCabang,
        if (jabatan != null && jabatan.isNotEmpty) 'jabatan': jabatan,
        if (departemen != null && departemen.isNotEmpty)
          'departemen': departemen,
      },
      options: Options(headers: _authHeaders()),
    ));
    return AppKaryawan.fromJson(
      Map<String, dynamic>.from(body['data'] as Map? ?? {}),
    );
  }

  Future<AppKaryawan> updateAppKaryawan(
    int id, {
    String? nama,
    String? email,
    String? password,
    String? phone,
    String? kodeCabang,
    String? jabatan,
    String? departemen,
    String? kodeKaryawan,
  }) async {
    final data = <String, dynamic>{};
    if (nama != null) data['nama'] = nama;
    if (email != null) data['email'] = email;
    if (password != null && password.isNotEmpty) data['password'] = password;
    if (phone != null) data['phone'] = phone;
    if (kodeCabang != null) data['kode_cabang'] = kodeCabang;
    if (jabatan != null) data['jabatan'] = jabatan;
    if (departemen != null) data['departemen'] = departemen;
    if (kodeKaryawan != null) data['kode_karyawan'] = kodeKaryawan;

    final body = await _staffJson(_dio.patch(
      _url('/api/finger/app-karyawan/$id'),
      data: data,
      options: Options(headers: _authHeaders()),
    ));
    return AppKaryawan.fromJson(
      Map<String, dynamic>.from(body['data'] as Map? ?? {}),
    );
  }

  Future<AppKaryawan> setAppKaryawanStatus(int id, String status) async {
    final body = await _staffJson(_dio.patch(
      _url('/api/finger/app-karyawan/$id'),
      data: {'status': status},
      options: Options(headers: _authHeaders()),
    ));
    return AppKaryawan.fromJson(
      Map<String, dynamic>.from(body['data'] as Map? ?? {}),
    );
  }

  Future<({List<KaryawanAbsensi> rows, int total})> fetchKaryawanAbsensiList({
    int page = 1,
    int perPage = 50,
    int? karyawanId,
    String? tanggal,
    String? status,
    String? search,
  }) async {
    final params = <String, dynamic>{
      'page': page,
      'perPage': perPage,
    };
    if (karyawanId != null && karyawanId > 0) {
      params['karyawan_id'] = karyawanId;
    }
    if (tanggal != null && tanggal.isNotEmpty) params['tanggal'] = tanggal;
    if (status != null && status.isNotEmpty) params['status'] = status;
    if (search != null && search.isNotEmpty) params['search'] = search;

    final body = await _staffJson(_dio.get(
      _url('/api/finger/app-karyawan/absensi'),
      queryParameters: params,
      options: Options(headers: _authHeaders()),
    ));
    final list = body['data'] as List? ?? [];
    return (
      rows: list
          .map((e) =>
              KaryawanAbsensi.fromJson(Map<String, dynamic>.from(e as Map)))
          .toList(),
      total: (body['total'] as num?)?.toInt() ?? list.length,
    );
  }

  Future<KaryawanAbsensi> createKaryawanAbsensi({
    required int karyawanId,
    required String nama,
    required String tanggal,
    String status = 'hadir',
    String? jamMasuk,
    String? jamPulang,
    String? keterangan,
    String? kodeCabang,
    String metodeAbsen = 'manual',
  }) async {
    final body = await _staffJson(_dio.post(
      _url('/api/finger/app-karyawan/absensi'),
      data: {
        'karyawan_id': karyawanId,
        'nama': nama,
        'tanggal': tanggal,
        'status': status,
        if (jamMasuk != null && jamMasuk.isNotEmpty) 'jam_masuk': jamMasuk,
        if (jamPulang != null && jamPulang.isNotEmpty) 'jam_pulang': jamPulang,
        if (keterangan != null && keterangan.isNotEmpty)
          'keterangan': keterangan,
        if (kodeCabang != null && kodeCabang.isNotEmpty)
          'kode_cabang': kodeCabang,
        'metode_absen': metodeAbsen,
      },
      options: Options(headers: _authHeaders()),
    ));
    return KaryawanAbsensi.fromJson(
      Map<String, dynamic>.from(body['data'] as Map? ?? {}),
    );
  }

  Map<String, dynamic> _mapBody(dynamic data) =>
      data is Map ? Map<String, dynamic>.from(data) : <String, dynamic>{};

  void _ensureSuccess(Map<String, dynamic> body) {
    if (body.containsKey('success') && body['success'] != true) {
      throw Exception(body['error'] ?? body['message'] ?? 'Permintaan gagal');
    }
  }

  String _extractServerError(DioException e) {
    final data = e.response?.data;
    if (data is Map) {
      final err = data['error'] ?? data['message'];
      if (err != null && err.toString().isNotEmpty) {
        return err.toString();
      }
    }
    return _dioErrorMessage(e);
  }

  Future<Map<String, dynamic>> _staffJson(Future<Response<dynamic>> req) async {
    try {
      final res = await req;
      final body = _mapBody(res.data);
      _ensureSuccess(body);
      return body;
    } on DioException catch (e) {
      throw Exception(_extractServerError(e));
    }
  }

  /// Upload satu template fingerprint (hasil enroll) ke server.
  /// Data dari ESP32 via BLE (hex 512 char) — dikirim lewat app Android.
  Future<void> uploadEnroll({
    required String employeeId,
    required int fingerId,
    required String templateHex,
  }) async {
    final res = await _dio.post(
      _url('/api/finger/arduino/register'),
      data: {
        'employeeId': employeeId,
        'device_id': settings.deviceId,
        'kode_cabang': settings.kodeCabang,
        'finger_id': fingerId,
        'templateHex': templateHex,
      },
      options: Options(headers: _headers()),
    );
    final body = res.data is Map
        ? Map<String, dynamic>.from(res.data as Map)
        : <String, dynamic>{};
    if (body['status'] == 'used_by_other') {
      throw Exception('Template sudah dipakai CPMI lain');
    }
    if (body['status'] != 'ok' && body['status'] != 'updated') {
      throw Exception(body['message'] ?? 'Gagal upload template');
    }
  }

  String _dioErrorMessage(DioException e) {
    if (e.type == DioExceptionType.connectionTimeout) {
      return 'Koneksi ke server timeout';
    }
    if (e.type == DioExceptionType.connectionError) {
      return 'Server tidak dapat dijangkau';
    }
    if (e.type == DioExceptionType.receiveTimeout) {
      return 'Server lama merespon';
    }
    if (e.response != null) {
      final code = e.response?.statusCode;
      if (code == 404) return 'Endpoint riwayat tidak ditemukan';
      if (code == 401) return 'Token tidak valid';
      if (code == 403) return 'Akses ke riwayat ditolak';
      return 'Server mengembalikan error $code';
    }
    return e.message ?? 'DioException saat memuat riwayat';
  }
}
