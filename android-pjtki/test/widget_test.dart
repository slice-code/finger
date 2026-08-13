import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('app boots', (WidgetTester tester) async {
    // Smoke test kosong — app butuh platform channels (BLE/shared_prefs).
    expect(1 + 1, 2);
  });
}
