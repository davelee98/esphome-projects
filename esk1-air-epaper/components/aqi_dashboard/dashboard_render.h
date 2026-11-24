#pragma once

// Primitive renderer for the AQI dashboard (docs/aqi-dashboard-specification.md
// section 6). Plain C++17 with no ESPHome dependency, so the same code draws
// into a host test buffer and into the driver's frame buffer.
//
// Everything is integer-positioned and 1-bit: solid rectangles, transparent
// 1-bpp bitmaps and native Terminus glyphs. Nothing allocates while drawing.

#include <cstddef>
#include <cstdint>

#include "dashboard_assets.h"
#include "dashboard_model.h"

namespace esphome::aqi_dashboard {

// A pixel sink. put_pixel is only ever called with 0 <= x < width and
// 0 <= y < height: every primitive clips first.
struct Raster {
  void *context;
  void (*put_pixel)(void *context, int x, int y, Ink ink);
  int width;
  int height;
};

// Half-open rectangle (x, y, w, h). Non-positive sizes draw nothing.
void fill_rect(const Raster &r, int x, int y, int w, int h, Ink ink);
void hline(const Raster &r, int x, int y, int width, Ink ink);
// Transparent 1-bpp bitmap, MSB first, rows `stride` bytes apart: set bits
// are drawn in `foreground`, clear bits leave the canvas untouched.
void blit_1bpp(const Raster &r, int x, int y, int w, int h, const uint8_t *data, int stride, Ink foreground);

// Decodes one UTF-8 code point from [*p, end) and advances *p. Malformed
// input yields U+FFFD and consumes one byte.
uint32_t utf8_next(const char **p, const char *end);
// Number of code points in a NUL-terminated UTF-8 string.
size_t utf8_length(const char *utf8);
const Glyph *find_glyph(const Font &font, uint32_t codepoint);
// Glyph used for a code point: the glyph itself, else '?', else nullptr
// (which draws nothing and advances by half the font size).
const Glyph *glyph_or_fallback(const Font &font, uint32_t codepoint);
// Sum of advances, in pixels.
int measure_text(const Font &font, const char *utf8);
// Draws from the pen position (x, top-of-text); returns the advance drawn.
int draw_text(const Raster &r, const Font &font, int x, int top, const char *utf8, Ink foreground);
// Top of text whose ink is vertically centred on center_y (section 6.2).
int centered_top(const Font &font, const char *utf8, int center_y);

// ---- Fixed dashboard geometry (section 2) ----
static constexpr int CANVAS_WIDTH = 400;
static constexpr int CANVAS_HEIGHT = 300;
static constexpr int CELL_WIDTH = 184;
static constexpr int CELL_HEIGHT = 60;

struct CellLayout {
  Metric metric;
  int x, y;
  const char *label;
  int unit_width;  // reserved unit column: 30 left, 24 right
};
// The eight cells in drawing order.
const CellLayout &cell_layout(size_t index);
const CellLayout &cell_layout_for(Metric metric);
// Pixels available to the 32 px value of a metric's cell (section 2.3).
int value_field_width(Metric metric);
// True if `text` fits that field in the bold 32 px font.
bool value_fits(Metric metric, const char *text);

// Draws the whole page from a snapshot (section 6.3). Reads nothing else.
void draw_dashboard(const Raster &r, const Snapshot &snapshot);

}  // namespace esphome::aqi_dashboard
