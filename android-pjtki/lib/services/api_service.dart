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
