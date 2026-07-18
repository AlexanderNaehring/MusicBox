#include "Battery.h"

#include <Arduino.h>

#include "Config.h"

#define LOG_TAG "Battery"

BatteryMonitor battery;

#if HW_REV == 2

#include <driver/adc.h>
#include <esp_adc_cal.h>

#if BLE
#include "MusicBoxBLE.h"
#endif

#define BATTERY_CHECK_INTERVAL 60000  // 60 seconds
#define BATTERY_SAMPLES 50
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_0  // GPIO36

// Below this: warn, but keep playing. Recovers only above
// BATTERY_LOW_RECOVER_PCT, so noise/voltage sag near the threshold doesn't
// flicker the warning on and off.
#define BATTERY_LOW_PCT 15
#define BATTERY_LOW_RECOVER_PCT 20
// Below this: stop drawing power. Sticky - once Critical, only a reboot
// (which re-runs begin()) clears it.
#define BATTERY_CRITICAL_PCT 5

static esp_adc_cal_characteristics_t* adcChars;

static unsigned long lastCheckTime = 0;
static bool sampling = false;
static int sampleCount = 0;
static uint32_t adcAccumulator = 0;

void BatteryMonitor::reportPercent(uint32_t adcReading) {
  // Convert ADC reading to voltage in mV
  uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adcReading, adcChars);
  float voltage = voltage_mv / 1000.0 * 2.0;  // 1:2 voltage divider

  const float voltage_min = 3.2;
  const float voltage_max = 4.2;
  float pct = ((voltage - voltage_min) / (voltage_max - voltage_min)) * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  LOGF("Battery: %.2f V (%.0f%%)\n", voltage, pct);
#if BLE
  bleInfo.setBatteryPct(pct);
#endif

  BatteryLevel newLevel = level_;
  if (level_ == BatteryLevel::Critical) {
    // sticky - no recovery
  } else if (pct <= BATTERY_CRITICAL_PCT) {
    newLevel = BatteryLevel::Critical;
  } else if (pct <= BATTERY_LOW_PCT) {
    newLevel = BatteryLevel::Low;
  } else if (level_ != BatteryLevel::Low || pct >= BATTERY_LOW_RECOVER_PCT) {
    newLevel = BatteryLevel::Normal;
  }
  if (newLevel != level_) {
    level_ = newLevel;
    if (onLevelChange_) onLevelChange_(level_);
  }
}

void BatteryMonitor::begin() {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_12);
  adcChars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, adcChars);

  // perform initial ADC read (blocking setup to get noise-free reading)
  uint32_t adcReading = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    adcReading += adc1_get_raw(BATTERY_ADC_CHANNEL);
  }
  reportPercent(adcReading / BATTERY_SAMPLES);
  lastCheckTime = millis();
}

void BatteryMonitor::tick() {
  unsigned long now = millis();

  if (!sampling) {
    if (now - lastCheckTime < BATTERY_CHECK_INTERVAL) return;
    sampling = true;
    sampleCount = 0;
    adcAccumulator = 0;
  }

  adcAccumulator += adc1_get_raw(BATTERY_ADC_CHANNEL);
  sampleCount++;
  if (sampleCount < BATTERY_SAMPLES) return;

  reportPercent(adcAccumulator / BATTERY_SAMPLES);
  sampling = false;
  lastCheckTime = now;
}

#else  // HW_REV != 2

void BatteryMonitor::begin() {}
void BatteryMonitor::tick() {}

#endif
