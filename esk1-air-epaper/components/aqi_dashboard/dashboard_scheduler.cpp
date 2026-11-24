#include "dashboard_scheduler.h"

#include <cmath>
#include <cstring>

namespace esphome::aqi_dashboard {

void Scheduler::configure(const Timing &timing, const Deadbands &deadbands) {
  this->timing_ = timing;
  this->deadbands_ = deadbands;
}

void Scheduler::start(uint32_t now_ms) {
  this->boot_ms_ = now_ms;
  this->started_ = true;
}

float Scheduler::deadband_(Metric m, float displayed_raw) const {
  const Deadbands &d = this->deadbands_;
  switch (m) {
    case Metric::PM2_5:
    case Metric::PM10:
    case Metric::PM1: {
      float relative = d.pm_relative * std::fabs(displayed_raw);
      return relative > d.pm_absolute ? relative : d.pm_absolute;
    }
    case Metric::CO2:
      return d.co2;
    case Metric::VOC:
      return d.voc;
    case Metric::NOX:
      return d.nox;
    case Metric::TEMP:
      return d.temperature;
    case Metric::HUM:
      return d.humidity;
  }
  return 0;
}

// Against the last *displayed* snapshot, recomputed every time, so a brief
// excursion that comes back leaves nothing pending (section 5.2).
bool Scheduler::numeric_change(const Snapshot &latest) const {
  if (!this->has_displayed_)
    return false;
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    const Cell &now = latest.cells[i], &shown = this->displayed_.cells[i];
    if (!state_has_value(now.state) || !state_has_value(shown.state))
      continue;  // state changes are handled as transitions
    if (strcmp(now.text, shown.text) == 0)
      continue;  // a change hidden by rounding is no change
    float delta = std::fabs(now.raw - shown.raw);
    if (delta >= this->deadband_(static_cast<Metric>(i), shown.raw))
      return true;  // equality meets the deadband
  }
  return false;
}

bool Scheduler::state_change(const Snapshot &latest) const {
  if (!this->has_displayed_)
    return false;
  for (size_t i = 0; i < METRIC_COUNT; i++)
    if (latest.cells[i].state != this->displayed_.cells[i].state)
      return true;
  // A sensor error appearing or clearing is an availability change too.
  return latest.fahrenheit != this->displayed_.fahrenheit || latest.stamp_alert != this->displayed_.stamp_alert;
}

Decision Scheduler::tick(uint32_t now_ms, const Snapshot &latest, bool sen65_seen, bool transport_busy) {
  Decision d;
  if (!this->started_)
    this->start(now_ms);
  if (this->in_flight_active_) {
    d.waiting_for = "upload in progress";
    return d;
  }

  uint8_t reasons = REASON_NONE;
  if (!this->has_displayed_) {
    // Cold boot: send the first SEN65 batch; without one, a single starting
    // page after startup_wait. After that the normal rules apply.
    if (sen65_seen || elapsed(now_ms, this->boot_ms_, this->timing_.startup_wait_ms))
      reasons |= REASON_BOOT;
  } else {
    if (this->state_change(latest))
      reasons |= REASON_TRANSITION;
    if (this->numeric_change(latest) && elapsed(now_ms, this->last_success_ms_, this->timing_.routine_ms))
      reasons |= REASON_NUMERIC;
    if (elapsed(now_ms, this->last_success_ms_, this->timing_.heartbeat_ms))
      reasons |= REASON_HEARTBEAT;
  }
  if (this->manual_pending_)
    reasons |= REASON_MANUAL;
  d.reasons = reasons;
  if (reasons == REASON_NONE)
    return d;

  if (this->has_retry_) {
    if (!elapsed(now_ms, this->retry_from_ms_, this->retry_after_ms_)) {
      d.waiting_for = "retry backoff";
      return d;
    }
    d.reasons |= REASON_RETRY;
  }
  if (transport_busy) {
    d.waiting_for = "transport busy";
    return d;
  }
  if (this->has_guard_ && !this->guard_bypass_ && !elapsed(now_ms, this->guard_start_ms_, this->timing_.guard_ms)) {
    d.waiting_for = "refresh guard";
    return d;
  }

  d.upload = true;
  // Heartbeat and manual must reach the panel even when the pixels are the
  // same (for example with no wall clock, so the stamp cannot move).
  d.force = (reasons & (REASON_HEARTBEAT | REASON_MANUAL)) != 0;
  this->in_flight_ = latest;
  this->in_flight_active_ = true;
  this->in_flight_reasons_ = d.reasons;
  this->manual_pending_ = false;
  return d;
}

void Scheduler::on_complete(Outcome outcome, uint32_t now_ms, bool has_marker, uint32_t refresh_marker_ms) {
  if (!this->in_flight_active_)
    return;
  this->in_flight_active_ = false;
  if (has_marker) {
    // Conservative: the guard runs from when a refresh could have begun,
    // whatever the outcome, so a lost success never leads to another
    // 30-second refresh straight away.
    this->has_guard_ = true;
    this->guard_start_ms_ = refresh_marker_ms;
  }
  // A test page's guard bypass is spent once the panel may have refreshed.
  if (outcome != Outcome::FAILED_NOT_SENT)
    this->guard_bypass_ = false;
  switch (outcome) {
    case Outcome::SUCCESS:
    case Outcome::UNCHANGED:
      this->displayed_ = this->in_flight_;
      this->has_displayed_ = true;
      this->has_success_ = true;
      this->last_success_ms_ = now_ms;
      this->failures_ = 0;
      this->has_retry_ = false;
      break;
    case Outcome::FAILED_NOT_SENT:
    case Outcome::FAILED_UNCERTAIN: {
      // Keep `displayed`; back off 2, 5, then 15 minutes. The next attempt
      // takes a fresh snapshot (tick() copies `latest` again).
      if (this->failures_ < 255)
        this->failures_++;
      size_t step = this->failures_ - 1;
      if (step > 2)
        step = 2;
      this->retry_after_ms_ = this->timing_.retry_ms[step];
      this->retry_from_ms_ = now_ms;
      this->has_retry_ = true;
      break;
    }
  }
}

}  // namespace esphome::aqi_dashboard
