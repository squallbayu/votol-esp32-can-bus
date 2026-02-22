/*
 * =============================================
 * VOTOL BLE OTA SERVICE
 * =============================================
 * 
 * BLE Over-The-Air Update Service for ESP32
 * Allows firmware updates via Bluetooth from:
 * - Web Browser (Chrome/Edge with Web Bluetooth)
 * - Flutter App
 * 
 * Author: Zekri R (ZEKRI.ID)
 * 
 * =============================================
 * OTA PROTOCOL
 * =============================================
 * 1. Client writes OTA_CMD_BEGIN to Control characteristic
 * 2. Client sends firmware chunks (512 bytes) to Data characteristic
 * 3. ESP32 notifies progress via Status characteristic
 * 4. Client writes OTA_CMD_END to finalize
 * 5. ESP32 reboots with new firmware
 * =============================================
 */

#ifndef VOTOL_OTA_H
#define VOTOL_OTA_H

#include <BLEServer.h>

// OTA Service UUIDs
#define OTA_SERVICE_UUID        "fb1e4001-54ae-4a28-9f74-dfccb248601d"
#define OTA_CONTROL_UUID        "fb1e4002-54ae-4a28-9f74-dfccb248601d"
#define OTA_DATA_UUID           "fb1e4003-54ae-4a28-9f74-dfccb248601d"
#define OTA_STATUS_UUID         "fb1e4004-54ae-4a28-9f74-dfccb248601d"

// OTA Commands (write to Control characteristic)
#define OTA_CMD_BEGIN     0x01  // Start OTA, next 4 bytes = firmware size (little-endian)
#define OTA_CMD_END       0x02  // Finalize OTA
#define OTA_CMD_ABORT     0x03  // Abort OTA
#define OTA_CMD_VERSION   0x04  // Request version info

// OTA Status codes (notify via Status characteristic)
#define OTA_STATUS_IDLE       0x00
#define OTA_STATUS_READY      0x01
#define OTA_STATUS_RECEIVING  0x02
#define OTA_STATUS_COMPLETE   0x03
#define OTA_STATUS_ERROR      0x10
#define OTA_STATUS_ERR_SIZE   0x11
#define OTA_STATUS_ERR_WRITE  0x12
#define OTA_STATUS_ERR_VERIFY 0x13
#define OTA_STATUS_ERR_ABORT  0x14

// OTA Configuration
#define OTA_CHUNK_SIZE 512
#define OTA_TIMEOUT_MS 30000  // 30 seconds timeout between chunks

// OTA API
void setupOtaService(BLEServer* pServer);
void checkOtaTimeout();
bool isOtaInProgress();
bool isOtaRebootPending();  // Check if OTA completed and reboot needed

#endif // VOTOL_OTA_H
