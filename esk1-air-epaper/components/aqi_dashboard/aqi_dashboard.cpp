#include "aqi_dashboard.h"

#ifdef USE_ESP32

#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::aqi_dashboard {

static const char *const TAG = "aqi_dashboard";


static void reasons_text(uint8_t r, char *out, size_t size) {
  static const struct {
    uint8_t bit;
    const char *name;
  } NAMES[] = {{REASON_BOOT, "boot"},         {REASON_TRANSITION, "transition"},
               {REASON_NUMERIC, "numeric"},   {REASON_HEARTBEAT, "heartbeat"},
               {REASON_MANUAL, "manual"},     {REASON_RETRY, "retry"}};
  out[0] = '\0';
  for (const auto &n : NAMES) {
    if (!(r & n.bit))
      continue;
    if (out[0] != '\0')
      strncat(out, "+", size - strlen(out) - 1);
    strncat(out, n.name, size - strlen(out) - 1);
  }
  if (out[0] == '\0')
    snprintf(out, size, "none");
}

void AqiDashboard::set_display(OpenDisplay *display) {
  this->display_ = display;
  // The fixed page is exactly 400 x 300 BWRY; the driver refuses any other
  // label rather than stretching or clipping it.
  display->set_required_panel(CANVAS_WIDTH, CANVAS_HEIGHT, opendisplay::COLOR_SCHEME_BWRY);
  // The scheduler decides; update() / component.update becomes a manual request.
  display->set_external_scheduling([this]() { this->request_manual(); });
  display->set_writer([this](display::Display &it) { this->draw_(it); });
  display->add_on_complete_callback(
      [this](od::Result result, bool has_marker, uint32_t marker_ms) { this->on_complete_(result, has_marker, marker_ms); });
}

void AqiDashboard::setup() {
  this->model_.configure(this->model_config_);
  this->scheduler_.configure(this->timing_, this->deadbands_);
  this->boot_ms_ = millis();
  this->scheduler_.start(this->boot_ms_);
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    sensor::Sensor *s = this->sensors_[i];
    if (s == nullptr)
      continue;
    Metric m = static_cast<Metric>(i);
    s->add_on_state_callback([this, m](float value) { this->on_sample_(m, value); });
  }
  this->set_interval("tick", 1000, [this]() { this->tick_(); });
}

void AqiDashboard::dump_config() {
  ESP_LOGCONFIG(TAG,
                "AQI dashboard:\n"
                "  Title       : %s\n"
                "  Temperature : %s\n"
                "  CO2         : %s\n"
                "  Routine     : %u s, heartbeat %u s, guard %u s\n"
                "  Persistence : %u s, stale after %u s, startup wait %u s",
                this->title_.c_str(), this->model_config_.fahrenheit ? "F" : "C",
                this->model_config_.co2_configured
                    ? (this->model_config_.co2.enabled ? "bound, classified" : "bound, not classified")
                    : "not bound",
                (unsigned) (this->timing_.routine_ms / 1000), (unsigned) (this->timing_.heartbeat_ms / 1000),
                (unsigned) (this->timing_.guard_ms / 1000), (unsigned) (this->model_config_.persistence_ms / 1000),
                (unsigned) (this->model_config_.stale_after_ms / 1000),
                (unsigned) (this->timing_.startup_wait_ms / 1000));
}

void AqiDashboard::request_manual() {
  ESP_LOGI(TAG, "Manual refresh requested");
  this->scheduler_.request_manual();
}

void AqiDashboard::show_test_pattern(TestPattern pattern) {
  ESP_LOGI(TAG, "Test page '%s' requested: real readings, test colours, for one page", test_pattern_name(pattern));
  this->test_pattern_ = pattern;
  this->scheduler_.request_test();  // skips the refresh guard, once
}

// Called when an upload finished with the page on the label (sent or already
// there): a test page has been shown once, so the next page is the real one.
void AqiDashboard::test_page_shown_() {
  TestPattern shown = this->test_in_flight_;
  this->test_in_flight_ = TestPattern::NONE;
  if (shown == TestPattern::NONE)
    return;
  ESP_LOGI(TAG, "Test page '%s' shown; the next update restores the real colours", test_pattern_name(shown));
  // A different pattern requested during the upload still gets its page.
  if (this->test_pattern_ == shown)
    this->test_pattern_ = TestPattern::NONE;
}

void AqiDashboard::on_sample_(Metric metric, float value) {
  uint32_t now = millis();
  this->model_.on_sample(metric, value, now);
  if (metric != Metric::CO2) {
    // The header stamp is the newest SEN65 batch's wall-clock time, taken when
    // it arrives; with no clock it stays unknown rather than guessed.
    this->sample_seen_ = true;
    int64_t epoch = 0;
    if (this->clock_ != nullptr) {
      ESPTime t = this->clock_->now();
      if (t.is_valid())
        epoch = (int64_t) t.timestamp;
    }
    this->sample_epoch_ = epoch;
  }
}

void AqiDashboard::format_stamp_(char *out, size_t size) const {
  if (!this->sample_seen_) {
    snprintf(out, size, "NO SAMPLE YET");
    return;
  }
  if (this->sample_epoch_ > 0) {
    ESPTime t = ESPTime::from_epoch_local((time_t) this->sample_epoch_);
    if (t.is_valid() && t.strftime(out, size, "%d %b %H:%M") > 0) {
      for (char *c = out; *c; c++)
        if (*c >= 'a' && *c <= 'z')
          *c -= 'a' - 'A';
      return;
    }
  }
  // No clock (no Home Assistant or NTP sync) when the sample arrived.
  snprintf(out, size, "-- --- --:--");
}

// sen6x marks itself failed when set-up cannot reach the sensor (absent at
// boot), and sets a warning when reads fail (unplugged later).
const char *AqiDashboard::sensor_error_(uint32_t now_ms) const {
  const Component *c = this->sensor_component_;
  return sensor_error(c != nullptr && c->is_failed(), c != nullptr && c->status_has_warning(),
                                 this->model_.sen65_fresh(now_ms), this->model_.sen65_seen(),
                                 static_cast<uint32_t>(now_ms - this->boot_ms_) >= this->timing_.startup_wait_ms);
}

void AqiDashboard::build_latest_(uint32_t now_ms) {
  const char *error = this->sensor_error_(now_ms);
  if (error != this->last_error_) {
    if (error != nullptr) {
      ESP_LOGW(TAG, "SEN65 %s: its cells show as faulted until it reports again",
               strcmp(error, "SENSOR MISSING") == 0 ? "missing (not detected, or never reported)"
                                                    : "not responding");
    } else {
      ESP_LOGI(TAG, "SEN65 reporting again");
    }
    this->last_error_ = error;
  }
  this->model_.set_sen65_fault(error != nullptr);
  this->model_.build(now_ms, &this->latest_);
  apply_test_pattern(this->test_pattern_, &this->latest_);
  snprintf(this->latest_.title, sizeof(this->latest_.title), "%s", this->title_.c_str());
  if (error != nullptr) {
    snprintf(this->latest_.stamp, sizeof(this->latest_.stamp), "%s", error);
    this->latest_.stamp_alert = true;
  } else {
    this->format_stamp_(this->latest_.stamp, sizeof(this->latest_.stamp));
  }
}

void AqiDashboard::tick_() {
  uint32_t now = millis();
  this->build_latest_(now);
  // A sensor error counts as "ready" for the first page: send it at once
  // rather than wait out startup_wait.
  bool ready = this->model_.sen65_seen() || this->latest_.stamp_alert;
  Decision d = this->scheduler_.tick(now, this->latest_, ready, this->display_->busy());

  if (d.reasons != this->last_reasons_ || d.waiting_for != this->last_waiting_) {
    if (d.reasons != 0 && !d.upload) {
      char why[64];
      reasons_text(d.reasons, why, sizeof(why));
      ESP_LOGD(TAG, "Update pending (%s), waiting for %s", why, d.waiting_for != nullptr ? d.waiting_for : "?");
    }
    this->last_reasons_ = d.reasons;
    this->last_waiting_ = d.waiting_for;
  }
  if (!d.upload)
    return;

  char why[64];
  reasons_text(d.reasons, why, sizeof(why));
  const Snapshot &s = this->scheduler_.in_flight();
  ESP_LOGI(TAG, "Sending page (%s%s), stamp %s", why, d.force ? ", forced" : "", s.stamp);
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    ESP_LOGD(TAG, "  %-5s %-6s %s", metric_name(static_cast<Metric>(i)), s.cells[i].text,
             state_name(s.cells[i].state));
  }
  this->last_reasons_ = 0;
  this->last_waiting_ = nullptr;
  // The frozen page carries the pattern; a press during this upload applies
  // to the next one instead.
  this->test_in_flight_ = this->test_pattern_;

  UploadStart started = this->display_->request_upload(d.force);
  switch (started) {
    case UploadStart::STARTED:
      break;  // on_complete_() follows
    case UploadStart::UNCHANGED:
      ESP_LOGD(TAG, "Page identical to the one displayed");
      this->scheduler_.on_complete(Outcome::UNCHANGED, millis(), false, 0);
      this->test_page_shown_();
      break;
    default:
      ESP_LOGW(TAG, "Upload could not start (%s)",
               started == UploadStart::BUSY ? "busy" : started == UploadStart::NO_LABEL ? "no label" : "not ready");
      this->scheduler_.on_complete(Outcome::FAILED_NOT_SENT, millis(), false, 0);
      break;
  }
}

void AqiDashboard::on_complete_(od::Result result, bool has_marker, uint32_t marker_ms) {
  Outcome outcome = result == od::Result::SUCCESS          ? Outcome::SUCCESS
                               : result == od::Result::FAILED_UNCERTAIN ? Outcome::FAILED_UNCERTAIN
                                                                        : Outcome::FAILED_NOT_SENT;
  this->scheduler_.on_complete(outcome, millis(), has_marker, marker_ms);
  if (outcome == Outcome::SUCCESS) {
    ESP_LOGI(TAG, "Page displayed");
    this->test_page_shown_();
  }
}

void AqiDashboard::put_pixel_(void *context, int x, int y, Ink ink) {
  static_cast<OpenDisplay *>(context)->put_ink(x, y, ink);
}

// The driver's writer: draws only the frozen in_flight snapshot, so the
// pixels cannot change while a transfer is running or being retried.
void AqiDashboard::draw_(display::Display &it) {
  Raster r{this->display_, &AqiDashboard::put_pixel_, it.get_width(), it.get_height()};
  if (it.get_width() < CANVAS_WIDTH || it.get_height() < CANVAS_HEIGHT) {
    ESP_LOGW(TAG, "Canvas %dx%d is smaller than the 400x300 page; not drawing", it.get_width(), it.get_height());
    return;
  }
  draw_dashboard(r, this->scheduler_.in_flight());
}

}  // namespace esphome::aqi_dashboard

#endif  // USE_ESP32
