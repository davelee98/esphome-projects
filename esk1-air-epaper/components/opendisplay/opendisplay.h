#pragma once

// OpenDisplay e-paper ESL driven directly over BLE.
//
// The frame is drawn locally into a buffer already in the wire format, then
// pushed to the label with the DIRECT_WRITE flow of the OpenDisplay BLE
// protocol (opendisplay-protocol, OD_PROTOCOL_VERSION 2.2):
//
//   connect -> CONFIG_READ (0x0040)        check panel size / colour scheme,
//                                          learn the transmission modes
//           -> DIRECT_WRITE_START (0x0070) zlib-compressed when the label
//                                          supports it, else uncompressed
//           -> DIRECT_WRITE_DATA (0x0071)  one chunk per ACK
//           -> DIRECT_WRITE_END (0x0072)   then wait for 0x73 / 0x74
//           -> disconnect
//
// Colour schemes: MONO, BWR, BWY and BWRY. Spectra (BWGBRY, BWGBRY_SPLIT,
// SEVEN_COLOR) is out of scope.
//
// Frame buffer: MAX_FRAME_BYTES (a 400x300 BWRY frame) is reserved once at
// boot, whatever the panel. The C6 has no PSRAM, so reserving it early keeps
// the block from being lost to heap fragmentation. The panel's size and colour
// scheme come from the label's own config on every connection, and any panel
// whose frame fits is drawn into the same block. The YAML's width / height /
// color_scheme are only a starting point for labels with no stored config.
//
// Direct write only, by design: PIPE write and PIPE partial (0x0080..0x0082)
// and partial refresh are out of scope for this component.
//
// Not implemented: encrypted sessions (AUTHENTICATE 0x0050) and the GRAY4,
// GRAY16 and RGB colour schemes.
//
// Finding the label: a non-zero ble_client mac_address is an override and is
// always used. With 00:00:00:00:00:00 the label is found by its advertised BLE
// name (ble_name, changeable at run time with set_ble_name()); a trailing '*'
// matches a prefix. The address found is kept in flash for that name, and
// forgotten when the label stops answering there or the name changes.

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "zlib_deflate.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <array>
#include <string>
#include <vector>

namespace esphome::opendisplay {

namespace espbt = esphome::esp32_ble_tracker;

// The reserved frame buffer: a 400x300 BWRY frame, 2 bits per pixel. Keep in
// step with MAX_FRAME_BYTES in display.py.
static constexpr size_t MAX_FRAME_BYTES = 400 * 300 * 2 / 8;  // 30,000

// DisplayConfig.color_scheme: enum ColorScheme of opendisplay-protocol
// (opendisplay_structs.h, as vendored in Firmware_Unified), with the OD_
// prefix dropped. The whole enum is
// mirrored so the label's own value can be named in the log; only MONO, BWR,
// BWY and BWRY can be drawn.
enum ColorScheme : uint8_t {
  COLOR_SCHEME_MONO = 0,           // 1bpp black/white
  COLOR_SCHEME_BWR = 1,            // black/white/red
  COLOR_SCHEME_BWY = 2,            // black/white/yellow
  COLOR_SCHEME_BWRY = 3,           // black/white/red/yellow
  COLOR_SCHEME_BWGBRY = 4,         // Spectra 6: black/white/green/blue/red/yellow
  COLOR_SCHEME_GRAY4 = 5,          // 2bpp 4-level grey
  COLOR_SCHEME_GRAY16 = 6,         // 4bpp 16-level grey
  COLOR_SCHEME_SEVEN_COLOR = 7,    // 7-colour (Spectra/ACeP 7)
  COLOR_SCHEME_BWGBRY_SPLIT = 8,   // Spectra 6, left half-plane then right
  // 9 is skipped on purpose (epaper-dithering's library-local Grayscale8).
  COLOR_SCHEME_BWRY16 = 10,        // BWRY packing, 16-colour multi-pass mode; never a panel's scheme
  COLOR_SCHEME_RGB565 = 100,       // non-e-paper
  COLOR_SCHEME_RGB888 = 101,       // non-e-paper
  COLOR_SCHEME_RGB16BPC = 102,     // non-e-paper
};

// Refresh byte of DIRECT_WRITE_END.
enum RefreshMode : uint8_t {
  REFRESH_MODE_FULL = 0,
  REFRESH_MODE_FAST = 1,
};

class OpenDisplay : public display::DisplayBuffer,
                    public ble_client::BLEClientNode,
                    public espbt::ESPBTDeviceListener {
 public:
  // The advertised name to find the label by. Takes effect at once: the
  // address found for the old name is forgotten and the new one looked up on
  // the next upload (which this starts).
  void set_ble_name(const std::string &name);
  const std::string &get_ble_name() const { return this->ble_name_; }
  // The label's address: the override, the one found by name, or "" if none
  // yet. For a text sensor on the web page.
  std::string get_label_mac();
  bool has_mac_override() const { return this->mac_override_; }
  bool parse_device(const espbt::ESPBTDevice &device) override;

  void set_panel_size(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_color_scheme(ColorScheme scheme) { this->color_scheme_ = scheme; }
  void set_refresh_mode(RefreshMode mode) { this->refresh_mode_ = mode; }
  void set_skip_unchanged(bool skip) { this->skip_unchanged_ = skip; }
  void set_compression(bool compression) { this->compression_ = compression; }

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
    CONNECTING,     // waiting for the label to be found, connected and discovered
    READ_CONFIG,    // CONFIG_READ sent, collecting chunks
    WAIT_START,     // DIRECT_WRITE_START sent
    SENDING,        // a DATA chunk is in flight
    WAIT_END,       // DIRECT_WRITE_END sent
    WAIT_REFRESH,   // END acked, waiting for 0x73 / 0x74
    DISCOVERING,    // listening for an advertisement with ble_name
  };

  // Address found by name, kept in flash.
  struct SavedLabel {
    uint32_t name_hash;
    uint64_t address;
    uint8_t address_type;
  } __attribute__((packed));

  // A pixel resolved to an ink the panel has. The values are the logical
  // colour indices of py-opendisplay's palettes, which BWRY's code_map is
  // indexed by.
  enum class Ink : uint8_t { BLACK = 0, WHITE = 1, YELLOW = 2, RED = 3 };

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  Ink classify_(Color color) const;
  // BWRY packs 2 bits per pixel; the others use 1bpp planes.
  uint8_t bits_per_pixel_() const { return this->color_scheme_ == COLOR_SCHEME_BWRY ? 2 : 1; }
  bool has_accent_plane_() const {
    return this->color_scheme_ == COLOR_SCHEME_BWR || this->color_scheme_ == COLOR_SCHEME_BWY;
  }
  size_t row_bytes_() const { return (this->width_ * this->bits_per_pixel_() + 7u) / 8u; }
  size_t plane_bytes_() const { return this->row_bytes_() * this->height_; }
  // The current frame's share of the reserved buffer.
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
  bool adopt_panel_(uint16_t width, uint16_t height, uint8_t scheme);
  void draw_frame_();
  void config_done_();
  uint32_t buffer_hash_() const;
  bool set_bwry_codes_(uint16_t panel_ic_type, const uint8_t *code_map);

  void set_state_(State state, uint32_t timeout_ms);
  bool name_matches_(const char *name, size_t len) const;
  void forget_address_();
  void connect_();
  void begin_session_();
  void on_frame_(const uint8_t *data, uint16_t len);
  void on_config_frame_(const uint8_t *data, uint16_t len);
  bool check_config_();
  void start_upload_();
  void send_uncompressed_start_();
  void send_command_(uint16_t opcode, const uint8_t *payload, size_t len, bool data_chunk);
  void send_next_chunk_();
  void send_end_();
  void finish_(bool success, const char *reason);

  // Configuration
  uint16_t width_{0};
  uint16_t height_{0};
  ColorScheme color_scheme_{COLOR_SCHEME_MONO};
  RefreshMode refresh_mode_{REFRESH_MODE_FULL};
  bool skip_unchanged_{true};
  bool compression_{true};

  // Finding the label
  std::string ble_name_;
  bool mac_override_{false};       // ble_client was given a real MAC
  bool name_change_pending_{false}; // set_ble_name() during a transfer
  bool setup_done_{false};
  ESPPreferenceObject pref_;

  // Connection
  uint16_t char_handle_{0};
  bool char_write_rsp_{true};     // characteristic supports write-with-response
  bool char_write_no_rsp_{false}; // characteristic supports write-without-response
  uint16_t mtu_{23};

  // Transfer
  State state_{State::IDLE};
  uint32_t state_started_ms_{0};
  uint32_t state_timeout_ms_{0};
  uint32_t upload_started_ms_{0};
  // What the DATA chunks carry: the frame buffer, or the tail of the zlib
  // stream after the bytes that rode in the START frame.
  const uint8_t *tx_src_{nullptr};
  size_t tx_len_{0};
  size_t sent_bytes_{0};
  size_t chunk_bytes_{0};          // size of the chunk in flight
  bool compressed_{false};         // current START is the compressed form
  int16_t transmission_modes_{-1}; // from the label's display config; -1 = unknown
  std::vector<uint8_t> zbuf_;      // zlib stream of the frame being uploaded
  uint32_t frame_hash_{0};         // hash of the frame being uploaded
  uint32_t last_sent_hash_{0};
  bool has_sent_{false};
  bool drawn_{false};              // the buffer holds this upload's frame, at the current panel size
  bool update_pending_{false};     // update() arrived during a transfer
  std::vector<uint8_t> config_;    // CONFIG_READ blob being assembled
  uint16_t config_total_{0};
  std::vector<uint8_t> tx_;        // reusable command buffer
  // BWRY: the buffer holds logical colours (Ink). When the label's code table
  // is not the identity, the buffer is mapped to panel codes for the upload
  // (bwry_lut_) and back afterwards (bwry_unlut_).
  bool bwry_remap_{false};
  bool bwry_applied_{false};
  std::array<uint8_t, 256> bwry_lut_{};
  std::array<uint8_t, 256> bwry_unlut_{};
};

}  // namespace esphome::opendisplay

#endif  // USE_ESP32
