#include "timezone_select.h"

#include <cstring>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::timezone_select {

static const char *const TAG = "timezone_select";

// Version of the stored choice (an FNV-1 hash of the option name).
static const uint32_t PREF_VERSION = 1;

static time::DSTRule make_rule(int32_t time_seconds, uint16_t day, uint8_t type, uint8_t month, uint8_t week,
                               uint8_t day_of_week) {
  time::DSTRule rule{};
  rule.time_seconds = time_seconds;
  rule.day = day;
  rule.type = static_cast<time::DSTRuleType>(type);
  rule.month = month;
  rule.week = week;
  rule.day_of_week = day_of_week;
  return rule;
}

void TimezoneSelect::add_zone(const char *posix, int32_t std_offset, int32_t dst_offset, int32_t start_time,
                              uint16_t start_day, uint8_t start_type, uint8_t start_month, uint8_t start_week,
                              uint8_t start_dow, int32_t end_time, uint16_t end_day, uint8_t end_type,
                              uint8_t end_month, uint8_t end_week, uint8_t end_dow) {
  Zone zone{};
  zone.posix = posix;
  zone.tz.std_offset_seconds = std_offset;
  zone.tz.dst_offset_seconds = dst_offset;
  zone.tz.dst_start = make_rule(start_time, start_day, start_type, start_month, start_week, start_dow);
  zone.tz.dst_end = make_rule(end_time, end_day, end_type, end_month, end_week, end_dow);
  this->zones_.push_back(zone);
}

void TimezoneSelect::setup() {
  size_t index = this->initial_index_;
  this->pref_ = this->make_entity_preference<uint32_t>(PREF_VERSION, true);
  uint32_t saved = 0;
  if (this->restore_value_ && this->pref_.load(&saved)) {
    bool found = false;
    for (size_t i = 0; i < this->zones_.size(); i++) {
      if (fnv1_hash(this->option_at(i)) == saved) {
        index = i;
        found = true;
        break;
      }
    }
    if (!found)
      ESP_LOGW(TAG, "Saved timezone is no longer listed; using %s", this->option_at(index));
  }
  this->apply_(index, false);

  if (this->clock_ != nullptr) {
    // A Home Assistant time sync can set Home Assistant's timezone right after
    // this callback runs, so apply the choice again once it has.
    this->clock_->add_on_time_sync_callback([this]() { this->defer([this]() { this->apply_(this->current_, false); }); });
  }
}

void TimezoneSelect::control(size_t index) {
  if (index >= this->zones_.size())
    return;
  this->apply_(index, true);
  if (this->restore_value_) {
    uint32_t hash = fnv1_hash(this->option_at(index));
    this->pref_.save(&hash);
  }
}

void TimezoneSelect::apply_(size_t index, bool log_change) {
  if (index >= this->zones_.size())
    return;
  const Zone &zone = this->zones_[index];
  bool changed = memcmp(&time::get_global_tz(), &zone.tz, sizeof(zone.tz)) != 0;
  this->current_ = index;
  time::set_global_tz(zone.tz);
  if (log_change || changed) {
    char local[24] = "unknown";
    if (this->clock_ != nullptr) {
      ESPTime now = this->clock_->now();
      if (now.is_valid())
        now.strftime(local, sizeof(local), "%Y-%m-%d %H:%M");
    }
    ESP_LOGI(TAG, "Timezone %s (%s); local time %s", this->option_at(index), zone.posix, local);
  }
  this->publish_state(index);
}

void TimezoneSelect::dump_config() {
  LOG_SELECT("", "Timezone Select", this);
  ESP_LOGCONFIG(TAG,
                "  Timezone      : %s (%s)\n"
                "  Options       : %u\n"
                "  Restore value : %s",
                this->option_at(this->current_), this->zones_[this->current_].posix, (unsigned) this->zones_.size(),
                YESNO(this->restore_value_));
}

}  // namespace esphome::timezone_select
