#ifndef MUSICBOX_POWER_H
#define MUSICBOX_POWER_H

#include <Arduino.h>

#include "DeviceState.h"

// Light/deep sleep helpers. Both are no-ops unless AllowSleep is enabled in
// Config.h.
namespace Power {

void printWakeupReason();

void lightSleep(uint64_t timeout_ms = 1000, uint8_t wakeup_pin = UINT8_MAX, int level = 0);

// Persists `currentState` into shutdownDeviceState (retained across deep
// sleep) before powering down, so setup() can see what state we were in.
void shutdown(DeviceState currentState, uint8_t wakeup_pin = UINT8_MAX, int level = 0);

}  // namespace Power

#endif
