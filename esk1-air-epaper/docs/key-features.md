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
  first values arrive about 2 minutes after boot. Until then the cells show
  `--` on white.

## The e-paper page: AQI dashboard

The page is the dashboard in
[AQI dashboard specification and primitive renderer](aqi-dashboard-specification.md),
built as its own component, [components/aqi_dashboard/](../components/aqi_dashboard/)
(`aqi_dashboard:` in the YAML; see [implementation-plan.md](implementation-plan.md)).
It draws through the `opendisplay` display, which is only the BLE driver. "AQI dashboard" is only
the project name: the page shows individual readings, not an official AQI.

- **Header.** The title (`INDOOR AIR` by default) on the left. On the right,
  when the SEN65 reading behind the values was taken (`23 NOV 14:35`), or
  `NO SAMPLE YET`, or `-- --- --:--` if the clock wasn't set at that moment.
  If the SEN65 is missing or failing, the stamp is replaced by a red
  `SENSOR MISSING` (not found at start-up, or never reported) or
  `SENSOR ERROR` (it worked, now reads fail), and its cells turn yellow `--`.
  Then a black rule.
- **Eight equal cells, 184×60.** PM2.5 | CO2, PM10 | VOC, PM1 | NOx,
  TEMP | HUM. Each has a red label, a bold 32 px value and its unit (`µg/m³`
  bitmap, `ppm`, `idx`, `°C`/`°F`, `%`). PM4 is reported to Home Assistant
  but not drawn.
- **Cell colour is the state:**

  | Cell | Meaning |
  |---|---|
  | White | Normal reading; or starting up / sensor not configured (`--`) |
  | Yellow | Elevated reading; or stale / invalid sensor (`--`); or a value too wide to show (`----`) |
  | Red, white text | High reading |

  The three PM cells follow PM2.5's level. VOC and NOx have their own.
  Temperature and humidity are always white. CO2 has no level until bands
  are configured, and the SEN65 has no CO2 channel, so the cell stays
  `CO2 -- ppm` unless an external sensor is bound.
- **Levels with hysteresis.** Entering and leaving a level use different
  thresholds, and a new level must hold for 3 minutes of fresh readings
  first. These are project event bands for tuning, not health limits.
- **Fonts.** Native Terminus bitmaps only: normal 14 px text and bold 32 px
  numbers, copied byte for byte from the BDF files by
  [build_display_fonts.py](../tools/build_display_fonts.py) into
  `components/aqi_dashboard/dashboard_assets.cpp` (about 4.3 KB of flash with the
  PM sprite). The 24 px font and the ESPHome `font:` assets are not linked.

### When the label refreshes

A BWRY refresh takes about 30 s and costs the label battery, so readings are
sampled every 60 s but the panel refreshes rarely:

| Trigger | When |
|---|---|
| First reading after boot | As soon as the SEN65 reports, or a `--` page after 3 minutes without it |
| A cell changes colour or availability | At once, but never within 15 min of the last refresh |
| A meaningful number change (PM ±max(2, 20 %), VOC 25, NOx 10, CO2 100, temperature 0.5 °C, humidity 3) | 30 min after the last successful refresh |
| Heartbeat | 60 min after the last successful refresh, even if nothing changed |
| **Refresh E-Paper** button | At once, but never within 15 min of the last refresh |
| Failure | Retry after 2, then 5, then every 15 min |

Every threshold and interval is in the YAML (`timing`, `deadbands`, `bands`).
The page being sent is frozen, so readings that arrive during an upload wait
for the next one. A page only counts as displayed when the label confirms the
refresh.

## The `opendisplay` component

A custom ESPHome display platform that drives an OpenDisplay label directly
over BLE. It uses only the OpenDisplay protocol's DIRECT_WRITE flow (checked
against the upstream sources in [opendisplay-protocol-notes.md](opendisplay-protocol-notes.md)): every upload is a full frame (see [Out of scope](#out-of-scope)).

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
  connect, 30 s for START (the panel initialises then), 10 s for other
  commands, 90 s per data ACK, END ACK and panel refresh. A failure (NACK,
  GATT error, disconnect, timeout, encryption required) sets a warning status
  on the component and logs the reason. The config CRC is checked, and a
  mismatch is logged, as the firmware itself does. A START refused because
  the label's config needs migrating or the panel is busy is reported as such
  rather than retried uncompressed. A successful upload logs its size and how
  long it took.
- **No lost updates.** If a redraw is requested while an upload is still
  running, it is queued and runs as soon as the transfer finishes. (With the
  dashboard, its scheduler decides instead.)
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
- **Exact inks, no colour matching.** Pixels are drawn in exactly the four
  inks defined in [ink.h](../components/opendisplay/ink.h). A lambda names
  them with `opendisplay::INK_WHITE` (= `COLOR_OFF`), `INK_BLACK`
  (= `COLOR_ON`), `INK_RED` (`Color(255, 0, 0)`) and `INK_YELLOW`
  (`Color(255, 255, 0)`). Any other colour is not an ink: it is drawn black,
  and the log names it once per frame. A panel without an ink uses the one it
  has: red and yellow are black on `MONO`, and on `BWR` / `BWY` the missing
  accent uses the panel's own. The dashboard writes the inks directly.
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
    ble_name: "OD*"         # find the label by name (see below)
    # Optional; all three or none. The label's own values win.
    width: 400              # the panel's native size, before rotation
    height: 300
    color_scheme: BWRY      # MONO, BWR, BWY or BWRY
    refresh_mode: FULL      # FULL or FAST
    skip_unchanged: true    # default
    compression: true       # default
    update_interval: never  # when aqi_dashboard decides when to refresh
    # or a lambda / pages for a page of your own (then a normal update_interval)

aqi_dashboard:              # the page, as its own component
  display_id: epaper        # must be 400x300 BWRY, update_interval: never, no lambda
  time_id: ha_time
  sen6x_id: sen65
  sensors: {pm_2_5: pm_2_5, pm_10_0: pm_10_0, ...}
```

Without `aqi_dashboard`, the display takes the standard ESPHome display
options (`lambda`, `pages`, `rotation`, and the rest) and refreshes on its
`update_interval`. Lambdas draw with the four inks `INK_WHITE`, `INK_BLACK`,
`INK_RED` and `INK_YELLOW`.

**Finding the label.** With `esl_mac` left at `00:00:00:00:00:00`, the label
is found by its advertised BLE name. OpenDisplay firmware advertises `OD` plus
6 hex digits; a trailing `*` matches a prefix, so `OD*` takes the first
OpenDisplay label heard. The address found is kept in flash. Any other
`esl_mac` is used as is. The web page at `http://esk1-air-epaper.local/` shows
the address in use (**Label MAC**) and lets you change the name
(**BLE Name**) and the **Timezone**.

**Clock.** Home Assistant time, with SNTP (`pool.ntp.org`) as a backup. The
page never waits for the clock; without it the stamp reads `-- --- --:--`.

**Tests.** `python tools/run_host_tests.py` builds and runs the host tests
(protocol against a simulated label, renderer pixels, value model, refresh
scheduler) and writes the test pages to `output/host-render/`.

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
