#include "opendisplay.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::opendisplay {

static const char *const TAG = "opendisplay";

// ---- Wire constants (opendisplay-protocol src/opendisplay_protocol.h, 2.2) ----
static const uint16_t OD_SERVICE_UUID = 0x2446;
static const uint16_t OD_CHAR_UUID = 0x2446;

static const uint16_t CMD_CONFIG_READ = 0x0040;
static const uint16_t CMD_DIRECT_WRITE_START = 0x0070;
static const uint16_t CMD_DIRECT_WRITE_DATA = 0x0071;
static const uint16_t CMD_DIRECT_WRITE_END = 0x0072;

static const uint8_t RESP_ACK = 0x00;
static const uint8_t RESP_NACK = 0xFF;
static const uint8_t RESP_AUTH_REQUIRED = 0xFE;
static const uint8_t RESP_CONFIG_READ = 0x40;
static const uint8_t RESP_DIRECT_WRITE_START_ACK = 0x70;
static const uint8_t RESP_DIRECT_WRITE_DATA_ACK = 0x71;
static const uint8_t RESP_DIRECT_WRITE_END_ACK = 0x72;
static const uint8_t RESP_DIRECT_WRITE_REFRESH_SUCCESS = 0x73;
static const uint8_t RESP_DIRECT_WRITE_REFRESH_TIMEOUT = 0x74;

static const size_t CHUNK_SIZE = 230;          // max DATA bytes per 0x71 frame, unencrypted
static const size_t ATT_WRITE_OVERHEAD = 3;    // ATT opcode + handle
static const size_t CMD_HEADER_SIZE = 2;       // big-endian opcode
static const size_t MAX_CONFIG_SIZE = 4096;    // sanity cap on a CONFIG_READ blob
static const size_t MAX_START_PAYLOAD = 200;   // whole START command, opcode included
static const size_t COMPRESSED_START_HEADER = 6;  // opcode(2) + uncompressed_size(4 LE)
// Older firmware's buffered decompressor holds the whole stream in RAM; only
// labels with streaming decompression take more (py-opendisplay protocol/commands.py).
static const size_t MAX_COMPRESSED_SIZE = 50 * 1024;

// DisplayConfig.transmission_modes bits (py-opendisplay models/config.py).
static const uint8_t TRANSMISSION_STREAMING_DECOMPRESSION = 0x01;  // post-2.0 firmware
static const uint8_t TRANSMISSION_ZIP = 0x02;                      // firmware <= 1.81

// Config TLV packet types and their fixed body sizes (py-opendisplay
// protocol/config_parser.py). Types >= 0x80 carry [len:2 LE] after the header.
static const uint8_t PACKET_TYPE_DISPLAY = 0x20;
static const uint8_t LENGTH_PREFIXED_MIN_TYPE = 0x80;
static const size_t DISPLAY_PACKET_SIZE = 46;

static int legacy_packet_size(uint8_t type) {
  switch (type) {
    case 0x01:  // system
    case 0x02:  // manufacturer
    case 0x21:  // led
      return 22;
    case 0x04:  // power
    case 0x23:  // sensor
    case 0x24:  // data bus
    case 0x25:  // binary inputs
      return 30;
    case 0x20:  // display
      return DISPLAY_PACKET_SIZE;
    case 0x26:  // wifi
      return 160;
    case 0x27:  // security
      return 64;
    case 0x28:  // touch
    case 0x29:  // passive buzzer
    case 0x2A:  // nfc
    case 0x2B:  // flash
      return 32;
    case 0x2C:  // data extended
      return 288;
    case 0x2D:  // findmy
      return 128;
    default:
      return -1;
  }
}

// ---- Timeouts (py-opendisplay device/upload.py budgets, ms) ----
static const uint32_t TIMEOUT_CONNECT = 120000;  // the label may advertise slowly
static const uint32_t TIMEOUT_COMMAND = 10000;
static const uint32_t TIMEOUT_DATA_ACK = 90000;  // each ACK follows a blocking panel write
static const uint32_t TIMEOUT_END_ACK = 90000;
static const uint32_t TIMEOUT_REFRESH = 90000;

static const char *state_name(uint8_t state) {
  static const char *const NAMES[] = {"idle",    "connecting", "reading config", "starting",
                                      "sending", "ending",     "refreshing"};
  return state < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[state] : "?";
}

static const char *scheme_name(ColorScheme scheme) {
  switch (scheme) {
    case COLOR_SCHEME_MONO:
      return "MONO";
    case COLOR_SCHEME_BWR:
      return "BWR";
    case COLOR_SCHEME_BWY:
      return "BWY";
    default:
      return "?";
  }
}

// ======================================================================
// Component
// ======================================================================

void OpenDisplay::setup() {
  this->init_internal_(this->buffer_bytes_());
  if (this->buffer_ == nullptr) {
    this->mark_failed();
    return;
  }
  // Connect only while an upload is running. ble_client's auto-connect does
  // the scan-and-connect (learning the label's address type) once enabled.
  this->parent()->set_enabled(false);
}

void OpenDisplay::dump_config() {
  LOG_DISPLAY("", "OpenDisplay", this);
  ESP_LOGCONFIG(TAG,
                "  MAC address   : %s\n"
                "  Panel         : %ux%u %s\n"
                "  Refresh mode  : %s\n"
                "  Skip unchanged: %s\n"
                "  Compression   : %s\n"
                "  Buffer        : %u bytes",
                this->parent()->address_str(), this->width_, this->height_, scheme_name(this->color_scheme_),
                this->refresh_mode_ == REFRESH_MODE_FAST ? "FAST" : "FULL", YESNO(this->skip_unchanged_),
                YESNO(this->compression_), (unsigned) this->buffer_bytes_());
  LOG_UPDATE_INTERVAL(this);
}

void OpenDisplay::update() {
  if (this->state_ != State::IDLE) {
    // The buffer is being sent; redraw once this transfer ends.
    this->update_pending_ = true;
    return;
  }
  this->do_update_();
  this->frame_hash_ = this->buffer_hash_();
  if (this->skip_unchanged_ && this->has_sent_ && this->frame_hash_ == this->last_sent_hash_) {
    ESP_LOGD(TAG, "Frame unchanged, not uploading");
    return;
  }
  ESP_LOGI(TAG, "Uploading frame to %s", this->parent()->address_str());
  this->upload_started_ms_ = millis();
  this->set_state_(State::CONNECTING, TIMEOUT_CONNECT);
  if (this->char_handle_ != 0 && this->node_state == espbt::ClientState::ESTABLISHED) {
    this->begin_session_();
  } else {
    this->parent()->set_enabled(true);
  }
}

void OpenDisplay::loop() {
  // Called both by the application and by the parent BLE client.
  if (this->state_ == State::IDLE || this->state_timeout_ms_ == 0)
    return;
  if (millis() - this->state_started_ms_ > this->state_timeout_ms_) {
    char reason[48];
    snprintf(reason, sizeof(reason), "timed out while %s", state_name(static_cast<uint8_t>(this->state_)));
    this->finish_(false, reason);
  }
}

void OpenDisplay::set_state_(State state, uint32_t timeout_ms) {
  this->state_ = state;
  this->state_started_ms_ = millis();
  this->state_timeout_ms_ = timeout_ms;
}

void OpenDisplay::finish_(bool success, const char *reason) {
  uint32_t elapsed = millis() - this->upload_started_ms_;
  if (success) {
    this->last_sent_hash_ = this->frame_hash_;
    this->has_sent_ = true;
    this->status_clear_warning();
    ESP_LOGI(TAG, "Frame displayed (%u bytes %s in %.1f s)", (unsigned) this->buffer_bytes_(),
             this->compressed_ ? "compressed" : "uncompressed", elapsed / 1000.0f);
  } else {
    this->status_set_warning();
    ESP_LOGW(TAG, "Upload failed: %s", reason);
  }
  this->state_ = State::IDLE;
  this->state_timeout_ms_ = 0;
  this->config_.clear();
  this->config_.shrink_to_fit();
  this->zbuf_.clear();
  this->zbuf_.shrink_to_fit();
  this->tx_src_ = nullptr;
  this->tx_.clear();
  this->tx_.shrink_to_fit();
  // Disconnect so the label can go back to sleep.
  this->parent()->set_enabled(false);
  if (this->update_pending_) {
    this->update_pending_ = false;
    this->defer([this]() { this->update(); });
  }
}

// ======================================================================
// Drawing
// ======================================================================

// COLOR_OFF (all zero) is bare paper and COLOR_ON (all 0xFF) is ink, as on the
// other ESPHome e-paper drivers, so auto-clear gives a white page and default
// text is black. MONO follows the binary convention: any lit colour is ink.
// The colour schemes also take real colours: a saturated red (BWR) or yellow
// (BWY) is the accent ink; anything else is black or white by luminance.
OpenDisplay::Ink OpenDisplay::classify_(Color color) const {
  if (color.raw_32 == 0)
    return Ink::WHITE;
  if (this->color_scheme_ == COLOR_SCHEME_MONO || color.raw_32 == 0xFFFFFFFF)
    return Ink::BLACK;
  bool r = color.r >= 128, g = color.g >= 128, b = color.b >= 128;
  if (this->color_scheme_ == COLOR_SCHEME_BWR && r && !g && !b)
    return Ink::ACCENT;
  if (this->color_scheme_ == COLOR_SCHEME_BWY && r && g && !b)
    return Ink::ACCENT;
  uint32_t luma = (color.r * 77u + color.g * 150u + color.b * 29u) >> 8;
  return luma >= 128 ? Ink::WHITE : Ink::BLACK;
}

// Plane 1 (BW): bit set = white. Plane 2 (accent): bit set = red / yellow.
// 8 px per byte, MSB first, rows padded to a byte. Red also sets the BW bit;
// yellow does not (py-opendisplay encoding/bitplanes.py).
void OpenDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_ || this->buffer_ == nullptr)
    return;
  size_t index = y * this->row_bytes_() + (x >> 3);
  uint8_t mask = 0x80 >> (x & 7);
  Ink ink = this->classify_(color);
  bool bw = ink == Ink::WHITE || (ink == Ink::ACCENT && this->color_scheme_ == COLOR_SCHEME_BWR);
  if (bw) {
    this->buffer_[index] |= mask;
  } else {
    this->buffer_[index] &= ~mask;
  }
  if (this->color_scheme_ != COLOR_SCHEME_MONO) {
    uint8_t *accent = this->buffer_ + this->plane_bytes_();
    if (ink == Ink::ACCENT) {
      accent[index] |= mask;
    } else {
      accent[index] &= ~mask;
    }
  }
}

void OpenDisplay::fill(Color color) {
  if (this->buffer_ == nullptr)
    return;
  // A clipped fill must go pixel by pixel.
  if (this->is_clipping()) {
    display::Display::fill(color);
    return;
  }
  Ink ink = this->classify_(color);
  bool bw = ink == Ink::WHITE || (ink == Ink::ACCENT && this->color_scheme_ == COLOR_SCHEME_BWR);
  memset(this->buffer_, bw ? 0xFF : 0x00, this->plane_bytes_());
  if (this->color_scheme_ != COLOR_SCHEME_MONO)
    memset(this->buffer_ + this->plane_bytes_(), ink == Ink::ACCENT ? 0xFF : 0x00, this->plane_bytes_());
}

uint32_t OpenDisplay::buffer_hash_() const {
  uint32_t hash = 2166136261UL;  // FNV-1a
  size_t len = this->buffer_bytes_();
  for (size_t i = 0; i < len; i++) {
    hash ^= this->buffer_[i];
    hash *= 16777619UL;
  }
  return hash;
}

// ======================================================================
// BLE
// ======================================================================

void OpenDisplay::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK)
        this->mtu_ = param->cfg_mtu.mtu;
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *chr = this->parent()->get_characteristic(OD_SERVICE_UUID, OD_CHAR_UUID);
      if (chr == nullptr) {
        ESP_LOGE(TAG, "[%s] OpenDisplay service/characteristic 0x2446 not found", this->parent()->address_str());
        if (this->state_ != State::IDLE)
          this->finish_(false, "not an OpenDisplay device");
        break;
      }
      this->char_handle_ = chr->handle;
      this->char_write_rsp_ = chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE;
      this->char_write_no_rsp_ = chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
      auto err = this->parent()->register_for_notify(chr->handle);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "register_for_notify failed: %d", err);
        if (this->state_ != State::IDLE)
          this->finish_(false, "could not enable notifications");
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->char_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Notify registration failed: %d", param->reg_for_notify.status);
        if (this->state_ != State::IDLE)
          this->finish_(false, "could not enable notifications");
        break;
      }
      // The handle is kept, so the parent may free its GATT cache now.
      this->node_state = espbt::ClientState::ESTABLISHED;
      ESP_LOGD(TAG, "Connected, MTU %u", this->mtu_);
      if (this->state_ == State::CONNECTING) {
        this->begin_session_();
      } else {
        // Connected without an upload pending (e.g. enabled elsewhere).
        this->parent()->set_enabled(false);
      }
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->char_handle_ && param->notify.value_len > 0)
        this->on_frame_(param->notify.value, param->notify.value_len);
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle == this->char_handle_ && param->write.status != ESP_GATT_OK &&
          this->state_ != State::IDLE) {
        char reason[40];
        snprintf(reason, sizeof(reason), "GATT write error %d", param->write.status);
        this->finish_(false, reason);
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      this->char_handle_ = 0;
      this->mtu_ = 23;
      if (this->state_ != State::IDLE)
        this->finish_(false, "disconnected");
      break;

    default:
      break;
  }
}

void OpenDisplay::send_command_(uint16_t opcode, const uint8_t *payload, size_t len, bool data_chunk) {
  this->tx_.resize(CMD_HEADER_SIZE + len);
  this->tx_[0] = opcode >> 8;
  this->tx_[1] = opcode & 0xFF;
  if (len > 0)
    memcpy(this->tx_.data() + CMD_HEADER_SIZE, payload, len);
  // DATA chunks are acked by notification, so they skip the ATT response when
  // the characteristic allows it (as py-opendisplay does).
  esp_gatt_write_type_t type = ESP_GATT_WRITE_TYPE_RSP;
  if ((data_chunk && this->char_write_no_rsp_) || !this->char_write_rsp_)
    type = ESP_GATT_WRITE_TYPE_NO_RSP;
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->char_handle_, this->tx_.size(), this->tx_.data(), type,
                                      ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    char reason[40];
    snprintf(reason, sizeof(reason), "write_char failed (%d)", err);
    this->finish_(false, reason);
  }
}

void OpenDisplay::begin_session_() {
  this->config_.clear();
  this->config_total_ = 0;
  this->transmission_modes_ = -1;
  this->set_state_(State::READ_CONFIG, TIMEOUT_COMMAND);
  this->send_command_(CMD_CONFIG_READ, nullptr, 0, false);
}

void OpenDisplay::on_frame_(const uint8_t *data, uint16_t len) {
  if (len < 2) {
    ESP_LOGV(TAG, "Short frame ignored");
    return;
  }
  uint8_t status = data[0], echo = data[1];
  ESP_LOGV(TAG, "Frame %02X %02X (%u bytes) in state %s", status, echo, len,
           state_name(static_cast<uint8_t>(this->state_)));

  if (status == RESP_AUTH_REQUIRED) {
    this->finish_(false, "the label has encryption enabled, which this component does not support");
    return;
  }

  switch (this->state_) {
    case State::READ_CONFIG:
      if (echo == RESP_CONFIG_READ)
        this->on_config_frame_(data, len);
      break;

    case State::WAIT_START:
      // A label that cannot take the compressed form answers [FF][70], or
      // [FF][FF] on older firmware.
      if (status == RESP_NACK && this->compressed_ && (echo == RESP_DIRECT_WRITE_START_ACK || echo == 0xFF)) {
        ESP_LOGW(TAG, "Label refused the compressed upload; sending uncompressed");
        this->send_uncompressed_start_();
        break;
      }
      if (echo != RESP_DIRECT_WRITE_START_ACK)
        break;
      if (status != RESP_ACK) {
        this->finish_(false, "DIRECT_WRITE_START refused");
        return;
      }
      this->sent_bytes_ = 0;
      if (this->tx_len_ > 0) {
        this->send_next_chunk_();
      } else {
        this->send_end_();  // the whole zlib stream fit in the START frame
      }
      break;

    case State::SENDING:
      if (echo == RESP_DIRECT_WRITE_END_ACK && status == RESP_ACK) {
        // The firmware's buffer filled and it ended the transfer itself; an
        // explicit END must not follow.
        ESP_LOGD(TAG, "Label auto-completed the transfer");
        this->set_state_(State::WAIT_REFRESH, TIMEOUT_REFRESH);
        break;
      }
      if (echo != RESP_DIRECT_WRITE_DATA_ACK)
        break;
      if (status != RESP_ACK) {
        char reason[48];
        snprintf(reason, sizeof(reason), "DATA NACK at byte %u", (unsigned) this->sent_bytes_);
        this->finish_(false, reason);
        return;
      }
      this->sent_bytes_ += this->chunk_bytes_;
      if (this->sent_bytes_ < this->tx_len_) {
        this->send_next_chunk_();
      } else {
        this->send_end_();
      }
      break;

    case State::WAIT_END:
      if (echo != RESP_DIRECT_WRITE_END_ACK)
        break;
      if (status != RESP_ACK) {
        this->finish_(false, "DIRECT_WRITE_END refused (transfer incomplete)");
        return;
      }
      this->set_state_(State::WAIT_REFRESH, TIMEOUT_REFRESH);
      break;

    case State::WAIT_REFRESH:
      if (echo == RESP_DIRECT_WRITE_REFRESH_SUCCESS) {
        this->finish_(true, nullptr);
      } else if (echo == RESP_DIRECT_WRITE_REFRESH_TIMEOUT) {
        this->finish_(false, "the label reported a panel refresh timeout");
      }
      break;

    default:
      break;
  }
}

// CONFIG_READ answers in chunks:
//   chunk 0 : [00][40][chunk:2 LE][total:2 LE][data...]
//   chunk n : [00][40][chunk:2 LE][data...]
//   no config: [FF][40][00][00]
void OpenDisplay::on_config_frame_(const uint8_t *data, uint16_t len) {
  if (data[0] == RESP_NACK) {
    ESP_LOGW(TAG, "Label has no stored config; cannot verify panel size");
    this->start_upload_();
    return;
  }
  if (len < 4)
    return;
  uint16_t chunk = data[2] | (data[3] << 8);
  const uint8_t *body = data + 4;
  size_t body_len = len - 4;
  if (chunk == 0) {
    if (len < 6)
      return;
    this->config_total_ = data[4] | (data[5] << 8);
    body += 2;
    body_len -= 2;
    this->config_.clear();
    if (this->config_total_ > MAX_CONFIG_SIZE) {
      this->finish_(false, "config too large");
      return;
    }
    this->config_.reserve(this->config_total_);
  } else if (body_len == 0) {
    this->finish_(false, "config read stalled");
    return;
  }
  size_t room = this->config_total_ - this->config_.size();
  this->config_.insert(this->config_.end(), body, body + std::min(body_len, room));
  this->set_state_(State::READ_CONFIG, TIMEOUT_COMMAND);  // restart the timer per chunk
  if (this->config_.size() < this->config_total_)
    return;

  if (!this->check_config_())
    return;  // finish_() already called
  this->start_upload_();
}

// Blob: [length:2 LE][version:1][packets...][crc:2]. Packets are
// [number:1][type:1][fixed body] (types < 0x80) or
// [number:1][type:1][len:2 LE][body] (types >= 0x80).
bool OpenDisplay::check_config_() {
  const auto &cfg = this->config_;
  if (cfg.size() < 5) {
    ESP_LOGW(TAG, "Config too short (%u bytes); not verifying panel", (unsigned) cfg.size());
    return true;
  }
  size_t off = 3, end = cfg.size() - 2;
  while (off + 2 <= end) {
    uint8_t type = cfg[off + 1];
    if (type >= LENGTH_PREFIXED_MIN_TYPE) {
      if (off + 4 > end)
        break;
      off += 4 + (cfg[off + 2] | (cfg[off + 3] << 8));
      continue;
    }
    int size = legacy_packet_size(type);
    if (size < 0 || off + 2 + size > end)
      break;
    if (type == PACKET_TYPE_DISPLAY) {
      const uint8_t *d = cfg.data() + off + 2;
      uint16_t width = d[4] | (d[5] << 8);
      uint16_t height = d[6] | (d[7] << 8);
      uint8_t rotation = d[14];
      uint8_t scheme = d[21];
      this->transmission_modes_ = d[22];
      ESP_LOGD(TAG, "Label reports %ux%u, colour scheme %u, rotation %u, transmission modes 0x%02X", width, height,
               scheme, rotation, d[22]);
      if (width != this->width_ || height != this->height_ || scheme != this->color_scheme_) {
        ESP_LOGE(TAG,
                 "Panel mismatch: label is %ux%u colour scheme %u (0=MONO 1=BWR 2=BWY), "
                 "config says %ux%u %s. Fix width/height/color_scheme in the YAML.",
                 width, height, scheme, this->width_, this->height_, scheme_name(this->color_scheme_));
        this->finish_(false, "panel mismatch");
        return false;
      }
      return true;
    }
    off += 2 + size;
  }
  ESP_LOGW(TAG, "No display packet found in the label's config; not verifying panel");
  return true;
}

void OpenDisplay::send_next_chunk_() {
  size_t max_chunk = CHUNK_SIZE;
  size_t mtu_room = this->mtu_ > ATT_WRITE_OVERHEAD + CMD_HEADER_SIZE
                        ? this->mtu_ - ATT_WRITE_OVERHEAD - CMD_HEADER_SIZE
                        : 1;
  if (mtu_room < max_chunk)
    max_chunk = mtu_room;
  size_t remaining = this->tx_len_ - this->sent_bytes_;
  this->chunk_bytes_ = std::min(remaining, max_chunk);
  this->set_state_(State::SENDING, TIMEOUT_DATA_ACK);
  this->send_command_(CMD_DIRECT_WRITE_DATA, this->tx_src_ + this->sent_bytes_, this->chunk_bytes_, true);
}

void OpenDisplay::send_end_() {
  uint8_t refresh = this->refresh_mode_;
  this->set_state_(State::WAIT_END, TIMEOUT_END_ACK);
  this->send_command_(CMD_DIRECT_WRITE_END, &refresh, 1, false);
}

// Compressed START: [00][70][uncompressed_size:4 LE][first zlib bytes...],
// the rest of the stream follows as DATA chunks. Used when the label's config
// advertises a decompressor (or has no config to say), the stream is smaller
// than the frame, and it fits the label's limit.
void OpenDisplay::start_upload_() {
  const size_t raw_len = this->buffer_bytes_();
  bool modes_known = this->transmission_modes_ >= 0;
  bool streaming = modes_known && (this->transmission_modes_ & TRANSMISSION_STREAMING_DECOMPRESSION);
  bool label_can = !modes_known ||
                   (this->transmission_modes_ & (TRANSMISSION_STREAMING_DECOMPRESSION | TRANSMISSION_ZIP));

  this->compressed_ = false;
  if (this->compression_ && label_can) {
    uint32_t t0 = millis();
    if (!zlib::compress(this->buffer_, raw_len, this->zbuf_)) {
      ESP_LOGW(TAG, "Compression ran out of memory; sending uncompressed");
    } else {
      size_t z_len = this->zbuf_.size();
      ESP_LOGD(TAG, "Compressed %u -> %u bytes in %u ms", (unsigned) raw_len, (unsigned) z_len,
               (unsigned) (millis() - t0));
      if (z_len < raw_len && (streaming || z_len < MAX_COMPRESSED_SIZE))
        this->compressed_ = true;
    }
  } else if (this->compression_) {
    ESP_LOGD(TAG, "Label does not advertise compressed uploads (modes 0x%02X)", this->transmission_modes_);
  }

  if (!this->compressed_) {
    this->send_uncompressed_start_();
    return;
  }

  size_t max_start = MAX_START_PAYLOAD;
  if (this->mtu_ > ATT_WRITE_OVERHEAD && this->mtu_ - ATT_WRITE_OVERHEAD < max_start)
    max_start = this->mtu_ - ATT_WRITE_OVERHEAD;
  size_t first = std::min(this->zbuf_.size(), max_start - COMPRESSED_START_HEADER);
  uint8_t payload[MAX_START_PAYLOAD];
  payload[0] = raw_len & 0xFF;
  payload[1] = (raw_len >> 8) & 0xFF;
  payload[2] = (raw_len >> 16) & 0xFF;
  payload[3] = (raw_len >> 24) & 0xFF;
  memcpy(payload + 4, this->zbuf_.data(), first);
  this->tx_src_ = this->zbuf_.data() + first;
  this->tx_len_ = this->zbuf_.size() - first;
  this->set_state_(State::WAIT_START, TIMEOUT_COMMAND);
  this->send_command_(CMD_DIRECT_WRITE_START, payload, 4 + first, false);
}

// Uncompressed START: bare [00][70]; the whole frame follows as DATA chunks.
void OpenDisplay::send_uncompressed_start_() {
  this->compressed_ = false;
  this->zbuf_.clear();
  this->zbuf_.shrink_to_fit();
  this->tx_src_ = this->buffer_;
  this->tx_len_ = this->buffer_bytes_();
  this->set_state_(State::WAIT_START, TIMEOUT_COMMAND);
  this->send_command_(CMD_DIRECT_WRITE_START, nullptr, 0, false);
}

}  // namespace esphome::opendisplay

#endif  // USE_ESP32
