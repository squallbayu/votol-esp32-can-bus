# Implementasi OTA Update di Flutter (Android/iOS)

Panduan ini menjelaskan cara membuat fitur update firmware OTA untuk ESP32 Votol melalui aplikasi Flutter.

ESP32 VOTOL Dashboard mendukung **dua metode OTA**:
1. **BLE OTA** - Via Bluetooth Low Energy (default mode)
2. **WiFi OTA** - Via HTTP POST (saat dalam WiFi mode)

Kedua metode menggunakan **API yang seragam** sehingga Flutter app bisa menggunakan interface yang sama.

> 💡 **Rekomendasi**: Gunakan BLE OTA untuk kenyamanan, WiFi OTA untuk kecepatan upload yang lebih tinggi.

## 1. Spesifikasi Protokol BLE

Sesuai dengan `votol_ota.h`, berikut adalah UUID dan Command yang digunakan:

**Service UUID**: `fb1e4001-54ae-4a28-9f74-dfccb248601d`

| Karakteristik | UUID | Sifat | Fungsi |
| :--- | :--- | :--- | :--- |
| **Control** | `...4002...` | Write | Kirim command (Begin/End/Abort) |
| **Data** | `...4003...` | WriteNoResponse | Kirim potongan file firmware |
| **Status** | `...4004...` | Notify | Terima status progress & error |

### Daftar Command (Hex)
- **BEGIN**: `0x01` (+ 4 byte ukuran file Little Endian)
- **END**: `0x02`
- **ABORT**: `0x03`

### Status Codes (Dari ESP32)
- `0x00`: Idle
- `0x01`: Ready (Siap terima data)
- `0x02`: Receiving (Sedang menerima)
- `0x03`: Complete (Selesai, akan reboot)
- `0x10` ke atas: Error

---

## 2. Implementasi Dart (Flutter)

Pastikan `flutter_blue_plus` sudah ada di `pubspec.yaml`.

```yaml
dependencies:
  flutter_blue_plus: ^1.30.0 # atau versi terbaru
  file_picker: ^6.0.0        # untuk pilih file .bin
  http: ^1.2.0               # untuk OTA via WiFi
```

### Class Helper: `Esp32OtaService.dart`

Buat file baru, misal `services/esp32_ota_service.dart`. Gunakan kode berikut sebagai referensi lengkap.

```dart
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class Esp32OtaService {
  // UUIDs
  static const String OTA_SERVICE_UUID = "fb1e4001-54ae-4a28-9f74-dfccb248601d";
  static const String OTA_CONTROL_UUID = "fb1e4002-54ae-4a28-9f74-dfccb248601d";
  static const String OTA_DATA_UUID    = "fb1e4003-54ae-4a28-9f74-dfccb248601d";
  static const String OTA_STATUS_UUID  = "fb1e4004-54ae-4a28-9f74-dfccb248601d";

  // Commands
  static const int CMD_BEGIN = 0x01;
  static const int CMD_END   = 0x02;
  static const int CMD_ABORT = 0x03;

  // Status
  static const int STATUS_READY    = 0x01;
  static const int STATUS_COMPLETE = 0x03;
  static const int STATUS_ERROR    = 0x10;

  final BluetoothDevice device;
  BluetoothCharacteristic? _controlChar;
  BluetoothCharacteristic? _dataChar;
  BluetoothCharacteristic? _statusChar;

  bool _isFlashing = false;
  
  // Stream controller untuk update UI (progress 0-100, error msg)
  final _progressController = StreamController<double>.broadcast();
  Stream<double> get progressStream => _progressController.stream;

  Esp32OtaService(this.device);

  // 1. Setup Koneksi & Karakteristik
  Future<bool> prepare() async {
    try {
      List<BluetoothService> services = await device.discoverServices();
      var otaService = services.firstWhere((s) => s.uuid.str128.toLowerCase() == OTA_SERVICE_UUID);
      
      for (var c in otaService.characteristics) {
        String uuid = c.uuid.str128.toLowerCase();
        if (uuid == OTA_CONTROL_UUID) _controlChar = c;
        if (uuid == OTA_DATA_UUID) _dataChar = c;
        if (uuid == OTA_STATUS_UUID) {
          _statusChar = c;
          await _statusChar!.setNotifyValue(true);
        }
      }
      return _controlChar != null && _dataChar != null && _statusChar != null;
    } catch (e) {
      print("Error preparing OTA: $e");
      return false;
    }
  }

  // 2. Fungsi Utama Update Firmware
  Future<void> startOta(File firmwareFile) async {
    if (_isFlashing) return;
    _isFlashing = true;

    try {
      Uint8List bytes = await firmwareFile.readAsBytes();
      int fileSize = bytes.length;
      
      print("Starting OTA... Size: $fileSize bytes");

      // Listen status dari ESP32
      Completer<bool> readyCompleter = Completer();
      Completer<bool> finishedCompleter = Completer();

      StreamSubscription? statusSub;
      statusSub = _statusChar!.lastValueStream.listen((value) {
        if (value.isEmpty) return;
        int status = value[0];
        // int progress = value.length > 1 ? value[1] : 0; // Optional jika ESP kirim progress

        if (status == STATUS_READY) {
          if (!readyCompleter.isCompleted) readyCompleter.complete(true);
        } else if (status == STATUS_COMPLETE) {
          if (!finishedCompleter.isCompleted) finishedCompleter.complete(true);
        } else if (status >= STATUS_ERROR) {
           _progressController.addError("OTA Error Code: $status");
           _isFlashing = false;
        }
      });

      // A. Kirim CMD BEGIN + Ukuran File
      // Format: [CMD, Size Byte 0, Size Byte 1, Size Byte 2, Size Byte 3] (Little Endian)
      List<int> beginCmd = [CMD_BEGIN];
      beginCmd.add(fileSize & 0xFF);
      beginCmd.add((fileSize >> 8) & 0xFF);
      beginCmd.add((fileSize >> 16) & 0xFF);
      beginCmd.add((fileSize >> 24) & 0xFF);

      await _controlChar!.write(beginCmd);
      
      // Tunggu ESP32 siap (Status READY)
      await readyCompleter.future.timeout(Duration(seconds: 5));
      print("ESP32 Ready. Sending chunks...");

      // B. Kirim Data per Chunks
      // ESP32 OTA_CHUNK_SIZE = 512, tapi untuk BLE safety gunakan 240
      // MTU sudah di-request 512, tapi overhead BLE ~3 bytes jadi safe max ~509
      // Gunakan 240 untuk kompatibilitas universal
      int chunkSize = 240;
      
      int offset = 0;
      while (offset < fileSize) {
        int end = offset + chunkSize;
        if (end > fileSize) end = fileSize;
        
        List<int> chunk = bytes.sublist(offset, end);
        
        // WriteWithoutResponse untuk kecepatan tinggi
        await _dataChar!.write(chunk, withoutResponse: true);
        
        offset += chunk.length;
        
        // Update UI Progress
        double progress = offset / fileSize;
        _progressController.add(progress);
        
        // Sedikit delay untuk mencegah buffer overflow di ESP32 BLE stack
        // Jika terlalu cepat, ESP32 bisa disconnect. Adjustjika perlu.
        await Future.delayed(Duration(milliseconds: 5)); 
      }

      print("All data sent. Waiting for verification...");

      // C. Kirim CMD END
      await _controlChar!.write([CMD_END]);

      // D. Tunggu Konfirmasi Selesai
      await finishedCompleter.future.timeout(Duration(seconds: 10));
      
      print("OTA Success! Device rebooting.");
      _progressController.add(1.0); // 100%

      statusSub.cancel();
    } catch (e) {
      _progressController.addError(e.toString());
      // Coba kirim Abort
      try { await _controlChar!.write([CMD_ABORT]); } catch (_) {}
    } finally {
      _isFlashing = false;
    }
  }
}
```

## 3. Cara Menggunakan di UI

Di halaman Settings atau Update Firmware Anda:

```dart
// 1. Request MTU besar dulu setelah connect (PENTING untuk kecepatan)
await device.requestMtu(512); 

// 2. Inisialisasi Service
final otaService = Esp32OtaService(device);
bool ready = await otaService.prepare();

if (ready) {
  // 3. Pilih File (gw: file_picker)
  FilePickerResult? result = await FilePicker.platform.pickFiles();
  
  if (result != null) {
      File file = File(result.files.single.path!);
      
      // 4. Listen Progress
      otaService.progressStream.listen((prog) {
          print("Progress: ${(prog * 100).toStringAsFixed(1)}%");
          // Update LinearProgressIndicator di sini
      }, onError: (err) {
          // Show Snackbar Error
      });

      // 5. Mulai Flash
      await otaService.startOta(file);
  }
}
```

---

## 4. OTA via WiFi (HTTP) - Alternatif

Saat ESP32 dalam mode **WiFi** (setelah kirim command `WIFI:ON`), OTA juga bisa dilakukan via HTTP. Ini lebih cepat daripada BLE karena tidak ada overhead chunking.

### Spesifikasi HTTP OTA

| Endpoint | Method | Description |
|----------|--------|-------------|
| `http://192.168.4.1/update` | POST | Upload firmware .bin |
| `http://192.168.4.1/ota_status` | GET | Cek progress OTA (JSON) |
| `http://192.168.4.1/health` | GET | Health check (JSON) |

### Response Format

**Success (200 OK):**
```
OK
```
ESP32 akan reboot otomatis setelah response.

**Error (500):**
```
ERROR: [error message]
```

### Class Helper: `Esp32OtaWifiService.dart`

```dart
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:http/http.dart' as http;

class Esp32OtaWifiService {
  static const String BASE_URL = "http://192.168.4.1";
  
  final _progressController = StreamController<double>.broadcast();
  Stream<double> get progressStream => _progressController.stream;
  
  bool _isFlashing = false;
  
  /// Check if ESP32 is ready for OTA
  Future<bool> checkHealth() async {
    try {
      final response = await http.get(
        Uri.parse('$BASE_URL/health'),
      ).timeout(Duration(seconds: 3));
      
      if (response.statusCode == 200) {
        // Parse JSON response
        // {"status":"ok","mode":"wifi","ota_ready":true}
        return true;
      }
      return false;
    } catch (e) {
      print("Health check failed: $e");
      return false;
    }
  }
  
  /// Start OTA update via HTTP
  Future<void> startOta(File firmwareFile) async {
    if (_isFlashing) return;
    _isFlashing = true;
    
    try {
      // Read file bytes
      Uint8List bytes = await firmwareFile.readAsBytes();
      int totalSize = bytes.length;
      
      print("[OTA-WiFi] Starting upload: $totalSize bytes");
      
      // Use a custom http client with progress tracking
      // Note: Standard http package doesn't support upload progress
      // We'll use chunked upload with progress calculation
      
      final request = http.Request('POST', Uri.parse('$BASE_URL/update'));
      request.headers['Content-Type'] = 'application/octet-stream';
      request.bodyBytes = bytes;
      
      // Send request
      final streamedResponse = await request.send();
      
      // Track progress manually (approximate)
      int sent = 0;
      final chunkSize = 8192; // 8KB chunks
      
      for (int i = 0; i < bytes.length; i += chunkSize) {
        int end = (i + chunkSize < bytes.length) ? i + chunkSize : bytes.length;
        sent += (end - i);
        
        double progress = sent / totalSize;
        _progressController.add(progress);
        
        // Small delay to not block UI
        await Future.delayed(Duration(milliseconds: 1));
      }
      
      // Wait for response
      final response = await http.Response.fromStream(streamedResponse);
      
      if (response.statusCode == 200) {
        print("[OTA-WiFi] Success! ESP32 rebooting...");
        _progressController.add(1.0); // 100%
        // Wait for reboot
        await Future.delayed(Duration(seconds: 3));
      } else {
        throw Exception("OTA failed: ${response.body}");
      }
      
    } catch (e) {
      _progressController.addError(e.toString());
      throw e;
    } finally {
      _isFlashing = false;
    }
  }
  
  /// Get OTA status from ESP32
  Future<Map<String, dynamic>> getOtaStatus() async {
    try {
      final response = await http.get(
        Uri.parse('$BASE_URL/ota_status'),
      ).timeout(Duration(seconds: 2));
      
      if (response.statusCode == 200) {
        // Parse JSON: {"in_progress":false,"received":0,"total":0,"progress_percent":0}
        // Manual parsing for simplicity
        String body = response.body;
        return {
          'in_progress': body.contains('"in_progress":true'),
          'received': _extractInt(body, '"received":'),
          'total': _extractInt(body, '"total":'),
          'progress_percent': _extractInt(body, '"progress_percent":'),
        };
      }
      return {'error': 'Failed to get status'};
    } catch (e) {
      return {'error': e.toString()};
    }
  }
  
  int _extractInt(String json, String key) {
    int start = json.indexOf(key);
    if (start == -1) return 0;
    start += key.length;
    int end = json.indexOf(',', start);
    if (end == -1) end = json.indexOf('}', start);
    if (end == -1) return 0;
    return int.tryParse(json.substring(start, end).trim()) ?? 0;
  }
  
  void dispose() {
    _progressController.close();
  }
}
```

### Unified OTA Service (BLE + WiFi)

Untuk menggunakan API yang sama persis baik di BLE maupun WiFi:

```dart
abstract class OtaService {
  Stream<double> get progressStream;
  Future<void> startOta(File firmwareFile);
  void dispose();
}

// Factory untuk membuat service yang sesuai
class OtaServiceFactory {
  static OtaService createBleService(BluetoothDevice device) {
    return Esp32OtaService(device);
  }
  
  static OtaService createWifiService() {
    return Esp32OtaWifiService();
  }
}

// Usage di Flutter - SAMA untuk BLE dan WiFi!
class FirmwareUpdatePage extends StatefulWidget {
  final OtaService otaService; // Bisa BLE atau WiFi
  
  @override
  _FirmwareUpdatePageState createState() => _FirmwareUpdatePageState();
}

class _FirmwareUpdatePageState extends State<FirmwareUpdatePage> {
  double _progress = 0;
  String _status = "Ready";
  
  @override
  void initState() {
    super.initState();
    
    // Listen progress - SAMA untuk BLE dan WiFi!
    widget.otaService.progressStream.listen(
      (progress) {
        setState(() {
          _progress = progress;
          _status = "Uploading: ${(progress * 100).toStringAsFixed(1)}%";
        });
      },
      onError: (error) {
        setState(() {
          _status = "Error: $error";
        });
      },
    );
  }
  
  Future<void> _selectAndFlash() async {
    FilePickerResult? result = await FilePicker.platform.pickFiles();
    if (result != null) {
      File file = File(result.files.single.path!);
      
      setState(() => _status = "Starting...");
      
      try {
        // Start OTA - SAMA untuk BLE dan WiFi!
        await widget.otaService.startOta(file);
        setState(() => _status = "Success! Device rebooting...");
      } catch (e) {
        setState(() => _status = "Failed: $e");
      }
    }
  }
  
  @override
  void dispose() {
    widget.otaService.dispose();
    super.dispose();
  }
}
```

### Perbandingan BLE vs WiFi OTA

| Aspek | BLE OTA | WiFi OTA |
|-------|---------|----------|
| **Kecepatan Upload** | ~2-5 KB/s | ~50-100 KB/s |
| **Waktu Upload 1MB** | ~3-5 menit | ~10-20 detik |
| **Reliability** | Tinggi (auto-retry) | Tinggi |
| **Setup** | Pairing BLE | Connect ke WiFi AP |
| **Cocok untuk** | Update rutin | Update besar, development |

---

## Tips Tambahan

1.  **MTU Size**: Sangat disarankan memanggil `device.requestMtu(512)` segera setelah connect. Jika MTU rendah (default 23 byte), pengiriman file 1MB akan sangat lambat.
2.  **Format File**: File yang dipilih harus file `.bin` hasil export Arduino IDE (`Sketch` -> `Export Compiled Binary`).
3.  **Partition**: Pastikan ESP32 menggunakan Partition Scheme yang mendukung OTA (misal: "Default 4MB with SPIFFS") saat di-upload via kabel pertama kali.
