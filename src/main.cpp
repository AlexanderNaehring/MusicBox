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

#define LOG_TAG "main"

void debugPrint(String msg) {
  if (DEBUG) LOGLN(msg);
#if BLE
  sendBleLog(msg + String("\n"));
#endif
}

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

// Battery Low is a warning only - playback continues, the LED just blinks
// (see loop()) and a cue plays once. Critical means further draw risks a
// brownout or over-discharge, so nothing else matters: save position, stop
// playback immediately (even mid-track), and deep-sleep as fast as
// possible - bypassing AllowSleep, since this is safety, not the
// experimental sleep feature.
static void onBatteryLevelChange(BatteryLevel level) {
  if (level == BatteryLevel::Low) {
    LOGLN("Battery low");
    board.playCue(SD, "/sounds/low_battery.mp3");
  } else if (level == BatteryLevel::Critical) {
    LOGLN("Battery critical - shutting down");
    // Only PLAYING guarantees a track is actually loaded (sourceId3_ set) -
    // this can fire from any state, including straight out of boot.
    if (stateMachine.get() == DeviceState::PLAYING) {
      player.saveCurrentPosition();
    }
    player.stop();
    stateMachine.set(DeviceState::STOPPED);
    Power::shutdown(DeviceState::STOPPED, UINT8_MAX, 0, /*force=*/true);
  }
}

// Auto-sleep: an optional per-tag countdown (from the tag's second NDEF
// record, see Nfc::PlaybackMode) that stops playback after a fixed
// duration - e.g. falling asleep to an album instead of it playing all
// night. Deliberately lives here, not in Player: it's about *when to stop*,
// not how to play, and it needs to keep ticking across PAUSED (card pulled)
// too, which Player has no visibility into.
static bool autoSleepActive = false;
static unsigned long autoSleepDeadlineMillis = 0;

// Call on every fresh "card is now playing" transition - both a brand new
// tag and the same tag being resumed after a pull count as restarting the
// countdown from its full duration.
static void resetAutoSleepTimer() {
  Nfc::PlaybackMode mode = nfcReader.currentMode();
  autoSleepActive = mode.autoSleepMinutes > 0;
  autoSleepDeadlineMillis = millis() + (unsigned long)mode.autoSleepMinutes * 60000UL;
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
  LOGLN("Welcome to MusicBox");
  LOGF("Built from git commit %s on %s at %s\n", GIT_COMMIT, __DATE__, __TIME__);

  Power::printWakeupReason();
  battery.onLevelChange(onBatteryLevelChange);
  battery.begin();

  LOGF("Last shutdown state: %s\n", toString(shutdownDeviceState).c_str());

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
  LOGLN("SD_SPI...");
  spi_sd.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  LOGLN("Try connecting SD with 40 MHz...");
  if (!SD.begin(SD_CS, spi_sd, 40000000)) {
    LOGLN("Try connecting SD with 25 MHz...");
    if (!SD.begin(SD_CS, spi_sd)) {
      LOGLN("Error: SD card mount failed");
      return;
    }
  }

  // RFID - NFC
  if (!nfcReader.begin()) {
    return;
  }

  // Audio
  audioLogger = &Serial;
  LOGLN("Audio...");
  board.beginAudioOutput();
  player.begin(SD, board.audioOutput());

// BLE
#if BLE
  LOGLN("BLE...");
  setupBLE("MusicBox");
#endif

// WIFI
#if WIFI
  if (digitalRead(BTN_PREV) == LOW) {
    LOGLN("BTN_PREV pressed, start WIFI setup mode");
    setupWifi(SD);
  } else {
    LOGLN("BTN_PREV not pressed, skip WIFI setup");
    WiFi.mode(WIFI_OFF);
  }
#endif

  LOGLN("Setup ready...");
  stateMachine.set(DeviceState::IDLE);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason != 0 && shutdownDeviceState == DeviceState::STOPPED) {
    LOGLN("Woke up after previous STOPPED state");
    stateMachine.set(DeviceState::STOPPED);
  }
}

void loop() {
  unsigned long now = millis();

  controls.tick();
  battery.tick();

  // Low-battery LED blink: while Low, alternate the LED between the warning
  // color and whatever the current state's color would be, every 500ms.
  // Repaints the real state color once Low clears, since the blink writes
  // to the LED outside StateMachine::onChange. 
  {
    static bool blinkOn = false;
    static bool wasLow = false;
    static unsigned long lastBlinkMillis = 0;
    bool isLow = (battery.level() == BatteryLevel::Low);
    if (isLow) {
      if (now - lastBlinkMillis >= 500) {
        lastBlinkMillis = now;
        blinkOn = !blinkOn;
        if (blinkOn) {
          board.setLED(RGB_LowBattery);
        } else {
          board.showState(stateMachine.get());
        }
      }
    } else if (wasLow) {
      board.showState(stateMachine.get());
    }
    wasLow = isLow;
  }

  bool cardPresent = (digitalRead(BTN_CARD_INSIDE) == LOW);

  // Auto-sleep expiry: ticks continuously in wall-clock time regardless of
  // PLAYING/PAUSED (see resetAutoSleepTimer()), so it can expire while the
  // card is out too. Only meaningful while a tag is actually committed to.
  if (autoSleepActive &&
      (stateMachine.get() == DeviceState::PLAYING || stateMachine.get() == DeviceState::PAUSED) &&
      (long)(now - autoSleepDeadlineMillis) >= 0) {
    LOGLN("Auto-sleep timer expired");
    autoSleepActive = false;
    player.stop();
    nfcReader.forget();
    stateMachine.set(cardPresent ? DeviceState::STOPPED : DeviceState::IDLE);
    board.setLED(RGB_SleepExpired);
  }

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
          resetAutoSleepTimer();
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
            LOGF("Content unavailable for '%s'\n", filePath.c_str());
            stateMachine.set(DeviceState::ERROR);
            break;
          }

          Nfc::PlaybackMode mode = nfcReader.currentMode();
          PlaybackOptions options;
          options.shuffle = mode.shuffle;
          options.persistPosition = !(mode.shuffle || mode.autoSleepMinutes > 0);
          Player::PlayResult result = player.playPathOrFolder(filePath.c_str(), options);
          nfcReader.rememberCurrentTag();
          resetAutoSleepTimer();
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
          LOGLN("Audio stopped unexpectedly");
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
      LOGLN("ERROR STATE - restarting in 10 seconds...");
      board.setLED(RGB_Error);
      delay(10000);
      ESP.restart();
      break;
    /////////////////////////////////////////////////////////////////////////////////
    case DeviceState::SETUP:
      LOGLN("ERROR - invalid state SETUP during loop()");
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
