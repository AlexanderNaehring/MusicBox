#include "Power.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "Config.h"

#define LOG_TAG "Power"

namespace Power {

void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      LOGLN("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      LOGLN("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      LOGLN("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      LOGLN("Wakeup caused by touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      LOGLN("Wakeup caused by ULP program");
      break;
    default:
      LOGF("Wakeup was not caused by sleep: %d\n", wakeup_reason);
      break;
  }
}

void lightSleep(uint64_t timeout_ms, uint8_t wakeup_pin, int level) {
#if !AllowSleep
  return;
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeup_pin == UINT8_MAX && timeout_ms == 0) {
    timeout_ms = 1000;
  }
  if (wakeup_pin != UINT8_MAX) esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeup_pin, level);
  if (timeout_ms > 0) esp_sleep_enable_timer_wakeup(timeout_ms * 1000);  // 100 ms

  LOGLN("light sleep...");
  if (ESP_OK == esp_light_sleep_start()) {
    printWakeupReason();
  } else {
    LOGLN("Error going to light sleep");
  }
}

void shutdown(DeviceState currentState, uint8_t wakeup_pin, int level, bool force) {
#if !AllowSleep
  if (!force) return;
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wakeup_pin != UINT8_MAX) esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeup_pin, level);
  shutdownDeviceState = currentState;
  LOGLN("Going to deep sleep...");
  esp_deep_sleep_start();
}

}  // namespace Power
