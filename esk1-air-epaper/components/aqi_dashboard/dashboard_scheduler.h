#pragma once

// Refresh policy for the AQI dashboard (docs/aqi-dashboard-specification.md
// section 5). Pure logic: it is given the time and the latest snapshot, and
// says when to upload and whether to force past the unchanged-frame check.
// It never touches BLE, sensors or pixels.
//
// Three snapshots (section 5.3): `latest` is built fresh by the caller each
// tick; `in_flight` is frozen here when an upload is decided; `displayed` is
// what the label confirmed. Ages for the routine and heartbeat refreshes run
// from confirmed success; the physical-refresh guard runs from the moment a
// refresh could have begun.

#include <cstdint>

#include "dashboard_model.h"

namespace esphome::aqi_dashboard {

struct Timing {
  uint32_t routine_ms{30u * 60u * 1000u};    // numeric changes wait this long after a success
  uint32_t heartbeat_ms{60u * 60u * 1000u};  // refresh even if stable
  uint32_t guard_ms{15u * 60u * 1000u};      // between possible panel-refresh starts
  uint32_t startup_wait_ms{180u * 1000u};    // first page: wait this long for the SEN65
  uint32_t retry_ms[3]{2u * 60u * 1000u, 5u * 60u * 1000u, 15u * 60u * 1000u};
};

struct Deadbands {
  float pm_absolute{2.0f};
  float pm_relative{0.20f};  // of abs(displayed value)
  float co2{100.0f};
  float voc{25.0f};
  float nox{10.0f};
  float temperature{0.5f};   // degrees C (0.9 F)
  float humidity{3.0f};
};

// Why an upload was requested; several can be merged.
enum Reason : uint8_t {
  REASON_NONE = 0,
  REASON_BOOT = 1u << 0,        // first page after start-up
  REASON_TRANSITION = 1u << 1,  // a cell's state changed (category, availability, overflow)
  REASON_NUMERIC = 1u << 2,     // a meaningful numeric change, routine deadline passed
  REASON_HEARTBEAT = 1u << 3,   // nothing sent successfully for heartbeat_ms
  REASON_MANUAL = 1u << 4,      // Refresh button
  REASON_RETRY = 1u << 5,       // retrying after a failed attempt
};

enum class Outcome : uint8_t {
  SUCCESS,           // the label confirmed the refresh
  UNCHANGED,         // identical to the frame on the label; nothing sent
  FAILED_NOT_SENT,   // failed before anything could trigger a refresh
  FAILED_UNCERTAIN,  // failed after a refresh could have begun
};

struct Decision {
  bool upload{false};
  bool force{false};  // upload even if the pixels match the last frame sent
  uint8_t reasons{REASON_NONE};
  // Why nothing happens although something is pending (for logs).
  const char *waiting_for{nullptr};
};

class Scheduler {
 public:
  void configure(const Timing &timing, const Deadbands &deadbands);
  void start(uint32_t now_ms);  // boot time

  void request_manual() { this->manual_pending_ = true; }

  // Once per tick, with the latest snapshot. If the result says upload, the
  // snapshot is frozen as in_flight() and must be what gets drawn.
  Decision tick(uint32_t now_ms, const Snapshot &latest, bool sen65_seen, bool transport_busy);

  // The upload's result. refresh_marker_ms, when has_marker, is when the
  // panel could first have started refreshing (it starts the guard).
  void on_complete(Outcome outcome, uint32_t now_ms, bool has_marker, uint32_t refresh_marker_ms);

  bool busy() const { return this->in_flight_active_; }
  const Snapshot &in_flight() const { return this->in_flight_; }
  bool has_displayed() const { return this->has_displayed_; }
  const Snapshot &displayed() const { return this->displayed_; }

  // The checks, public for tests and logs.
  bool numeric_change(const Snapshot &latest) const;
  bool state_change(const Snapshot &latest) const;

 protected:
  static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
    return static_cast<uint32_t>(now - since) >= interval;
  }
  float deadband_(Metric m, float displayed_raw) const;

  Timing timing_;
  Deadbands deadbands_;

  uint32_t boot_ms_{0};
  bool started_{false};

  bool has_displayed_{false};
  Snapshot displayed_{};
  bool has_success_{false};
  uint32_t last_success_ms_{0};

  bool in_flight_active_{false};
  Snapshot in_flight_{};
  uint8_t in_flight_reasons_{REASON_NONE};

  bool has_guard_{false};
  uint32_t guard_start_ms_{0};

  uint8_t failures_{0};
  bool has_retry_{false};
  uint32_t retry_after_ms_{0};
  uint32_t retry_from_ms_{0};

  bool manual_pending_{false};
};

}  // namespace esphome::aqi_dashboard
