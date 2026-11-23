#pragma once

// Minimal zlib (RFC 1950) / DEFLATE (RFC 1951) encoder for OpenDisplay uploads.
//
// OpenDisplay firmware inflates with uzlib built for a 9-bit (512-byte)
// window and rejects zlib headers that advertise more, so this encoder writes
// a 9-bit header and never references further back than MAX_DISTANCE. One
// final block with the fixed Huffman codes is enough here: e-paper frames are
// mostly long runs, which LZ77 turns into length-258 / distance-1 matches.
//
// Header-only and free of ESPHome dependencies so it can be tested on a host.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome::opendisplay::zlib {

static constexpr uint8_t WINDOW_BITS = 9;
// zlib keeps MIN_LOOKAHEAD (262) of its window free, so a stock wbits=9
// stream never goes back further than 512 - 262 = 250. Stay inside that too.
static constexpr uint16_t MAX_DISTANCE = 250;
static constexpr uint16_t MIN_MATCH = 3;
static constexpr uint16_t MAX_MATCH = 258;
static constexpr uint16_t HASH_BITS = 10;
static constexpr uint16_t HASH_SIZE = 1u << HASH_BITS;
static constexpr uint16_t MAX_CHAIN = 32;

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t> &out) : out_(out) {}
  // DEFLATE packs data elements LSB-first.
  void put_bits(uint32_t value, uint8_t count) {
    this->acc_ |= value << this->n_;
    this->n_ += count;
    while (this->n_ >= 8) {
      this->out_.push_back(this->acc_ & 0xFF);
      this->acc_ >>= 8;
      this->n_ -= 8;
    }
  }
  // Huffman codes are packed MSB-first, so reverse them into the LSB stream.
  void put_code(uint32_t code, uint8_t len) {
    uint32_t rev = 0;
    for (uint8_t i = 0; i < len; i++)
      rev |= ((code >> i) & 1u) << (len - 1 - i);
    this->put_bits(rev, len);
  }
  void flush() {
    if (this->n_ > 0)
      this->out_.push_back(this->acc_ & 0xFF);
    this->acc_ = 0;
    this->n_ = 0;
  }

 private:
  std::vector<uint8_t> &out_;
  uint32_t acc_{0};
  uint8_t n_{0};
};

// Fixed literal/length code (RFC 1951 3.2.6).
inline void put_litlen(BitWriter &w, uint16_t sym) {
  if (sym < 144) {
    w.put_code(0x30 + sym, 8);
  } else if (sym < 256) {
    w.put_code(0x190 + (sym - 144), 9);
  } else if (sym < 280) {
    w.put_code(sym - 256, 7);
  } else {
    w.put_code(0xC0 + (sym - 280), 8);
  }
}

inline void put_length(BitWriter &w, uint16_t len) {
  static const uint16_t BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  uint8_t i = 28;
  while (BASE[i] > len)
    i--;
  put_litlen(w, 257 + i);
  if (EXTRA[i])
    w.put_bits(len - BASE[i], EXTRA[i]);
}

inline void put_distance(BitWriter &w, uint16_t dist) {
  static const uint16_t BASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  uint8_t i = 29;
  while (BASE[i] > dist)
    i--;
  w.put_code(i, 5);  // fixed distance codes are 5 bits
  if (EXTRA[i])
    w.put_bits(dist - BASE[i], EXTRA[i]);
}

inline uint16_t hash3(const uint8_t *p) {
  uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  return (v * 2654435761u) >> (32 - HASH_BITS);
}

// Compress `data` into a zlib stream. Returns false if memory runs out.
inline bool compress(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
  out.clear();
  out.reserve(len / 4 + 64);
  // CMF: CM = 8 (deflate), CINFO = WINDOW_BITS - 8. FLG: FCHECK makes
  // CMF*256 + FLG a multiple of 31, no dictionary, FLEVEL 0.
  uint8_t cmf = ((WINDOW_BITS - 8) << 4) | 8;
  uint8_t flg = 31 - ((cmf << 8) % 31);
  out.push_back(cmf);
  out.push_back(flg);

  // Hash chains: head[h] = last position + 1 with hash h, prev[pos % window]
  // = previous position + 1 in the same chain (0 = none).
  std::vector<uint32_t> head(HASH_SIZE, 0);
  std::vector<uint32_t> prev(512, 0);
  if (head.size() != HASH_SIZE || prev.size() != 512)
    return false;

  BitWriter w(out);
  w.put_bits(1, 1);  // BFINAL
  w.put_bits(1, 2);  // BTYPE = 01, fixed Huffman

  auto insert = [&](size_t pos) {
    if (pos + MIN_MATCH > len)
      return;
    uint16_t h = hash3(data + pos);
    prev[pos & 511] = head[h];
    head[h] = pos + 1;
  };

  size_t pos = 0;
  while (pos < len) {
    uint16_t best_len = 0, best_dist = 0;
    if (pos + MIN_MATCH <= len) {
      uint16_t max_len = len - pos < MAX_MATCH ? len - pos : MAX_MATCH;
      uint32_t cand = head[hash3(data + pos)];
      for (uint16_t chain = 0; cand != 0 && chain < MAX_CHAIN; chain++) {
        size_t c = cand - 1;
        size_t dist = pos - c;
        if (dist == 0 || dist > MAX_DISTANCE)
          break;
        uint16_t l = 0;
        while (l < max_len && data[c + l] == data[pos + l])
          l++;
        if (l > best_len) {
          best_len = l;
          best_dist = dist;
          if (l == max_len)
            break;
        }
        uint32_t next = prev[c & 511];
        if (next == 0 || next - 1 >= c)
          break;  // chain left the window or wrapped
        cand = next;
      }
    }
    if (best_len >= MIN_MATCH) {
      put_length(w, best_len);
      put_distance(w, best_dist);
      for (uint16_t i = 0; i < best_len; i++)
        insert(pos + i);
      pos += best_len;
    } else {
      put_litlen(w, data[pos]);
      insert(pos);
      pos++;
    }
  }
  put_litlen(w, 256);  // end of block
  w.flush();

  // Adler-32 of the uncompressed data, big-endian.
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  uint32_t adler = (b << 16) | a;
  out.push_back(adler >> 24);
  out.push_back((adler >> 16) & 0xFF);
  out.push_back((adler >> 8) & 0xFF);
  out.push_back(adler & 0xFF);
  return true;
}

}  // namespace esphome::opendisplay::zlib
