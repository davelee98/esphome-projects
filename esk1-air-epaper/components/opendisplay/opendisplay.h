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
// Not implemented: encrypted sessions (AUTHENTICATE 0x0050), the PIPE
// sliding window (0x0080..0x0082) and partial refresh.

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/core/component.h"

#include "zlib_deflate.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <vector>

namespace esphome::opendisplay {

namespace espbt = esphome::esp32_ble_tracker;

// Wire value of the firmware's DisplayConfig.color_scheme.
enum ColorScheme : uint8_t {
  COLOR_SCHEME_MONO = 0,
  COLOR_SCHEME_BWR = 1,
  COLOR_SCHEME_BWY = 2,
};

// Refresh byte of DIRECT_WRITE_END.
enum RefreshMode : uint8_t {
  REFRESH_MODE_FULL = 0,
  REFRESH_MODE_FAST = 1,
};

class OpenDisplay : public display::DisplayBuffer, public ble_client::BLEClientNode {
 public:
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
  };

  // A pixel resolved to what the panel can show.
  enum class Ink : uint8_t { WHITE, BLACK, ACCENT };

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  Ink classify_(Color color) const;
  size_t row_bytes_() const { return (this->width_ + 7u) / 8u; }
  size_t plane_bytes_() const { return this->row_bytes_() * this->height_; }
  size_t buffer_bytes_() const {
    return this->plane_bytes_() * (this->color_scheme_ == COLOR_SCHEME_MONO ? 1u : 2u);
  }
  uint32_t buffer_hash_() const;

  void set_state_(State state, uint32_t timeout_ms);
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
  bool update_pending_{false};     // update() arrived during a transfer
  std::vector<uint8_t> config_;    // CONFIG_READ blob being assembled
  uint16_t config_total_{0};
  std::vector<uint8_t> tx_;        // reusable command buffer
};

}  // namespace esphome::opendisplay

#endif  // USE_ESP32
