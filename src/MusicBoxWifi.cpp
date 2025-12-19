#include "MusicBoxWifi.h"

#ifndef WIFI_SSID
#define WIFI_SSID "<please set your SSID>"
#define WIFI_PASSWORD "<please set your Wifi password>"
#endif

const char* host = "MusicBox";

WebServer httpServer(80);
HTTPUpdateServer httpUpdater;
WifiState wifiState = WifiState::Setup;

void setupWifi(void) { wifiState = WifiState::Setup; }

void shutdownWifi(void) {
  Serial.println("Shutting down WiFi...");
  httpServer.close();
  MDNS.end();
  WiFi.disconnectAsync(true);
  wifiState = WifiState::Idle;
}

void loopWifi(void) {
  switch (wifiState) {
    case WifiState::Idle:
      break;
    case WifiState::Setup:
      WiFi.mode(WIFI_AP_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
      wifiState = WifiState::WaitingForConnection;
      break;
    case WifiState::WaitingForConnection:
      if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin(host)) {
          Serial.println("mDNS responder started");
        }
        httpUpdater.setup(&httpServer, "/update");
        httpServer.begin();
        MDNS.addService("http", "tcp", 80);
        Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n",
                      host);
        wifiState = WifiState::Listening;
      }
      break;
    case WifiState::Listening:
      httpServer.handleClient();
      break;
    default:
      Serial.println("Unknown WiFi state!");
      shutdownWifi();
      break;
  }
}