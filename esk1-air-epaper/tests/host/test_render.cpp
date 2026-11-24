// Primitive renderer and page geometry (plan 3.3, spec section 9).

#include <cstdio>
#include <string>

#include "../../components/aqi_dashboard/dashboard_model.h"
#include "../../components/aqi_dashboard/dashboard_render.h"
#include "test_util.h"

using namespace esphome::aqi_dashboard;

namespace {

// A canvas with a canary margin all round; put_pixel outside the canvas is
// counted, and the margin must stay untouched.
struct Canvas {
  static constexpr int M = 16;
  int w, h;
  std::vector<uint8_t> px;  // (w + 2M) x (h + 2M), 0xEE = canary
  int out_of_bounds{0};
  Canvas(int w_ = CANVAS_WIDTH, int h_ = CANVAS_HEIGHT) : w(w_), h(h_), px((w_ + 2 * M) * (h_ + 2 * M), 0xEE) {
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        at(x, y) = 0xAA;  // "never drawn"
  }
  uint8_t &at(int x, int y) { return px[(y + M) * (w + 2 * M) + (x + M)]; }
  Ink ink(int x, int y) { return static_cast<Ink>(at(x, y)); }
  static void put(void *ctx, int x, int y, Ink ink) {
    auto *c = static_cast<Canvas *>(ctx);
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) {
      c->out_of_bounds++;
      return;
    }
    c->at(x, y) = static_cast<uint8_t>(ink);
  }
  Raster raster() { return Raster{this, &Canvas::put, w, h}; }
  bool canary_intact() {
    for (int y = -M; y < h + M; y++)
      for (int x = -M; x < w + M; x++)
        if ((x < 0 || y < 0 || x >= w || y >= h) && at(x, y) != 0xEE)
          return false;
    return true;
  }
  int count(int x0, int y0, int x1, int y1, Ink ink) {
    int n = 0;
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        if (at(x, y) == static_cast<uint8_t>(ink))
          n++;
    return n;
  }
  // Leftmost / rightmost column with a pixel of `ink` in a band, or -1.
  int leftmost(int x0, int y0, int x1, int y1, Ink ink) {
    for (int x = x0; x < x1; x++)
      for (int y = y0; y < y1; y++)
        if (at(x, y) == static_cast<uint8_t>(ink))
          return x;
    return -1;
  }
  int rightmost(int x0, int y0, int x1, int y1, Ink ink) {
    for (int x = x1 - 1; x >= x0; x--)
      for (int y = y0; y < y1; y++)
        if (at(x, y) == static_cast<uint8_t>(ink))
          return x;
    return -1;
  }
  void write_pnm(const std::string &name) {
    if (host_test::output_dir().empty())
      return;
    std::string path = host_test::output_dir() + "/" + name + ".ppm";
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
      return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    static const uint8_t RGB[4][3] = {{0, 0, 0}, {255, 255, 255}, {245, 200, 0}, {200, 30, 30}};
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        uint8_t v = at(x, y);
        const uint8_t magenta[3] = {255, 0, 255};
        std::fwrite(v < 4 ? RGB[v] : magenta, 1, 3, f);
      }
    std::fclose(f);
  }
};

void set_cell(Snapshot &s, Metric m, CellState st, const char *text, float raw = 0) {
  Cell &c = s.cells[metric_index(m)];
  c.state = st;
  c.raw = raw;
  snprintf(c.text, sizeof(c.text), "%s", text);
}

Snapshot sample_snapshot() {
  Snapshot s{};
  snprintf(s.title, sizeof(s.title), "INDOOR AIR");
  snprintf(s.stamp, sizeof(s.stamp), "23 NOV 14:35");
  s.fahrenheit = false;
  set_cell(s, Metric::PM2_5, CellState::NORMAL, "8", 8);
  set_cell(s, Metric::PM10, CellState::NORMAL, "12", 12);
  set_cell(s, Metric::PM1, CellState::NORMAL, "5", 5);
  set_cell(s, Metric::CO2, CellState::PLACEHOLDER, "--");
  set_cell(s, Metric::VOC, CellState::NORMAL, "125", 125);
  set_cell(s, Metric::NOX, CellState::NORMAL, "1", 1);
  set_cell(s, Metric::TEMP, CellState::NORMAL, "22.4", 22.4f);
  set_cell(s, Metric::HUM, CellState::NORMAL, "46", 46);
  return s;
}

Canvas render(const Snapshot &s) {
  Canvas c;
  draw_dashboard(c.raster(), s);
  return c;
}

}  // namespace

TEST(render_field_widths) {
  // Labels are 24 px bold, 12 px per glyph.
  CHECK_EQ(value_field_width(Metric::PM2_5), 68);
  CHECK_EQ(value_field_width(Metric::PM10), 80);
  CHECK_EQ(value_field_width(Metric::PM1), 92);
  CHECK_EQ(value_field_width(Metric::TEMP), 80);
  CHECK_EQ(value_field_width(Metric::CO2), 98);
  CHECK_EQ(value_field_width(Metric::VOC), 98);
  CHECK_EQ(value_field_width(Metric::NOX), 98);
  CHECK_EQ(value_field_width(Metric::HUM), 98);
  CHECK(value_fits(Metric::PM2_5, "1000"));  // the SEN65's top of range
  CHECK(!value_fits(Metric::PM2_5, "10000"));
  CHECK(value_fits(Metric::TEMP, "-10.5"));
  CHECK(value_fits(Metric::TEMP, "104.0"));
  CHECK(!value_fits(Metric::TEMP, "-100.5"));
  CHECK(value_fits(Metric::CO2, "9999"));
  CHECK(value_fits(Metric::CO2, "999999"));
  CHECK(!value_fits(Metric::CO2, "1000000"));
  CHECK_EQ(measure_text(FONT_BOLD32, "----"), 64);
}

TEST(render_text_metrics) {
  CHECK_EQ(centered_top(FONT_TEXT14, "PM2.5", 70), 63);
  CHECK_EQ(centered_top(FONT_BOLD32, "12", 70), 54);
  CHECK_EQ(measure_text(FONT_TEXT14, "\xC2\xB0" "C"), 16);  // the degree sign is one glyph
  CHECK_EQ(measure_text(FONT_TEXT24, "\xC2\xB0" "C"), 24);
  CHECK_EQ(measure_text(FONT_TEXT24, "PM2.5"), 60);
  CHECK_EQ(utf8_length("\xC2\xB5g/m\xC2\xB3"), 5u);
  CHECK(find_glyph(FONT_BOLD32, '?') == nullptr);
  CHECK(find_glyph(FONT_TEXT14, 0xB0) != nullptr);
  CHECK_EQ(FONT_TEXT14.glyph_count, 98);
  CHECK_EQ(FONT_TEXT24.glyph_count, 98);
  CHECK(find_glyph(FONT_TEXT24, 0xB0) != nullptr);
  CHECK_EQ(FONT_BOLD32.glyph_count, 17);
  // Malformed UTF-8 becomes U+FFFD, drawn as '?'.
  const char bad[] = "\xC3";
  const char *p = bad;
  CHECK_EQ(utf8_next(&p, bad + 1), 0xFFFDu);
  CHECK_EQ(measure_text(FONT_TEXT14, "\xC3\xA9"), 8);  // e-acute is not in the font: '?'
}

TEST(render_geometry) {
  Snapshot s = sample_snapshot();
  for (auto &c : s.cells)
    c.state = CellState::ELEVATED;  // every cell yellow, to see the fills
  Canvas c = render(s);
  CHECK(c.canary_intact());
  CHECK_EQ(c.out_of_bounds, 0);
  CHECK_EQ(c.count(0, 0, 400, 300, static_cast<Ink>(0xAA)), 0);  // every pixel painted
  for (size_t i = 0; i < METRIC_COUNT; i++) {
    const CellLayout &l = cell_layout(i);
    // Corners inside are yellow; the pixel outside each edge is white.
    CHECK(c.ink(l.x, l.y) == Ink::YELLOW);
    CHECK(c.ink(l.x + 183, l.y + 59) == Ink::YELLOW);
    CHECK(c.ink(l.x - 1, l.y) == Ink::WHITE);
    CHECK(c.ink(l.x + 184, l.y) == Ink::WHITE);
    CHECK(c.ink(l.x, l.y - 1) == Ink::WHITE || l.y == 40);
    CHECK(c.ink(l.x, l.y + 60) == Ink::WHITE);
  }
  // Gutters: 8 px between columns, 4 px between rows, 8 px at the bottom.
  CHECK_EQ(c.count(196, 40, 204, 292, Ink::YELLOW), 0);
  CHECK_EQ(c.count(12, 100, 388, 104, Ink::YELLOW), 0);
  CHECK_EQ(c.count(0, 292, 400, 300, Ink::WHITE), 400 * 8);
  // Header rule (12, 30, 376, 1): last pixel (387, 30).
  CHECK_EQ(c.count(12, 30, 388, 31, Ink::BLACK), 376);
  CHECK(c.ink(11, 30) == Ink::WHITE);
  CHECK(c.ink(388, 30) == Ink::WHITE);
  CHECK(c.ink(200, 29) == Ink::WHITE);
  CHECK(c.ink(200, 31) == Ink::WHITE);
}

TEST(render_anchors) {
  Snapshot s = sample_snapshot();
  Canvas c = render(s);
  c.write_pnm("normal");
  // TEMP row (y 232..292): value "22.4" ends before x = 152, the unit starts at 158.
  int value_right = c.rightmost(20, 232, 156, 292, Ink::BLACK);
  CHECK(value_right >= 140 && value_right < 152);
  int unit_left = c.leftmost(156, 232, 196, 292, Ink::BLACK);
  CHECK(unit_left >= 158 && unit_left < 166);
  // HUM: value ends before 350, "%" starts at or after 356, inside 380.
  CHECK(c.rightmost(212, 232, 354, 292, Ink::BLACK) < 350);
  int pct = c.leftmost(352, 232, 388, 292, Ink::BLACK);
  CHECK(pct >= 356 && pct < 380);
  // Labels are red, left at x = 20 / 212.
  int label = c.leftmost(12, 40, 60, 100, Ink::RED);
  CHECK(label >= 20 && label < 24);
  CHECK(c.leftmost(204, 232, 260, 292, Ink::RED) >= 212);
  // PM sprite: at (158, cell_y + 24), 30 x 12, in black.
  CHECK(c.count(158, 64, 188, 76, Ink::BLACK) > 20);
  CHECK_EQ(c.count(156, 40, 196, 64, Ink::BLACK), 0);
  CHECK_EQ(c.count(156, 76, 196, 100, Ink::BLACK), 0);
  // Header text is black and inside its line.
  CHECK(c.count(12, 0, 388, 30, Ink::BLACK) > 50);
  CHECK(c.rightmost(200, 0, 400, 30, Ink::BLACK) < 388);
  CHECK(c.leftmost(0, 0, 100, 30, Ink::BLACK) >= 12);
  CHECK(c.canary_intact());
}

TEST(render_states_and_colors) {
  Snapshot s = sample_snapshot();
  set_cell(s, Metric::PM2_5, CellState::HIGH, "48", 48);
  set_cell(s, Metric::PM10, CellState::HIGH, "60", 60);
  set_cell(s, Metric::PM1, CellState::HIGH, "30", 30);
  set_cell(s, Metric::VOC, CellState::ELEVATED, "180", 180);
  set_cell(s, Metric::NOX, CellState::FAULT, "--");
  set_cell(s, Metric::HUM, CellState::OVERFLOW, "----", 46);
  Canvas c = render(s);
  c.write_pnm("mixed");
  // High (red) cell: label and value white; no black at all.
  CHECK_EQ(c.count(12, 40, 196, 100, Ink::BLACK), 0);
  CHECK(c.count(12, 40, 196, 100, Ink::WHITE) > 100);
  CHECK(c.count(158, 64, 188, 76, Ink::WHITE) > 20);  // the sprite is white on red
  // Elevated (yellow): label and value black, no red.
  CHECK_EQ(c.count(204, 104, 388, 164, Ink::RED), 0);
  CHECK(c.count(204, 104, 388, 164, Ink::BLACK) > 50);
  CHECK_EQ(c.count(204, 104, 388, 164, Ink::WHITE), 0);
  // Fault (yellow "--"), unit "idx" still visible.
  CHECK(c.ink(204, 168) == Ink::YELLOW);
  CHECK(c.count(356, 168, 380, 228, Ink::BLACK) > 10);
  // Overflow: yellow, "----" right-aligned at 350, "%" kept.
  CHECK(c.ink(204, 232) == Ink::YELLOW);
  int r = c.rightmost(212, 232, 354, 292, Ink::BLACK);
  CHECK(r < 350 && r >= 340);
  CHECK(c.count(356, 232, 380, 292, Ink::BLACK) > 5);
  // Placeholder CO2: white, red label, black "--" and "ppm".
  CHECK(c.ink(204, 40) == Ink::WHITE);
  CHECK(c.count(204, 40, 388, 100, Ink::RED) > 10);
}

TEST(render_pm_sprite_left_out_for_missing) {
  Snapshot s = sample_snapshot();
  set_cell(s, Metric::PM2_5, CellState::PLACEHOLDER, "--");
  set_cell(s, Metric::PM10, CellState::OVERFLOW, "----", 1e6f);
  Canvas c = render(s);
  CHECK_EQ(c.count(158, 64, 188, 76, Ink::BLACK), 0);  // no sprite beside "--"
  CHECK(c.count(158, 128, 188, 140, Ink::BLACK) > 20);  // kept on overflow
}

TEST(render_value_wider_than_field_is_never_drawn) {
  Snapshot s = sample_snapshot();
  set_cell(s, Metric::PM2_5, CellState::NORMAL, "123456", 123456);  // caller got it wrong
  Canvas c = render(s);
  // Drawn as "----": 64 px wide, right edge before 152.
  int left = c.leftmost(20 + 40, 40, 156, 100, Ink::BLACK);
  CHECK(left >= 152 - 64);
}

TEST(render_header_titles_and_stamps) {
  Snapshot s = sample_snapshot();
  snprintf(s.title, sizeof(s.title), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");  // 30 glyphs
  snprintf(s.stamp, sizeof(s.stamp), "-- --- --:--");
  Canvas c = render(s);
  c.write_pnm("header_long_title");
  // 13 glyphs + "..." = 16 glyphs of 12 px = 192 px from x = 12.
  CHECK(c.rightmost(0, 0, 220, 30, Ink::BLACK) < 12 + 192);
  CHECK(c.rightmost(0, 0, 400, 30, Ink::BLACK) < 388);
  int stamp_left = c.leftmost(220, 0, 400, 30, Ink::BLACK);
  CHECK(stamp_left >= 388 - 12 * 12);

  snprintf(s.title, sizeof(s.title), "K\xC3\xBC" "che");  // u-umlaut is not in the font
  snprintf(s.stamp, sizeof(s.stamp), "NO SAMPLE YET");
  Canvas c2 = render(s);
  CHECK(c2.canary_intact());
  CHECK_EQ(c2.count(0, 0, 400, 300, static_cast<Ink>(0xAA)), 0);
}

TEST(render_sensor_missing) {
  Snapshot s = sample_snapshot();
  for (Metric m : {Metric::PM2_5, Metric::PM10, Metric::PM1, Metric::VOC, Metric::NOX, Metric::TEMP, Metric::HUM})
    set_cell(s, m, CellState::FAULT, "--");
  snprintf(s.stamp, sizeof(s.stamp), "SENSOR MISSING");
  s.stamp_alert = true;
  Canvas c = render(s);
  c.write_pnm("sensor_missing");
  // The error is red, right-aligned at 388; the title stays black.
  CHECK(c.count(200, 0, 400, 30, Ink::RED) > 50);
  CHECK_EQ(c.count(200, 0, 400, 30, Ink::BLACK), 0);
  CHECK(c.rightmost(200, 0, 400, 30, Ink::RED) < 388);
  CHECK(c.leftmost(200, 0, 400, 30, Ink::RED) >= 388 - 14 * 12);
  CHECK(c.count(0, 0, 120, 30, Ink::BLACK) > 20);
  CHECK(c.ink(12, 40) == Ink::YELLOW);
  CHECK(c.ink(204, 40) == Ink::WHITE);  // CO2 is not the SEN65's
  // Yellow cells carry black text only: the whole left column has no red.
  CHECK_EQ(c.count(12, 40, 196, 292, Ink::RED), 0);
  CHECK(c.count(12, 40, 196, 292, Ink::BLACK) > 200);
  CHECK(c.count(204, 40, 388, 100, Ink::RED) > 10);  // white CO2 keeps its red label
  CHECK(c.canary_intact());
}

TEST(render_deterministic) {
  Snapshot s = sample_snapshot();
  Canvas a = render(s), b = render(s);
  CHECK(a.px == b.px);
}

TEST(render_clipping) {
  Canvas c(40, 20);
  Raster r = c.raster();
  fill_rect(r, -10, -10, 100, 100, Ink::YELLOW);
  CHECK_EQ(c.count(0, 0, 40, 20, Ink::YELLOW), 800);
  fill_rect(r, 5, 5, 0, 10, Ink::RED);
  fill_rect(r, 5, 5, -3, 10, Ink::RED);
  CHECK_EQ(c.count(0, 0, 40, 20, Ink::RED), 0);
  fill_rect(r, 2147483000, 0, 2000, 5, Ink::RED);  // no overflow
  CHECK_EQ(c.count(0, 0, 40, 20, Ink::RED), 0);

  // A sprite cut at the left edge keeps its source columns.
  Canvas full(60, 20), cut(60, 20);
  blit_1bpp(full.raster(), 10, 2, SPRITE_PM_UNITS.width, SPRITE_PM_UNITS.height, SPRITE_PM_UNITS.data,
            SPRITE_PM_UNITS.stride, Ink::BLACK);
  blit_1bpp(cut.raster(), -7, 2, SPRITE_PM_UNITS.width, SPRITE_PM_UNITS.height, SPRITE_PM_UNITS.data,
            SPRITE_PM_UNITS.stride, Ink::BLACK);
  bool same = true;
  for (int y = 0; y < 20; y++)
    for (int x = 0; x + 17 < 60; x++)
      if (x < 23 && full.at(x + 17, y) != cut.at(x, y))
        same = false;
  CHECK(same);
  CHECK(cut.canary_intact());
  CHECK_EQ(cut.out_of_bounds, 0);
  // Zero bits are transparent: the background survives.
  Canvas bg(40, 20);
  fill_rect(bg.raster(), 0, 0, 40, 20, Ink::RED);
  blit_1bpp(bg.raster(), 0, 0, SPRITE_PM_UNITS.width, SPRITE_PM_UNITS.height, SPRITE_PM_UNITS.data,
            SPRITE_PM_UNITS.stride, Ink::WHITE);
  CHECK(bg.count(0, 0, 40, 20, Ink::RED) > 300);
  CHECK(bg.count(0, 0, 40, 20, Ink::WHITE) > 20);
  CHECK(bg.count(30, 0, 40, 20, Ink::WHITE) == 0);  // padding bits are not pixels
}

TEST(render_preview_scenarios) {
  // Images for comparison with output/sen65-status-preview.html.
  Snapshot s = sample_snapshot();
  for (auto &cell : s.cells) {
    cell.state = CellState::PLACEHOLDER;
    snprintf(cell.text, sizeof(cell.text), "--");
  }
  snprintf(s.stamp, sizeof(s.stamp), "NO SAMPLE YET");
  render(s).write_pnm("starting");

  Snapshot st = sample_snapshot();
  for (Metric m : {Metric::PM2_5, Metric::PM10, Metric::PM1, Metric::VOC, Metric::NOX, Metric::TEMP, Metric::HUM})
    set_cell(st, m, CellState::FAULT, "--");
  render(st).write_pnm("stale");

  Snapshot hi = sample_snapshot();
  set_cell(hi, Metric::PM2_5, CellState::HIGH, "52", 52);
  set_cell(hi, Metric::PM10, CellState::HIGH, "71", 71);
  set_cell(hi, Metric::PM1, CellState::HIGH, "33", 33);
  set_cell(hi, Metric::VOC, CellState::HIGH, "420", 420);
  set_cell(hi, Metric::NOX, CellState::ELEVATED, "35", 35);
  set_cell(hi, Metric::CO2, CellState::NORMAL, "9999", 9999);
  set_cell(hi, Metric::TEMP, CellState::NORMAL, "-40.0", -40);
  set_cell(hi, Metric::HUM, CellState::NORMAL, "100", 100);
  Canvas c = render(hi);
  c.write_pnm("high");
  CHECK(c.canary_intact());
}
