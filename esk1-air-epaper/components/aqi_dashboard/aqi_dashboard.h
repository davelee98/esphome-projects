#pragma once

// The AQI dashboard component (docs/aqi-dashboard-specification.md): binds
// the sensors and the clock to the value model, runs the refresh scheduler
// once a second, and draws the frozen snapshot through an opendisplay display
// when the scheduler says so. The page, model and policy are plain C++ in
// dashboard_*.h (tested on a host); this file is the ESPHome glue.
//
//   sensor callbacks -> Model (latest)          never draws or connects
//   1 s tick         -> Scheduler.tick()        decides; freezes in_flight
//                    -> OpenDisplay.request_upload(force)
//   driver writer    -> draw_dashboard(in_flight)
//   driver callback  -> Scheduler.on_complete() promotes in_flight on success

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"

#include "dashboard_model.h"
#include "dashboard_render.h"
#include "dashboard_scheduler.h"
#include "esphome/components/opendisplay/opendisplay.h"

#ifdef USE_ESP32

#include <string>

namespace esphome::aqi_dashboard {

// The driver this dashboard draws through (components/opendisplay).
using opendisplay::OpenDisplay;
using opendisplay::UploadStart;
namespace od = opendisplay::od;

class AqiDashboard : public Component {
 public:
  void set_display(OpenDisplay *display);
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
  // The sen6x component, to tell a missing or failing sensor (shown in the
  // header) from one still warming up.
  void set_sensor_component(Component *component) { this->sensor_component_ = component; }
  void set_title(const std::string &title) { this->title_ = title; }
  void set_sensor(Metric metric, sensor::Sensor *s) {
    this->sensors_[metric_index(metric)] = s;
    if (metric == Metric::CO2)
      this->model_config_.co2_configured = s != nullptr;
  }
  // Scalar setters for code generation (__init__.py).
  void set_fahrenheit(bool f) { this->model_config_.fahrenheit = f; }
  // classifier: PM2_5, VOC, NOX or CO2.
  void set_bands(Metric classifier, bool enabled, float enter_yellow, float enter_red, float leave_yellow,
                 float leave_red) {
    Bands b{enabled, enter_yellow, enter_red, leave_yellow, leave_red};
    switch (classifier) {
      case Metric::PM2_5:
        this->model_config_.pm2_5 = b;
        break;
      case Metric::VOC:
        this->model_config_.voc = b;
        break;
      case Metric::NOX:
        this->model_config_.nox = b;
        break;
      case Metric::CO2:
        this->model_config_.co2 = b;
        break;
      default:
        break;
    }
  }
  void set_timing(uint32_t routine_ms, uint32_t heartbeat_ms, uint32_t guard_ms, uint32_t persistence_ms,
                  uint32_t stale_after_ms, uint32_t startup_wait_ms) {
    this->timing_.routine_ms = routine_ms;
    this->timing_.heartbeat_ms = heartbeat_ms;
    this->timing_.guard_ms = guard_ms;
    this->timing_.startup_wait_ms = startup_wait_ms;
    this->model_config_.persistence_ms = persistence_ms;
    this->model_config_.stale_after_ms = stale_after_ms;
  }
  void set_deadbands(float pm_absolute, float pm_relative, float co2, float voc, float nox, float temperature,
                     float humidity) {
    this->deadbands_ = {pm_absolute, pm_relative, co2, voc, nox, temperature, humidity};
  }

  // Refresh E-Paper: bypasses deadbands and the routine deadline, but still
  // waits for the physical-refresh guard and any running upload.
  void request_manual();

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void tick_();
  void on_sample_(Metric metric, float value);
  void on_complete_(od::Result result, bool has_marker, uint32_t marker_ms);
  void build_latest_(uint32_t now_ms);
  void format_stamp_(char *out, size_t size) const;
  // "SENSOR MISSING", "SENSOR ERROR" or nullptr.
  const char *sensor_error_(uint32_t now_ms) const;
  void draw_(display::Display &it);
  static void put_pixel_(void *context, int x, int y, Ink ink);

  OpenDisplay *display_{nullptr};
  time::RealTimeClock *clock_{nullptr};
  Component *sensor_component_{nullptr};
  uint32_t boot_ms_{0};
  const char *last_error_{nullptr};
  sensor::Sensor *sensors_[METRIC_COUNT]{};
  std::string title_{"INDOOR AIR"};
  ModelConfig model_config_;
  Timing timing_;
  Deadbands deadbands_;

  Model model_;
  Scheduler scheduler_;
  Snapshot latest_{};

  // Newest SEN65 batch: wall-clock epoch at arrival, 0 if the clock was not set.
  bool sample_seen_{false};
  int64_t sample_epoch_{0};

  // For logging changes only.
  uint8_t last_reasons_{0};
  const char *last_waiting_{nullptr};
};

}  // namespace esphome::aqi_dashboard

#endif  // USE_ESP32
