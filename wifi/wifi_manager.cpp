#include "wifi_manager.h"

#if ENABLE_WIFI_AIRDROP

#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <cstring>
#include <cstdio>
#include <Arduino.h>
#ifdef ESP32
#include <esp_wifi.h>
#include <ESPmDNS.h>
#endif

static WebServer *g_web_server = nullptr;
static constexpr uint16_t WEB_SERVER_PORT = 80;
static constexpr const char *UPLOAD_DIR = "/";

// HTML page for drag-and-drop file upload
static const char *UPLOAD_HTML = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Cardputer Airdrop</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: white;
      border-radius: 20px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
      padding: 40px;
      max-width: 500px;
      width: 100%;
    }
    h1 {
      color: #333;
      margin-bottom: 10px;
      font-size: 28px;
    }
    .subtitle {
      color: #666;
      margin-bottom: 30px;
      font-size: 14px;
    }
    .drop-zone {
      border: 3px dashed #667eea;
      border-radius: 15px;
      padding: 60px 20px;
      text-align: center;
      background: #f8f9ff;
      transition: all 0.3s ease;
      cursor: pointer;
      margin-bottom: 20px;
    }
    .drop-zone:hover, .drop-zone.dragover {
      border-color: #764ba2;
      background: #f0f2ff;
      transform: scale(1.02);
    }
    .drop-icon {
      font-size: 64px;
      margin-bottom: 20px;
    }
    .drop-text {
      color: #667eea;
      font-size: 18px;
      font-weight: 600;
      margin-bottom: 10px;
    }
    .drop-hint {
      color: #999;
      font-size: 14px;
    }
    input[type="file"] {
      display: none;
    }
    .upload-btn {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border: none;
      padding: 15px 30px;
      border-radius: 10px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      width: 100%;
      transition: transform 0.2s;
    }
    .upload-btn:hover {
      transform: translateY(-2px);
      box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4);
    }
    .upload-btn:active {
      transform: translateY(0);
    }
    .progress {
      margin-top: 20px;
      display: none;
    }
    .progress-bar {
      width: 100%;
      height: 8px;
      background: #e0e0e0;
      border-radius: 4px;
      overflow: hidden;
    }
    .progress-fill {
      height: 100%;
      background: linear-gradient(90deg, #667eea, #764ba2);
      width: 0%;
      transition: width 0.3s;
    }
    .status {
      margin-top: 15px;
      padding: 12px;
      border-radius: 8px;
      display: none;
      font-size: 14px;
    }
    .status.success {
      background: #d4edda;
      color: #155724;
      display: block;
    }
    .status.error {
      background: #f8d7da;
      color: #721c24;
      display: block;
    }
    .file-list {
      margin-top: 20px;
      max-height: 200px;
      overflow-y: auto;
    }
    .file-item {
      padding: 8px;
      background: #f8f9fa;
      border-radius: 6px;
      margin-bottom: 5px;
      font-size: 13px;
      color: #555;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 Cardputer Airdrop</h1>
    <div class="subtitle">Drag and drop files to transfer to your Cardputer</div>
    
    <div class="drop-zone" id="dropZone">
      <div class="drop-icon">📤</div>
      <div class="drop-text">Drop files here</div>
      <div class="drop-hint">or click to browse</div>
    </div>
    
    <input type="file" id="fileInput" multiple>
    
    <div class="progress" id="progress">
      <div class="progress-bar">
        <div class="progress-fill" id="progressFill"></div>
      </div>
    </div>
    
    <div class="status" id="status"></div>
    
    <div class="file-list" id="fileList"></div>
  </div>

  <script>
    const dropZone = document.getElementById('dropZone');
    const fileInput = document.getElementById('fileInput');
    const progress = document.getElementById('progress');
    const progressFill = document.getElementById('progressFill');
    const status = document.getElementById('status');
    const fileList = document.getElementById('fileList');

    dropZone.addEventListener('click', () => fileInput.click());
    
    dropZone.addEventListener('dragover', (e) => {
      e.preventDefault();
      dropZone.classList.add('dragover');
    });
    
    dropZone.addEventListener('dragleave', () => {
      dropZone.classList.remove('dragover');
    });
    
    dropZone.addEventListener('drop', (e) => {
      e.preventDefault();
      dropZone.classList.remove('dragover');
      handleFiles(e.dataTransfer.files);
    });
    
    fileInput.addEventListener('change', (e) => {
      handleFiles(e.target.files);
    });

    function handleFiles(files) {
      if (files.length === 0) return;
      
      Array.from(files).forEach(file => uploadFile(file));
    }

    function uploadFile(file) {
      const formData = new FormData();
      formData.append('file', file);
      
      const xhr = new XMLHttpRequest();
      
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
          const percent = (e.loaded / e.total) * 100;
          progressFill.style.width = percent + '%';
          progress.style.display = 'block';
        }
      });
      
      xhr.addEventListener('load', () => {
        progress.style.display = 'none';
        progressFill.style.width = '0%';
        
        if (xhr.status === 200) {
          status.className = 'status success';
          status.textContent = '✓ ' + file.name + ' uploaded successfully!';
          
          // Add to file list
          const fileItem = document.createElement('div');
          fileItem.className = 'file-item';
          fileItem.textContent = '✓ ' + file.name;
          fileList.appendChild(fileItem);
        } else {
          status.className = 'status error';
          status.textContent = '✗ Upload failed: ' + xhr.responseText;
        }
        
        setTimeout(() => {
          status.style.display = 'none';
        }, 3000);
      });
      
      xhr.addEventListener('error', () => {
        progress.style.display = 'none';
        progressFill.style.width = '0%';
        status.className = 'status error';
        status.textContent = '✗ Network error during upload';
        setTimeout(() => {
          status.style.display = 'none';
        }, 3000);
      });
      
      xhr.open('POST', '/upload');
      xhr.send(formData);
    }
  </script>
</body>
</html>
)";

static void handleRoot() {
  if(g_web_server == nullptr) {
    Serial.println("Root request failed: server is null");
    return;
  }
  
  Serial.println("=== Root request received ===");
  Serial.printf("Client IP: %s\n", g_web_server->client().remoteIP().toString().c_str());
  Serial.printf("Method: %s\n", g_web_server->method() == HTTP_GET ? "GET" : "OTHER");
  Serial.printf("URI: %s\n", g_web_server->uri().c_str());
  Serial.printf("HTML size: %u bytes\n", static_cast<unsigned>(strlen(UPLOAD_HTML)));
  
  // Send response directly - ESP32 WebServer handles large responses
  g_web_server->send(200, "text/html", UPLOAD_HTML);
  
  Serial.println("Root response sent");
}

static void handleNotFound() {
  if(g_web_server != nullptr) {
    Serial.printf("404: %s\n", g_web_server->uri().c_str());
    g_web_server->send(404, "text/plain", "Not found");
  }
}

static File g_upload_file;

static void handleUpload() {
  if(g_web_server == nullptr) {
    return;
  }

  HTTPUpload& upload = g_web_server->upload();
  
  if(upload.status == UPLOAD_FILE_START) {
    String filename = String(UPLOAD_DIR) + upload.filename;
    Serial.printf("Upload start: %s\n", filename.c_str());
    
    // Open file for writing on SD card
    if(SD.exists(filename.c_str())) {
      SD.remove(filename.c_str());
    }
    
    g_upload_file = SD.open(filename.c_str(), FILE_WRITE);
    if(!g_upload_file) {
      Serial.printf("Failed to open file for writing: %s\n", filename.c_str());
    }
  } else if(upload.status == UPLOAD_FILE_WRITE) {
    if(g_upload_file) {
      size_t written = g_upload_file.write(upload.buf, upload.currentSize);
      if(written != upload.currentSize) {
        Serial.printf("Write mismatch: %u != %u\n", 
                      static_cast<unsigned>(written),
                      static_cast<unsigned>(upload.currentSize));
      }
    }
  } else if(upload.status == UPLOAD_FILE_END) {
    // File will be closed in handleUploadComplete
    Serial.printf("Upload ending: %s (%u bytes)\n",
                  upload.filename.c_str(),
                  static_cast<unsigned>(upload.totalSize));
  } else if(upload.status == UPLOAD_FILE_ABORTED) {
    if(g_upload_file) {
      g_upload_file.close();
      Serial.printf("Upload aborted: %s\n", upload.filename.c_str());
    }
    g_upload_file = File();
  }
}

static void handleUploadComplete() {
  if(g_web_server == nullptr) {
    return;
  }
  
  if(g_upload_file) {
    g_upload_file.close();
    g_web_server->send(200, "text/plain", "File uploaded successfully");
  } else {
    g_web_server->send(500, "text/plain", "Upload failed: Could not write to SD card");
  }
  g_upload_file = File();
}

WiFiManager &WiFiManager::instance() {
  static WiFiManager inst;
  return inst;
}

bool WiFiManager::initialize() {
  if(initialized_) {
    return true;
  }

  // Defer WiFi.mode() until AP is actually started to avoid unnecessary memory allocation
  // WiFi.mode(WIFI_AP) allocates buffers even if AP isn't active
  // This is now called in startAccessPoint() instead
  
  initialized_ = true;
  return true;
}

void WiFiManager::shutdown() {
  stopAccessPoint();
  initialized_ = false;
}

bool WiFiManager::startAccessPoint(const char *ssid, const char *password) {
  if(!initialized_) {
    if(!initialize()) {
      return false;
    }
  }

  if(ap_active_) {
    return true;  // Already active
  }

  // Initialize WiFi mode only when actually starting the AP (deferred allocation)
  WiFi.mode(WIFI_AP);
  
  // Disable power saving mode to ensure responsive network
  #ifdef ESP32
  esp_wifi_set_ps(WIFI_PS_NONE);
  #endif

  if(ssid != nullptr && strlen(ssid) > 0) {
    strncpy(ap_ssid_, ssid, sizeof(ap_ssid_) - 1);
    ap_ssid_[sizeof(ap_ssid_) - 1] = '\0';
  }

  if(password != nullptr && strlen(password) > 0) {
    strncpy(ap_password_, password, sizeof(ap_password_) - 1);
    ap_password_[sizeof(ap_password_) - 1] = '\0';
  } else {
    ap_password_[0] = '\0';  // Open AP if no password
  }

  IPAddress local_ip(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(local_ip, gateway, subnet);
  
  bool result = false;
  if(ap_password_[0] != '\0') {
    result = WiFi.softAP(ap_ssid_, ap_password_);
  } else {
    result = WiFi.softAP(ap_ssid_);
  }

  if(!result) {
    Serial.println("Failed to start WiFi AP");
    return false;
  }

  // Wait a bit for AP to fully initialize
  delay(1000);
  
  IPAddress ip = WiFi.softAPIP();
  if(ip == IPAddress(0,0,0,0)) {
    Serial.println("WiFi AP IP is invalid");
    WiFi.softAPdisconnect();
    return false;
  }
  
  ap_ip_ = ip.toString().c_str();
  ap_active_ = true;

  Serial.printf("WiFi AP started: SSID=%s, IP=%s\n", ap_ssid_, ap_ip_.c_str());
  Serial.printf("WiFi AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("WiFi AP clients: %d\n", WiFi.softAPgetStationNum());
  Serial.printf("WiFi AP status: %d\n", WiFi.status());

  // Start web server
  if(g_web_server == nullptr) {
    g_web_server = new WebServer(WEB_SERVER_PORT);
    if(g_web_server != nullptr) {
      g_web_server->on("/", HTTP_GET, handleRoot);
      g_web_server->on("/upload", HTTP_POST, handleUploadComplete, handleUpload);
      g_web_server->onNotFound(handleNotFound);
      
      g_web_server->begin();
      Serial.println("Web server started on port 80");
      delay(200);  // Give server time to initialize
      
      // Start mDNS for Bonjour/ZeroConf discovery (makes it discoverable in Finder)
      #ifdef ESP32
      if(MDNS.begin("cardputer")) {
        MDNS.addService("http", "tcp", WEB_SERVER_PORT);
        MDNS.addServiceTxt("http", "tcp", "name", "Cardputer Airdrop");
        MDNS.addServiceTxt("http", "tcp", "type", "file-transfer");
        Serial.println("mDNS started: cardputer.local");
      } else {
        Serial.println("mDNS failed to start");
      }
      #endif
    } else {
      Serial.println("Failed to create WebServer object");
      return false;
    }
  }

  return true;
}

void WiFiManager::stopAccessPoint() {
  if(!ap_active_) {
    return;
  }

  if(g_web_server != nullptr) {
    g_web_server->stop();
    delete g_web_server;
    g_web_server = nullptr;
  }

  #ifdef ESP32
  MDNS.end();
  #endif

  WiFi.softAPdisconnect(true);
  ap_active_ = false;
  ap_ip_.clear();
  Serial.println("WiFi AP stopped");
}

void WiFiManager::loop() {
  if(ap_active_ && g_web_server != nullptr) {
    // Check for clients periodically
    static uint32_t last_client_check = 0;
    static uint32_t last_loop_log = 0;
    uint32_t now = millis();
    
    if(now - last_client_check > 5000) {
      last_client_check = now;
      int client_count = WiFi.softAPgetStationNum();
      Serial.printf("WiFi status: AP clients=%d, server active=%d\n", 
                    client_count, 
                    g_web_server != nullptr ? 1 : 0);
    }
    
    // Log loop activity occasionally for debugging
    if(now - last_loop_log > 10000) {
      last_loop_log = now;
      Serial.println("WiFi loop active");
    }
    
    // Handle web server clients - this must be called frequently
    g_web_server->handleClient();
    
    // Note: mDNS works automatically once started, no update() call needed
    
    // Small yield to prevent watchdog issues
    yield();
  }
}

#endif  // ENABLE_WIFI_AIRDROP

