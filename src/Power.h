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
// Gated by AllowSleep unless `force` is set - for safety-critical shutdowns
// (e.g. critical battery) that must work even while the general sleep
// feature is disabled/untrusted.
void shutdown(DeviceState currentState, uint8_t wakeup_pin = UINT8_MAX, int level = 0,
              bool force = false);

}  // namespace Power

#endif
