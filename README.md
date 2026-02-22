# Votol BLE+WiFi Dashboard

[![Author](https://img.shields.io/badge/Author-Zekri%20R-blue)](https://zekri.id)
[![Platform](https://img.shields.io/badge/Platform-ESP32-green)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

Real-time monitoring dashboard for **Votol Controller** and **BMS (Battery Management System)** via CAN bus. Built with **ESP32** and a **Single-Binary Go Application**.

> **NEW**: Now powered by Go! No Python installation required. Just download and run.

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

Jika project ini bermanfaat bagi Anda, Anda bisa mendukung pengembangan selanjutnya dengan berdonasi melalui QRIS berikut:

<p align="center">
  <img src="QRIS.jpg" alt="QRIS Zekri R" width="200" />
</p>

Terima kasih atas dukungan Anda! 🙏

---

## 📋 Deskripsi Project

Sistem monitoring real-time untuk kendaraan listrik dengan **Votol Controller** dan **BMS** berbasis CAN bus. Data dikirim via Bluetooth Low Energy (BLE) atau WiFi ke dashboard.

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

---

## 📥 Download & Jalankan (Cara Mudah)

Anda tidak perlu menginstall Python atau dependency apapun. Cukup download binary yang sudah di-compile dari halaman **Releases**.

### 1. Download Aplikasi
Kunjungi halaman **[Releases](../../releases)** dan download file sesuai sistem operasi Anda:

- **Windows**: `votol-dashboard-windows-amd64.exe`
- **Linux**: `votol-dashboard-linux-amd64` (PC) atau `votol-dashboard-linux-arm64` (Raspberry Pi)
- **macOS**: `votol-dashboard-darwin-amd64` (Intel) atau `votol-dashboard-darwin-arm64` (Apple Silicon)
- **Firmware ESP32**: `firmware.bin`

> 🍎 **Note untuk macOS:** Binary macOS di build secara native (tanpa cross-compilation) sehingga fitur BLE berfungsi 100% menggunakan Apple CoreBluetooth.

### 2. Jalankan Aplikasi
**Windows**: 
Double-click `votol-dashboard-windows-amd64.exe`. Browser akan otomatis terbuka.

**Linux / macOS**:
Buka terminal dan jalankan:
```bash
chmod +x votol-dashboard-linux-amd64  # Sesuaikan dengan nama file yang di-download
./votol-dashboard-linux-amd64
```

Web dashboard akan otomatis terbuka dan mencoba mendeteksi ESP32 via BLE, USB Serial, atau integrasi WiFi.

---

## 🔧 Setup Hardware (ESP32)

### 1. Wiring Diagram
Hubungkan ESP32 dengan CAN Transceiver (SN65HVD230 recommended):

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

### 2. Flash Firmware
1. Download `firmware.bin` dari halaman **Releases**.
2. Gunakan [ESP Web Tools](https://espressif.github.io/esptool-js/) atau `esptool.py` untuk flash ke ESP32.
   ```bash
   esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x10000 firmware.bin
   ```
   
> ⚡ **FITUR DUAL-CORE**: Firmware ini sekarang menggunakan arsitektur Dual-Core (Core 0 khusus untuk membaca CAN Bus, Core 1 untuk BLE/WiFi/OTA) sehingga bebas bottleneck dan jauh lebih responsif!

---

## 🎯 Features

### ✅ Real-time Monitoring & Logging
- Speed, RPM, Mode (PARK/DRIVE/SPORT/BRAKE/REVERSE)
- Pack Voltage, Current, Power
- SOC (%), SOH (%), Cycle Count
- Controller, Motor, Battery Temperatures

### ✅ Advanced Charging System (Injector)
- Deteksi otomatis saat Charger terhubung (Original vs Generic)
- Fitur **CAN Injector** untuk mensimulasikan protokol original charger, memungkinkan charging dengan charger 3rd-party.
- Mode "CHARGING" otomatis menyesuaikan kecepatan parsing untuk menghemat CPU.

### ✅ Terintegrasi dengan OTA (Over-The-Air)
- Update firmware ESP32 langsung melalui Web Dashboard via WiFi, tanpa perlu mencolok kabel USB lagi!

### ✅ Dual Transport Mode
- **BLE Mode (Default)**: Auto-connect, hemat daya, sangat stabil.
- **WiFi Mode**: Range lebih jauh, digunakan untuk fitur OTA (Over-The-Air) update. Transisi BLE ↔ WiFi diatur secara mulus.
- **USB Serial**: Fallback mode untuk debugging langsung.

---

## 💻 Build from Source (Advanced)

Jika Anda ingin memodifikasi kode Go atau Firmware secara mandiri:

### Go Dashboard
Kode Go sekarang menggunakan build tags untuk memisahkan implementasi BLE CGO.
```bash
cd go_web
# Compile dengan BLE (hanya bisa di Linux atau macOS lokal)
CGO_ENABLED=1 go build -o votol-dashboard .

# Compile tanpa BLE (untuk cross-compilation CI)
CGO_ENABLED=0 go build -o votol-dashboard .
```

### ESP32 Firmware
Gunakan **Arduino IDE** dengan konfigurasi berikut:
1. Instal ESP32 Board Manager versi **2.0.17** (Versi 3.x tidak kompatibel dengan library AsyncTCP).
2. Board: **ESP32 Dev Module**
3. Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** *(WAJIB! Firmware ini terlalu besar untuk default scheme).*
4. Pastikan menginstal library `AsyncTCP` dan `ESPAsyncWebServer` langsung dari [repo me-no-dev](https://github.com/me-no-dev/ESPAsyncWebServer).

Buka `esp32/votol_ble_dualcore/votol_ble_dualcore.ino` dan klik Upload.

---

## 📝 License
MIT License

*Copyright (c) 2026 Zekri R*

