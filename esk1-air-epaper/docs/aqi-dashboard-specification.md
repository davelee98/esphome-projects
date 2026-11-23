# AQI dashboard specification and primitive renderer

Status: implementation specification, 23 November 2026.

This document defines the target 400 × 300 BWRY dashboard for the
`esk1-air-epaper` custom component. It consolidates the current preview and
describes a renderer built from integer-coordinate rectangles and transparent
1-bit bitmaps. The rendering core can use C-compatible data and functions; its
ESPHome integration will be C++ inside `components/opendisplay/`.

The visual requirements below reflect the selected design. Refresh scheduling,
classification thresholds and change deadbands are **proposed implementation
defaults**, explicitly identified below. They are not claims about the current
firmware or previously approved environmental limits.

## 1. Scope and existing implementation

The dashboard displays individual air-quality readings. “AQI dashboard” is the
project name for this page; it does not calculate or claim an official AQI.
Instantaneous PM readings and gas indices must not be labelled as a regulatory
air-quality index.

| Item | Requirement |
|---|---|
| Canvas | 400 × 300 physical pixels, landscape, origin at upper left |
| Palette | Black, white, red and yellow; no intermediate shades |
| Transport | OpenDisplay DIRECT_WRITE over BLE; complete frames |
| Rendering | Solid rectangles, one horizontal rule, native monochrome glyphs and one PM-unit bitmap |
| Fonts | One family, Terminus; two runtime assets: normal 14 px and bold 32 px |
| Panel refresh | Approximately 30 seconds on the target BWRY panel, as observed by the project owner |
| Sensor sampling | Keep the existing 60-second SEN65 sampling interval |
| Data publishing | Keep all existing sensor reporting, including PM4, independent of display refresh |

The [current YAML](../esk1-air-epaper.yaml) still has a five-minute display
polling interval and an older drawing lambda. This document specifies their
replacement; adding this document does not implement that replacement.
The [existing driver](../components/opendisplay/opendisplay.cpp) already
provides a framebuffer, unchanged-frame detection, BLE transfer, compression,
panel discovery and refresh acknowledgements. Reuse those facilities.

The visual reference is [sen65-status-preview.html](../../output/sen65-status-preview.html).
The corresponding image is
[sen65-native-numeric-preview.png](../../output/sen65-native-numeric-preview.png).
The HTML has sample scenarios, not a production sensor classifier. Historical
preview files are not authoritative for this specification. Where an existing
preview differs, this specification takes precedence, including the 32-pixel
overflow policy and default red metric labels.

## 2. Exact page geometry

All rectangles use half-open bounds: `(x, y, width, height)` covers
`x <= pixel_x < x + width` and `y <= pixel_y < y + height`.

### 2.1 Header

| Element | Position and rendering |
|---|---|
| Background | Fill `(0, 0, 400, 300)` white before drawing anything else |
| Room title | Text top origin `(12, 7)`, Terminus normal 14, black, left aligned; default `INDOOR AIR` |
| Sample stamp | Text top origin `(388, 7)`, Terminus normal 14, black, right aligned |
| Stamp format | `DD MON HH:MM`, for example `23 NOV 14:35`; uppercase English month abbreviation and 24-hour clock |
| Header rule | Fill `(12, 30, 376, 1)` black; last painted pixel is `(387, 30)` |

Keep the title and stamp on one line. Limit the configurable title to 24
supported characters; shorten longer titles with three ASCII periods within
that limit. Do not overlap the stamp. There is no SEN65 label, footer,
connectivity icon or visible status sentence.

The timestamp describes the captured SEN65 sample, not the time of each
one-second scheduler tick. If a frame includes readings with different sample
times, retain those times internally; the header is not a guarantee that every
source was sampled simultaneously. Use the newest accepted SEN65 batch time,
including on a stale-data page so its age remains visible. Until any sample has
arrived, show `NO SAMPLE YET`. If samples exist but wall-clock time is unknown,
show `TIME UNKNOWN`. Do not fabricate a date from uptime. If time becomes valid
after a sample, stamp a subsequent fresh sample rather than guessing its date.

Freeze the stamp with the frame snapshot. Do not update it every minute just to
make the frame hash change. A heartbeat may capture a newer sample timestamp.
Use an explicitly configured timezone; elapsed-time scheduling uses a monotonic
clock, not this wall clock.

### 2.2 Eight equal cells

Each cell is **184 × 60 pixels**. The two column origins are `12` and `204`.
Rows begin at `40`, `104`, `168` and `232`. Column gutter: 8 pixels; row gutter:
4 pixels. The final row ends at y = 292, leaving 8 pixels below it.

| Row | Left cell `(x,y)` | Left label | Right cell `(x,y)` | Right label |
|---|---|---|---|---|
| 1 | `(12,40)` | `PM2.5` | `(204,40)` | `CO2` |
| 2 | `(12,104)` | `PM10` | `(204,104)` | `VOC` |
| 3 | `(12,168)` | `PM1` | `(204,168)` | `NOx` |
| 4 | `(12,232)` | `TEMP` | `(204,232)` | `HUM` |

PM4 is not drawn. TEMP and HUM have separate cells and textual labels, without
icons. There are no cell outlines, rounded corners, vertical rules or additional
horizontal rules. A colored status area is the whole cell's filled rectangle.

### 2.3 Inline labels, readings and unit columns

Each cell contains one horizontal line: **label, right-aligned value,
left-aligned unit**. All three are vertically centered at `cell_y + 30`.
Keep the selected 8-pixel inner horizontal padding.

Render all eight metric labels (`PM2.5`, `PM10`, `PM1`, `CO2`, `VOC`, `NOx`,
`TEMP` and `HUM`) in red by default, including on yellow cells. On red cells,
render the label in white so it remains visible. This label color is a fixed
style, not a concentration status indicator. Values and units retain their
state-dependent colors. The header title, timestamp and rule remain black.

| Anchor | Left column | Right column |
|---|---:|---:|
| Label left x | 20 | 212 |
| Number right x, exclusive | 152 | 350 |
| Unit left x | 158 | 356 |
| Inner right edge, exclusive | 188 | 380 |
| Reserved unit width | 30 | 24 |

Use these anchors for every row, including TEMP and HUM. Short units do not
shift toward the right edge. The gap between the number's advance box and unit
origin is exactly 6 pixels. The label-to-number advance-box gap is at least
4 pixels; native bitmap whitespace may make the visible ink gap larger.

For a cell at x:

```c
label_x = x + 8;
inner_right = x + 176;
unit_x = inner_right - unit_column_width;
value_right = unit_x - 6;
available_value_width = value_right - (label_x + label_width + 4);
```

At the 14-pixel font's 8-pixel advance, the available numeric widths are:

| Label | Available pixels | Characters at 32 px |
|---|---:|---:|
| PM2.5 | 88 | 5 |
| PM10 | 96 | 6 |
| PM1 | 104 | 6 |
| TEMP | 96 | 6 |
| CO2 / VOC / NOx / HUM | 110 | 6 |

Always use native 32-pixel bold at scale 1 for readings. A four-digit CO2 value
such as `9999` needs only 64 pixels and fits without any layout change.
Count the minus sign and decimal point toward the character limit. Check the
width after rounding, since rounding can add a digit. These limits describe
layout capacity; source validity ranges still apply independently.

If the complete formatted value exceeds the available width, show the error
marker `----` in 32-pixel bold at the same right anchor and record an overflow
diagnostic. Its four glyphs occupy 64 pixels and fit every cell. This is
distinct from the `--` missing-value marker and needs no additional glyphs.
Use a yellow background with a red label and black value and unit. Keep the unit visible,
including the PM sprite, to identify the affected measurement. Do not fall back
to 24 px, reduce padding, truncate digits or scale the bitmap. Preserve the raw
reading and its concentration category in telemetry; only its display state
becomes overflow. Entering or leaving overflow is a display-state transition
for refresh scheduling, subject to the physical refresh guard.

## 3. Font and bitmap assets

### 3.1 Native fonts

Use the unmodified Terminus 4.49.1 BDF inputs under
[fonts/terminus/](../fonts/terminus/). Pixel sizes are native bitmap heights,
not typographic point sizes. The selected native sizes supersede the earlier
integer-multiple scaling experiments. Do not synthesize bold or resample.

| Runtime role | Source | Cell / advance | Baseline from text top | Glyphs | Bitmap bytes |
|---|---|---|---:|---:|---:|
| Header, labels, text units | `ter-u14n.bdf` | 8 × 14 / 8 | 12 | 98 | 1,372 |
| Main numeric values | `ter-u32b.bdf` | 16 × 32 / 16 | 26 | 17 | 1,088 |

The text subset is ASCII U+0020 through U+007E plus `°`, `µ` and `³`.
The exact numeric subset is `0123456789.- °FC%`, including the space. The
numeric assets support missing values, decimals and negative temperatures.
Units are nevertheless rendered separately in the small font or PM bitmap.

The [upstream Terminus page](https://terminus-font.sourceforge.net/) documents
the native bitmap sizes. Keep [OFL.TXT](../fonts/terminus/OFL.TXT) and
[SOURCE.txt](../fonts/terminus/SOURCE.txt) with redistributed assets. Spleen,
Roboto and browser fonts are not part of this renderer.

### 3.2 PM-unit sprite

The PM unit is one transparent **30 × 12** bitmap containing `µg/m³` on a
single line. Its source is Terminus normal 6 × 12, composed at build time.
This is a sprite, not an additional runtime font. Use
[pm-units.h](../fonts/pm-units.h); the PNG is only a visual reference.

Rows are byte padded, MSB first: `ceil(30 / 8) = 4` bytes per row, 48 bytes
total. The final two padding bits in each row are not pixels. Draw set bits in
the value/unit foreground color and leave zero bits transparent. Place its upper
left at `(158, cell_y + 24)` for each PM row. This centers its 12-pixel height
on the same line as label and number. Omit the sprite when the PM value is
`--`, matching the preview.

| Reading | Unit asset | Unit origin x |
|---|---|---:|
| PM2.5, PM10, PM1 | 30 × 12 PM sprite | 158 |
| TEMP | `°C` or `°F`, normal 14 | 158 |
| CO2 | `ppm`, normal 14 | 356 |
| VOC, NOx | `idx`, normal 14 | 356 |
| HUM | `%`, normal 14 | 356 |

Text units remain visible beside `--`. Do not write `INDEX` or append a unit
to a large numeric string. The optional climate icon assets are not used.

### 3.3 Generation and storage

From the repository root, regenerate the existing assets with:

```sh
python esk1-air-epaper/tools/build_display_fonts.py --sheet output/numeric-glyphs.png
```

[build_display_fonts.py](../tools/build_display_fonts.py) reads the original
BDF bitmap rows directly. Pillow is needed for its PNG output. The generated
[display-glyphs.json](../fonts/display-glyphs.json) supplies all three fonts to
the preview; [numeric-glyphs.h](../fonts/numeric-glyphs.h) currently supplies
only the two numeric fonts to a C renderer. Those existing files still contain
the earlier 24-pixel fallback; the target dashboard uses only 14 and 32 px.

Before implementing the standalone primitive renderer, extend the generator to
emit an equivalent 14-pixel C table and a dashboard asset set containing only
the 14- and 32-pixel fonts. Exclude the unused 24-pixel table from the linked
dashboard build. JSON, BDF and PNG parsing belong on the
development computer, never on the microcontroller. Include generated static
arrays in one translation unit, with declarations elsewhere, to prevent
duplicate copies. Do not also compile duplicate ESPHome font assets if the
custom renderer owns those same glyph tables.

## 4. Values, source validity and color

### 4.1 Source and formatting contract

The [SEN65 product specification](https://sensirion.com/products/catalog/SEN65)
identifies PM, temperature, humidity, VOC and NOx channels. **SEN65 has no CO2
channel.** CO2 must come from an optional, explicitly bound external sensor.
Without one, retain the CO2 cell as `CO2 -- ppm` on white. Never infer CO2
from either gas index.

| Metric | Input | Visible value | Required handling |
|---|---|---|---|
| PM2.5, PM10, PM1 | µg/m³ | Nearest integer, e.g. `12` | Zero is valid; negative values are invalid |
| CO2 | External ppm | Nearest integer, including four digits | Missing source is unavailable; reject negative input |
| VOC, NOx | Sensor index | Nearest integer, e.g. `125` | Preserve index semantics; honor driver validity and warm-up |
| TEMP | Sensor °C | One decimal, e.g. `22.4` or `-5.2` | Convert to °F before rounding when configured |
| HUM | % relative humidity | Nearest integer, e.g. `46` | Valid input range 0 through 100 inclusive |

Reject NaN, infinity and values outside the configured source's valid range
before formatting. Do not treat every zero as missing. Use explicit source
validity, and honor any documented gas-index startup sentinel. Keep raw values
for publishing and deadband comparisons. Formatting must not modify them.

Use a consistent rounding rule, equivalent to `lround` at the displayed
precision. Avoid negative zero by formatting a rounded zero as `0.0` for
temperature. Use an ASCII period and minus sign; never use scientific notation
or thousands separators. Use bounded stack buffers, for example 16 bytes per
value, and check formatting success and width. An invalid input becomes `--`;
a valid reading whose formatted output exceeds the 32-pixel field becomes
`----`. If a bounded formatting buffer would truncate a valid reading, treat
that as overflow too. Never display a plausible clipped reading.

Each metric needs a validity state and its own monotonic last-sample time.
An external CO2 source can be fresh when the SEN65 is stale, or vice versa.

### 4.2 Visual states

| State | Cell background | Label | Value and unit | Value |
|---|---|---|---|---|
| Normal valid reading | White | Red | Black | Formatted reading |
| Elevated valid reading | Yellow | Red | Black | Formatted reading |
| High valid reading | Red | White | White | Formatted reading |
| Starting / never sampled | White | Red | Black | `--` |
| Optional source not configured | White | Red | Black | `--` |
| Configured source stale, invalid or faulted | Yellow | Red | Black | `--` |
| Formatted reading exceeds the 32-pixel field capacity | Yellow | Red | Black | `----` |

There are no visible status words. Retain detailed states in component logs or
diagnostics. A red cell background is reserved for a high valid reading, so a
missing sensor does not resemble a pollution measurement. The default red
labels do not indicate high readings. Invalid data overrides classification.
Do not leave an old numeric value visible as though it were current once its
source has become stale and a new page can be sent.

The current preview colors the three PM cells together from its PM status.
Retain that behavior: use the PM2.5 category for valid PM2.5, PM10 and PM1
cells. A missing individual PM value still gets its own invalid appearance.
If PM2.5 is unavailable, do not invent PM1/PM10 health cutoffs; show their
valid readings on white and report the missing classification diagnostically.
TEMP and HUM remain white for valid values. CO2 classification is disabled by
default, including after a source is bound, until its thresholds are configured.

### 4.3 Proposed classification parameters

These are configurable **project event bands for initial implementation and
tuning**, not official AQI breakpoints, exposure limits or medical advice.
They are not implemented by the current preview or driver.

| Classifier | Enter yellow at or above | Enter red at or above | Leave yellow below | Leave red below |
|---|---:|---:|---:|---:|
| PM group, using PM2.5 µg/m³ | 15 | 35 | 12 | 30 |
| VOC index | 150 | 400 | 130 | 350 |
| NOx index | 20 | 300 | 15 | 250 |
| Optional CO2 ppm, example only; disabled by default | 1,000 | 1,500 | 900 | 1,300 |

Classify unrounded values. From normal, enter the highest crossed entry band.
From elevated, enter high at its entry threshold, otherwise remain elevated
until below the yellow exit threshold. From high, remain high until below the
red exit threshold; then select elevated unless also below the yellow exit
threshold. Equality at an exit threshold retains the current category.

Require a candidate category to remain unchanged for 180 seconds of fresh
samples before committing a category transition, upward or downward. Start the
timer at the first qualifying sample, not one sampling interval before it.
Reset it if the candidate changes, validity is lost, or the sample stream goes
stale. At one-minute sampling, four qualifying timestamps span three minutes.
Initial classification of the first valid sample does not need this delay.

Stale is proposed as `now - last_sample_time > 180 seconds` with the current
60-second sampling interval. Evaluate this without waiting for another sensor
callback. Availability changes are independent of concentration-band debounce.

## 5. Proposed refresh and change policy

The user rejected a five-minute refresh because a BWRY update takes about
30 seconds. Sampling, deciding to redraw and physically refreshing are separate
operations. Keep frequent sensor readings while making panel updates sparse.

### 5.1 Timing defaults

| Parameter | Proposed default | Meaning |
|---|---|---|
| Sensor sampling | 60 s | Existing sensor setting; does not imply a display update |
| Scheduler check | 1 s, nonblocking | Checks monotonic deadlines; does not render or connect by itself |
| Routine update eligibility | 30 min since last successful display | Send only if a meaningful numeric change is pending |
| Heartbeat | 60 min since last successful display | Capture current values and refresh even if stable |
| Category persistence | 3 min | Required before committing a concentration-category change |
| Physical refresh guard | 15 min | Minimum interval between possible panel-refresh starts |
| Source stale timeout | More than 180 s | A configured source has missed expected updates |
| Initial startup wait | Up to 180 s | Prefer the first sensor batch over immediately painting placeholders |

A committed category, availability or overflow transition may request an earlier update,
subject to the 15-minute guard. A routine numeric change alone waits for the
30-minute deadline. Heartbeat bypasses numeric deadbands, but not the guard.
Manual refresh bypasses numeric deadbands and the routine deadline, but still
queues behind the guard and any active upload. Multiple requests coalesce.

On cold boot, with no display history in RAM, send the first usable SEN65 batch
once the panel is identified. If none arrives within 180 seconds, one starting
or fault page may be sent. A subsequent valid batch then follows the normal
guard; do not repeatedly repaint warm-up placeholders. If persistent state is
later added, it must not pretend an old snapshot was displayed after the label
has been replaced or its identity changed.

The physical guard is measured conservatively from the point where a refresh
could begin, including a transfer auto-completion or an uncertain END outcome.
Set a guard timestamp no later than handing off the final data chunk, a START
containing the entire payload, or an END command that can trigger refresh.
A lost success acknowledgement must not lead
to another 30-second refresh immediately afterward. Measure routine and
heartbeat ages from confirmed success instead.

This page therefore shows a retained snapshot, not every transient event. Both
the cooldown and the approximately 30-second waveform contribute to visible
latency. It is not a real-time alarm display.

### 5.2 Proposed meaningful-change deadbands

Compare each latest valid raw value with the corresponding value in the last
**successfully displayed snapshot**, not the previous sensor sample.

| Metric | Proposed minimum absolute change |
|---|---:|
| Each displayed PM value | `max(2 µg/m³, 20% of abs(displayed value))` |
| CO2, if configured | 100 ppm |
| VOC | 25 index points |
| NOx | 10 index points |
| TEMP | 0.5 °C, or equivalent 0.9 °F |
| HUM | 3 percentage points |

Equality meets a deadband. Numeric eligibility additionally requires that the
formatted visible string has changed; a raw change hidden by rounding must
not cause an otherwise identical numeric update. At each decision, recompute
whether any metric currently qualifies. A brief excursion that returns to the
displayed value must not leave a permanent dirty flag. Small cumulative drift
will eventually exceed a deadband relative to the displayed snapshot.

Category, availability and overflow-state changes bypass numeric deadbands. A routine
refresh, transition refresh or heartbeat captures all current readings together;
do not keep some cells artificially old just because their own deadband was not
crossed. The heartbeat also refreshes small changes accumulated below every
deadband. Keep these settings configurable and separate from display precision.

### 5.3 Snapshot and completion lifecycle

Maintain small metadata structures for:

1. `latest`: newest values, sample times, validity and committed categories.
2. `in_flight`: immutable snapshot and frozen stamp currently being transmitted.
3. `displayed`: last snapshot confirmed by panel refresh success.

These are value records, not three pixel buffers. Sensor callbacks update
`latest` during a transfer, but must never redraw or mutate `in_flight`.
After successful refresh, promote `in_flight` to `displayed`, record the success
time, and reevaluate requests against the newest values and deadlines.

The existing driver's `finish_(true, ...)` is reached on
`RESP_DIRECT_WRITE_REFRESH_SUCCESS`; use that confirmed outcome as the commit
point. Sending bytes, receiving END_ACK or seeing the nominal 30 seconds elapse
is insufficient. On failure, retain the prior `displayed` metadata and mark
delivery uncertain if the panel might already have refreshed. Do not clear a
needed update or advance the successful-display deadline.

Proposed transport retry backoff is 2, 5 and then 15 minutes, capped at
15 minutes, further limited by any physical-refresh guard. Only one retry may
be pending. Retries within a transaction, including compressed-to-raw fallback,
use its frozen snapshot. After a failed transaction, the next scheduled attempt
captures current data after reevaluation, rather than repeatedly sending a
snapshot that has since become stale. Never
interleave two transfers or retry every one-second scheduler tick.

The panel retains its previous image through controller power loss. Firmware
cannot paint a stale warning while the controller or BLE path is unavailable;
the retained absolute timestamp is the available freshness clue.

## 6. Primitive rendering interface

### 6.1 Core API

The following names are a proposed internal API, not existing exported driver
methods. The core should be deterministic, allocation-free during drawing, and
independent of BLE or sensor objects.

```c
typedef enum { INK_BLACK, INK_WHITE, INK_YELLOW, INK_RED } Ink;

typedef struct {
  void *context;
  void (*put_pixel)(void *context, int x, int y, Ink ink);
  int width;
  int height;
} Raster;

void fill_rect(Raster *r, int x, int y, int w, int h, Ink ink);
void hline(Raster *r, int x, int y, int width, Ink ink);
void blit_1bpp(Raster *r, int x, int y, int w, int h,
               const uint8_t *data, int stride, Ink foreground);
/* Font is a static descriptor over glyph metrics and bitmap bytes. */
int measure_text(const Font *font, const char *utf8);
void draw_text(Raster *r, const Font *font, int x, int top,
               const char *utf8, Ink foreground);
void draw_dashboard(Raster *r, const DashboardSnapshot *snapshot);
```

Only `put_pixel` is required from the display adapter. `fill_rect` can be a
clipped nested loop or use a driver rectangle operation; `hline` is a rectangle
of height 1. Transparent bitmap blits and text reduce to setting foreground
pixels, optionally coalescing adjacent pixels into horizontal runs.

Use signed intermediate coordinates and clip before touching buffer memory.
Reject nonpositive rectangle dimensions. A clipping operation must adjust
source coordinates correctly, not restart a clipped bitmap at source column 0.
All text and bitmaps use integer positions, no alpha blending or antialiasing.
No runtime font engine, SVG, HTML, image decoder, layout engine or UI framework
is needed.

### 6.2 Bitmaps and glyph positioning

For every source row and column in a transparent 1-bpp bitmap:

```c
if (data[row * stride + (column >> 3)] & (0x80u >> (column & 7))) {
  /* Adapter clips this destination pixel to the canvas. */
  raster_put_pixel(r, x + column, y + row, foreground);
}
```

For a glyph, use `stride = (glyph.width + 7) / 8` and start at its bitmap
offset. Preserve the generated advance and bearing metrics:

```c
glyph_x = pen_x + glyph.left;
glyph_y = text_top + font.baseline + glyph.top;
pen_x += glyph.advance;
```

`glyph.top` is signed and relative to the baseline. The current native glyphs
have full cell-height bitmaps, including blank rows: do not trim them and then
reuse the old bearings. Width measurement is the sum of glyph advances.
For right alignment, subtract that width from the exclusive right anchor
before drawing. For left alignment, draw directly at the anchor.

Center each string with the same metric rule as the preview:

```text
top_bearing = minimum glyph.top in the string
bottom_bearing = maximum (glyph.top + glyph.height) in the string
text_top = round(center_y - font.baseline
                 - (top_bearing + bottom_bearing) / 2)
```

For the current full-height native tables this simplifies to
`text_top = center_y - font.size / 2`. Thus at first-row center y = 70, the
14- and 32-pixel font tops are 63 and 54 respectively, and the PM
sprite top is 64. The header uses its specified top of 7 instead of centering.

Decode bounded UTF-8 into code points; `strlen()` counts bytes, not glyphs.
The degree sign in `°C`, for example, is two UTF-8 bytes but one 8-pixel glyph.
Validate configurable text against the text subset. Substitute `?` for an
unsupported text glyph; invalid numeric content must become `--`. Do not
silently borrow a system font or read past a glyph table.

### 6.3 Draw order

1. Validate panel dimensions and palette before drawing.
2. Clear the full canvas white.
3. Draw room title, frozen timestamp and header rule.
4. For each of eight cells, format and measure its value at native 32 px. If it
   exceeds the field width, replace it with `----` and select overflow colors.
5. Fill the entire cell in its selected background.
6. Draw its normal-14 label in red, or white on a red background. Draw its bold
   value and transparent unit in black, or white on a red background. Select
   label ink separately from value/unit ink; bitmap drawing uses the latter.
7. Return the completed logical frame to the existing transport code.

Drawing the same snapshot must produce the same pixels, with no reading of
the live clock or sensor objects partway through drawing. A full redraw is
cheap in MCU time; optimize physical refresh frequency rather than introducing
dirty-rectangle rendering for this panel.

### 6.4 BWRY adapter

Use a four-value logical ink type throughout the renderer. The current driver's
logical two-bit codes are black 0, white 1, yellow 2 and red 3. Those are not a
promise about the physical panel's wire encoding: the driver remaps BWRY bytes
to the panel's color scheme for upload and restores them afterward.

Implement the C++ adapter through the component's existing drawing path.
Map black and white to its established `COLOR_ON` and `COLOR_OFF` conventions,
and map yellow/red through its supported color conversion. Do not assume an
all-zero RGB `Color` means black in a monochrome-style display API. Keep wire
color conversion out of the glyph renderer and do not apply the BWRY LUT twice.

The generic driver can adopt dimensions reported by other labels. This fixed
dashboard requires exactly 400 × 300 BWRY. Validate after discovery and again
after configuration changes; refuse this page on an incompatible panel with a
clear diagnostic. Do not stretch the geometry or write out of bounds. Preserve
the driver's generic panel support for other callers.

Every upload is a full frame. `FAST` is a waveform selection, not a partial
update facility. Use `FULL` for the target behavior unless a separately tested
panel-specific waveform is deliberately configured.

## 7. Flash and RAM budget

The target 14- and 32-pixel glyph pixels total 2,460 bytes. Including the PM
sprite gives 2,508 bytes. The existing development assets also contain an
unused 24-pixel font, which must be excluded from the dashboard build.
For a compact C table matching the current numeric glyph struct,
the estimated static data is:

| Item | Bytes |
|---|---:|
| 14-pixel text glyph pixels | 1,372 |
| 32-pixel numeric glyph pixels | 1,088 |
| PM sprite pixels | 48 |
| 115 glyph records × 16 bytes | 1,840 |
| Two font descriptors × 16 bytes | 32 |
| **Total font and sprite data** | **4,380 bytes, approximately 4.28 KiB** |

This assumes 32-bit pointers, the current 16-byte glyph layout and an equivalent
16-byte descriptor for the text font. Confirm `sizeof` on ESP32 and check the
linked map file; this is not a measurement of total firmware growth. Renderer
code, strings, layout descriptors and alignment add flash. A different ESPHome
font representation has different metadata overhead. BDF, JSON, HTML and PNG
development file sizes do not count as embedded asset sizes unless mistakenly
included in the build.

The existing 2-bpp framebuffer is `400 × 300 × 2 / 8 = 30,000` bytes of RAM,
already reserved by the driver. Draw directly into it and keep assets `const`
in flash. No second full framebuffer is needed. Snapshot metadata, bounded
formatting buffers and scheduler state should remain well below 1 KiB of
additional RAM; verify the actual structs. Existing BLE and optional compression
buffers are transport costs, additional to the framebuffer, and must remain in
the overall heap budget. Drawing itself must not allocate per glyph or per cell.

## 8. Custom-component integration plan

Keep rendering, policy and transport distinct inside the component so the page
can be verified without a BLE label:

| Proposed module | Responsibility |
|---|---|
| `aqi_dashboard.h/.cpp` | Snapshot types, fixed geometry, formatting, primitive renderer |
| Generated dashboard font table | Const normal-14 and bold-32 glyph data plus PM unit |
| Dashboard scheduler / component glue | Sensor bindings, classification, freshness, deadlines, snapshots |
| Existing `opendisplay.h/.cpp` | Framebuffer, discovery, protocol, compression, ACKs and completion |
| `display.py` | Validated dashboard configuration and code generation |

These are proposed additions; they are not existing public interfaces.
Bind the current PM, VOC, NOx, temperature and humidity sensor IDs. Make CO2 an
optional sensor binding. Expose room title, temperature unit, clock/timezone,
timing parameters, deadbands and event-band configuration. Validate ascending
entry bands, lower exit bands, positive intervals and the fixed target geometry.
For the table above require `leave_yellow < enter_yellow < leave_red < enter_red`
so the hysteresis behavior is unambiguous.

Dashboard mode and a user drawing lambda must be mutually exclusive. Preserve
the generic display mode, name-based label discovery, existing configuration
and web controls. Dashboard mode must not accidentally inherit the old
five-minute unconditional display poll: its scheduler decides when to call the
transport. Sensor reporting continues at its own interval.

Use wrap-safe unsigned elapsed-time comparisons for `millis()`-based intervals,
or a monotonic 64-bit time source. Never compare a wall-clock deadline with an
uptime timestamp. Clock synchronization or daylight-saving changes must not
trigger a burst of panel refreshes.

Required driver integration points:

- A pre-render path that draws only an eligible, frozen dashboard snapshot,
  including when the panel is first learned or its configuration is reread.
- A completion notification carrying confirmed success versus failure. Promote
  snapshots at the same success point as `last_sent_hash_` is committed.
- A one-shot force-upload request for a heartbeat or manual refresh when the
  pixels are identical, such as when wall-clock time is unavailable. Keep
  ordinary `skip_unchanged` behavior enabled. A redraw alone does not guarantee
  a physical refresh.
- A conservative refresh-start marker for the cooldown, including panel-side
  automatic completion when the last data chunk fills its buffer.
- A single pending dashboard request that is reevaluated after completion.
  The current `update_pending_` path must not bypass dashboard deadlines or
  immediately repaint the newest sample simply because a request arrived busy.

Do not redraw the buffer while the driver is transmitting it or while it holds
wire-remapped BWRY pixels. The existing `update()` busy guard and
`finish_()` cleanup provide the appropriate lifecycle boundaries. Distinguish
`draw requested`, `upload started` and `refresh confirmed` in diagnostics.

A manual Refresh E-Paper action should route through the dashboard scheduler,
not call its renderer independently. After cooldown it captures a new snapshot
and requests one full refresh. A sensor tick alone must not turn Bluetooth on.

## 9. Acceptance and verification

### Pixel rendering

- Render deterministic snapshots into a host-side 400 × 300 pixel buffer with
  the same primitive functions. Compare against the current preview for its
  normal, mixed, high, starting, missing-gas and stale examples, applying the
  red-label and overflow requirements here where the preview still differs.
- Check all eight 184 × 60 fills, gutters, header rule and fixed text anchors.
  No cell outlines, footer, status words, PM4 or climate icons may appear.
- Compare source glyph rows byte-for-byte with the selected native BDF assets.
  Numeric weight must be bold; text must remain normal; only two runtime
  font assets may be linked.
- Check CO2 `9999`, PM `0` and `1000`, gas `500`, temperature `-40.0`, a
  six-character negative temperature, HUM `100`, and missing values. Exercise
  deliberate overflow at 32 px, including a value that gains a digit through
  rounding. Verify five-character PM2.5 and six-character other fields fit,
  while one extra character produces right-aligned `----` on yellow with its
  unit retained. Check that no advance boxes
  overlap and both unit columns retain their exact left origins.
- Verify `°C`, `°F`, `%`, `idx`, `ppm` and the PM sprite on white, yellow and
  red fills. Zero bits must preserve the background. Verify row padding and
  edge clipping with canary bytes around the host buffer.
- Verify all eight metric labels are red on white and yellow, including missing
  and overflow states, and white on red. Values and units remain black on white
  and yellow, and white on red. Header text and rule remain black.
- Check long and unsupported room titles, missing time and sample time around
  midnight. Header layout must remain bounded and use only supported glyphs.

### Scheduling and delivery

- Stable data: no five-minute refresh; one heartbeat at the proposed hour.
- A 0.1 °C or 1-point RH fluctuation: no numeric refresh; an accumulated
  0.5 °C or 3-point RH change: eligible at the routine deadline.
- Compare PM deadbands to the displayed snapshot, including zero and gradual
  drift. Returning below a deadband must clear numeric eligibility.
- Exercise exact band thresholds, exit hysteresis, three-minute persistence,
  an interrupted candidate and a source going stale without another callback.
- A committed category transition may refresh early, but never inside the
  physical guard. Check the same guard after lost refresh acknowledgement.
- During upload, deliver newer sensor samples and manual requests. Assert the
  in-flight pixels and timestamp never change and requests coalesce.
- Fail before START, during DATA, after END and during refresh. Failed delivery
  must not promote a snapshot or cause immediate repeated connection attempts.
- With no valid wall clock, ensure heartbeat/manual refresh can bypass the
  unchanged-frame hash once. Normal unchanged frames must still be skipped.
- Test first boot, no sensor samples, optional CO2 absent, external CO2 stale,
  label replacement and a discovered panel with incompatible dimensions.

### On-device completion criteria

Confirm actual flash and heap use from a linked ESP32-C6 build. Confirm panel
black/white/yellow/red mapping and native-pixel readability on the physical
label. Measure transfer and refresh durations rather than treating 30 seconds
as a fixed timer. Verify completion against the panel's refresh-success response,
and confirm BLE disconnects afterward. Keep the dashboard's configurable event
bands and proposed timing defaults visible in configuration and documentation
so later tuning does not require changing its rendering primitives.
