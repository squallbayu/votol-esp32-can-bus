/*
 * =============================================
 * WIFI WEBSOCKET HANDLER - Header
 * Compatible with ESP32 Arduino Core 3.x
 * =============================================
 */
#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include <atomic>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// === WiFi AP Configuration ===
#define WIFI_AP_SSID        "VOTOL_Wifi"
#define WIFI_AP_PASSWORD    "votol12345"      // Min 8 chars
#define WIFI_AP_CHANNEL     6
#define WIFI_MAX_CLIENTS    4

// === WebSocket Configuration ===
#define WS_PORT             81
#define WS_URL              "/ws"

// === Timing ===
#define WIFI_CHECK_INTERVAL_MS  500

// === Global State ===
extern AsyncWebServer wsServer;
extern AsyncWebSocket ws;
extern bool wifiModeActive;

// === Transport Mode Enum ===
#ifndef TRANSPORT_MODE_ENUM_DEFINED
#define TRANSPORT_MODE_ENUM_DEFINED
enum TransportMode : uint8_t {
  TRANSPORT_BLE = 0,
  TRANSPORT_WIFI,
  TRANSPORT_SWITCHING
};
#endif

// === External State (defined in main .ino) ===
extern std::atomic<TransportMode> transportMode;

// === Function Declarations ===
void initWiFiMode();
void stopWiFiMode();
void handleWiFiLoop();
void wsBroadcast(const char* data);
void wsBroadcast(uint8_t* data, size_t len);
bool isWiFiModeActive();
bool isWsClientConnected();

#endif // WIFI_HANDLER_H
