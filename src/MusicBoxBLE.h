#ifndef MUSICBOX_BLE_H
#define MUSICBOX_BLE_H

#include <Arduino.h>
#include <NimBLEDevice.h>

// UUID Definitions
#define SERVICE_UUID "adb18dc5-9d83-408e-afe7-9b19faa130a5"
#define CHAR_INFO_UUID "aa0d9e11-c2b2-46cd-8d50-99a1dbbc22ec"
#define CHAR_CMD_UUID "02d22a93-100e-4a10-b4ab-f2a0c8efb538"
#define CHAR_LOG_UUID "c68e8ebd-c2c1-40b6-8731-19a4605e424b"

// Function Declarations
void setupBLE(String deviceName);
void loopBLE();  // Call this in main loop for periodic state updates

// Functions to send data to the App
void sendBleInfo(String state = "", int batteryPct = -1, String folder = "", String file = "",
                 int trackIdx = -1, int trackTotal = -1);
void sendBleLog(String message);

// Callback to handle incoming commands from the App
void onBleCommand(String command);

#endif