#ifndef MUSICBOX_DEVICE_STATE_H
#define MUSICBOX_DEVICE_STATE_H

#include <Arduino.h>
#include <functional>

// Player states
enum class DeviceState {
  SETUP,        // initial state
  IDLE,         // Waiting for card insertion, nothing in queue
  READING_NFC,  // (New) card detected, reading NFC data
  PLAYING,      // Playing audio from queue, card is inserted
  PAUSED,       // Card removed during playback.
  STOPPED,      // Playback finished, wait for card to be removed.
  ERROR         // ERROR state
};

inline String toString(DeviceState state) {
  switch (state) {
    case DeviceState::SETUP:
      return "SETUP";
    case DeviceState::IDLE:
      return "IDLE";
    case DeviceState::READING_NFC:
      return "READING_NFC";
    case DeviceState::PLAYING:
      return "PLAYING";
    case DeviceState::PAUSED:
      return "PAUSED";
    case DeviceState::STOPPED:
      return "STOPPED";
    case DeviceState::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN/ERROR";
  }
}

// Tracks the device's current state and how long it's been there. Register a
// single onChange() handler (e.g. in setup()) to react to transitions - the
// device's LED color and BLE status both hang off of that one hook, instead
// of every part of the codebase reaching in to set them individually.
class StateMachine {
 public:
  using ChangeHandler = std::function<void(DeviceState)>;

  DeviceState get() const { return current_; }

  void set(DeviceState newState) {
    if (current_ == newState) return;
    Serial.printf("State: %s -> %s\n", toString(current_).c_str(), toString(newState).c_str());
    current_ = newState;
    lastSwitchMillis_ = millis();
    if (onChange_) onChange_(newState);
  }

  unsigned long msInState() const { return millis() - lastSwitchMillis_; }

  void onChange(ChangeHandler handler) { onChange_ = handler; }

 private:
  DeviceState current_ = DeviceState::SETUP;
  unsigned long lastSwitchMillis_ = 0;
  ChangeHandler onChange_;
};

// Retained across deep sleep (RTC slow memory) so setup() can tell what state
// the device was in when it went down. Needs real static storage, so it stays
// a plain global rather than a class member. Defined in main.cpp, written by
// Power::shutdown().
extern RTC_DATA_ATTR DeviceState shutdownDeviceState;

#endif
