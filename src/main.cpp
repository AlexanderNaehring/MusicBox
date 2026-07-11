#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_sleep.h>

#include "Battery.h"
#include "Board.h"
#include "Config.h"
#include "Controls.h"
#include "DeviceState.h"
#include "Nfc.h"
#include "Player.h"
#include "Power.h"
#include "git_info.h"

#if BLE
#include "MusicBoxBLE.h"
#endif

#if WIFI
#include "MusicBoxWifi.h"
#endif

#if WIFI_DOWNLOAD
#include "MusicBoxWifiDownloader.h"
#endif

void debugPrint(String msg) {
  if (DEBUG) Serial.println(msg);
#if BLE
  sendBleLog(msg + String("\n"));
#endif
}

#define BATTERY_CHECK_INTERVAL 60000  // 60 seconds
static unsigned long lastBatteryCheck = 0;

RTC_DATA_ATTR DeviceState shutdownDeviceState = DeviceState::SETUP;

StateMachine stateMachine;

// Sends the BLE app a full snapshot of what's now playing. Needed both when
// playback starts fresh and when the queue silently advances to the next
// track mid-song - the state itself doesn't change in the latter case, so
// the StateMachine's onChange hook alone wouldn't cover it.
static void publishNowPlaying() {
#if BLE
  bleInfo.beginUpdate();
  bleInfo.setState(toString(DeviceState::PLAYING));
  bleInfo.setBatteryPct(100);
  bleInfo.setFolder(player.currentFolderPath() ? String(player.currentFolderPath()) : String(""));
  bleInfo.setFile(String(player.currentTrackPath()));
  bleInfo.setTrackIdx(player.currentIndex() + 1);
  bleInfo.setTrackTotal((int)player.queueSize());
  bleInfo.endUpdate();
#endif
}

// Common landing point for every direct playNext()/playPrev()/playFirst()
// attempt (buttons, BLE commands): translates the result into a state
// transition, since Player itself has no notion of device state.
static void afterPlayAttempt(bool started) {
  if (started) {
    stateMachine.set(DeviceState::PLAYING);
    publishNowPlaying();
  } else {
    nfcReader.forget();
    stateMachine.set(DeviceState::STOPPED);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Welcome to MusicBox");
  Serial.printf("Built from git commit %s on %s at %s\n", GIT_COMMIT, __DATE__, __TIME__);

  Power::printWakeupReason();
  battery.begin();

  Serial.printf("Last shutdown state: %s\n", toString(shutdownDeviceState).c_str());

  board.begin();
  stateMachine.onChange([](DeviceState newState) {
    board.showState(newState);
#if BLE
    bleInfo.setState(toString(newState));
#endif
  });

  // Buttons
  pinMode(BTN_CARD_INSIDE, INPUT_PULLUP);
  controls.begin(player, afterPlayAttempt);

  // SD card
  Serial.println("SD_SPI...");
  spi_sd.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("Try connecting SD with 40 MHz...");
  if (!SD.begin(SD_CS, spi_sd, 40000000)) {
    Serial.println("Try connecting SD with 25 MHz...");
    if (!SD.begin(SD_CS, spi_sd)) {
      Serial.println("Error: SD card mount failed");
      return;
    }
  }

  // RFID - NFC
  if (!nfcReader.begin()) {
    return;
  }

  // Audio
  audioLogger = &Serial;
  Serial.println("Audio...");
  board.beginAudioOutput();
  player.begin(SD, board.audioOutput());

// BLE
#if BLE
  Serial.println("BLE...");
  setupBLE("MusicBox");
#endif

// WIFI
#if WIFI
  if (digitalRead(BTN_PREV) == LOW) {
    Serial.println("BTN_PREV pressed, start WIFI setup mode");
    setupWifi(SD);
  } else {
    Serial.println("BTN_PREV not pressed, skip WIFI setup");
    WiFi.mode(WIFI_OFF);
  }
#endif

  Serial.println("Setup ready...");
  stateMachine.set(DeviceState::IDLE);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason != 0 && shutdownDeviceState == DeviceState::STOPPED) {
    Serial.println("Woke up after previous STOPPED state");
    stateMachine.set(DeviceState::STOPPED);
  }
}

void loop() {
  unsigned long now = millis();

  controls.tick();

  // check battery every interval
  if (now - lastBatteryCheck > BATTERY_CHECK_INTERVAL) {
    lastBatteryCheck = now;
    battery.readPercent();
  }

  bool cardPresent = (digitalRead(BTN_CARD_INSIDE) == LOW);

  switch (stateMachine.get()) {
      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::IDLE:
      // waiting for new NFC card to be detected.
      // No playback or other activity.
      if (cardPresent) {
        stateMachine.set(DeviceState::READING_NFC);
      }
#if HW_REV == 1
      Power::lightSleep(250);
#elif HW_REV == 2
      Power::lightSleep(5000, BTN_CARD_INSIDE, 0);
#endif
      break;

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::READING_NFC: {
      if (!cardPresent) {
        // card removed during reading
        stateMachine.set(DeviceState::IDLE);
        break;
      }

      String filePath;
      switch (nfcReader.poll(now, filePath)) {
        case Nfc::PollResult::NoTag:
          break;

        case Nfc::PollResult::SameTagResumed:
          stateMachine.set(DeviceState::PLAYING);
          break;

        case Nfc::PollResult::InvalidTag:
          stateMachine.set(DeviceState::STOPPED);
          board.setLED(RGB_Error);
          break;

        case Nfc::PollResult::UnsupportedRecord:
          board.setLED(RGB_Error);
          break;

        case Nfc::PollResult::NewTagPath: {
          bool contentReady = true;
#if WIFI_DOWNLOAD
          contentReady = ensureContentAvailable(SD, filePath.c_str());
#endif
          if (!contentReady) {
            Serial.printf("Content unavailable for '%s'\n", filePath.c_str());
            stateMachine.set(DeviceState::ERROR);
            break;
          }

          Player::PlayResult result = player.playPathOrFolder(filePath.c_str());
          nfcReader.rememberCurrentTag();
          if (result != Player::PlayResult::Started) {
            board.setLED(RGB_Error);
          }
          // Matches the device's existing behavior: once content is ready,
          // playback is considered started even if playPathOrFolder didn't
          // actually find anything playable.
          stateMachine.set(DeviceState::PLAYING);
          if (result == Player::PlayResult::Started) {
            publishNowPlaying();
          }
          break;
        }
      }
      break;
    }

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::PLAYING:
      if (!cardPresent) {
        player.saveCurrentPosition();
        stateMachine.set(DeviceState::PAUSED);
        break;
      }

      switch (player.update(now)) {
        case Player::UpdateResult::TrackAdvanced:
          publishNowPlaying();
          break;
        case Player::UpdateResult::QueueFinished:
          nfcReader.forget();
          stateMachine.set(DeviceState::STOPPED);
          break;
        case Player::UpdateResult::StalledError:
          // Audio stopped unexpectedly - maybe SD card error
          Serial.println("Audio stopped unexpectedly");
          stateMachine.set(DeviceState::STOPPED);
          board.setLED(RGB_Error);
          break;
        case Player::UpdateResult::Playing:
          break;
      }
      break;

      /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::PAUSED:
      if (cardPresent) {
        // Card inserted - resume playback (or start new card)
        stateMachine.set(DeviceState::READING_NFC);
        break;
      }
#if HW_REV == 1
      Power::lightSleep(250);
#elif HW_REV == 2
      Power::lightSleep(5000, BTN_CARD_INSIDE, 0);
#endif
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::STOPPED:
      if (!cardPresent) {
        // Card removed - now ready for a new card
        stateMachine.set(DeviceState::IDLE);
      }
#if HW_REV == 1
      // While card is still in, just wait - do nothing
      Power::lightSleep(1000);
#elif HW_REV == 2
      // save energy by sleeping until card is removed
      if (stateMachine.msInState() >= 1 * 60 * 1000) {
        Power::shutdown(stateMachine.get(), BTN_CARD_INSIDE, 1);
      } else {
        Power::lightSleep(5000, BTN_CARD_INSIDE, 1);
      }
#endif
      break;

    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::ERROR:
      Serial.println("ERROR STATE - restarting in 10 seconds...");
      board.setLED(RGB_Error);
      delay(10000);
      ESP.restart();
      break;
    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::SETUP:
      Serial.println("ERROR - invalid state SETUP during loop()");
      // fall through
    default:
      stateMachine.set(DeviceState::ERROR);
  }
}

#if BLE
void onBleCommand(String command) {
  debugPrint("BLE CMD Rx: " + command);

  if (command == "CMD:NEXT") {
    if (stateMachine.get() == DeviceState::PLAYING || stateMachine.get() == DeviceState::PAUSED)
      afterPlayAttempt(player.playNext());
  } else if (command == "CMD:PREV") {
    if (stateMachine.get() == DeviceState::PLAYING || stateMachine.get() == DeviceState::PAUSED)
      afterPlayAttempt(player.playPrev());
  } else if (command == "CMD:RESTART") {
    debugPrint("Rebooting...");
    delay(500);
    ESP.restart();
  } else if (command == "CMD:DIAG") {
    debugPrint("--- DIAGNOSTICS ---");
    debugPrint("Uptime: " + String(millis() / 1000) + "s");
    debugPrint("Heap Free: " + String(ESP.getFreeHeap()));
    debugPrint("Build: " + String(GIT_COMMIT) + " at " + String(__DATE__) + " at " +
               String(__TIME__));
  } else if (command == "CMD:TREE") {
    debugPrint("--- FILE TREE ---");
    // File root = filesystem->open("/");
    // print...
    // root.close();
    debugPrint("--- END TREE ---");
  }
}
#endif
