/*
 * =============================================
 * VOTOL CAN SNIFFER - EMBEDDED WEB SERVER
 * =============================================
 * 
 * Author: Zekri R (ZEKRI.ID)
 * Purpose: CAN Bus Sniffer with built-in Web UI
 * No Python required - just connect to WiFi and open browser!
 * 
 * Usage:
 * 1. Upload to ESP32
 * 2. Connect to WiFi "VOTOL_SNIFFER" (password: 12345678)
 * 3. Open http://192.168.4.1 in browser
 * 
 * =============================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "driver/twai.h"

// === CONFIGURATION ===
const char* ap_ssid = "VOTOL_SNIFFER";
const char* ap_pass = "12345678";

#define CAN_TX_PIN GPIO_NUM_21
#define CAN_RX_PIN GPIO_NUM_22
#define LED_PIN 2

// CAN ID Names
struct CANIDInfo {
  uint32_t id;
  const char* name;
};

const CANIDInfo CAN_IDS[] = {
  {0x0A010810, "DRIVE/TEMPS"},
  {0x0A6D0D09, "VOLT/AMPS"},
  {0x0E6C0D09, "BATT TEMP"},
  {0x0A6E0D09, "SOC/HEALTH"},
  {0x0A6F0D09, "CELL STATS"},
  {0x0A700D09, "TEMP STATS"},
  {0x0A730D09, "BALANCE"},
  {0x1810D0F3, "CHARGER 1"},
  {0x1811D0F3, "CHARGER 2"},
  {0x0E640D09, "CELLS 1-4"},
  {0x0E650D09, "CELLS 5-8"},
  {0x0E660D09, "CELLS 9-12"},
  {0x0E670D09, "CELLS 13-16"},
  {0x0E680D09, "CELLS 17-20"},
  {0x0E690D09, "CELLS 21-23"},
};
const int CAN_IDS_COUNT = sizeof(CAN_IDS) / sizeof(CAN_IDS[0]);

// Web Server
WebServer server(80);

// Message buffer
#define MAX_MESSAGES 100
struct CANMessage {
  uint32_t timestamp;
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
};

volatile CANMessage msgBuffer[MAX_MESSAGES];
volatile int msgWriteIndex = 0;
volatile int msgReadIndex = 0;
volatile uint32_t totalMessages = 0;
volatile uint32_t messagesPerSec = 0;
volatile uint32_t msgCountThisSec = 0;
uint32_t lastSecond = 0;

// Get CAN ID name
const char* getCANIDName(uint32_t id) {
  for (int i = 0; i < CAN_IDS_COUNT; i++) {
    if (CAN_IDS[i].id == id) return CAN_IDS[i].name;
  }
  return "UNKNOWN";
}

// =============================================
// HTML PAGE (Embedded)
// =============================================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VOTOL CAN Sniffer</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0a;color:#0f0;font-family:Consolas,Monaco,monospace;padding:10px}
h1{color:#0ff;font-size:1.5em;margin-bottom:8px}
.stats{color:#ff0;margin-bottom:10px;font-size:13px}
.ctrl{margin-bottom:10px}
button{background:#333;color:#0f0;border:1px solid #0f0;padding:6px 12px;margin-right:8px;cursor:pointer}
button:hover{background:#0f0;color:#000}
button.on{background:#0f0;color:#000}
input{background:#111;color:#0f0;border:1px solid #0f0;padding:6px;width:150px}
#log{background:#111;border:1px solid #333;height:calc(100vh - 140px);overflow-y:auto;padding:8px;font-size:12px;line-height:1.4}
.m{white-space:pre}
.DRIVE{color:#0ff}.VOLT{color:#ff0}.SOC{color:#0f0}.CHARGER{color:#f0f}
.CELLS{color:#fa0}.BATT{color:#f80}.BALANCE{color:#8ff}.TEMP{color:#f88}.UNKNOWN{color:#888}
</style>
</head>
<body>
<h1>🔧 VOTOL CAN Sniffer</h1>
<div class="stats">Msg: <span id="t">0</span> | Rate: <span id="r">0</span>/s | <span id="s" style="color:#f00">Connecting...</span></div>
<div class="ctrl">
<button id="bp" onclick="togglePause()">⏸ Pause</button>
<button onclick="document.getElementById('log').innerHTML='';tc=0;document.getElementById('t').textContent='0'">🗑 Clear</button>
<button id="bs" class="on" onclick="toggleScroll()">⬇ Scroll</button>
<input id="f" placeholder="Filter (e.g. VOLT)">
</div>
<div id="log"></div>
<script>
var paused=false,scroll=true,tc=0,last=0;
function togglePause(){paused=!paused;document.getElementById('bp').textContent=paused?'▶ Resume':'⏸ Pause';document.getElementById('bp').classList.toggle('on',paused)}
function toggleScroll(){scroll=!scroll;document.getElementById('bs').classList.toggle('on',scroll)}
function getClass(t){if(t.includes('DRIVE'))return'DRIVE';if(t.includes('VOLT'))return'VOLT';if(t.includes('SOC'))return'SOC';if(t.includes('CHARGER'))return'CHARGER';if(t.includes('CELL'))return'CELLS';if(t.includes('BATT'))return'BATT';if(t.includes('BALANCE'))return'BALANCE';if(t.includes('TEMP'))return'TEMP';return'UNKNOWN'}
function poll(){
  if(paused){setTimeout(poll,200);return}
  fetch('/data?after='+last).then(r=>r.json()).then(d=>{
    document.getElementById('s').textContent='Connected';
    document.getElementById('s').style.color='#0f0';
    document.getElementById('r').textContent=d.rate;
    var log=document.getElementById('log'),fv=document.getElementById('f').value.toUpperCase();
    d.msgs.forEach(m=>{
      if(fv&&!m.t.toUpperCase().includes(fv))return;
      var div=document.createElement('div');
      div.className='m '+getClass(m.t);
      div.textContent=m.t;
      log.appendChild(div);
      tc++;last=m.i;
      if(log.children.length>500)log.removeChild(log.firstChild);
    });
    document.getElementById('t').textContent=tc;
    if(scroll&&d.msgs.length>0)log.scrollTop=log.scrollHeight;
    setTimeout(poll,150);
  }).catch(e=>{
    document.getElementById('s').textContent='Disconnected';
    document.getElementById('s').style.color='#f00';
    setTimeout(poll,1000);
  });
}
poll();
</script>
</body>
</html>
)rawliteral";

// =============================================
// WEB HANDLERS
// =============================================
void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}

void handleData() {
  int afterIndex = 0;
  if (server.hasArg("after")) {
    afterIndex = server.arg("after").toInt();
  }
  
  String json = "{\"rate\":";
  json += String(messagesPerSec);
  json += ",\"msgs\":[";
  
  int count = 0;
  int readIdx = msgReadIndex;
  
  // Find messages newer than afterIndex
  for (int i = 0; i < MAX_MESSAGES && count < 50; i++) {
    int idx = (readIdx + i) % MAX_MESSAGES;
    if (msgBuffer[idx].timestamp == 0) continue;
    if ((int)msgBuffer[idx].timestamp <= afterIndex) continue;
    
    if (count > 0) json += ",";
    
    // Format message
    char buf[120];
    char dataStr[32] = "";
    for (int j = 0; j < msgBuffer[idx].len && j < 8; j++) {
      char hex[4];
      sprintf(hex, "%02X ", msgBuffer[idx].data[j]);
      strcat(dataStr, hex);
    }
    
    sprintf(buf, "{\"i\":%lu,\"t\":\"[%8lums] [%-12s] 0x%08X | %s\"}", 
      msgBuffer[idx].timestamp,
      msgBuffer[idx].timestamp,
      getCANIDName(msgBuffer[idx].id),
      msgBuffer[idx].id,
      dataStr);
    
    json += buf;
    count++;
  }
  
  json += "]}";
  server.send(200, "application/json", json);
}

// =============================================
// CAN TASK
// =============================================
void canTask(void *pvParameters) {
  twai_message_t message;
  
  while (true) {
    if (twai_receive(&message, pdMS_TO_TICKS(10)) == ESP_OK) {
      // Store in buffer
      int idx = msgWriteIndex;
      msgBuffer[idx].timestamp = millis();
      msgBuffer[idx].id = message.identifier;
      msgBuffer[idx].len = message.data_length_code;
      memcpy((void*)msgBuffer[idx].data, message.data, 8);
      
      msgWriteIndex = (msgWriteIndex + 1) % MAX_MESSAGES;
      if (msgWriteIndex == msgReadIndex) {
        msgReadIndex = (msgReadIndex + 1) % MAX_MESSAGES;
      }
      
      totalMessages++;
      msgCountThisSec++;
      
      // LED feedback
      digitalWrite(LED_PIN, HIGH);
      delayMicroseconds(100);
      digitalWrite(LED_PIN, LOW);
    }
    
    // Rate calculation
    uint32_t nowSec = millis() / 1000;
    if (nowSec != lastSecond) {
      messagesPerSec = msgCountThisSec;
      msgCountThisSec = 0;
      lastSecond = nowSec;
    }
  }
}

// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  
  Serial.println("\n=== VOTOL CAN SNIFFER - WEB UI ===");
  
  // Start WiFi AP
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("WiFi AP: ");
  Serial.println(ap_ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Setup Web Server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("Web Server started on port 80");
  
  // Setup CAN
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
  g_config.rx_queue_len = 64;
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  
  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    twai_start();
    Serial.println("CAN Bus started (Listen Only)");
  } else {
    Serial.println("CAN Failed!");
  }
  
  // Start CAN task on Core 0
  xTaskCreatePinnedToCore(canTask, "CAN", 4096, NULL, 2, NULL, 0);
  
  Serial.println("\nOpen browser: http://192.168.4.1");
  Serial.println("================================\n");
}

// =============================================
// LOOP - Handle Web Requests
// =============================================
void loop() {
  server.handleClient();
  delay(1);
}
