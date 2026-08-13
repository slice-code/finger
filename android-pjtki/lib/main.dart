import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'screens/connect_screen.dart';
import 'screens/home_shell.dart';
import 'services/api_service.dart';
import 'services/ble_service.dart';
import 'services/settings_store.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final settings = SettingsStore();
  await settings.init();
  final api = ApiService(settings);
  final ble = BleService();
  ble.bindSettings(settings);
  // JANGAN restore di sini (sebelum runApp) — permission dialog BLE belum bisa
  // tampil & Android stack belum siap → koneksi gagal diam-diam.
  // restoreLastDevice() dipanggil dari HomeShell setelah widget tree siap.

  runApp(
    MultiProvider(
      providers: [
        Provider.value(value: settings),
        Provider.value(value: api),
        ChangeNotifierProvider.value(value: ble),
      ],
      child: const PjtkiApp(),
    ),
  );
}

class PjtkiApp extends StatelessWidget {
  const PjtkiApp({super.key});

  @override
  Widget build(BuildContext context) {
    final settings = context.watch<SettingsStore>();
    return MaterialApp(
      title: 'PJTKI Absensi',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF1E88E5)),
        useMaterial3: true,
      ),
      home: settings.isConfigured ? const HomeShell() : const ConnectScreen(),
    );
  }
}
