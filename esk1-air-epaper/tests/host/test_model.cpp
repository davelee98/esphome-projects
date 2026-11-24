// Value model: formatting, validity, classification (plan 4, spec section 4).

#include <cmath>

#include "../../components/aqi_dashboard/dashboard_model.h"
#include "../../components/aqi_dashboard/dashboard_scheduler.h"
#include "test_util.h"

using namespace esphome::aqi_dashboard;

namespace {
std::string fmt(Metric m, float v, bool f = false) {
  char buf[16];
  if (!format_value(m, v, f, buf, sizeof(buf)))
    return "<overflow>";
  return buf;
}
const uint32_t MIN = 60000;
}  // namespace

TEST(model_formatting) {
  CHECK_STR(fmt(Metric::PM2_5, 12.4f), "12");
  CHECK_STR(fmt(Metric::PM2_5, 12.5f), "13");  // lround: half away from zero
  CHECK_STR(fmt(Metric::PM2_5, 0.0f), "0");
  CHECK_STR(fmt(Metric::CO2, 9999.4f), "9999");
  CHECK_STR(fmt(Metric::TEMP, 22.44f), "22.4");
  CHECK_STR(fmt(Metric::TEMP, -5.25f), "-5.3");
  CHECK_STR(fmt(Metric::TEMP, -0.04f), "0.0");  // no negative zero
  CHECK_STR(fmt(Metric::TEMP, -40.0f), "-40.0");
  CHECK_STR(fmt(Metric::TEMP, 99.96f), "100.0");  // rounding adds a digit
  CHECK_STR(fmt(Metric::TEMP, 20.0f, true), "68.0");
  CHECK_STR(fmt(Metric::TEMP, -40.0f, true), "-40.0");
  CHECK_STR(fmt(Metric::HUM, 99.6f), "100");
  char tiny[4];
  CHECK(!format_value(Metric::CO2, 12345, false, tiny, sizeof(tiny)));  // truncation is overflow
  CHECK(!format_value(Metric::PM2_5, NAN, false, tiny, sizeof(tiny)));
}

TEST(model_validity) {
  CHECK(value_in_range(Metric::PM2_5, 0));
  CHECK(!value_in_range(Metric::PM2_5, -1));
  CHECK(!value_in_range(Metric::PM2_5, NAN));
  CHECK(!value_in_range(Metric::PM2_5, INFINITY));
  CHECK(value_in_range(Metric::HUM, 100));
  CHECK(!value_in_range(Metric::HUM, 100.1f));
  CHECK(value_in_range(Metric::VOC, 500));
  CHECK(!value_in_range(Metric::VOC, 501));
  CHECK(!value_in_range(Metric::CO2, -5));
}

TEST(model_hysteresis) {
  Bands b{true, 15, 35, 12, 30};
  CHECK(classify(Category::NORMAL, 14.9f, b) == Category::NORMAL);
  CHECK(classify(Category::NORMAL, 15, b) == Category::ELEVATED);
  CHECK(classify(Category::NORMAL, 35, b) == Category::HIGH);
  CHECK(classify(Category::ELEVATED, 12, b) == Category::ELEVATED);  // equality at exit stays
  CHECK(classify(Category::ELEVATED, 11.9f, b) == Category::NORMAL);
  CHECK(classify(Category::ELEVATED, 35, b) == Category::HIGH);
  CHECK(classify(Category::HIGH, 30, b) == Category::HIGH);
  CHECK(classify(Category::HIGH, 29.9f, b) == Category::ELEVATED);
  CHECK(classify(Category::HIGH, 11, b) == Category::NORMAL);
  CHECK(classify(Category::HIGH, 13, b) == Category::ELEVATED);  // below enter_yellow, above leave_yellow
}

TEST(model_persistence) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  m.on_sample(Metric::PM2_5, 8, t);  // first valid sample commits at once
  CHECK(m.state(Metric::PM2_5, t) == CellState::NORMAL);
  m.on_sample(Metric::PM2_5, 20, t += MIN);  // candidate ELEVATED starts here
  CHECK(m.state(Metric::PM2_5, t) == CellState::NORMAL);
  m.on_sample(Metric::PM2_5, 20, t += MIN);
  m.on_sample(Metric::PM2_5, 20, t += MIN);
  CHECK(m.state(Metric::PM2_5, t) == CellState::NORMAL);  // 120 s held
  m.on_sample(Metric::PM2_5, 20, t += MIN);  // 180 s: four qualifying samples
  CHECK(m.state(Metric::PM2_5, t) == CellState::ELEVATED);
  // PM10 and PM1 follow PM2.5's category.
  m.on_sample(Metric::PM10, 3, t);
  m.on_sample(Metric::PM1, 2, t);
  CHECK(m.state(Metric::PM10, t) == CellState::ELEVATED);
  CHECK(m.state(Metric::PM1, t) == CellState::ELEVATED);

  // An interrupted candidate restarts.
  m.on_sample(Metric::PM2_5, 40, t += MIN);  // candidate HIGH
  m.on_sample(Metric::PM2_5, 40, t += MIN);
  m.on_sample(Metric::PM2_5, 20, t += MIN);  // back to elevated: candidate cleared
  m.on_sample(Metric::PM2_5, 40, t += MIN);  // HIGH again, timer restarts
  m.on_sample(Metric::PM2_5, 40, t += MIN);
  m.on_sample(Metric::PM2_5, 40, t += MIN);
  CHECK(m.state(Metric::PM2_5, t) == CellState::ELEVATED);
  m.on_sample(Metric::PM2_5, 40, t += MIN);
  CHECK(m.state(Metric::PM2_5, t) == CellState::HIGH);
}

TEST(model_staleness_without_callback) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 5000;
  m.on_sample(Metric::TEMP, 21, t);
  CHECK(m.state(Metric::TEMP, t + 180000) == CellState::NORMAL);  // exactly 180 s: still fresh
  CHECK(m.state(Metric::TEMP, t + 180001) == CellState::FAULT);
  // Wrap-safe across the millis() overflow.
  Model w;
  w.configure(ModelConfig{});
  uint32_t near = 0xFFFFFFFFu - 1000;
  w.on_sample(Metric::HUM, 40, near);
  CHECK(w.state(Metric::HUM, near + 5000) == CellState::NORMAL);
}

TEST(model_availability_states) {
  Model m;
  ModelConfig cfg;
  m.configure(cfg);
  uint32_t t = 1000;
  CHECK(m.state(Metric::VOC, t) == CellState::PLACEHOLDER);   // starting
  m.on_sample(Metric::VOC, NAN, t);                            // warm-up NaN
  CHECK(m.state(Metric::VOC, t) == CellState::PLACEHOLDER);
  m.on_sample(Metric::VOC, 100, t);
  CHECK(m.state(Metric::VOC, t) == CellState::NORMAL);
  m.on_sample(Metric::VOC, NAN, t + 1000);                     // invalid after valid
  CHECK(m.state(Metric::VOC, t + 1000) == CellState::FAULT);
  CHECK(m.state(Metric::CO2, t) == CellState::PLACEHOLDER);   // not configured

  ModelConfig with_co2;
  with_co2.co2_configured = true;
  Model c;
  c.configure(with_co2);
  c.on_sample(Metric::CO2, 2500, t);
  CHECK(c.state(Metric::CO2, t) == CellState::NORMAL);  // classification off by default
  CHECK(c.state(Metric::CO2, t + 200000) == CellState::FAULT);
}

TEST(model_pm_group_without_pm25) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  m.on_sample(Metric::PM2_5, 50, t);  // HIGH at once (first sample)
  m.on_sample(Metric::PM10, 60, t);
  CHECK(m.state(Metric::PM10, t) == CellState::HIGH);
  m.on_sample(Metric::PM2_5, NAN, t + 1000);  // PM2.5 lost
  m.on_sample(Metric::PM10, 60, t + 1000);
  CHECK(m.state(Metric::PM2_5, t + 1000) == CellState::FAULT);
  CHECK(m.state(Metric::PM10, t + 1000) == CellState::NORMAL);  // no invented PM10 cut-offs
}

TEST(model_build_snapshot_overflow) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  m.on_sample(Metric::PM2_5, 99999.6f, t);  // rounds to 100000: six digits, five fit
  m.on_sample(Metric::TEMP, 22.44f, t);
  Snapshot s{};
  m.build(t, &s);
  const Cell &pm = s.cells[metric_index(Metric::PM2_5)];
  CHECK(pm.state == CellState::OVERFLOW);
  CHECK_STR(pm.text, "----");
  CHECK(pm.raw > 99999);  // the raw reading is kept
  CHECK_STR(s.cells[metric_index(Metric::TEMP)].text, "22.4");
  CHECK(s.cells[metric_index(Metric::VOC)].state == CellState::PLACEHOLDER);
  CHECK_STR(s.cells[metric_index(Metric::VOC)].text, "--");
  CHECK(sizeof(Snapshot) * 3 < 1024);  // latest + in_flight + displayed
}

TEST(model_sensor_error_decision) {
  //            failed read_err fresh  ever   waited
  CHECK_STR(sensor_error(true, false, false, false, false), "SENSOR MISSING");  // not on I2C at boot
  CHECK_STR(sensor_error(true, false, true, true, true), "SENSOR MISSING");     // failed wins
  CHECK(sensor_error(false, false, false, false, false) == nullptr);           // warming up
  CHECK_STR(sensor_error(false, false, false, false, true), "SENSOR MISSING");  // nothing by startup_wait
  CHECK_STR(sensor_error(false, true, false, true, true), "SENSOR ERROR");      // unplugged later
  CHECK_STR(sensor_error(false, true, false, false, false), "SENSOR MISSING");  // read errors, never reported
  CHECK(sensor_error(false, true, true, true, true) == nullptr);               // a current reading: no error
  CHECK(sensor_error(false, false, false, true, true) == nullptr);             // stale, no read errors: yellow only
}

TEST(model_sen65_fault_cells) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  CHECK(m.state(Metric::PM2_5, t) == CellState::PLACEHOLDER);
  CHECK(!m.sen65_fresh(t));
  m.set_sen65_fault(true);
  for (Metric x : {Metric::PM2_5, Metric::PM10, Metric::PM1, Metric::VOC, Metric::NOX, Metric::TEMP, Metric::HUM})
    CHECK(m.state(x, t) == CellState::FAULT);
  CHECK(m.state(Metric::CO2, t) == CellState::PLACEHOLDER);  // not the SEN65's
  m.set_sen65_fault(false);
  m.on_sample(Metric::TEMP, 21, t);
  CHECK(m.sen65_fresh(t));
  CHECK(!m.sen65_fresh(t + 200000));
}

TEST(model_gap_resets_persistence) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  m.on_sample(Metric::NOX, 5, t);
  m.on_sample(Metric::NOX, 25, t += MIN);  // candidate ELEVATED
  m.on_sample(Metric::NOX, 25, t += MIN);
  m.on_sample(Metric::NOX, 25, t += 4 * MIN);  // the stream went stale: start over
  CHECK(m.state(Metric::NOX, t) == CellState::ELEVATED);  // reclassified at once after the gap
}

TEST(model_test_patterns) {
  Model m;
  m.configure(ModelConfig{});
  uint32_t t = 1000;
  m.on_sample(Metric::PM2_5, 8, t);
  m.on_sample(Metric::PM10, 12, t);
  m.on_sample(Metric::PM1, 5, t);
  m.on_sample(Metric::VOC, 125, t);
  m.on_sample(Metric::NOX, 1, t);
  m.on_sample(Metric::TEMP, 22.4f, t);
  m.on_sample(Metric::HUM, 46, t);  // CO2 not configured: "--"
  Snapshot real{};
  m.build(t, &real);

  Snapshot mixed = real;
  apply_test_pattern(TestPattern::MIXED, &mixed);
  CHECK(mixed.cells[metric_index(Metric::PM2_5)].state == CellState::HIGH);
  CHECK(mixed.cells[metric_index(Metric::PM10)].state == CellState::HIGH);
  CHECK(mixed.cells[metric_index(Metric::PM1)].state == CellState::HIGH);
  CHECK(mixed.cells[metric_index(Metric::VOC)].state == CellState::ELEVATED);
  CHECK(mixed.cells[metric_index(Metric::NOX)].state == CellState::ELEVATED);
  CHECK(mixed.cells[metric_index(Metric::HUM)].state == CellState::ELEVATED);
  CHECK(mixed.cells[metric_index(Metric::TEMP)].state == CellState::NORMAL);
  // No value: the real state stays, never coloured as a measurement.
  CHECK(mixed.cells[metric_index(Metric::CO2)].state == CellState::PLACEHOLDER);
  // The readings are the real ones.
  for (size_t i = 0; i < METRIC_COUNT; i++)
    CHECK_STR(mixed.cells[i].text, real.cells[i].text);

  Snapshot high = real;
  apply_test_pattern(TestPattern::HIGH, &high);
  CHECK(high.cells[metric_index(Metric::VOC)].state == CellState::HIGH);
  CHECK(high.cells[metric_index(Metric::NOX)].state == CellState::ELEVATED);
  CHECK(high.cells[metric_index(Metric::HUM)].state == CellState::NORMAL);
  CHECK(high.cells[metric_index(Metric::PM2_5)].state == CellState::HIGH);

  // NONE leaves the page alone; the scheduler sees the test page as a
  // state change both ways, so the next update reverts it.
  Snapshot none = real;
  apply_test_pattern(TestPattern::NONE, &none);
  for (size_t i = 0; i < METRIC_COUNT; i++)
    CHECK(none.cells[i].state == real.cells[i].state);
  Scheduler sch;
  sch.configure(Timing{}, Deadbands{});
  sch.start(0);
  CHECK(sch.tick(t, mixed, true, false).upload);  // boot page: the test page
  sch.on_complete(Outcome::SUCCESS, t + 1000, false, 0);
  CHECK(sch.state_change(real));
  CHECK_STR(test_pattern_name(TestPattern::HIGH), "high");
}
