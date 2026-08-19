import 'dart:convert';

class DeviceStatus {
  final bool ready;
  final bool autoActive;
  final int count;
  final String wifiMode;
  final int temp;
  final bool sensorReady;
  final bool irEnabled;
  final int uploadIntervalMinutes;

  DeviceStatus({
    this.ready = false,
    this.autoActive = false,
    this.count = 0,
    this.wifiMode = "AP",
    this.temp = 0,
    this.sensorReady = false,
    this.irEnabled = true,
    this.uploadIntervalMinutes = 120,
  });

  factory DeviceStatus.fromJson(Map<String, dynamic> json) {
    return DeviceStatus(
      ready: json['ready'] == true,
      autoActive: json['autoActive'] == true,
      count: (json['count'] as num?)?.toInt() ?? 0,
      wifiMode: json['wifiMode']?.toString() ?? "AP",
      temp: (json['temp'] as num?)?.toInt() ?? 0,
      sensorReady: json['sensorReady'] == true,
      irEnabled: json['irEnabled'] != false,
      uploadIntervalMinutes: (json['uploadIntervalMinutes'] as num?)?.toInt() ?? 120,
    );
  }

  @override
  bool operator ==(Object other) {
    return identical(this, other) ||
        other is DeviceStatus &&
            ready == other.ready &&
            autoActive == other.autoActive &&
            count == other.count &&
            wifiMode == other.wifiMode &&
            temp == other.temp &&
            sensorReady == other.sensorReady &&
            irEnabled == other.irEnabled &&
            uploadIntervalMinutes == other.uploadIntervalMinutes;
  }

  @override
  int get hashCode => Object.hash(
        ready,
        autoActive,
        count,
        wifiMode,
        temp,
        sensorReady,
        irEnabled,
        uploadIntervalMinutes,
      );
}

class Employee {
  final String id;
  final String nama;
  final bool fingerTerdaftar;

  Employee({required this.id, required this.nama, required this.fingerTerdaftar});

  factory Employee.fromJson(Map<String, dynamic> json) {
    return Employee(
      id: json['id']?.toString() ?? '',
      nama: json['nama']?.toString() ?? '',
      fingerTerdaftar: json['finger_terdaftar'] == true,
    );
  }
}

class FingerTemplate {
  final String employeeId;
  final String nama;
  final int fingerId;
  final bool hasHex;
  final String kodeCabang;
  final String hex;

  FingerTemplate({
    required this.employeeId,
    required this.nama,
    this.fingerId = 0,
    this.hasHex = false,
    this.kodeCabang = '',
    this.hex = '',
  });

  factory FingerTemplate.fromJson(Map<String, dynamic> json) {
    final hexStr =
        json['template_hex']?.toString() ?? json['hex']?.toString() ?? '';
    final hexOk = hexStr.length >= 512 ? hexStr.substring(0, 512) : hexStr;
    return FingerTemplate(
      employeeId: (json['employee_id'] ?? json['employeeId'])?.toString() ?? '',
      nama: json['nama']?.toString() ?? json['name']?.toString() ?? '',
      fingerId: (json['finger_id'] as num?)?.toInt() ??
          (json['id'] as num?)?.toInt() ??
          0,
      hasHex: json['has_hex'] == true ||
          hexOk.length == 512 ||
          (json['template_hex']?.toString().length ?? 0) == 512,
      kodeCabang: json['kode_cabang']?.toString() ?? '',
      hex: hexOk,
    );
  }
}

class Branch {
  final String kode;
  final String nama;
  final String kota;

  Branch({required this.kode, required this.nama, required this.kota});

  factory Branch.fromJson(Map<String, dynamic> json) {
    return Branch(
      kode: json['kode_cabang']?.toString() ?? '',
      nama: json['nama_cabang']?.toString() ?? '',
      kota: json['kota']?.toString() ?? '',
    );
  }

  String get label => "$nama ($kode)";
}

class AttendanceRecord {
  final int id;
  final String? idBiodata;
  final String nama;
  final String tanggal;
  final String? jamMasuk;
  final String? jamPulang;
  final String status;
  final String? metode;
  final String? deviceId;
  final bool synced;

  AttendanceRecord({
    required this.id,
    this.idBiodata,
    required this.nama,
    required this.tanggal,
    this.jamMasuk,
    this.jamPulang,
    required this.status,
    this.metode,
    this.deviceId,
    this.synced = true,
  });

  factory AttendanceRecord.fromJson(Map<String, dynamic> json) {
    // Format lokal ESP32 (BLE history): {employeeId, nama, tanggal, jam, synced}
    final employeeId = json['employeeId']?.toString();
    final jamLocal = json['jam']?.toString();
    final syncedLocal = json['synced'] == true;
    final hasLocal = employeeId != null;
    return AttendanceRecord(
      id: (json['id'] as num?)?.toInt() ?? 0,
      idBiodata: hasLocal ? employeeId : json['id_biodata']?.toString(),
      nama: hasLocal
          ? (json['nama']?.toString() ?? employeeId)
          : (json['nama']?.toString() ?? '-'),
      tanggal: hasLocal
          ? (json['tanggal']?.toString() ?? '')
          : (json['tanggal']?.toString() ?? ''),
      jamMasuk: hasLocal ? jamLocal : json['jam_masuk']?.toString(),
      jamPulang: hasLocal ? null : json['jam_pulang']?.toString(),
      status: hasLocal ? (syncedLocal ? 'hadir' : 'pending') : (json['status']?.toString() ?? ''),
      metode: hasLocal ? 'lokal' : json['metode_absen']?.toString(),
      deviceId: hasLocal ? null : json['device_id']?.toString(),
      synced: hasLocal ? syncedLocal : true,
    );
  }
}

/// Akun karyawan internal yang login ke mobile app (bukan CPMI/fingerprint).
class AppKaryawan {
  final int id;
  final String? kodeKaryawan;
  final String nama;
  final String email;
  final String? phone;
  final String? kodeCabang;
  final String? jabatan;
  final String? departemen;
  final String status;

  AppKaryawan({
    required this.id,
    this.kodeKaryawan,
    required this.nama,
    required this.email,
    this.phone,
    this.kodeCabang,
    this.jabatan,
    this.departemen,
    this.status = 'active',
  });

  bool get isActive => status == 'active';

  factory AppKaryawan.fromJson(Map<String, dynamic> json) {
    return AppKaryawan(
      id: (json['id'] as num?)?.toInt() ?? 0,
      kodeKaryawan: json['kode_karyawan']?.toString(),
      nama: json['nama']?.toString() ?? '',
      email: json['email']?.toString() ?? '',
      phone: json['phone']?.toString(),
      kodeCabang: json['kode_cabang']?.toString(),
      jabatan: json['jabatan']?.toString(),
      departemen: json['departemen']?.toString(),
      status: json['status']?.toString() ?? 'active',
    );
  }
}

/// Absensi karyawan app (tabel karyawan_absensi di server).
class KaryawanAbsensi {
  final int id;
  final int karyawanId;
  final String nama;
  final String tanggal;
  final String? jamMasuk;
  final String? jamPulang;
  final String status;
  final String? keterangan;
  final String? metodeAbsen;
  final String? kodeCabang;

  KaryawanAbsensi({
    required this.id,
    required this.karyawanId,
    required this.nama,
    required this.tanggal,
    this.jamMasuk,
    this.jamPulang,
    required this.status,
    this.keterangan,
    this.metodeAbsen,
    this.kodeCabang,
  });

  factory KaryawanAbsensi.fromJson(Map<String, dynamic> json) {
    return KaryawanAbsensi(
      id: (json['id'] as num?)?.toInt() ?? 0,
      karyawanId: (json['karyawan_id'] as num?)?.toInt() ?? 0,
      nama: json['nama']?.toString() ?? '',
      tanggal: json['tanggal']?.toString() ?? '',
      jamMasuk: json['jam_masuk']?.toString(),
      jamPulang: json['jam_pulang']?.toString(),
      status: json['status']?.toString() ?? '',
      keterangan: json['keterangan']?.toString(),
      metodeAbsen: json['metode_absen']?.toString(),
      kodeCabang: json['kode_cabang']?.toString(),
    );
  }
}

class User {
  final int id;
  final String name;
  final String email;
  final String role;
  final String? kodeCabang;

  User({
    required this.id,
    required this.name,
    required this.email,
    required this.role,
    this.kodeCabang,
  });

  factory User.fromJson(Map<String, dynamic> json) {
    return User(
      id: (json['id'] as num?)?.toInt() ?? 0,
      name: json['name']?.toString() ?? '',
      email: json['email']?.toString() ?? '',
      role: json['role']?.toString() ?? '',
      kodeCabang: json['kode_cabang']?.toString(),
    );
  }
}

class BleDeviceInfo {
  final String id;
  final String name;
  final int rssi;

  BleDeviceInfo({required this.id, required this.name, this.rssi = 0});
}

class BleEvent {
  final String type;
  final Map<String, dynamic> data;

  BleEvent(this.type, this.data);

  factory BleEvent.parse(String raw) {
    try {
      final json = jsonDecode(raw);
      if (json is! Map) return BleEvent('', {});
      final type = json['event']?.toString() ?? '';
      final data = Map<String, dynamic>.from(json);
      data.remove('event');
      return BleEvent(type, data);
    } catch (_) {
      try {
        final m = RegExp(r'"event"\s*:\s*"([^"]+)"').firstMatch(raw);
        final type = m?.group(1) ?? '';
        return BleEvent(type, {});
      } catch (_) {
        return BleEvent('', {});
      }
    }
  }
}
