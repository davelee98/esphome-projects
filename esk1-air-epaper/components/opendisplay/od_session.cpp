#include "od_session.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "zlib_deflate.h"

namespace esphome::opendisplay::od {

// ---- Wire constants (opendisplay_protocol.h; Firmware_Unified od_xfer_direct.c) ----
static const uint16_t CMD_CONFIG_READ = 0x0040;
static const uint16_t CMD_DIRECT_WRITE_START = 0x0070;
static const uint16_t CMD_DIRECT_WRITE_DATA = 0x0071;
static const uint16_t CMD_DIRECT_WRITE_END = 0x0072;

static const uint8_t RESP_ACK = 0x00;
static const uint8_t RESP_NACK = 0xFF;
static const uint8_t RESP_AUTH_REQUIRED = 0xFE;
static const uint8_t RESP_CONFIG_READ = 0x40;
static const uint8_t RESP_START = 0x70;
static const uint8_t RESP_DATA = 0x71;
static const uint8_t RESP_END = 0x72;
static const uint8_t RESP_REFRESH_SUCCESS = 0x73;
static const uint8_t RESP_REFRESH_TIMEOUT = 0x74;

// START NACK error codes ([FF][70][err][00]) that a raw retry cannot fix.
static const uint8_t ERR_NEEDS_MIGRATION = 0x08;  // firmware 3.x: config lacks drive packets
static const uint8_t ERR_PANEL_BUSY = 0x09;

static const size_t ATT_WRITE_OVERHEAD = 3;  // ATT opcode + handle
static const size_t CMD_HEADER = 2;          // big-endian opcode
static const size_t COMPRESSED_START_HEADER = CMD_HEADER + 4;  // + uncompressed size, LE

const char *result_name(Result r) {
  switch (r) {
    case Result::SUCCESS:
      return "success";
    case Result::FAILED_NOT_SENT:
      return "failed, nothing sent that could refresh";
    case Result::FAILED_UNCERTAIN:
      return "failed, the panel may have refreshed";
  }
  return "?";
}

const char *Session::state_name() const {
  switch (this->state_) {
    case State::IDLE:
      return "idle";
    case State::READ_CONFIG:
      return "reading config";
    case State::WAIT_START:
      return "starting";
    case State::SENDING:
      return "sending";
    case State::WAIT_END:
      return "ending";
    case State::WAIT_REFRESH:
      return "refreshing";
  }
  return "?";
}

void Session::log_(LogLevel level, const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  this->host_->od_log(level, buf);
}

void Session::set_state_(State s, uint32_t timeout_ms, uint32_t now_ms) {
  this->state_ = s;
  this->state_started_ms_ = now_ms;
  this->state_timeout_ms_ = timeout_ms;
}

void Session::mark_(uint32_t now_ms) {
  if (!this->has_marker_) {
    this->has_marker_ = true;
    this->marker_ms_ = now_ms;
  }
}

void Session::finish_(bool success, const char *reason) {
  if (this->state_ == State::IDLE)
    return;
  this->state_ = State::IDLE;
  this->state_timeout_ms_ = 0;
  this->config_.clear();
  this->config_.shrink_to_fit();
  this->zbuf_.clear();
  this->zbuf_.shrink_to_fit();
  this->tx_.clear();
  this->tx_.shrink_to_fit();
  this->tx_src_ = nullptr;
  this->frame_ = nullptr;
  Result result = success ? Result::SUCCESS : (this->has_marker_ ? Result::FAILED_UNCERTAIN : Result::FAILED_NOT_SENT);
  this->host_->od_finished(result, success ? nullptr : reason);
}

void Session::abort(const char *reason) { this->finish_(false, reason); }

bool Session::send_(uint16_t opcode, const uint8_t *payload, size_t len, bool data_chunk) {
  this->tx_.resize(CMD_HEADER + len);
  this->tx_[0] = opcode >> 8;
  this->tx_[1] = opcode & 0xFF;
  if (len > 0)
    memcpy(this->tx_.data() + CMD_HEADER, payload, len);
  if (!this->host_->od_write(this->tx_.data(), this->tx_.size(), data_chunk)) {
    char reason[48];
    snprintf(reason, sizeof(reason), "write of command 0x%04X failed", opcode);
    this->finish_(false, reason);
    return false;
  }
  return true;
}

void Session::begin(uint32_t now_ms) {
  this->now_ms_ = now_ms;
  if (this->state_ != State::IDLE)
    this->finish_(false, "restarted");
  this->config_.clear();
  this->config_total_ = 0;
  this->transmission_modes_ = -1;
  this->compressed_ = false;
  this->has_marker_ = false;
  this->sent_ = 0;
  this->tx_len_ = 0;
  this->frame_len_ = 0;
  this->set_state_(State::READ_CONFIG, TIMEOUT_COMMAND, now_ms);
  this->send_(CMD_CONFIG_READ, nullptr, 0, false);
}

void Session::tick(uint32_t now_ms) {
  this->now_ms_ = now_ms;
  if (this->state_ == State::IDLE || this->state_timeout_ms_ == 0)
    return;
  if (static_cast<uint32_t>(now_ms - this->state_started_ms_) > this->state_timeout_ms_) {
    char reason[48];
    snprintf(reason, sizeof(reason), "timed out while %s", this->state_name());
    this->finish_(false, reason);
  }
}

void Session::on_write_error(int status, uint32_t now_ms) {
  this->now_ms_ = now_ms;
  char reason[40];
  snprintf(reason, sizeof(reason), "GATT write error %d", status);
  this->finish_(false, reason);
}

void Session::on_disconnect(uint32_t now_ms) {
  this->now_ms_ = now_ms;
  this->finish_(false, "disconnected");
}

void Session::on_notify(const uint8_t *data, size_t len, uint32_t now_ms) {
  this->now_ms_ = now_ms;
  if (len < 2) {
    this->log_(LogLevel::VERBOSE, "Short frame ignored");
    return;
  }
  uint8_t status = data[0], echo = data[1];
  this->log_(LogLevel::VERBOSE, "Frame %02X %02X (%u bytes) while %s", status, echo, (unsigned) len,
             this->state_name());

  if (status == RESP_AUTH_REQUIRED) {
    this->finish_(false, "the label has encryption enabled, which this component does not support");
    return;
  }

  switch (this->state_) {
    case State::READ_CONFIG:
      if (echo == RESP_CONFIG_READ)
        this->on_config_frame_(data, len, now_ms);
      break;

    case State::WAIT_START:
      // [FF][70] (or [FF][FF] on old firmware), optionally [err][00].
      if (status == RESP_NACK && (echo == RESP_START || echo == 0xFF)) {
        uint8_t err = len >= 3 ? data[2] : 0;
        if (err == ERR_NEEDS_MIGRATION) {
          this->finish_(false, "START refused: the label's config needs migrating (no drive packets)");
          return;
        }
        if (err == ERR_PANEL_BUSY) {
          this->finish_(false, "START refused: panel busy");
          return;
        }
        if (this->compressed_) {
          this->log_(LogLevel::WARN, "Label refused the compressed upload (error 0x%02X); sending uncompressed", err);
          this->send_uncompressed_start_(now_ms);
          return;
        }
        char reason[40];
        snprintf(reason, sizeof(reason), "START refused (error 0x%02X)", err);
        this->finish_(false, reason);
        return;
      }
      if (echo != RESP_START)
        break;
      if (status != RESP_ACK) {
        this->finish_(false, "START refused");
        return;
      }
      this->sent_ = 0;
      if (this->tx_len_ > 0) {
        this->send_next_chunk_(now_ms);
      } else {
        this->send_end_(now_ms);  // the whole zlib stream fit in START
      }
      break;

    case State::SENDING:
      if (echo == RESP_END && status == RESP_ACK) {
        // The firmware filled its buffer and ended the transfer itself
        // (ESP32 builds, uncompressed): no END may follow.
        this->mark_(now_ms);
        this->log_(LogLevel::DEBUG, "Label auto-completed the transfer");
        this->set_state_(State::WAIT_REFRESH, TIMEOUT_REFRESH, now_ms);
        break;
      }
      if (echo != RESP_DATA)
        break;
      if (status != RESP_ACK) {
        char reason[48];
        snprintf(reason, sizeof(reason), "DATA refused at byte %u", (unsigned) this->sent_);
        this->finish_(false, reason);
        return;
      }
      this->sent_ += this->chunk_;
      if (this->sent_ < this->tx_len_) {
        this->send_next_chunk_(now_ms);
      } else {
        this->send_end_(now_ms);
      }
      break;

    case State::WAIT_END:
      if (echo != RESP_END)
        break;
      if (status != RESP_ACK) {
        this->finish_(false, "END refused (image incomplete)");
        return;
      }
      this->set_state_(State::WAIT_REFRESH, TIMEOUT_REFRESH, now_ms);
      break;

    case State::WAIT_REFRESH:
      if (echo == RESP_REFRESH_SUCCESS) {
        this->finish_(true, nullptr);
      } else if (echo == RESP_REFRESH_TIMEOUT) {
        this->finish_(false, "the label reported a panel refresh timeout");
      }
      break;

    case State::IDLE:
      break;
  }
}

// CONFIG_READ answers in chunks of at most 100 bytes:
//   chunk 0 : [00][40][chunk:2 LE][total:2 LE][data...]
//   chunk n : [00][40][chunk:2 LE][data...]
//   no config: [FF][40][00][00]
void Session::on_config_frame_(const uint8_t *data, size_t len, uint32_t now_ms) {
  if (data[0] == RESP_NACK) {
    ConfigResult none;
    none.status = ConfigStatus::NO_CONFIG;
    this->config_done_(none, now_ms);
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
  this->set_state_(State::READ_CONFIG, TIMEOUT_COMMAND, now_ms);  // restart the timer per chunk
  if (this->config_.size() < this->config_total_)
    return;

  // The raw blob at VERBOSE, to capture a label's config for the host tests.
  for (size_t off = 0; off < this->config_.size(); off += 32) {
    char hex[3 * 32 + 1];
    size_t n = std::min<size_t>(32, this->config_.size() - off);
    for (size_t i = 0; i < n; i++)
      snprintf(hex + 3 * i, 4, "%02X ", this->config_[off + i]);
    hex[3 * n] = '\0';
    this->log_(LogLevel::VERBOSE, "Config %04X: %s", (unsigned) off, hex);
  }
  ConfigResult cfg = parse_config(this->config_.data(), this->config_.size());
  if (cfg.crc_present && !cfg.crc_ok) {
    // Firmware only warns about this too.
    this->log_(LogLevel::WARN, "Config CRC mismatch (stored 0x%04X, computed 0x%04X); using it anyway",
               cfg.crc_stored, cfg.crc_computed);
  }
  if (cfg.truncated)
    this->log_(LogLevel::DEBUG, "Config packet walk stopped early after %u packets", (unsigned) cfg.packets);
  this->config_.clear();
  this->config_.shrink_to_fit();
  this->config_done_(cfg, now_ms);
}

void Session::config_done_(const ConfigResult &cfg, uint32_t now_ms) {
  this->transmission_modes_ = cfg.has_display ? cfg.display.transmission_modes : -1;
  const char *reason = "rejected by the host";
  if (!this->host_->od_config_ready(cfg, &reason)) {
    this->finish_(false, reason);
    return;
  }
  if (this->state_ != State::READ_CONFIG)
    return;  // the host ended the session
  this->start_upload_(now_ms);
}

// Compressed START: [00][70][uncompressed_size:4 LE][first zlib bytes...];
// the rest of the stream follows as DATA chunks. Any START body of 4 bytes or
// more is read as compressed, and the size must equal the panel's frame size.
void Session::start_upload_(uint32_t now_ms) {
  this->host_->od_frame(&this->frame_, &this->frame_len_);
  if (this->frame_ == nullptr || this->frame_len_ == 0) {
    this->finish_(false, "no frame to send");
    return;
  }
  bool modes_known = this->transmission_modes_ >= 0;
  bool streaming = modes_known && (this->transmission_modes_ & TRANSMISSION_STREAMING_DECOMPRESSION);
  bool label_can =
      !modes_known || (this->transmission_modes_ & (TRANSMISSION_STREAMING_DECOMPRESSION | TRANSMISSION_ZIP));

  this->compressed_ = false;
  if (this->options_.compression && label_can) {
    if (!zlib::compress(this->frame_, this->frame_len_, this->zbuf_)) {
      this->log_(LogLevel::WARN, "Compression ran out of memory; sending uncompressed");
    } else {
      size_t z_len = this->zbuf_.size();
      this->log_(LogLevel::DEBUG, "Compressed %u -> %u bytes", (unsigned) this->frame_len_, (unsigned) z_len);
      if (z_len < this->frame_len_ && (streaming || z_len < MAX_COMPRESSED_SIZE))
        this->compressed_ = true;
    }
  } else if (this->options_.compression) {
    this->log_(LogLevel::DEBUG, "Label does not advertise compressed uploads (modes 0x%02X)",
               (unsigned) this->transmission_modes_);
  }
  if (!this->compressed_) {
    this->send_uncompressed_start_(now_ms);
    return;
  }

  size_t max_start = MAX_START_PAYLOAD;
  uint16_t mtu = this->host_->od_mtu();
  if (mtu > ATT_WRITE_OVERHEAD && static_cast<size_t>(mtu - ATT_WRITE_OVERHEAD) < max_start)
    max_start = mtu - ATT_WRITE_OVERHEAD;
  if (max_start <= COMPRESSED_START_HEADER) {
    this->send_uncompressed_start_(now_ms);
    return;
  }
  size_t first = std::min(this->zbuf_.size(), max_start - COMPRESSED_START_HEADER);
  uint8_t payload[MAX_START_PAYLOAD];
  uint32_t raw = static_cast<uint32_t>(this->frame_len_);
  payload[0] = raw & 0xFF;
  payload[1] = (raw >> 8) & 0xFF;
  payload[2] = (raw >> 16) & 0xFF;
  payload[3] = (raw >> 24) & 0xFF;
  memcpy(payload + 4, this->zbuf_.data(), first);
  this->tx_src_ = this->zbuf_.data() + first;
  this->tx_len_ = this->zbuf_.size() - first;
  this->set_state_(State::WAIT_START, TIMEOUT_START, now_ms);
  if (this->tx_len_ == 0)
    this->mark_(now_ms);  // START carries the whole stream
  this->send_(CMD_DIRECT_WRITE_START, payload, 4 + first, false);
}

// Uncompressed START: a bare [00][70]; the whole frame follows as DATA.
void Session::send_uncompressed_start_(uint32_t now_ms) {
  this->compressed_ = false;
  this->zbuf_.clear();
  this->zbuf_.shrink_to_fit();
  this->tx_src_ = this->frame_;
  this->tx_len_ = this->frame_len_;
  this->set_state_(State::WAIT_START, TIMEOUT_START, now_ms);
  this->send_(CMD_DIRECT_WRITE_START, nullptr, 0, false);
}

void Session::send_next_chunk_(uint32_t now_ms) {
  size_t max_chunk = CHUNK_SIZE;
  uint16_t mtu = this->host_->od_mtu();
  size_t mtu_room = mtu > ATT_WRITE_OVERHEAD + CMD_HEADER ? mtu - ATT_WRITE_OVERHEAD - CMD_HEADER : 1;
  if (mtu_room < max_chunk)
    max_chunk = mtu_room;
  size_t remaining = this->tx_len_ - this->sent_;
  this->chunk_ = std::min(remaining, max_chunk);
  this->set_state_(State::SENDING, TIMEOUT_DATA_ACK, now_ms);
  if (this->sent_ + this->chunk_ >= this->tx_len_)
    this->mark_(now_ms);  // the last chunk can complete the transfer
  this->send_(CMD_DIRECT_WRITE_DATA, this->tx_src_ + this->sent_, this->chunk_, true);
}

void Session::send_end_(uint32_t now_ms) {
  uint8_t refresh = this->options_.refresh_mode;
  this->set_state_(State::WAIT_END, TIMEOUT_END_ACK, now_ms);
  this->mark_(now_ms);
  this->send_(CMD_DIRECT_WRITE_END, &refresh, 1, false);
}

}  // namespace esphome::opendisplay::od
