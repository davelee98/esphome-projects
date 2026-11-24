#include "dashboard_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dashboard_render.h"

namespace esphome::aqi_dashboard {

const char *metric_name(Metric m) {
  static const char *const NAMES[METRIC_COUNT] = {"PM2.5", "PM10", "PM1", "CO2", "VOC", "NOx", "TEMP", "HUM"};
  size_t i = metric_index(m);
  return i < METRIC_COUNT ? NAMES[i] : "?";
}

bool state_has_value(CellState s) {
  return s == CellState::NORMAL || s == CellState::ELEVATED || s == CellState::HIGH;
}

const char *state_name(CellState s) {
  switch (s) {
    case CellState::NORMAL:
      return "normal";
    case CellState::ELEVATED:
      return "elevated";
    case CellState::HIGH:
      return "high";
    case CellState::PLACEHOLDER:
      return "placeholder";
    case CellState::FAULT:
      return "fault";
    case CellState::OVERFLOW:
      return "overflow";
  }
  return "?";
}

const char *test_pattern_name(TestPattern p) {
  switch (p) {
    case TestPattern::MIXED:
      return "mixed";
    case TestPattern::HIGH:
      return "high";
    case TestPattern::NONE:
    default:
      return "none";
  }
}

void apply_test_pattern(TestPattern p, Snapshot *s) {
  using S = CellState;
  // Indexed by Metric: PM2.5, PM10, PM1, CO2, VOC, NOx, TEMP, HUM.
  static const S MIXED[METRIC_COUNT] = {S::HIGH, S::HIGH, S::HIGH, S::NORMAL,
                                        S::ELEVATED, S::ELEVATED, S::NORMAL, S::ELEVATED};
  static const S HIGH[METRIC_COUNT] = {S::HIGH, S::HIGH, S::HIGH, S::NORMAL,
                                       S::HIGH, S::ELEVATED, S::NORMAL, S::NORMAL};
  if (s == nullptr || p == TestPattern::NONE)
    return;
  const S *pattern = p == TestPattern::MIXED ? MIXED : HIGH;
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    if (state_has_value(s->cells[i].state))
      s->cells[i].state = pattern[i];
  }
}

Category classify(Category current, float value, const Bands &b) {
  switch (current) {
    case Category::NORMAL:
      if (value >= b.enter_red)
        return Category::HIGH;
      if (value >= b.enter_yellow)
        return Category::ELEVATED;
      return Category::NORMAL;
    case Category::ELEVATED:
      if (value >= b.enter_red)
        return Category::HIGH;
      if (value < b.leave_yellow)
        return Category::NORMAL;
      return Category::ELEVATED;
    case Category::HIGH:
      if (value >= b.leave_red)
        return Category::HIGH;  // equality at the exit threshold stays
      return value < b.leave_yellow ? Category::NORMAL : Category::ELEVATED;
  }
  return Category::NORMAL;
}

bool value_in_range(Metric m, float raw) {
  if (!std::isfinite(raw))
    return false;
  switch (m) {
    case Metric::PM2_5:
    case Metric::PM10:
    case Metric::PM1:
    case Metric::CO2:
      return raw >= 0.0f;  // zero is a real reading
    case Metric::VOC:
    case Metric::NOX:
      return raw >= 0.0f && raw <= 500.0f;  // Sensirion gas index scale
    case Metric::TEMP:
      return raw >= -200.0f && raw <= 200.0f;  // sanity bounds; wide values overflow, never clip
    case Metric::HUM:
      return raw >= 0.0f && raw <= 100.0f;
  }
  return false;
}

bool format_value(Metric m, float raw, bool fahrenheit, char *out, size_t size) {
  if (out == nullptr || size == 0)
    return false;
  out[0] = '\0';
  int n;
  if (m == Metric::TEMP) {
    double t = fahrenheit ? raw * 9.0 / 5.0 + 32.0 : raw;
    if (!std::isfinite(t) || std::fabs(t) > 1.0e6)
      return false;
    long tenths = std::lround(t * 10.0);  // round at the displayed precision
    long mag = tenths < 0 ? -tenths : tenths;
    // A rounded zero is "0.0", never "-0.0".
    n = snprintf(out, size, "%s%ld.%ld", tenths < 0 ? "-" : "", mag / 10, mag % 10);
  } else {
    double v = raw;
    if (!std::isfinite(v) || std::fabs(v) > 1.0e9)
      return false;
    n = snprintf(out, size, "%ld", std::lround(v));
  }
  if (n < 0 || static_cast<size_t>(n) >= size) {
    out[0] = '\0';
    return false;  // would truncate: show as overflow
  }
  return true;
}

const char *sensor_error(bool failed, bool read_errors, bool has_fresh_reading, bool ever_reported,
                         bool startup_wait_over) {
  if (failed)
    return "SENSOR MISSING";
  if (has_fresh_reading)
    return nullptr;
  if (read_errors || (!ever_reported && startup_wait_over))
    return ever_reported ? "SENSOR ERROR" : "SENSOR MISSING";
  return nullptr;
}

void Model::configure(const ModelConfig &config) { this->config_ = config; }

const Bands *Model::bands_for_(Metric m) const {
  switch (m) {
    case Metric::PM2_5:
      return &this->config_.pm2_5;
    case Metric::VOC:
      return &this->config_.voc;
    case Metric::NOX:
      return &this->config_.nox;
    case Metric::CO2:
      return &this->config_.co2;
    default:
      return nullptr;
  }
}

Model::Classifier *Model::classifier_for_(Metric m) {
  return const_cast<Classifier *>(static_cast<const Model *>(this)->classifier_for_(m));
}

const Model::Classifier *Model::classifier_for_(Metric m) const {
  switch (m) {
    case Metric::PM2_5:
    case Metric::PM10:
    case Metric::PM1:
      return &this->pm_;  // the PM group follows PM2.5
    case Metric::VOC:
      return &this->voc_;
    case Metric::NOX:
      return &this->nox_;
    case Metric::CO2:
      return &this->co2_;
    default:
      return nullptr;
  }
}

bool Model::fresh_(Metric m, uint32_t now_ms) const {
  const Source &s = this->sources_[metric_index(m)];
  return static_cast<uint32_t>(now_ms - s.sample_ms) <= this->config_.stale_after_ms;
}

void Model::update_classifier_(Classifier &c, const Bands &bands, bool usable, float value, uint32_t now_ms) {
  if (!usable || !bands.enabled) {
    // Losing validity resets both the candidate and the committed category;
    // the next valid reading classifies at once (section 4.3).
    c = Classifier{};
    return;
  }
  if (!c.committed_valid) {
    c.committed = classify(Category::NORMAL, value, bands);
    c.committed_valid = true;
    c.has_candidate = false;
    return;
  }
  Category target = classify(c.committed, value, bands);
  if (target == c.committed) {
    c.has_candidate = false;
    return;
  }
  if (!c.has_candidate || c.candidate != target) {
    // The timer starts at the first qualifying sample.
    c.has_candidate = true;
    c.candidate = target;
    c.candidate_since_ms = now_ms;
    if (this->config_.persistence_ms != 0)
      return;
  }
  if (static_cast<uint32_t>(now_ms - c.candidate_since_ms) >= this->config_.persistence_ms) {
    c.committed = target;
    c.has_candidate = false;
  }
}

void Model::on_sample(Metric m, float value, uint32_t now_ms) {
  Source &s = this->sources_[metric_index(m)];
  // A gap longer than the stale timeout breaks persistence like lost validity.
  bool gap = s.seen_valid && !this->fresh_(m, now_ms);
  bool valid = value_in_range(m, value);
  s.valid = valid;
  s.sample_ms = now_ms;
  if (valid) {
    s.seen_valid = true;
    s.raw = value;
  }
  if (m != Metric::CO2)
    this->sen65_seen_ = true;

  // PM10 and PM1 follow PM2.5's category; only PM2.5 drives the classifier.
  if (m == Metric::PM2_5 || m == Metric::VOC || m == Metric::NOX || m == Metric::CO2) {
    Classifier *c = this->classifier_for_(m);
    if (gap)
      *c = Classifier{};
    this->update_classifier_(*c, *this->bands_for_(m), valid, value, now_ms);
  }
}

CellState Model::state(Metric m, uint32_t now_ms) const {
  if (m == Metric::CO2 && !this->config_.co2_configured)
    return CellState::PLACEHOLDER;
  const Source &s = this->sources_[metric_index(m)];
  if (!s.seen_valid) {
    // Starting, or never sampled; faulted if the SEN65 itself is missing.
    return (this->sen65_fault_ && m != Metric::CO2) ? CellState::FAULT : CellState::PLACEHOLDER;
  }
  if (!s.valid || !this->fresh_(m, now_ms))
    return CellState::FAULT;

  Category cat = Category::NORMAL;
  switch (m) {
    case Metric::PM2_5:
    case Metric::PM10:
    case Metric::PM1: {
      // Without a usable PM2.5 the other PM cells show plain white.
      const Source &pm25 = this->sources_[metric_index(Metric::PM2_5)];
      if (pm25.valid && this->fresh_(Metric::PM2_5, now_ms) && this->pm_.committed_valid)
        cat = this->pm_.committed;
      break;
    }
    case Metric::VOC:
      if (this->voc_.committed_valid)
        cat = this->voc_.committed;
      break;
    case Metric::NOX:
      if (this->nox_.committed_valid)
        cat = this->nox_.committed;
      break;
    case Metric::CO2:
      if (this->config_.co2.enabled && this->co2_.committed_valid)
        cat = this->co2_.committed;
      break;
    default:
      break;  // TEMP and HUM stay white
  }
  switch (cat) {
    case Category::ELEVATED:
      return CellState::ELEVATED;
    case Category::HIGH:
      return CellState::HIGH;
    default:
      return CellState::NORMAL;
  }
}

bool Model::sen65_fresh(uint32_t now_ms) const {
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    Metric m = static_cast<Metric>(i);
    if (m == Metric::CO2)
      continue;
    const Source &s = this->sources_[i];
    if (s.seen_valid && s.valid && this->fresh_(m, now_ms))
      return true;
  }
  return false;
}

Category Model::committed(Metric classifier) const {
  const Classifier *c = this->classifier_for_(classifier);
  return (c != nullptr && c->committed_valid) ? c->committed : Category::NORMAL;
}

void Model::build(uint32_t now_ms, Snapshot *out) const {
  out->fahrenheit = this->config_.fahrenheit;
  out->stamp_alert = false;  // the caller sets the stamp
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    Metric m = static_cast<Metric>(i);
    Cell &cell = out->cells[i];
    cell.state = this->state(m, now_ms);
    cell.raw = 0;
    if (!state_has_value(cell.state)) {
      snprintf(cell.text, sizeof(cell.text), "--");
      continue;
    }
    cell.raw = this->sources_[i].raw;
    if (!format_value(m, cell.raw, out->fahrenheit, cell.text, sizeof(cell.text)) || !value_fits(m, cell.text)) {
      // Keep the raw reading; only the display state becomes overflow.
      cell.state = CellState::OVERFLOW;
      snprintf(cell.text, sizeof(cell.text), "----");
    }
  }
}

}  // namespace esphome::aqi_dashboard
