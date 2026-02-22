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
#include <Update.h>
#include "driver/twai.h"

// === CONFIGURATION ===
#define FIRMWARE_VERSION "1.0.0"

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

// OTA State
volatile bool otaInProgress = false;
volatile size_t otaProgress = 0;
volatile size_t otaTotal = 0;

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
<button onclick="location.href='/ota'">📦 OTA Update</button>
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
// OTA UPDATE PAGE
// =============================================
const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OTA Update - VOTOL Sniffer</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0a;color:#0f0;font-family:Consolas,Monaco,monospace;padding:20px;min-height:100vh}
.container{max-width:500px;margin:0 auto}
h1{color:#0ff;font-size:1.5em;margin-bottom:20px}
.info{background:#111;border:1px solid #333;padding:15px;margin-bottom:20px;border-radius:4px}
.info p{margin:5px 0;font-size:13px}
.info .version{color:#ff0;font-size:16px}
.upload-area{background:#111;border:2px dashed #0f0;padding:40px;text-align:center;margin-bottom:20px;border-radius:4px;cursor:pointer;transition:all 0.3s}
.upload-area:hover{background:#1a1a1a;border-color:#0ff}
.upload-area.dragover{background:#002200;border-color:#0f0}
.upload-area input{display:none}
.upload-area .icon{font-size:48px;margin-bottom:10px}
.upload-area p{color:#888}
.file-info{color:#ff0;margin-top:10px;display:none}
.progress{background:#222;border:1px solid #333;height:30px;margin-bottom:20px;display:none;border-radius:4px;overflow:hidden}
.progress-bar{background:linear-gradient(90deg,#0a0,#0f0);height:100%;width:0%;transition:width 0.3s;display:flex;align-items:center;justify-content:center;color:#000;font-weight:bold}
.btn{background:#333;color:#0f0;border:1px solid #0f0;padding:12px 24px;cursor:pointer;font-size:14px;width:100%;margin-bottom:10px;border-radius:4px;transition:all 0.2s}
.btn:hover{background:#0f0;color:#000}
.btn:disabled{opacity:0.5;cursor:not-allowed}
.btn-back{background:transparent;border-color:#888;color:#888}
.btn-back:hover{border-color:#0f0;color:#0f0;background:transparent}
.status{padding:15px;margin-bottom:20px;border-radius:4px;display:none}
.status.error{background:#300;border:1px solid #f00;color:#f88}
.status.success{background:#030;border:1px solid #0f0;color:#0f0}
.status.info{background:#330;border:1px solid #ff0;color:#ff0}
</style>
</head>
<body>
<div class="container">
<h1>📦 OTA Firmware Update</h1>

<div class="info">
<p class="version">Current Version: )==VERSION==</p>
<p>Free Heap: <span id="heap">-</span> bytes</p>
<p>Flash Size: <span id="flash">-</span> bytes</p>
</div>

<div class="status" id="status"></div>

<div class="upload-area" id="dropZone">
<div class="icon">📁</div>
<p>Click or drag .bin file here</p>
<input type="file" id="fileInput" accept=".bin">
<div class="file-info" id="fileInfo"></div>
</div>

<div class="progress" id="progress">
<div class="progress-bar" id="progressBar">0%</div>
</div>

<button class="btn" id="uploadBtn" onclick="startUpload()" disabled>🚀 Start Update</button>
<button class="btn btn-back" onclick="location.href='/'">← Back to Sniffer</button>
</div>

<script>
var selectedFile = null;

// Get system info
fetch('/ota/info').then(r=>r.json()).then(d=>{
  document.getElementById('heap').textContent = d.heap.toLocaleString();
  document.getElementById('flash').textContent = d.flash.toLocaleString();
});

// Drag and drop
var dropZone = document.getElementById('dropZone');
var fileInput = document.getElementById('fileInput');

dropZone.onclick = () => fileInput.click();

dropZone.ondragover = (e) => { e.preventDefault(); dropZone.classList.add('dragover'); };
dropZone.ondragleave = () => dropZone.classList.remove('dragover');
dropZone.ondrop = (e) => {
  e.preventDefault();
  dropZone.classList.remove('dragover');
  if(e.dataTransfer.files.length) handleFile(e.dataTransfer.files[0]);
};

fileInput.onchange = () => { if(fileInput.files.length) handleFile(fileInput.files[0]); };

function handleFile(file) {
  if(!file.name.endsWith('.bin')) {
    showStatus('Please select a .bin file', 'error');
    return;
  }
  selectedFile = file;
  document.getElementById('fileInfo').style.display = 'block';
  document.getElementById('fileInfo').textContent = file.name + ' (' + (file.size/1024).toFixed(1) + ' KB)';
  document.getElementById('uploadBtn').disabled = false;
  showStatus('File ready. Click "Start Update" to begin.', 'info');
}

function showStatus(msg, type) {
  var s = document.getElementById('status');
  s.textContent = msg;
  s.className = 'status ' + type;
  s.style.display = 'block';
}

function startUpload() {
  if(!selectedFile) return;
  
  document.getElementById('uploadBtn').disabled = true;
  document.getElementById('progress').style.display = 'block';
  showStatus('Uploading firmware... Do not disconnect!', 'info');
  
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/ota/upload', true);
  
  xhr.upload.onprogress = (e) => {
    if(e.lengthComputable) {
      var pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('progressBar').style.width = pct + '%';
      document.getElementById('progressBar').textContent = pct + '%';
    }
  };
  
  xhr.onload = () => {
    if(xhr.status === 200) {
      var r = JSON.parse(xhr.responseText);
      if(r.success) {
        document.getElementById('progressBar').style.width = '100%';
        document.getElementById('progressBar').textContent = '100%';
        showStatus('Update successful! Rebooting in 3 seconds...', 'success');
        setTimeout(() => { location.href = '/'; }, 5000);
      } else {
        showStatus('Update failed: ' + r.error, 'error');
        document.getElementById('uploadBtn').disabled = false;
      }
    } else {
      showStatus('Upload failed: HTTP ' + xhr.status, 'error');
      document.getElementById('uploadBtn').disabled = false;
    }
  };
  
  xhr.onerror = () => {
    showStatus('Connection error', 'error');
    document.getElementById('uploadBtn').disabled = false;
  };
  
  var formData = new FormData();
  formData.append('firmware', selectedFile);
  xhr.send(formData);
}
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
  // Skip during OTA to prioritize update
  if (otaInProgress) {
    server.send(503, "application/json", "{\"error\":\"OTA in progress\"}");
    return;
  }
  
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
// OTA HANDLERS
// =============================================
void handleOtaPage() {
  String html = OTA_PAGE;
  html.replace(")==VERSION==", FIRMWARE_VERSION);
  server.send(200, "text/html", html);
}

void handleOtaInfo() {
  String json = "{\"heap\":";
  json += String(ESP.getFreeHeap());
  json += ",\"flash\":";
  json += String(ESP.getFlashChipSize());
  json += ",\"version\":\"";
  json += FIRMWARE_VERSION;
  json += "\"}";
  server.send(200, "application/json", json);
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Begin: %s (%u bytes free)\n", upload.filename.c_str(), ESP.getFreeHeap());
    
    otaInProgress = true;
    otaProgress = 0;
    otaTotal = 0;
    
    // Start update - use max available size
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.printf("[OTA] Begin failed: %s\n", Update.errorString());
      otaInProgress = false;
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning()) {
      size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        Serial.printf("[OTA] Write error: %s\n", Update.errorString());
      }
      otaProgress += written;
      
      // Progress feedback via LED
      digitalWrite(LED_PIN, (millis() / 100) % 2);
    }
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Success! Total: %u bytes\n", upload.totalSize);
    } else {
      Serial.printf("[OTA] End failed: %s\n", Update.errorString());
    }
    otaInProgress = false;
    digitalWrite(LED_PIN, LOW);
  } 
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    Serial.println("[OTA] Aborted");
    digitalWrite(LED_PIN, LOW);
  }
}

void handleOtaResult() {
  if (Update.hasError()) {
    String error = Update.errorString();
    String json = "{\"success\":false,\"error\":\"" + error + "\"}";
    server.send(200, "application/json", json);
  } else {
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    Serial.println("[OTA] Rebooting...");
    ESP.restart();
  }
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
  Serial.printf("Firmware Version: %s\n", FIRMWARE_VERSION);
  
  // Start WiFi AP
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("WiFi AP: ");
  Serial.println(ap_ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Setup Web Server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/ota", handleOtaPage);
  server.on("/ota/info", handleOtaInfo);
  server.on("/ota/upload", HTTP_POST, handleOtaResult, handleOtaUpload);
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
  Serial.println("OTA Update:  http://192.168.4.1/ota");
  Serial.println("================================\n");
}

// =============================================
// LOOP - Handle Web Requests
// =============================================
void loop() {
  server.handleClient();
  delay(1);
}
