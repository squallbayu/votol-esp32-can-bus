/*
 * =============================================
 * WIFI WEBSOCKET HANDLER - Implementation
 * Compatible with ESP32 Arduino Core 3.x
 * =============================================
 */
#include "wifi_handler.h"
#include "ota_http.h"
#include <Preferences.h>
#include <BLEDevice.h>

// BLE globals from main firmware (may be null when booting in WiFi mode)
extern BLEServer* pServer;

// External declarations from main .ino
extern Preferences preferences;
extern SemaphoreHandle_t nvsMutex;
extern SemaphoreHandle_t dataMutex;
extern std::atomic<bool> isInjectorEnabled;
extern bool currentTwaiModeNormal;

// For inject status
extern std::atomic<int32_t> atomicAmpereRaw;
extern std::atomic<VehicleMode> atomicMode;
extern bool bmsChargingFlag;
extern bool chargerConnected;
extern bool oriChargerDetected;

// Helper: NVS write with mutex protection
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

// Global instances
AsyncWebServer wsServer(WS_PORT);
AsyncWebSocket ws(WS_URL);
bool wifiModeActive = false;
static uint32_t lastWiFiCheck = 0;
static bool wifiEventRegistered = false;
static bool wsHandlersRegistered = false;  // Fix #6: prevent handler stacking

// WiFi Event Handler
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[WiFi-Event] AP Started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("[WiFi-Event] AP Stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("[WiFi-Event] Station connected - MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
        info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
        info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[WiFi-Event] Station disconnected - MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
        info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
        info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("[WiFi-Event] Station got IP: %s\n", 
        IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
      break;
    default:
      break;
  }
}

// WebSocket event handler
static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WiFi] WebSocket client #%u connected from %s (total: %u)\n", 
                    (unsigned int)client->id(), client->remoteIP().toString().c_str(),
                    (unsigned int)server->count());
      // Disable Nagle's Algorithm: send packets immediately for real-time responsiveness
      // Without this, TCP buffers small packets (~200ms delay) causing dashboard jitter
      client->client()->setNoDelay(true);
      Serial.printf("[WiFi] TCP_NODELAY enabled for client #%u\n", (unsigned int)client->id());
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("[WiFi] WebSocket client #%u disconnected (remaining: %u)\n", 
                    (unsigned int)client->id(), 
                    server->count() > 0 ? (unsigned int)(server->count() - 1) : 0u);
      break;
      
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      
      // DEBUG: Log all incoming frames
      Serial.printf("[WiFi] WS_EVT_DATA received: len=%u, opcode=%d, final=%d, index=%u\n", 
                    (unsigned int)len, info->opcode, info->final, (unsigned int)info->index);
      
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        // Fix #1: Don't write past data buffer. Use length-bounded String constructor.
        String cmd((char*)data, len);
        Serial.printf("[WiFi] Command received: '%s'\n", cmd.c_str());
        
        if (cmd.startsWith("BLE:ON")) {
           Serial.println("[WiFi] Switch to BLE mode requested");
           transportMode.store(TRANSPORT_BLE, std::memory_order_release);
           // Mode saved by loop() auto-save
        }
        else if (cmd.startsWith("WIFI:ON")) {
           // No-op in WiFi mode, but acknowledge for UI consistency
           Serial.println("[WiFi] WIFI:ON received (already in WiFi mode)");
        }
        // Cmd: "TIMESYNC:YY,MM,DD,HH,MI,SS" -> Set BMS clock

      } else if (info->opcode == WS_BINARY) {
        Serial.printf("[WiFi] BINARY frame received (len=%u) - IGNORED\n", (unsigned int)len);
      }
      break;
    }
    
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void initWiFiMode() {
  if (wifiModeActive) {
    Serial.println("[WiFi] Already active!");
    return;
  }
  
  Serial.println("\n========================================");
  Serial.println("       Starting WiFi AP Mode");
  Serial.println("========================================\n");
  
  // === STEP 1: BLE already stopped by commTask (stopBLE + btStop) ===
  // No need to touch BLE here - commTask handles full teardown
  Serial.println("[WiFi] Step 1: BLE already stopped by commTask");
  
  // === STEP 2: Reset WiFi Completely ===
  Serial.println("[WiFi] Step 2: Resetting WiFi...");
  
  // Stop any existing AP
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(500); // Reduced from 1000ms - sufficient for WiFi reset
  Serial.println("       - WiFi reset complete");
  
  // === STEP 3: Register Event Handler ===
  if (!wifiEventRegistered) {
    WiFi.onEvent(onWiFiEvent);
    wifiEventRegistered = true;
  }
  
  // === STEP 4: Configure and Start AP ===
  Serial.println("[WiFi] Step 3: Configuring AP...");
  
  // Set mode to AP
  WiFi.mode(WIFI_AP);
  delay(300);  // Reduced from 500ms
  
  // Disable power saving
  WiFi.setSleep(false);
  
  // Configure static IP (IMPORTANT: Must be before softAP!)
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  
  if (!WiFi.softAPConfig(localIP, gateway, subnet)) {
    Serial.println("[WiFi] ERROR: softAPConfig failed!");
    return;
  }
  Serial.println("       - AP IP Config: 192.168.4.1/24");
  
  // Start AP
  Serial.println("[WiFi] Step 4: Starting AP...");
  bool result = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_MAX_CLIENTS);
  if (!result) {
    Serial.println("[WiFi] ERROR: softAP failed!");
    return;
  }
  
  // Wait for AP ready
  delay(800);  // Reduced from 1500ms - usually ready faster
  
  // Verify IP
  IPAddress actualIP = WiFi.softAPIP();
  if (actualIP == IPAddress(0, 0, 0, 0)) {
    Serial.println("[WiFi] ERROR: No IP assigned!");
    return;
  }
  
  Serial.println("       - AP Started successfully");
  Serial.printf("       - AP IP: %s\n", actualIP.toString().c_str());
  
  // === STEP 5: Start WebSocket Server ===
  Serial.println("[WiFi] Step 5: Starting WebSocket server...");
  
  // Fix #6: Only register handlers once to prevent handler stacking
  if (!wsHandlersRegistered) {
    ws.onEvent(onWsEvent);
    wsServer.addHandler(&ws);
    
    // Setup OTA HTTP endpoints
    setupOtaHttp(&wsServer);
    
    // === HTTP API: Switch to BLE ===
    // More reliable than WebSocket for mode switching
    wsServer.on("/SWITCH_BLE", HTTP_GET, [](AsyncWebServerRequest *request){
      Serial.println("[HTTP] /SWITCH_BLE requested");
      request->send(200, "text/plain", "OK. Switching to BLE...");
      
      transportMode.store(TRANSPORT_BLE, std::memory_order_release);
      nvsWriteString("mode", "BLE");
    });
    
    // === HTTP API: Inject Enable/Disable ===
    // Simple HTTP toggle (alternative to WebSocket INJECT:1/0 and BLE)
    wsServer.on("/inject_on", HTTP_GET, [](AsyncWebServerRequest *request){
      isInjectorEnabled.store(true, std::memory_order_release);
      nvsWriteBool("inj", true);
      Serial.println("[HTTP] Injector: ON");
      request->send(200, "text/plain", "Injector ENABLED");
    });
    wsServer.on("/inject_off", HTTP_GET, [](AsyncWebServerRequest *request){
      isInjectorEnabled.store(false, std::memory_order_release);
      nvsWriteBool("inj", false);
      Serial.println("[HTTP] Injector: OFF");
      request->send(200, "text/plain", "Injector DISABLED");
    });
    
    // === HTTP API: Inject Status ===
    wsServer.on("/inject_status", HTTP_GET, [](AsyncWebServerRequest *request){
      bool injEnabled = isInjectorEnabled.load(std::memory_order_acquire);
      float amp = atomicAmpereRaw.load(std::memory_order_acquire) / 10.0f;
      
      bool localBms = false;
      bool localChrConn = false;
      bool localOriDet = false;
      
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        localBms = bmsChargingFlag;
        localChrConn = chargerConnected;
        localOriDet = oriChargerDetected;
        xSemaphoreGive(dataMutex);
      }
      
      VehicleMode mode = atomicMode.load(std::memory_order_acquire);
      bool modeOK = (mode == MODE_PARK || mode == MODE_CHARGING || mode == MODE_STAND);
      
      char buf[320];
      snprintf(buf, sizeof(buf),
        "{"
        "\"injectorEnabled\":%s,"
        "\"bmsCharging\":%s,"
        "\"chargerConnected\":%s,"
        "\"oriCharger\":%s,"
        "\"ampere\":%.1f,"
        "\"vehicleMode\":\"%s\","
        "\"modeAllowsInject\":%s,"
        "\"twaiMode\":\"%s\""
        "}",
        injEnabled ? "true" : "false",
        localBms ? "true" : "false",
        localChrConn ? "true" : "false",
        localOriDet ? "true" : "false",
        amp,
        getModeString(mode),
        modeOK ? "true" : "false",
        currentTwaiModeNormal ? "NORMAL" : "LISTEN_ONLY"
      );
      
      request->send(200, "application/json", buf);
    });
    
    wsHandlersRegistered = true;
    Serial.println("       - Handlers registered (first time)");
  } else {
    Serial.println("       - Handlers already registered, skipping");
  }
  
  wsServer.begin();
  
  wifiModeActive = true;
  lastWiFiCheck = millis();
  
  Serial.println("\n========================================");
  Serial.printf("  SSID:     %s\n", WIFI_AP_SSID);
  Serial.printf("  Password: %s\n", WIFI_AP_PASSWORD);
  Serial.printf("  AP IP:    %s\n", actualIP.toString().c_str());
  Serial.printf("  WebSocket: ws://%s:%d%s\n", actualIP.toString().c_str(), WS_PORT, WS_URL);
  Serial.println("\n  Status: READY - Connect your phone!");
  Serial.println("========================================\n");
  
  // Debug: Show connected stations periodically
  Serial.println("[WiFi] Waiting for stations to connect...");
  Serial.println("[WiFi] Tip: If phone gets stuck 'Obtaining IP', try:");
  Serial.println("      1. Forget network on phone and reconnect");
  Serial.println("      2. Restart ESP32");
  Serial.println("      3. Use static IP on phone (192.168.4.x)");
}

void stopWiFiMode() {
  if (!wifiModeActive) return;
  
  Serial.println("[WiFi] Stopping WiFi mode...");
  
  ws.closeAll();
  delay(100);
  
  wsServer.end();
  delay(100);
  
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
  
  wifiModeActive = false;
  
  Serial.println("[WiFi] WiFi stopped");
}

void handleWiFiLoop() {
  if (!wifiModeActive) return;
  
  uint32_t now = millis();
  if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWiFiCheck = now;
    ws.cleanupClients();
  }
}

void wsBroadcast(const char* data) {
  // Fix #4: Use ws.count() instead of single bool to support multiple clients
  if (!wifiModeActive || ws.count() == 0) return;
  ws.textAll(data);
}

void wsBroadcast(uint8_t* data, size_t len) {
  if (!wifiModeActive || ws.count() == 0) return;
  ws.binaryAll(data, len);
}

bool isWiFiModeActive() {
  return wifiModeActive;
}

bool isWsClientConnected() {
  return ws.count() > 0;
}
