#ifndef MUSICBOX_WIFI_H
#define MUSICBOX_WIFI_H

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdateServer.h>
#include <WebServer.h>
#include <WiFi.h>

enum class WifiState { Idle, Setup, WaitingForConnection, Listening };

void setupWifi(void);
void shutdownWifi(void);

void loopWifi(void);

#endif