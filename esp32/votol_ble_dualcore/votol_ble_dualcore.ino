/*
 * =============================================
 * VOTOL BLE+WIFI DASHBOARD - ESP32 DUAL-CORE VERSION
 * =============================================
 * 
 * Author: Zekri R (ZEKRI.ID)
 * Website: https://zekri.id
 * 
 * Based on: https://github.com/yudhaime/displaypolytronfoxrs
 * 
 * =============================================
 * DUAL-CORE ARCHITECTURE
 * =============================================
 * Core 0: CAN Task (High Priority) - Dedicated CAN bus reading
 * Core 1: Comm Task - Handle BLE or WiFi data transmission
 * 
 * Transport Modes (Dual-Mode Startup):
 * - BOTH BLE + WiFi start active on boot for maximum accessibility
 * - First connection (BLE or WiFi) auto-disables the unused mode
 * - On disconnect, both modes re-activate (dual-mode ready again)
 * 
 * Benefits:
 * - Universal access: Android/iPhone/PC can all connect immediately
 * - Power efficient after connection (only one mode active)
 * - No manual mode switching required
 * =============================================
 * 
 * DISCLAIMER / PERINGATAN
 * =============================================
 * This project is provided "AS IS" without any warranty.
 * Use at your own risk. The author is NOT responsible
 * for any damage, malfunction, or injury to your vehicle,
 * controller, battery, or any other components.
 * =============================================
 */
#include <Arduino.h>
#include "driver/twai.h"
#include <atomic>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <Preferences.h>
#include "votol_ota.h"
#include "wifi_handler.h"

// === Vehicle Mode Enum (thread-safe) ===
// Shared definition in wifi_handler.h, guarded against duplicate
#ifndef VEHICLE_MODE_ENUM_DEFINED
#define VEHICLE_MODE_ENUM_DEFINED
enum VehicleMode : uint8_t {
  MODE_PARK = 0,
  MODE_STAND,
  MODE_CHARGING,
  MODE_DRIVE,
  MODE_SPORT,
  MODE_REVERSE,
  MODE_BRAKE
};
#endif

// getModeString() and modeStrings[] now defined in wifi_handler.h

// === Transport Mode Enum (BLE vs WiFi) ===
#ifndef TRANSPORT_MODE_ENUM_DEFINED
#define TRANSPORT_MODE_ENUM_DEFINED
enum TransportMode : uint8_t {
  TRANSPORT_BLE = 0,      // BLE active, WiFi off
  TRANSPORT_WIFI,         // WiFi active, BLE off
  TRANSPORT_SWITCHING     // Transition state
};
#endif

// === CONFIGURATION ===
#define CAN_TX_PIN GPIO_NUM_21
#define CAN_RX_PIN GPIO_NUM_22
#define DEVICE_NAME "Votol_BLE"

#define LED_PIN 2

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"



// Timing - ADAPTIVE MODE-BASED for optimal performance
// Format: { FAST_MS, SLOW_MS }
// PARK/STAND: Slow updates, save power
static const uint32_t PARK_FAST_MS = 500;
static const uint32_t PARK_SLOW_MS = 2000;

// CHARGING: Medium speed, monitor charging data
static const uint32_t CHARGING_FAST_MS = 500;
static const uint32_t CHARGING_SLOW_MS = 2000;

// DRIVE/SPORT/REVERSE: Fast updates for real-time responsiveness
static const uint32_t DRIVE_FAST_MS = 100;
static const uint32_t DRIVE_SLOW_MS = 1000;

// BRAKE: Super fast for capturing peak regenerative amps
static const uint32_t BRAKE_FAST_MS = 50;
static const uint32_t BRAKE_SLOW_MS = 1000;

// Timeout constants (ms)
static const uint32_t CHARGER_TIMEOUT_MS = 5000;    // Reset charger status after no CAN msg
static const uint32_t ORI_CHARGER_TIMEOUT_MS = 5000; // Reset ORI charger detection
static const uint32_t SOC_PERIODIC_SAVE_MS = 3600000; // SOC periodic backup (1 hour)
static const uint32_t CAN_TIMEOUT_MS = 30000;       // CAN timeout = motor off (30s)
static const uint32_t INJECTOR_INTERVAL_MS = 500;   // Charger inject message interval
static const uint32_t MUTEX_TIMEOUT_MS = 5;         // Default mutex wait timeout

// BLE chunking - OPTIMIZED
static const uint16_t BLE_SAFE_CHUNK = 240;       // Keep safe for compatibility
static const uint32_t BLE_PUMP_BUDGET_US = 10000; // 10ms budget (was 6ms)
static const int BLE_PUMP_MAX_CHUNKS = 4;         // Less chunks per loop (was 12)

// Task configuration - OPTIMIZED
#define CAN_TASK_STACK_SIZE 4096
#define BLE_TASK_STACK_SIZE 8192
#define CAN_TASK_PRIORITY 3    // Higher priority for real-time CAN (was 2)
#define BLE_TASK_PRIORITY 1

// === RTOS Objects ===
SemaphoreHandle_t dataMutex = NULL;
SemaphoreHandle_t nvsMutex = NULL;  // Fix #2: Dedicated mutex for NVS access
TaskHandle_t canTaskHandle = NULL;
TaskHandle_t bleTaskHandle = NULL;

BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
std::atomic<bool> deviceConnected{false};  // Atomic: BLE callback (any core) + BLE task
bool oldDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected.store(true, std::memory_order_release);
    Serial.println("[BLE] Client connected - staying in BLE mode");
    // WiFi should already be off when BLE is active
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected.store(false, std::memory_order_release);
    Serial.println("[BLE] Client disconnected (Staying in BLE mode)");
    // Do NOT switch to WiFi automatically
    // commTask will see deviceConnected=false and restart advertising
  }
};




// === SOC lookup table ===
const uint16_t socToBms[101] = {
  0, 60,70,80,90,95,105,115,125,135,140,150,160,170,180,185,195,205,215,225,
  230,240,250,260,270,275,285,295,305,315,320,330,340,350,360,365,375,385,395,405,
  410,420,430,440,450,455,465,475,485,495,500,510,520,530,540,550,555,565,575,585,
  590,600,610,620,630,635,645,655,665,675,680,690,700,710,720,725,735,745,755,765,
  770,780,790,800,810,815,825,835,845,855,860,870,880,890,900,905,915,925,935,945,950
};

float getSoCFromLookup(uint16_t raw) {
  if (raw >= socToBms[100]) return 100.0f;
  if (raw <= socToBms[0]) return 0.0f;

  for (int i = 0; i < 100; i++) {
    if (raw >= socToBms[i] && raw <= socToBms[i + 1]) {
      float range = (float)(socToBms[i + 1] - socToBms[i]);
      float delta = (float)(raw - socToBms[i]);
      if (range == 0) return (float)i;
      return (float)i + (delta / range);
    }
  }
  return 0.0f;
}

// === Data state ===
// CRITICAL real-time data: use atomic (lock-free, no mutex needed)
std::atomic<int32_t> atomicAmpereRaw{0};    // Store as int (x10) for atomic
std::atomic<int32_t> atomicVoltsRaw{0};     // Store as int (x10) for atomic
std::atomic<int32_t> atomicPowerRaw{0};     // Store as int for atomic
std::atomic<int> atomicRPM{0};
std::atomic<int> atomicSpeed{0};
std::atomic<VehicleMode> atomicMode{MODE_PARK};  // Thread-safe mode
std::atomic<TransportMode> transportMode{TRANSPORT_BLE};  // Thread-safe transport mode

// Non-critical data: protected by mutex (no volatile needed - mutex provides memory barrier)
int valRPM = 0;
int valSpeed = 0;
float valVolts = 0.0f;
float valAmpere = 0.0f;
float valPower = 0.0f;

Preferences preferences;
int valSOC = 0;
std::atomic<bool> isInjectorEnabled{false}; // Atomic: accessed from BLE/WiFi (Core 1) + CAN task (Core 0)
bool currentTwaiModeNormal = false;          // TWAI mode: false=LISTEN_ONLY, true=NORMAL
unsigned long lastPeriodicSave = 0;  // Last periodic backup timestamp
bool shutdownSaved = false;          // Flag to prevent multiple shutdown saves
std::atomic<bool> canDataReady{false};  // Atomic: true after first valid SOC received from CAN

// NVS-safe write helpers (Fix #2: wrap all NVS writes in mutex)
static inline void nvsWriteInt(const char* key, int val) {
  if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    preferences.putInt(key, val);
    xSemaphoreGive(nvsMutex);
  }
}
static inline void nvsWriteBool(const char* key, bool val) {
  if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    preferences.putBool(key, val);
    xSemaphoreGive(nvsMutex);
  }
}
static inline void nvsWriteString(const char* key, const char* val) {
  if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    preferences.putString(key, val);
    xSemaphoreGive(nvsMutex);
  }
}

int valCtrlTemp = 0;
int valMotorTemp = 0;
int valBattTemp = 0;

// Handle Write Requests from Flutter
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      String value = pCharacteristic->getValue();
#else
      std::string rxValue = pCharacteristic->getValue();
      String value = String(rxValue.c_str());
#endif

      if (value.length() > 0) {
        // Simple command parser
        // Cmd: "INJECT:1" -> Enable
        // Cmd: "INJECT:0" -> Disable
        if (value.startsWith("INJECT:")) {
           char cmdVal = value.charAt(7);
           if (cmdVal == '1') {
             isInjectorEnabled.store(true, std::memory_order_release);
             nvsWriteBool("inj", true);
           } else if (cmdVal == '0') {
             isInjectorEnabled.store(false, std::memory_order_release);
             nvsWriteBool("inj", false);
           }
        }
        // Cmd: "WIFI:ON" -> Switch to WiFi mode
        else if (value.startsWith("WIFI:ON")) {
          Serial.println("[BLE] Switch to WiFi mode requested");
          transportMode.store(TRANSPORT_WIFI, std::memory_order_release);
          // Mode saved by loop() auto-save
        }
      }
    }
};

// strModeBuffer and isBraking replaced by atomicMode

// All below protected by dataMutex (no volatile needed)
uint16_t valCells[23] = {0};
uint8_t valCellTemps[5] = {0};

float valRemainingCapacity = 0.0f;
float valFullCapacity = 0.0f;

int valSOH = 0;
uint16_t valCycleCount = 0;

uint16_t valHighestCellVolt = 0;
uint8_t  valHighestCellNum  = 0;
uint16_t valLowestCellVolt  = 0;
uint8_t  valLowestCellNum   = 0;
uint16_t valAvgCellVolt     = 0;

uint8_t valMaxTemp = 0;
uint8_t valMaxTempCell = 0;
uint8_t valMinTemp = 0;
uint8_t valMinTempCell = 0;

uint8_t valBalanceMode = 0;
uint8_t valBalanceStatus = 0;
uint8_t valBalanceBits[4] = {0};

uint16_t rawCurrentHex = 0;
uint16_t rawVoltageHex = 0;
uint16_t rawSOCHex = 0;
char rawBalanceHexBuffer[24] = "00 00 00 00 00 00";

// Charger data (mutex-protected)
float valChargerVoltage = 0.0f;
float valChargerCurrent = 0.0f;
uint8_t valChargerStatus = 0;
bool chargerConnected = false;
unsigned long lastChargerMsg = 0;

// BMS Info (mutex-protected)
bool bmsChargingFlag = false;        // 0x0AB40D09 byte[0]=1 when charging
bool oriChargerDetected = false;     // 0x10261041 only from ORI charger
unsigned long lastOriChargerMsg = 0;
char bmsHwVersion[8] = "";           // 0x0A750D09 "H:v21"
char bmsFwVersion[8] = "";           // 0x0A760D09 "F:v23"

std::atomic<uint32_t> canMessagesPerSec{0};  // Atomic: written by CAN task (Core 0), read by Comm task (Core 1)
uint32_t canMsgCount = 0;       // Only used in CAN task (Core 0)
uint32_t lastSecond = 0;        // Only used in CAN task (Core 0)

unsigned long lastLEDBlink = 0;
bool ledState = false;

unsigned long heartbeatCounter = 0;

// === BLE TX state (BLE task only) ===
static char bleTxBuf[2200];        // Full JSON buffer
static uint16_t bleTxLen = 0;
static uint16_t bleTxOffset = 0;
static bool bleTxInProgress = false;
static uint32_t lastFastSend = 0;  // Last fast update time
static uint32_t lastSlowSend = 0;  // Last slow update time

// =============================================
// CAN PARSING (runs on Core 0)
// =============================================
void handleCANMessage(twai_message_t &msg) {
  uint32_t id = msg.identifier;

  // REMOVED: Aggressive prefix filter to prevent missing unknown IDs
  // uint8_t prefix = id >> 24;
  // if (prefix != 0x0A && prefix != 0x0E && prefix != 0x18 && prefix != 0x10) return;



  // Controller basic
  if (id == 0x0A010810 && msg.data_length_code >= 8) {
    // OPTIMIZATION: Update Atomic variables IMMEDIATELY (No Mutex needed)
    int rpm = msg.data[2] | (msg.data[3] << 8);
    int speed = (int)(rpm * 0.1033f);
    atomicRPM.store(rpm, std::memory_order_release);
    atomicSpeed.store(speed, std::memory_order_release);

    // Parse mode and update atomic (no mutex needed for atomic)
    uint8_t m = msg.data[1];
    VehicleMode newMode = MODE_PARK;  // Default for unknown modes

    if (m == 0x00) newMode = MODE_PARK;
    else if (m == 0x61) newMode = MODE_CHARGING;
    else if (m == 0x70) newMode = MODE_DRIVE;
    else if (m == 0x50 || m == 0xF0 || m == 0x30 || m == 0xF8) newMode = MODE_REVERSE;
    else if (m == 0x72 || m == 0xB2) newMode = MODE_BRAKE;
    else if (m == 0xB0) newMode = MODE_SPORT;
    else if (m == 0x78 || m == 0x08) newMode = MODE_STAND;
    // Unknown modes default to MODE_PARK (already set above)

    atomicMode.store(newMode, std::memory_order_release);

    // Update non-critical data with Mutex
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      valRPM = rpm;
      valSpeed = speed;
      valCtrlTemp = msg.data[4];
      valMotorTemp = msg.data[5];
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Battery temp sensors
  if (id == 0x0E6C0D09 && msg.data_length_code >= 5) {
     if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      int sum = 0;
      for (int i = 0; i < 5; i++) {
        valCellTemps[i] = msg.data[i];
        sum += (int)valCellTemps[i];
      }
      valBattTemp = sum / 5;
      xSemaphoreGive(dataMutex);
     }
    return;
  }

  // General info 1 (Voltage/Current/Capacity) - CRITICAL FOR REAL-TIME AMPERE
  if (id == 0x0A6D0D09 && msg.data_length_code >= 8) {
    // OPTIMIZATION: Decode and update Atomic variables IMMEDIATELY (No Mutex needed)
    uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    float volts = vRaw * 0.1f;
    atomicVoltsRaw.store((int32_t)(volts * 10.0f), std::memory_order_release);

    uint16_t iRawU = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    int16_t iRawS = (int16_t)iRawU;
    float ampere = iRawS * 0.1f;
    atomicAmpereRaw.store((int32_t)(ampere * 10.0f), std::memory_order_release);

    float power = volts * ampere;
    atomicPowerRaw.store((int32_t)power, std::memory_order_release);

    // Update non-critical shared data with Mutex
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      if (fabs(ampere) < 0.2f) ampere = 0.0f; // Zero clamping for display
      valVolts = volts;
      valAmpere = ampere;
      valPower = power;
      rawVoltageHex = vRaw;
      rawCurrentHex = iRawU;

      uint16_t remainCap = (uint16_t)((msg.data[4] << 8) | msg.data[5]);
      valRemainingCapacity = remainCap * 0.1f;

      uint16_t fullCap = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
      valFullCapacity = fullCap * 0.1f;
      
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Battery health
  if (id == 0x0A6E0D09 && msg.data_length_code >= 6) {
    // Mark data ready BEFORE mutex (atomic, lock-free)
    // This ensures flag is set even if mutex temporarily unavailable
    canDataReady.store(true, std::memory_order_release);
    
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      uint16_t socVal = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
      rawSOCHex = socVal;

      valSOC = (int)getSoCFromLookup(socVal);
      if (valSOC > 100) valSOC = 100;
      if (valSOC < 0) valSOC = 0;

      uint16_t sohVal = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
      valSOH = (int)(sohVal * 0.1f);
      if (valSOH > 100) valSOH = 100;

      valCycleCount = (uint16_t)((msg.data[4] << 8) | msg.data[5]);
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Cell voltage stats
  if (id == 0x0A6F0D09 && msg.data_length_code >= 8) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      valHighestCellVolt = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
      valHighestCellNum  = msg.data[2];
      valLowestCellVolt  = (uint16_t)((msg.data[3] << 8) | msg.data[4]);
      valLowestCellNum   = msg.data[5];
      valAvgCellVolt     = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Temp stats
  if (id == 0x0A700D09 && msg.data_length_code >= 6) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      valMaxTemp = msg.data[0];
      valMaxTempCell = msg.data[1];
      valMinTemp = msg.data[4];
      valMinTempCell = msg.data[5];
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Balance status
  if (id == 0x0A730D09 && msg.data_length_code >= 6) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      valBalanceMode = msg.data[0];
      valBalanceStatus = msg.data[1];
      valBalanceBits[0] = msg.data[2];
      valBalanceBits[1] = msg.data[3];
      valBalanceBits[2] = msg.data[4];
      valBalanceBits[3] = msg.data[5];

      snprintf(rawBalanceHexBuffer, sizeof(rawBalanceHexBuffer), "%02X %02X %02X %02X %02X %02X",
              msg.data[0], msg.data[1], msg.data[2], msg.data[3], msg.data[4], msg.data[5]);
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // Cell volt blocks 0x0E64..0x0E69
  if ((id & 0xFFF0FFFF) == 0x0E600D09) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      int baseIndex = -1;
      switch (id) {
        case 0x0E640D09: baseIndex = 0;  break;
        case 0x0E650D09: baseIndex = 4;  break;
        case 0x0E660D09: baseIndex = 8;  break;
        case 0x0E670D09: baseIndex = 12; break;
        case 0x0E680D09: baseIndex = 16; break;
        case 0x0E690D09: baseIndex = 20; break;
        default: break;
      }
      if (baseIndex >= 0) {
        for (int i = 0; i < 4 && (baseIndex + i) < 23; i++) {
          int off = i * 2;
          if (off + 1 < msg.data_length_code) {
            valCells[baseIndex + i] = (uint16_t)((msg.data[off] << 8) | msg.data[off + 1]);
          }
        }
      }
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // External Charger Messages (Protokol Charger)
  // Was labeled CCS/China Charger Standard, but could be generic.
  if ((id == 0x1810D0F3 || id == 0x1811D0F3) && msg.data_length_code >= 5) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
      valChargerVoltage = vRaw * 0.1f;

      uint16_t iRaw = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
      valChargerCurrent = iRaw * 0.1f;

      valChargerStatus = msg.data[4];
      chargerConnected = true;
      lastChargerMsg = millis();
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // BMS Charging Flag (0x0AB40D09) - byte[0]=1 when charging
  if (id == 0x0AB40D09 && msg.data_length_code >= 1) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      bmsChargingFlag = (msg.data[0] == 0x01);
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // BMS Hardware Version (0x0A750D09) - ASCII "H:v21"
  if (id == 0x0A750D09 && msg.data_length_code >= 6) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      size_t copyLen = min((size_t)msg.data_length_code, sizeof(bmsHwVersion) - 1);
      memcpy(bmsHwVersion, msg.data, copyLen);
      bmsHwVersion[copyLen] = '\0';
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // BMS Firmware Version (0x0A760D09) - ASCII "F:v23"
  if (id == 0x0A760D09 && msg.data_length_code >= 6) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      size_t copyLen = min((size_t)msg.data_length_code, sizeof(bmsFwVersion) - 1);
      memcpy(bmsFwVersion, msg.data, copyLen);
      bmsFwVersion[copyLen] = '\0';
      xSemaphoreGive(dataMutex);
    }
    return;
  }

  // ORI Charger Detection (0x10261041) - only present with original charger
  // CRITICAL FIX: In NORMAL mode, ESP32 receives its own transmitted messages!
  // If we're in NORMAL mode, WE are the sender → ignore to prevent self-detection
  // which would set oriChargerDetected=true → oriTimeout=false → kill our own injector.
  // Only detect ORI charger in LISTEN_ONLY mode (we can't TX, so it must be real ORI).
  if (id == 0x10261041) {
    if (!currentTwaiModeNormal) {
      // We're in LISTEN_ONLY → this IS from ORI charger
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        oriChargerDetected = true;
        lastOriChargerMsg = millis();
        xSemaphoreGive(dataMutex);
      }
    }
    // else: In NORMAL mode → this is our own echo, ignore
    return;
  }



}

// Check if TWAI bus is healthy before transmitting
bool isCanBusHealthy() {
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return false;
  if (status.state == TWAI_STATE_BUS_OFF) {
    Serial.println("[CAN] BUS-OFF detected! Recovering...");
    twai_initiate_recovery();
    return false;
  }
  if (status.state == TWAI_STATE_RECOVERING) return false;
  return (status.state == TWAI_STATE_RUNNING);
}

// =============================================
// DYNAMIC TWAI MODE SWITCHER
// =============================================
// currentTwaiModeNormal is declared in global variables section above

void switchTwaiMode(bool normalMode) {
  if (currentTwaiModeNormal == normalMode) return; // Already in target mode

  // Stop and uninstall the current driver
  twai_stop();
  twai_driver_uninstall();

  // Re-install with new mode
  twai_mode_t newMode = normalMode ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY;
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, newMode);
  g_config.rx_queue_len = 50; // Use larger queue to prevent data loss 
  
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    if (twai_start() == ESP_OK) {
      currentTwaiModeNormal = normalMode;
      // Serial.printf("[CAN] Switched mode to %s\n", normalMode ? "NORMAL" : "LISTEN_ONLY");
    } else {
      Serial.println("[CAN] ERROR: Failed to start after mode switch!");
    }
  } else {
    Serial.println("[CAN] ERROR: Failed to install after mode switch!");
  }
}

// =============================================
// CAN INJECTOR (Simulate Original Charger)
// =============================================
// Caller must ensure TWAI is in NORMAL mode before calling.
// Mode management is handled by the CAN task loop.
void injectChargerMessage() {
  if (!currentTwaiModeNormal) return;
  
  twai_message_t tx_msg;
  memset(&tx_msg, 0, sizeof(tx_msg));
  tx_msg.identifier = 0x10261041;
  tx_msg.extd = 1;
  tx_msg.data_length_code = 8;
  tx_msg.data[0] = 0x05;
  tx_msg.data[1] = 0x0A;
  tx_msg.data[2] = 0x1C;

  esp_err_t result = twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
  if (result != ESP_OK) {
    Serial.printf("[CAN-INJ] TX FAIL: 0x%x\n", result);
  }
}



// =============================================
// CAN TASK (Core 0 - High Priority)
// =============================================
void canTask(void *pvParameters) {
  
  twai_message_t message;
  bool gotMessage = false;
  static unsigned long lastInjectTime = 0;
  
  while (true) {
    gotMessage = false;
    
    // Drain CAN RX queue (Rate Limited) - Process as many as possible to avoid queue overflow
    while (twai_receive(&message, 0) == ESP_OK) {
      handleCANMessage(message);
      canMsgCount++;
      gotMessage = true;
    }

    // LED feedback
    if (gotMessage) {
      digitalWrite(LED_PIN, HIGH);
      lastLEDBlink = millis();
      ledState = true;
    } else if (ledState && (millis() - lastLEDBlink > 50)) {
      digitalWrite(LED_PIN, LOW);
      ledState = false;
    }

    // CAN rate per second
    uint32_t nowSec = millis() / 1000;
    if (nowSec != lastSecond) {
      canMessagesPerSec.store(canMsgCount, std::memory_order_release);
      canMsgCount = 0;
      lastSecond = nowSec;
    }

    // === HYBRID SOC SAVE STRATEGY ===
    // Reduces NVS writes from 17,280/day to ~3/day
    // Strategy 1: Periodic backup (every 1 hour when motor running)
    // Strategy 2: Shutdown save (CAN timeout = motor off)
    // Strategy 3: Shutdown handler save (esp_restart / clean shutdown)
    
    static unsigned long lastCanMessage = millis();  // Track last CAN activity
    
    // Update lastCanMessage if we got messages this loop
    if (gotMessage) {
      lastCanMessage = millis();
      shutdownSaved = false;  // Reset shutdown flag when motor active
    }
    
    uint32_t canAge = millis() - lastCanMessage;
    bool motorRunning = (canAge < 5000);  // Motor running if CAN active within 5s
    
    // SAVE STRATEGY #1: Shutdown Detection (CAN timeout)
    // Save once when motor turns off (30s no CAN = shutdown)
    if (canAge > CAN_TIMEOUT_MS && !shutdownSaved) {
      bool dataReady = canDataReady.load(std::memory_order_acquire);
      int currentSoc = 0;
      
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        currentSoc = valSOC;
        xSemaphoreGive(dataMutex);
      }
      
      if (dataReady && currentSoc >= 0 && currentSoc <= 100) {
        nvsWriteInt("soc", currentSoc);
        Serial.printf("[SOC-SAVE] Shutdown detected - SOC saved: %d%% (CAN inactive)\n", currentSoc);
        shutdownSaved = true;  // Prevent multiple saves
      }
    }
    
    // SAVE STRATEGY #2: Periodic Backup (every 1 hour when motor running)
    // Safety net in case of crash/unexpected shutdown
    if (motorRunning && millis() - lastPeriodicSave > SOC_PERIODIC_SAVE_MS) {
      bool dataReady = canDataReady.load(std::memory_order_acquire);
      int currentSoc = 0;
      
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        currentSoc = valSOC;
        xSemaphoreGive(dataMutex);
      }
      
      if (dataReady && currentSoc >= 0 && currentSoc <= 100) {
        nvsWriteInt("soc", currentSoc);
        Serial.printf("[SOC-SAVE] Periodic backup - SOC saved: %d%%\n", currentSoc);
        lastPeriodicSave = millis();
      }
    }


    // Snapshot shared variables ONCE under mutex for this iteration
    bool localChargerConnected = false;
    bool localOriChargerDetected = false;
    unsigned long localLastChargerMsg = 0;
    unsigned long localLastOriChargerMsg = 0;
    bool localBmsCharging = false;
    
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      localChargerConnected = chargerConnected;
      localOriChargerDetected = oriChargerDetected;
      localLastChargerMsg = lastChargerMsg;
      localLastOriChargerMsg = lastOriChargerMsg;
      localBmsCharging = bmsChargingFlag;
      xSemaphoreGive(dataMutex);
    }

    // Charger timeout - reset chargerConnected if no CAN message
    if (localChargerConnected && (millis() - localLastChargerMsg > CHARGER_TIMEOUT_MS)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        chargerConnected = false;
        valChargerVoltage = 0.0f;
        valChargerCurrent = 0.0f;
        valChargerStatus = 0;
        xSemaphoreGive(dataMutex);
      }
      localChargerConnected = false; // Update local snapshot
    }

    // ORI Charger timeout - reset if no CAN message
    if (localOriChargerDetected && (millis() - localLastOriChargerMsg > ORI_CHARGER_TIMEOUT_MS)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        oriChargerDetected = false;
        xSemaphoreGive(dataMutex);
      }
    }

    // Injector Logic for Third-Party Charger
    // FIX v2: Third-party chargers do NOT send CAN messages, so we can't rely on
    // chargerConnected flag. Instead detect charging via BMS charging flag + positive current.
    // 
    // From CAN sniff data:
    //   - Third-party charger: 0x0AB40D09 byte[0]=0x01 (BMS sees charging),
    //     current=4.2-4.3A, but mode stays PARK because no 0x10261041 injected.
    //   - ORI charger sends 0x10261041 itself, so injector must NOT interfere.
    //
    // Only inject if:
    // 1. Injector enabled by user
    // 2. BMS reports charging state (0x0AB40D09 byte[0]=1)
    // 3. ORI charger not detected (no 0x10261041 for >2s)
    // 4. Charging current is positive (>1A to filter noise/REGEN)
    //    OR external CAN charger is connected (chargerConnected flag)
    
    // Snapshot current ampere value under mutex
    float currentAmpere = 0.0f;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      currentAmpere = valAmpere;
      xSemaphoreGive(dataMutex);
    }
    
    // Detect real charging: either CAN-connected charger OR BMS+current indicate charging
    bool canChargerActive = localChargerConnected && 
                            (millis() - localLastChargerMsg < CHARGER_TIMEOUT_MS);
    bool thirdPartyCharging = localBmsCharging && (currentAmpere > 1.0f);
    bool chargerActuallyActive = canChargerActive || thirdPartyCharging;
    
    // SAFETY: Only inject when vehicle is stationary.
    // This prevents false injection during REGEN braking where current is also positive.
    // Valid stationary modes: PARK(0x00), CHARGING(0x61), STAND(0x78/0x08)
    // Blocked modes: DRIVE, SPORT, REVERSE, BRAKE (vehicle is moving/decelerating)
    VehicleMode currentMode = atomicMode.load(std::memory_order_acquire);
    bool modeAllowsInjection = (currentMode == MODE_PARK || 
                                currentMode == MODE_CHARGING || 
                                currentMode == MODE_STAND);
    
    bool injEnabled = isInjectorEnabled.load(std::memory_order_acquire);
    bool oriTimeout = (millis() - localLastOriChargerMsg > 2000);
    
    // Determine if injection should be active
    // All 5 conditions must be true:
    // 1. User enabled injector (via WebSocket/BLE/HTTP)
    // 2. BMS reports charging state
    // 3. Vehicle in PARK or CHARGING (prevents false inject during REGEN)
    // 4. No ORI charger present (prevents interference)
    // 5. Actual charging detected (current > 1A or CAN charger connected)
    bool shouldInject = injEnabled && 
                        localBmsCharging && 
                        modeAllowsInjection &&
                        oriTimeout &&
                        chargerActuallyActive;
    
    // === TWAI Mode Management ===
    // Switch to NORMAL mode when inject is needed (with bus sync delay)
    if (shouldInject && !currentTwaiModeNormal) {
      switchTwaiMode(true);
      vTaskDelay(pdMS_TO_TICKS(50));  // Wait for bus synchronization
    }
    
    // Auto-inject every 500ms when all conditions met
    if (shouldInject && currentTwaiModeNormal) {
       if (millis() - lastInjectTime > INJECTOR_INTERVAL_MS) {
         injectChargerMessage();
         lastInjectTime = millis();
       }
    }

    // Switch back to LISTEN_ONLY when injection no longer needed (2s grace period)
    if (currentTwaiModeNormal && !shouldInject && (millis() - lastInjectTime > 2000)) {
      switchTwaiMode(false);
    }

    // Adaptive rate limiting: faster when driving, slower when charging
    // Driving: 50Hz (20ms) for responsive real-time data
    // Charging: 20Hz (50ms) to reduce CPU load during charger data flood
    // OTA Active: 5Hz (200ms) to prioritize flash writing
    if (isOtaInProgress()) {
      vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      vTaskDelay(pdMS_TO_TICKS(localChargerConnected ? 50 : 20));
    }
  }
}

// Build FAST JSON - Critical real-time data only (~200-300 bytes)
static bool buildFastJson() {
  // Read atomic values (no mutex needed)
  int localRPM = atomicRPM.load(std::memory_order_acquire);
  int localSpeed = atomicSpeed.load(std::memory_order_acquire);
  float localAmpere = atomicAmpereRaw.load(std::memory_order_acquire) / 10.0f;
  float localVolts = atomicVoltsRaw.load(std::memory_order_acquire) / 10.0f;
  float localPower = (float)atomicPowerRaw.load(std::memory_order_acquire);
  VehicleMode localMode = atomicMode.load(std::memory_order_acquire);

  // Read other critical data under mutex
  int localSOC, localCtrlTemp, localMotorTemp, localBattTemp;

  uint32_t localCanRate;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    localSOC = valSOC;
    localCtrlTemp = valCtrlTemp;
    localMotorTemp = valMotorTemp;
    localBattTemp = valBattTemp;
    localCanRate = canMessagesPerSec.load(std::memory_order_acquire);

    xSemaphoreGive(dataMutex);
  } else {
    return false; // Skip if can't get mutex
  }

  // Build compact JSON with abbreviated field names
  int fastLen = snprintf(bleTxBuf, sizeof(bleTxBuf),
    "{\"r\":%d,"           // rpm
    "\"s\":%d,"           // speed
    "\"m\":\"%s\","       // mode
    "\"v\":%.1f,"         // volts
    "\"a\":%.1f,"         // amps
    "\"p\":%.0f,"         // power
    "\"sc\":%d,"          // soc
    "\"t\":{\"c\":%d,\"m\":%d,\"b\":%d},"  // temps: ctrl, motor, batt
    "\"cr\":%lu,"         // canRate
    "\"hb\":%lu,"         // heartbeat
    "\"inj\":%d,"         // injector enabled
    "\"type\":\"fast\""   // indicator for fast update
    "}\n",
    localRPM, localSpeed, getModeString(localMode),
    localVolts, localAmpere, localPower, localSOC,
    localCtrlTemp, localMotorTemp, localBattTemp,
    (unsigned long)localCanRate,
    (unsigned long)heartbeatCounter++,
    isInjectorEnabled.load(std::memory_order_acquire) ? 1 : 0
  );
  if (fastLen < 0) fastLen = 0;
  if ((size_t)fastLen >= sizeof(bleTxBuf)) {
    bleTxLen = sizeof(bleTxBuf) - 1;
  } else {
    bleTxLen = (uint16_t)fastLen;
  }
  return true;
}

// Build FULL JSON - Complete data including cells, balance, health (~1.5-2KB)
static bool buildFullJson() {
  // Read atomic values FIRST (no mutex needed, always fresh)
  int localRPM = atomicRPM.load(std::memory_order_acquire);
  int localSpeed = atomicSpeed.load(std::memory_order_acquire);
  float localAmpere = atomicAmpereRaw.load(std::memory_order_acquire) / 10.0f;
  float localVolts = atomicVoltsRaw.load(std::memory_order_acquire) / 10.0f;
  float localPower = (float)atomicPowerRaw.load(std::memory_order_acquire);
  VehicleMode localMode = atomicMode.load(std::memory_order_acquire);

  // Local copies for thread safety (non-critical data)
  int localSOC, localCtrlTemp, localMotorTemp, localBattTemp;
  int localSOH, localCycleCount;
  float localRemainingCap, localFullCap;
  uint16_t localCells[23];
  uint16_t localHighestVolt, localLowestVolt, localAvgVolt;
  uint8_t localHighestNum, localLowestNum;
  uint8_t localMaxTemp, localMaxTempCell, localMinTemp, localMinTempCell;
  uint8_t localBalanceMode, localBalanceStatus, localBalanceBits[4];
  float localChargerVolt, localChargerCurrent;
  // localChargerStatus removed - unused
  bool localChargerConnected;
  unsigned long localLastChargerMsg;
  uint32_t localCanRate;
  // localRawCurrent, localRawVoltage, localRawSOC removed - unused in output
  // New BMS info
  bool localBmsChargingFlag;
  bool localOriCharger;
  unsigned long localLastOriChargerMsg;
  char localHwVersion[8];
  char localFwVersion[8];


  // Read non-critical data under mutex (short hold time)
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    localSOC = valSOC;
    localCtrlTemp = valCtrlTemp;
    localMotorTemp = valMotorTemp;
    localBattTemp = valBattTemp;
    localSOH = valSOH;
    localCycleCount = valCycleCount;
    localRemainingCap = valRemainingCapacity;
    localFullCap = valFullCapacity;
    localHighestVolt = valHighestCellVolt;
    localHighestNum = valHighestCellNum;
    localLowestVolt = valLowestCellVolt;
    localLowestNum = valLowestCellNum;
    localAvgVolt = valAvgCellVolt;
    localMaxTemp = valMaxTemp;
    localMaxTempCell = valMaxTempCell;
    localMinTemp = valMinTemp;
    localMinTempCell = valMinTempCell;
    localBalanceMode = valBalanceMode;
    localBalanceStatus = valBalanceStatus;
    memcpy(localBalanceBits, valBalanceBits, 4);
    localChargerVolt = valChargerVoltage;
    localChargerCurrent = valChargerCurrent;
    // localChargerStatus not used in JSON output
    localChargerConnected = chargerConnected;
    localLastChargerMsg = lastChargerMsg;
    localCanRate = canMessagesPerSec.load(std::memory_order_acquire);
    // rawCurrentHex, rawVoltageHex, rawSOCHex not used in JSON output
    memcpy(localCells, valCells, sizeof(localCells));
    // New BMS info
    localBmsChargingFlag = bmsChargingFlag;
    localOriCharger = oriChargerDetected;
    localLastOriChargerMsg = lastOriChargerMsg;
    strcpy(localHwVersion, bmsHwVersion);
    strcpy(localFwVersion, bmsFwVersion);

    xSemaphoreGive(dataMutex);
  } else {
    // Fallback - use last known values
    return false;
  }

  // Note: Atomic values (RPM, speed, ampere, volts, power, mode) already read above

  // Calculate cell delta
  uint16_t minCell = 9999, maxCell = 0;
  for (int i = 0; i < 23; i++) {
    if (localCells[i] > 0 && localCells[i] < minCell) minCell = localCells[i];
    if (localCells[i] > maxCell) maxCell = localCells[i];
  }
  int cellDelta = (minCell > maxCell) ? 0 : ((int)maxCell - (int)minCell);

  // Build balance cells array string (compact: 0/1 instead of true/false)
  // 23 cells * 1 digit + 22 commas + null = 46 bytes, use 64 for safety
  char balanceCells[64];
  int bpos = 0;
  for (int i = 0; i < 23; i++) {
    int byteIndex = i / 8;
    int bitIndex = i % 8;
    bool isBalancing = (localBalanceBits[byteIndex] & (1 << bitIndex)) != 0;
    int written = snprintf(balanceCells + bpos, sizeof(balanceCells) - bpos, 
                     "%d%s", isBalancing ? 1 : 0, (i < 22) ? "," : "");
    if (written > 0) bpos += written;
  }

  // Build cells array string
  // 23 cells * max 5 digits (65535) + 22 commas + null = 138 bytes, use 160 for safety
  char cellsStr[160];
  int cpos = 0;
  for (int i = 0; i < 23; i++) {
    int written = snprintf(cellsStr + cpos, sizeof(cellsStr) - cpos, 
                     "%u%s", localCells[i], (i < 22) ? "," : "");
    if (written > 0) cpos += written;
  }

  // Build complete JSON with abbreviated field names
  int fullLen = snprintf(bleTxBuf, sizeof(bleTxBuf),
    "{\"r\":%d,"
    "\"s\":%d,"
    "\"m\":\"%s\","
    "\"v\":%.1f,"
    "\"a\":%.1f,"
    "\"p\":%.0f,"
    "\"sc\":%d,"
    "\"t\":{\"c\":%d,\"m\":%d,\"b\":%d},"
    "\"cells\":[%s],"
    "\"cd\":%d,"
    "\"cr\":%lu,"
    "\"h\":{\"soh\":%d,\"cyc\":%u,\"rc\":%.1f,\"fc\":%.1f},"
    "\"cvs\":{\"hi\":%u,\"hiC\":%u,\"lo\":%u,\"loC\":%u,\"av\":%u},"
    "\"ts\":{\"max\":%u,\"maxC\":%u,\"min\":%u,\"minC\":%u},"
    "\"b\":{\"md\":%u,\"st\":%u,\"cells\":[%s]},"
    "\"chr\":{\"on\":%d,\"v\":%.1f,\"a\":%.1f,\"ori\":%d},"
    "\"bms\":{\"hw\":\"%s\",\"fw\":\"%s\"},"
    "\"hb\":%lu,"
    "\"inj\":%d,"
    "\"type\":\"full\""
    "}\n",
    localRPM, localSpeed, getModeString(localMode),
    localVolts, localAmpere, localPower, localSOC,
    localCtrlTemp, localMotorTemp, localBattTemp,
    cellsStr, cellDelta,
    (unsigned long)localCanRate,
    localSOH, localCycleCount, localRemainingCap, localFullCap,
    localHighestVolt, localHighestNum, localLowestVolt, localLowestNum, localAvgVolt,
    localMaxTemp, localMaxTempCell, localMinTemp, localMinTempCell,
    localBalanceMode, localBalanceStatus, balanceCells,
    localBmsChargingFlag ? 1 : 0,  // charger.on = 0/1
    (localChargerVolt > 0.1f) ? localChargerVolt : localVolts,      // Fallback to system volts
    (localChargerCurrent > 0.1f) ? localChargerCurrent : fabs(localAmpere), // Fallback to system amps
    (millis() - localLastOriChargerMsg < ORI_CHARGER_TIMEOUT_MS && localOriCharger) ? 1 : 0,  // charger.ori = 0/1
    localHwVersion, localFwVersion,
    (unsigned long)heartbeatCounter++,
    isInjectorEnabled.load(std::memory_order_acquire) ? 1 : 0
  );
  if (fullLen < 0) fullLen = 0;
  if ((size_t)fullLen >= sizeof(bleTxBuf)) {
    bleTxLen = sizeof(bleTxBuf) - 1;
  } else {
    bleTxLen = (uint16_t)fullLen;
  }
  return true;
}

static void startBleTxIfIdle(bool useFast) {
  if (!deviceConnected.load(std::memory_order_acquire)) return;
  if (bleTxInProgress) return;

  bool ok;
  if (useFast) {
    ok = buildFastJson();
  } else {
    ok = buildFullJson();
  }
  
  if (!ok) return; // SKIP jika build gagal, jangan kirim data stale
  
  bleTxOffset = 0;
  bleTxInProgress = true;
}

static void pumpBleTx() {
  if (!deviceConnected.load(std::memory_order_acquire)) {
    bleTxInProgress = false;
    bleTxOffset = 0;
    return;
  }
  if (!bleTxInProgress) return;

  if (bleTxOffset >= bleTxLen) {
    bleTxInProgress = false;
    return;
  }

  const uint32_t startUs = micros();
  int sent = 0;

  while (bleTxInProgress &&
         sent < BLE_PUMP_MAX_CHUNKS &&
         (micros() - startUs) < BLE_PUMP_BUDGET_US) {

    int remain = bleTxLen - (int)bleTxOffset;
    if (remain <= 0) {
      bleTxInProgress = false;
      break;
    }

    int chunkLen = min((int)BLE_SAFE_CHUNK, remain);
    const char *p = bleTxBuf + bleTxOffset;

    pCharacteristic->setValue((uint8_t*)p, chunkLen);
    pCharacteristic->notify();

    bleTxOffset += chunkLen;
    sent++;
  }

  if (bleTxOffset >= bleTxLen) bleTxInProgress = false;
}

// =============================================
// ADAPTIVE TIMING HELPERS
// =============================================
static uint32_t getFastUpdateInterval() {
  // Read current mode from atomic (no mutex needed)
  VehicleMode mode = atomicMode.load(std::memory_order_acquire);

  // Return interval based on mode
  switch (mode) {
    case MODE_PARK:
    case MODE_STAND:
      return PARK_FAST_MS;
    case MODE_CHARGING:
      return CHARGING_FAST_MS;
    case MODE_BRAKE:
      return BRAKE_FAST_MS;
    default:
      // DRIVE, SPORT, REVERSE
      return DRIVE_FAST_MS;
  }
}

static uint32_t getSlowUpdateInterval() {
  // Read current mode from atomic (no mutex needed)
  VehicleMode mode = atomicMode.load(std::memory_order_acquire);

  switch (mode) {
    case MODE_PARK:
    case MODE_STAND:
      return PARK_SLOW_MS;
    case MODE_CHARGING:
      return CHARGING_SLOW_MS;
    default:
      // DRIVE, SPORT, REVERSE, BRAKE
      return DRIVE_SLOW_MS;
  }
}



// =============================================
// BLE LIFECYCLE MANAGEMENT
// =============================================

// Shared BLE server initialization (eliminates duplication)
static void initBLEServer() {
  // Always reinitialize after deinit
  Serial.println("[BLE] Initializing BLE server...");
  
  // Ensure BT controller is started
  btStart();
  delay(100);
  
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(512);
  
  pServer = BLEDevice::createServer();
  static MyServerCallbacks serverCallbacks;
  pServer->setCallbacks(&serverCallbacks);
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | 
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE
  );
  static BLE2902 ble2902Descriptor;
  pCharacteristic->addDescriptor(&ble2902Descriptor);
  static MyCallbacks charCallbacks;
  pCharacteristic->setCallbacks(&charCallbacks);
  pService->start();
  
  setupOtaService(pServer);
  
  Serial.println("[BLE] BLE server initialized successfully");
}

static void stopBLE() {
  Serial.println("[Comm] Stopping BLE...");
  
  if (pServer != nullptr) {
    // Disconnect any active clients gracefully
    if (deviceConnected.load(std::memory_order_acquire)) {
      Serial.println("[BLE] Disconnecting client...");
      delay(100);
    }
    
    // Stop advertising
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    if (pAdvertising != nullptr) {
      pAdvertising->stop();
    }
  }
  
  deviceConnected.store(false, std::memory_order_release);
  oldDeviceConnected = false;
  
  // Fully deinit BLE to release Bluetooth controller
  // This is CRITICAL: ESP32 shares one radio between BLE and WiFi.
  // Without deinit, BT controller keeps the radio and WiFi AP fails to start.
  BLEDevice::deinit(false);  // false = don't release memory (we'll reinit later)
  pServer = nullptr;
  pCharacteristic = nullptr;
  delay(200);  // Let BT controller fully release
  
  // Stop Bluetooth controller at hardware level
  btStop();
  delay(100);
  
  Serial.println("[BLE] BLE fully stopped (controller released)");
}

static void startBLE() {
  Serial.println("[Comm] Starting BLE...");
  
  // Full reinitialize (server was deinit'd during stop)
  initBLEServer();
  
  // Configure and start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("[Comm] BLE active and advertising");
}


// WebSocket transmission state
static uint32_t lastWsFastSend = 0;
static uint32_t lastWsSlowSend = 0;
static char wsTxBuf[2200];

static void sendWsFastUpdate() {
  // Read atomic values (no mutex needed)
  int localRPM = atomicRPM.load(std::memory_order_acquire);
  int localSpeed = atomicSpeed.load(std::memory_order_acquire);
  float localAmpere = atomicAmpereRaw.load(std::memory_order_acquire) / 10.0f;
  float localVolts = atomicVoltsRaw.load(std::memory_order_acquire) / 10.0f;
  float localPower = (float)atomicPowerRaw.load(std::memory_order_acquire);
  VehicleMode localMode = atomicMode.load(std::memory_order_acquire);

  int localSOC = 0, localCtrlTemp = 0, localMotorTemp = 0, localBattTemp = 0;
  uint32_t localCanRate = 0;


  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    localSOC = valSOC;
    localCtrlTemp = valCtrlTemp;
    localMotorTemp = valMotorTemp;
    localBattTemp = valBattTemp;
    localCanRate = canMessagesPerSec.load(std::memory_order_acquire);

    xSemaphoreGive(dataMutex);
  } else {
    return; // Skip broadcast, jangan kirim data default 0
  }

  int len = snprintf(wsTxBuf, sizeof(wsTxBuf),
    "{\"r\":%d,\"s\":%d,\"m\":\"%s\",\"v\":%.1f,\"a\":%.1f,\"p\":%.0f,\"sc\":%d,\"t\":{\"c\":%d,\"m\":%d,\"b\":%d},\"cr\":%lu,\"hb\":%lu,\"type\":\"fast\"}\n",
    localRPM, localSpeed, getModeString(localMode),
    localVolts, localAmpere, localPower, localSOC,
    localCtrlTemp, localMotorTemp, localBattTemp,
    (unsigned long)localCanRate,
    (unsigned long)heartbeatCounter++
  );
  
  if (len > 0 && len < (int)sizeof(wsTxBuf)) {
    wsBroadcast(wsTxBuf);
  }
}

static void sendWsSlowUpdate() {
  // Build full JSON (same logic as BLE)
  if (buildFullJson()) {  // Hanya broadcast kalau berhasil
    wsBroadcast(bleTxBuf);
  }
}

void commTask(void *pvParameters) {
  // Read initial mode from NVS (set in setup())
  TransportMode currentMode = transportMode.load(std::memory_order_acquire);
  
  if (currentMode == TRANSPORT_WIFI) {
    Serial.println("[Comm] Starting in WiFi mode (from saved preference)");
    // Start WiFi immediately
    initWiFiMode();
  } else {
    Serial.println("[Comm] Starting in BLE mode (Manual Switch Only)");
    // BLE already initialized in setup()
  }
  
  while (true) {
    TransportMode requestedMode = transportMode.load(std::memory_order_acquire);
    
    // No auto-switch logic anymore
    // We just listen to requestedMode changes from callbacks
    
    // === Mode Switch Logic ===
    if (requestedMode != currentMode) {
      Serial.println("========================================");
      Serial.printf("[SWITCH] %s → %s\n", 
                    currentMode == TRANSPORT_BLE ? "BLE" : "WiFi",
                    requestedMode == TRANSPORT_BLE ? "BLE" : "WiFi");
      Serial.println("========================================");
      
      unsigned long switchStart = millis();
      
      if (requestedMode == TRANSPORT_WIFI && currentMode == TRANSPORT_BLE) {
        // Switch from BLE to WiFi
        Serial.println("[SWITCH] Step 1/3: Stopping BLE completely...");
        stopBLE();  // Full deinit + btStop()
        delay(500); // Let radio fully release before WiFi takes over
        
        Serial.println("[SWITCH] Step 2/3: Starting WiFi AP...");
        initWiFiMode();  // This handles WiFi init
        
        Serial.println("[SWITCH] Step 3/3: WiFi ready!");
        currentMode = TRANSPORT_WIFI;
        
      } else if (requestedMode == TRANSPORT_BLE && currentMode == TRANSPORT_WIFI) {
        // Switch from WiFi to BLE
        Serial.println("[SWITCH] Step 1/3: Stopping WiFi...");
        stopWiFiMode();
        delay(500); // Let WiFi radio fully release
        
        Serial.println("[SWITCH] Step 2/3: Starting BLE...");
        startBLE();  // Full reinit (btStart + BLEDevice::init)
        
        Serial.println("[SWITCH] Step 3/3: BLE ready!");
        currentMode = TRANSPORT_BLE;
      }
      
      unsigned long switchTime = millis() - switchStart;
      Serial.printf("[SWITCH] Complete! Time: %lums\n", switchTime);
      Serial.println("========================================\n");
    }
    
    // === Handle current mode ===
    if (currentMode == TRANSPORT_WIFI) {
      // === WIFI MODE ===
      handleWiFiLoop();
      
      // WebSocket transmission
      if (isWsClientConnected()) {
        uint32_t now = millis();
        uint32_t fastInterval = getFastUpdateInterval();
        uint32_t slowInterval = getSlowUpdateInterval();
        
        if (now - lastWsSlowSend >= slowInterval) {
          lastWsSlowSend = now;
          lastWsFastSend = now;
          sendWsSlowUpdate();
        } else if (now - lastWsFastSend >= fastInterval) {
          lastWsFastSend = now;
          sendWsFastUpdate();
        }
      }
      
      vTaskDelay(pdMS_TO_TICKS(5));
      
    } else if (currentMode == TRANSPORT_BLE) {
      // === BLE MODE ===
      
      // Skip BLE operations if not initialized (e.g., started in WiFi mode)
      if (pServer == nullptr || pCharacteristic == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      
      // BLE reconnect advertising
      bool connected = deviceConnected.load(std::memory_order_acquire);
      if (!connected && oldDeviceConnected) {
        delay(200);
        pServer->startAdvertising();
        oldDeviceConnected = connected;
      }
      if (connected && !oldDeviceConnected) {
        oldDeviceConnected = connected;
      }

      // Adaptive dual-rate transmission logic
      uint32_t now = millis();
      
      if (!bleTxInProgress) {
        uint32_t fastInterval = getFastUpdateInterval();
        uint32_t slowInterval = getSlowUpdateInterval();
        
        if (now - lastSlowSend >= slowInterval) {
          lastSlowSend = now;
          lastFastSend = now;
          startBleTxIfIdle(false);
        } else if (now - lastFastSend >= fastInterval) {
          lastFastSend = now;
          startBleTxIfIdle(true);
        }
      }

      // Handle OTA reboot (safely outside BLE callback context)
      if (isOtaRebootPending()) {
        Serial.println("[OTA] Rebooting now...");
        delay(2000);  // Give BLE stack time to send final status
        ESP.restart();
      }

      // Pump BLE chunks
      if (!isOtaInProgress()) {
        pumpBleTx();
        checkOtaTimeout();
        vTaskDelay(pdMS_TO_TICKS(2));
      } else {
        checkOtaTimeout();
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }
  }
}

// =============================================
// SHUTDOWN HANDLER - Save SOC on Clean Restart
// =============================================
// SAVE STRATEGY #3: Save SOC during esp_restart() / clean shutdown.
// NOTE: This is NOT a brownout handler. For true power-cut protection,
// SOC is periodically saved to NVS (Strategy #1/#2) and could be
// backed up to RTC memory if needed.
void shutdownHandler() {
  // Direct save to NVS (bypass mutex - we're shutting down, single-threaded)
  preferences.putInt("soc", valSOC);
  preferences.end();  // Flush to ensure write completes
}

// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[Init] VOTOL BLE+WiFi Dashboard Starting...");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Create mutex
  dataMutex = xSemaphoreCreateMutex();
  nvsMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL || nvsMutex == NULL) {
    while(1); // Critical error - halt
  }

  preferences.begin("votol", false);
  valSOC = preferences.getInt("soc", 0);
  
  // Initialize SOC save state
  lastPeriodicSave = millis();  // Start periodic timer
  shutdownSaved = false;        // Reset shutdown flag
  
  isInjectorEnabled.store(preferences.getBool("inj", false), std::memory_order_relaxed);
  
  // Register shutdown handler for clean restart SOC saves
  esp_register_shutdown_handler(shutdownHandler);
  Serial.println("[Init] Shutdown handler registered for SOC saves");
  
  // Load saved transport mode (BLE or WiFi)
  String savedMode = preferences.getString("mode", "BLE");
  TransportMode initialMode = (savedMode == "WIFI") ? TRANSPORT_WIFI : TRANSPORT_BLE;
  transportMode.store(initialMode, std::memory_order_release);
  Serial.printf("[Init] Saved mode: %s\n", savedMode.c_str());

  // BLE Setup - Only if starting in BLE mode
  if (initialMode == TRANSPORT_BLE) {
    Serial.println("[Init] Initializing BLE mode...");
    startBLE();  // Use shared function (configures advertising UUIDs + starts)
    Serial.println("[Init] BLE started and advertising");
    Serial.println("[Init] Send 'WIFI:ON' via Serial to switch to WiFi mode");
  } else {
    Serial.println("[Init] WiFi mode selected - BLE not started");
    Serial.println("[Init] WiFi AP will start in commTask...");
    pServer = nullptr;
    pCharacteristic = nullptr;
  }

  // CAN (TWAI) Setup - Use LISTEN_ONLY as default to prevent ACK clashes with speedometer
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
  g_config.rx_queue_len = 50; // Increase queue to prevent stale data buildup & drops

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    twai_start();
  }

  // Create tasks on specific cores
  xTaskCreatePinnedToCore(
    canTask,              // Task function
    "CAN_Task",           // Name
    CAN_TASK_STACK_SIZE,  // Stack size
    NULL,                 // Parameters
    CAN_TASK_PRIORITY,    // Priority (higher = more important)
    &canTaskHandle,       // Task handle
    0                     // Core 0
  );

  xTaskCreatePinnedToCore(
    commTask,
    "Comm_Task",
    BLE_TASK_STACK_SIZE,
    NULL,
    BLE_TASK_PRIORITY,
    &bleTaskHandle,
    1                     // Core 1
  );


}

// =============================================
// LOOP (empty - tasks handle everything)
// =============================================
void loop() {
  // Serial Command Handler for Manual Switching
  static TransportMode lastSavedMode = transportMode.load(std::memory_order_acquire);
  
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    if (input.equalsIgnoreCase("WIFI:ON")) {
      Serial.println("[Serial] Command received: Switch to WiFi");
      transportMode.store(TRANSPORT_WIFI, std::memory_order_release);
      // Mode saved by auto-save below
    } 
    else if (input.equalsIgnoreCase("BLE:ON")) {
      Serial.println("[Serial] Command received: Switch to BLE");
      transportMode.store(TRANSPORT_BLE, std::memory_order_release);
      // Mode saved by auto-save below
    }
    else if (input.length() > 0) {
      Serial.println("[Serial] Unknown command: " + input);
    }
  }
  
  // Auto-save mode changes
  TransportMode current = transportMode.load(std::memory_order_acquire);
  if (current != lastSavedMode) {
    nvsWriteString("mode", (current == TRANSPORT_WIFI) ? "WIFI" : "BLE");
    lastSavedMode = current;
    Serial.printf("[Main] Mode saved: %s\n", (current == TRANSPORT_WIFI) ? "WIFI" : "BLE");
  }
  
  vTaskDelay(pdMS_TO_TICKS(100));
}
