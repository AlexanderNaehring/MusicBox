#ifndef MUSICBOX_BATTERY_H
#define MUSICBOX_BATTERY_H

#include <cstdint>
#include <functional>

// Normal: nothing to report. Low: past the warning threshold, recovers with
// hysteresis once the charge rises again. Critical: past the point where
// continuing to draw power risks a brownout or over-discharge - sticky, does
// not recover, since the only sane response is to shut down.
enum class BatteryLevel { Normal, Low, Critical };

// Reads battery charge from a 1:2 voltage divider on ADC1 channel 0 (GPIO36).
// Only HW_REV 2 has this circuit; on other revisions begin()/tick() are
// no-ops and level() always reports Normal, so callers never need to check
// HW_REV themselves.
class BatteryMonitor {
 public:
  void begin();
  // Call once per loop()
  void tick();

  BatteryLevel level() const { return level_; }

  // Registers the single handler invoked when level() changes
  using LevelChangeHandler = std::function<void(BatteryLevel)>;
  void onLevelChange(LevelChangeHandler handler) { onLevelChange_ = handler; }

 private:
  // Only defined when HW_REV == 2. Computes the percentage from an averaged
  // ADC reading, logs/reports it, and updates level_ (with hysteresis),
  // firing onLevelChange_ if it changed.
  void reportPercent(uint32_t adcReading);

  BatteryLevel level_ = BatteryLevel::Normal;
  LevelChangeHandler onLevelChange_;
};

extern BatteryMonitor battery;

#endif
