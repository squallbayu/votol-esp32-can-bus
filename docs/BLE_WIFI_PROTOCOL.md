# ESP32 BLE & WiFi JSON Protocol Documentation

## Overview

ESP32 VOTOL Dashboard mendukung **dua mode transport** untuk komunikasi real-time:
- **BLE Mode** (Default): Low power, short range (~10m), cocok untuk monitoring sehari-hari
- **WiFi Mode** (On-demand): Higher throughput, longer range (~50m), cocok untuk debug/analisis detail

Kedua mode menggunakan **format JSON yang sama persis**, sehingga Flutter app hanya perlu satu parser.

ESP32 menggunakan **dual-rate transmission** untuk optimasi real-time:
- **FAST** (mode-adaptive 50-500ms): Data kritikal untuk driving (200-300 bytes)
- **FULL** (mode-adaptive 1000-2000ms): Data lengkap termasuk cells, balance, health (1.5-2KB)

Kedua tipe JSON memiliki field `"type"` untuk membedakan:
- `"type":"fast"` → Fast update
- `"type":"full"` → Full update

---

## Transport Mode Comparison

| Aspek | BLE Mode | WiFi Mode |
|-------|----------|-----------|
| **Jangkauan** | ~10 meter | ~50 meter |
| **Throughput** | ~1 Mbps | ~10+ Mbps |
| **Latency** | 10-50ms | 5-20ms |
| **Power** | Low | Higher |
| **Connection** | Pairing required | Direct connect (AP mode) |
| **Concurrent** | BLE mati saat WiFi aktif | WiFi mati saat BLE aktif |
| **Use Case** | Daily monitoring, mobile | Debug, analysis, desktop |

---

## Mode Switching

### BLE → WiFi
Kirim command via BLE dari Flutter app (raw text):
```
WIFI:ON
```

ESP32 akan:
1. Stop BLE advertising dan deinit BLE stack
2. Start WiFi AP (SSID: `VOTOL_Wifi`, Pass: `votol12345`)
3. Start WebSocket server (`ws://192.168.4.1:81/ws`)

### WiFi → BLE (Manual)
Kirim command via WebSocket dari Flutter app (raw text):
```
BLE:ON
```

ESP32 akan mematikan WiFi dan menyalakan BLE lagi.  
**Catatan:** Saat ini tidak ada auto-return ke BLE.

### WiFi Configuration

| Parameter | Value |
|-----------|-------|
| SSID | `VOTOL_Wifi` |
| Password | `votol12345` (min 8 chars) |
| IP Address | `192.168.4.1` |
| WebSocket Port | `81` |
| WebSocket URL | `ws://192.168.4.1:81/ws` |
| Max Clients | 4 |

---

## Flutter WebSocket Implementation

```dart
import 'package:web_socket_channel/web_socket_channel.dart';

class VotolWebSocket {
  WebSocketChannel? _channel;
  
  void connect() {
    _channel = WebSocketChannel.connect(
      Uri.parse('ws://192.168.4.1:81/ws'),
    );
    
    _channel!.stream.listen(
      (data) => _onData(data),
      onError: (error) => print('WS Error: $error'),
      onDone: () => print('WS Disconnected - Auto returning to BLE'),
    );
  }
  
  void _onData(String jsonData) {
    // Parse SAMA PERSIS dengan data BLE
    final data = jsonDecode(jsonData);
    // ... handle data
  }
  
  void disconnect() {
    _channel?.sink.close();
    // ESP32 akan auto-return ke BLE setelah 10 detik
  }
}
```

---

## Field Name Mapping

### Top Level Fields
| Field | Description |
|-------|-------------|
| `r` | Motor RPM |
| `s` | Speed in km/h |
| `m` | Driving mode (PARK/DRIVE/SPORT/REVERSE/BRAKE/CHARGING/STAND) |
| `v` | Battery voltage (V) |
| `a` | Battery current (A) - negative = discharge, positive = charge/regen |
| `p` | Power (W) |
| `sc` | State of Charge (%) |
| `cr` | CAN messages per second |
| `odo` | Odometer reading (km) |
| `cd` | Cell voltage delta (mV) |
| `hb` | Heartbeat counter |

### Temps Object
| Field | Description |
|-------|-------------|
| `t.c` | Controller temp (°C) |
| `t.m` | Motor temp (°C) |
| `t.b` | Battery average temp (°C) |

### Health Object (Full only)
| Field | Description |
|-------|-------------|
| `h.soh` | State of Health (%) |
| `h.cyc` | Charge cycle count |
| `h.rc` | Remaining capacity (Ah) |
| `h.fc` | Full capacity (Ah) |

### Cell Volt Stats (Full only)
| Field | Description |
|-------|-------------|
| `cvs.hi` | Highest cell voltage (mV) |
| `cvs.hiC` | Highest cell number (1-23) |
| `cvs.lo` | Lowest cell voltage (mV) |
| `cvs.loC` | Lowest cell number (1-23) |
| `cvs.av` | Average cell voltage (mV) |

### Temp Stats (Full only)
| Field | Description |
|-------|-------------|
| `ts.max` | Max battery temp (°C) |
| `ts.maxC` | Max temp sensor number |
| `ts.min` | Min battery temp (°C) |
| `ts.minC` | Min temp sensor number |

### Balance Object (Full only)
| Field | Description |
|-------|-------------|
| `b.md` | Balance mode |
| `b.st` | Balance status |
| `b.cells` | Balance status array [0/1] for 23 cells |

### Charger Object (Full only)
| Field | Description |
|-------|-------------|
| `chr.on` | Charging flag (0/1) |
| `chr.v` | Charger voltage (V) |
| `chr.a` | Charger current (A) |
| `chr.ori` | Original charger detected (0/1) |

### BMS Info (Full only)
| Field | Description |
|-------|-------------|
| `bms.hw` | Hardware version (e.g. "H:v21") |
| `bms.fw` | Firmware version (e.g. "F:v23") |

### Arrays (Full only - unchanged names)
- `cells` → array of 23 cell voltages in mV

---

## JSON Examples

### FAST Update

```json
{
  "r": 2500,
  "s": 45,
  "m": "DRIVE",
  "v": 72.3,
  "a": -15.2,
  "p": 1099,
  "sc": 85,
  "t": {
    "c": 45,
    "m": 52,
    "b": 38
  },
  "cr": 120,
  "hb": 12345,
  "type": "fast"
}
```

**Size**: ~200 bytes  
**Update Rate**: Mode-adaptive (50-500ms)  
**Use Case**: Real-time dashboard, speed/power display, real-time amp monitoring

---

### FULL Update

```json
{
  "r": 2500,
  "s": 45,
  "m": "DRIVE",
  "v": 72.3,
  "a": -15.2,
  "p": 1099,
  "sc": 85,
  "t": {
    "c": 45,
    "m": 52,
    "b": 38
  },
  "cells": [3650, 3652, 3648, 3655, 3651, 3649, 3653, 3650, 3652, 3654, 3651, 3650, 3649, 3653, 3652, 3651, 3650, 3654, 3652, 3651, 3650, 3653, 3651],
  "cd": 7,
  "cr": 120,
  "odo": 1234,
  "h": {
    "soh": 98,
    "cyc": 45,
    "rc": 28.5,
    "fc": 29.0
  },
  "cvs": {
    "hi": 3655,
    "hiC": 4,
    "lo": 3648,
    "loC": 3,
    "av": 3651
  },
  "ts": {
    "max": 42,
    "maxC": 2,
    "min": 35,
    "minC": 4
  },
  "b": {
    "md": 1,
    "st": 2,
    "cells": [0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
  },
  "chr": {
    "on": 0,
    "v": 72.3,
    "a": 15.2,
    "ori": 0
  },
  "bms": {
    "hw": "H:v21",
    "fw": "F:v23"
  },
  "hb": 12345,
  "type": "full"
}
```

**Size**: ~800-1500 bytes  
**Update Rate**: Mode-adaptive (1000-2000ms)  
**Use Case**: Cell monitoring, balance status, battery health, charger info

---

## Mode Values

| Mode String | Description |
|-------------|-------------|
| `PARK` | Vehicle in park |
| `DRIVE` | Normal driving mode |
| `SPORT` | Sport mode |
| `REVERSE` | Reverse gear |
| `BRAKE` | Braking / Regenerative braking |
| `CHARGING` | Battery charging mode |
| `STAND` | Standby mode |

---

## Commands

### CAN Injector Control
Fitur ini digunakan untuk mengaktifkan "injeksi" sinyal charger agar speedometer mendeteksi Mode Charging saat menggunakan charger third-party/KW.

| Command | Description | Effect |
|---------|-------------|--------|
| `INJECT:1` | **Enable** Injector | ESP32 akan inject pesan `0x10261041` jika charger terdeteksi |
| `INJECT:0` | **Disable** Injector | ESP32 tidak akan inject pesan apapun (Safe Mode) |

### Transport Mode Switching

| Command | Description | Effect |
|---------|-------------|--------|
| `WIFI:ON` | Switch to WiFi mode | BLE mati, WiFi AP aktif, WebSocket server start |

---

## Flutter Implementation Examples

### BLE Implementation

```dart
Future<void> setInjector(bool enable) async {
  String cmd = enable ? "INJECT:1" : "INJECT:0";
  await characteristic.write(cmd.codeUnits);
}

Future<void> switchToWiFi() async {
  await characteristic.write("WIFI:ON".codeUnits);
  // Wait 2 seconds for ESP32 to switch
  await Future.delayed(Duration(seconds: 2));
  // Now connect via WebSocket
}
```

### Unified Data Parser

```dart
class VotolDataModel {
  factory VotolDataModel.fromJson(Map<String, dynamic> json) {
    final type = json['type'] ?? 'full';
    
    if (type == 'fast') {
      return VotolDataModel._fromFastJson(json);
    } else {
      return VotolDataModel._fromFullJson(json);
    }
  }
  
  factory VotolDataModel._fromFastJson(Map<String, dynamic> json) {
    return VotolDataModel(
      rpm: json['r'] ?? 0,
      speed: json['s'] ?? 0,
      mode: json['m'] ?? 'PARK',
      volts: (json['v'] ?? 0.0).toDouble(),
      amps: (json['a'] ?? 0.0).toDouble(),
      power: (json['p'] ?? 0.0).toDouble(),
      soc: json['sc'] ?? 0,
      ctrlTemp: json['t']['c'] ?? 0,
      motorTemp: json['t']['m'] ?? 0,
      battTemp: json['t']['b'] ?? 0,
      canRate: json['cr'] ?? 0,
      heartbeat: json['hb'] ?? 0,
    );
  }
  
  factory VotolDataModel._fromFullJson(Map<String, dynamic> json) {
    return VotolDataModel(
      rpm: json['r'] ?? 0,
      speed: json['s'] ?? 0,
      mode: json['m'] ?? 'PARK',
      volts: (json['v'] ?? 0.0).toDouble(),
      amps: (json['a'] ?? 0.0).toDouble(),
      power: (json['p'] ?? 0.0).toDouble(),
      soc: json['sc'] ?? 0,
      ctrlTemp: json['t']['c'] ?? 0,
      motorTemp: json['t']['m'] ?? 0,
      battTemp: json['t']['b'] ?? 0,
      cells: List<int>.from(json['cells'] ?? []),
      cellDelta: json['cd'] ?? 0,
      soh: json['h']['soh'] ?? 0,
      cycles: json['h']['cyc'] ?? 0,
      remainCapacity: (json['h']['rc'] ?? 0.0).toDouble(),
      fullCapacity: (json['h']['fc'] ?? 0.0).toDouble(),
      highestCellVolt: json['cvs']['hi'] ?? 0,
      highestCellNum: json['cvs']['hiC'] ?? 0,
      lowestCellVolt: json['cvs']['lo'] ?? 0,
      lowestCellNum: json['cvs']['loC'] ?? 0,
      avgCellVolt: json['cvs']['av'] ?? 0,
      maxTemp: json['ts']['max'] ?? 0,
      maxTempCell: json['ts']['maxC'] ?? 0,
      minTemp: json['ts']['min'] ?? 0,
      minTempCell: json['ts']['minC'] ?? 0,
      balanceMode: json['b']['md'] ?? 0,
      balanceStatus: json['b']['st'] ?? 0,
      balanceCells: List<int>.from(json['b']['cells'] ?? []),
      chargerOn: json['chr']['on'] == 1,
      chargerVoltage: (json['chr']['v'] ?? 0.0).toDouble(),
      chargerCurrent: (json['chr']['a'] ?? 0.0).toDouble(),
      oriCharger: json['chr']['ori'] == 1,
      bmsHwVersion: json['bms']['hw'] ?? '',
      bmsFwVersion: json['bms']['fw'] ?? '',
      canRate: json['cr'] ?? 0,
      odometer: json['odo'] ?? 0,
      heartbeat: json['hb'] ?? 0,
    );
  }
}
```

---

## Adaptive Update Rates

Update rates menyesuaikan dengan mode kendaraan:

| Mode | Fast Update | Slow Update | Use Case |
|------|-------------|-------------|----------|
| **PARK** | 500ms (2 Hz) | 2000ms (0.5 Hz) | Hemat power saat parkir |
| **STAND** | 500ms (2 Hz) | 2000ms (0.5 Hz) | Standby mode |
| **CHARGING** | 500ms (2 Hz) | 2000ms (0.5 Hz) | Monitor charging progress |
| **DRIVE** | 100ms (10 Hz) | 1000ms (1 Hz) | Responsif saat berkendara |
| **SPORT** | 100ms (10 Hz) | 1000ms (1 Hz) | Maximum responsiveness |
| **REVERSE** | 100ms (10 Hz) | 1000ms (1 Hz) | Fast reverse monitoring |
| **BRAKE** | 50ms (20 Hz) | 1000ms (1 Hz) | **Super fast** - capture peak regen amps |

---

## Memory & Storage Requirements

### Partition Scheme
**WAJIB** menggunakan partition scheme OTA karena ukuran firmware dengan WiFi support lebih besar:

| Partition Scheme | App Size | Compatible |
|------------------|----------|------------|
| `Default 4MB` | 1.3 MB | ❌ Tidak cukup |
| `Minimal SPIFFS (1.9MB APP)` | 1.9 MB | ✅ **Recommended** |
| `No OTA (2MB APP)` | 2 MB | ⚠️ OTA tidak berfungsi |

**Arduino IDE**: `Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`

### Memory Usage

| Mode | Free Heap (est) | Notes |
|------|-----------------|-------|
| BLE Only | ~120 KB | Default mode |
| WiFi Only | ~100 KB | Slightly higher usage |
| BLE+WiFi Concurrent | ~60 KB | **Not used** - we switch, not concurrent |

### Compile Stats
```
Sketch uses 1719259 bytes (87%) of program storage space.
Global variables use 65140 bytes (19%) of dynamic memory.
```

---

## Testing Checklist

### BLE Mode
- [ ] Verify fast updates arrive at correct intervals
- [ ] Verify full updates include all cell data
- [ ] Test `INJECT:1` and `INJECT:0` commands
- [ ] Test `WIFI:ON` command triggers mode switch

### WiFi Mode
- [ ] Verify AP appears with correct SSID
- [ ] Verify WebSocket connection at `ws://192.168.4.1:81/ws`
- [ ] Verify JSON format identical to BLE
- [ ] Test auto-return to BLE after disconnect

### General
- [ ] Test mode transitions (PARK → DRIVE → BRAKE → CHARGING)
- [ ] Check real-time amp display is smooth in both modes
- [ ] Verify charger detection still works
- [ ] Test BMS version info display

---

**Last Updated**: 2026-02-03
**Firmware Version**: Dual-core v2.1.0 (BLE + WiFi Support)
