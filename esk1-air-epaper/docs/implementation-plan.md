# Implementation plan: OpenDisplay protocol and AQI dashboard

Status: phases 1–6 implemented on 23 November 2026; phase 7 (on the label)
is still to do. Section 1.1 below describes the starting point, before that
work.

| Phase | Result |
|---|---|
| 1.1 Wire facts | Checked upstream: [opendisplay-protocol-notes.md](opendisplay-protocol-notes.md). Corrections made: CRC-16/CCITT-FALSE checked (advisory), START NACK error codes 0x08 / 0x09 not retried raw, START timeout 30 s, BLE name default `OD*` |
| 1.2 Session | `od_session.*`, `od_config.*`; `OpenDisplay` is the GATT glue |
| 1.3 Simulated label | `tests/host/fake_label.h` (own inflater), `test_session.cpp`, `test_config.cpp` |
| 1.4 Driver hooks | `request_upload()`, `busy()`, `add_on_complete_callback()` with a refresh marker, `set_external_scheduling()`, `set_required_panel()`, `put_ink()` |
| 1.5 Bring-up | Not done: needs the label |
| Layout | After implementation the page moved to its own component, `components/aqi_dashboard/` (`dashboard_*`, `aqi_dashboard.*`, `__init__.py` with the `aqi_dashboard:` schema, namespace `esphome::aqi_dashboard`). `components/opendisplay/` is only the BLE driver. Paths in the sections below are as first planned. |
| 2 Assets | `build_display_fonts.py --dashboard components/aqi_dashboard` writes `dashboard_assets.*`; the host tests check them byte for byte against the BDF sources |
| 3 Renderer | `dashboard_render.*`, `test_render.cpp`; pages in `output/host-render/` |
| 4 Model | `dashboard_model.*`, `test_model.cpp` |
| 5 Scheduler | `dashboard_scheduler.*`, `test_scheduler.cpp` |
| 6 Integration | `aqi_dashboard.*` and the `aqi_dashboard:` schema (with a final check of the display it draws on), YAML migrated. Builds on ESPHome 2026.11.0 with no warnings: RAM 41.6 %, flash 42.6 % |
| 7 On device | Not done: needs the label |

`python tools/run_host_tests.py` runs 62 tests (about 2,400 checks) plus the
asset check.

**Change to D5 (agreed with the project owner):** no colour matching. The
four inks are defined once in `ink.h` and shared by the driver and the
renderer. The dashboard writes them directly
(`OpenDisplay::put_ink()` → `write_ink_()`). A lambda's colours must be
exactly `INK_WHITE` / `COLOR_OFF`, `INK_BLACK` / `COLOR_ON`, `INK_RED` or
`INK_YELLOW`. Anything else is drawn black and logged, never matched to the
nearest ink. The BWRY LUT is still applied once, for the upload only.

This plan turns two things into working firmware:

1. **The OpenDisplay transport.** Finish, verify and harden the BLE protocol
   implementation in [components/opendisplay/](../components/opendisplay/),
   and add the hooks the dashboard needs.
2. **The AQI dashboard.** Implement the page, renderer, value model and
   refresh policy defined in
   [aqi-dashboard-specification.md](aqi-dashboard-specification.md) (called
   "the spec" below; section numbers such as §5.1 refer to it).

The spec is authoritative for what the page looks like and when it refreshes.
This plan covers how to build it, in what order, and how each step is checked.

## 1. Starting point

### 1.1 What already existed (before this work)

| Area | Current state | File |
|---|---|---|
| BLE connect only for an upload; disconnect afterwards | Done | `opendisplay.cpp` `update()`, `finish_()` |
| Label found by BLE name, MAC override, address kept in flash | Done | `parse_device()`, `set_ble_name()` |
| CONFIG_READ, chunk reassembly, TLV parse (display 0x20, display_panel 0xB1) | Done, not tested on a label | `on_config_frame_()`, `check_config_()` |
| Panel size / scheme taken from the label, 30,000-byte buffer reserved at boot | Done | `adopt_panel_()`, `setup()` |
| BWRY 2 bpp buffer, per-panel code map, remap before upload and back after | Done, not tested on a label | `set_bwry_codes_()`, `start_upload_()` |
| DIRECT_WRITE START / DATA / END, ACK flow control, MTU-sized chunks | Done, not tested on a label | `send_next_chunk_()`, `on_frame_()` |
| zlib (9-bit window, fixed Huffman) with fallback to uncompressed | Done, compressor round-trip tested on host | `zlib_deflate.h` |
| Refresh success / timeout notifications, auto-complete, auth-required | Done, not tested on a label | `on_frame_()` |
| Unchanged-frame skip (FNV-1a) | Done | `update()`, `buffer_hash_()` |
| Web page: BLE Name text, Label MAC sensor | Done | `esk1-air-epaper.base.yaml` |
| Clock (`ha_time`) and run-time Timezone select | Done | `components/timezone_select/` |
| Terminus BDF inputs, generator, numeric 24/32 C table, PM sprite | Done (24 px is to be dropped) | `fonts/`, `tools/build_display_fonts.py` |
| Dashboard renderer, value model, scheduler | Not started | — |

### 1.2 What is missing

- **Protocol.** None of the protocol has been checked against a real label or
  a simulator. The protocol state machine is tied to ESP-IDF GATT events, so
  it can't be tested on a host. The driver has none of the hooks in spec §8
  (completion result, refresh-start marker, forced upload, owned scheduling).
- **Dashboard.** Everything in spec §2–§7: the 14 px and 24 px text tables, the
  primitive renderer, formatting, classification with hysteresis and
  persistence, staleness, deadbands, snapshots, refresh scheduling, a clock,
  and the configuration schema.

### 1.3 Decisions this plan makes

These follow the spec where it is explicit. Where it leaves a choice open,
the plan picks one, so the work can proceed. Each can be revisited.

| # | Decision | Reason |
|---|---|---|
| D1 | The protocol state machine moves into an ESPHome-free class, `od::Session`, behind a small transport interface. `OpenDisplay` keeps the GATT glue. | Lets the protocol run against a simulated label on a host, as `zlib_deflate.h` already does. |
| D2 | The renderer core (`dashboard_render.h/.cpp`) is plain C++17 with no ESPHome includes, using the spec's `Raster` / `put_pixel` interface (§6.1). | The spec requires host pixel tests (§9). |
| D3 | The dashboard is configured as a `dashboard:` block under `display: platform: opendisplay`, mutually exclusive with `lambda` and `pages`. | The spec keeps it inside `components/opendisplay/` (§8) and requires that exclusivity. |
| D4 | The dashboard draws through the driver's writer (`set_writer`), and the writer draws the frozen `in_flight` snapshot. | Reuses `do_update_()`, the hash, and the redraw-on-panel-change path, so there is no second drawing path. |
| D5 | The `Raster.put_pixel` adapter maps `Ink` to the colours `classify_()` already resolves exactly: `COLOR_OFF` → white, `COLOR_ON` → black, `Color(255,255,0)` → yellow, `Color(255,0,0)` → red. It calls `draw_absolute_pixel_internal()`. | Spec §6.4 asks for the existing drawing path, and this keeps the LUT applied once, in `start_upload_()`. Absolute coordinates, because the geometry is physical pixels. |
| D6 | The scheduler ticks every 1 s with `set_interval`, and uses wrap-safe `uint32_t` `millis()` differences. In dashboard mode the display's own polling is `never`. | Spec §5.1 and §8. |
| D7 | The time source is the `ha_time` Home Assistant `time:` component, with an explicit `timezone` set by the `timezone` substitution. The timezone can be changed at run time with the **Timezone** select (local `timezone_select` component, already in place). The dashboard formats the stamp with `ha_time.now()`, so it follows whichever zone is selected. | Spec §2.1 requires an explicit timezone and no stamp from uptime. A YAML timezone also stops Home Assistant replacing it on each sync. |
| D8 | Host tests compile with zig's C++ compiler (`uvx --from ziglang python -m ziglang c++`), the same toolchain already used for the compressor test. They are driven by `tools/run_host_tests.py`. | There is no other host C++ compiler on this machine. |
| D9 | Generated dashboard assets live in `components/opendisplay/` (`dashboard_assets.h` declarations, `dashboard_assets.cpp` definitions). | ESPHome only compiles sources inside the component directory. The spec asks for one translation unit for the tables (§3.3). |
| D10 | Encrypted labels (AUTHENTICATE 0x0050) stay out of scope for this plan. | Not needed for the target label. Already reported clearly when a label requires it. |

## 2. Target file layout

```
esk1-air-epaper/
  components/opendisplay/
    __init__.py                 (unchanged)
    display.py                  + dashboard: schema, validation, codegen
    opendisplay.h / .cpp        GATT glue + driver hooks; state machine moves out
    od_session.h / .cpp         NEW  transport-independent protocol session
    od_config.h / .cpp          NEW  CONFIG_READ blob parser (moved from check_config_)
    zlib_deflate.h              (unchanged)
    dashboard_render.h / .cpp   NEW  Raster, primitives, text, draw_dashboard()
    dashboard_model.h / .cpp    NEW  snapshot types, formatting, classifier, deadbands
    dashboard_scheduler.h/.cpp  NEW  refresh policy (pure; takes now_ms)
    dashboard.h / .cpp          NEW  ESPHome glue: sensor bindings, time, ticks
    dashboard_assets.h / .cpp   NEW  generated: text 14, bold 32, PM sprite
  tests/
    host/                       NEW  host test sources and fakes
      test_session.cpp          protocol against a simulated label
      test_config.cpp           TLV parser against captured / built blobs
      test_render.cpp           pixel tests, writes PNM for review
      test_model.cpp            formatting, classification, deadbands
      test_scheduler.cpp        refresh policy with a fake clock
      fake_label.h              simulated OpenDisplay firmware
  tools/
    build_display_fonts.py      + --dashboard output of dashboard_assets.*
    run_host_tests.py           NEW  builds and runs tests/host with zig c++
```

The `fonts/` headers stay as development and preview assets. The firmware
links only `dashboard_assets.cpp` (spec §3.3, §7).

## 3. Work breakdown

The phases are ordered by dependency. Phases 1 and 2 can run in parallel.
Phases 3 and 4 depend on 2. Phase 5 depends on 1.4 and 4. Phase 6 depends on
everything before it. Phase 7 needs the hardware.

Sizes are relative: **S** about half a day, **M** one to two days, **L** three
days or more.

---

### Phase 1: OpenDisplay protocol

#### 1.1 Check every wire fact against upstream (S)

The driver's constants and offsets were written from the protocol sources
named in its comments, but they haven't been checked against a running
label. Before refactoring, check each item against the upstream
`opendisplay-protocol` headers, py-opendisplay, and Firmware_Unified. Write
what you find, with file and line references, into a new
`docs/opendisplay-protocol-notes.md`.

| Fact to confirm | Where the driver uses it |
|---|---|
| Service and characteristic UUID 0x2446; write / write-without-response properties | `OD_SERVICE_UUID`, `send_command_()` |
| Opcodes 0x0040, 0x0070–0x0072 big-endian; response layout `[status][echo]` | `send_command_()`, `on_frame_()` |
| CONFIG_READ chunk framing: chunk 0 carries `total:2 LE`; NACK `[FF][40]` when there's no config | `on_config_frame_()` |
| Config blob `[len:2][version:1][packets][crc:2]`; legacy fixed packet sizes; types ≥ 0x80 length-prefixed | `check_config_()`, `legacy_packet_size()` |
| Display packet offsets: panel IC [2..3], width [4..5], height [6..7], rotation [14], colour scheme [21], transmission modes [22] | `check_config_()` |
| display_panel (0xB1) code_map offset 7, instance byte 0 | `DISPLAY_PANEL_CODE_MAP_OFFSET` |
| BWRY 2 bpp packing order and the EP29YR swap | `draw_absolute_pixel_internal()`, `BWRY_CODES_*` |
| Compressed START `[size:4 LE][zlib…]` and the 200-byte START limit; uzlib window of 9 bits | `start_upload_()`, `zlib_deflate.h` |
| NACK forms for a refused compressed START (`[FF][70]`, `[FF][FF]`) | `on_frame_()` WAIT_START |
| DATA chunk max 230 bytes unencrypted; END payload is one refresh byte (0 full, 1 fast) | `CHUNK_SIZE`, `send_end_()` |
| 0x72 during DATA means auto-complete; 0x73 success, 0x74 timeout | `on_frame_()` |
| 50 KB buffered-decompression limit and the transmission-mode bits 0x01 / 0x02 | `MAX_COMPRESSED_SIZE`, `TRANSMISSION_*` |
| Whether the CONFIG_READ CRC should be verified | not checked today |

**Done when:** every row is confirmed or corrected in code, each with a
source reference in the notes file. Any correction gets a host test in 1.3.

#### 1.2 Move the state machine out of the GATT handler (M)

Create `od::Session` in `od_session.h/.cpp`, with no ESPHome or ESP-IDF
includes:

```cpp
struct Transport {
  virtual bool write(const uint8_t *data, size_t len, bool with_response) = 0;
  virtual uint16_t mtu() const = 0;
};

enum class Outcome : uint8_t { SUCCESS, FAILED_NOT_SENT, FAILED_UNCERTAIN };

class Session {
 public:
  // Starts CONFIG_READ. The frame comes from the FrameProvider after the panel is known.
  void begin(uint32_t now_ms);
  void on_notify(const uint8_t *data, size_t len, uint32_t now_ms);
  void on_write_error(int status, uint32_t now_ms);
  void on_disconnect(uint32_t now_ms);
  void tick(uint32_t now_ms);          // per-state timeouts
  bool busy() const;
  // Set once a panel refresh could have started (spec §5.1).
  std::optional<uint32_t> refresh_may_have_started_ms() const;
};
```

- Move the parsing in `check_config_()` into `od_config.h/.cpp` as a pure
  function: `parse_config(const uint8_t*, size_t) -> ConfigResult` (panel IC,
  width, height, scheme, rotation, transmission modes, optional code map,
  errors).
- `OpenDisplay` keeps the GATT pieces: connecting, discovering the
  characteristic, registering for notifications, the MTU, and enabling or
  disabling the client. It forwards events to the `Session` and implements
  `Transport` with `esp_ble_gattc_write_char`.
- The panel decision (`adopt_panel_()`), the BWRY remap and the frame buffer
  stay in `OpenDisplay`. The session asks for them through a small
  `FrameProvider` interface: `on_panel(ConfigResult) -> bool`, then
  `frame() -> {ptr, len}`, then `on_complete(Outcome)`.
- Timeouts stay as they are, but are driven by `tick(now_ms)` with wrap-safe
  arithmetic.

**Done when:** the firmware builds with the same behaviour as before, the
compile shows no new warnings, and RAM/flash change by less than 1 KB.

#### 1.3 Simulated label and protocol tests on the host (M)

`tests/host/fake_label.h` acts as the label's firmware: it takes writes and
queues notifications. It is configurable for:

- panel config, or no config (NACK); config split over 1, 2 or many chunks;
- MTU 23, 185 and 247;
- transmission modes 0x00, 0x01 and 0x02;
- refusing compressed START with `[FF][70]` or `[FF][FF]`;
- auto-complete when the buffer fills; refresh success or timeout;
- encryption required (0xFE); a NACK on DATA at byte N;
- a write error, a disconnect or silence at every state.

The fake inflates compressed uploads with a real zlib (use the
`zlib_deflate` host test's reference decoder, or miniz), then checks the
received frame byte for byte.

Test cases (`test_session.cpp`, `test_config.cpp`):

1. A raw upload at each MTU: the bytes match and the chunk sizes are `min(230, mtu − 5)`.
2. A compressed upload where the stream fits in START (no DATA), and where it spans many chunks.
3. Compressed START refused, falling back to raw within the same session.
4. Auto-complete mid-DATA: no END is sent and the session waits for 0x73.
5. Refresh timeout (0x74) gives FAILED_UNCERTAIN; auth-required gives FAILED_NOT_SENT.
6. A disconnect in each state gives the right `Outcome`, and the refresh-start marker is set correctly.
7. Config parser: real blobs captured from the label in Phase 7 (added later), plus
   built blobs with legacy and length-prefixed packets, a truncated blob, a
   missing display packet, a code map that isn't a permutation, and an
   oversize total.

**Done when:** `python tools/run_host_tests.py` passes, and every row of 1.1
is covered.

#### 1.4 Driver hooks for the dashboard (M)

These are the integration points spec §8 lists, added to `OpenDisplay`:

| Hook | Behaviour |
|---|---|
| `request_upload(bool force)` | Starts a transfer if idle. Returns `false` if busy, and never queues. `force` skips the unchanged-hash check for this one request (spec §8, heartbeat or manual refresh with identical pixels). |
| `bool busy() const` | True from the start of discovery until `finish_()`. |
| `add_on_complete_callback(std::function<void(Outcome)>)` | Called from `finish_()` after cleanup. SUCCESS only on 0x73, at the same point as `last_sent_hash_`. |
| `refresh_may_have_started_ms()` | Set when the last DATA chunk is handed off, when a START carries the whole payload, when END is sent, and on auto-complete (spec §5.1). Cleared at the start of each transfer. |
| `set_external_scheduling(true)` | Dashboard mode. `update()` (polling or `component.update`) does not start a transfer, and `update_pending_` is not used. The owner decides. |
| `on_panel_learned` path | Unchanged. When the label reports a different panel, `config_done_()` redraws through the writer, which draws the same frozen snapshot (D4). |

Also:

- **Panel validation for the dashboard (spec §6.4).** If the dashboard is
  enabled and the label's config isn't exactly 400×300 BWRY, end with
  FAILED_NOT_SENT and the log line "dashboard needs 400x300 BWRY, label is …".
  Generic mode keeps adopting any panel that fits.
- **Diagnostics.** Log `draw requested`, `upload started` and
  `refresh confirmed` as separate lines (spec §8). Add the outcome to the
  existing success and failure lines.

**Done when:** host tests cover `force`, busy refusal, every outcome, and the
refresh marker in each auto-complete or END path, and the generic-mode YAML
behaves exactly as before.

#### 1.5 Protocol bring-up on the label (S, needs hardware)

Done in Phase 7, but listed here because it closes the protocol work. Use
generic mode with a test-pattern lambda:

1. Name discovery finds the label. Label MAC on the web page shows it. Reboot
   and confirm the saved address is reused without searching.
2. Capture the raw CONFIG_READ blob (temporary VERBOSE hex dump) into
   `tests/host/data/` for `test_config.cpp`. Check the logged panel IC, size,
   scheme, rotation and transmission modes.
3. **Colour check:** four vertical bands of black, white, yellow and red.
   Confirm each band shows on the panel in the colour it was drawn, which
   checks the code map.
4. Upload once compressed and once with `compression: false`. Record both
   transfer times and the refresh time.
5. Confirm the client disconnects after 0x73, and that pulling the label out
   of range gives a clean timeout and the name is looked up again on the next
   upload.

---

### Phase 2: Font and sprite assets

#### 2.1 Generator emits the dashboard asset set (S)

Extend `tools/build_display_fonts.py` with `--dashboard DIR`, which writes:

- `dashboard_assets.h`: types (`Glyph`, `Font`, `Sprite`) and `extern`
  declarations, with no data;
- `dashboard_assets.cpp`: `const` tables for
  - `text14`: `ter-u14n.bdf`, ASCII U+0020–U+007E plus `°`, `µ`, `³` (98 glyphs);
  - `text24`: `ter-u24b.bdf`, the same 98 glyphs in bold, for the header, labels,
    `°C`/`°F` and `%`;
  - `bold32`: `ter-u32b.bdf`, `0123456789.- °FC%` (17 glyphs);
  - `pm_units`: the 30×12 sprite, 4-byte stride.

Keep the existing glyph struct layout (16 bytes), sorted by code point, so the
renderer can binary-search. Drop the 24 px numeric table from the dashboard output only;
the existing `fonts/` outputs stay for the preview.

**Done when:** running the generator twice gives identical files; the bitmap
byte counts are 1,372, 4,704, 1,088 and 48 (spec §3.1, §3.2); a host test compares
every glyph row with the BDF source.

#### 2.2 Remove duplicate ESPHome fonts in dashboard mode (S)

Spec §3.3 says not to link ESPHome `font:` assets alongside the custom
tables. The three `font:` entries are only used by the old lambda, so they
are removed when the YAML moves to the dashboard in 6.3.

---

### Phase 3: Primitive renderer (spec §6)

#### 3.1 Primitives (M)

In `dashboard_render.h/.cpp`:

- `Raster`, `Ink` (values 0–3 matching the driver: black, white, yellow, red).
- `fill_rect`, `hline`, `blit_1bpp`. Clip in signed `int` before touching
  memory; reject non-positive sizes; when clipped, the source offset follows
  the clip (spec §6.1).
- `utf8_next()`: bounded decoding; invalid bytes and unsupported code points
  become `?` in text (spec §6.2).
- `measure_text`, `draw_text` using the glyph's `left`, `top` and `advance`
  (spec §6.2). Text is centred with the spec's top-bearing / bottom-bearing
  rule; the result for the current tables is checked in tests (63 / 54 / 64 at
  centre y = 70).

#### 3.2 `draw_dashboard()` (M)

Implements spec §6.3 exactly, from a `DashboardSnapshot` (Phase 4) only. No
clock or sensor reads.

- Header: 24 px bold title at (12, 3), shortened with `...` to 16 characters; stamp right
  aligned at 388; rule `(12, 30, 376, 1)`.
- Eight cells from a constant table: origin, label, unit kind, column anchors
  (spec §2.2, §2.3).
- For each cell: format, measure at 32 px, choose `----` if it doesn't fit,
  choose colours from the visual state (spec §4.2), fill, then label, then
  value, then unit.
- PM sprite at `(158, cell_y + 24)`, left out when the value is `--`.

#### 3.3 Host pixel tests (M)

`test_render.cpp` draws into a 400×300 `Ink` array with canary bytes around
it, and writes a `.pnm` per scenario to `output/host-render/` for visual
comparison with `sen65-status-preview.html`.

Checks (spec §9, Pixel rendering):

- The eight fills cover exactly `184 × 60` at the listed origins; the gutters
  and the 8 px bottom margin stay white; the rule's last pixel is (387, 30).
- Label, value and unit anchors: unit left x 158 / 356; value right edge
  152 / 350; 6 px gap from value to unit.
- The value cases in spec §9: CO2 `9999`, PM `0` and `1000`, gas `500`, `-40.0`,
  six-character negative temperature, HUM `100`, a rounding carry such as
  `99.96 → 100.0`, and widths one character over the limit giving `----`.
- Colours: labels red on white, black on yellow, white on red; values and units
  black on white and yellow, white on red; sprite zero bits leave the
  background as it was.
- Header: long and unsupported titles, `NO SAMPLE YET`, the `-- --- --:--` error stamp, and a
  stamp around midnight.
- Running the same snapshot twice gives identical pixels.

**Done when:** all checks pass and the PNMs match the preview, apart from the
differences the spec says override the preview (red labels, the overflow
style).

---

### Phase 4: Value model (spec §4)

#### 4.1 Types (S)

In `dashboard_model.h`:

```cpp
enum class Metric : uint8_t { PM2_5, PM10, PM1, CO2, VOC, NOX, TEMP, HUM, COUNT };
enum class Availability : uint8_t { NEVER_SAMPLED, NOT_CONFIGURED, VALID, STALE_OR_INVALID };
enum class Category : uint8_t { NORMAL, ELEVATED, HIGH };

struct MetricState {
  float raw;                  // last accepted value, unformatted
  uint32_t sample_ms;         // monotonic
  Availability availability;
  Category committed;         // hysteresis + persistence result
  Category candidate;
  uint32_t candidate_since_ms;
};

struct DashboardSnapshot {     // what draw_dashboard() reads; plain values
  char title[25];
  char stamp[16];             // frozen "23 NOV 14:35", "NO SAMPLE YET" or "-- --- --:--"
  struct Cell { char value[16]; Availability a; Category c; bool overflow; } cells[8];
  bool fahrenheit;
};
```

`latest`, `in_flight` and `displayed` (spec §5.3) are three of these
record sets, not three frame buffers. Check the `sizeof` in a test; the target
is well under 1 KiB in total (spec §7).

#### 4.2 Validity and formatting (S)

- Valid ranges: PM ≥ 0, CO2 ≥ 0, HUM 0–100 inclusive, VOC and NOx through the
  driver's validity. Reject NaN and infinity before anything else.
- **Check on the hardware:** how `sen6x` reports the gas-index warm-up and
  invalid readings (NaN, or a sentinel). Record the answer in the protocol
  notes, and map it to `NEVER_SAMPLED` or `STALE_OR_INVALID`.
- Formatting: `lround` at the displayed precision; temperature converted to
  °F before rounding; no `-0.0`; ASCII only; `snprintf` into 16 bytes with the
  result checked; truncation becomes overflow (spec §4.1).

#### 4.3 Classifier (M)

A table-driven implementation of spec §4.3:

- bands per classifier: enter yellow, enter red, leave yellow, leave red;
- the PM group uses PM2.5 for all three PM cells; a PM cell with its own
  invalid value keeps its own invalid appearance; with PM2.5 missing, valid
  PM1 and PM10 show on white and the log says so;
- CO2 classification is off unless bands are configured;
- persistence: a candidate must hold for 180 s of fresh samples, timed from
  the first qualifying sample, and resets on a candidate change, lost
  validity or a stale stream. The first valid sample commits at once.

#### 4.4 Staleness (S)

`now − sample_ms > 180 s` makes a configured source `STALE_OR_INVALID`. It is
checked on every scheduler tick, not only on sensor callbacks.

**Done when (4.1–4.4):** `test_model.cpp` covers each threshold exactly,
equality at exit thresholds, an interrupted candidate, going stale without a
callback, the PM group rules, and CO2 being absent, configured or stale.

---

### Phase 5: Refresh scheduler (spec §5)

#### 5.1 Pure policy (L)

`dashboard_scheduler.h/.cpp` takes `now_ms` and the model, and returns at most
one action per tick: `NONE`, `UPLOAD`, or `UPLOAD_FORCED`. It owns:

| State | Rule |
|---|---|
| `last_success_ms` | From `Outcome::SUCCESS` only. Routine (30 min) and heartbeat (60 min) are measured from it. |
| `guard_until_ms` | `refresh_may_have_started_ms + 15 min`. Also set after a failure that might have refreshed. |
| `retry_at_ms` | Backoff of 2, 5, then 15 min, capped at 15 min, and never inside the guard. Only one pending. |
| `pending_reason` | A bit set of TRANSITION, NUMERIC, HEARTBEAT, MANUAL, BOOT. Requests are merged. |
| boot state | Wait up to 180 s for the first full SEN65 batch, then send once; don't repaint warm-up placeholders again. |

The decision on each tick:

1. If an upload is running, do nothing (requests stay merged).
2. Work out whether there is a meaningful numeric change against `displayed`
   (spec §5.2): the deadband is met **and** the formatted string differs.
   Recompute this every time; don't keep a sticky dirty flag.
3. A transition is a committed category, availability or overflow change
   against `displayed`.
4. Eligible if any of:
   - transition;
   - numeric change and 30 min since `last_success_ms`;
   - heartbeat (60 min);
   - manual;
   - the boot rule;
   - a retry is due.
5. If eligible and the guard has passed, take a snapshot (`latest` → `in_flight`,
   freezing the stamp) and return `UPLOAD`. Return `UPLOAD_FORCED` for
   heartbeat, manual, or when the time is unknown.
6. On SUCCESS, promote `in_flight` to `displayed` and set `last_success_ms`.
   On failure, keep `displayed`, schedule a retry, and on the next attempt take
   a new snapshot (spec §5.3).

#### 5.2 Scheduler tests with a fake clock (M)

`test_scheduler.cpp` drives simulated hours of samples through the model and
scheduler. It covers every case in spec §9 "Scheduling and delivery":

- stable data: no upload except the hourly heartbeat;
- 0.1 °C or 1 % RH jitter: nothing; an accumulated 0.5 °C or 3 %: eligible at 30 min;
- PM deadbands against the displayed value, including 0 and slow drift, and a
  change that comes back clears eligibility;
- a transition sends early, but never inside the 15 min guard, including after
  a lost 0x73;
- samples and manual presses during an upload: `in_flight` is never changed,
  and requests merge;
- a failure at each protocol stage: no promotion, backoff respected, no burst
  of reconnects;
- no wall clock: heartbeat and manual force one upload;
- first boot with no samples, CO2 absent or stale, a new label, and a label
  with the wrong panel.

**Done when:** all of these pass.

---

### Phase 6: ESPHome integration

#### 6.1 Component glue (M)

`dashboard.h/.cpp`, class `AqiDashboard`, owned by `OpenDisplay` when the
`dashboard:` block is present:

- Subscribes to each bound sensor with `add_on_state_callback`, and records
  the raw value and `millis()` into `latest`. The sample stamp is
  `time->now()` at the callback if the time is valid; otherwise it is marked
  unknown.
- A `set_interval(1000)` tick runs staleness, classification and the
  scheduler, then calls `display->request_upload(force)`.
- Writer (D4): `display->set_writer([this](display::Display &){ draw_dashboard(&raster_, &in_flight_); })`.
- Completion callback: passes the `Outcome` to the scheduler.
- `request_manual()` for the Refresh E-Paper button.
- Logs every scheduler decision at DEBUG, with the reason bits.

#### 6.2 Configuration schema (M)

In `display.py`:

```yaml
display:
  - platform: opendisplay
    id: epaper
    ble_client_id: esl_client
    ble_name: ${esl_name}
    refresh_mode: FULL
    update_interval: never          # required in dashboard mode
    dashboard:
      time_id: ha_time              # required; the Timezone select sets its zone
      title: INDOOR AIR             # ≤ 16 supported characters
      temperature_unit: C           # C or F
      sensors:
        pm_2_5: pm_2_5
        pm_10_0: pm_10_0
        pm_1_0: pm_1_0
        voc_index: voc_index
        nox_index: nox_index
        temperature: temperature
        humidity: humidity
        co2: my_co2                 # optional
      timing:                       # defaults from spec §5.1
        routine: 30min
        heartbeat: 60min
        guard: 15min
        persistence: 3min
        stale_after: 180s
        startup_wait: 180s
      deadbands:                    # defaults from spec §5.2
        pm_absolute: 2
        pm_relative: 20%
        co2: 100
        voc: 25
        nox: 10
        temperature: 0.5
        humidity: 3
      bands:                        # defaults from spec §4.3; co2 has none by default
        pm_2_5: {enter_yellow: 15, enter_red: 35, leave_yellow: 12, leave_red: 30}
        voc:    {enter_yellow: 150, enter_red: 400, leave_yellow: 130, leave_red: 350}
        nox:    {enter_yellow: 20, enter_red: 300, leave_yellow: 15, leave_red: 250}
```

Validation:

- `dashboard` excludes `lambda` and `pages`;
- `width`, `height` and `color_scheme`, if given, must be 400 / 300 / BWRY;
- `update_interval` must be `never`;
- bands must satisfy `leave_yellow < enter_yellow < leave_red < enter_red`
  (spec §8);
- all intervals must be positive;
- the title may only use supported glyphs, with a length check.

#### 6.3 Device YAML migration (S)

- `time:` (`ha_time`) and the Timezone select already exist; point
  `dashboard: time_id` at `ha_time`.
- Replace the lambda with the `dashboard:` block; set `update_interval: never`.
- Remove the three `font:` entries (2.2).
- Point the Refresh E-Paper button at `lambda: id(epaper).dashboard()->request_manual();`.
- Keep all sensor publishing, including PM4, the web page entities, and the
  BLE name and MAC controls (spec §1, §8).
- Update `key-features.md` (page and refresh sections) and the comments at the
  top of the YAML.

**Done when:** the firmware compiles on ESPHome 2026.11.0 with no component
warnings; the linked map shows only the dashboard asset tables and no 24 px
font; the RAM and flash change is recorded, against spec §7's estimate of
about 4.3 KB of assets and under 1 KiB of RAM.

---

### Phase 7: On-device validation

1. The protocol bring-up in 1.5.
2. Flash the dashboard build. Check:
   - the first SEN65 batch is painted within about 3 min of boot;
   - no refresh at 5 min with stable air;
   - a heartbeat at 60 min.
3. Push a category transition (a match or incense near the sensor for PM; an
   alcohol wipe for VOC). Confirm the 3 min persistence, the early send, and
   the 15 min guard.
4. Read the panel at its native size: the 24 px labels and the 32 px values
   in all four inks; black labels on yellow; white on red.
5. Measure the actual transfer and refresh durations (log them) and compare
   with the 30 s assumption.
6. Fault cases:
   - label out of range;
   - label power-cycled mid-upload;
   - Wi-Fi down, so no time;
   - SEN65 unplugged, so stale cells turn yellow with `--`.
7. Heap after 24 h: record the free heap and the largest free block before and
   after uploads, to check for fragmentation.

**Done when:** every check passes or has a logged follow-up item, and the
captured config blob is checked into the host tests.

## 4. Order and milestones

| Milestone | Contents | Can be checked without hardware |
|---|---|---|
| M1: Protocol verified on host | 1.1, 1.2, 1.3 | Yes |
| M2: Assets and renderer | 2.1, 3.1–3.3 | Yes; PNM review |
| M3: Model and scheduler | 4.x, 5.x | Yes |
| M4: Integrated firmware | 1.4, 6.x | Compiles; behaviour via logs |
| M5: On the label | 1.5, 7 | No |

M1 and M2 are independent and can be done in either order. Doing 1.5 early (a
test-pattern upload with the current code) is worth it if the label is to
hand: it is the cheapest way to find a wrong wire assumption before building
on top of it.

## 5. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| A protocol offset or framing assumption is wrong | Nothing displays, or wrong colours | 1.1 source check; 1.5 colour bands before the dashboard work depends on it |
| uzlib on the label rejects this encoder's stream | Compressed uploads fail, wasting a round trip each time | The fallback already exists; 1.5 confirms; if it always falls back, cache "raw only" per label |
| Heap fragmentation from the compressor and config buffers over days | An upload fails to allocate | Measure in 7.7; if needed, reserve the zlib buffer once at boot, as the frame buffer is |
| `sen6x` reports gas warm-up differently from what the model assumes | Wrong cells during the first minutes | Checked in 4.2 on hardware |
| No clock (Home Assistant and NTP both unreachable) | `-- --- --:--` stamp; the page is still drawn and sent | SNTP backup (`sntp_time`) is in place; the page never waits for the clock |
| BLE scanning and Wi-Fi coexistence on the C6 | Missed advertisements, slow discovery | Discovery timeout already 120 s; record the times in Phase 7 |

## 6. Questions for the project owner

None of these block M1–M3.

1. **CO2.** Is an external CO2 sensor planned? If so, which one, and what
   bands? Until then the cell shows `CO2 -- ppm` on white (spec §4.1).
2. **Time source.** Settled: Home Assistant time, with SNTP (`pool.ntp.org`)
   as a backup; timezone from the `timezone` substitution and the Timezone
   select. Should SNTP use a local server (the router) instead?
3. **Title and units.** Is `INDOOR AIR` and °C right for this device?
4. **Bands.** Are the proposed bands in spec §4.3 acceptable as first
   defaults? They are marked as tuning values, not health limits.

## 7. Definition of done

- Every phase's "done when" is met.
- `python tools/run_host_tests.py` passes, and the PNM renders are reviewed
  against the preview.
- The firmware builds on ESPHome 2026.11.0 with no component warnings, and
  its RAM and flash use are recorded in `key-features.md`.
- The Phase 7 on-device checklist is complete on the 4.2" BWRY label.
- `key-features.md` describes the dashboard as implemented, and the spec's
  "proposed" parameters are visible in the YAML.
