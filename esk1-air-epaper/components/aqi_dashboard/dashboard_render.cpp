#include "dashboard_render.h"

#include <cmath>
#include <cstring>

namespace esphome::aqi_dashboard {

// ---- Primitives ----

void fill_rect(const Raster &r, int x, int y, int w, int h, Ink ink) {
  if (w <= 0 || h <= 0)
    return;
  // Clip in 64-bit so huge sizes cannot overflow.
  long long x0 = x, y0 = y, x1 = (long long) x + w, y1 = (long long) y + h;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > r.width)
    x1 = r.width;
  if (y1 > r.height)
    y1 = r.height;
  for (long long py = y0; py < y1; py++)
    for (long long px = x0; px < x1; px++)
      r.put_pixel(r.context, (int) px, (int) py, ink);
}

void hline(const Raster &r, int x, int y, int width, Ink ink) { fill_rect(r, x, y, width, 1, ink); }

void blit_1bpp(const Raster &r, int x, int y, int w, int h, const uint8_t *data, int stride, Ink foreground) {
  if (w <= 0 || h <= 0 || data == nullptr || stride < (w + 7) / 8)
    return;
  // Clip the source window, so a bitmap cut at the left edge still starts at
  // the right source column.
  int col0 = x < 0 ? -x : 0;
  int row0 = y < 0 ? -y : 0;
  int col1 = w, row1 = h;
  if ((long long) x + col1 > r.width)
    col1 = r.width - x;
  if ((long long) y + row1 > r.height)
    row1 = r.height - y;
  for (int row = row0; row < row1; row++) {
    const uint8_t *line = data + (size_t) row * stride;
    for (int col = col0; col < col1; col++) {
      if (line[col >> 3] & (0x80u >> (col & 7)))
        r.put_pixel(r.context, x + col, y + row, foreground);
    }
  }
}

// ---- Text ----

uint32_t utf8_next(const char **p, const char *end) {
  const auto *s = reinterpret_cast<const uint8_t *>(*p);
  const auto *e = reinterpret_cast<const uint8_t *>(end);
  if (s >= e)
    return 0;
  uint8_t b0 = s[0];
  int len;
  uint32_t cp;
  if (b0 < 0x80) {
    *p += 1;
    return b0;
  } else if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    *p += 1;
    return 0xFFFD;
  }
  if (e - s < len) {
    *p += 1;
    return 0xFFFD;
  }
  for (int i = 1; i < len; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *p += 1;
      return 0xFFFD;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  // Reject overlong forms, surrogates and out-of-range values.
  static const uint32_t MIN[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < MIN[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *p += 1;
    return 0xFFFD;
  }
  *p += len;
  return cp;
}

size_t utf8_length(const char *utf8) {
  if (utf8 == nullptr)
    return 0;
  const char *p = utf8, *end = utf8 + strlen(utf8);
  size_t n = 0;
  while (p < end) {
    utf8_next(&p, end);
    n++;
  }
  return n;
}

const Glyph *find_glyph(const Font &font, uint32_t codepoint) {
  // Glyphs are sorted by code point.
  int lo = 0, hi = (int) font.glyph_count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t c = font.glyphs[mid].codepoint;
    if (c == codepoint)
      return &font.glyphs[mid];
    if (c < codepoint)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return nullptr;
}

const Glyph *glyph_or_fallback(const Font &font, uint32_t codepoint) {
  const Glyph *g = find_glyph(font, codepoint);
  return g != nullptr ? g : find_glyph(font, '?');
}

static int advance_of(const Font &font, const Glyph *g) { return g != nullptr ? g->advance : font.size / 2; }

int measure_text(const Font &font, const char *utf8) {
  if (utf8 == nullptr)
    return 0;
  const char *p = utf8, *end = utf8 + strlen(utf8);
  int width = 0;
  while (p < end)
    width += advance_of(font, glyph_or_fallback(font, utf8_next(&p, end)));
  return width;
}

int draw_text(const Raster &r, const Font &font, int x, int top, const char *utf8, Ink foreground) {
  if (utf8 == nullptr)
    return 0;
  const char *p = utf8, *end = utf8 + strlen(utf8);
  int pen = x;
  while (p < end) {
    const Glyph *g = glyph_or_fallback(font, utf8_next(&p, end));
    if (g != nullptr) {
      blit_1bpp(r, pen + g->left, top + font.baseline + g->top, g->width, g->height, font.bitmap + g->offset,
                (g->width + 7) / 8, foreground);
    }
    pen += advance_of(font, g);
  }
  return pen - x;
}

int centered_top(const Font &font, const char *utf8, int center_y) {
  int top_bearing = 0, bottom_bearing = 0;
  bool any = false;
  if (utf8 != nullptr) {
    const char *p = utf8, *end = utf8 + strlen(utf8);
    while (p < end) {
      const Glyph *g = glyph_or_fallback(font, utf8_next(&p, end));
      if (g == nullptr)
        continue;
      int t = g->top, b = g->top + g->height;
      if (!any || t < top_bearing)
        top_bearing = t;
      if (!any || b > bottom_bearing)
        bottom_bearing = b;
      any = true;
    }
  }
  if (!any)
    return center_y - font.size / 2;
  return (int) std::lround(center_y - font.baseline - (top_bearing + bottom_bearing) / 2.0);
}

// ---- Layout ----

// Drawing order: row by row, left then right (section 2.2).
static const CellLayout CELLS[METRIC_COUNT] = {
    {Metric::PM2_5, 12, 40, "PM2.5", 30}, {Metric::CO2, 204, 40, "CO2", 24},
    {Metric::PM10, 12, 104, "PM10", 30},  {Metric::VOC, 204, 104, "VOC", 24},
    {Metric::PM1, 12, 168, "PM1", 30},    {Metric::NOX, 204, 168, "NOx", 24},
    {Metric::TEMP, 12, 232, "TEMP", 30},  {Metric::HUM, 204, 232, "HUM", 24},
};

static constexpr int INNER_PADDING = 8;    // label_x = x + 8; inner right = x + 176
static constexpr int UNIT_GAP = 6;         // value advance box to unit origin
static constexpr int LABEL_GAP = 4;        // label advance box to value advance box, minimum
// 16 glyphs of 12 px end at x = 204, clear of the widest stamp
// ("SENSOR MISSING", 168 px, from x = 220).
static constexpr int TITLE_MAX_GLYPHS = 16;
static constexpr int HEADER_TOP = 3;  // 24 px text: caps 7..21, descenders to 26, rule at 30

const CellLayout &cell_layout(size_t index) { return CELLS[index < METRIC_COUNT ? index : 0]; }

const CellLayout &cell_layout_for(Metric metric) {
  for (const auto &c : CELLS)
    if (c.metric == metric)
      return c;
  return CELLS[0];
}

static int unit_x_of(const CellLayout &c) { return c.x + CELL_WIDTH - INNER_PADDING - c.unit_width; }
static int value_right_of(const CellLayout &c) { return unit_x_of(c) - UNIT_GAP; }

int value_field_width(Metric metric) {
  const CellLayout &c = cell_layout_for(metric);
  int label_end = c.x + INNER_PADDING + measure_text(FONT_TEXT24, c.label);
  return value_right_of(c) - (label_end + LABEL_GAP);
}

bool value_fits(Metric metric, const char *text) {
  return measure_text(FONT_BOLD32, text) <= value_field_width(metric);
}

// ---- Page ----

struct CellInks {
  Ink background, label, value;
};

static CellInks inks_for(CellState state) {
  switch (state) {
    case CellState::ELEVATED:
    case CellState::FAULT:
    case CellState::OVERFLOW:
      return {Ink::YELLOW, Ink::BLACK, Ink::BLACK};  // red on yellow reads poorly
    case CellState::HIGH:
      return {Ink::RED, Ink::WHITE, Ink::WHITE};
    case CellState::NORMAL:
    case CellState::PLACEHOLDER:
    default:
      return {Ink::WHITE, Ink::RED, Ink::BLACK};
  }
}

static const char *text_unit(Metric m, bool fahrenheit) {
  switch (m) {
    case Metric::TEMP:
      return fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
    case Metric::CO2:
      return "ppm";
    case Metric::VOC:
    case Metric::NOX:
      return "idx";
    case Metric::HUM:
      return "%";
    default:
      return nullptr;  // PM cells use the sprite
  }
}

static bool is_pm(Metric m) { return m == Metric::PM2_5 || m == Metric::PM10 || m == Metric::PM1; }

// The title, shortened to 16 glyphs with "..." (section 2.1).
static void shorten_title(const char *title, char *out, size_t size) {
  out[0] = '\0';
  if (title == nullptr || size == 0)
    return;
  size_t glyphs = utf8_length(title);
  const char *p = title, *end = title + strlen(title);
  size_t keep = glyphs <= TITLE_MAX_GLYPHS ? glyphs : TITLE_MAX_GLYPHS - 3;
  for (size_t i = 0; i < keep && p < end; i++)
    utf8_next(&p, end);
  size_t bytes = (size_t) (p - title);
  if (bytes >= size)
    bytes = size - 1;
  memcpy(out, title, bytes);
  out[bytes] = '\0';
  if (glyphs > TITLE_MAX_GLYPHS && bytes + 3 < size)
    strcat(out, "...");
}

static void draw_cell(const Raster &r, const CellLayout &c, const Cell &cell, bool fahrenheit) {
  CellInks inks = inks_for(cell.state);
  fill_rect(r, c.x, c.y, CELL_WIDTH, CELL_HEIGHT, inks.background);
  const int center_y = c.y + CELL_HEIGHT / 2;

  draw_text(r, FONT_TEXT24, c.x + INNER_PADDING, centered_top(FONT_TEXT24, c.label, center_y), c.label, inks.label);

  // The snapshot decided overflow already; never draw a value wider than its
  // field, whatever the caller passed.
  const char *value = cell.text;
  if (cell.state == CellState::OVERFLOW || !value_fits(c.metric, value))
    value = "----";
  int value_w = measure_text(FONT_BOLD32, value);
  draw_text(r, FONT_BOLD32, value_right_of(c) - value_w, centered_top(FONT_BOLD32, value, center_y), value,
            inks.value);

  int unit_x = unit_x_of(c);
  if (is_pm(c.metric)) {
    if (state_has_value(cell.state) || cell.state == CellState::OVERFLOW) {
      blit_1bpp(r, unit_x, c.y + (CELL_HEIGHT - SPRITE_PM_UNITS.height) / 2, SPRITE_PM_UNITS.width,
                SPRITE_PM_UNITS.height, SPRITE_PM_UNITS.data, SPRITE_PM_UNITS.stride, inks.value);
    }
  } else {
    const char *unit = text_unit(c.metric, fahrenheit);
    // Degree and percent fit their unit column at 24 px; "ppm" and "idx" do not.
    const Font &font = (c.metric == Metric::TEMP || c.metric == Metric::HUM) ? FONT_TEXT24 : FONT_TEXT14;
    draw_text(r, font, unit_x, centered_top(font, unit, center_y), unit, inks.value);
  }
}

void draw_dashboard(const Raster &r, const Snapshot &snapshot) {
  if (r.put_pixel == nullptr || r.width < CANVAS_WIDTH || r.height < CANVAS_HEIGHT)
    return;  // the fixed page needs the full 400 x 300 canvas
  fill_rect(r, 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, Ink::WHITE);

  char title[sizeof(snapshot.title) + 4];
  shorten_title(snapshot.title, title, sizeof(title));
  draw_text(r, FONT_TEXT24, 12, HEADER_TOP, title, Ink::BLACK);
  int stamp_w = measure_text(FONT_TEXT24, snapshot.stamp);
  // A sensor error replaces the stamp, in red: the one status word on the page.
  draw_text(r, FONT_TEXT24, 388 - stamp_w, HEADER_TOP, snapshot.stamp, snapshot.stamp_alert ? Ink::RED : Ink::BLACK);
  hline(r, 12, 30, 376, Ink::BLACK);

  for (size_t i = 0; i < METRIC_COUNT; i++) {
    const CellLayout &c = cell_layout(i);
    draw_cell(r, c, snapshot.cells[metric_index(c.metric)], snapshot.fahrenheit);
  }
}

}  // namespace esphome::aqi_dashboard
