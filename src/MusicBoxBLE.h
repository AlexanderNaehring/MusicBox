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

// BLE info manager: cache device info and send updates.
// Usage:
//   bleInfo.setState("PLAYING"); // sends update immediately if not batching
//   bleInfo.beginUpdate();
//   bleInfo.setFolder("Abba", false);
//   bleInfo.setFile("DancingQueen.mp3", false);
//   bleInfo.setTrackIdx(1, false);
//   bleInfo.setTrackTotal(12, false);
//   bleInfo.endUpdate(); // sends single update with all changes
class BleInfo {
 public:
  BleInfo();
  void beginUpdate();
  void endUpdate();
  void send();  // explicit send

  void setState(const String& state, bool sendImmediately = true);
  void setBatteryPct(int batteryPct, bool sendImmediately = true);
  void setFolder(const String& folder, bool sendImmediately = true);
  void setFile(const String& file, bool sendImmediately = true);
  void setTrackIdx(int idx, bool sendImmediately = true);
  void setTrackTotal(int total, bool sendImmediately = true);

  // getters for convenience
  String getState() const;
  int getBatteryPct() const;
  String getFolder() const;
  String getFile() const;
  int getTrackIdx() const;
  int getTrackTotal() const;

 private:
  String state_;
  int batteryPct_;
  String folder_;
  String file_;
  int trackIdx_;
  int trackTotal_;
  bool batching_;
};

extern BleInfo bleInfo;

void sendBleLog(String message);

// Callback to handle incoming commands from the App
void onBleCommand(String command);

#endif