#include "Battery.h"

#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "Config.h"

#if BLE
#include "MusicBoxBLE.h"
#endif

#define LOG_TAG "Battery"

#define BATTERY_SAMPLES 10
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_0  // GPIO36

BatteryMonitor battery;

static esp_adc_cal_characteristics_t* adcChars;

void BatteryMonitor::begin() {
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_12);
  adcChars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, adcChars);

  readPercent();
}

float BatteryMonitor::readPercent() {
  uint32_t adcReading = 0;

  // Take multiple samples and average
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    adcReading += adc1_get_raw(BATTERY_ADC_CHANNEL);
  }
  adcReading /= BATTERY_SAMPLES;
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
  return pct;
}
