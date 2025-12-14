#ifndef MUSICBOX_BLE_H
#define MUSICBOX_BLE_H

#include <Arduino.h>
#include <BLE2902.h>  // Required for notifications
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// --- UUID Definitions (Must match Android App) ---
#define SERVICE_UUID "adb18dc5-9d83-408e-afe7-9b19faa130a5"
#define CHAR_PLAYBACK_UUID "aa0d9e11-c2b2-46cd-8d50-99a1dbbc22ec"  // Notify: Track info
#define CHAR_CONTROL_UUID "02d22a93-100e-4a10-b4ab-f2a0c8efb538"   // Write: Commands
#define CHAR_DEBUG_UUID "c68e8ebd-c2c1-40b6-8731-19a4605e424b"     // Notify: Debug logs
#define CHAR_STATE_UUID "48e8dc38-e760-4547-a4ed-727dff06a90b"     // Notify: System state

// --- Global Flags & Variables ---
extern bool deviceConnected;
extern bool debugEnabled;  // Controlled by Android app (DEBUG_START/STOP)

// --- Function Declarations ---
void setupBLE(String deviceName);
void loopBLE();  // Call this in main loop for periodic state updates

// Functions to send data to the App
void sendBlePlaybackInfo(int track, int total, String file, int volume);
void sendBleStateInfo(String state, int uptime);
void sendBleDebugLog(String message);

// Placeholders for your actual hardware logic (You need to fill these in main.cpp or separate
// handler) These are called when the App writes to the Control Characteristic
void onCommandNext();
void onCommandPrev();
void onCommandVolume(int volume);
void onCommandDebugTree();
void onCommandReboot();

#endif