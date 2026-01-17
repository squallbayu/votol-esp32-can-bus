/*
 * =============================================
 * VOTOL FRAME REQUEST V2 - IMPROVED & STABLE
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
 * Key Features:
 * - Listen-before-talk (bus sampling)
 * - State machine with exponential backoff
 * - Periodic cooldown (prevent 15min hang)
 * - Request budget system
 * - Auto CAN recovery
 * - Memory leak protection
 * 
 * IMPROVEMENTS over friend's code:
 * - Longer intervals (prevent Votol stress)
 * - Cooldown period every 10 minutes
 * - Request budget (max 100 req/min)
 * - Better memory management
 * - Health monitoring
 */

#include <Arduino.h>
#include "driver/twai.h"

// === CONFIGURATION ===
#define CAN_TX_PIN GPIO_NUM_21
#define CAN_RX_PIN GPIO_NUM_22
#define LED_PIN 2

// === CAN IDs ===
#define VOTOL_REQUEST_ID  0x3FF  // 1023
#define VOTOL_RESPONSE_ID 0x3FE  // 1022

// === REQUEST FRAMES (DO NOT CHANGE!) ===
const uint8_t REQ1[8] = {0x09, 0x55, 0xAA, 0xAA, 0x00, 0xAA, 0x00, 0x00};
const uint8_t REQ2[8] = {0x00, 0x18, 0xAA, 0x05, 0xD2, 0x00, 0x20, 0x33};

// === TIMING PARAMETERS ===
const unsigned long BASE_INTERVAL_MS      = 300;   // Base: 300ms (Match friend's code)
const unsigned long MIN_INTERVAL_MS       = 150;   // Min: 150ms
const unsigned long MAX_INTERVAL_MS       = 2000;  // Max: 2s
const unsigned long RESPONSE_TIMEOUT_MS   = 350;   // Wait 350ms (Match friend's code)
const unsigned long LISTEN_BEFORE_TALK_MS = 8;     // Sample bus 8ms
const unsigned int  BETWEEN_FRAME_DELAY_US = 3000; // Delay between REQ1 & REQ2

// === BACKOFF & RECOVERY ===
const uint8_t  MAX_BACKOFF_ATTEMPTS = 5;
const uint32_t CAN_RECOVERY_INTERVAL = 60000;  // Recovery every 60s if issues

// === ANTI-HANG PROTECTIONS ===
const uint32_t COOLDOWN_PERIOD_MS    = 30000;  // 30s cooldown
const uint32_t COOLDOWN_INTERVAL_MS  = 600000; // Every 10 minutes
const uint8_t  MAX_REQUESTS_PER_MIN  = 100;    // Budget limit

// === STATE MACHINE ===
enum VotolState {
    VS_IDLE,
    VS_CHECK_BUDGET,
    VS_SAMPLE_BUS,
    VS_SEND_REQUEST,
    VS_WAIT_RESPONSE,
    VS_PARSE_DATA,
    VS_BACKOFF,
    VS_COOLDOWN,
    VS_RECOVERY
};

// === DATA STRUCTURES ===
struct VotolData {
    float voltage;
    float current;
    uint16_t rpm;
    int8_t controllerTemp;
    int8_t motorTemp;
    
    // Status bits
    uint8_t gear;
    bool reverse;
    bool parking;
    bool brake;
    bool antitheft;
    bool sidestand;
    bool regen;
    
    // Stats
    uint32_t totalRequests;
    uint32_t successCount;
    uint32_t timeoutCount;
    uint32_t cooldownCount;
    uint32_t recoveryCount;
    float successRate;
    unsigned long lastResponseTime;
};

// === GLOBAL VARIABLES ===
VotolState currentState = VS_IDLE;
VotolData votolData = {0};
uint8_t responseBuffer[24];
uint8_t frameCount = 0;

unsigned long lastRequestTime = 0;
unsigned long lastCooldownTime = 0;
unsigned long lastRecoveryTime = 0;
unsigned long currentInterval = BASE_INTERVAL_MS;

uint8_t backoffAttempts = 0;
unsigned long stateStartTime = 0;

// Request budget tracking
uint8_t requestsThisMinute = 0;
unsigned long minuteStartTime = 0;

// Health monitoring
uint32_t consecutiveTimeouts = 0;
const uint8_t MAX_CONSECUTIVE_TIMEOUTS = 10;

// LED
bool ledState = false;
unsigned long lastLEDBlink = 0;

// =============================================
// HELPER FUNCTIONS
// =============================================

// Old isBusActive removed, we rely on passive draining now
// void isBusActive... REMOVED

// Transmit CAN frame
esp_err_t transmitFrame(uint32_t id, const uint8_t* data) {
    twai_message_t msg = {};
    msg.identifier = id;
    msg.extd = 0;
    msg.rtr = 0;
    msg.data_length_code = 8;
    memcpy(msg.data, data, 8);
    return twai_transmit(&msg, pdMS_TO_TICKS(100));
}

// Send request pair
bool sendRequestPair() {
    esp_err_t r1 = transmitFrame(VOTOL_REQUEST_ID, REQ1);
    if (r1 != ESP_OK) {
        Serial.printf("[TX] REQ1 failed: %d\n", r1);
        return false;
    }
    
    delayMicroseconds(BETWEEN_FRAME_DELAY_US);
    
    esp_err_t r2 = transmitFrame(VOTOL_REQUEST_ID, REQ2);
    if (r2 != ESP_OK) {
        Serial.printf("[TX] REQ2 failed: %d\n", r2);
        return false;
    }
    
    Serial.println("[TX] Request pair sent");
    votolData.totalRequests++;
    requestsThisMinute++;
    
    return true;
}

// Parse response buffer
void parseResponseData() {
    // Battery Voltage (Byte 7-8)
    uint16_t rawVolt = (responseBuffer[7] << 8) | responseBuffer[8];
    votolData.voltage = rawVolt * 0.1;
    
    // Battery Current (Byte 9-10)
    int16_t rawCurr = (int16_t)((responseBuffer[9] << 8) | responseBuffer[10]);
    votolData.current = -(rawCurr * 0.1);  // Negative convention
    
    // Deadband
    if (fabs(votolData.current) < 0.5) votolData.current = 0.0;
    
    // RPM (Byte 16-17)
    votolData.rpm = (responseBuffer[16] << 8) | responseBuffer[17];
    
    // Temperatures
    votolData.controllerTemp = responseBuffer[18] - 50;
    votolData.motorTemp = responseBuffer[19] - 50;
    
    // Status bitfield (Byte 22)
    uint8_t status = responseBuffer[22];
    votolData.gear = status & 0x03;
    votolData.reverse = (status & (1 << 2)) != 0;
    votolData.parking = (status & (1 << 3)) != 0;
    votolData.brake = (status & (1 << 4)) != 0;
    votolData.antitheft = (status & (1 << 5)) != 0;
    votolData.sidestand = (status & (1 << 6)) != 0;
    votolData.regen = (status & (1 << 7)) != 0;
    
    // Update stats
    votolData.successCount++;
    consecutiveTimeouts = 0;
    votolData.successRate = (votolData.successCount * 100.0) / votolData.totalRequests;
    
    Serial.printf("[DATA] V=%.1fV | I=%.1fA | RPM=%d | Ctrl=%d°C | Motor=%d°C\n",
        votolData.voltage, votolData.current, votolData.rpm,
        votolData.controllerTemp, votolData.motorTemp);
}

// CAN bus recovery
void performRecovery() {
    Serial.println("[RECOVERY] Restarting CAN bus...");
    twai_stop();
    delay(100);
    twai_start();
    delay(100);
    
    // Reset counters
    backoffAttempts = 0;
    consecutiveTimeouts = 0;
    frameCount = 0;
    currentInterval = BASE_INTERVAL_MS;
    votolData.recoveryCount++;
    
    Serial.println("[RECOVERY] Done");
}

// Output JSON
void outputJSON() {
    String json = "{";
    json += "\"voltage\":" + String(votolData.voltage, 1) + ",";
    json += "\"current\":" + String(votolData.current, 1) + ",";
    json += "\"rpm\":" + String(votolData.rpm) + ",";
    json += "\"temps\":{";
    json += "\"controller\":" + String(votolData.controllerTemp) + ",";
    json += "\"motor\":" + String(votolData.motorTemp);
    json += "},";
    json += "\"status\":{";
    json += "\"gear\":" + String(votolData.gear) + ",";
    json += "\"reverse\":" + String(votolData.reverse) + ",";
    json += "\"parking\":" + String(votolData.parking) + ",";
    json += "\"brake\":" + String(votolData.brake) + ",";
    json += "\"antitheft\":" + String(votolData.antitheft) + ",";
    json += "\"sidestand\":" + String(votolData.sidestand) + ",";
    json += "\"regen\":" + String(votolData.regen);
    json += "},";
    json += "\"stats\":{";
    json += "\"requests\":" + String(votolData.totalRequests) + ",";
    json += "\"success\":" + String(votolData.successCount) + ",";
    json += "\"timeouts\":" + String(votolData.timeoutCount) + ",";
    json += "\"cooldowns\":" + String(votolData.cooldownCount) + ",";
    json += "\"recoveries\":" + String(votolData.recoveryCount) + ",";
    json += "\"successRate\":" + String(votolData.successRate, 1) + ",";
    json += "\"interval\":" + String(currentInterval);
    json += "}";
    json += "}\n";
    
    Serial.print("[JSON] ");
    Serial.println(json);
}

// =============================================
// SETUP
// =============================================

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n========================================");
    Serial.println("VOTOL FRAME REQUEST V2 - IMPROVED");
    Serial.println("========================================");
    Serial.println("Anti-hang protections:");
    Serial.println("- Cooldown every 10 minutes (30s)");
    Serial.println("- Request budget: 100/minute");
    Serial.println("- Auto recovery on issues");
    Serial.println("========================================\n");
    
    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // CAN Bus
    Serial.println("[INIT] CAN Bus...");
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL
    );
    g_config.rx_queue_len = 20; // ⚠️ INCREASE QUEUE SIZE! Prevents overflow under load
    
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        if (twai_start() == ESP_OK) {
            Serial.println("✓ CAN Bus: 250kbps - READY\n");
        }
    }
    
    lastRequestTime = millis();
    lastCooldownTime = millis();
    lastRecoveryTime = millis();
    minuteStartTime = millis();
}

// =============================================
// STATE MACHINE
// =============================================

void updateStateMachine() {
    unsigned long now = millis();
    
    // 1. HYBRID PASSIVE-ACTIVE RECEIVE
    // Drain buffer & check for BOTH response AND broadcast data
    twai_message_t msg;
    while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK) {
        
        bool usefulData = false;
        
        // A. Check for RESPONSE to our request (0x3FE)
        if (msg.identifier == VOTOL_RESPONSE_ID) {
            if (currentState == VS_WAIT_RESPONSE && frameCount < 3) {
                // Filter echo
                bool isEcho = (memcmp(msg.data, REQ1, 8) == 0) || (memcmp(msg.data, REQ2, 8) == 0);
                if (!isEcho) {
                    memcpy(&responseBuffer[frameCount * 8], msg.data, 8);
                    frameCount++;
                    usefulData = true;
                }
            }
        }
        
        // B. Check for BROADCAST data (Traffic saat di-gas!)
        // Jika terima ini, berarti Votol sedang aktif bicara. JANGAN REQUEST!
        else if ((msg.identifier >> 24) == 0x0A || (msg.identifier >> 24) == 0x0E) {
            // Kita anggap ini "Response" juga karena membuktikan koneksi hidup
            usefulData = true; 
            
            // Bisa parsing data broadcast disini jika mau, tapi
            // tujuan utama sekarang adalah MENCEGAH REQUEST saat ini terjadi.
        }
        
        // C. SUPPRESS REQUEST jika ada traffic valid
        if (usefulData) {
            // Mundurkan jadwal request berikutnya!
            // "Votol lagi sibuk ngomong, gausah ditanya dulu."
            lastRequestTime = now; 
            
            // Jika kita sedang menunggu response request, tapi malah dapat broadcast,
            // itu tanda bus sangat sibuk. Reset state ke IDLE biar gak timeout.
            if (currentState == VS_WAIT_RESPONSE) {
                currentState = VS_IDLE;
                frameCount = 0;
            }
        }
    }
    
    // Reset request budget every minute
    if (now - minuteStartTime >= 60000) {
        requestsThisMinute = 0;
        minuteStartTime = now;
    }
    
    // State machine
    switch (currentState) {
        case VS_IDLE:
            if (now - lastRequestTime >= currentInterval) {
                currentState = VS_CHECK_BUDGET;
                stateStartTime = now;
            }
            break;
            
        case VS_CHECK_BUDGET:
            // Check cooldown schedule
            if (now - lastCooldownTime >= COOLDOWN_INTERVAL_MS) {
                Serial.println("\n🛑 [COOLDOWN] Scheduled cooldown period");
                currentState = VS_COOLDOWN;
                stateStartTime = now;
                votolData.cooldownCount++;
                lastCooldownTime = now;
                break;
            }
            
            // Check request budget
            if (requestsThisMinute >= MAX_REQUESTS_PER_MIN) {
                Serial.printf("⚠️ [BUDGET] Limit reached (%d/%d), waiting...\n",
                    requestsThisMinute, MAX_REQUESTS_PER_MIN);
                lastRequestTime = now + 1000;  // Wait 1s
                currentState = VS_IDLE;
                break;
            }
            
            // Check if recovery needed
            if (consecutiveTimeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
                currentState = VS_RECOVERY;
                break;
            }
            
            currentState = VS_SAMPLE_BUS;
            break;
            
        case VS_SAMPLE_BUS:
            // Friend's logic: simple sampling
            // We just check if we received anything in the last few ms
            // Since we are draining buffer above, we can assume bus is "clear"
            // if we haven't seen messages recently.
            // Simplified: Just delay a tiny bit to random offset
            delayMicroseconds(random(100, 500)); 
            currentState = VS_SEND_REQUEST;
            break;
            
        case VS_SEND_REQUEST:
            digitalWrite(LED_PIN, HIGH);
            lastLEDBlink = now;
            ledState = true;
            
            if (sendRequestPair()) {
                frameCount = 0;  // Reset for new response
                stateStartTime = now;
                currentState = VS_WAIT_RESPONSE;
            } else {
                currentState = VS_BACKOFF;
            }
            break;
            
        case VS_WAIT_RESPONSE:
            if (frameCount >= 3) {
                // Got all 3 frames!
                votolData.lastResponseTime = now - stateStartTime;
                currentState = VS_PARSE_DATA;
            } else if (now - stateStartTime > RESPONSE_TIMEOUT_MS) {
                // Timeout
                Serial.printf("⏱️ [TIMEOUT] Only %d/3 frames\n", frameCount);
                votolData.timeoutCount++;
                consecutiveTimeouts++;
                frameCount = 0;
                currentState = VS_BACKOFF;
            }
            break;
            
        case VS_PARSE_DATA:
            parseResponseData();
            outputJSON();
            
            // Success - reset to base interval
            currentInterval = BASE_INTERVAL_MS;
            backoffAttempts = 0;
            lastRequestTime = now;
            currentState = VS_IDLE;
            break;
            
        case VS_BACKOFF:
            backoffAttempts++;
            
            if (backoffAttempts >= MAX_BACKOFF_ATTEMPTS) {
                currentState = VS_RECOVERY;
            } else {
                // Exponential backoff with jitter
                unsigned long backoff = currentInterval * (1UL << (backoffAttempts - 1));
                backoff = constrain(backoff, MIN_INTERVAL_MS, MAX_INTERVAL_MS);
                backoff += random(0, 100);
                
                Serial.printf("⏮️ [BACKOFF] %lums (attempt %d/%d)\n",
                    backoff, backoffAttempts, MAX_BACKOFF_ATTEMPTS);
                
                currentInterval = min(currentInterval + 100, MAX_INTERVAL_MS);
                lastRequestTime = now + backoff;
                currentState = VS_IDLE;
            }
            break;
            
        case VS_COOLDOWN:
            if (now - stateStartTime >= COOLDOWN_PERIOD_MS) {
                Serial.println("✅ [COOLDOWN] Complete, resuming...\n");
                lastRequestTime = now;
                currentState = VS_IDLE;
            } else {
                // Still in cooldown
                static unsigned long lastPrint = 0;
                if (now - lastPrint > 5000) {
                    unsigned long remaining = COOLDOWN_PERIOD_MS - (now - stateStartTime);
                    Serial.printf("💤 [COOLDOWN] %lus remaining\n", remaining / 1000);
                    lastPrint = now;
                }
            }
            break;
            
        case VS_RECOVERY:
            performRecovery();
            lastRequestTime = now + 2000;  // Wait 2s after recovery
            lastRecoveryTime = now;
            currentState = VS_IDLE;
            break;
    }
    
    // LED off after 100ms
    if (ledState && (now - lastLEDBlink > 100)) {
        digitalWrite(LED_PIN, LOW);
        ledState = false;
    }
}

// =============================================
// MAIN LOOP
// =============================================

void loop() {
    updateStateMachine();
    
    // Heartbeat every 30s
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 30000) {
        Serial.printf("\n💓 Uptime: %lus | State: %d | Interval: %lums\n",
            millis() / 1000, currentState, currentInterval);
        lastHeartbeat = millis();
    }
    
    delay(1);  // Small yield
}
