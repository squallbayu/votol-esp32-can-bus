/*
 * =============================================
 * OTA VIA HTTP - WiFi Mode Support
 * =============================================
 * Endpoint: POST http://192.168.4.1/update
 * Body: Raw binary firmware (.bin file)
 * =============================================
 */
#ifndef OTA_HTTP_H
#define OTA_HTTP_H

#include <ESPAsyncWebServer.h>

void setupOtaHttp(AsyncWebServer* server);
bool isOtaHttpInProgress();

#endif // OTA_HTTP_H
