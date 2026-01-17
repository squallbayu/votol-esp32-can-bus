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
 * - WiFi OTA updates
 */


#include <Arduino.h>
#include "driver/twai.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h> // NEW: For saving SOC

// === CONFIGURATION ===
#define CAN_TX_PIN GPIO_NUM_21
#define CAN_RX_PIN GPIO_NUM_22
#define DEVICE_NAME "Votol_BLE"

// Lookup table SOC to BMS value (0-100%)
// Input: Index (0-100) = SOC %
// Output: Value = Raw CAN Data
const uint16_t socToBms[101] = {
    0, 60,70,80,90,95,105,115,125,135,140,150,160,170,180,185,195,205,215,225,
    230,240,250,260,270,275,285,295,305,315,320,330,340,350,360,365,375,385,395,405,
    410,420,430,440,450,455,465,475,485,495,500,510,520,530,540,550,555,565,575,585,
    590,600,610,620,630,635,645,655,665,675,680,690,700,710,720,725,735,745,755,765,
    770,780,790,800,810,815,825,835,845,855,860,870,880,890,900,905,915,925,935,945,950
};

// Helper to convert Raw BMS Value -> SOC % using lookup table
float getSoCFromLookup(uint16_t raw) {
    if (raw >= socToBms[100]) return 100.0;
    if (raw <= socToBms[0]) return 0.0;

    for (int i = 0; i < 100; i++) {
        if (raw >= socToBms[i] && raw <= socToBms[i+1]) {
             // Linear Interpolation
             float range = socToBms[i+1] - socToBms[i];
             float delta = raw - socToBms[i];
             if (range == 0) return (float)i; // Avoid div by zero
             return (float)i + (delta / range);
        }
    }
    return 0.0;
}

// CAN Sniffer - Set to true to log ALL CAN messages (for finding odometer, etc)
#define CAN_SNIFFER_ENABLE false  // Disabled for stability

// LED
#define LED_PIN 2

// BLE UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Connection callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Client connected");
    };

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("[BLE] Client disconnected");
    }
};

// Data state
int valRPM = 0;
int valSpeed = 0;
float valVolts = 0.0;

// BMS DOCUMENTATION METHOD (From 0x0A6D0D09)
float valAmpere = 0.0;   // BMS current (main)
float valPower = 0.0;    // BMS power




// Preferences
Preferences preferences;
int valSOC = 0;
unsigned long lastSoCSave = 0;
int valCtrlTemp = 0;
int valMotorTemp = 0;
int valBattTemp = 0;
String strMode = "PARK";
bool isBraking = false; // NEW: Brake status for sign
uint16_t valCells[23] = {0};


// NEW: Cell Temperatures (5 sensors)
uint8_t valCellTemps[5] = {0};

// NEW: BMS Info
String bmsSerial = "";
String bmsHardwareVer = "";
String bmsFirmwareVer = "";

uint32_t valOdometer = 0;

// BMS Health & Statistics (corrected based on official doc)
// From 0x0A6D0D09
float valRemainingCapacity = 0.0;  // Remaining capacity (Ah)
float valFullCapacity = 0.0;       // Full charge capacity (Ah)

// From 0x0A6E0D09
int valSOH = 0;                    // State of Health (%)
uint16_t valCycleCount = 0;        // Number of charge cycles

// From 0x0A6F0D09 - Cell Voltage Statistics
uint16_t valHighestCellVolt = 0;   // Highest cell voltage (mV)
uint8_t valHighestCellNum = 0;     // Cell number with highest voltage
uint16_t valLowestCellVolt = 0;    // Lowest cell voltage (mV)
uint8_t valLowestCellNum = 0;      // Cell number with lowest voltage
uint16_t valAvgCellVolt = 0;       // Average cell voltage (mV)

// From 0x0A700D09 - Temperature Statistics
uint8_t valMaxTemp = 0;            // Maximum temperature (°C) - direct value, no offset
uint8_t valMaxTempCell = 0;        // Cell number with max temp
uint8_t valMinTemp = 0;            // Minimum temperature (°C) - direct value, no offset
uint8_t valMinTempCell = 0;        // Cell number with min temp

// From 0x0A730D09 - Balance Status
uint8_t valBalanceMode = 0;        // 0=automatic, 1=mode1
uint8_t valBalanceStatus = 0;      // 0=no_balance, 1=charge_balance, 2=discharge_balance, 3=standstill
uint8_t valBalanceBits[4] = {0};   // Bitmasks for cells 1-8, 9-16, 17-24, 25-32

// Debug: Raw hex values
uint16_t rawCurrentHex = 0;
uint16_t rawVoltageHex = 0;
uint16_t rawSOCHex = 0;
String rawBalanceHex = "00 00 00 00 00 00"; // Default to prevent empty string error

uint32_t canMessagesPerSec = 0;
uint32_t canMsgCount = 0;
uint32_t lastSecond = 0;
unsigned long lastDataSend = 0; // Moved here for scope visibility

bool canActive = false;
unsigned long lastCANMessage = 0;
#define CAN_TIMEOUT 2000

// LED
unsigned long lastLEDBlink = 0;
bool ledState = false;

// Keep-alive heartbeat
unsigned long heartbeatCounter = 0;

// =============================================
// CAN PARSING
// =============================================


void handleCANMessage(twai_message_t &msg) { // Renamed from parseVotolMessage
    uint32_t id = msg.identifier;
    
    // --- VOTOL CAN PARSING ---
    // Only parse if it looks like Votol Extended ID (0x0A...) or Cell Info (0x0E...)
    if ((id >> 24) == 0x0A || (id >> 24) == 0x0E) {  
        
        // CAN Sniffer - Log all messages
        #if CAN_SNIFFER_ENABLE
        Serial.printf("[CAN] ID: 0x%08X, Len: %d, Data: ", id, msg.data_length_code);
        for(int i = 0; i < msg.data_length_code; i++) {
            Serial.printf("%02X ", msg.data[i]);
        }
        Serial.println();
        #endif
        
        if (id == 0x0A010810 && msg.data_length_code >= 8) {
            uint8_t m = msg.data[1];
            isBraking = false; // Default not braking
            
            if (m == 0x00) strMode = "PARK";
            else if (m == 0x70) strMode = "DRIVE";
            else if (m == 0x50) strMode = "REVERSE";
            else if (m == 0x72 || m == 0xB2) {
                strMode = "BRAKE"; 
                isBraking = true;
            }
            else if (m == 0xB0) strMode = "SPORT";
            else if (m == 0x78 || m == 0x08) strMode = "STAND";
            else if (m == 0xF0 || m == 0x30 || m == 0xF8) strMode = "REVERSE";
            // else keep previous
            
            valRPM = msg.data[2] | (msg.data[3] << 8);
            valSpeed = valRPM * 0.1033;
            valCtrlTemp = msg.data[4];
            valMotorTemp = msg.data[5];

            // INSTANT REACT: RPM/Speed
            if (millis() - lastDataSend > 30) {
                 sendBLEData();
            }
        }
        else if (id == 0x0A010A10 && msg.data_length_code >= 6) {
            // [0x0A010A10] - NO battery temp here
            // Byte 5 (0x32 = 50) is NOT battery temperature
        }
        else if (id == 0x0E6C0D09 && msg.data_length_code >= 5) {
            // CELL TEMPERATURES (5 sensors)
            for (int i = 0; i < 5 && i < msg.data_length_code; i++) {
                valCellTemps[i] = msg.data[i];
            }
            int sum = 0;
            for (int i = 0; i < 5; i++) {
                sum += valCellTemps[i];
            }
            valBattTemp = sum / 5;
        }
        else if (id == 0x0A6D0D09 && msg.data_length_code >= 8) {
            // ========================================
            // GENERAL INFORMATION 1 (0x0A6D0D09)
            // ========================================
            // Byte 0-1: Total Voltage (0.1V/bit)
            // Byte 2-3: Total Current (0.1A/bit, signed)
            // Byte 4-5: Remaining Capacity (0.1Ah/bit)
            // Byte 6-7: Full Charge Capacity (0.1Ah/bit)
            
            // Voltage
            uint16_t vRaw = (msg.data[0] << 8) | msg.data[1];
            rawVoltageHex = vRaw;
            valVolts = vRaw * 0.1;
            
            // Current (signed, 0.1A/bit)
            uint16_t rawCurrent = (msg.data[2] << 8) | msg.data[3];
            rawCurrentHex = rawCurrent;
            bool isDischarge = (rawCurrent & 0x8000) != 0;
            
            if (isDischarge) {
                int complement = (0x10000 - rawCurrent);
                valAmpere = -(complement * 0.1);
            } else {
                valAmpere = rawCurrent * 0.1;
            }
            if (abs(valAmpere) < 1.0) valAmpere = 0.0;
            
            // Remaining Capacity (Byte 4-5)
            uint16_t remainCap = (msg.data[4] << 8) | msg.data[5];
            valRemainingCapacity = remainCap * 0.1;
            
            // Full Charge Capacity (Byte 6-7)
            uint16_t fullCap = (msg.data[6] << 8) | msg.data[7];
            valFullCapacity = fullCap * 0.1;
            
            // Power
            valPower = valVolts * valAmpere;

            // INSTANT REACT: Ampere/Volts
            if (millis() - lastDataSend > 30) {
                 sendBLEData();
            }
        }
        else if (id == 0x0A6E0D09 && msg.data_length_code >= 6) {
            // ========================================
            // BATTERY HEALTH (0x0A6E0D09)
            // ========================================
            // Byte 0-1: SOC (0.1%/bit)
            // Byte 2-3: SOH (0.1%/bit)
            // Byte 4-5: Cycle count
            
            // SOC
            uint16_t socVal = (msg.data[0] << 8) | msg.data[1];
            rawSOCHex = socVal;
            // OLD: valSOC = socVal * 0.1;
            // NEW: Use Lookup Table
            valSOC = getSoCFromLookup(socVal); 
            if (valSOC > 100) valSOC = 100;

            // SAVE SOC to Memory (Changes only + Rate Limit 5s)
            if (millis() - lastSoCSave > 5000) {
                int savedSoc = preferences.getInt("soc", -1);
                if (savedSoc != valSOC) {
                    preferences.putInt("soc", valSOC);
                    lastSoCSave = millis();
                    // Serial.println("[MEM] SOC Saved");
                }
            }
            
            // SOH (State of Health)
            uint16_t sohVal = (msg.data[2] << 8) | msg.data[3];
            valSOH = sohVal * 0.1;  // 0.1%/bit
            if (valSOH > 100) valSOH = 100;
            
            // Cycle Count
            valCycleCount = (msg.data[4] << 8) | msg.data[5];
        }
        else if (id == 0x0A6F0D09 && msg.data_length_code >= 8) {
            // ========================================
            // CELL VOLTAGE STATISTICS (0x0A6F0D09)
            // ========================================
            // Byte 0-1: Highest cell voltage (1mV/bit)
            // Byte 2: Cell number tertinggi
            // Byte 3-4: Lowest cell voltage (1mV/bit)
            // Byte 5: Cell number terendah
            // Byte 6-7: Average cell voltage (1mV/bit)
            
            valHighestCellVolt = (msg.data[0] << 8) | msg.data[1];
            valHighestCellNum = msg.data[2];
            
            valLowestCellVolt = (msg.data[3] << 8) | msg.data[4];
            valLowestCellNum = msg.data[5];
            
            
            valAvgCellVolt = (msg.data[6] << 8) | msg.data[7];
        }
        else if (id == 0x0A700D09 && msg.data_length_code >= 6) {
            // ========================================
            // TEMPERATURE STATISTICS (0x0A700D09)
            // ========================================
            // Byte 0: Max temp (direct °C, no offset)
            // Byte 1: Cell index
            // Byte 4: Min temp (direct °C, no offset)
            // Byte 5: Cell index
            
            valMaxTemp = msg.data[0];  // Direct value, no -40 offset
            valMaxTempCell = msg.data[1];
            
            valMinTemp = msg.data[4];  // Direct value, no -40 offset
            valMinTempCell = msg.data[5];
        }
        else if (id == 0x0A730D09 && msg.data_length_code >= 6) {
            // ========================================
            // BALANCE STATUS (0x0A730D09)
            // ========================================
            // Byte 0: Balance mode (0=automatic, 1=mode1)
            // Byte 1: Balance status (0=no, 1=charge, 2=discharge, 3=standstill)
            // Byte 2: Balance bitmask cells 1-8
            // Byte 3: Balance bitmask cells 9-16
            // Byte 4: Balance bitmask cells 17-24
            // Byte 5: Balance bitmask cells 25-32
            
            // Balance Status (from 0x0A730D09)
            valBalanceMode = msg.data[0];
            valBalanceStatus = msg.data[1];
            valBalanceBits[0] = msg.data[2];  // Cells 1-8
            valBalanceBits[1] = msg.data[3];  // Cells 9-16
            valBalanceBits[2] = msg.data[4];  // Cells 17-24
            valBalanceBits[3] = msg.data[5];  // Cells 25-32

            // Store raw hex for debug
            char buff[20];
            sprintf(buff, "%02X %02X %02X %02X %02X %02X", 
                msg.data[0], msg.data[1], msg.data[2], msg.data[3], msg.data[4], msg.data[5]);
            rawBalanceHex = String(buff);
        }
        // Odometer parser removed - ID 0x0A700D09 data (28) does not match user's odometer (7990)
        // Needs external calculation or manual offset

        else if ((id & 0xFFF0FFFF) == 0x0E600D09) {
            Serial.print("[CELL-DBG] ID: 0x"); Serial.println(id, HEX);
            int baseIndex = -1;
            switch(id) {
                case 0x0E640D09: baseIndex = 0; break;
                case 0x0E650D09: baseIndex = 4; break;
                case 0x0E660D09: baseIndex = 8; break;
                case 0x0E670D09: baseIndex = 12; break;
                case 0x0E680D09: baseIndex = 16; break;
                case 0x0E690D09: baseIndex = 20; break;
            }
            
            if (baseIndex >= 0) {
                // Serial.print("[CELL-PARSE] ID 0x"); Serial.print(id, HEX);
                for(int i=0; i<4 && (baseIndex+i)<23; i++) {
                    int offset = i*2;
                    if(offset+1 < msg.data_length_code) {
                        valCells[baseIndex+i] = (msg.data[offset] << 8) | msg.data[offset+1];
                    }
                }
            }
        }
    }
}

// =============================================
// BLE DATA SEND
// =============================================
void sendBLEData() {
    if (!deviceConnected) {
        return;
    }
    
    // Serial.println("[BLE] Sending..."); // Removed to reduce Serial load
    
    // Calculate delta
    uint16_t minCell = 9999, maxCell = 0;
    for(int i=0; i<23; i++) {
        if(valCells[i] > 0 && valCells[i] < minCell) minCell = valCells[i];
        if(valCells[i] > maxCell) maxCell = valCells[i];
    }
    int cellDelta = maxCell - minCell;
    
    // Build JSON (keep-alive even if no data)
    String json = "{";
    json += "\"rpm\":" + String(valRPM) + ",";
    json += "\"speed\":" + String(valSpeed) + ",";
    json += "\"mode\":\"" + strMode + "\",";
    json += "\"volts\":" + String(valVolts, 1) + ",";
    json += "\"amps\":" + String(valAmpere, 1) + ",";
    json += "\"power\":" + String(valPower, 0) + ",";
    json += "\"soc\":" + String(valSOC) + ",";
    json += "\"temps\":{";
    json += "\"ctrl\":" + String(valCtrlTemp) + ",";
    json += "\"motor\":" + String(valMotorTemp) + ",";
    json += "\"batt\":" + String(valBattTemp);
    json += "},";
    json += "\"cells\":[";
    for(int i=0; i<23; i++) {
        json += String(valCells[i]);
        if(i < 22) json += ",";
    }
    json += "],";
    json += "\"cellDelta\":" + String(cellDelta) + ",";
    json += "\"canRate\":" + String(canMessagesPerSec) + ",";
    
    // NEW: Cell Temperatures (removed to reduce BLE payload size)
    // json += "\"cellTemps\":[";
    // for(int i=0; i<5; i++) {
    //     json += String(valCellTemps[i]);
    //     if(i < 4) json += ",";
    // }
    // json += "],";
    
    // NEW: Cell Voltages
    // (Duplicate 'cells' block removed)
    
    // BMS Info removed from BLE (too large, check Serial Monitor instead)
    
    // NEW: Odometer
    json += "\"odometer\":" + String(valOdometer) + ",";
    
    // Battery Health Metrics (from 0x0A6D0D09 + 0x0A6E0D09)
    json += "\"health\":{";
    json += "\"soh\":" + String(valSOH) + ",";                                    // From 0x0A6E0D09
    json += "\"cycles\":" + String(valCycleCount) + ",";                         // From 0x0A6E0D09
    json += "\"remainCap\":" + String(valRemainingCapacity, 1) + ",";           // From 0x0A6D0D09
    json += "\"fullCap\":" + String(valFullCapacity, 1);                        // From 0x0A6D0D09
    json += "},";
    
    // Cell Voltage Statistics (from 0x0A6F0D09)
    json += "\"cellVoltStats\":{";
    json += "\"highest\":" + String(valHighestCellVolt) + ",";
    json += "\"highestCell\":" + String(valHighestCellNum) + ",";
    json += "\"lowest\":" + String(valLowestCellVolt) + ",";
    json += "\"lowestCell\":" + String(valLowestCellNum) + ",";
    json += "\"average\":" + String(valAvgCellVolt) + ",";
    json += "\"delta\":" + String(valHighestCellVolt - valLowestCellVolt);
    json += "},";
    
    json += "\"tempStats\":{";
    json += "\"max\":" + String(valMaxTemp) + ",";
    json += "\"maxCell\":" + String(valMaxTempCell) + ",";
    json += "\"min\":" + String(valMinTemp) + ",";
    json += "\"minCell\":" + String(valMinTempCell);
    json += "},";
    
    // Balance Status (from 0x0A730D09)
    json += "\"balance\":{";
    json += "\"mode\":" + String(valBalanceMode) + ",";
    json += "\"status\":" + String(valBalanceStatus) + ",";
    
    // Balance cells array (23 booleans)
    json += "\"cells\":[";
    for (int i = 0; i < 23; i++) {
        int byteIndex = i / 8;
        int bitIndex = i % 8;
        bool isBalancing = (valBalanceBits[byteIndex] & (1 << bitIndex)) != 0;
        json += isBalancing ? "true" : "false";
        if (i < 22) json += ",";
    }
    json += "]";
    json += "},";
    
    // Keep-alive heartbeat (increment every send to prevent timeout)
    json += "\"heartbeat\":" + String(heartbeatCounter++) + ",";
    
    // Debug info (raw hex values)
    json += "\"debug\":{";
    json += "\"currentHex\":\"0x" + String(rawCurrentHex, HEX) + "\",";
    json += "\"voltageHex\":\"0x" + String(rawVoltageHex, HEX) + "\",";
    json += "\"socHex\":\"0x" + String(rawSOCHex, HEX) + "\",";
    json += "\"balanceHex\":\"" + rawBalanceHex + "\"";
    json += "}";
    
    json += "}\n";
    
    // Send to Serial (for USB fallback)
    Serial.print("[JSON]");
    Serial.println(json);
    
    // Send via BLE - CHUNKED TRANSMISSION
    if (deviceConnected) {
        const int CHUNK_SIZE = 500;
        int totalLen = json.length();
        const char* jsonData = json.c_str();
        
        for (int offset = 0; offset < totalLen; offset += CHUNK_SIZE) {
            int chunkLen = min(CHUNK_SIZE, totalLen - offset);
            pCharacteristic->setValue((uint8_t*)(jsonData + offset), chunkLen);
            pCharacteristic->notify();
            delay(20);
        }
    }
    
    // Small delay to prevent buffer overflow
    delay(10);
    lastDataSend = millis(); // Update timer for throttling
}

// =============================================
// SETUP
// =============================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n=== VOTOL BLE DASHBOARD v2.2 (Hybrid + Stable) ===");
    
    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Initialize Preferences
    preferences.begin("votol", false); 
    // Load last known SOC
    valSOC = preferences.getInt("soc", 0);
    Serial.printf("[MEM] Last SOC loaded: %d%%\n", valSOC);
    
    // BLE
    BLEDevice::init(DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    BLEService *pService = pServer->createService(SERVICE_UUID);
    
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    
    pCharacteristic->addDescriptor(new BLE2902());
    
    // Set larger MTU for better throughput
    BLEDevice::setMTU(512);
    
    pService->start();
    
    // Optimize advertising for stability
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // 7.5ms
    pAdvertising->setMaxPreferred(0x12);  // 22.5ms
    BLEDevice::startAdvertising();
    
    Serial.print("✓ BLE: ");
    Serial.println(DEVICE_NAME);
    
    // CAN
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        twai_start();
        Serial.println("✓ CAN: 250kbps\n");
    }
}



void loop() {
    // Handle reconnection
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        Serial.println("[BLE] Advertising for reconnect...");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
    
    // Read CAN
    twai_message_t message;
    if (twai_receive(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
        canActive = true;
        lastCANMessage = millis();
        
        digitalWrite(LED_PIN, HIGH);
        lastLEDBlink = millis();
        ledState = true;
        
        handleCANMessage(message);
        canMsgCount++;
        
        // Sniffer (Disabled by default to prevent crash)
        #if CAN_SNIFFER_ENABLE
        if (message.identifier != VOTOL_REQ_ID && message.identifier != VOTOL_RESP_ID) {
            Serial.printf("[CAN] ID: 0x%08X, Len: %d, Data:", (unsigned long)message.identifier, message.data_length_code);
            for(int i = 0; i < message.data_length_code; i++) {
                Serial.printf("%02X ", message.data[i]);
            }
            Serial.println();
        }
        #endif
    }
    
    if (ledState && (millis() - lastLEDBlink > 50)) {
        digitalWrite(LED_PIN, LOW);
        ledState = false;
    }
    
    // CAN rate
    if (millis() / 1000 != lastSecond) {
        canMessagesPerSec = canMsgCount;
        canMsgCount = 0;
        lastSecond = millis() / 1000;
    }

    // Active Request Removed

    
    // Send BLE data every 150ms (Fallback Timer)
    if (millis() - lastDataSend >= 150) {
        // lastDataSend = millis(); // sendBLEData updates this now? No, stick to calling it.
        // wait, sendBLEData needs to update lastDataSend.
        sendBLEData();
    }
}
