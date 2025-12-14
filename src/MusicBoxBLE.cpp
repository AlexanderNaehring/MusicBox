#include "MusicBoxBLE.h"

// Globals
BLEServer* pServer = NULL;
BLECharacteristic* pPlaybackChar = NULL;
BLECharacteristic* pControlChar = NULL;
BLECharacteristic* pDebugChar = NULL;
BLECharacteristic* pStateChar = NULL;

bool deviceConnected = false;
bool oldDeviceConnected = false;
bool debugEnabled = false;  // Default to false to save bandwidth

// --- Server Callback: Handle Connect/Disconnect ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE: Device Connected");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    debugEnabled = false;  // Reset debug on disconnect
    Serial.println("BLE: Device Disconnected");
  }
};

// --- Characteristic Callback: Handle Incoming Commands ---
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();

    if (value.length() > 0) {
      String command = value;
      Serial.print("BLE Received: ");
      Serial.println(command);

      // --- Command Parsing ---
      if (command == "NEXT") {
        onCommandNext();
      } else if (command == "PREV") {
        onCommandPrev();
      } else if (command.startsWith("VOL:")) {
        int newVol = command.substring(4).toInt();
        onCommandVolume(newVol);
      } else if (command == "DEBUG_START") {
        debugEnabled = true;
        sendBleDebugLog("Debug logging enabled on device.");
      } else if (command == "DEBUG_STOP") {
        sendBleDebugLog("Stopping debug logging...");
        debugEnabled = false;
      } else if (command == "CMD:TREE") {
        onCommandDebugTree();
      } else if (command == "CMD:REBOOT") {
        onCommandReboot();
      }
    }
  }
};

void setupBLE(String deviceName) {
  BLEDevice::init(deviceName.c_str());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the Service
  BLEService* pService = pServer->createService(SERVICE_UUID);

  // 1. Playback Characteristic (Notify)
  pPlaybackChar =
      pService->createCharacteristic(CHAR_PLAYBACK_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pPlaybackChar->addDescriptor(new BLE2902());

  // 2. Control Characteristic (Write)
  pControlChar =
      pService->createCharacteristic(CHAR_CONTROL_UUID, BLECharacteristic::PROPERTY_WRITE);
  pControlChar->setCallbacks(new MyCallbacks());

  // 3. Debug Characteristic (Notify)
  pDebugChar = pService->createCharacteristic(CHAR_DEBUG_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pDebugChar->addDescriptor(new BLE2902());

  // 4. State Characteristic (Notify)
  pStateChar = pService->createCharacteristic(CHAR_STATE_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pStateChar->addDescriptor(new BLE2902());

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();

  Serial.println("BLE: Waiting for a client connection to notify...");
}

void loopBLE() {
  // Handle reconnection logic if needed (ESP32 BLE quirks)
  if (!deviceConnected && oldDeviceConnected) {
    delay(100);                   // Give the bluetooth stack the chance to get things ready
    pServer->startAdvertising();  // Restart advertising
    Serial.println("BLE: Restarting advertising");
    oldDeviceConnected = deviceConnected;
  }
  // Connection established
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
}

// --- Sender Functions ---

void sendBlePlaybackInfo(int track, int total, String file, int volume) {
  if (deviceConnected) {
    // Construct JSON manually: {"track":1, "total":10, "file":"song.mp3", "volume":50}
    // Note: Escaping quotes with \"
    String json = "{";
    json += "\"track\":" + String(track) + ",";
    json += "\"total\":" + String(total) + ",";
    json += "\"file\":\"" + file + "\",";
    json += "\"volume\":" + String(volume);
    json += "}";

    pPlaybackChar->setValue((uint8_t*)json.c_str(), json.length());
    pPlaybackChar->notify();
  }
}

void sendBleStateInfo(String state, int uptime) {
  if (deviceConnected) {
    // Construct JSON: {"state":"PLAYING", "heap":12345, "uptime":100}
    String json = "{";
    json += "\"state\":\"" + state + "\",";
    json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime\":" + String(uptime);
    json += "}";

    pStateChar->setValue((uint8_t*)json.c_str(), json.length());
    pStateChar->notify();
  }
}

void sendBleDebugLog(String message) {
  // Only send if connected AND enabled via the "DEBUG_START" command
  if (deviceConnected && debugEnabled) {
    // Limit message size if necessary (BLE MTU default is 23 bytes, but libraries handle splitting
    // often) Ideally keep strings under 20 chars if MTU isn't negotiated, but ESP32 usually handles
    // more.
    pDebugChar->setValue((uint8_t*)message.c_str(), message.length());
    pDebugChar->notify();
  }
}