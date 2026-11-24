// Refresh policy over simulated hours (plan 5.2, spec section 9).

#include <cmath>

#include "../../components/aqi_dashboard/dashboard_model.h"
#include "../../components/aqi_dashboard/dashboard_scheduler.h"
#include "test_util.h"

using namespace esphome::aqi_dashboard;

namespace {

const uint32_t S = 1000, MIN = 60 * S;

// Sensor values over time, the model, the scheduler and a fake transport.
struct Sim {
  Model model;
  Scheduler sched;
  uint32_t now{10 * S};
  float pm25{8}, pm10{12}, pm1{5}, voc{100}, nox{1}, temp{21.0f}, hum{45};
  bool sensor_alive{true};
  TestPattern pattern{TestPattern::NONE};  // applied to every snapshot, as the component does
  // Transport: an upload completes after `upload_ms` with `next_outcome`.
  bool busy{false};
  uint32_t upload_done_at{0};
  uint32_t upload_ms{40 * S};
  Outcome next_outcome{Outcome::SUCCESS};
  bool marker{true};
  uint32_t marker_at{0};
  std::vector<uint32_t> uploads;  // start times
  std::vector<uint8_t> reasons;
  std::vector<bool> forced;
  Snapshot drawn_during_upload{};
  bool in_flight_changed{false};

  Sim() {
    model.configure(ModelConfig{});
    sched.configure(Timing{}, Deadbands{});
    sched.start(now);
  }
  void sample() {
    if (!sensor_alive)
      return;
    model.on_sample(Metric::PM2_5, pm25, now);
    model.on_sample(Metric::PM10, pm10, now);
    model.on_sample(Metric::PM1, pm1, now);
    model.on_sample(Metric::VOC, voc, now);
    model.on_sample(Metric::NOX, nox, now);
    model.on_sample(Metric::TEMP, temp, now);
    model.on_sample(Metric::HUM, hum, now);
  }
  void tick() {
    if (busy && now >= upload_done_at) {
      busy = false;
      sched.on_complete(next_outcome, now, marker, marker_at);
    }
    Snapshot latest{};
    model.build(now, &latest);
    apply_test_pattern(pattern, &latest);
    if (busy && memcmp(&sched.in_flight(), &drawn_during_upload, sizeof(Snapshot)) != 0)
      in_flight_changed = true;
    Decision d = sched.tick(now, latest, model.sen65_seen(), busy);
    if (d.upload) {
      uploads.push_back(now);
      reasons.push_back(d.reasons);
      forced.push_back(d.force);
      busy = true;
      upload_done_at = now + upload_ms;
      marker_at = now + upload_ms - 5 * S;  // last chunk handed off
      drawn_during_upload = sched.in_flight();
    }
  }
  // Runs for `ms`, sampling every 60 s and ticking every second.
  void run(uint32_t ms) {
    uint32_t end = now + ms;
    while (now < end) {
      if ((now / S) % 60 == 0)
        sample();
      tick();
      now += S;
    }
  }
  size_t count_since(uint32_t t) const {
    size_t n = 0;
    for (uint32_t u : uploads)
      if (u >= t)
        n++;
    return n;
  }
};

}  // namespace

TEST(sched_boot_then_stable_only_heartbeat) {
  Sim s;
  s.run(2 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);  // the first SEN65 batch
  CHECK(s.reasons[0] & REASON_BOOT);
  uint32_t t0 = s.now;
  s.run(59 * MIN);  // no five-minute refreshes with stable air
  CHECK_EQ(s.count_since(t0), 0u);
  s.run(3 * MIN);
  CHECK_EQ(s.count_since(t0), 1u);  // one heartbeat at the hour
  CHECK(s.reasons.back() & REASON_HEARTBEAT);
  CHECK(s.forced.back());
}

TEST(sched_startup_wait_without_sensor) {
  Sim s;
  s.sensor_alive = false;
  s.run(179 * S);
  CHECK_EQ(s.uploads.size(), 0u);
  s.run(5 * S);
  CHECK_EQ(s.uploads.size(), 1u);  // one starting page
  s.run(10 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);  // placeholders are not repainted
  s.sensor_alive = true;
  s.run(20 * MIN);
  CHECK_EQ(s.uploads.size(), 2u);  // first batch: a transition, after the 15 min guard
  CHECK(s.reasons.back() & REASON_TRANSITION);
  CHECK(s.uploads[1] - s.uploads[0] >= 15 * MIN - 5 * S);
}

TEST(sched_small_jitter_then_accumulated_change) {
  Sim s;
  s.run(2 * MIN);
  uint32_t t0 = s.now;
  for (int i = 0; i < 25; i++) {  // 0.1 C and 1 % RH jitter for 25 minutes
    s.temp = 21.0f + ((i % 2) ? 0.1f : 0.0f);
    s.hum = 45 + (i % 2);
    s.run(MIN);
  }
  CHECK_EQ(s.count_since(t0), 0u);
  s.temp = 21.6f;  // accumulated 0.6 C
  s.run(10 * MIN);
  CHECK_EQ(s.count_since(t0), 1u);  // eligible at the 30 min routine deadline
  CHECK(s.reasons.back() & REASON_NUMERIC);
  CHECK(!s.forced.back());
  CHECK(s.uploads.back() - s.uploads[0] >= 30 * MIN);
}

TEST(sched_excursion_that_returns_clears) {
  Sim s;
  s.run(2 * MIN);
  uint32_t t0 = s.now;
  s.hum = 52;  // over the 3-point deadband...
  s.run(10 * MIN);
  s.hum = 45;  // ...but back before the routine deadline
  s.run(30 * MIN);
  CHECK_EQ(s.count_since(t0), 0u);
}

TEST(sched_pm_deadband_relative_and_zero) {
  Sim s;
  s.pm25 = 0;
  s.pm10 = 0;
  s.pm1 = 0;
  s.run(2 * MIN);
  uint32_t t0 = s.now;
  s.pm10 = 1;  // below the 2 ug/m3 absolute minimum
  s.run(40 * MIN);
  CHECK_EQ(s.count_since(t0), 0u);
  s.pm10 = 2;  // equality meets the deadband
  s.run(MIN);
  CHECK_EQ(s.count_since(t0), 1u);
  Sim r;
  r.pm10 = 100;
  r.run(2 * MIN);
  uint32_t t1 = r.now;
  r.pm10 = 115;  // 15 < 20 % of 100
  r.run(40 * MIN);
  CHECK_EQ(r.count_since(t1), 0u);
}

TEST(sched_transition_early_but_guarded) {
  Sim s;
  s.run(2 * MIN);
  uint32_t first = s.uploads[0];
  s.pm25 = 20;  // elevated: committed after 3 min of samples
  s.run(4 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);  // still inside the 15 min guard
  s.run(15 * MIN);
  CHECK_EQ(s.uploads.size(), 2u);
  CHECK(s.reasons.back() & REASON_TRANSITION);
  CHECK(s.uploads[1] - (first + s.upload_ms - 5 * S) >= 15 * MIN);
}

TEST(sched_lost_ack_still_guarded) {
  Sim s;
  s.next_outcome = Outcome::FAILED_UNCERTAIN;  // refresh may have happened, no success seen
  s.run(2 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);
  s.next_outcome = Outcome::SUCCESS;
  s.run(10 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);  // retry waits for the guard, not just the 2 min backoff
  s.run(10 * MIN);
  CHECK_EQ(s.uploads.size(), 2u);
  CHECK(s.reasons.back() & REASON_RETRY);
}

TEST(sched_failure_backoff_no_burst) {
  Sim s;
  s.next_outcome = Outcome::FAILED_NOT_SENT;
  s.marker = false;
  s.upload_ms = 2 * S;
  s.run(60 * MIN);
  // 2, 5, then 15 minute gaps: no reconnect storm.
  CHECK(s.uploads.size() >= 4 && s.uploads.size() <= 7);
  for (size_t i = 1; i < s.uploads.size(); i++)
    CHECK(s.uploads[i] - s.uploads[i - 1] >= 2 * MIN);
  CHECK(!s.sched.has_displayed());  // failures never promote
}

TEST(sched_in_flight_frozen_and_requests_merge) {
  Sim s;
  s.upload_ms = 90 * S;
  s.run(61 * S);  // boot upload starts on the first sample
  CHECK_EQ(s.uploads.size(), 1u);
  CHECK(s.busy);
  s.temp = 30;  // samples arrive during the upload
  s.sched.request_manual();
  s.sched.request_manual();
  s.run(80 * S);
  CHECK(!s.in_flight_changed);
  CHECK_EQ(s.uploads.size(), 1u);
}

TEST(sched_manual_waits_for_guard_and_is_forced) {
  Sim s;
  s.run(2 * MIN);
  s.sched.request_manual();
  s.run(5 * MIN);
  CHECK_EQ(s.uploads.size(), 1u);  // inside the guard
  s.run(10 * MIN);
  CHECK_EQ(s.uploads.size(), 2u);
  CHECK(s.reasons.back() & REASON_MANUAL);
  CHECK(s.forced.back());
}

TEST(sched_test_page_skips_guard_once) {
  Sim s;
  s.run(2 * MIN);  // boot page; its guard runs to about 17 min
  CHECK_EQ(s.uploads.size(), 1u);
  s.pattern = TestPattern::HIGH;
  s.sched.request_test();
  s.run(2 * S);
  CHECK_EQ(s.uploads.size(), 2u);  // at once, inside the guard
  CHECK(s.reasons.back() & REASON_MANUAL);
  CHECK(s.forced.back());
  s.run(1 * MIN);  // the test page completes and restarts the guard
  s.pattern = TestPattern::NONE;  // shown once: the component drops it
  // The bypass is spent: the revert page waits the full guard again.
  s.run(10 * MIN);
  CHECK_EQ(s.uploads.size(), 2u);
  s.run(6 * MIN);
  CHECK_EQ(s.uploads.size(), 3u);
}

TEST(sched_test_page_bypass_survives_unsent_failure) {
  Sim s;
  s.run(2 * MIN);
  s.next_outcome = Outcome::FAILED_NOT_SENT;
  s.marker = false;  // nothing reached the panel
  s.pattern = TestPattern::MIXED;  // kept until shown, so the retry has a reason
  s.sched.request_test();
  s.run(1 * MIN);  // the 40 s upload fails
  CHECK_EQ(s.uploads.size(), 2u);
  s.next_outcome = Outcome::SUCCESS;
  s.marker = true;
  s.run(2 * MIN);  // the 2 min backoff; still inside the boot page's guard
  CHECK_EQ(s.uploads.size(), 3u);  // the retry kept the bypass
  CHECK(s.reasons.back() & REASON_RETRY);
}

TEST(sched_sensor_goes_stale_without_callback) {
  Sim s;
  s.run(20 * MIN);  // boot + guard expired
  size_t n = s.uploads.size();
  s.sensor_alive = false;
  s.run(4 * MIN);  // > 180 s without samples
  CHECK_EQ(s.uploads.size(), n + 1);  // availability transition
  CHECK(s.reasons.back() & REASON_TRANSITION);
  CHECK(s.sched.in_flight().cells[metric_index(Metric::TEMP)].state == CellState::FAULT);
}

TEST(sched_sensor_error_is_a_transition) {
  Sim s;
  s.run(20 * MIN);
  size_t n = s.uploads.size();
  // Same cells, but the header now carries a sensor error: refresh (guarded).
  Snapshot latest{};
  s.model.build(s.now, &latest);
  latest.stamp_alert = true;
  CHECK(s.sched.state_change(latest));
  Decision d = s.sched.tick(s.now, latest, true, false);
  CHECK(d.upload);
  CHECK(d.reasons & REASON_TRANSITION);
  (void) n;
}

TEST(sched_unchanged_counts_as_displayed) {
  Sim s;
  s.next_outcome = Outcome::UNCHANGED;
  s.marker = false;
  s.run(2 * MIN);
  CHECK(s.sched.has_displayed());
}
