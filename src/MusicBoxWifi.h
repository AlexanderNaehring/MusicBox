#ifndef MUSICBOX_WIFI_H
#define MUSICBOX_WIFI_H

#include <ESPAsyncHTTPUpdateServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

void setupWifi(fs::FS& fs);
void shutdownWifi(void);

class WebFileManager {
 public:
  WebFileManager();

  void setup(AsyncWebServer& server, fs::FS& fs, const char* root = "/");

 private:
  AsyncWebServer* _server;
  fs::FS* _fs;
  String _rootPath;

  void setupRoutes();
  void handleRoot(AsyncWebServerRequest* request);
  void handleFileList(AsyncWebServerRequest* request);
  void handleFileDelete(AsyncWebServerRequest* request);
  void handleFileUpload(AsyncWebServerRequest* request, String filename, size_t index,
                        uint8_t* data, size_t len, bool final);
  void handleCreateFolder(AsyncWebServerRequest* request);

  String listFilesJSON(const char* dirname, uint8_t levels = 1);
  void deleteRecursive(const char* path);
};

#endif