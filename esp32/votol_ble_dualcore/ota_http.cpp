/*
 * =============================================
 * OTA VIA HTTP - Implementation
 * =============================================
 * Usage from Flutter/Dart:
 * 
 * final response = await http.post(
 *   Uri.parse('http://192.168.4.1/update'),
 *   headers: {'Content-Type': 'application/octet-stream'},
 *   body: await firmwareFile.readAsBytes(),
 * );
 * =============================================
 */
#include "ota_http.h"
#include <Update.h>
#include <WiFi.h>

static bool otaHttpInProgress = false;
static size_t otaTotalSize = 0;
static size_t otaReceived = 0;

// OTA Progress callback for status endpoint
static void handleOtaStatus(AsyncWebServerRequest *request) {
  String status = "{";
  status += "\"in_progress\":" + String(otaHttpInProgress ? "true" : "false") + ",";
  status += "\"received\":" + String(otaReceived) + ",";
  status += "\"total\":" + String(otaTotalSize) + ",";
  status += "\"progress_percent\":" + String(otaTotalSize > 0 ? (otaReceived * 100 / otaTotalSize) : 0);
  status += "}";
  request->send(200, "application/json", status);
}

void setupOtaHttp(AsyncWebServer* server) {
  // OTA Update endpoint - POST /update
  // Using raw body handler for better memory efficiency
  server->on("/update", HTTP_POST, 
    [](AsyncWebServerRequest *request) {
      // Final response handler - called after all data received
      if (!otaHttpInProgress) {
        // Error: OTA not started properly
        request->send(500, "text/plain", "OTA not started");
        return;
      }
      
      // Check if update was successful
      if (Update.end(true)) {
        // Success
        Serial.println("[OTA-HTTP] Update success! Rebooting...");
        request->send(200, "text/plain", "OK\n");
        delay(500);
        ESP.restart();
      } else {
        // Failed
        otaHttpInProgress = false;
        Serial.printf("[OTA-HTTP] Update failed: %s\n", Update.errorString());
        request->send(500, "text/plain", String("ERROR: ") + Update.errorString());
      }
    },
    NULL,  // Upload handler not used with raw body
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      // Raw body handler - called for each chunk of data
      
      if (index == 0) {
        // Upload start
        Serial.printf("[OTA-HTTP] Start OTA upload, total size: %d bytes\n", total);
        
        otaTotalSize = total;
        otaHttpInProgress = true;
        otaReceived = 0;
        
        // Begin OTA update
        if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
          Serial.println("[OTA-HTTP] Update.begin() failed!");
          otaHttpInProgress = false;
          return;
        }
      }
      
      // Write data
      if (len > 0 && otaHttpInProgress) {
        size_t written = Update.write(data, len);
        if (written != len) {
          Serial.println("[OTA-HTTP] Write failed!");
          Update.abort();
          otaHttpInProgress = false;
          return;
        }
        otaReceived += len;
        
        // Progress every 50KB
        if (otaReceived % 51200 == 0 || index + len >= total) {
          int percent = (total > 0) ? (otaReceived * 100 / total) : 0;
          Serial.printf("[OTA-HTTP] Progress: %d/%d bytes (%d%%)\n", otaReceived, total, percent);
        }
      }
    }
  );
  
  // OTA Status endpoint - GET /ota_status
  server->on("/ota_status", HTTP_GET, handleOtaStatus);
  
  // Simple health check - GET /health
  server->on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    String health = "{";
    health += "\"status\":\"ok\",";
    health += "\"mode\":\"wifi\",";
    health += "\"ota_ready\":" + String(otaHttpInProgress ? "false" : "true");
    health += "}";
    request->send(200, "application/json", health);
  });
  
  Serial.println("[OTA-HTTP] HTTP OTA endpoints registered:");
  Serial.println("  POST /update       - Upload firmware .bin (Content-Type: application/octet-stream)");
  Serial.println("  GET  /ota_status   - Check OTA progress (JSON)");
  Serial.println("  GET  /health       - Health check (JSON)");
}

bool isOtaHttpInProgress() {
  return otaHttpInProgress;
}
