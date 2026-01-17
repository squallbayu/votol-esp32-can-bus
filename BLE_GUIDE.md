# BLE Setup Guide

[![BLE](https://img.shields.io/badge/Connection-BLE-blue)](https://www.bluetooth.com/)

Panduan lengkap untuk setup koneksi Bluetooth Low Energy (BLE) antara ESP32 dan Python Dashboard.

---

## ⚠️ DISCLAIMER

> Gunakan dengan risiko Anda sendiri. Penulis (**Zekri R / ZEKRI.ID**) **TIDAK bertanggung jawab** atas kerusakan apapun.  
> Lihat [README.md](README.md) untuk disclaimer lengkap.

---

## 📡 Kenapa BLE?

| Feature | BLE | Bluetooth Classic |
|---------|-----|-------------------|
| **Auto-reconnect** | ✅ Ya | ❌ Tidak |
| **Stabilitas** | ✅ Tinggi | ⚠️ Sering disconnect |
| **Power** | ✅ Hemat | ❌ Boros |
| **Range** | ~10m | ~10m |
| **Setup** | Auto-scan | Manual pair |
| **Latency** | ~20ms | ~100ms |

---

## ⚙️ Setup ESP32 (Firmware BLE)

### 1. Upload Firmware

```
File: esp32/votol_ble.ino
```

1. Buka `esp32/votol_ble.ino` di Arduino IDE
2. Pilih Board: **ESP32 Dev Module**
3. Upload ke ESP32
4. Cek Serial Monitor (115200 baud):

```
=== VOTOL BLE DASHBOARD v2.2 (Hybrid + Stable) ===
[MEM] Last SOC loaded: 85%
✓ BLE: Votol_BLE
✓ CAN: 250kbps
```

### 2. BLE Service Configuration

ESP32 menggunakan GATT (Generic Attribute Profile) dengan konfigurasi:

| Parameter | Value |
|-----------|-------|
| **Device Name** | `Votol_BLE` |
| **Service UUID** | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| **Characteristic UUID** | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| **MTU Size** | 512 bytes |
| **Properties** | READ, NOTIFY |

---

## 🐍 Setup Python Dashboard

### 1. Install Dependencies

```bash
cd python_web
pip install -r requirements_ble.txt
```

Isi `requirements_ble.txt`:
```
flask>=2.0
flask-socketio>=5.0
bleak>=0.19
pyserial>=3.5
```

### 2. Run Dashboard

```bash
python app_ble.py
```

Output yang diharapkan:
```
==================================================
   VOTOL BLE + USB DASHBOARD
==================================================
Starting BLE connection...
Starting USB Serial reader...
Dashboard: http://localhost:5000
==================================================

[BLE] Scanning for Votol_BLE...
[BLE] Found Votol_BLE, connecting...
[BLE] Connected!
```

### 3. Buka Browser

```
http://localhost:5000
```

---

## 📊 JSON Data Format

ESP32 mengirim data via BLE dalam format JSON. Data di-chunk menjadi paket 500 bytes karena limitasi BLE MTU.

### Full JSON Structure

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
  "odometer": 7990,
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
  "debug": {
    "currentHex": "0x1234",
    "voltageHex": "0x02E5",
    "socHex": "0x0355",
    "balanceHex": "00 01 04 00 00 00"
  },
  "heartbeat": 12345
}
```

### Data Fields Explanation

| Field | Type | Description |
|-------|------|-------------|
| `rpm` | int | Motor RPM dari Votol Controller |
| `speed` | int | Kecepatan (km/h), dihitung dari RPM × 0.1033 |
| `mode` | string | Mode: PARK, DRIVE, SPORT, BRAKE, REVERSE, STAND |
| `volts` | float | Total pack voltage (V) |
| `amps` | float | Current (A), negatif = discharge, positif = charge |
| `power` | float | Power (W) = volts × amps |
| `soc` | int | State of Charge (%), menggunakan lookup table |
| `temps.ctrl` | int | Controller temperature (°C) |
| `temps.motor` | int | Motor temperature (°C) |
| `temps.batt` | int | Average battery temperature (°C) |
| `cells` | array[23] | Individual cell voltages (mV) |
| `cellDelta` | int | Max - Min cell voltage (mV) |
| `health.soh` | int | State of Health (%) |
| `health.cycles` | int | Charge cycle count |
| `balance.status` | int | 0=No, 1=Charge, 2=Discharge, 3=Standstill |

---

## 🔧 Troubleshooting

### BLE device not found

- Pastikan ESP32 sudah upload firmware BLE
- Restart ESP32
- Cek Serial Monitor: harus ada `✓ BLE: Votol_BLE`
- Matikan Bluetooth device lain yang mungkin interfere

### Python error: "bleak not found"

```bash
pip install bleak
```

### Connection timeout

- Terlalu banyak Bluetooth device aktif di sekitar
- Disable BT device lain yang tidak diperlukan
- Restart Bluetooth adapter di laptop/PC

### Auto-reconnect tidak jalan

- Normal! BLE scan ulang setiap 3 detik
- Tunggu ~5 detik untuk reconnect otomatis

### Data tidak update / 0 bytes received

- Pastikan ESP32 terhubung ke CAN bus
- Cek wiring ke CAN transceiver
- Coba gunakan USB Serial sebagai fallback (colok USB ke PC)

---

## 💡 Tips

- **BLE lebih stabil** dari Bluetooth Classic
- **Tidak perlu pairing** manual di Windows/Linux
- **Auto-reconnect** otomatis saat connection terputus
- **USB Serial fallback** - colok USB untuk koneksi langsung
- **Dashboard tetap sama** antara BLE dan USB mode

---

## 🔄 Update Firmware (OTA)

OTA (Over-The-Air) update tersedia jika WiFi diaktifkan:

1. Connect ke WiFi AP "Votol_OTA"
2. Arduino IDE → Tools → Port → Select Network Port
3. Upload seperti biasa

> Note: BLE tetap berjalan selama proses OTA!

---

## 🙏 Credits

**Author:** Zekri R ([ZEKRI.ID](https://zekri.id))

**Based on:** [displaypolytronfoxrs](https://github.com/yudhaime/displaypolytronfoxrs) by yudhaime

---

*Last Updated: January 2026*
