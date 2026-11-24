#include "opendisplay.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::opendisplay {

static const char *const TAG = "opendisplay";

static const uint16_t OD_SERVICE_UUID = 0x2446;
static const uint16_t OD_CHAR_UUID = 0x2446;

// Preference key for the address found by name.
static const uint32_t PREF_KEY = 0x0D15A1B1;

static const uint32_t TIMEOUT_DISCOVER = 120000;  // listening for the label's name
static const uint32_t TIMEOUT_CONNECT = 120000;   // the label may advertise slowly

// BWRY logical colour (Ink) -> 2-bit panel code. Most panels use the palette
// order; EP29YR_128x296 swaps yellow and red (py-opendisplay display_palettes.py).
static const uint16_t PANEL_IC_EP29YR_128X296 = 0x001D;
static const uint8_t BWRY_CODES_DEFAULT[4] = {0, 1, 2, 3};
static const uint8_t BWRY_CODES_SWAPPED[4] = {0, 1, 3, 2};

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

static const char *state_name(uint8_t state) {
  static const char *const NAMES[] = {"idle", "discovering", "connecting", "in session"};
  return state < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[state] : "?";
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
  if (this->panel_known_()) {
    const char *reason = nullptr;
    uint16_t w = this->width_, h = this->height_;
    this->width_ = this->height_ = 0;  // adopt_panel_ compares against the current panel
    if (!this->adopt_panel_(w, h, this->color_scheme_, &reason))
      ESP_LOGE(TAG, "YAML panel ignored: %s", reason);  // display.py rejects these already
  }
  this->session_.set_options({this->compression_, static_cast<uint8_t>(this->refresh_mode_)});
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

void OpenDisplay::dump_config() {
  LOG_DISPLAY("", "OpenDisplay", this);
  if (this->mac_override_) {
    ESP_LOGCONFIG(TAG, "  Label         : MAC %s (override; BLE name '%s' not used)", this->parent()->address_str(),
                  this->ble_name_.c_str());
  } else {
    const char *mac = this->parent()->address_str();
    ESP_LOGCONFIG(TAG, "  Label         : BLE name '%s', %s%s", this->ble_name_.c_str(),
                  mac[0] != '\0' ? "last found at " : "not found yet", mac);
  }
  char panel[48];
  if (this->panel_known_()) {
    snprintf(panel, sizeof(panel), "%ux%u %s (%u bytes)", this->width_, this->height_,
             scheme_name(this->color_scheme_), (unsigned) this->buffer_bytes_());
  } else {
    snprintf(panel, sizeof(panel), "read from the label");
  }
  ESP_LOGCONFIG(TAG,
                "  Panel         : %s\n"
                "  Refresh mode  : %s\n"
                "  Skip unchanged: %s\n"
                "  Compression   : %s\n"
                "  Buffer        : %u bytes reserved\n"
                "  Scheduling    : %s",
                panel, this->refresh_mode_ == REFRESH_MODE_FAST ? "FAST" : "FULL", YESNO(this->skip_unchanged_),
                YESNO(this->compression_), (unsigned) MAX_FRAME_BYTES,
                this->external_update_ ? "owned by another component" : "update_interval");
  if (this->required_width_ != 0) {
    ESP_LOGCONFIG(TAG, "  Required panel: %ux%u %s", this->required_width_, this->required_height_,
                  scheme_name(this->required_scheme_));
  }
  LOG_UPDATE_INTERVAL(this);
}

// ---- Finding the label ----

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
  this->has_sent_ = false;  // whatever label turns up next has not had this frame
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

// ---- Transfers ----

void OpenDisplay::update() {
  if (this->external_update_) {
    // The owner (set_external_scheduling) decides when to upload; this is a
    // refresh request for it.
    this->external_update_();
    return;
  }
  if (this->state_ != State::IDLE) {
    this->update_pending_ = true;  // redraw once this transfer ends
    return;
  }
  this->start_upload_(false);
}

UploadStart OpenDisplay::request_upload(bool force) {
  if (this->state_ != State::IDLE)
    return UploadStart::BUSY;
  return this->start_upload_(force);
}

UploadStart OpenDisplay::start_upload_(bool force) {
  if (this->buffer_ == nullptr)
    return UploadStart::FAILED;
  ESP_LOGD(TAG, "Draw requested%s", force ? " (forced)" : "");
  this->drawn_ = false;
  if (this->panel_known_()) {
    // Draw at the panel size learned last time; if the label reports another
    // one on connecting, the frame is drawn again at that size.
    this->draw_frame_();
    if (!force && this->skip_unchanged_ && this->has_sent_ && this->frame_hash_ == this->last_sent_hash_) {
      ESP_LOGD(TAG, "Frame unchanged, not uploading");
      return UploadStart::UNCHANGED;
    }
  } else {
    ESP_LOGD(TAG, "Panel size not known yet; reading it from the label before drawing");
  }
  this->upload_started_ms_ = millis();
  if (this->parent()->get_address() == 0) {
    if (this->ble_name_.empty()) {
      ESP_LOGW(TAG, "No label to upload to: set ble_client mac_address, or ble_name");
      this->status_set_warning();
      return UploadStart::NO_LABEL;
    }
    ESP_LOGI(TAG, "Upload started: searching for a label named '%s'", this->ble_name_.c_str());
    this->set_state_(State::DISCOVERING, TIMEOUT_DISCOVER);
    return UploadStart::STARTED;  // parse_device() connects when it hears the name
  }
  ESP_LOGI(TAG, "Upload started: %s", this->parent()->address_str());
  this->connect_();
  return UploadStart::STARTED;
}

void OpenDisplay::loop() {
  // Called both by the application and by the parent BLE client.
  if (this->state_ == State::SESSION) {
    this->session_.tick(millis());
    return;
  }
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
    this->finish_(od::Result::FAILED_NOT_SENT, reason);
    if (state == State::CONNECTING && !this->mac_override_ && this->parent()->get_address() != 0) {
      // The label may have a new address now; look it up by name next time.
      ESP_LOGW(TAG, "No answer at %s; searching by name next time", this->parent()->address_str());
      this->forget_address_();
    }
  }
}

void OpenDisplay::set_state_(State state, uint32_t timeout_ms) {
  this->state_ = state;
  this->state_started_ms_ = millis();
  this->state_timeout_ms_ = timeout_ms;
}

void OpenDisplay::connect_() {
  this->set_state_(State::CONNECTING, TIMEOUT_CONNECT);
  if (this->char_handle_ != 0 && this->node_state == espbt::ClientState::ESTABLISHED) {
    this->begin_session_();
  } else {
    this->parent()->set_enabled(true);
  }
}

void OpenDisplay::begin_session_() {
  this->set_state_(State::SESSION, 0);
  this->bwry_remap_ = false;
  this->session_.set_options({this->compression_, static_cast<uint8_t>(this->refresh_mode_)});
  this->session_.begin(millis());
}

void OpenDisplay::draw_frame_() {
  this->foreign_pixels_ = 0;
  this->do_update_();
  this->report_foreign_colours_();
  this->frame_hash_ = this->buffer_hash_();
  this->drawn_ = true;
}

void OpenDisplay::finish_(od::Result result, const char *reason) {
  uint32_t elapsed = millis() - this->upload_started_ms_;
  bool success = result == od::Result::SUCCESS;
  // Only a session can have reached a point where the panel might refresh.
  bool from_session = this->state_ == State::SESSION;
  if (success) {
    this->last_sent_hash_ = this->frame_hash_;
    this->has_sent_ = true;
    this->status_clear_warning();
    ESP_LOGI(TAG, "Refresh confirmed (%u bytes %s in %.1f s)", (unsigned) this->buffer_bytes_(),
             this->session_.compressed() ? "compressed" : "uncompressed", elapsed / 1000.0f);
  } else {
    this->status_set_warning();
    ESP_LOGW(TAG, "Upload failed: %s (%s)", reason != nullptr ? reason : "?", od::result_name(result));
  }
  this->restore_bwry_codes_();
  this->state_ = State::IDLE;
  this->state_timeout_ms_ = 0;
  // Disconnect so the label can go back to sleep.
  this->parent()->set_enabled(false);

  uint32_t marker = 0;
  bool has_marker = from_session && this->session_.refresh_marker(&marker);
  for (auto &cb : this->complete_callbacks_)
    cb(result, has_marker, marker);

  if (this->name_change_pending_) {
    this->name_change_pending_ = false;
    this->forget_address_();
    if (!this->external_update_)
      this->update_pending_ = true;  // send to the label with the new name
  }
  if (this->update_pending_ && !this->external_update_) {
    this->update_pending_ = false;
    this->defer([this]() { this->update(); });
  }
}

// ======================================================================
// Panel and BWRY
// ======================================================================

bool OpenDisplay::adopt_panel_(uint16_t width, uint16_t height, uint8_t scheme, const char **reason) {
  if (!scheme_supported_(scheme)) {
    ESP_LOGE(TAG, "The label's colour scheme %s (%u) is not supported; only MONO, BWR, BWY and BWRY are",
             scheme_name(scheme), scheme);
    *reason = "unsupported colour scheme";
    return false;
  }
  size_t need = frame_bytes_(width, height, scheme);
  if (width == 0 || height == 0 || need > MAX_FRAME_BYTES) {
    ESP_LOGE(TAG, "A %ux%u %s frame needs %u bytes; the reserved buffer holds %u (a 400x300 BWRY frame)", width,
             height, scheme_name(scheme), (unsigned) need, (unsigned) MAX_FRAME_BYTES);
    *reason = "panel too large";
    return false;
  }
  if (width == this->width_ && height == this->height_ && scheme == this->color_scheme_)
    return true;
  if (this->state_ != State::IDLE) {
    if (this->panel_known_()) {
      ESP_LOGW(TAG, "Label is %ux%u %s, not %ux%u %s; drawing for the label's panel", width, height,
               scheme_name(scheme), this->width_, this->height_, scheme_name(this->color_scheme_));
    } else {
      ESP_LOGI(TAG, "Label is %ux%u %s (%u of %u buffer bytes)", width, height, scheme_name(scheme), (unsigned) need,
               (unsigned) MAX_FRAME_BYTES);
    }
  }
  this->width_ = width;
  this->height_ = height;
  this->color_scheme_ = static_cast<ColorScheme>(scheme);
  // Draw again at the new size; the last frame sent says nothing about this one.
  this->drawn_ = false;
  this->has_sent_ = false;
  return true;
}

bool OpenDisplay::required_panel_ok_(const char **reason) const {
  if (this->required_width_ == 0)
    return true;
  if (this->width_ == this->required_width_ && this->height_ == this->required_height_ &&
      this->color_scheme_ == this->required_scheme_)
    return true;
  ESP_LOGE(TAG, "This display needs a %ux%u %s label; this one is %ux%u %s", this->required_width_,
           this->required_height_, scheme_name(this->required_scheme_), this->width_, this->height_,
           scheme_name(this->color_scheme_));
  *reason = "panel does not match the required one";
  return false;
}

// BWRY: picks the label's colour -> panel code table. A 0xB1 code_map wins
// (firmware stores it but leaves applying it to the host); otherwise the
// built-in table for the panel IC.
bool OpenDisplay::set_bwry_codes_(uint16_t panel_ic_type, const uint8_t *code_map, const char **reason) {
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
      *reason = "invalid BWRY code_map";
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

void OpenDisplay::apply_bwry_codes_() {
  if (!this->bwry_remap_ || this->bwry_applied_)
    return;
  for (size_t i = 0, n = this->buffer_bytes_(); i < n; i++)
    this->buffer_[i] = this->bwry_lut_[this->buffer_[i]];
  this->bwry_applied_ = true;
}

void OpenDisplay::restore_bwry_codes_() {
  if (!this->bwry_applied_)
    return;
  for (size_t i = 0, n = this->buffer_bytes_(); i < n; i++)
    this->buffer_[i] = this->bwry_unlut_[this->buffer_[i]];
  this->bwry_applied_ = false;
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
// od::SessionHost
// ======================================================================

bool OpenDisplay::od_write(const uint8_t *data, size_t len, bool data_chunk) {
  // DATA chunks are acked by notification, so they skip the ATT response when
  // the characteristic allows it (as py-opendisplay does).
  esp_gatt_write_type_t type = ESP_GATT_WRITE_TYPE_RSP;
  if ((data_chunk && this->char_write_no_rsp_) || !this->char_write_rsp_)
    type = ESP_GATT_WRITE_TYPE_NO_RSP;
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->char_handle_, len, const_cast<uint8_t *>(data), type,
                                      ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write_char failed (%d)", err);
    return false;
  }
  return true;
}

bool OpenDisplay::od_config_ready(const od::ConfigResult &cfg, const char **reason) {
  if (cfg.status == od::ConfigStatus::OK) {
    const od::DisplayInfo &d = cfg.display;
    ESP_LOGD(TAG,
             "Label reports panel IC %u, %ux%u, colour scheme %s (%u), rotation %u, transmission modes 0x%02X%s",
             d.panel_ic_type, d.width, d.height, scheme_name(d.color_scheme), d.color_scheme, d.rotation,
             d.transmission_modes, cfg.has_drive_packets ? "" : ", no drive packets");
    if (!this->adopt_panel_(d.width, d.height, d.color_scheme, reason))
      return false;
    if (!this->required_panel_ok_(reason))
      return false;
    if (!this->set_bwry_codes_(d.panel_ic_type, cfg.has_code_map ? cfg.code_map : nullptr, reason))
      return false;
  } else {
    const char *why = cfg.status == od::ConfigStatus::NO_CONFIG    ? "has no stored config"
                      : cfg.status == od::ConfigStatus::TOO_SHORT ? "sent a config too short to read"
                                                                   : "has no display packet in its config";
    if (!this->panel_known_()) {
      ESP_LOGE(TAG, "Label %s; set width, height and color_scheme in the YAML", why);
      *reason = "panel size unknown";
      return false;
    }
    ESP_LOGW(TAG, "Label %s; using the YAML's %ux%u %s", why, this->width_, this->height_,
             scheme_name(this->color_scheme_));
    if (!this->required_panel_ok_(reason))
      return false;
    if (!this->set_bwry_codes_(0, cfg.has_code_map ? cfg.code_map : nullptr, reason))
      return false;
  }
  // The panel is settled: draw if the buffer does not hold this frame at this
  // size yet, then turn logical BWRY colours into this label's panel codes
  // for the transfer (finish_() turns them back).
  if (!this->drawn_)
    this->draw_frame_();
  this->apply_bwry_codes_();
  return true;
}

void OpenDisplay::od_frame(const uint8_t **data, size_t *len) {
  *data = this->buffer_;
  *len = this->buffer_bytes_();
}

void OpenDisplay::od_finished(od::Result result, const char *reason) {
  if (this->state_ == State::SESSION)
    this->finish_(result, reason);
}

void OpenDisplay::od_log(od::LogLevel level, const char *message) {
  switch (level) {
    case od::LogLevel::ERROR:
      ESP_LOGE(TAG, "%s", message);
      break;
    case od::LogLevel::WARN:
      ESP_LOGW(TAG, "%s", message);
      break;
    case od::LogLevel::INFO:
      ESP_LOGI(TAG, "%s", message);
      break;
    case od::LogLevel::DEBUG:
      ESP_LOGD(TAG, "%s", message);
      break;
    case od::LogLevel::VERBOSE:
      ESP_LOGV(TAG, "%s", message);
      break;
  }
}

// ======================================================================
// Drawing
// ======================================================================

// Exact colours only (INK_* in opendisplay.h); no nearest-colour matching.
// COLOR_OFF (all zero) is paper and COLOR_ON (all 0xFF) is black, as on the
// other ESPHome e-paper drivers. The white channel is ignored otherwise, so
// Color(255, 0, 0) and Color(255, 0, 0, 0) are both red.
Ink OpenDisplay::ink_of_(Color color) {
  if (color.raw_32 == 0)
    return Ink::WHITE;
  if (color.raw_32 == 0xFFFFFFFF)
    return Ink::BLACK;
  if (color.r == 255 && color.g == 255 && color.b == 255)
    return Ink::WHITE;
  if (color.r == 0 && color.g == 0 && color.b == 0)
    return Ink::BLACK;
  if (color.r == 255 && color.g == 0 && color.b == 0)
    return Ink::RED;
  if (color.r == 255 && color.g == 255 && color.b == 0)
    return Ink::YELLOW;
  if (this->foreign_pixels_++ == 0)
    this->foreign_colour_ = color;
  return Ink::BLACK;
}

Ink OpenDisplay::panel_ink_(Ink ink) const {
  switch (this->color_scheme_) {
    case COLOR_SCHEME_MONO:
      return (ink == Ink::RED || ink == Ink::YELLOW) ? Ink::BLACK : ink;
    case COLOR_SCHEME_BWR:
      return ink == Ink::YELLOW ? Ink::RED : ink;
    case COLOR_SCHEME_BWY:
      return ink == Ink::RED ? Ink::YELLOW : ink;
    default:
      return ink;  // BWRY has all four
  }
}

void OpenDisplay::report_foreign_colours_() {
  if (this->foreign_pixels_ == 0)
    return;
  const Color &c = this->foreign_colour_;
  ESP_LOGW(TAG,
           "%u pixel(s) drawn in colours that are not inks, e.g. Color(%u, %u, %u, %u); drawn black. Use "
           "INK_WHITE / COLOR_OFF, INK_BLACK / COLOR_ON, INK_RED or INK_YELLOW",
           (unsigned) this->foreign_pixels_, c.r, c.g, c.b, c.w);
  this->foreign_pixels_ = 0;
}

void OpenDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  this->write_ink_(x, y, this->panel_ink_(this->ink_of_(color)));
}

// Buffer layouts, rows padded to a whole byte, leftmost pixel in the high bits
// (py-opendisplay encoding/bitplanes.py and images.py):
//   MONO      one 1bpp plane, bit set = white.
//   BWR / BWY 1bpp BW plane (bit set = white) then 1bpp accent plane (bit set
//             = red / yellow). Red also sets the BW bit; yellow does not.
//   BWRY      2bpp, the logical colour (Ink 0..3), mapped to the label's
//             panel codes just before upload (apply_bwry_codes_()).
void OpenDisplay::write_ink_(int x, int y, Ink ink) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_ || this->buffer_ == nullptr)
    return;
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
  Ink ink = this->panel_ink_(this->ink_of_(color));
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
        if (this->state_ == State::CONNECTING)
          this->finish_(od::Result::FAILED_NOT_SENT, "not an OpenDisplay device");
        break;
      }
      this->char_handle_ = chr->handle;
      this->char_write_rsp_ = chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE;
      this->char_write_no_rsp_ = chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
      // ble_client writes the CCC descriptor on REG_FOR_NOTIFY; the firmware
      // drops every response until notifications are on.
      auto err = this->parent()->register_for_notify(chr->handle);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "register_for_notify failed: %d", err);
        if (this->state_ == State::CONNECTING)
          this->finish_(od::Result::FAILED_NOT_SENT, "could not enable notifications");
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->char_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Notify registration failed: %d", param->reg_for_notify.status);
        if (this->state_ == State::CONNECTING)
          this->finish_(od::Result::FAILED_NOT_SENT, "could not enable notifications");
        break;
      }
      // The handle is kept, so the parent may free its GATT cache now.
      this->node_state = espbt::ClientState::ESTABLISHED;
      ESP_LOGD(TAG, "Connected, MTU %u", this->mtu_);
      if (this->state_ == State::CONNECTING) {
        this->begin_session_();
      } else {
        this->parent()->set_enabled(false);  // connected without an upload pending
      }
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->char_handle_ && param->notify.value_len > 0 &&
          this->state_ == State::SESSION)
        this->session_.on_notify(param->notify.value, param->notify.value_len, millis());
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle == this->char_handle_ && param->write.status != ESP_GATT_OK &&
          this->state_ == State::SESSION)
        this->session_.on_write_error(param->write.status, millis());
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      this->char_handle_ = 0;
      this->mtu_ = 23;
      if (this->state_ == State::SESSION) {
        this->session_.on_disconnect(millis());
      } else if (this->state_ == State::CONNECTING) {
        this->finish_(od::Result::FAILED_NOT_SENT, "disconnected");
      }
      break;

    default:
      break;
  }
}

}  // namespace esphome::opendisplay

#endif  // USE_ESP32
