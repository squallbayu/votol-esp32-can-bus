/*
 * =============================================
 * VOTOL BLE OTA SERVICE - Implementation
 * =============================================
 */
#include "votol_ota.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Update.h>
#include <atomic>

// Firmware version (update this when you release new versions)
#define FIRMWARE_VERSION "2.0.1"

// Global OTA state - atomic for cross-core safety (BLE callback vs commTask)
static std::atomic<bool> otaInProgress{false};
static std::atomic<uint32_t> otaExpectedSize{0};
static std::atomic<uint32_t> otaReceivedSize{0};
static std::atomic<uint32_t> otaLastChunkTime{0};
static std::atomic<bool> otaRebootPending{false};  // Set by callback, handled by commTask

// BLE Characteristic pointers
static BLECharacteristic* pOtaControlChar = nullptr;
static BLECharacteristic* pOtaDataChar = nullptr;
static BLECharacteristic* pOtaStatusChar = nullptr;

// Progress tracking (moved to global for proper reset)
static uint8_t otaLastProgress = 0;

static void sendOtaStatus(uint8_t status, uint8_t progress = 0);
static void abortOta(uint8_t errorCode);
static void resetOtaProgress();

// =============================================
// OTA CONTROL CALLBACK
// =============================================
class OtaControlCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    uint8_t* data = pCharacteristic->getData();
    size_t len = pCharacteristic->getValue().length();
    if (len < 1) return;

    uint8_t cmd = data[0];

    switch (cmd) {
      case OTA_CMD_BEGIN: {
        if (len < 5) {
          sendOtaStatus(OTA_STATUS_ERR_SIZE);
          return;
        }

        // Extract firmware size (4 bytes, little-endian)
        uint32_t fwSize = (uint32_t)data[1] |
                          ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) |
                          ((uint32_t)data[4] << 24);

        // Validate size (max app partition size: 0x1E0000 = 1,966,080 bytes)
        if (fwSize == 0 || fwSize > 1966080) {
          sendOtaStatus(OTA_STATUS_ERR_SIZE);
          return;
        }

        // Begin OTA update
        if (!Update.begin(fwSize)) {
          sendOtaStatus(OTA_STATUS_ERR_SIZE);
          return;
        }

        otaExpectedSize.store(fwSize, std::memory_order_release);
        otaInProgress.store(true, std::memory_order_release);
        otaReceivedSize.store(0, std::memory_order_release);
        otaLastChunkTime.store(millis(), std::memory_order_release);
        resetOtaProgress();

        Serial.printf("[OTA] Begin - expecting %lu bytes\n", (unsigned long)fwSize);
        sendOtaStatus(OTA_STATUS_READY);
        break;
      }

      case OTA_CMD_END: {
        if (!otaInProgress.load(std::memory_order_acquire)) {
          sendOtaStatus(OTA_STATUS_ERROR);
          return;
        }

        if (otaReceivedSize.load(std::memory_order_acquire) != otaExpectedSize.load(std::memory_order_acquire)) {
          Serial.printf("[OTA] Size mismatch: received %lu, expected %lu\n",
                        (unsigned long)otaReceivedSize.load(std::memory_order_acquire),
                        (unsigned long)otaExpectedSize.load(std::memory_order_acquire));
          abortOta(OTA_STATUS_ERR_SIZE);
          return;
        }

        if (!Update.end(true)) {
          Serial.println("[OTA] Update.end() failed");
          abortOta(OTA_STATUS_ERR_VERIFY);
          return;
        }

        otaInProgress.store(false, std::memory_order_release);
        Serial.println("[OTA] Complete! Reboot scheduled...");
        sendOtaStatus(OTA_STATUS_COMPLETE, 100);

        // Don't delay+restart inside BLE callback (blocks Bluedroid stack)
        // Set flag for commTask to handle reboot safely
        otaRebootPending.store(true, std::memory_order_release);
        break;
      }

      case OTA_CMD_ABORT: {
        abortOta(OTA_STATUS_ERR_ABORT);
        break;
      }

      case OTA_CMD_VERSION: {
        String version = FIRMWARE_VERSION;
        pOtaStatusChar->setValue(version.c_str());
        pOtaStatusChar->notify();
        break;
      }

      default:
        Serial.printf("[OTA] Unknown command: 0x%02X\n", cmd);
        break;
    }
  }
};

// =============================================
// OTA DATA CALLBACK (receives firmware chunks)
// =============================================
class OtaDataCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    if (!otaInProgress.load(std::memory_order_acquire)) {
      return;
    }

    uint8_t* data = pCharacteristic->getData();
    size_t len = pCharacteristic->getValue().length();
    if (len == 0) return;

    // Write chunk to flash
    size_t written = Update.write(data, len);

    if (written != len) {
      Serial.printf("[OTA] Write error: expected %d, wrote %d\n",
                    (int)len, (int)written);
      abortOta(OTA_STATUS_ERR_WRITE);
      return;
    }

    otaReceivedSize.fetch_add(written, std::memory_order_release);
    otaLastChunkTime.store(millis(), std::memory_order_release);

    // Calculate and send progress
    uint32_t received = otaReceivedSize.load(std::memory_order_acquire);
    uint32_t expected = otaExpectedSize.load(std::memory_order_acquire);
    uint8_t progress = (uint8_t)((received * 100) / expected);

    // Send progress every 5% (using global otaLastProgress)
    if (progress >= otaLastProgress + 5 || progress == 100) {
      sendOtaStatus(OTA_STATUS_RECEIVING, progress);
      otaLastProgress = progress;
      Serial.printf("[OTA] Progress: %d%% (%lu/%lu bytes)\n",
                    progress, (unsigned long)received, (unsigned long)expected);
    }
  }
};

// =============================================
// HELPER FUNCTIONS
// =============================================
static void sendOtaStatus(uint8_t status, uint8_t progress) {
  if (pOtaStatusChar == nullptr) return;

  uint8_t data[2] = { status, progress };
  pOtaStatusChar->setValue(data, 2);
  pOtaStatusChar->notify();
}

static void abortOta(uint8_t errorCode) {
  if (otaInProgress.load(std::memory_order_acquire)) {
    Update.abort();
    otaInProgress.store(false, std::memory_order_release);
    otaReceivedSize.store(0, std::memory_order_release);
    otaExpectedSize.store(0, std::memory_order_release);
    otaLastProgress = 0;
    Serial.printf("[OTA] Aborted with error: 0x%02X\n", errorCode);
  }
  sendOtaStatus(errorCode);
}

static void resetOtaProgress() {
  otaLastProgress = 0;
}

void checkOtaTimeout() {
  if (otaInProgress.load(std::memory_order_acquire) && 
      (millis() - otaLastChunkTime.load(std::memory_order_acquire) > OTA_TIMEOUT_MS)) {
    Serial.println("[OTA] Timeout - aborting");
    abortOta(OTA_STATUS_ERROR);
  }
}

bool isOtaRebootPending() {
  return otaRebootPending.load(std::memory_order_acquire);
}

// =============================================
// SETUP OTA SERVICE
// =============================================
void setupOtaService(BLEServer* pServer) {
  // Create OTA Service
  BLEService* pOtaService = pServer->createService(OTA_SERVICE_UUID);

  // Static callbacks to prevent memory leak
  static OtaControlCallback otaControlCallback;
  static OtaDataCallback otaDataCallback;
  static BLE2902 otaStatusDescriptor;

  // Control Characteristic (write only)
  pOtaControlChar = pOtaService->createCharacteristic(
    OTA_CONTROL_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pOtaControlChar->setCallbacks(&otaControlCallback);

  // Data Characteristic (write only, large MTU)
  pOtaDataChar = pOtaService->createCharacteristic(
    OTA_DATA_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pOtaDataChar->setCallbacks(&otaDataCallback);

  // Status Characteristic (notify only)
  pOtaStatusChar = pOtaService->createCharacteristic(
    OTA_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pOtaStatusChar->addDescriptor(&otaStatusDescriptor);

  // Start service
  pOtaService->start();

  Serial.println("[OTA] Service initialized");
  Serial.printf("[OTA] Firmware version: %s\n", FIRMWARE_VERSION);
}

bool isOtaInProgress() {
  return otaInProgress.load(std::memory_order_acquire);
}
