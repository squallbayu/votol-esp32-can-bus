/*
 * =============================================
 * VOTOL BLE DASHBOARD - ESP32 DUAL-CORE VERSION
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
 * Core 1: BLE Task - Handle BLE advertising & data transmission
 * 
 * Benefits:
 * - Zero CAN message loss
 * - Real-time ampere readings
 * - Smooth BLE transmission
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

// === CONFIGURATION ===
#define CAN_TX_PIN GPIO_NUM_21
#define CAN_RX_PIN GPIO_NUM_22
#define DEVICE_NAME "Votol_BLE"

#define LED_PIN 2

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"



// Timing - OPTIMIZED for real-time
static const uint32_t BLE_SEND_PERIOD_MS = 200;    // 5 updates/sec (was 150, then 75)
static const uint32_t SERIAL_JSON_PERIOD_MS = 0;

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
TaskHandle_t canTaskHandle = NULL;
TaskHandle_t bleTaskHandle = NULL;

BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
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

// Non-critical data: protected by mutex
volatile int valRPM = 0;
volatile int valSpeed = 0;
volatile float valVolts = 0.0f;
volatile float valAmpere = 0.0f;
volatile float valPower = 0.0f;

Preferences preferences;
volatile int valSOC = 0;
unsigned long lastSoCSave = 0;

volatile int valCtrlTemp = 0;
volatile int valMotorTemp = 0;
volatile int valBattTemp = 0;

volatile char strModeBuffer[16] = "PARK";
volatile bool isBraking = false;

volatile uint16_t valCells[23] = {0};
volatile uint8_t valCellTemps[5] = {0};

volatile uint32_t valOdometer = 0;

volatile float valRemainingCapacity = 0.0f;
volatile float valFullCapacity = 0.0f;

volatile int valSOH = 0;
volatile uint16_t valCycleCount = 0;

volatile uint16_t valHighestCellVolt = 0;
volatile uint8_t  valHighestCellNum  = 0;
volatile uint16_t valLowestCellVolt  = 0;
volatile uint8_t  valLowestCellNum   = 0;
volatile uint16_t valAvgCellVolt     = 0;

volatile uint8_t valMaxTemp = 0;
volatile uint8_t valMaxTempCell = 0;
volatile uint8_t valMinTemp = 0;
volatile uint8_t valMinTempCell = 0;

volatile uint8_t valBalanceMode = 0;
volatile uint8_t valBalanceStatus = 0;
volatile uint8_t valBalanceBits[4] = {0};

volatile uint16_t rawCurrentHex = 0;
volatile uint16_t rawVoltageHex = 0;
volatile uint16_t rawSOCHex = 0;
volatile char rawBalanceHexBuffer[24] = "00 00 00 00 00 00";

// Charger data
volatile float valChargerVoltage = 0.0f;
volatile float valChargerCurrent = 0.0f;
volatile uint8_t valChargerStatus = 0;
volatile bool chargerConnected = false;
volatile unsigned long lastChargerMsg = 0;

// BMS Info (newly parsed IDs)
volatile bool bmsChargingFlag = false;        // 0x0AB40D09 byte[0]=1 when charging
volatile bool oriChargerDetected = false;     // 0x10261041 only from ORI charger
volatile unsigned long lastOriChargerMsg = 0;
volatile char bmsHwVersion[8] = "";           // 0x0A750D09 "H:v21"
volatile char bmsFwVersion[8] = "";           // 0x0A760D09 "F:v23"

volatile uint32_t canMessagesPerSec = 0;
volatile uint32_t canMsgCount = 0;
uint32_t lastSecond = 0;

unsigned long lastLEDBlink = 0;
bool ledState = false;

unsigned long heartbeatCounter = 0;

// === BLE TX state (BLE task only) ===
static char bleTxBuf[2200];
static uint16_t bleTxLen = 0;
static uint16_t bleTxOffset = 0;
static bool bleTxInProgress = false;
static uint32_t lastDataSend = 0;
static uint32_t lastSerialJson = 0;

// =============================================
// CAN PARSING (runs on Core 0)
// =============================================
void handleCANMessage(twai_message_t &msg) {
  uint32_t id = msg.identifier;

  // Only parse likely extended IDs (0x0A=Votol, 0x0E=BMS, 0x18=Charger)
  uint8_t prefix = id >> 24;
  if (prefix != 0x0A && prefix != 0x0E && prefix != 0x18 && prefix != 0x10) return;



  // Take mutex before writing shared data
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
    return; // Skip if can't get mutex quickly
  }

  // Controller basic
  if (id == 0x0A010810 && msg.data_length_code >= 8) {
    uint8_t m = msg.data[1];
    isBraking = false;

    if (m == 0x00) strcpy((char*)strModeBuffer, "PARK");
    else if (m == 0x61) strcpy((char*)strModeBuffer, "CHARGING");
    else if (m == 0x70) strcpy((char*)strModeBuffer, "DRIVE");
    else if (m == 0x50) strcpy((char*)strModeBuffer, "REVERSE");
    else if (m == 0x72 || m == 0xB2) { strcpy((char*)strModeBuffer, "BRAKE"); isBraking = true; }
    else if (m == 0xB0) strcpy((char*)strModeBuffer, "SPORT");
    else if (m == 0x78 || m == 0x08) strcpy((char*)strModeBuffer, "STAND");
    else if (m == 0xF0 || m == 0x30 || m == 0xF8) strcpy((char*)strModeBuffer, "REVERSE");

    int rpm = msg.data[2] | (msg.data[3] << 8);
    int speed = (int)(rpm * 0.1033f);
    valRPM = rpm;
    valSpeed = speed;
    atomicRPM.store(rpm, std::memory_order_release);
    atomicSpeed.store(speed, std::memory_order_release);
    valCtrlTemp = msg.data[4];
    valMotorTemp = msg.data[5];
    xSemaphoreGive(dataMutex);
    return;
  }

  // Battery temp sensors
  if (id == 0x0E6C0D09 && msg.data_length_code >= 5) {
    int sum = 0;
    for (int i = 0; i < 5; i++) {
      valCellTemps[i] = msg.data[i];
      sum += (int)valCellTemps[i];
    }
    valBattTemp = sum / 5;
    xSemaphoreGive(dataMutex);
    return;
  }

  // General info 1 (Voltage/Current/Capacity) - CRITICAL FOR REAL-TIME AMPERE
  // NOTE: Ampere/Volts/Power use ATOMIC - no mutex needed for these!
  if (id == 0x0A6D0D09 && msg.data_length_code >= 8) {
    uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    rawVoltageHex = vRaw;
    float volts = vRaw * 0.1f;
    valVolts = volts;
    atomicVoltsRaw.store((int32_t)(volts * 10.0f), std::memory_order_release);

    // Signed current (0.1A/bit) - ATOMIC UPDATE
    uint16_t iRawU = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    rawCurrentHex = iRawU;
    int16_t iRawS = (int16_t)iRawU;
    float ampere = iRawS * 0.1f;
    if (fabs(ampere) < 0.2f) ampere = 0.0f;
    valAmpere = ampere;
    atomicAmpereRaw.store((int32_t)(ampere * 10.0f), std::memory_order_release);

    uint16_t remainCap = (uint16_t)((msg.data[4] << 8) | msg.data[5]);
    valRemainingCapacity = remainCap * 0.1f;

    uint16_t fullCap = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
    valFullCapacity = fullCap * 0.1f;

    float power = volts * ampere;
    valPower = power;
    atomicPowerRaw.store((int32_t)power, std::memory_order_release);
    
    xSemaphoreGive(dataMutex);
    return;
  }

  // Battery health
  if (id == 0x0A6E0D09 && msg.data_length_code >= 6) {
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
    return;
  }

  // Cell voltage stats
  if (id == 0x0A6F0D09 && msg.data_length_code >= 8) {
    valHighestCellVolt = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    valHighestCellNum  = msg.data[2];
    valLowestCellVolt  = (uint16_t)((msg.data[3] << 8) | msg.data[4]);
    valLowestCellNum   = msg.data[5];
    valAvgCellVolt     = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
    xSemaphoreGive(dataMutex);
    return;
  }

  // Temp stats
  if (id == 0x0A700D09 && msg.data_length_code >= 6) {
    valMaxTemp = msg.data[0];
    valMaxTempCell = msg.data[1];
    valMinTemp = msg.data[4];
    valMinTempCell = msg.data[5];
    xSemaphoreGive(dataMutex);
    return;
  }

  // Balance status
  if (id == 0x0A730D09 && msg.data_length_code >= 6) {
    valBalanceMode = msg.data[0];
    valBalanceStatus = msg.data[1];
    valBalanceBits[0] = msg.data[2];
    valBalanceBits[1] = msg.data[3];
    valBalanceBits[2] = msg.data[4];
    valBalanceBits[3] = msg.data[5];

    snprintf((char*)rawBalanceHexBuffer, sizeof(rawBalanceHexBuffer), "%02X %02X %02X %02X %02X %02X",
             msg.data[0], msg.data[1], msg.data[2], msg.data[3], msg.data[4], msg.data[5]);
    xSemaphoreGive(dataMutex);
    return;
  }

  // Cell volt blocks 0x0E64..0x0E69
  if ((id & 0xFFF0FFFF) == 0x0E600D09) {
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
    return;
  }

  // Charger messages (CCS protocol)
  if ((id == 0x1810D0F3 || id == 0x1811D0F3) && msg.data_length_code >= 5) {
    uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    valChargerVoltage = vRaw * 0.1f;

    uint16_t iRaw = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    valChargerCurrent = iRaw * 0.1f;

    valChargerStatus = msg.data[4];
    chargerConnected = true;
    lastChargerMsg = millis();
    xSemaphoreGive(dataMutex);
    return;
  }

  // BMS Charging Flag (0x0AB40D09) - byte[0]=1 when charging
  if (id == 0x0AB40D09 && msg.data_length_code >= 1) {
    bmsChargingFlag = (msg.data[0] == 0x01);
    xSemaphoreGive(dataMutex);
    return;
  }

  // BMS Hardware Version (0x0A750D09) - ASCII "H:v21"
  if (id == 0x0A750D09 && msg.data_length_code >= 5) {
    memcpy((void*)bmsHwVersion, msg.data, 6);
    bmsHwVersion[6] = '\0';
    xSemaphoreGive(dataMutex);
    return;
  }

  // BMS Firmware Version (0x0A760D09) - ASCII "F:v23"
  if (id == 0x0A760D09 && msg.data_length_code >= 5) {
    memcpy((void*)bmsFwVersion, msg.data, 6);
    bmsFwVersion[6] = '\0';
    xSemaphoreGive(dataMutex);
    return;
  }

  // ORI Charger Detection (0x10261041) - only present with original charger
  if (id == 0x10261041) {
    oriChargerDetected = true;
    lastOriChargerMsg = millis();
    xSemaphoreGive(dataMutex);
    return;
  }

  // Release mutex if no match
  xSemaphoreGive(dataMutex);
}

// =============================================
// CAN TASK (Core 0 - High Priority)
// =============================================
void canTask(void *pvParameters) {

  
  twai_message_t message;
  bool gotMessage = false;
  
  while (true) {
    gotMessage = false;
    
    // Drain CAN RX queue (Rate Limited)
    int processed = 0;
    while (processed < 10 && twai_receive(&message, 0) == ESP_OK) {
      handleCANMessage(message);
      canMsgCount++;
      processed++;
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
      canMessagesPerSec = canMsgCount;
      canMsgCount = 0;
      lastSecond = nowSec;
    }

    // SOC save (moved here since we have the data)
    if (millis() - lastSoCSave > 5000) {
      int currentSoc = valSOC;
      int savedSoc = preferences.getInt("soc", -1);
      if (savedSoc != currentSoc) {
        preferences.putInt("soc", currentSoc);
        lastSoCSave = millis();
      }
    }

    // Charger timeout - reset chargerConnected if no CAN message for 5 seconds
    if (chargerConnected && (millis() - lastChargerMsg > 5000)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        chargerConnected = false;
        valChargerVoltage = 0.0f;
        valChargerCurrent = 0.0f;
        valChargerStatus = 0;
        xSemaphoreGive(dataMutex);
      }
    }

    // Adaptive rate limiting: faster when driving, slower when charging
    // Driving: 50Hz (20ms) for responsive real-time data
    // Charging: 20Hz (50ms) to reduce CPU load during charger data flood
    vTaskDelay(pdMS_TO_TICKS(chargerConnected ? 50 : 20));
  }
}

// =============================================
// BLE DATA BUILD (thread-safe read)
// =============================================
static void buildJsonInto() {
  // Local copies for thread safety
  int localRPM, localSpeed, localSOC, localCtrlTemp, localMotorTemp, localBattTemp;
  int localSOH, localCycleCount;
  float localVolts, localAmpere, localPower;
  float localRemainingCap, localFullCap;
  uint16_t localCells[23];
  uint16_t localHighestVolt, localLowestVolt, localAvgVolt;
  uint8_t localHighestNum, localLowestNum;
  uint8_t localMaxTemp, localMaxTempCell, localMinTemp, localMinTempCell;
  uint8_t localBalanceMode, localBalanceStatus, localBalanceBits[4];
  float localChargerVolt, localChargerCurrent;
  uint8_t localChargerStatus;
  bool localChargerConnected;
  unsigned long localLastChargerMsg;
  uint32_t localCanRate;
  uint32_t localOdometer;
  uint16_t localRawCurrent, localRawVoltage, localRawSOC;
  char localStrMode[16];
  char localRawBalance[24];
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
    memcpy(localBalanceBits, (void*)valBalanceBits, 4);
    localChargerVolt = valChargerVoltage;
    localChargerCurrent = valChargerCurrent;
    localChargerStatus = valChargerStatus;
    localChargerConnected = chargerConnected;
    localLastChargerMsg = lastChargerMsg;
    localCanRate = canMessagesPerSec;
    localOdometer = valOdometer;
    localRawCurrent = rawCurrentHex;
    localRawVoltage = rawVoltageHex;
    localRawSOC = rawSOCHex;
    memcpy(localCells, (void*)valCells, sizeof(localCells));
    strcpy(localStrMode, (char*)strModeBuffer);
    strcpy(localRawBalance, (char*)rawBalanceHexBuffer);
    // New BMS info
    localBmsChargingFlag = bmsChargingFlag;
    localOriCharger = oriChargerDetected;
    localLastOriChargerMsg = lastOriChargerMsg;
    strcpy(localHwVersion, (char*)bmsHwVersion);
    strcpy(localFwVersion, (char*)bmsFwVersion);
    xSemaphoreGive(dataMutex);
  } else {
    // Fallback - use last known values
    return;
  }

  // CRITICAL: Read real-time values from ATOMIC AFTER mutex release
  // This ensures freshest ampere/volts/power without blocking CAN task
  localRPM = atomicRPM.load(std::memory_order_acquire);
  localSpeed = atomicSpeed.load(std::memory_order_acquire);
  localAmpere = atomicAmpereRaw.load(std::memory_order_acquire) / 10.0f;
  localVolts = atomicVoltsRaw.load(std::memory_order_acquire) / 10.0f;
  localPower = (float)atomicPowerRaw.load(std::memory_order_acquire);

  // Note: Now CAN task can update ampere while we build JSON below

  // Calculate cell delta
  uint16_t minCell = 9999, maxCell = 0;
  for (int i = 0; i < 23; i++) {
    if (localCells[i] > 0 && localCells[i] < minCell) minCell = localCells[i];
    if (localCells[i] > maxCell) maxCell = localCells[i];
  }
  int cellDelta = (int)maxCell - (int)minCell;

  // Build balance cells array string (compact: 0/1 instead of true/false)
  char balanceCells[70];  // 23 cells * 2 chars max = 46 + commas
  int bpos = 0;
  for (int i = 0; i < 23; i++) {
    int byteIndex = i / 8;
    int bitIndex = i % 8;
    bool isBalancing = (localBalanceBits[byteIndex] & (1 << bitIndex)) != 0;
    bpos += snprintf(balanceCells + bpos, sizeof(balanceCells) - bpos, 
                     "%d%s", isBalancing ? 1 : 0, (i < 22) ? "," : "");
  }

  // Build cells array string
  char cellsStr[180];
  int cpos = 0;
  for (int i = 0; i < 23; i++) {
    cpos += snprintf(cellsStr + cpos, sizeof(cellsStr) - cpos, 
                     "%u%s", localCells[i], (i < 22) ? "," : "");
  }

  // Build complete JSON (COMPACT - backward compatible, new fields added)
  bleTxLen = snprintf(bleTxBuf, sizeof(bleTxBuf),
    "{\"rpm\":%d,"
    "\"speed\":%d,"
    "\"mode\":\"%s\","
    "\"volts\":%.1f,"
    "\"amps\":%.1f,"
    "\"power\":%.0f,"
    "\"soc\":%d,"
    "\"temps\":{\"ctrl\":%d,\"motor\":%d,\"batt\":%d},"
    "\"cells\":[%s],"
    "\"cellDelta\":%d,"
    "\"canRate\":%lu,"
    "\"odometer\":%lu,"
    "\"health\":{\"soh\":%d,\"cycles\":%u,\"remainCap\":%.1f,\"fullCap\":%.1f},"
    "\"cellVoltStats\":{\"highest\":%u,\"highestCell\":%u,\"lowest\":%u,\"lowestCell\":%u,\"avg\":%u},"
    "\"tempStats\":{\"max\":%u,\"maxCell\":%u,\"min\":%u,\"minCell\":%u},"
    "\"balance\":{\"mode\":%u,\"status\":%u,\"cells\":[%s]},"
    "\"charger\":{\"on\":%d,\"v\":%.1f,\"a\":%.1f,\"ori\":%d},"
    "\"bms\":{\"hw\":\"%s\",\"fw\":\"%s\"},"
    "\"hb\":%lu"
    "}\n",
    localRPM, localSpeed, localStrMode,
    localVolts, localAmpere, localPower, localSOC,
    localCtrlTemp, localMotorTemp, localBattTemp,
    cellsStr, cellDelta,
    (unsigned long)localCanRate,
    (unsigned long)localOdometer,
    localSOH, localCycleCount, localRemainingCap, localFullCap,
    localHighestVolt, localHighestNum, localLowestVolt, localLowestNum, localAvgVolt,
    localMaxTemp, localMaxTempCell, localMinTemp, localMinTempCell,
    localBalanceMode, localBalanceStatus, balanceCells,
    localBmsChargingFlag ? 1 : 0,  // charger.on = 0/1
    (localChargerVolt > 0.1f) ? localChargerVolt : localVolts,      // Fallback to system volts
    (localChargerCurrent > 0.1f) ? localChargerCurrent : fabs(localAmpere), // Fallback to system amps
    (millis() - localLastOriChargerMsg < 5000 && localOriCharger) ? 1 : 0,  // charger.ori = 0/1
    localHwVersion, localFwVersion,
    (unsigned long)heartbeatCounter++
  );
}

static void startBleTxIfIdle() {
  if (!deviceConnected) return;
  if (bleTxInProgress) return;

  buildJsonInto();
  bleTxOffset = 0;
  bleTxInProgress = true;


}

static void pumpBleTx() {
  if (!deviceConnected) {
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
// BLE TASK (Core 1)
// =============================================
void bleTask(void *pvParameters) {
  
  while (true) {
    // BLE reconnect advertising
    if (!deviceConnected && oldDeviceConnected) {
      delay(200);
      pServer->startAdvertising();
      oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = deviceConnected;
    }

    // Send JSON periodically
    uint32_t now = millis();
    if (!bleTxInProgress && (now - lastDataSend >= BLE_SEND_PERIOD_MS)) {
      lastDataSend = now;
      startBleTxIfIdle();
    }

    // Pump BLE chunks
    pumpBleTx();

    // Yield
    vTaskDelay(pdMS_TO_TICKS(2));  // OPTIMIZED: faster loop (was 5ms)
  }
}

// =============================================
// SETUP
// =============================================
void setup() {

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Create mutex
  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    while(1); // Critical error - halt
  }

  preferences.begin("votol", false);
  valSOC = preferences.getInt("soc", 0);

  // BLE Setup
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(512);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();



  // CAN (TWAI) Setup - Optimized Queue
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  g_config.rx_queue_len = 10; // Reduce queue to prevent stale data buildup

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
    bleTask,
    "BLE_Task",
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
  // Main loop is empty - all work done in FreeRTOS tasks
  vTaskDelay(portMAX_DELAY);
}
