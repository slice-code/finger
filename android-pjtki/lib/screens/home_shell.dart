import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/ble_service.dart';
import '../theme/app_theme.dart';
import 'app_karyawan_screen.dart';
import 'dashboard_screen.dart';
import 'device_scan_screen.dart';
import 'karyawan_screen.dart';
import 'riwayat_screen.dart';
import 'setelan_screen.dart';
import 'sync_screen.dart';

class HomeShell extends StatefulWidget {
  const HomeShell({super.key});

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int _index = 0;
  final _staffKey = GlobalKey<AppKaryawanScreenState>();

  late final List<Widget> _pages;

  @override
  void initState() {
    super.initState();
    _pages = [
      const DashboardScreen(),
      const KaryawanScreen(),
      AppKaryawanScreen(key: _staffKey),
      const RiwayatScreen(),
      const SyncScreen(),
      const SetelanScreen(),
    ];
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      context.read<BleService>().restoreLastDevice();
    });
  }

  Future<void> _openScan() async {
    await Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => const DeviceScanScreen()),
    );
  }

  Future<void> _disconnect() async {
    final ble = context.read<BleService>();
    if (ble.isConnecting || ble.isAutoReconnecting) {
      await ble.disconnect(forget: true);
      return;
    }
    if (!await confirmBleDisconnect(context, ble.connectedName)) return;
    await ble.disconnect(forget: true);
  }

  @override
  Widget build(BuildContext context) {
    return Selector<BleService, (bool connected, bool connecting, String name)>(
      selector: (_, ble) => (
        ble.isConnected,
        ble.isConnecting || ble.isAutoReconnecting,
        ble.connectedName,
      ),
      builder: (context, conn, _) {
        final connected = conn.$1;
        final connecting = conn.$2;
        final connectedName = conn.$3;
        return Scaffold(
          body: IndexedStack(index: _index, children: _pages),
          bottomNavigationBar: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (connected)
                Material(
                  color: const Color(0xFFE8F5E9),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                        horizontal: 12, vertical: 6),
                    child: Row(
                      children: [
                        Icon(Icons.bluetooth_connected,
                            size: 20, color: Colors.green.shade700),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            connectedName,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(
                              fontSize: 13,
                              fontWeight: FontWeight.w700,
                              color: Colors.green.shade800,
                            ),
                          ),
                        ),
                        TextButton(
                          onPressed: _openScan,
                          child: const Text('Ganti'),
                        ),
                        TextButton(
                          onPressed: _disconnect,
                          style: TextButton.styleFrom(
                              foregroundColor: AppTheme.danger),
                          child: const Text('Lepas'),
                        ),
                      ],
                    ),
                  ),
                )
              else
                Material(
                  color: connecting
                      ? const Color(0xFFFFF3E0)
                      : const Color(0xFFFFEBEE),
                  child: InkWell(
                    onTap: connecting ? null : _openScan,
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 16, vertical: 10),
                      child: Row(
                        children: [
                          if (connecting)
                            const SizedBox(
                              width: 18,
                              height: 18,
                              child:
                                  CircularProgressIndicator(strokeWidth: 2),
                            )
                          else
                            Icon(
                              Icons.bluetooth_disabled,
                              size: 20,
                              color: Colors.red.shade700,
                            ),
                          const SizedBox(width: 10),
                          Expanded(
                            child: Text(
                              connecting
                                  ? 'Menghubungkan ke device terakhir…'
                                  : 'Device belum terhubung — ketuk untuk mencari',
                              style: TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                                color: connecting
                                    ? Colors.orange.shade900
                                    : Colors.red.shade800,
                              ),
                            ),
                          ),
                          if (connecting)
                            TextButton(
                              onPressed: _disconnect,
                              child: const Text('Batal'),
                            )
                          else
                            Text(
                              'Hubungkan',
                              style: TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w800,
                                color:
                                    Theme.of(context).colorScheme.primary,
                              ),
                            ),
                        ],
                      ),
                    ),
                  ),
                ),
              NavigationBar(
                selectedIndex: _index,
                onDestinationSelected: (i) {
                  setState(() => _index = i);
                  if (i == 2) _staffKey.currentState?.reload();
                },
                destinations: const [
                  NavigationDestination(
                    icon: Icon(Icons.home_outlined),
                    selectedIcon: Icon(Icons.home),
                    label: 'Beranda',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.people_outline),
                    selectedIcon: Icon(Icons.people),
                    label: 'CPMI',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.badge_outlined),
                    selectedIcon: Icon(Icons.badge),
                    label: 'Staff',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.history_outlined),
                    selectedIcon: Icon(Icons.history),
                    label: 'Riwayat',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.sync_outlined),
                    selectedIcon: Icon(Icons.sync),
                    label: 'Sync',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.settings_outlined),
                    selectedIcon: Icon(Icons.settings),
                    label: 'Setelan',
                  ),
                ],
              ),
            ],
          ),
        );
      },
    );
  }
}
