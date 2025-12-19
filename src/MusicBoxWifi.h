#ifndef MUSICBOX_WIFI_H
#define MUSICBOX_WIFI_H

#include <ESPAsyncHTTPUpdateServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

enum class WifiState { Idle, Setup, WaitingForConnection, Listening };

void setupWifi(void);
void shutdownWifi(void);

void loopWifi(void);

#endif