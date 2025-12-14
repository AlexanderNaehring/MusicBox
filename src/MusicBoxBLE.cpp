#include "MusicBoxBLE.h"

NimBLEServer* pServer = NULL;
NimBLECharacteristic* pInfoChar = NULL;
NimBLECharacteristic* pCmdChar = NULL;
NimBLECharacteristic* pLogChar = NULL;

bool bleConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    bleConnected = true;
    NimBLEDevice::startAdvertising();  // Keep advertising for multi-connect!
    Serial.println("BLE: Client connected");
  };

  void onDisconnect(NimBLEServer* pServer) {
    Serial.println("BLE: Client disconnected");
    if (pServer->getConnectedCount() == 0) {
      bleConnected = false;
    }
  }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    String cmd = pChar->getValue().c_str();
    if (cmd.length() > 0) {
      if (onBleCommand) onBleCommand(cmd);  // Forward to main.cpp
    }
  }
};

void setupBLE(String deviceName) {
  NimBLEDevice::init(deviceName.c_str());

  // high power = better range, but less battery life
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // ESP_PWR_LVL_P9

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  // 1. INFO Char (Notify only)
  pInfoChar = pService->createCharacteristic(CHAR_INFO_UUID,
                                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  // 2. LOG Char (Notify only)
  pLogChar = pService->createCharacteristic(CHAR_LOG_UUID,
                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  // 3. CMD Char (Write only)
  pCmdChar = pService->createCharacteristic(CHAR_CMD_UUID, NIMBLE_PROPERTY::WRITE);
  pCmdChar->setCallbacks(new CmdCallbacks());

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  Serial.println("BLE: Service started & Advertising");
}

void loopBLE() {
  // NimBLE handles housekeeping mostly automatically
}

void sendBleInfo(String state, int batteryPct, String folder, String file, int trackIdx,
                 int trackTotal) {
  // Keep cached last-known values so callers don't need to send everything each time
  static String lastState = "";
  static int lastBatteryPct = -1;
  static String lastFolder = "";
  static String lastFile = "";
  static int lastTrackIdx = -1;
  static int lastTrackTotal = -1;

  if (pServer->getConnectedCount() > 0) {
    // Update cached values only when callers provide them. The function signature uses
    // sentinel defaults (empty string or -1) to indicate "not provided".
    if (state.length() > 0) lastState = state;
    if (batteryPct != -1) lastBatteryPct = batteryPct;
    if (folder.length() > 0) lastFolder = folder;
    if (file.length() > 0) lastFile = file;
    if (trackIdx >= 0) lastTrackIdx = trackIdx;
    if (trackTotal >= 0) lastTrackTotal = trackTotal;

    // Construct JSON from the cached values
    // Example: {"s":"PLAYING","b":85,"fo":"Abba","fi":"DancingQueen.mp3","i":1,"n":12}
    String json = "{";
    json += "\"s\":\"" + lastState + "\",";
    json += "\"b\":" + String(lastBatteryPct);

    // Only add track info if we actually have a file cached
    if (lastFile.length() > 0 && (state == "PLAYING" || state == "PAUSED")) {
      json += ",\"fo\":\"" + lastFolder + "\",";
      json += "\"fi\":\"" + lastFile + "\",";
      json += "\"i\":" + String(lastTrackIdx) + ",";
      json += "\"n\":" + String(lastTrackTotal);
    }

    json += "}";

    Serial.println("BLE: Sending Info: " + json);

    pInfoChar->setValue(json);
    pInfoChar->notify();
  }
}

void sendBleLog(String message) {
  if (pServer->getConnectedCount() > 0) {
    pLogChar->setValue(message);
    pLogChar->notify();
  }
}