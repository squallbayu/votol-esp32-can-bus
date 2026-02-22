# Flutter: Kirim Command `WIFI:ON` / `BLE:ON`

Dokumen ini menjelaskan **cara kirim command** untuk pindah mode transport:

- **BLE -> WiFi**: kirim `WIFI:ON` lewat BLE characteristic (raw text).
- **WiFi -> BLE**: kirim `BLE:ON` lewat WebSocket (WiFi AP, raw text).

Dokumen ini **mengacu ke firmware saat ini** di `esp32/votol_ble_dualcore`.

---

## 1) BLE -> WiFi (`WIFI:ON`)

**BLE device name**: `Votol_BLE`  
**Service UUID**: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`  
**Characteristic UUID (write)**: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

Contoh dengan `flutter_blue_plus` (pastikan kirim **string mentah**, bukan JSON):

```dart
import 'dart:convert';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

Future<void> switchToWifi() async {
  // Scan & connect
  await FlutterBluePlus.startScan(timeout: const Duration(seconds: 5));
  final results = await FlutterBluePlus.scanResults.first;
  final dev = results
      .map((r) => r.device)
      .firstWhere((d) => d.name == 'Votol_BLE');
  await FlutterBluePlus.stopScan();
  await dev.connect();

  // Discover services & characteristic
  final services = await dev.discoverServices();
  final svc = services.firstWhere(
    (s) => s.uuid.toString().toLowerCase() == '4fafc201-1fb5-459e-8fcc-c5c9c331914b',
  );
  final ch = svc.characteristics.firstWhere(
    (c) => c.uuid.toString().toLowerCase() == 'beb5483e-36e1-4688-b7f5-ea07361b26a8',
  );

  // Send command
  await ch.write(utf8.encode('WIFI:ON'), withoutResponse: false);

  // ESP32 akan mematikan BLE dan menyalakan WiFi AP
}
```

**Expected behavior**:  
BLE akan disconnect, lalu ESP32 **start WiFi AP**. Tunggu 2-3 detik sampai SSID muncul.

---

## 2) WiFi -> BLE (`BLE:ON`)

**WiFi AP SSID**: `VOTOL_Wifi`  
**Password**: `votol12345`  
**WebSocket**: `ws://192.168.4.1:81/ws`

Contoh dengan `web_socket_channel`:

```dart
import 'package:web_socket_channel/web_socket_channel.dart';

void switchToBle() {
  final ws = WebSocketChannel.connect(
    Uri.parse('ws://192.168.4.1:81/ws'),
  );

  // Kirim raw text (bukan JSON)
  ws.sink.add('BLE:ON');

  // ESP32 akan mematikan WiFi dan mengaktifkan BLE
  // WebSocket akan terputus setelah switch berhasil
  ws.sink.close();
}
```

**Expected behavior**:  
WiFi AP akan berhenti, WebSocket disconnect, dan BLE advertising kembali muncul.

---

## Troubleshooting Singkat

1. **Command tidak bereaksi**
   - Pastikan UUID benar.
   - Pastikan command persis `WIFI:ON` atau `BLE:ON` (huruf besar).
   - Pastikan **kirim raw text**, bukan JSON.
   - Cek log Serial ESP32 untuk melihat perubahan mode.

2. **Tidak bisa connect BLE**
   - Pastikan device name `Votol_BLE`.
   - Android: pastikan permission Bluetooth + Location sudah diizinkan.
   - iOS: pastikan `NSBluetoothAlwaysUsageDescription` sudah di `Info.plist`.

3. **WiFi AP tidak muncul**
   - Setelah `WIFI:ON`, tunggu 2-3 detik.
   - Cek SSID `VOTOL_Wifi` (bukan `VOTOL_Dashboard`).

4. **WebSocket tidak bisa connect (Android)**
   - Pastikan cleartext HTTP/WS diizinkan (`android:usesCleartextTraffic="true"` atau Network Security Config).
