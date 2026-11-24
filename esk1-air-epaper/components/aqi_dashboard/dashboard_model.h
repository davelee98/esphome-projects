#pragma once

// Value model for the AQI dashboard (docs/aqi-dashboard-specification.md
// section 4): what each reading is, whether it is usable, how it is formatted
// and which concentration category it is in. Plain C++17, no ESPHome
// dependency. Times are millis()-style uint32_t and compared wrap-safely.

#include <cstddef>
#include <cstdint>

#include "../opendisplay/ink.h"

namespace esphome::aqi_dashboard {

// The panel's inks, defined by the opendisplay driver (opendisplay/ink.h).
using opendisplay::Ink;

enum class Metric : uint8_t { PM2_5, PM10, PM1, CO2, VOC, NOX, TEMP, HUM };
static constexpr size_t METRIC_COUNT = 8;
inline size_t metric_index(Metric m) { return static_cast<size_t>(m); }
const char *metric_name(Metric m);

// How a cell looks (section 4.2). Category, availability and overflow all
// collapse into this one value, so "the page changed state" is a comparison.
enum class CellState : uint8_t {
  NORMAL,       // valid; white
  ELEVATED,     // valid; yellow
  HIGH,         // valid; red, white text
  PLACEHOLDER,  // never sampled, or optional source not configured; white "--"
  FAULT,        // configured source stale, invalid or faulted; yellow "--"
  OVERFLOW,     // valid but too wide for the 32 px field; yellow "----"
};
bool state_has_value(CellState s);  // NORMAL, ELEVATED or HIGH
const char *state_name(CellState s);

enum class Category : uint8_t { NORMAL, ELEVATED, HIGH };

struct Cell {
  CellState state;
  float raw;      // the reading behind the text (valid states only), unformatted
  char text[16];  // what is drawn: the formatted value, "--" or "----"
};

// Everything one page shows: plain values, so it can be copied, frozen for an
// upload and compared with what was last displayed (section 5.3).
struct Snapshot {
  Cell cells[METRIC_COUNT];  // indexed by Metric
  char title[80];            // UTF-8, 16 glyphs at most (validated in display.py)
  char stamp[24];            // frozen header stamp
  bool stamp_alert;          // the stamp is a sensor error ("SENSOR MISSING" / "SENSOR ERROR"), drawn red
  bool fahrenheit;
};

// Test pages (the Test Mixed / Test High buttons): the real readings drawn
// with a fixed pattern of cell colours, for checking the inks on the panel.
//   MIXED: PM2.5, PM10, PM1 red; VOC, NOx, HUM yellow; CO2, TEMP white.
//   HIGH:  PM2.5, PM10, PM1, VOC red; NOx yellow; CO2, TEMP, HUM white.
// Only cells holding a value are recoloured; "--" and "----" cells keep their
// real state, so a test page never shows a missing reading as a measurement.
enum class TestPattern : uint8_t { NONE, MIXED, HIGH };
const char *test_pattern_name(TestPattern p);
void apply_test_pattern(TestPattern p, Snapshot *s);

// Event bands (section 4.3). Entry and exit thresholds give hysteresis.
struct Bands {
  bool enabled;
  float enter_yellow, enter_red, leave_yellow, leave_red;
};

struct ModelConfig {
  Bands pm2_5{true, 15, 35, 12, 30};
  Bands voc{true, 150, 400, 130, 350};
  Bands nox{true, 20, 300, 15, 250};
  Bands co2{false, 1000, 1500, 900, 1300};  // disabled until configured
  bool co2_configured{false};
  bool fahrenheit{false};
  uint32_t stale_after_ms{180000};
  uint32_t persistence_ms{180000};
};

// Category with hysteresis from the committed category (section 4.3).
Category classify(Category current, float value, const Bands &bands);

// Formats a valid reading for display (section 4.1). Temperature is given in
// degrees C and converted first if `fahrenheit`. Returns false if the result
// does not fit `size` (to be shown as overflow, never clipped).
bool format_value(Metric m, float raw, bool fahrenheit, char *out, size_t size);
// Source validity before formatting: finite and inside the metric's range.
bool value_in_range(Metric m, float raw);

// The header's sensor error, or nullptr:
//   failed                    -> "SENSOR MISSING" (sen6x failed set-up: not on I2C)
//   no current reading, and read errors, or nothing at all by startup_wait
//                             -> "SENSOR ERROR" if it ever reported, else "SENSOR MISSING"
// Warming up, or stale without read errors, is not an error.
const char *sensor_error(bool failed, bool read_errors, bool has_fresh_reading, bool ever_reported,
                         bool startup_wait_over);

class Model {
 public:
  void configure(const ModelConfig &config);
  const ModelConfig &config() const { return this->config_; }

  // A new reading (NaN = the sensor reported "not available").
  void on_sample(Metric m, float value, uint32_t now_ms);
  // True once any SEN65 reading has arrived.
  bool sen65_seen() const { return this->sen65_seen_; }
  // True if any SEN65 metric has a valid reading that is not stale.
  bool sen65_fresh(uint32_t now_ms) const;
  // The SEN65 itself is missing or failing: cells without a usable reading
  // show as faulted (yellow "--") rather than still starting (white "--").
  void set_sen65_fault(bool fault) { this->sen65_fault_ = fault; }
  bool sen65_fault() const { return this->sen65_fault_; }

  // The page as it would be drawn now (stamp and title are filled in by the
  // caller). Applies staleness against now_ms without waiting for a callback.
  void build(uint32_t now_ms, Snapshot *out) const;

  // For logs and tests.
  CellState state(Metric m, uint32_t now_ms) const;
  Category committed(Metric classifier) const;

 protected:
  struct Source {
    bool seen_valid{false};   // a valid reading has ever arrived
    bool valid{false};        // the last reading was valid
    float raw{0};
    uint32_t sample_ms{0};
  };
  struct Classifier {
    bool committed_valid{false};
    Category committed{Category::NORMAL};
    bool has_candidate{false};
    Category candidate{Category::NORMAL};
    uint32_t candidate_since_ms{0};
  };

  bool fresh_(Metric m, uint32_t now_ms) const;
  const Bands *bands_for_(Metric m) const;
  Classifier *classifier_for_(Metric m);
  const Classifier *classifier_for_(Metric m) const;
  void update_classifier_(Classifier &c, const Bands &bands, bool usable, float value, uint32_t now_ms);

  ModelConfig config_;
  Source sources_[METRIC_COUNT];
  // PM group (PM2.5 drives all three PM cells), VOC, NOx, CO2.
  Classifier pm_, voc_, nox_, co2_;
  bool sen65_seen_{false};
  bool sen65_fault_{false};
};

}  // namespace esphome::aqi_dashboard
