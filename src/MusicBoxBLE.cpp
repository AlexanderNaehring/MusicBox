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
    bleInfo.send();  // Send current state upon connection
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

// Implementation of BleInfo declared in the header.
BleInfo::BleInfo()
    : state_(""),
      batteryPct_(-1),
      folder_(""),
      file_(""),
      trackIdx_(-1),
      trackTotal_(-1),
      batching_(false) {}

void BleInfo::beginUpdate() { batching_ = true; }
void BleInfo::endUpdate() {
  batching_ = false;
  send();
}

void BleInfo::setState(const String& state, bool sendImmediately) {
  if (state_ != state) {
    state_ = state;
    if (sendImmediately && !batching_) send();
  }
}

void BleInfo::setBatteryPct(int batteryPct, bool sendImmediately) {
  if (batteryPct_ != batteryPct) {
    batteryPct_ = batteryPct;
    if (sendImmediately && !batching_) send();
  }
}

void BleInfo::setFolder(const String& folder, bool sendImmediately) {
  if (folder_ != folder) {
    folder_ = folder;
    if (sendImmediately && !batching_) send();
  }
}

void BleInfo::setFile(const String& file, bool sendImmediately) {
  if (file_ != file) {
    file_ = file;
    if (sendImmediately && !batching_) send();
  }
}

void BleInfo::setTrackIdx(int idx, bool sendImmediately) {
  if (trackIdx_ != idx) {
    trackIdx_ = idx;
    if (sendImmediately && !batching_) send();
  }
}

void BleInfo::setTrackTotal(int total, bool sendImmediately) {
  if (trackTotal_ != total) {
    trackTotal_ = total;
    if (sendImmediately && !batching_) send();
  }
}

String BleInfo::getState() const { return state_; }
int BleInfo::getBatteryPct() const { return batteryPct_; }
String BleInfo::getFolder() const { return folder_; }
String BleInfo::getFile() const { return file_; }
int BleInfo::getTrackIdx() const { return trackIdx_; }
int BleInfo::getTrackTotal() const { return trackTotal_; }

void BleInfo::send() {
  if (!pServer) return;
  if (pServer->getConnectedCount() > 0) {
    // Construct JSON from the cached values
    String json = "{";
    json += "\"s\":\"" + state_ + "\",";
    json += "\"b\":" + String(batteryPct_);

    // Only add track info if we actually have a file cached
    if (file_.length() > 0 && (state_ == "PLAYING" || state_ == "PAUSED")) {
      json += ",\"fo\":\"" + folder_ + "\",";
      json += "\"fi\":\"" + file_ + "\",";
      json += "\"i\":" + String(trackIdx_) + ",";
      json += "\"n\":" + String(trackTotal_);
    }
    json += "}";

    Serial.println("BLE: Sending Info: " + json);

    if (pInfoChar) {
      pInfoChar->setValue(json);
      pInfoChar->notify();
    }
  }
}

BleInfo bleInfo;

void sendBleLog(String message) {
  if (pServer->getConnectedCount() > 0) {
    pLogChar->setValue(message);
    pLogChar->notify();
  }
}