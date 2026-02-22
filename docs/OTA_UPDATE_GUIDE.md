# 📘 Cara Upload OTA Firmware ke ESP32

Panduan lengkap untuk upload firmware ESP32 dengan OTA (Over-The-Air) via Bluetooth.

---

## ⚠️ Langkah Pertama: Flash dengan Partition Table OTA

**PENTING:** OTA memerlukan partition table khusus yang **berbeda** dengan default Arduino. Anda hanya perlu melakukan ini **SEKALI** saat pertama kali setup.

### Mengapa Perlu Partition Table OTA?

ESP32 memiliki flash memory 4MB yang dibagi menjadi beberapa partisi:

| Partition Default | Ukuran | Keterangan |
|-------------------|--------|------------|
| app | 1.3 MB | Firmware aplikasi |
| nvs | 24 KB | Penyimpanan data |
| spiffs | 1.5 MB | File system |

Dengan partition table OTA, struktur menjadi:

| Partition OTA | Ukuran | Keterangan |
|---------------|--------|------------|
| app0 | 1.3 MB | Firmware aktif saat ini |
| app1 | 1.3 MB | Firmware baru (OTA target) |
| nvs | 24 KB | Penyimpanan data |

Saat OTA update:
1. Firmware baru ditulis ke `app1`
2. ESP32 reboot dan boot dari `app1`
3. Jika gagal, otomatis rollback ke `app0` ✅

---

## 🔧 Cara Setup Partition Table OTA

### Di Arduino IDE

1. **Buka Arduino IDE**
2. **Buka file** `votol_ble_dualcore.ino`
3. **Pilih Tools → Partition Scheme**
4. **Pilih:** `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`
   
   ![Partition Scheme](https://i.imgur.com/partition_table_example.png)
   
   Alternatif lain yang bisa digunakan:
   - `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)` + OTA
   - `No OTA (2MB APP/2MB SPIFFS)` ❌ **JANGAN PILIH INI**

5. **Compile dan Upload via USB** (pertama kali saja!)

   ```
   Arduino IDE → Sketch → Upload
   ```

6. **Setelah berhasil**, lihat output Serial Monitor:
   ```
   [OTA] Service initialized
   [OTA] Firmware version: 1.0.0
   ```

**🎉 Selesai!** Anda tidak perlu kabel USB lagi untuk update berikutnya.

---

### Di PlatformIO (Alternatif)

Jika menggunakan PlatformIO, tambahkan di `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
board_build.partitions = min_spiffs.csv  ; OTA with minimal SPIFFS
```


## 🔨 Cara Membuat File Firmware (.bin)

Sebelum upload OTA, Anda perlu build firmware jadi file `.bin` terlebih dahulu.

### Di Arduino IDE

**Langkah 1: Pastikan Partition Table Sudah Benar**

```
Tools → Partition Scheme → "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
```

**Langkah 2: Export Compiled Binary**

Ada 2 cara:

#### Cara A: Export Tanpa Upload (Recommended)

1. Buka file `votol_ble_dualcore.ino`
2. Klik menu **Sketch → Export Compiled Binary**
3. Tunggu proses compile selesai (2-3 menit)
4. File `.bin` akan disimpan di **folder yang sama** dengan file `.ino`

```
/home/zekri/Downloads/votol-esp32-can-bus/esp32/votol_ble_dualcore/
├── votol_ble_dualcore.ino
├── votol_ble_dualcore.ino.bin           ← File firmware OTA
├── votol_ble_dualcore.ino.bootloader.bin
└── votol_ble_dualcore.ino.partitions.bin
```

**Yang Anda butuhkan:** File `votol_ble_dualcore.ino.bin` (yang tidak ada embel-embel bootloader/partitions)

#### Cara B: Cari di Build Folder (Alternatif)

Jika metode A tidak berhasil, cari di build folder:

1. Klik **Verify** (✓) atau **Upload** (→)
2. Lihat output console, cari baris seperti:
   ```
   Sketch uses xxxxxx bytes (XX%) of program storage space.
   ```
3. File `.bin` ada di:
   ```
   /tmp/arduino_build_xxxxxx/votol_ble_dualcore.ino.bin
   ```

**Tips:** Copy file `.bin` ke folder yang mudah diakses (Desktop/Downloads) sebelum upload OTA.

---

### Di PlatformIO (Alternatif)

Jika menggunakan PlatformIO:

```bash
# Build firmware
pio run

# File .bin akan ada di:
.pio/build/esp32dev/firmware.bin
```

---

## 🚀 Cara Update Firmware via OTA


### Opsi 1: Web OTA Tool (Recommended)

**File:** `ota_web/index.html`

#### Langkah-langkah:

1. **Buka file HTML** di browser Chrome atau Edge
   
   ```bash
   # Linux/Mac
   xdg-open ota_web/index.html
   
   # Windows
   start ota_web/index.html
   ```

2. **Klik "Connect ke Votol_BLE"**
   
   Browser akan muncul popup pilihan Bluetooth → Pilih `Votol_BLE`

3. **Pilih file firmware .bin**
   
   File `.bin` ada di folder output Arduino IDE:
   ```
   Arduino IDE → Sketch → Export Compiled Binary
   ```
   
   File akan disimpan di:
   ```
   /tmp/arduino_build_xxxxxx/votol_ble_dualcore.ino.bin
   ```

4. **Klik "Upload Firmware"**
   
   Progress bar akan muncul. Tunggu sampai 100%.

5. **ESP32 reboot otomatis**
   
   Setelah selesai, ESP32 akan restart dengan firmware baru!

---

### Opsi 2: Via Flutter App (Nanti)

Dokumentasi akan ditambahkan setelah Flutter integration selesai.

---

## 🧪 Testing OTA Update

### Test 1: Upload Firmware yang Sama

**Tujuan:** Memastikan OTA berfungsi tanpa resiko.

1. Build firmware versi saat ini
2. Upload via Web OTA tool
3. ESP32 harus reboot dan kembali normal

**Expected:** Tidak ada perubahan, tapi proses OTA berhasil.

---

### Test 2: Upload dengan Perubahan Kecil

**Tujuan:** Verifikasi bahwa firmware baru berjalan.

1. Edit `FIRMWARE_VERSION` di `votol_ota.h`:
   ```cpp
   #define FIRMWARE_VERSION "1.0.1"  // Ubah dari 1.0.0
   ```

2. Build dan upload firmware baru via OTA
3. Connect lagi via Web OTA tool
4. Klik "Connect" → Versi firmware harus berubah ke `1.0.1`

**Expected:** Versi firmware berubah.

---

### Test 3: Rollback Test

**Tujuan:** Pastikan rollback bekerja jika firmware gagal.

1. Buat firmware yang **sengaja rusak** (misalnya hapus `BLEDevice::init()`)
2. Upload via OTA
3. ESP32 akan gagal boot → otomatis rollback ke firmware lama
4. ESP32 kembali online dengan firmware sebelumnya

**Expected:** ESP32 tidak brick, kembali ke firmware lama.

---

## ❓ Troubleshooting

### Error: "Partition scheme tidak support OTA"

**Solusi:**
- Pastikan partition scheme sudah `Minimal SPIFFS with OTA`
- Re-upload via USB dengan partition table yang benar

### Error: "Upload gagal di 50%"

**Solusi:**
- Pastikan ESP32 dan PC/HP cukup dekat (< 5 meter)
- Tutup aplikasi Bluetooth lain yang sedang connect
- Coba lagi dari awal

### Error: "Browser tidak support Web Bluetooth"

**Solusi:**
- Gunakan Chrome atau Edge (terbaru)
- Firefox dan Safari belum support Web Bluetooth
- Atau gunakan Flutter app (nanti)

### ESP32 tidak muncul di daftar Bluetooth

**Solusi:**
- Pastikan ESP32 sudah booting (tunggu 10 detik)
- Cek LED blink (tanda CAN aktif)
- Restart ESP32 (tekan tombol RESET)

---

## 📝 Catatan Penting

1. **Ukuran firmware maksimal:** 1.3 MB (OTA partition limit)
2. **Waktu upload:** ~2-5 menit tergantung ukuran firmware
3. **Jangan cabut power** saat proses OTA berlangsung!
4. **Backup firmware lama:** Simpan file `.bin` versi sebelumnya untuk berjaga-jaga

---

## 🎯 Summary

| Item | Penjelasan |
|------|------------|
| **First-time setup** | Flash via USB dengan partition table OTA |
| **Update selanjutnya** | Via Web OTA tool (tanpa kabel) |
| **Rollback otomatis** | Ya, jika firmware baru gagal boot |
| **Keamanan** | Tidak ada password (bisa ditambahkan nanti) |

---

**Selamat mencoba! 🚀**

Jika ada pertanyaan, silakan hubungi [ZEKRI.ID](https://zekri.id)
