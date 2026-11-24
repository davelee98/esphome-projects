#pragma once

// A select that sets the device's timezone at run time.
//
// The device cannot parse a timezone string itself (ESPHome removed the
// on-device POSIX TZ parser), so every option is parsed at compile time and
// stored here as a time::ParsedTimezone. Choosing an option applies it with
// time::set_global_tz(), which every time::RealTimeClock::now() then uses.
//
// The choice is kept in flash by option name, so adding or reordering zones in
// the YAML does not change it; a name no longer listed falls back to
// initial_option.
//
// Home Assistant time sends Home Assistant's timezone with each sync when its
// time platform has no timezone of its own. The choice is applied again after
// every sync so it is not overridden.

#include "esphome/components/select/select.h"
#include "esphome/components/time/posix_tz.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <vector>

namespace esphome::timezone_select {

class TimezoneSelect : public select::Select, public Component {
 public:
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  void set_initial_index(size_t index) { this->initial_index_ = index; }
  void set_restore_value(bool restore) { this->restore_value_ = restore; }
  // One option, in option order: its POSIX string (for the log) and the
  // fields of its time::ParsedTimezone.
  void add_zone(const char *posix, int32_t std_offset, int32_t dst_offset, int32_t start_time, uint16_t start_day,
                uint8_t start_type, uint8_t start_month, uint8_t start_week, uint8_t start_dow, int32_t end_time,
                uint16_t end_day, uint8_t end_type, uint8_t end_month, uint8_t end_week, uint8_t end_dow);

  void setup() override;
  void dump_config() override;
  // Before the clocks and anything that formats local time.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void control(size_t index) override;
  void apply_(size_t index, bool log_change);

  struct Zone {
    const char *posix;
    time::ParsedTimezone tz;
  };

  time::RealTimeClock *clock_{nullptr};
  std::vector<Zone> zones_;
  size_t initial_index_{0};
  size_t current_{0};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::timezone_select
