# Votol BLE Dashboard

[![Author](https://img.shields.io/badge/Author-Zekri%20R-blue)](https://zekri.id)
[![Platform](https://img.shields.io/badge/Platform-ESP32-green)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

Real-time monitoring dashboard for **Votol Controller** and **BMS (Battery Management System)** via CAN bus. Built with ESP32 + BLE (Bluetooth Low Energy) + Python Web Dashboard.

---

## ⚠️ DISCLAIMER / PERINGATAN

> **ENGLISH:**  
> This project is provided "AS IS" without any warranty. Use at your own risk.  
> The author (**Zekri R / ZEKRI.ID**) is **NOT responsible** for any damage, malfunction, or injury that may occur to your vehicle, controller, battery, or any other components.  
> Modifying or monitoring your electric vehicle's CAN bus may void your warranty and could be dangerous if done incorrectly.

> **BAHASA INDONESIA:**  
> Proyek ini disediakan "APA ADANYA" tanpa jaminan apapun. Gunakan dengan risiko Anda sendiri.  
> Penulis (**Zekri R / ZEKRI.ID**) **TIDAK bertanggung jawab** atas kerusakan, malfungsi, atau cedera yang mungkin terjadi pada kendaraan, controller, baterai, atau komponen lainnya.  
> Memodifikasi atau memonitor CAN bus kendaraan listrik Anda dapat membatalkan garansi dan berbahaya jika dilakukan secara tidak benar.

---

## 🎁 Donasi / Support

Jika project ini bermanfaat bagi Anda, Anda bisa mendukung pengembangan selanjutnya dengan berdonasi melalui QRIS berikut (Scan menggunakan GoPay, OVO, Dana, ShopeePay, BCA Mobile, dll):

<p align="center">
  <img src="QRIS.jpg" alt="QRIS Zekri R" width="200" />
</p>

Terima kasih atas dukungan Anda! 🙏

---

## 📋 Deskripsi Project

Sistem monitoring real-time untuk kendaraan listrik dengan **Votol Controller** dan **BMS** berbasis CAN bus. Data dikirim via Bluetooth Low Energy (BLE) ke Python web dashboard.

### Data yang Dimonitor

| Kategori | Data |
|----------|------|
| **Votol Controller** | RPM, Speed, Mode (PARK/DRIVE/SPORT/BRAKE/REVERSE/CHARGING), Controller Temp, Motor Temp |
| **BMS General** | Pack Voltage, Current, Power, SOC (State of Charge), SOH (State of Health), Cycle Count |
| **BMS Capacity** | Remaining Capacity (Ah), Full Charge Capacity (Ah) |
| **Cell Voltages** | 23 individual cell voltages (mV), Cell Delta, Highest/Lowest/Average Cell |
| **Temperatures** | 5 cell temperature sensors, Max/Min Temperature with cell number |
| **Balance Status** | Balance Mode, Balance Status, Individual cell balancing bitmask |
| **Charging Info** | Charger Connected Status, Charging Voltage/Current, **Original Charger Detection** |
| **Device Info** | BMS Hardware Version, BMS Firmware Version |

---

## 🔧 Hardware Requirements

### Komponen Utama
- **ESP32 DevKit** (ESP32-WROOM-32 atau sejenisnya)
- **CAN Transceiver Module** - pilih salah satu:
  - SN65HVD230 (3.3V, recommended)
  - MCP2515 (dengan level shifter jika perlu)
  - TJA1050 (5V, butuh level shifter)

### Wiring Diagram

```
┌─────────────────┐              ┌──────────────────┐
│     ESP32       │              │  CAN Transceiver │
│                 │              │   (SN65HVD230)   │
│  GPIO21 (TX) ───┼──────────────┼─── CTX           │
│  GPIO22 (RX) ───┼──────────────┼─── CRX           │
│  3.3V ──────────┼──────────────┼─── VCC           │
│  GND ───────────┼──────────────┼─── GND           │
└─────────────────┘              │                  │
                                 │  CANH ───────────┼──→ CAN High (Votol/BMS)
                                 │  CANL ───────────┼──→ CAN Low (Votol/BMS)
                                 └──────────────────┘
```

---

## 📁 Struktur Project

```
votol-esp32-can-bus/
├── README.md                    # Dokumentasi ini
├── QRIS.jpg                     # QR Code Donasi
├── esp32/
│   ├── votol_ble.ino            # Firmware ESP32 (single-core)
│   └── votol_ble_dualcore.ino   # ✅ Firmware ESP32 Dual-Core (RECOMMENDED)
└── python_web/
    ├── app_ble.py               # ✅ Flask server (BLE - RECOMMENDED)
    ├── requirements_ble.txt     # Dependencies (BLE)
    └── templates/
        └── dashboard.html       # Web dashboard UI
```

---

## ⚙️ Installation

### 1. Setup ESP32 Firmware

1. **Install Arduino IDE** dan tambahkan ESP32 board:
   - File → Preferences → Additional Board URLs:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   - Tools → Board → Boards Manager → Install "ESP32"

2. **Buka file** `esp32/votol_ble_dualcore.ino` di Arduino IDE
   > 💡 Gunakan versi **dual-core** untuk performa terbaik (zero CAN message loss)

3. **Pilih Board**: Tools → Board → ESP32 Dev Module

4. **Upload** ke ESP32

5. **Verifikasi** via Serial Monitor (115200 baud):
   ```
   === VOTOL BLE DASHBOARD (Dual-Core) ===
   ✓ BLE: Votol_BLE
   ✓ CAN: 250kbps
   ✓ Tasks: CAN@Core0, BLE@Core1
   ```

### 2. Setup Python Dashboard

```bash
cd python_web
pip install -r requirements_ble.txt
python app_ble.py
```

Dashboard akan tersedia di: **http://localhost:5000**

---

## 🎯 Features

### ✅ Real-time Monitoring
- Speed, RPM, Mode (PARK/DRIVE/SPORT/BRAKE/REVERSE)
- Pack Voltage, Current, Power
- SOC (%), SOH (%), Cycle Count
- Controller, Motor, Battery Temperatures

### ✅ Charging System Detection (NEW)
- Detects when Charger is Connected
- Identifies **Original Charger** vs Generic
- Monitors Charging Voltage and Current
- Special "CHARGING" mode indication

### ✅ BMS Cell Inspector
- 23 individual cell voltages
- Cell delta (mV) - difference between highest and lowest
- Highest/Lowest/Average cell statistics
- Color-coded alerts for imbalanced cells

### ✅ Device Information
- Reads BMS Hardware Version (e.g., H:v21)
- Reads BMS Firmware Version (e.g., F:v23)

### ✅ Temperature Monitoring
- 5 cell temperature sensors
- Max/Min temperature with cell identification
- Average battery temperature

### ✅ Balance Status
- Balance mode (Automatic/Mode1)
- Balance status (No Balance/Charge/Discharge/Standstill)
- Per-cell balancing indicator (23 cells)

### ✅ Dual Connection Mode
- **BLE (Primary)**: Auto-connect, stable, low power
- **USB Serial (Fallback)**: Direct connection for debugging

---

## 📊 JSON Data Format

ESP32 mengirim data dalam format JSON berikut:

```json
{
  "rpm": 1200,
  "speed": 124,
  "mode": "SPORT",
  "volts": 74.5,
  "amps": -15.2,
  "power": -1132,
  "soc": 85,
  "temps": {
    "ctrl": 45,
    "motor": 50,
    "batt": 35
  },
  "cells": [3900, 3905, 3898, ...],
  "cellDelta": 25,
  "canRate": 120,
  "health": {
    "soh": 98,
    "cycles": 42,
    "remainCap": 28.5,
    "fullCap": 30.0
  },
  "cellVoltStats": {
    "highest": 3920,
    "highestCell": 5,
    "lowest": 3895,
    "lowestCell": 18,
    "average": 3907,
    "delta": 25
  },
  "tempStats": {
    "max": 38,
    "maxCell": 2,
    "min": 32,
    "minCell": 4
  },
  "balance": {
    "mode": 0,
    "status": 1,
    "cells": [false, false, true, ...]
  },
  "charger": {
    "on": 0,
    "v": 0.0,
    "a": 0.0,
    "ori": 0
  },
  "bms": {
    "hw": "H:v21",
    "fw": "F:v23"
  },
  "hb": 12345
}
```

---

## 🔧 Troubleshooting

### ESP32 tidak terdeteksi Bluetooth
1. Cek Serial Monitor: pastikan ada "✓ BLE: Votol_BLE"
2. Restart ESP32
3. Pastikan tidak ada device lain yang connect ke ESP32

### Python error "bleak not found"
```bash
pip install bleak
```

### Dashboard tidak update
1. Check browser console (F12)
2. Pastikan connection status "CONNECTED" (hijau)
3. Cek Serial Monitor ESP32: pastikan ada JSON output

### CAN data tidak masuk
1. Periksa wiring CAN Transceiver
2. Pastikan CAN bus terhubung dengan benar (CANH/CANL)
3. Cek baud rate: 250kbps

---

## 🙏 Credits

**Author:** Zekri R ([ZEKRI.ID](https://zekri.id))

**Based on:** [displaypolytronfoxrs](https://github.com/yudhaime/displaypolytronfoxrs) by yudhaime

---

## 📝 License

MIT License

---

*Last Updated: January 2026*
