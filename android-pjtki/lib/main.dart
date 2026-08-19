import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'screens/connect_screen.dart';
import 'screens/home_shell.dart';
import 'screens/login_screen.dart';
import 'services/api_service.dart';
import 'services/ble_service.dart';
import 'services/enroll_store.dart';
import 'services/settings_store.dart';
import 'theme/app_theme.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final settings = SettingsStore();
  await settings.init();
  final enrollStore = EnrollStore();
  await enrollStore.init();
  final api = ApiService(settings);
  final ble = BleService();
  ble.bindSettings(settings);

  runApp(
    MultiProvider(
      providers: [
        Provider.value(value: settings),
        Provider.value(value: enrollStore),
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
      theme: AppTheme.light(),
      routes: {
        '/home': (_) => const HomeShell(),
      },
      home: !settings.isLoggedIn
          ? const LoginScreen()
          : (settings.isConfigured ? const HomeShell() : const ConnectScreen()),
    );
  }
}
