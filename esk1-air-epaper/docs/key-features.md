# esk1-air-epaper: key features

An air-quality monitor built from the ESK-1 starter kit's ESP32-C6 board and a
Sensirion SEN65. It shows its readings over Bluetooth LE on a 4.2" e-paper
shelf label (ESL) that runs OpenDisplay firmware. The panel is BWRY: black,
white, red and yellow. The ESK-1 draws the page itself and pushes it to the
label. No wires run to the display, and no Home Assistant integration sits in
the middle.

## At a glance

| | |
|---|---|
| Controller | ESPHome C6 board (ESP32-C6, 8 MB flash), ESP-IDF framework |
| Sensor | Sensirion SEN65 on I2C: SDA GPIO1, SCL GPIO0 |
| Display | 4.2" BWRY e-paper label, 400×300, running OpenDisplay firmware, over BLE |
| Display driver | Local `opendisplay` component ([components/opendisplay/](../components/opendisplay/)) |
| Minimum ESPHome | 2026.3.0 (for the `sen6x` platform) |

## Air-quality sensing

- **Eight readings from one sensor.** The SEN65 reports PM1.0, PM2.5, PM4.0,
  PM10 (µg/m³), temperature, relative humidity, the VOC index and the NOx
  index. It reads every 60 s.
- **Everything goes to Home Assistant.** Each reading is a named sensor
  through the encrypted native API. A diagnostic Wi-Fi signal sensor comes
  with them.
- **Start-up warm-up.** `sen6x` ignores readings for the first 60 s, so the
  first values arrive about 2 minutes after boot. Until then the page shows
  `--` for missing values.

## The e-paper page

The target custom-component dashboard is specified in
[AQI dashboard specification and primitive renderer](aqi-dashboard-specification.md).
It defines the current eight-cell preview and its primitive renderer, with
proposed refresh and threshold policies. The layout and five-minute polling
described below are the existing YAML implementation, pending that migration.

A two-column layout on the 400×300 panel:

- **Left, particulates:** PM2.5 and PM10 in large figures, with PM1.0
  and PM4.0 in small type beneath them.
- **Right, gases and climate:** VOC and NOx indices in large figures,
  temperature (°C, one decimal) and humidity (%) beneath them.

Values are rounded before they are drawn. Small sensor jitter then doesn't
change the frame, so it doesn't trigger another upload (see below). All three
font assets use native Terminus bitmaps: normal 14 px text and bold 24/32 px
numerical readings. Glyph bytes are copied directly
from the upstream BDF files at their exact native sizes, without rasterizing
outlines or scaling pixels.

The two value fonts contain only `0123456789.- °FC%` (17 glyphs, including
space). The period supports decimals, the minus supports negative values and
the `--` missing-value marker, and both temperature units are available.
`glyphsets: []` keeps ESPHome from adding its default alphabet to these fonts.
The original BDF files and their license are in [fonts/terminus/](../fonts/terminus/).
For plain C rendering, [numeric-glyphs.h](../fonts/numeric-glyphs.h) contains
the same native bold Terminus 24 px and 32 px glyphs as packed 1-bpp rows with advance
and bearing metrics. Their bitmap data totals 1,904 bytes, excluding metrics.
Regenerate
the assets with [build_display_fonts.py](../tools/build_display_fonts.py),
using Python and Pillow. Pillow only writes the PNG previews; font pixels
are read directly from BDF. All glyphs at each size use the original fixed
advance width for monospaced alignment.

The PM unit is a separate transparent 30×12 bitmap: `µg/m³` on one line.
Its monospaced letters come from the original 6×12 Terminus normal bitmap.
[pm-units.h](../fonts/pm-units.h) contains its 48 bytes of 1-bpp
pixels; [pm-units.png](../fonts/pm-units.png) is the transparent image version.
The preview places it beside each PM value in the same ink as the number.
The 12 px BDF is only a build input for this complete unit bitmap; it does
not add a fourth runtime font asset.

Optional native 24×24 thermometer and droplet bitmaps are available in
[climate-icons.h](../fonts/climate-icons.h), 72 bytes each. Their PNG versions
have transparent backgrounds and do not need an icon font. The current preview
uses the monospaced `TEMP` and `HUM` labels instead.

The page is redrawn every 5 minutes. A **Refresh E-Paper** button in Home
Assistant forces a redraw at any time.

## The `opendisplay` component

A custom ESPHome display platform that drives an OpenDisplay label directly
over BLE. It uses only the OpenDisplay protocol's (version 2.2) DIRECT_WRITE
flow: every upload is a full frame (see [Out of scope](#out-of-scope)).

### Battery-friendly by design

- **Connects only to upload.** The BLE client stays disabled between
  uploads. For each frame it scans, connects, sends, then disconnects, so
  the label can go back to sleep.
- **Skips unchanged frames.** Each frame is hashed (FNV-1a) after drawing. If
  it matches the last frame displayed, nothing is sent and the panel doesn't
  refresh. Turn this off with `skip_unchanged: false`.
- **Compressed uploads.** When the label's config says it has a
  decompressor, the frame is zlib-compressed first, so there's less to send.
  If the label refuses the compressed form, the component sends the frame
  uncompressed instead. Turn this off with `compression: false`.
- **No Bluetooth proxy.** The YAML leaves out `bluetooth_proxy` on purpose.
  With it, Home Assistant's own OpenDisplay integration could reach the same
  label through this device, and the two would fight over it.

### Safe uploads

- **The label says what its panel is.** Before it sends anything, the
  component reads the label's stored config (CONFIG_READ) for the panel's
  width, height and colour scheme. If they differ from what the frame was
  drawn for, it redraws the frame at the label's size in the same buffer
  and logs the change (see [Frame buffer](#frame-buffer)). It stops and
  logs why if the label's panel is too large for the buffer, or uses a
  scheme the component can't draw.
- **Flow control by acknowledgement.** Image data goes out in chunks of up to
  230 bytes, sized to the negotiated MTU. The next chunk waits for the
  label's ACK. Data chunks use write-without-response where the
  characteristic allows it.
- **Clear failure reporting.** Every stage has its own timeout: 120 s to
  connect, 10 s per command, 90 s per data ACK, END ACK and panel refresh. A
  failure (NACK, GATT error, disconnect, timeout, encryption required) sets a
  warning status on the component and logs the reason. A successful upload
  logs its size and how long it took.
- **No lost updates.** If a redraw is requested while an upload is still
  running, it is queued and runs as soon as the transfer finishes.
- **Handles firmware auto-complete.** If the label ends the transfer itself
  once its buffer is full, the component skips its own END command.

### Drawing

- **Colour schemes from the protocol.** `color_scheme` takes the names of
  the OpenDisplay protocol's `ColorScheme` enum, and the component sends the
  same wire values:

  | `color_scheme` | Inks | Buffer format | 400×300 frame |
  |---|---|---|---|
  | `MONO` | black, white | 1 bpp | 15,000 bytes |
  | `BWR` | black, white, red | two 1 bpp planes | 30,000 bytes |
  | `BWY` | black, white, yellow | two 1 bpp planes | 30,000 bytes |
  | `BWRY` | black, white, red, yellow | 2 bpp | 30,000 bytes |

  This project's label is `BWRY`. Spectra panels are
  [out of scope](#out-of-scope).
- **Wire-format framebuffer.** Pixels are drawn straight into the format the
  label takes: rows padded to a whole byte, leftmost pixel in the high bits.
- **Per-panel BWRY colour codes.** Some BWRY panels number their red and
  yellow codes differently. The component reads the label's own code table
  (the `code_map` in its display_panel config packet) on each connection.
  If there is none, it uses the built-in table for the panel IC. It maps
  the frame to those codes just before the upload, and maps it back
  afterwards.
- **Standard ESPHome conventions.** `COLOR_OFF` is paper and `COLOR_ON` is
  black ink, as on ESPHome's other e-paper drivers. Auto-clear gives a white
  page, and default text is black.
- **Real colours on colour panels.** Any other colour becomes the nearest ink
  the panel has. For example, `Color(255, 0, 0)` draws red on BWR and BWRY
  panels. On a `MONO` panel, any colour other than `COLOR_OFF` is black.
- **Refresh modes.** `FULL` (the default) or `FAST`, sent with each frame.
- **Fast fills.** A full-screen `fill()` is a `memset` of the buffer. A
  clipped fill falls back to pixel by pixel.

### Frame buffer

At boot, the component reserves one fixed block of **30,000 bytes**, the
size of a 400×300 BWRY frame. That is the largest supported frame. It
reserves this whatever panel is configured, and keeps it.

The panel's real size and colour scheme come from the label on each
connection. Whatever panel the label reports is drawn into the start of the
same block, so a different or smaller panel needs no new memory:

1. On a scheduled update, the component draws the frame for the panel it
   last learned. On the first update, if the YAML gives no panel, it doesn't
   draw yet.
2. It connects and reads the label's config.
3. If the label's panel differs from what was drawn, the component takes on
   the label's width, height and colour scheme. It logs the change and
   draws the frame again at that size. A change of panel always sends the
   new frame, even if `skip_unchanged` is on.
4. It uploads the frame.

So `width`, `height` and `color_scheme` are optional in the YAML. If you
give them, give all three; they set the panel to draw for before the first
connection. They're also the fallback for a label with no stored config.
Without them, that label can't be driven, and the upload fails with a log
message saying to set them.

The block is reserved at boot because of RAM. The ESP32-C6 has 512 KB of
SRAM and no PSRAM. This build's memory report counts 452 KB of it for the
firmware, and 185 KB of that is taken at link time. Wi-Fi, Bluetooth and the
Home Assistant API then take most of the remaining heap when they start. A
frame buffer must be one contiguous block. Reserving it once, before the
heap fragments, means a later panel change never has to find a new one.
An upload also needs, for a short time, about 10–15 KB more: the compressed
copy of the frame, the compressor's tables, and the label's config.

### Frame size limit

A frame bigger than the 30,000-byte buffer isn't supported:

- **In the YAML:** a `width`/`height`/`color_scheme` set whose frame is too
  large fails validation, with an error that gives the size.
- **From the label:** if the label reports a panel whose frame doesn't fit,
  the upload stops and the log gives the size it needs.

The limit is on bytes, not on the scheme, so any supported scheme fits as
long as its frame is no bigger:

| `color_scheme` | Bytes per pixel | Fits, for example |
|---|---|---|
| `MONO` | 1/8 | 400×300 (15,000), 480×400 (24,000) |
| `BWR`, `BWY` | 2/8 | 400×300 (30,000) |
| `BWRY` | 2/8 | 400×300 (30,000) |

Sizes count each row rounded up to a whole byte. The limit is
`MAX_FRAME_BYTES`, defined in both
[opendisplay.h](../components/opendisplay/opendisplay.h) and
[display.py](../components/opendisplay/display.py). The two must match.

### Configuration

```yaml
display:
  - platform: opendisplay
    ble_client_id: esl_client
    # Optional; all three or none. The label's own values win.
    width: 400              # the panel's native size, before rotation
    height: 300
    color_scheme: BWRY      # MONO, BWR, BWY or BWRY
    refresh_mode: FULL      # FULL or FAST
    skip_unchanged: true    # default
    compression: true       # default
    update_interval: 5min   # component default is 10min
    lambda: |-
      it.print(0, 0, id(my_font), "Hello");
```

The component also accepts the standard ESPHome display options (`pages`,
`rotation`, and the rest). Set the label's BLE address in the `esl_mac`
substitution. The OpenDisplay app and Home Assistant both show it. So does
this device's log with `esp32_ble_tracker` at DEBUG level.

## Not supported (yet)

- The protocol's grey and RGB colour schemes: `GRAY4`, `GRAY16`, `RGB565`,
  `RGB888` and `RGB16BPC`. The log names them if a label reports one.
- Encrypted labels (AUTHENTICATE). An upload to a label with encryption on
  fails with a clear log message.

## Out of scope

These won't be added.

**Spectra panels.** That covers Spectra 6 (`BWGBRY`), dual-controller
Spectra 6 (`BWGBRY_SPLIT`) and 7-colour (`SEVEN_COLOR`). Their 4-bit pixels
make a 400×300 frame 60,000 bytes, twice the [size limit](#frame-size-limit).
If a label reports one of these schemes, the upload stops and the log names
the scheme.

**Anything but direct write.** The component sends every frame as a
full-screen DIRECT_WRITE upload. These aren't supported:

- Partial refresh (updating only part of the screen).
- PIPE write (the sliding-window transfer).
- PIPE partial.

`refresh_mode: FAST` is still available. It only chooses the panel's refresh
waveform for a full-screen frame, so it isn't a partial update.
