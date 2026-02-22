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

// === Vehicle Mode Enum (shared between .ino and .cpp) ===
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

inline const char* getModeString(VehicleMode m) {
  static const char* const modeStrings[] = {
    "PARK", "STAND", "CHARGING", "DRIVE", "SPORT", "REVERSE", "BRAKE"
  };
  uint8_t mVal = static_cast<uint8_t>(m);
  if (mVal >= 7) return "UNKNOWN";
  return modeStrings[mVal];
}
#endif

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
