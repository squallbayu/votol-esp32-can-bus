/*
 * =============================================
 * VOTOL BLE DASHBOARD - ESP32 Firmware
 * =============================================
 * 
 * Author: Zekri R (ZEKRI.ID)
 * Website: https://zekri.id
 * 
 * Based on: https://github.com/yudhaime/displaypolytronfoxrs
 * 
 * =============================================
 * DISCLAIMER / PERINGATAN
 * =============================================
 * This project is provided "AS IS" without any warranty.
 * Use at your own risk. The author is NOT responsible
 * for any damage, malfunction, or injury to your vehicle,
 * controller, battery, or any other components.
 * 
 * Proyek ini disediakan "APA ADANYA" tanpa jaminan apapun.
 * Gunakan dengan risiko Anda sendiri. Penulis TIDAK
 * bertanggung jawab atas kerusakan, malfungsi, atau cedera
 * pada kendaraan, controller, baterai, atau komponen lainnya.
 * =============================================
 * 
 * Features:
 * - BLE GATT service for data streaming
 * - Auto-reconnect capability
 * - Lower power consumption
 * - CAN bus parsing for Votol Controller + BMS
 * - Cell voltage monitoring (23 cells)
 * - Temperature monitoring (5 sensors)
 * - Balance status monitoring
 * - SOC calculation with lookup table
 */
#include <Arduino.h>
#include "driver/twai.h"

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

// CAN Sniffer
#define CAN_SNIFFER_ENABLE false

// Timing
static const uint32_t BLE_SEND_PERIOD_MS = 150;   // keep same as your original fallback
static const uint32_t SERIAL_JSON_PERIOD_MS = 0;  // 0 = OFF (recommended). set 1000/3000 if you want.

// BLE chunking (tuneable)
static const uint16_t BLE_SAFE_CHUNK = 240; // bigger chunk => fewer notify; start with 240
static const uint32_t BLE_PUMP_BUDGET_US = 6000; // how long we spend sending BLE chunks per loop
static const int BLE_PUMP_MAX_CHUNKS = 8;        // max notify calls per loop

BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected");
  }
};

// === SOC lookup table (unchanged) ===
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

// === Data state (same JSON fields) ===
int valRPM = 0;
int valSpeed = 0;
float valVolts = 0.0f;

float valAmpere = 0.0f;
float valPower  = 0.0f;

Preferences preferences;
int valSOC = 0;
unsigned long lastSoCSave = 0;

int valCtrlTemp = 0;
int valMotorTemp = 0;
int valBattTemp = 0;

String strMode = "PARK";
bool isBraking = false;

uint16_t valCells[23] = {0};
uint8_t valCellTemps[5] = {0};

uint32_t valOdometer = 0;

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
String rawBalanceHex = "00 00 00 00 00 00";

// Charger data
float valChargerVoltage = 0.0f;
float valChargerCurrent = 0.0f;
uint8_t valChargerStatus = 0;
bool chargerConnected = false;
unsigned long lastChargerMsg = 0;

uint32_t canMessagesPerSec = 0;
uint32_t canMsgCount = 0;
uint32_t lastSecond = 0;

unsigned long lastLEDBlink = 0;
bool ledState = false;

unsigned long heartbeatCounter = 0;

// === BLE TX state ===
static char bleTxBuf[2200];  // Static buffer - no heap fragmentation
static uint16_t bleTxLen = 0;
static uint16_t bleTxOffset = 0;
static bool bleTxInProgress = false;
static uint32_t lastDataSend = 0;
static uint32_t lastSerialJson = 0;

// =============================================
// CAN PARSING
// =============================================
void handleCANMessage(twai_message_t &msg) {
  uint32_t id = msg.identifier;

  // Only parse likely extended IDs
  if ((id >> 24) != 0x0A && (id >> 24) != 0x0E) return;

#if CAN_SNIFFER_ENABLE
  Serial.printf("[CAN] ID: 0x%08X Len:%d Data:", id, msg.data_length_code);
  for (int i = 0; i < msg.data_length_code; i++) Serial.printf("%02X ", msg.data[i]);
  Serial.println();
#endif

  // Controller basic
  if (id == 0x0A010810 && msg.data_length_code >= 8) {
    uint8_t m = msg.data[1];
    isBraking = false;

    if (m == 0x00) strMode = "PARK";
    else if (m == 0x70) strMode = "DRIVE";
    else if (m == 0x50) strMode = "REVERSE";
    else if (m == 0x72 || m == 0xB2) { strMode = "BRAKE"; isBraking = true; }
    else if (m == 0xB0) strMode = "SPORT";
    else if (m == 0x78 || m == 0x08) strMode = "STAND";
    else if (m == 0xF0 || m == 0x30 || m == 0xF8) strMode = "REVERSE";

    valRPM = msg.data[2] | (msg.data[3] << 8);
    valSpeed = (int)(valRPM * 0.1033f);
    valCtrlTemp = msg.data[4];
    valMotorTemp = msg.data[5];
    return;
  }

  // Battery temp sensors (DIRECT °C, NO OFFSET)
  if (id == 0x0E6C0D09 && msg.data_length_code >= 5) {
    int sum = 0;
    for (int i = 0; i < 5; i++) {
      valCellTemps[i] = msg.data[i];  // direct °C
      sum += (int)valCellTemps[i];
    }
    valBattTemp = sum / 5;
    return;
  }

  // General info 1 (Voltage/Current/Capacity)
  if (id == 0x0A6D0D09 && msg.data_length_code >= 8) {
    uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    rawVoltageHex = vRaw;
    valVolts = vRaw * 0.1f;

    // Signed current (0.1A/bit)
    uint16_t iRawU = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    rawCurrentHex = iRawU;
    int16_t iRawS = (int16_t)iRawU;
    valAmpere = iRawS * 0.1f;
    if (fabs(valAmpere) < 0.2f) valAmpere = 0.0f;

    uint16_t remainCap = (uint16_t)((msg.data[4] << 8) | msg.data[5]);
    valRemainingCapacity = remainCap * 0.1f;

    uint16_t fullCap = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
    valFullCapacity = fullCap * 0.1f;

    valPower = valVolts * valAmpere;
    return;
  }

  // Battery health
  if (id == 0x0A6E0D09 && msg.data_length_code >= 6) {
    uint16_t socVal = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    rawSOCHex = socVal;

    valSOC = (int)getSoCFromLookup(socVal);
    if (valSOC > 100) valSOC = 100;
    if (valSOC < 0) valSOC = 0;

    if (millis() - lastSoCSave > 5000) {
      int savedSoc = preferences.getInt("soc", -1);
      if (savedSoc != valSOC) {
        preferences.putInt("soc", valSOC);
        lastSoCSave = millis();
      }
    }

    uint16_t sohVal = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    valSOH = (int)(sohVal * 0.1f);
    if (valSOH > 100) valSOH = 100;

    valCycleCount = (uint16_t)((msg.data[4] << 8) | msg.data[5]);
    return;
  }

  // Cell voltage stats
  if (id == 0x0A6F0D09 && msg.data_length_code >= 8) {
    valHighestCellVolt = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    valHighestCellNum  = msg.data[2];
    valLowestCellVolt  = (uint16_t)((msg.data[3] << 8) | msg.data[4]);
    valLowestCellNum   = msg.data[5];
    valAvgCellVolt     = (uint16_t)((msg.data[6] << 8) | msg.data[7]);
    return;
  }

  // Temp stats (DIRECT °C, NO OFFSET)
  if (id == 0x0A700D09 && msg.data_length_code >= 6) {
    valMaxTemp = msg.data[0];
    valMaxTempCell = msg.data[1];
    valMinTemp = msg.data[4];
    valMinTempCell = msg.data[5];
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

    char buff[20];
    snprintf(buff, sizeof(buff), "%02X %02X %02X %02X %02X %02X",
             msg.data[0], msg.data[1], msg.data[2], msg.data[3], msg.data[4], msg.data[5]);
    rawBalanceHex = String(buff);
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
    return;
  }

  // Charger messages (0x1810D0F3 or 0x1811D0F3)
  if ((id == 0x1810D0F3 || id == 0x1811D0F3) && msg.data_length_code >= 5) {
    // Typical charger protocol: Voltage (2 bytes), Current (2 bytes), Status (1 byte)
    uint16_t vRaw = (uint16_t)((msg.data[0] << 8) | msg.data[1]);
    valChargerVoltage = vRaw * 0.1f;

    uint16_t iRaw = (uint16_t)((msg.data[2] << 8) | msg.data[3]);
    valChargerCurrent = iRaw * 0.1f;

    valChargerStatus = msg.data[4];
    chargerConnected = true;
    lastChargerMsg = millis();
    return;
  }
}

// =============================================
// BLE DATA SEND (JSON FORMAT MUST STAY SAME)
// Using snprintf for better memory efficiency
// =============================================
static void buildJsonInto() {
  uint16_t minCell = 9999, maxCell = 0;
  for (int i = 0; i < 23; i++) {
    if (valCells[i] > 0 && valCells[i] < minCell) minCell = valCells[i];
    if (valCells[i] > maxCell) maxCell = valCells[i];
  }
  int cellDelta = (int)maxCell - (int)minCell;

  // Build balance cells array string
  char balanceCells[120];
  int bpos = 0;
  for (int i = 0; i < 23; i++) {
    int byteIndex = i / 8;
    int bitIndex = i % 8;
    bool isBalancing = (valBalanceBits[byteIndex] & (1 << bitIndex)) != 0;
    bpos += snprintf(balanceCells + bpos, sizeof(balanceCells) - bpos, 
                     "%s%s", isBalancing ? "true" : "false", (i < 22) ? "," : "");
  }

  // Build cells array string
  char cellsStr[180];
  int cpos = 0;
  for (int i = 0; i < 23; i++) {
    cpos += snprintf(cellsStr + cpos, sizeof(cellsStr) - cpos, 
                     "%u%s", valCells[i], (i < 22) ? "," : "");
  }

  // Build complete JSON - EXACT same format as before
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
    "\"cellVoltStats\":{\"highest\":%u,\"highestCell\":%u,\"lowest\":%u,\"lowestCell\":%u,\"average\":%u,\"delta\":%d},"
    "\"tempStats\":{\"max\":%u,\"maxCell\":%u,\"min\":%u,\"minCell\":%u},"
    "\"balance\":{\"mode\":%u,\"status\":%u,\"cells\":[%s]},"
    "\"charger\":{\"connected\":%s,\"voltage\":%.1f,\"current\":%.1f,\"status\":%u},"
    "\"heartbeat\":%lu,"
    "\"debug\":{\"currentHex\":\"0x%X\",\"voltageHex\":\"0x%X\",\"socHex\":\"0x%X\",\"balanceHex\":\"%s\"}"
    "}\n",
    valRPM, valSpeed, strMode.c_str(),
    valVolts, valAmpere, valPower, valSOC,
    valCtrlTemp, valMotorTemp, valBattTemp,
    cellsStr, cellDelta,
    (unsigned long)canMessagesPerSec,
    (unsigned long)valOdometer,
    valSOH, valCycleCount, valRemainingCapacity, valFullCapacity,
    valHighestCellVolt, valHighestCellNum, valLowestCellVolt, valLowestCellNum, valAvgCellVolt, (int)valHighestCellVolt - (int)valLowestCellVolt,
    valMaxTemp, valMaxTempCell, valMinTemp, valMinTempCell,
    valBalanceMode, valBalanceStatus, balanceCells,
    (millis() - lastChargerMsg < 5000 && chargerConnected) ? "true" : "false",
    valChargerVoltage, valChargerCurrent, valChargerStatus,
    (unsigned long)heartbeatCounter++,
    rawCurrentHex, rawVoltageHex, rawSOCHex, rawBalanceHex.c_str()
  );
}

static void startBleTxIfIdle() {
  if (!deviceConnected) return;
  if (bleTxInProgress) return; // never queue

  buildJsonInto();
  bleTxOffset = 0;
  bleTxInProgress = true;

  // Serial JSON OFF by default (blocking risk)
  if (SERIAL_JSON_PERIOD_MS > 0) {
    uint32_t now = millis();
    if (now - lastSerialJson >= SERIAL_JSON_PERIOD_MS) {
      lastSerialJson = now;
      Serial.print("[JSON]");
      Serial.println(bleTxBuf);
    }
  }
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

  // Multi-chunk per loop (fast) with time budget
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
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== VOTOL BLE DASHBOARD OPT FINAL (Fast BLE + Direct Temps) ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  preferences.begin("votol", false);
  valSOC = preferences.getInt("soc", 0);
  Serial.printf("[MEM] Last SOC loaded: %d%%\n", valSOC);

  // BLE
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

  Serial.print("✓ BLE: ");
  Serial.println(DEVICE_NAME);

  // CAN (TWAI)
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);

  // IMPORTANT: ensure bitrate matches your vehicle bus
  // If your bus is 500kbps, change to TWAI_TIMING_CONFIG_500KBITS()
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    if (twai_start() == ESP_OK) Serial.println("✓ CAN: started\n");
    else Serial.println("✗ CAN: start failed\n");
  } else {
    Serial.println("✗ CAN: driver install failed\n");
  }
}

// =============================================
// LOOP
// =============================================
void loop() {
  // BLE reconnect advertising
  if (!deviceConnected && oldDeviceConnected) {
    delay(200);
    pServer->startAdvertising();
    Serial.println("[BLE] Advertising for reconnect...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Drain CAN RX queue (prevents backlog & improves current responsiveness)
  twai_message_t message;
  bool gotMessage = false;
  while (twai_receive(&message, 0) == ESP_OK) {
    handleCANMessage(message);
    canMsgCount++;
    gotMessage = true;
  }

  // LED optimization: only toggle once per batch, not per message
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

  // Start sending JSON only when idle (no queue)
  uint32_t now = millis();
  if (!bleTxInProgress && (now - lastDataSend >= BLE_SEND_PERIOD_MS)) {
    lastDataSend = now;
    startBleTxIfIdle();
  }

  // Pump BLE chunks fast
  pumpBleTx();
}
