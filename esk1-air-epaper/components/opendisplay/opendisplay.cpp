#include "opendisplay.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
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

// Preference key for the address found by name.
static const uint32_t PREF_KEY = 0x0D15A1B1;

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
static const uint8_t PACKET_TYPE_DISPLAY_PANEL = 0xB1;
static const uint8_t LENGTH_PREFIXED_MIN_TYPE = 0x80;
static const size_t DISPLAY_PACKET_SIZE = 46;
// display_panel (0xB1) body: <BBBHH4s...: instance, flags, busy idle level,
// busy timeout, busy delay, code_map (py-opendisplay models/panel_drive.py).
static const size_t DISPLAY_PANEL_CODE_MAP_OFFSET = 7;

// BWRY logical colour (Ink) -> 2-bit panel code. Most panels use the palette
// order; EP29YR_128x296 swaps yellow and red (py-opendisplay display_palettes.py).
static const uint16_t PANEL_IC_EP29YR_128X296 = 0x001D;
static const uint8_t BWRY_CODES_DEFAULT[4] = {0, 1, 2, 3};
static const uint8_t BWRY_CODES_SWAPPED[4] = {0, 1, 3, 2};

// Nominal ink colours, indexed by Ink (py-opendisplay palettes/ideal_*.json).
static const uint8_t INK_RGB[4][3] = {
    {0, 0, 0},        // black
    {255, 255, 255},  // white
    {255, 255, 0},    // yellow
    {255, 0, 0},      // red
};

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
static const uint32_t TIMEOUT_DISCOVER = 120000; // listening for the label's name
static const uint32_t TIMEOUT_CONNECT = 120000;  // the label may advertise slowly
static const uint32_t TIMEOUT_COMMAND = 10000;
static const uint32_t TIMEOUT_DATA_ACK = 90000;  // each ACK follows a blocking panel write
static const uint32_t TIMEOUT_END_ACK = 90000;
static const uint32_t TIMEOUT_REFRESH = 90000;

static const char *state_name(uint8_t state) {
  static const char *const NAMES[] = {"idle",    "connecting", "reading config", "starting",
                                      "sending", "ending",     "refreshing",     "discovering"};
  return state < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[state] : "?";
}

static const char *scheme_name(uint8_t scheme) {
  switch (scheme) {
    case COLOR_SCHEME_MONO:
      return "MONO";
    case COLOR_SCHEME_BWR:
      return "BWR";
    case COLOR_SCHEME_BWY:
      return "BWY";
    case COLOR_SCHEME_BWRY:
      return "BWRY";
    case COLOR_SCHEME_BWGBRY:
      return "BWGBRY (Spectra 6)";
    case COLOR_SCHEME_GRAY4:
      return "GRAY4";
    case COLOR_SCHEME_GRAY16:
      return "GRAY16";
    case COLOR_SCHEME_SEVEN_COLOR:
      return "SEVEN_COLOR";
    case COLOR_SCHEME_BWGBRY_SPLIT:
      return "BWGBRY_SPLIT";
    case COLOR_SCHEME_BWRY16:
      return "BWRY16";
    case COLOR_SCHEME_RGB565:
      return "RGB565";
    case COLOR_SCHEME_RGB888:
      return "RGB888";
    case COLOR_SCHEME_RGB16BPC:
      return "RGB16BPC";
    default:
      return "unknown";
  }
}

// Maps every 2-bit field of a byte through a 4-entry table.
static void build_2bpp_lut(const uint8_t codes[4], std::array<uint8_t, 256> &lut) {
  for (unsigned b = 0; b < 256; b++) {
    uint8_t out = 0;
    for (unsigned shift = 0; shift < 8; shift += 2)
      out |= codes[(b >> shift) & 0x03] << shift;
    lut[b] = out;
  }
}

// ======================================================================
// Component
// ======================================================================

void OpenDisplay::setup() {
  // Room for the largest supported frame, reserved once; whatever panel the
  // label reports is drawn into the start of it.
  this->init_internal_(MAX_FRAME_BYTES);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not reserve the %u-byte frame buffer", (unsigned) MAX_FRAME_BYTES);
    this->mark_failed();
    return;
  }
  if (this->panel_known_() && !this->adopt_panel_(this->width_, this->height_, this->color_scheme_)) {
    // display.py rejects these already; this only guards hand-written code.
    this->width_ = this->height_ = 0;
  }
  // Connect only while an upload is running. ble_client's auto-connect does
  // the scan-and-connect (learning the label's address type) once enabled.
  this->parent()->set_enabled(false);

  // A real mac_address in the YAML wins; 00:00:00:00:00:00 means find the
  // label by name, starting from the address last found for that name.
  this->mac_override_ = this->parent()->get_address() != 0;
  this->pref_ = global_preferences->make_preference<SavedLabel>(PREF_KEY, true);
  SavedLabel saved{};
  if (!this->mac_override_ && !this->ble_name_.empty() && this->pref_.load(&saved) &&
      saved.name_hash == fnv1_hash(this->ble_name_) && saved.address != 0) {
    this->parent()->set_address(saved.address);
    this->parent()->set_remote_addr_type(static_cast<esp_ble_addr_type_t>(saved.address_type));
    ESP_LOGD(TAG, "Label '%s' was last found at %s", this->ble_name_.c_str(), this->parent()->address_str());
  }
  this->setup_done_ = true;
}

void OpenDisplay::set_ble_name(const std::string &name) {
  if (name == this->ble_name_)
    return;
  this->ble_name_ = name;
  if (!this->setup_done_)
    return;  // setup() looks up the saved address for this name
  if (this->mac_override_) {
    ESP_LOGI(TAG, "BLE name set to '%s'; the MAC override %s is still used", name.c_str(),
             this->parent()->address_str());
    return;
  }
  ESP_LOGI(TAG, "BLE name set to '%s'", name.c_str());
  if (this->state_ == State::DISCOVERING) {
    this->forget_address_();  // keep listening, for the new name
  } else if (this->state_ != State::IDLE) {
    this->name_change_pending_ = true;  // finish_() applies it
  } else {
    this->forget_address_();
    this->defer([this]() { this->update(); });
  }
}

std::string OpenDisplay::get_label_mac() { return std::string(this->parent()->address_str()); }

bool OpenDisplay::name_matches_(const char *name, size_t len) const {
  const std::string &want = this->ble_name_;
  if (want.empty())
    return false;
  if (want.back() == '*') {
    size_t n = want.size() - 1;
    return len >= n && memcmp(name, want.data(), n) == 0;
  }
  return len == want.size() && memcmp(name, want.data(), len) == 0;
}

// Drops the address found by name, so the next upload looks it up again.
void OpenDisplay::forget_address_() {
  this->parent()->set_enabled(false);
  this->parent()->set_address(0);
  SavedLabel none{};
  this->pref_.save(&none);
  // Whatever label turns up next has not been sent this frame.
  this->has_sent_ = false;
}

bool OpenDisplay::parse_device(const espbt::ESPBTDevice &device) {
  if (this->state_ != State::DISCOVERING)
    return false;
  auto name = device.get_name();
  if (name.empty() || !this->name_matches_(name.c_str(), name.size()))
    return false;
  uint64_t address = device.address_uint64();
  auto address_type = static_cast<esp_ble_addr_type_t>(device.get_address_type());
  this->parent()->set_address(address);
  this->parent()->set_remote_addr_type(address_type);
  SavedLabel saved{fnv1_hash(this->ble_name_), address, static_cast<uint8_t>(address_type)};
  this->pref_.save(&saved);
  ESP_LOGI(TAG, "Found label '%.*s' at %s (RSSI %d)", (int) name.size(), name.c_str(), this->parent()->address_str(),
           device.get_rssi());
  this->connect_();
  return true;
}

void OpenDisplay::connect_() {
  this->set_state_(State::CONNECTING, TIMEOUT_CONNECT);
  if (this->char_handle_ != 0 && this->node_state == espbt::ClientState::ESTABLISHED) {
    this->begin_session_();
  } else {
    this->parent()->set_enabled(true);
  }
}

void OpenDisplay::dump_config() {
  LOG_DISPLAY("", "OpenDisplay", this);
  char panel[48];
  if (this->panel_known_()) {
    snprintf(panel, sizeof(panel), "%ux%u %s (%u bytes)", this->width_, this->height_,
             scheme_name(this->color_scheme_), (unsigned) this->buffer_bytes_());
  } else {
    snprintf(panel, sizeof(panel), "read from the label");
  }
  if (this->mac_override_) {
    ESP_LOGCONFIG(TAG, "  Label         : MAC %s (override; BLE name '%s' not used)", this->parent()->address_str(),
                  this->ble_name_.c_str());
  } else {
    const char *mac = this->parent()->address_str();
    ESP_LOGCONFIG(TAG, "  Label         : BLE name '%s', %s%s", this->ble_name_.c_str(),
                  mac[0] != '\0' ? "last found at " : "not found yet", mac);
  }
  ESP_LOGCONFIG(TAG,
                "  Panel         : %s\n"
                "  Refresh mode  : %s\n"
                "  Skip unchanged: %s\n"
                "  Compression   : %s\n"
                "  Buffer        : %u bytes reserved",
                panel, this->refresh_mode_ == REFRESH_MODE_FAST ? "FAST" : "FULL",
                YESNO(this->skip_unchanged_), YESNO(this->compression_), (unsigned) MAX_FRAME_BYTES);
  LOG_UPDATE_INTERVAL(this);
}

void OpenDisplay::update() {
  if (this->state_ != State::IDLE) {
    // The buffer is being sent; redraw once this transfer ends.
    this->update_pending_ = true;
    return;
  }
  this->drawn_ = false;
  if (this->panel_known_()) {
    // Draw at the panel size learned last time; if the label reports another
    // one on connecting, the frame is drawn again at that size.
    this->draw_frame_();
    if (this->skip_unchanged_ && this->has_sent_ && this->frame_hash_ == this->last_sent_hash_) {
      ESP_LOGD(TAG, "Frame unchanged, not uploading");
      return;
    }
  } else {
    ESP_LOGD(TAG, "Panel size not known yet; reading it from the label before drawing");
  }
  this->upload_started_ms_ = millis();
  if (this->parent()->get_address() == 0) {
    if (this->ble_name_.empty()) {
      ESP_LOGW(TAG, "No label to upload to: set ble_client mac_address, or ble_name");
      this->status_set_warning();
      return;
    }
    ESP_LOGI(TAG, "Searching for a label named '%s'", this->ble_name_.c_str());
    this->set_state_(State::DISCOVERING, TIMEOUT_DISCOVER);
    return;  // parse_device() connects when it hears the name
  }
  ESP_LOGI(TAG, "Uploading frame to %s", this->parent()->address_str());
  this->connect_();
}

void OpenDisplay::loop() {
  // Called both by the application and by the parent BLE client.
  if (this->state_ == State::IDLE || this->state_timeout_ms_ == 0)
    return;
  if (millis() - this->state_started_ms_ > this->state_timeout_ms_) {
    State state = this->state_;
    char reason[96];
    if (state == State::DISCOVERING) {
      snprintf(reason, sizeof(reason), "no label named '%.40s' heard", this->ble_name_.c_str());
    } else {
      snprintf(reason, sizeof(reason), "timed out while %s", state_name(static_cast<uint8_t>(state)));
    }
    this->finish_(false, reason);
    if (state == State::CONNECTING && !this->mac_override_ && this->parent()->get_address() != 0) {
      // The label may have a new address now; look it up by name next time.
      ESP_LOGW(TAG, "No answer at %s; searching by name next time", this->parent()->address_str());
      this->forget_address_();
    }
  }
}

void OpenDisplay::draw_frame_() {
  this->do_update_();
  this->frame_hash_ = this->buffer_hash_();
  this->drawn_ = true;
}

// Takes on the panel the label reports, if its frame fits the reserved buffer.
bool OpenDisplay::adopt_panel_(uint16_t width, uint16_t height, uint8_t scheme) {
  bool same = width == this->width_ && height == this->height_ && scheme == this->color_scheme_;
  if (!scheme_supported_(scheme)) {
    ESP_LOGE(TAG, "The label's colour scheme %s (%u) is not supported; only MONO, BWR, BWY and BWRY are",
             scheme_name(scheme), scheme);
    if (this->state_ != State::IDLE)
      this->finish_(false, "unsupported colour scheme");
    return false;
  }
  size_t need = frame_bytes_(width, height, scheme);
  if (width == 0 || height == 0 || need > MAX_FRAME_BYTES) {
    ESP_LOGE(TAG, "A %ux%u %s frame needs %u bytes; the reserved buffer holds %u (a 400x300 BWRY frame)", width,
             height, scheme_name(scheme), (unsigned) need, (unsigned) MAX_FRAME_BYTES);
    if (this->state_ != State::IDLE)
      this->finish_(false, "panel too large");
    return false;
  }
  if (same)
    return true;
  if (this->state_ != State::IDLE) {
    if (this->panel_known_()) {
      ESP_LOGW(TAG, "Label is %ux%u %s, not %ux%u %s; drawing for the label's panel", width, height,
               scheme_name(scheme), this->width_, this->height_, scheme_name(this->color_scheme_));
    } else {
      ESP_LOGI(TAG, "Label is %ux%u %s (%u of %u buffer bytes)", width, height, scheme_name(scheme),
               (unsigned) need, (unsigned) MAX_FRAME_BYTES);
    }
  }
  this->width_ = width;
  this->height_ = height;
  this->color_scheme_ = static_cast<ColorScheme>(scheme);
  // The buffer must be drawn again at the new size, and the last frame sent
  // no longer says anything about this one.
  this->drawn_ = false;
  this->has_sent_ = false;
  return true;
}

// The panel is settled: draw if the buffer does not hold this frame at this
// size yet, then send it.
void OpenDisplay::config_done_() {
  if (!this->drawn_)
    this->draw_frame_();
  this->start_upload_();
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
  // Put the BWRY buffer back into logical colours for the next draw.
  if (this->bwry_applied_) {
    for (size_t i = 0, n = this->buffer_bytes_(); i < n; i++)
      this->buffer_[i] = this->bwry_unlut_[this->buffer_[i]];
    this->bwry_applied_ = false;
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
  if (this->name_change_pending_) {
    this->name_change_pending_ = false;
    this->forget_address_();
    this->update_pending_ = true;  // send to the label with the new name
  }
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
// The colour schemes take real colours too: each becomes the nearest ink the
// panel has (by RGB distance), so Color(255, 0, 0) is red on BWR and BWRY.
OpenDisplay::Ink OpenDisplay::classify_(Color color) const {
  if (color.raw_32 == 0)
    return Ink::WHITE;
  if (this->color_scheme_ == COLOR_SCHEME_MONO || color.raw_32 == 0xFFFFFFFF)
    return Ink::BLACK;
  uint8_t inks;  // bit n set = Ink n available
  switch (this->color_scheme_) {
    case COLOR_SCHEME_BWR:
      inks = 0b001011;
      break;
    case COLOR_SCHEME_BWY:
      inks = 0b000111;
      break;
    default:  // BWRY
      inks = 0b001111;
      break;
  }
  uint8_t best = 0;
  uint32_t best_dist = UINT32_MAX;
  for (uint8_t i = 0; i < 4; i++) {
    if (!(inks & (1u << i)))
      continue;
    int dr = color.r - INK_RGB[i][0], dg = color.g - INK_RGB[i][1], db = color.b - INK_RGB[i][2];
    uint32_t dist = dr * dr + dg * dg + db * db;
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return static_cast<Ink>(best);
}

// Buffer layouts, rows padded to a whole byte, leftmost pixel in the high bits
// (py-opendisplay encoding/bitplanes.py and images.py):
//   MONO      one 1bpp plane, bit set = white.
//   BWR / BWY 1bpp BW plane (bit set = white) then 1bpp accent plane (bit set
//             = red / yellow). Red also sets the BW bit; yellow does not.
//   BWRY      2bpp, the logical colour (Ink 0..3). It is mapped to the label's
//             panel codes just before upload (see start_upload_()).
void OpenDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_ || this->buffer_ == nullptr)
    return;
  Ink ink = this->classify_(color);
  uint8_t *row = this->buffer_ + y * this->row_bytes_();

  if (this->color_scheme_ == COLOR_SCHEME_BWRY) {
    uint8_t shift = 6 - 2 * (x & 3);
    uint8_t &b = row[x >> 2];
    b = (b & ~(0x03 << shift)) | (static_cast<uint8_t>(ink) << shift);
    return;
  }

  size_t index = x >> 3;
  uint8_t mask = 0x80 >> (x & 7);
  bool accent = ink == Ink::RED || ink == Ink::YELLOW;
  bool bw = ink == Ink::WHITE || ink == Ink::RED;
  if (bw) {
    row[index] |= mask;
  } else {
    row[index] &= ~mask;
  }
  if (this->has_accent_plane_()) {
    uint8_t *accent_row = row + this->plane_bytes_();
    if (accent) {
      accent_row[index] |= mask;
    } else {
      accent_row[index] &= ~mask;
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
  if (this->color_scheme_ == COLOR_SCHEME_BWRY) {
    memset(this->buffer_, static_cast<uint8_t>(ink) * 0x55, this->buffer_bytes_());
    return;
  }
  bool bw = ink == Ink::WHITE || ink == Ink::RED;
  memset(this->buffer_, bw ? 0xFF : 0x00, this->plane_bytes_());
  if (this->has_accent_plane_()) {
    bool accent = ink == Ink::RED || ink == Ink::YELLOW;
    memset(this->buffer_ + this->plane_bytes_(), accent ? 0xFF : 0x00, this->plane_bytes_());
  }
}

// BWRY: picks the label's colour -> panel code table. A 0xB1 code_map wins
// (firmware 3.0 stores it but leaves applying it to the host); otherwise the
// built-in table for the panel IC.
bool OpenDisplay::set_bwry_codes_(uint16_t panel_ic_type, const uint8_t *code_map) {
  this->bwry_remap_ = false;
  if (this->color_scheme_ != COLOR_SCHEME_BWRY)
    return true;
  const uint8_t *codes = panel_ic_type == PANEL_IC_EP29YR_128X296 ? BWRY_CODES_SWAPPED : BWRY_CODES_DEFAULT;
  if (code_map != nullptr) {
    uint8_t seen = 0;
    for (int i = 0; i < 4; i++)
      seen |= code_map[i] < 4 ? (1u << code_map[i]) : 0x10;
    if (seen != 0x0F) {
      ESP_LOGE(TAG, "The label's code_map %u,%u,%u,%u is not a permutation of 0..3", code_map[0], code_map[1],
               code_map[2], code_map[3]);
      this->finish_(false, "invalid BWRY code_map");
      return false;
    }
    codes = code_map;
  }
  ESP_LOGD(TAG, "BWRY codes (black, white, yellow, red): %u, %u, %u, %u", codes[0], codes[1], codes[2], codes[3]);
  if (memcmp(codes, BWRY_CODES_DEFAULT, 4) == 0)
    return true;
  uint8_t inverse[4];
  for (uint8_t i = 0; i < 4; i++)
    inverse[codes[i]] = i;
  build_2bpp_lut(codes, this->bwry_lut_);
  build_2bpp_lut(inverse, this->bwry_unlut_);
  this->bwry_remap_ = true;
  return true;
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
  this->bwry_remap_ = false;
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
    if (!this->panel_known_()) {
      ESP_LOGE(TAG, "Label has no stored config; set width, height and color_scheme in the YAML");
      this->finish_(false, "panel size unknown");
      return;
    }
    ESP_LOGW(TAG, "Label has no stored config; using the YAML's %ux%u %s", this->width_, this->height_,
             scheme_name(this->color_scheme_));
    this->config_done_();
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
  this->config_done_();
}

// Blob: [length:2 LE][version:1][packets...][crc:2]. Packets are
// [number:1][type:1][fixed body] (types < 0x80) or
// [number:1][type:1][len:2 LE][body] (types >= 0x80).
bool OpenDisplay::check_config_() {
  const auto &cfg = this->config_;
  if (cfg.size() < 5) {
    if (!this->panel_known_()) {
      ESP_LOGE(TAG, "Config too short (%u bytes); set width, height and color_scheme in the YAML",
               (unsigned) cfg.size());
      this->finish_(false, "panel size unknown");
      return false;
    }
    ESP_LOGW(TAG, "Config too short (%u bytes); using the YAML's panel", (unsigned) cfg.size());
    return true;
  }
  // The first display packet and the code_map of display 0 are used.
  const uint8_t *display = nullptr;
  const uint8_t *code_map = nullptr;
  size_t off = 3, end = cfg.size() - 2;
  while (off + 2 <= end) {
    uint8_t type = cfg[off + 1];
    if (type >= LENGTH_PREFIXED_MIN_TYPE) {
      if (off + 4 > end)
        break;
      size_t len = cfg[off + 2] | (cfg[off + 3] << 8);
      const uint8_t *body = cfg.data() + off + 4;
      if (off + 4 + len > end)
        break;
      if (type == PACKET_TYPE_DISPLAY_PANEL && code_map == nullptr && len >= DISPLAY_PANEL_CODE_MAP_OFFSET + 4 &&
          body[0] == 0)
        code_map = body + DISPLAY_PANEL_CODE_MAP_OFFSET;
      off += 4 + len;
      continue;
    }
    int size = legacy_packet_size(type);
    if (size < 0 || off + 2 + size > end)
      break;
    if (type == PACKET_TYPE_DISPLAY && display == nullptr)
      display = cfg.data() + off + 2;
    off += 2 + size;
  }

  if (display == nullptr) {
    if (!this->panel_known_()) {
      ESP_LOGE(TAG, "No display packet in the label's config; set width, height and color_scheme in the YAML");
      this->finish_(false, "panel size unknown");
      return false;
    }
    ESP_LOGW(TAG, "No display packet in the label's config; using the YAML's panel");
    return this->set_bwry_codes_(0, code_map);
  }
  const uint8_t *d = display;
  uint16_t panel_ic_type = d[2] | (d[3] << 8);
  uint16_t width = d[4] | (d[5] << 8);
  uint16_t height = d[6] | (d[7] << 8);
  uint8_t rotation = d[14];
  uint8_t scheme = d[21];
  this->transmission_modes_ = d[22];
  ESP_LOGD(TAG, "Label reports panel IC %u, %ux%u, colour scheme %s (%u), rotation %u, transmission modes 0x%02X",
           panel_ic_type, width, height, scheme_name(scheme), scheme, rotation, d[22]);
  if (!this->adopt_panel_(width, height, scheme))
    return false;  // finish_() already called
  return this->set_bwry_codes_(panel_ic_type, code_map);
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
  // BWRY: turn logical colours into this label's panel codes for the
  // transfer; finish_() turns them back.
  if (this->bwry_remap_ && !this->bwry_applied_) {
    for (size_t i = 0; i < raw_len; i++)
      this->buffer_[i] = this->bwry_lut_[this->buffer_[i]];
    this->bwry_applied_ = true;
  }
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
