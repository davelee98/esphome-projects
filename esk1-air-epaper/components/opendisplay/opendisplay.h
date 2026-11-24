#pragma once

// OpenDisplay e-paper ESL driven directly over BLE.
//
// The frame is drawn locally into a buffer already in the wire format, then
// pushed to the label with the DIRECT_WRITE flow of the OpenDisplay BLE
// protocol. The protocol itself lives in od_session.h (transport-independent,
// tested on a host); this class is the ESPHome display and the GATT glue:
//
//   find the label (MAC, or BLE name) -> connect -> discover 0x2446
//   -> od::Session: CONFIG_READ, START, DATA..., END, refresh -> disconnect
//
// Colour schemes: MONO, BWR, BWY and BWRY. Spectra is out of scope.
//
// Frame buffer: MAX_FRAME_BYTES (a 400x300 BWRY frame) is reserved once at
// boot. The panel's size and colour scheme come from the label's config on
// every connection; any panel whose frame fits is drawn into the same block.
//
// Finding the label: a non-zero ble_client mac_address is an override and is
// always used. With 00:00:00:00:00:00 the label is found by its advertised BLE
// name (ble_name; OpenDisplay firmware advertises "OD" + 6 hex digits); a
// trailing '*' matches a prefix. The address found is kept in flash for that
// name, and forgotten when the label stops answering there or the name changes.
//
// Owned scheduling (for a page component such as aqi_dashboard):
// set_external_scheduling() makes
// update() a request to the owner instead of an upload; the owner calls
// request_upload() and hears the outcome through add_on_complete_callback().
//
// Direct write only, by design. Not implemented: encrypted sessions
// (AUTHENTICATE 0x0050) and the grey / RGB colour schemes.

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "ink.h"
#include "od_session.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace esphome::opendisplay {

namespace espbt = esphome::esp32_ble_tracker;

// The reserved frame buffer: a 400x300 BWRY frame, 2 bits per pixel. Keep in
// step with MAX_FRAME_BYTES in display.py.
static constexpr size_t MAX_FRAME_BYTES = 400 * 300 * 2 / 8;  // 30,000

// DisplayConfig.color_scheme (opendisplay_structs.h), OD_ prefix dropped.
// The whole enum is mirrored so the label's value can be named in the log;
// only MONO, BWR, BWY and BWRY can be drawn.
enum ColorScheme : uint8_t {
  COLOR_SCHEME_MONO = 0,
  COLOR_SCHEME_BWR = 1,
  COLOR_SCHEME_BWY = 2,
  COLOR_SCHEME_BWRY = 3,
  COLOR_SCHEME_BWGBRY = 4,
  COLOR_SCHEME_GRAY4 = 5,
  COLOR_SCHEME_GRAY16 = 6,
  COLOR_SCHEME_SEVEN_COLOR = 7,
  COLOR_SCHEME_BWGBRY_SPLIT = 8,
  // 9 is skipped on purpose.
  COLOR_SCHEME_BWRY16 = 10,  // mode-level only; never a panel's scheme
  COLOR_SCHEME_RGB565 = 100,
  COLOR_SCHEME_RGB888 = 101,
  COLOR_SCHEME_RGB16BPC = 102,
};

// The colours a drawing lambda uses, one per ink (ink.h). Only these are
// drawn as themselves: COLOR_OFF / Color(255, 255, 255) is white paper,
// COLOR_ON is black, and red and yellow are exactly these values. Any other
// colour is not an ink; it is drawn black and named in the log. A panel
// without red or yellow draws that ink black (MONO) or in its accent (BWR /
// BWY).
inline const Color INK_WHITE = display::COLOR_OFF;
inline const Color INK_BLACK = display::COLOR_ON;
inline const Color INK_RED = Color(255, 0, 0);
inline const Color INK_YELLOW = Color(255, 255, 0);

// Refresh byte of DIRECT_WRITE_END.
enum RefreshMode : uint8_t {
  REFRESH_MODE_FULL = 0,
  REFRESH_MODE_FAST = 1,
};

// What request_upload() did.
enum class UploadStart : uint8_t {
  STARTED,    // a transfer is under way; the complete callback follows
  UNCHANGED,  // the frame matches the last one displayed; nothing sent
  BUSY,       // a transfer is already running
  NO_LABEL,   // no MAC and no BLE name to find the label by
  FAILED,     // not ready (no frame buffer)
};

class OpenDisplay : public display::DisplayBuffer,
                    public ble_client::BLEClientNode,
                    public espbt::ESPBTDeviceListener,
                    protected od::SessionHost {
 public:
  void set_panel_size(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_color_scheme(ColorScheme scheme) { this->color_scheme_ = scheme; }
  void set_refresh_mode(RefreshMode mode) { this->refresh_mode_ = mode; }
  void set_skip_unchanged(bool skip) { this->skip_unchanged_ = skip; }
  void set_compression(bool compression) { this->compression_ = compression; }

  // The advertised name to find the label by. Takes effect at once: the
  // address found for the old name is forgotten and the new one looked up on
  // the next upload (which this starts).
  void set_ble_name(const std::string &name);
  const std::string &get_ble_name() const { return this->ble_name_; }
  // The label's address: the override, the one found by name, or "" if none.
  std::string get_label_mac();
  bool has_mac_override() const { return this->mac_override_; }
  bool parse_device(const espbt::ESPBTDevice &device) override;

  // ---- Owned scheduling ----
  // update() (polling, component.update) calls `on_update` instead of
  // uploading, and nothing is queued while busy: the owner decides.
  void set_external_scheduling(std::function<void()> &&on_update) {
    this->external_update_ = std::move(on_update);
  }
  // Starts a transfer of a freshly drawn frame. force skips the
  // unchanged-frame check this once.
  UploadStart request_upload(bool force);
  bool busy() const { return this->state_ != State::IDLE; }
  // Called after every transfer that request_upload() or update() started.
  // has_marker / marker_ms: when a panel refresh could first have begun.
  void add_on_complete_callback(std::function<void(od::Result, bool has_marker, uint32_t marker_ms)> &&cb) {
    this->complete_callbacks_.push_back(std::move(cb));
  }
  // Refuse any other panel (for a fixed page that needs exactly this one).
  void set_required_panel(uint16_t width, uint16_t height, ColorScheme scheme) {
    this->required_width_ = width;
    this->required_height_ = height;
    this->required_scheme_ = scheme;
  }
  // Draws one ink at physical coordinates, for a page component's renderer.
  void put_ink(int x, int y, Ink ink) { this->write_ink_(x, y, this->panel_ink_(ink)); }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  // After ble_client (AFTER_BLUETOOTH), whose setup() enables the client.
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH - 1.0f; }

  void fill(Color color) override;
  display::DisplayType get_display_type() override {
    return this->color_scheme_ == COLOR_SCHEME_MONO ? display::DISPLAY_TYPE_BINARY : display::DISPLAY_TYPE_COLOR;
  }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

 protected:
  enum class State : uint8_t {
    IDLE,
    DISCOVERING,  // listening for an advertisement with ble_name
    CONNECTING,   // waiting for the label to be found, connected and discovered
    SESSION,      // od::Session running over the connection
  };

  // Address found by name, kept in flash.
  struct SavedLabel {
    uint32_t name_hash;
    uint64_t address;
    uint8_t address_type;
  } __attribute__((packed));

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  void write_ink_(int x, int y, Ink ink);

  // The ink a colour names exactly (see INK_*); anything else is black and
  // counted for the log.
  Ink ink_of_(Color color);
  // An ink the panel lacks becomes the one it has: black on MONO, the accent
  // on BWR / BWY.
  Ink panel_ink_(Ink ink) const;
  void report_foreign_colours_();
  uint8_t bits_per_pixel_() const { return this->color_scheme_ == COLOR_SCHEME_BWRY ? 2 : 1; }
  bool has_accent_plane_() const {
    return this->color_scheme_ == COLOR_SCHEME_BWR || this->color_scheme_ == COLOR_SCHEME_BWY;
  }
  size_t row_bytes_() const { return (this->width_ * this->bits_per_pixel_() + 7u) / 8u; }
  size_t plane_bytes_() const { return this->row_bytes_() * this->height_; }
  size_t buffer_bytes_() const { return frame_bytes_(this->width_, this->height_, this->color_scheme_); }
  static bool scheme_supported_(uint8_t scheme) {
    return scheme == COLOR_SCHEME_MONO || scheme == COLOR_SCHEME_BWR || scheme == COLOR_SCHEME_BWY ||
           scheme == COLOR_SCHEME_BWRY;
  }
  static size_t frame_bytes_(uint32_t width, uint32_t height, uint8_t scheme) {
    uint32_t bpp = scheme == COLOR_SCHEME_BWRY ? 2u : 1u;
    uint32_t planes = (scheme == COLOR_SCHEME_BWR || scheme == COLOR_SCHEME_BWY) ? 2u : 1u;
    return (width * bpp + 7u) / 8u * height * planes;
  }
  bool panel_known_() const { return this->width_ != 0 && this->height_ != 0; }
  // Takes on a panel if its frame fits; else returns false with *reason.
  bool adopt_panel_(uint16_t width, uint16_t height, uint8_t scheme, const char **reason);
  bool required_panel_ok_(const char **reason) const;
  void draw_frame_();
  uint32_t buffer_hash_() const;
  bool set_bwry_codes_(uint16_t panel_ic_type, const uint8_t *code_map, const char **reason);
  void apply_bwry_codes_();
  void restore_bwry_codes_();

  UploadStart start_upload_(bool force);
  void set_state_(State state, uint32_t timeout_ms);
  bool name_matches_(const char *name, size_t len) const;
  void forget_address_();
  void connect_();
  void begin_session_();
  void finish_(od::Result result, const char *reason);

  // od::SessionHost
  bool od_write(const uint8_t *data, size_t len, bool data_chunk) override;
  uint16_t od_mtu() override { return this->mtu_; }
  bool od_config_ready(const od::ConfigResult &cfg, const char **reason) override;
  void od_frame(const uint8_t **data, size_t *len) override;
  void od_finished(od::Result result, const char *reason) override;
  void od_log(od::LogLevel level, const char *message) override;

  // Configuration
  uint16_t width_{0};
  uint16_t height_{0};
  ColorScheme color_scheme_{COLOR_SCHEME_MONO};
  RefreshMode refresh_mode_{REFRESH_MODE_FULL};
  bool skip_unchanged_{true};
  bool compression_{true};
  uint16_t required_width_{0};
  uint16_t required_height_{0};
  ColorScheme required_scheme_{COLOR_SCHEME_MONO};

  // Finding the label
  std::string ble_name_;
  bool mac_override_{false};
  bool name_change_pending_{false};
  bool setup_done_{false};
  ESPPreferenceObject pref_;

  // Connection
  uint16_t char_handle_{0};
  bool char_write_rsp_{true};
  bool char_write_no_rsp_{false};
  uint16_t mtu_{23};

  // Transfer
  State state_{State::IDLE};
  uint32_t state_started_ms_{0};
  uint32_t state_timeout_ms_{0};
  uint32_t upload_started_ms_{0};
  od::Session session_{this};
  uint32_t frame_hash_{0};
  uint32_t last_sent_hash_{0};
  bool has_sent_{false};
  bool drawn_{false};           // the buffer holds this upload's frame, at the current panel size
  uint32_t foreign_pixels_{0};  // pixels drawn in a colour that is not an ink, this frame
  Color foreign_colour_{};      // the first such colour
  bool update_pending_{false};  // update() arrived during a transfer (generic mode)
  std::function<void()> external_update_;
  std::vector<std::function<void(od::Result, bool, uint32_t)>> complete_callbacks_;

  // BWRY: the buffer holds logical colours (Ink). When the label's code table
  // is not the identity, the buffer is mapped to panel codes for the upload
  // and back afterwards.
  bool bwry_remap_{false};
  bool bwry_applied_{false};
  std::array<uint8_t, 256> bwry_lut_{};
  std::array<uint8_t, 256> bwry_unlut_{};
};

}  // namespace esphome::opendisplay

#endif  // USE_ESP32
