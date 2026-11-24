#pragma once

// One OpenDisplay DIRECT_WRITE session over an established connection:
//
//   CONFIG_READ (0x0040)        -> host checks the panel and prepares the frame
//   DIRECT_WRITE_START (0x0070) zlib-compressed when allowed, else bare
//   DIRECT_WRITE_DATA (0x0071)  one chunk per ACK
//   DIRECT_WRITE_END (0x0072)   then [00][73] success / [00][74] timeout
//
// Transport-independent (no ESPHome, no ESP-IDF): the host provides writes,
// the MTU, the frame and the panel decision, and feeds back notifications,
// write errors, disconnects and the time. That lets the whole protocol run
// against a simulated label on a PC (tests/host).
//
// Wire facts are referenced in docs/opendisplay-protocol-notes.md.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "od_config.h"

namespace esphome::opendisplay::od {

enum class Result : uint8_t {
  SUCCESS,           // [00][73]: the panel refreshed
  FAILED_NOT_SENT,   // failed before anything could start a refresh
  FAILED_UNCERTAIN,  // failed after a refresh could have begun
};
const char *result_name(Result r);

enum class LogLevel : uint8_t { ERROR, WARN, INFO, DEBUG, VERBOSE };

class SessionHost {
 public:
  virtual ~SessionHost() = default;
  // Queues one write to the OpenDisplay characteristic. data_chunk marks a
  // DATA frame (acked by notification, so write-without-response is fine).
  // Returns false if it could not be queued.
  virtual bool od_write(const uint8_t *data, size_t len, bool data_chunk) = 0;
  virtual uint16_t od_mtu() = 0;
  // CONFIG_READ finished (cfg.status says how it went). Prepare the frame
  // for that panel and return true, or return false with *reason set.
  virtual bool od_config_ready(const ConfigResult &cfg, const char **reason) = 0;
  // The prepared frame, in wire format. Valid until od_finished().
  virtual void od_frame(const uint8_t **data, size_t *len) = 0;
  // The session ended. reason is nullptr on success.
  virtual void od_finished(Result result, const char *reason) = 0;
  virtual void od_log(LogLevel level, const char *message) = 0;
};

struct SessionOptions {
  bool compression{true};
  uint8_t refresh_mode{0};  // END byte: 0 full, 1 fast
};

class Session {
 public:
  explicit Session(SessionHost *host) : host_(host) {}
  void set_options(const SessionOptions &options) { this->options_ = options; }

  // Starts with CONFIG_READ. The connection must be ready for writes.
  void begin(uint32_t now_ms);
  void on_notify(const uint8_t *data, size_t len, uint32_t now_ms);
  void on_write_error(int status, uint32_t now_ms);
  void on_disconnect(uint32_t now_ms);
  void tick(uint32_t now_ms);  // per-state timeouts
  void abort(const char *reason);

  bool busy() const { return this->state_ != State::IDLE; }
  const char *state_name() const;
  // When a panel refresh could first have started in the last (or current)
  // transfer: the last DATA chunk, a START carrying the whole stream, END, or
  // a firmware auto-complete. Cleared by begin().
  bool refresh_marker(uint32_t *ms) const {
    if (this->has_marker_ && ms != nullptr)
      *ms = this->marker_ms_;
    return this->has_marker_;
  }
  bool compressed() const { return this->compressed_; }
  size_t frame_bytes() const { return this->frame_len_; }
  size_t sent_bytes() const { return this->tx_len_; }

  // Limits (opendisplay_protocol.h; py-opendisplay protocol/commands.py).
  static constexpr size_t CHUNK_SIZE = 230;           // DATA bytes per frame, unencrypted
  static constexpr size_t MAX_START_PAYLOAD = 200;    // whole START write, opcode included
  static constexpr size_t MAX_CONFIG_SIZE = 4096;     // sanity cap on a CONFIG_READ blob
  static constexpr size_t MAX_COMPRESSED_SIZE = 50 * 1024;  // without streaming decompression
  static constexpr uint32_t TIMEOUT_COMMAND = 10000;
  static constexpr uint32_t TIMEOUT_START = 30000;    // the panel is initialised on START
  static constexpr uint32_t TIMEOUT_DATA_ACK = 90000;
  static constexpr uint32_t TIMEOUT_END_ACK = 90000;
  static constexpr uint32_t TIMEOUT_REFRESH = 90000;

 protected:
  enum class State : uint8_t { IDLE, READ_CONFIG, WAIT_START, SENDING, WAIT_END, WAIT_REFRESH };

  void set_state_(State s, uint32_t timeout_ms, uint32_t now_ms);
  void finish_(bool success, const char *reason);
  bool send_(uint16_t opcode, const uint8_t *payload, size_t len, bool data_chunk);
  void on_config_frame_(const uint8_t *data, size_t len, uint32_t now_ms);
  void config_done_(const ConfigResult &cfg, uint32_t now_ms);
  void start_upload_(uint32_t now_ms);
  void send_uncompressed_start_(uint32_t now_ms);
  void send_next_chunk_(uint32_t now_ms);
  void send_end_(uint32_t now_ms);
  void mark_(uint32_t now_ms);
  void log_(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

  SessionHost *host_;
  SessionOptions options_;
  State state_{State::IDLE};
  uint32_t state_started_ms_{0};
  uint32_t state_timeout_ms_{0};
  uint32_t now_ms_{0};

  std::vector<uint8_t> config_;
  uint16_t config_total_{0};
  int16_t transmission_modes_{-1};  // -1 = unknown

  const uint8_t *frame_{nullptr};
  size_t frame_len_{0};
  std::vector<uint8_t> zbuf_;
  bool compressed_{false};
  const uint8_t *tx_src_{nullptr};
  size_t tx_len_{0};
  size_t sent_{0};
  size_t chunk_{0};

  bool has_marker_{false};
  uint32_t marker_ms_{0};
  std::vector<uint8_t> tx_;
};

}  // namespace esphome::opendisplay::od
