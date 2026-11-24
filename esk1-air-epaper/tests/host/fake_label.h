#pragma once

// A simulated OpenDisplay label for host tests: it takes the writes a
// od::Session makes and queues the notifications the firmware would send
// (Firmware_Unified shared/core/od_xfer_direct.c, od_config_read.c, as read
// for docs/opendisplay-protocol-notes.md). Compressed uploads are inflated by
// an independent decoder written here from RFC 1950/1951, and the result is
// compared with the frame the test expected.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace host_test {

// ---- Minimal inflate (stored + fixed Huffman blocks), RFC 1951 ----
class Inflater {
 public:
  // Returns false on any format error. window_bits_out: from the zlib header.
  static bool inflate_zlib(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, int *window_bits_out,
                           size_t max_distance_seen[1]) {
    if (in.size() < 6)
      return false;
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
      return false;
    *window_bits_out = (cmf >> 4) + 8;
    Inflater st(in.data() + 2, in.size() - 6);
    max_distance_seen[0] = 0;
    if (!st.run(out, max_distance_seen))
      return false;
    uint32_t a = 1, b = 0;
    for (uint8_t c : out) {
      a = (a + c) % 65521;
      b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    const uint8_t *t = in.data() + in.size() - 4;
    uint32_t stored = (uint32_t(t[0]) << 24) | (uint32_t(t[1]) << 16) | (uint32_t(t[2]) << 8) | t[3];
    return adler == stored;
  }

 private:
  Inflater(const uint8_t *p, size_t n) : p_(p), n_(n) {}
  int bit() {
    if (this->pos_ >= this->n_ * 8) {
      this->overrun_ = true;
      return 0;
    }
    int b = (this->p_[this->pos_ >> 3] >> (this->pos_ & 7)) & 1;
    this->pos_++;
    return b;
  }
  uint32_t bits(int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
      v |= uint32_t(this->bit()) << i;
    return v;
  }
  // Fixed literal/length code, read MSB-first.
  int fixed_litlen() {
    int code = 0;
    for (int len = 1; len <= 9; len++) {
      code = (code << 1) | this->bit();
      if (len == 7 && code <= 0x17)
        return 256 + code;
      if (len == 8 && code >= 0x30 && code <= 0xBF)
        return code - 0x30;
      if (len == 8 && code >= 0xC0 && code <= 0xC7)
        return 280 + (code - 0xC0);
      if (len == 9 && code >= 0x190)
        return 144 + (code - 0x190);
    }
    return -1;
  }
  bool run(std::vector<uint8_t> &out, size_t *max_dist) {
    static const uint16_t LBASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t LEXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t DBASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t DEXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    bool final_block = false;
    while (!final_block) {
      final_block = this->bit();
      uint32_t type = this->bits(2);
      if (type == 0) {
        this->pos_ = (this->pos_ + 7) & ~size_t(7);
        uint32_t len = this->bits(16), nlen = this->bits(16);
        if ((len ^ 0xFFFF) != nlen)
          return false;
        for (uint32_t i = 0; i < len; i++)
          out.push_back(uint8_t(this->bits(8)));
      } else if (type == 1) {
        for (;;) {
          int sym = this->fixed_litlen();
          if (sym < 0 || this->overrun_)
            return false;
          if (sym < 256) {
            out.push_back(uint8_t(sym));
          } else if (sym == 256) {
            break;
          } else {
            int li = sym - 257;
            if (li >= 29)
              return false;
            uint32_t len = LBASE[li] + this->bits(LEXTRA[li]);
            int di = 0;
            for (int i = 0; i < 5; i++)
              di = (di << 1) | this->bit();  // fixed distance codes, MSB-first
            if (di >= 30)
              return false;
            uint32_t dist = DBASE[di] + this->bits(DEXTRA[di]);
            if (dist > out.size())
              return false;
            if (dist > *max_dist)
              *max_dist = dist;
            for (uint32_t i = 0; i < len; i++)
              out.push_back(out[out.size() - dist]);
          }
        }
      } else {
        return false;  // dynamic Huffman is not produced by our encoder
      }
      if (this->overrun_)
        return false;
    }
    return true;
  }
  const uint8_t *p_;
  size_t n_;
  size_t pos_{0};
  bool overrun_{false};
};

// ---- Config blob builder ----
struct ConfigSpec {
  uint16_t width{400}, height{300};
  uint8_t color_scheme{3};  // BWRY
  uint8_t transmission_modes{0x01 | 0x08};
  uint16_t panel_ic{0x0042};
  uint8_t rotation{0};
  bool drive_packets{true};
  bool has_code_map{false};
  uint8_t code_map[4]{0, 1, 2, 3};
  bool corrupt_crc{false};
  bool with_display{true};
  bool wifi_packet{false};  // adds a 160-byte wifi packet (0x26) first
};

inline uint16_t crc16_ccitt_false_ref(const std::vector<uint8_t> &d) {
  uint16_t crc = 0xFFFF;
  for (uint8_t b : d) {
    crc ^= uint16_t(b) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
  }
  return crc;
}

inline std::vector<uint8_t> build_config(const ConfigSpec &s) {
  std::vector<uint8_t> b = {0, 0, 1};  // length (filled below), version
  // system packet (0x01, 22 bytes)
  b.push_back(0);
  b.push_back(0x01);
  b.insert(b.end(), 22, 0);
  if (s.wifi_packet) {
    b.push_back(0);
    b.push_back(0x26);
    b.insert(b.end(), 160, 0x41);
  }
  if (s.with_display) {
    b.push_back(0);
    b.push_back(0x20);
    uint8_t d[46] = {};
    d[2] = s.panel_ic & 0xFF;
    d[3] = s.panel_ic >> 8;
    d[4] = s.width & 0xFF;
    d[5] = s.width >> 8;
    d[6] = s.height & 0xFF;
    d[7] = s.height >> 8;
    d[14] = s.rotation;
    d[21] = s.color_scheme;
    d[22] = s.transmission_modes;
    b.insert(b.end(), d, d + 46);
  }
  if (s.drive_packets || s.has_code_map) {
    // display_panel (0xB1): <BBBHH4sBB + no sleep sequence = 13 bytes
    uint8_t body[13] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0};
    if (s.has_code_map)
      memcpy(body + 7, s.code_map, 4);
    b.push_back(1);
    b.push_back(0xB1);
    b.push_back(sizeof(body));
    b.push_back(0);
    b.insert(b.end(), body, body + sizeof(body));
  }
  size_t total = b.size() + 2;
  b[0] = total & 0xFF;
  b[1] = total >> 8;
  std::vector<uint8_t> crc_in = b;
  crc_in[0] = crc_in[1] = 0;
  uint16_t crc = crc16_ccitt_false_ref(crc_in);
  if (s.corrupt_crc)
    crc ^= 0x5A5A;
  b.push_back(crc & 0xFF);
  b.push_back(crc >> 8);
  return b;
}

// ---- The label ----
class FakeLabel {
 public:
  // Behaviour
  bool has_config{true};
  std::vector<uint8_t> config;
  size_t expected_frame_bytes{30000};
  bool refuse_compressed{false};
  uint8_t refuse_error{0};          // 0 = bare [FF][70]; else [FF][70][err][00]
  bool legacy_refuse{false};        // [FF][FF] instead of [FF][70]
  bool start_nack_any{false};       // refuse every START with refuse_error
  bool auto_complete{false};        // uncompressed: answer the last DATA with [00][72]
  uint8_t refresh_echo{0x73};       // 0x73 success, 0x74 timeout, 0 = never answer
  bool auth_required{false};
  long nack_data_at{-1};            // refuse the DATA that would pass this byte
  int silent_after_writes{-1};      // stop answering after this many writes
  long end_incomplete{false};       // answer END with [FF][72]

  // Observed
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> received;    // DATA payload bytes in order
  bool start_compressed{false};
  uint32_t start_declared_size{0};
  std::vector<uint8_t> zstream;     // compressed stream: START tail + DATA
  int starts{0};
  bool end_seen{false};
  uint8_t end_mode{0xFF};
  std::vector<uint8_t> image;       // what the panel would show
  bool image_ok{false};
  int window_bits{0};
  size_t max_distance{0};

  std::deque<std::vector<uint8_t>> notifications;

  void on_write(const uint8_t *d, size_t n) {
    this->writes.emplace_back(d, d + n);
    if (this->silent_after_writes >= 0 && (int) this->writes.size() > this->silent_after_writes)
      return;
    if (n < 2)
      return;
    uint16_t op = (d[0] << 8) | d[1];
    if (this->auth_required) {
      this->notify({0xFE, uint8_t(op & 0xFF)});
      return;
    }
    switch (op) {
      case 0x0040:
        this->config_read_();
        break;
      case 0x0070:
        this->start_(d + 2, n - 2);
        break;
      case 0x0071:
        this->data_(d + 2, n - 2);
        break;
      case 0x0072:
        this->end_(d + 2, n - 2);
        break;
      default:
        this->notify({0xFF, uint8_t(op & 0xFF)});
        break;
    }
  }

  bool finish_image_() {
    if (this->start_compressed) {
      std::vector<uint8_t> out;
      size_t md[1] = {0};
      if (!Inflater::inflate_zlib(this->zstream, out, &this->window_bits, md))
        return false;
      this->max_distance = md[0];
      this->image = out;
    } else {
      this->image = this->received;
    }
    return this->image.size() == this->expected_frame_bytes;
  }

 private:
  void notify(std::vector<uint8_t> v) { this->notifications.push_back(std::move(v)); }

  void config_read_() {
    if (!this->has_config || this->config.empty()) {
      this->notify({0xFF, 0x40, 0x00, 0x00});
      return;
    }
    // 100-byte frames: chunk 0 carries <= 94 data bytes, later chunks <= 96.
    size_t off = 0;
    uint16_t chunk = 0;
    while (off < this->config.size() || chunk == 0) {
      std::vector<uint8_t> f = {0x00, 0x40, uint8_t(chunk & 0xFF), uint8_t(chunk >> 8)};
      size_t room = 96;
      if (chunk == 0) {
        f.push_back(this->config.size() & 0xFF);
        f.push_back(this->config.size() >> 8);
        room = 94;
      }
      size_t take = std::min(room, this->config.size() - off);
      f.insert(f.end(), this->config.begin() + off, this->config.begin() + off + take);
      off += take;
      this->notify(f);
      chunk++;
    }
  }

  void refuse_() {
    if (this->legacy_refuse) {
      this->notify({0xFF, 0xFF});
    } else if (this->refuse_error != 0) {
      this->notify({0xFF, 0x70, this->refuse_error, 0x00});
    } else {
      this->notify({0xFF, 0x70});
    }
  }

  void start_(const uint8_t *body, size_t n) {
    this->starts++;
    this->received.clear();
    this->zstream.clear();
    this->end_seen = false;
    // Any START body of >= 4 bytes is the compressed form.
    this->start_compressed = n >= 4;
    if (this->start_nack_any || (this->start_compressed && this->refuse_compressed)) {
      this->refuse_();
      return;
    }
    if (this->start_compressed) {
      this->start_declared_size = body[0] | (body[1] << 8) | (body[2] << 16) | (uint32_t(body[3]) << 24);
      if (this->start_declared_size != this->expected_frame_bytes) {
        this->notify({0xFF, 0x70});
        return;
      }
      this->zstream.insert(this->zstream.end(), body + 4, body + n);
    }
    this->notify({0x00, 0x70});
  }

  void data_(const uint8_t *body, size_t n) {
    size_t have = this->start_compressed ? this->zstream.size() : this->received.size();
    if (this->nack_data_at >= 0 && (long) (have + n) > this->nack_data_at) {
      this->notify({0xFF, 0x71});
      return;
    }
    if (this->start_compressed) {
      this->zstream.insert(this->zstream.end(), body, body + n);
    } else {
      this->received.insert(this->received.end(), body, body + n);
    }
    if (!this->start_compressed && this->auto_complete && this->received.size() >= this->expected_frame_bytes) {
      this->image_ok = this->finish_image_();
      this->notify({0x00, 0x72});
      if (this->refresh_echo != 0)
        this->notify({0x00, this->refresh_echo});
      return;
    }
    this->notify({0x00, 0x71});
  }

  void end_(const uint8_t *body, size_t n) {
    this->end_seen = true;
    this->end_mode = n >= 1 ? body[0] : 0xFF;
    this->image_ok = this->finish_image_();
    if (!this->image_ok || this->end_incomplete) {
      this->notify({0xFF, 0x72});
      return;
    }
    this->notify({0x00, 0x72});
    if (this->refresh_echo != 0)
      this->notify({0x00, this->refresh_echo});
  }
};

}  // namespace host_test
