# esk1-air-epaper: key features

An air-quality monitor built from the ESK-1 starter kit's ESP32-C6 board and a
Sensirion SEN65. It shows its readings on a 4.2" OpenDisplay e-paper shelf
label over Bluetooth LE. The ESK-1 draws the page itself and pushes it to the
label. No wires run to the display, and no Home Assistant integration sits in
the middle.

## At a glance

| | |
|---|---|
| Controller | ESPHome C6 board (ESP32-C6, 8 MB flash), ESP-IDF framework |
| Sensor | Sensirion SEN65 on I2C: SDA GPIO1, SCL GPIO0 |
| Display | 4.2" OpenDisplay e-paper label, 400×300, over BLE |
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

A two-column layout on the 400×300 panel:

- **Left, particulates:** PM2.5 and PM10 in large bold figures, with PM1.0
  and PM4.0 in small type beneath them.
- **Right, gases and climate:** VOC and NOx indices in large figures,
  temperature (°C, one decimal) and humidity (%) beneath them.

Values are rounded before they are drawn. Small sensor jitter then doesn't
change the frame, so it doesn't trigger another upload (see below). Fonts are
Roboto Regular and Bold from Google Fonts, at 16, 22 and 40 px.

The page is redrawn every 5 minutes. A **Refresh E-Paper** button in Home
Assistant forces a redraw at any time.

## The `opendisplay` component

A custom ESPHome display platform that drives an OpenDisplay label directly
over BLE, using the OpenDisplay protocol (version 2.2) DIRECT_WRITE flow.

### Battery-friendly by design

- **Connects only to upload.** The BLE client stays disabled between
  uploads. For each frame it scans, connects, sends, then disconnects, so
  the label can go back to sleep.
- **Skips unchanged frames.** Each frame is hashed (FNV-1a) after drawing. If
  it matches the last frame displayed, nothing is sent and the panel doesn't
  refresh. Turn this off with `skip_unchanged: false`.
- **No Bluetooth proxy.** The YAML leaves out `bluetooth_proxy` on purpose.
  With it, Home Assistant's own OpenDisplay integration could reach the same
  label through this device, and the two would fight over it.

### Safe uploads

- **Panel check on every connection.** Before it sends anything, the
  component reads the label's stored config (CONFIG_READ). It compares the
  panel's width, height and colour scheme with the YAML. On a mismatch it
  stops and logs the label's real values, so you can correct the YAML. If
  the label has no config, it logs a warning and uploads anyway.
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

- **Wire-format framebuffer.** Pixels are drawn straight into the label's
  bit-plane format: 8 px per byte, MSB first, rows padded to a whole byte.
  The buffer is sent as-is, with no conversion step. A 400×300 mono frame is
  15,000 bytes.
- **Standard ESPHome conventions.** `COLOR_OFF` is paper and `COLOR_ON` is
  ink, as on ESPHome's other e-paper drivers. Auto-clear gives a white page,
  and default text is black.
- **Colour panels.** Besides `MONO`, the component supports `BWR`
  (black/white/red) and `BWY` (black/white/yellow) labels with a second
  accent plane. A saturated red or yellow becomes the accent ink. Any other
  colour becomes black or white by luminance.
- **Refresh modes.** `FULL` (the default) or `FAST`, sent with each frame.
- **Fast fills.** A full-screen `fill()` is a `memset` of each plane. A
  clipped fill falls back to pixel by pixel.

### Configuration

```yaml
display:
  - platform: opendisplay
    ble_client_id: esl_client
    width: 400              # the panel's native size, before rotation
    height: 300
    color_scheme: MONO      # MONO, BWR or BWY
    refresh_mode: FULL      # FULL or FAST
    skip_unchanged: true    # default
    update_interval: 5min   # component default is 10min
    lambda: |-
      it.print(0, 0, id(my_font), "Hello");
```

The component also accepts the standard ESPHome display options (`pages`,
`rotation`, and the rest). Set the label's BLE address in the `esl_mac`
substitution. The OpenDisplay app and Home Assistant both show it. So does
this device's log with `esp32_ble_tracker` at DEBUG level.

## Not supported (yet)

- Encrypted labels (AUTHENTICATE). An upload to a label with encryption on
  fails with a clear log message.
- Compressed uploads. A zlib/DEFLATE encoder sized for the firmware's 512-byte
  inflate window ([zlib_deflate.h](../components/opendisplay/zlib_deflate.h))
  is in the tree, but it isn't wired into the upload path yet.
- The PIPE sliding-window transfer and partial refresh.
