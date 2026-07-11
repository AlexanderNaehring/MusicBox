#ifndef MUSICBOX_BATTERY_H
#define MUSICBOX_BATTERY_H

// Reads battery charge from a 1:2 voltage divider on ADC1 channel 0 (GPIO36).
class BatteryMonitor {
 public:
  void begin();
  // Samples the ADC, logs the voltage, and returns the estimated charge as a
  // percentage (0-100, clamped).
  float readPercent();
};

extern BatteryMonitor battery;

#endif
