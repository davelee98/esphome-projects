# esphome-projects

Standalone [ESPHome](https://esphome.io) projects. Each one lives in its own
directory with everything it needs to compile, apart from any external
components it loads from GitHub and its `secrets.yaml`.

| Project | What it is |
|---|---|
| [fortune-button](fortune-button/) | A yes/no oracle toy: press the button, a prize wheel of LEDs spins and ticks, then it gives a verdict in light and sound |
| [esk1-house](esk1-house/) | The whole ESK-1 kit in one device: an air-quality status board on the LED house, plus the fortune button on the same LEDs |

---

## Fortune Button

Press the button and the LED ring becomes a prize wheel. A blue pointer
clicks round a white ring, ticking the buzzer at every step, and slows to a
stop. After a moment of darkness and silence, the toy answers:

- **Yes:** green fills the ring, then a rising two-note chime.
- **No:** red blinks three times, then a sad trombone.

Hold the button for 1.5–8 seconds instead for an easter egg: a magenta
flicker and a dissonant melody.

It runs with no network at all: no Wi-Fi, no Home Assistant, no API. It
works anywhere you plug it in.

### Hardware

Stock parts from the [Apollo Automation ESK-1 ESPHome Starter
Kit](https://apolloautomation.com/products/esk-1-esphome-starter-kit), the
[official ESPHome starter kit](https://esphome.io/starter-kit/). No soldering
or extra parts.

| Part | Connection |
|---|---|
| ESPHome C6 board (ESP32-C6) | Controller, powered over USB-C |
| Button module | GPIO6 |
| LED/buzzer module: 10 WS2812 LEDs | GPIO14 |
| LED/buzzer module: 2.7 kHz buzzer (via a transistor) | GPIO18 |
| Accessory power switch (+3V3_CTRL, the buzzer's supply) | GPIO4 |

The temperature/humidity and PIR modules in the kit aren't used.

### Build and flash

Keep the directory layout as it is. The YAML loads the local component with
`external_components: path: components`, which resolves relative to the YAML
file.

```
fortune-button/
├── fortune-button.yaml
└── components/pwm_speech/     # plays the sad trombone
```

With the ESPHome CLI, from `fortune-button/`:

```sh
esphome run fortune-button.yaml
```

**With Device Builder:** it only lists YAML files at the top of its config
directory. Copy the `fortune-button/` directory into the config directory,
copy `fortune-button.yaml` up to the top level beside it, and in that copy
change the component path to `path: fortune-button/components`.

There's no OTA, because there's no network, so flash it over USB-C. It needs
ESP-IDF, as ESPHome doesn't support Arduino on the C6. It was built and tested
with ESPHome 2026.8.

On Windows, turn on long paths (`LongPathsEnabled=1`) before the first
compile, or the ESP-IDF toolchain fails with
`bits/c++config.h: No such file`.

### Behaviour

| Phase | Light | Sound | Length |
|---|---|---|---|
| Idle | Violet breathing at 10% brightness, off after 60 s with no press | — | — |
| Spin | White ring, blue pointer stepping round | One tick per step | 5.6 s |
| Suspense | Dark | Silent | 0.9 s |
| Yes | Green fills the ring, then holds | Rising chime | 3.5 s |
| No | Red, three blinks, then fades | Sad trombone | 5.9 s |

Each spin makes a random 50–60 steps (at least five laps of the ring). It
starts at 17–21 steps a second and slows steadily to 2 a second. Beeps are
half the gap between steps, up to an eighth note at the tempo.

The yes/no roll is made fresh on every press. It mixes the microsecond at
which it runs, so it depends on exactly when you pressed. That way it can't
repeat from one power-up to the next, even though with no radio running the
chip's RNG is only pseudo-random. Each roll is logged, for example
`fortune: Roll 37: yes`.

### Settings

Everything tunable is in `substitutions:` at the top of the YAML.

| Setting | Default | Effect |
|---|---|---|
| `yes_max` | `50` | Percentage chance of yes |
| `idle_timeout` | `60s` | Idle glow turns off after this long with no press |
| `tempo_bpm` | `120` | Tempo of the yes chime and the no blinks, and the tick-length cap |
| `yes_fill_ms` | `1000` | Time for green to fill the ring |
| `spin_ticks` / `spin_tick_spread` | `55` / `5` | Each spin makes a random 50–60 steps |
| `spin_end_speed` | `2` | Steps per second when the wheel stops |
| `spin_ms` | `5600` | Length of the spin |
| `spin_settle_ms` | `500` | How long the pointer holds on its last step |
| `spin_white_level` | `0.6` | Brightness of the white ring during the spin |
| `spin_tone_hz` | `2700` | Tick pitch (the buzzer's resonance, its loudest note) |
| `spin_tone_fraction` | `0.5` | Tick length as a fraction of the gap between steps |
| `spin_tone_level` | `0.5` | Tick volume as PWM duty (0.5 is loudest) |
| `ring_cw` | `1` | Set to `-1` if the spin and fill go anticlockwise on your ring |

The LED effects assume the 10 LEDs form a closed ring, with LED 9 next to
LED 0.

### The `pwm_speech` component

A small local external component that plays the sad trombone on the buzzer
pin. The trombone is a pitch and loudness contour measured from a recording,
270 steps of 12 ms (3.24 s) in `contour_data.h`. The component replays it
from elapsed time, so it doesn't block the main loop and always lasts exactly
3.24 s. A new press or a long hold cuts it off.

```yaml
pwm_speech.play:
  id: speech
  sound: trombone          # starts it and returns at once
pwm_speech.stop: speech    # silences it
pwm_speech.is_playing: speech   # condition
```

The name is left over from an earlier version that also spoke the words
"yes" and "no". The kit's buzzer couldn't reproduce speech, so the words were
removed.

---

## esk1 house

Every module in the ESK-1 kit on one ESP32-C6. The 10 LEDs of the Casita
house show indoor air quality from the SEN65, and pressing the button asks
the fortune button for a yes or no on the same LEDs. When the reading ends,
the air-quality display comes back.

It's built from two external components, loaded from GitHub:

- [esphome-casita-status](https://github.com/davelee98/esphome-casita-status)
  drives the status display.
- [esphome-fortune-button](https://github.com/davelee98/esphome-fortune-button)
  is the fortune button as a component.

The YAML wires them to the hardware and has no lambdas of its own.

### Hardware

| Part | Connection |
|---|---|
| ESPHome C6 board (ESP32-C6) | Controller, ESP-IDF |
| Button module | GPIO6 |
| PIR module | GPIO3 |
| LED/buzzer module: 10 WS2812 LEDs | GPIO14 |
| LED/buzzer module: buzzer | GPIO18 (LEDC) |
| Accessory power switch (+3V3_CTRL, the buzzer's supply) | GPIO4 |
| AHT20 and SEN65 | I2C: SDA GPIO1, SCL GPIO0 |

### What the LEDs show

| Group | LEDs | Shows |
|---|---|---|
| Top | 3 | PM2.5, in EPA AQI bands |
| Left | 3 | VOC index |
| Right | 3 | NOx index |
| Center | 1 | The worst status of the other three |

After power-up the groups show Initializing for up to about 2 minutes. The
SEN65 driver ignores readings for its first 60 s, and the sensor reports
every 60 s. If a sensor stops reporting for 3 minutes, its group shows
Unknown.

While a fortune reading or the easter egg runs, Fortune suspends Casita and
draws on the LEDs, then resumes it. Fortune's brightness is set to 78% so
that it matches Casita's 50%: Fortune scales colours before gamma
correction, and Casita scales the output after it.

### Exposed to Home Assistant and the web page

- AHT20 temperature and humidity.
- SEN65 PM1.0, PM2.5, PM4.0, PM10, temperature, humidity, VOC index and
  NOx index.
- Motion, from the PIR.
- Casita brightness, as a number (25–100%, saved across restarts).
- Casita group states.
- **Casita Self Test**, a button that runs a test of about 22 s:
  - A white dot walks the LEDs clockwise from the roof peak.
  - Every group then shows statuses 1–6, Initializing and Unknown.

### Build and flash

It uses Wi-Fi, the API and OTA, so put a `secrets.yaml` beside the YAML with
these four entries:

| Secret | What it is |
|---|---|
| `wifi_ssid` | Your Wi-Fi network |
| `wifi_password` | Your Wi-Fi password |
| `esk1_house_ap_password` | Password for the fallback hotspot |
| `esk1_house_api_key` | The API encryption key, 32 bytes in base64 |

From `esk1-house/`:

```sh
esphome run esk1-house.yaml
```

The first flash is over USB-C, and later ones can be over OTA. With Device
Builder, copy `esk1-house.yaml` to the top of its config directory; there are
no local paths to change.

Both components are fetched from `main` with `refresh: always`, so each build
picks up their latest changes. It needs ESPHome 2026.8 or later.

---

## License

[GNU General Public License v3.0](LICENSE). You may use, modify and
redistribute this code, but distributed versions, including modified ones,
must stay under GPL-3.0 and include their source.
