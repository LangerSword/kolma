// kolma -- bit-level and variable-length integer readers/writers.
#pragma once

#include <cstdint>
#include <stdexcept>

#include "kolma/util.hpp"

namespace kolma {

// Little-endian base-128 varint, 7 bits per byte, high bit = "more follows".
inline void put_varint(Bytes& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(uint8_t(v) | 0x80);
    v >>= 7;
  }
  out.push_back(uint8_t(v));
}

// Returns false when the buffer ends mid-varint or the encoding is over-long.
inline bool get_varint(View in, size_t& pos, uint64_t& out) {
  out = 0;
  int shift = 0;
  while (pos < in.size()) {
    uint8_t b = in[pos++];
    if (shift > 63) return false;
    out |= uint64_t(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

inline void put_u16(Bytes& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
inline void put_u32(Bytes& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (i * 8)));
}
inline void put_u64(Bytes& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (i * 8)));
}

inline bool get_u16(View in, size_t& pos, uint16_t& v) {
  if (pos + 2 > in.size()) return false;
  v = uint16_t(in[pos]) | (uint16_t(in[pos + 1]) << 8);
  pos += 2;
  return true;
}
inline bool get_u32(View in, size_t& pos, uint32_t& v) {
  if (pos + 4 > in.size()) return false;
  v = uint32_t(in[pos]) | (uint32_t(in[pos + 1]) << 8) | (uint32_t(in[pos + 2]) << 16) |
      (uint32_t(in[pos + 3]) << 24);
  pos += 4;
  return true;
}
inline bool get_u64(View in, size_t& pos, uint64_t& v) {
  if (pos + 8 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(in[pos + size_t(i)]) << (i * 8);
  pos += 8;
  return true;
}

// MSB-first bit writer, matching DEFLATE/Huffman convention.
class BitWriter {
 public:
  explicit BitWriter(Bytes& out) : out_(out) {}

  void put(uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; --i) {
      cur_ = uint8_t((cur_ << 1) | ((value >> i) & 1u));
      if (++filled_ == 8) {
        out_.push_back(cur_);
        cur_ = 0;
        filled_ = 0;
      }
    }
  }

  // Pad the final partial byte with zero bits.
  void flush() {
    if (filled_ > 0) {
      cur_ = uint8_t(cur_ << (8 - filled_));
      out_.push_back(cur_);
      cur_ = 0;
      filled_ = 0;
    }
  }

  size_t bits_written() const { return bit_count_; }

 private:
  Bytes& out_;
  uint8_t cur_ = 0;
  int filled_ = 0;
  size_t bit_count_ = 0;
};

class BitReader {
 public:
  explicit BitReader(View in) : in_(in) {}

  // Returns false when fewer than `bits` remain.
  bool get(int bits, uint32_t& value) {
    value = 0;
    for (int i = 0; i < bits; ++i) {
      if (pos_ >= in_.size()) return false;
      uint8_t bit = uint8_t((in_[pos_] >> (7 - bit_)) & 1u);
      value = (value << 1) | bit;
      if (++bit_ == 8) {
        bit_ = 0;
        ++pos_;
      }
    }
    return true;
  }

  size_t bits_consumed() const { return pos_ * 8 + size_t(bit_); }

 private:
  View in_;
  size_t pos_ = 0;
  int bit_ = 0;
};

}  // namespace kolma
