#include "MusicBoxWifi.h"

#include <ArduinoJson.h>
#include <FS.h>

#ifndef WIFI_SSID
#define WIFI_SSID "<please set your SSID>"
#define WIFI_PASSWORD "<please set your Wifi password>"
#endif

const char* host = "MusicBox";

AsyncWebServer webServer(80);
ESPAsyncHTTPUpdateServer updateServer;
WebFileManager fileManager;

void setupWifi(fs::FS& fs) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);

  while (WiFi.status() != WL_CONNECTED) {
    delay(10);
  }
  if (MDNS.begin(host)) {
    Serial.println("mDNS responder started");
  }
  fileManager.setup(webServer, fs, "/");
  updateServer.setup(&webServer, "/update");
  webServer.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n", host);
}

void shutdownWifi(void) {
  Serial.println("Shutting down WiFi...");
  webServer.end();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void loopWifi(void) {}

WebFileManager::WebFileManager() {}

void WebFileManager::setup(AsyncWebServer& server, fs::FS& fs, const char* root) {
  _server = &server;
  _fs = &fs;
  _rootPath = root;
  setupRoutes();
  _server->begin();
}

void WebFileManager::setupRoutes() {
  _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleRoot(request); });
  _server->on("/api/files", HTTP_GET,
              [this](AsyncWebServerRequest* request) { this->handleFileList(request); });
  _server->on("/api/delete", HTTP_POST,
              [this](AsyncWebServerRequest* request) { this->handleFileDelete(request); });
  _server->on("/api/mkdir", HTTP_POST,
              [this](AsyncWebServerRequest* request) { this->handleCreateFolder(request); });
  _server->on(
      "/api/upload", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"success\":true}");
      },
      [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data,
             size_t len,
             bool final) { this->handleFileUpload(request, filename, index, data, len, final); });
}

void WebFileManager::handleRoot(AsyncWebServerRequest* request) {
  String htmlPath = String("/web/index.html");
  File htmlFile = _fs->open(htmlPath.c_str());
  if (htmlFile && !htmlFile.isDirectory()) {
    request->send(*_fs, htmlPath.c_str(), "text/html");
    htmlFile.close();
    return;
  }
  if (htmlFile) htmlFile.close();

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MusicBox File Manager</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
            background: white;
            border-radius: 12px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            overflow: hidden;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            text-align: center;
        }
        .header h1 { font-size: 2em; margin-bottom: 10px; }
        .header p { opacity: 0.9; }
        .controls {
            padding: 20px;
            background: #f8f9fa;
            border-bottom: 1px solid #dee2e6;
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
        }
        .btn {
            padding: 10px 20px;
            border: none;
            border-radius: 6px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 500;
            transition: all 0.3s;
        }
        .btn-primary {
            background: #667eea;
            color: white;
        }
        .btn-primary:hover { background: #5568d3; }
        .btn-danger {
            background: #dc3545;
            color: white;
        }
        .btn-danger:hover { background: #c82333; }
        .btn-success {
            background: #28a745;
            color: white;
        }
        .btn-success:hover { background: #218838; }
        input[type="text"], input[type="file"] {
            padding: 10px;
            border: 1px solid #ced4da;
            border-radius: 6px;
            font-size: 14px;
        }
        .file-list {
            padding: 20px;
            max-height: 600px;
            overflow-y: auto;
        }
        .breadcrumb {
            padding: 15px 20px;
            background: #e9ecef;
            display: flex;
            align-items: center;
            gap: 5px;
            font-size: 14px;
        }
        .breadcrumb-item {
            cursor: pointer;
            color: #667eea;
        }
        .breadcrumb-item:hover { text-decoration: underline; }
        .file-item {
            display: flex;
            align-items: center;
            padding: 12px;
            border-bottom: 1px solid #e9ecef;
            transition: background 0.2s;
        }
        .file-item:hover { background: #f8f9fa; }
        .file-icon {
            width: 40px;
            height: 40px;
            margin-right: 15px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 24px;
        }
        .file-info {
            flex: 1;
        }
        .file-name {
            font-weight: 500;
            margin-bottom: 3px;
            cursor: pointer;
        }
        .file-name:hover { color: #667eea; }
        .file-size {
            font-size: 12px;
            color: #6c757d;
        }
        .file-actions {
            display: flex;
            gap: 5px;
        }
        .icon-btn {
            padding: 6px 12px;
            background: #e9ecef;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-size: 12px;
            transition: all 0.2s;
        }
        .icon-btn:hover { background: #dee2e6; }
        .status {
            padding: 15px 20px;
            background: #d1ecf1;
            color: #0c5460;
            border-top: 1px solid #bee5eb;
            display: none;
        }
        .status.show { display: block; }
        .status.error {
            background: #f8d7da;
            color: #721c24;
            border-color: #f5c6cb;
        }
        .modal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0,0,0,0.5);
            align-items: center;
            justify-content: center;
            z-index: 1000;
        }
        .modal.show { display: flex; }
        .modal-content {
            background: white;
            padding: 30px;
            border-radius: 12px;
            max-width: 500px;
            width: 90%;
        }
        .modal-header {
            font-size: 1.5em;
            margin-bottom: 20px;
        }
        .modal-body {
            margin-bottom: 20px;
        }
        .modal-footer {
            display: flex;
            justify-content: flex-end;
            gap: 10px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🎵 MusicBox File Manager</h1>
            <p>Manage your music files and folders</p>
        </div>
        
        <div class="controls">
            <input type="file" id="fileInput" multiple style="flex: 1; min-width: 200px;">
            <button class="btn btn-primary" onclick="uploadFiles()">📤 Upload</button>
            <button class="btn btn-success" onclick="showCreateFolder()">📁 New Folder</button>
            <button class="btn btn-primary" onclick="refreshFiles()">🔄 Refresh</button>
        </div>
        
        <div class="breadcrumb" id="breadcrumb"></div>
        
        <div class="file-list" id="fileList">
            <p style="text-align: center; color: #6c757d; padding: 40px;">Loading files...</p>
        </div>
        
        <div class="status" id="status"></div>
    </div>
    
    <div class="modal" id="createFolderModal">
        <div class="modal-content">
            <div class="modal-header">Create New Folder</div>
            <div class="modal-body">
                <input type="text" id="folderName" placeholder="Folder name" style="width: 100%;">
            </div>
            <div class="modal-footer">
                <button class="btn" onclick="hideCreateFolder()">Cancel</button>
                <button class="btn btn-success" onclick="createFolder()">Create</button>
            </div>
        </div>
    </div>

    <script>
        let currentPath = '/';
        
        function showStatus(message, isError = false) {
            const status = document.getElementById('status');
            status.textContent = message;
            status.className = 'status show' + (isError ? ' error' : '');
            setTimeout(() => status.classList.remove('show'), 3000);
        }
        
        function updateBreadcrumb() {
            const breadcrumb = document.getElementById('breadcrumb');
            const parts = currentPath.split('/').filter(p => p);
            
            let html = '<span class="breadcrumb-item" onclick="navigateTo(\'/\')">🏠 Root</span>';
            let path = '';
            
            parts.forEach((part, index) => {
                path += '/' + part;
                const fullPath = path;
                html += ` / <span class="breadcrumb-item" onclick="navigateTo('${fullPath}')">${part}</span>`;
            });
            
            breadcrumb.innerHTML = html;
        }
        
        function navigateTo(path) {
            currentPath = path;
            refreshFiles();
        }
        
        async function refreshFiles() {
            try {
                const response = await fetch(`/api/files?path=${encodeURIComponent(currentPath)}`);
                const data = await response.json();
                
                updateBreadcrumb();
                displayFiles(data);
            } catch (error) {
                showStatus('Failed to load files: ' + error.message, true);
            }
        }
        
        function displayFiles(data) {
            const fileList = document.getElementById('fileList');
            
            if (!data.files || data.files.length === 0) {
                fileList.innerHTML = '<p style="text-align: center; color: #6c757d; padding: 40px;">This folder is empty</p>';
                return;
            }
            
            let html = '';
            
            data.files.sort((a, b) => {
                if (a.type !== b.type) return a.type === 'dir' ? -1 : 1;
                return a.name.localeCompare(b.name);
            });
            
            data.files.forEach(file => {
                const icon = file.type === 'dir' ? '📁' : 
                            file.name.endsWith('.mp3') ? '🎵' : '📄';
                const size = file.type === 'dir' ? '' : formatBytes(file.size);
                const fullPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + file.name;
                
                html += `
                    <div class="file-item">
                        <div class="file-icon">${icon}</div>
                        <div class="file-info">
                            <div class="file-name" onclick="${file.type === 'dir' ? `navigateTo('${fullPath}')` : ''}">${file.name}</div>
                            <div class="file-size">${size}</div>
                        </div>
                        <div class="file-actions">
                            <button class="icon-btn" onclick="deleteFile('${fullPath}', '${file.type === 'dir'}')">🗑️ Delete</button>
                        </div>
                    </div>
                `;
            });
            
            fileList.innerHTML = html;
        }
        
        function formatBytes(bytes) {
            if (bytes === 0) return '0 Bytes';
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
        }
        
        async function deleteFile(path, isDir) {
            if (!confirm(`Are you sure you want to delete ${isDir ? 'this folder and all its contents' : 'this file'}?`)) {
                return;
            }
            
            try {
                const response = await fetch('/api/delete', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `path=${encodeURIComponent(path)}`
                });
                
                const result = await response.json();
                
                if (result.success) {
                    showStatus('Deleted successfully');
                    refreshFiles();
                } else {
                    showStatus('Delete failed: ' + (result.error || 'Unknown error'), true);
                }
            } catch (error) {
                showStatus('Delete failed: ' + error.message, true);
            }
        }
        
        async function uploadFiles() {
            const fileInput = document.getElementById('fileInput');
            const files = fileInput.files;
            
            if (files.length === 0) {
                showStatus('Please select files to upload', true);
                return;
            }
            
            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                const formData = new FormData();
                formData.append('file', file);
                formData.append('path', currentPath);
                
                try {
                    showStatus(`Uploading ${file.name}...`);
                    const response = await fetch('/api/upload', {
                        method: 'POST',
                        body: formData
                    });
                    
                    if (!response.ok) {
                        throw new Error(`Upload failed for ${file.name}`);
                    }
                } catch (error) {
                    showStatus(`Upload failed: ${error.message}`, true);
                    return;
                }
            }
            
            showStatus('All files uploaded successfully');
            fileInput.value = '';
            refreshFiles();
        }
        
        function showCreateFolder() {
            document.getElementById('createFolderModal').classList.add('show');
            document.getElementById('folderName').value = '';
            document.getElementById('folderName').focus();
        }
        
        function hideCreateFolder() {
            document.getElementById('createFolderModal').classList.remove('show');
        }
        
        async function createFolder() {
            const folderName = document.getElementById('folderName').value.trim();
            
            if (!folderName) {
                showStatus('Please enter a folder name', true);
                return;
            }
            
            const folderPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + folderName;
            
            try {
                const response = await fetch('/api/mkdir', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `path=${encodeURIComponent(folderPath)}`
                });
                
                const result = await response.json();
                
                if (result.success) {
                    showStatus('Folder created successfully');
                    hideCreateFolder();
                    refreshFiles();
                } else {
                    showStatus('Failed to create folder: ' + (result.error || 'Unknown error'), true);
                }
            } catch (error) {
                showStatus('Failed to create folder: ' + error.message, true);
            }
        }
        
        // Initial load
        refreshFiles();
    </script>
</body>
</html>
)rawliteral";

  request->send(200, "text/html", html);
}

void WebFileManager::handleFileList(AsyncWebServerRequest* request) {
  String path = "/";
  if (request->hasParam("path")) {
    path = request->getParam("path")->value();
  }

  String json = listFilesJSON(path.c_str());
  request->send(200, "application/json", json);
}

String WebFileManager::listFilesJSON(const char* dirname, uint8_t levels) {
  DynamicJsonDocument doc(8192);
  JsonArray filesArray = doc.createNestedArray("files");

  File root = _fs->open(dirname);
  if (!root || !root.isDirectory()) {
    doc["error"] = "Failed to open directory";
    String output;
    serializeJson(doc, output);
    return output;
  }

  File file = root.openNextFile();
  while (file) {
    JsonObject fileObj = filesArray.createNestedObject();
    fileObj["name"] = String(file.name());
    fileObj["type"] = file.isDirectory() ? "dir" : "file";
    fileObj["size"] = file.size();

    file.close();
    file = root.openNextFile();
  }
  root.close();

  String output;
  serializeJson(doc, output);
  return output;
}

void WebFileManager::handleFileDelete(AsyncWebServerRequest* request) {
  if (!request->hasParam("path", true)) {
    request->send(400, "application/json", "{\"success\":false,\"error\":\"No path specified\"}");
    return;
  }

  String path = request->getParam("path", true)->value();

  File file = _fs->open(path.c_str());
  if (!file) {
    request->send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    deleteRecursive(path.c_str());
  } else {
    file.close();
    _fs->remove(path.c_str());
  }

  request->send(200, "application/json", "{\"success\":true}");
}

void WebFileManager::deleteRecursive(const char* path) {
  File file = _fs->open(path);
  if (!file) return;

  if (file.isDirectory()) {
    File child = file.openNextFile();
    while (child) {
      String childPath = String(path) + "/" + String(child.name());
      if (child.isDirectory()) {
        child.close();
        deleteRecursive(childPath.c_str());
      } else {
        child.close();
        _fs->remove(childPath.c_str());
      }
      child = file.openNextFile();
    }
    file.close();
    _fs->rmdir(path);
  } else {
    file.close();
    _fs->remove(path);
  }
}

void WebFileManager::handleFileUpload(AsyncWebServerRequest* request, String filename, size_t index,
                                      uint8_t* data, size_t len, bool final) {
  static File uploadFile;
  static String uploadPath;

  if (index == 0) {
    // Get path from request on first chunk
    uploadPath = _rootPath;
    if (request->hasArg("path")) {
      uploadPath = request->arg("path");
    }

    if (!uploadPath.endsWith("/")) uploadPath += "/";
    uploadPath += filename;

    Serial.printf("Upload Start: %s\n", uploadPath.c_str());
    uploadFile = _fs->open(uploadPath.c_str(), FILE_WRITE);
  }

  if (uploadFile && len > 0) {
    uploadFile.write(data, len);
  }

  if (final) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload Complete: %s (%u bytes)\n", filename.c_str(), index + len);
    }
  }
}

void WebFileManager::handleCreateFolder(AsyncWebServerRequest* request) {
  if (!request->hasParam("path", true)) {
    request->send(400, "application/json", "{\"success\":false,\"error\":\"No path specified\"}");
    return;
  }

  String path = request->getParam("path", true)->value();

  if (_fs->mkdir(path.c_str())) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    request->send(500, "application/json",
                  "{\"success\":false,\"error\":\"Failed to create folder\"}");
  }
}
