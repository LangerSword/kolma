// lzw -- Lempel-Ziv-Welch with variable-width codes (9..12 bits), a clear code
// and an end-of-information code.
//
// Block layout: varint(original length) | MSB-first bitstream of codes
// Code 256 = clear (reset the dictionary), 257 = EOI, 258+ = dictionary entries.
//
// Encoder and decoder must agree on the exact width-bump instant. The decoder's
// next free code lags the encoder's by one, so the decoder bumps at
// (1 << width) - 1 while the encoder bumps at (1 << width). Round-trip tests
// cover the 512 and 4096 boundaries deliberately.
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr uint32_t kClearCode = 256;
constexpr uint32_t kEoiCode = 257;
constexpr uint32_t kFirstFree = 258;
constexpr int kMinWidth = 9;
constexpr int kMaxWidth = 12;
constexpr uint32_t kMaxCodes = 1u << kMaxWidth;

class LzwAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    put_varint(out, in.size());
    if (in.empty()) return;

    // Dictionary keyed by (prefix code, next byte).
    std::unordered_map<uint32_t, uint32_t> dict;
    dict.reserve(kMaxCodes * 2);

    BitWriter bw(out);
    int width = kMinWidth;
    uint32_t next_code = kFirstFree;
    uint32_t prefix = in[0];

    for (size_t i = 1; i < in.size(); ++i) {
      uint8_t c = in[i];
      uint32_t key = (prefix << 8) | c;
      auto it = dict.find(key);
      if (it != dict.end()) {
        prefix = it->second;
        continue;
      }
      bw.put(prefix, width);
      dict.emplace(key, next_code);
      ++next_code;
      if (next_code == (1u << width) && width < kMaxWidth) ++width;
      if (next_code >= kMaxCodes) {
        bw.put(kClearCode, width);
        dict.clear();
        next_code = kFirstFree;
        width = kMinWidth;
      }
      prefix = c;
    }
    bw.put(prefix, width);
    bw.put(kEoiCode, width);
    bw.flush();
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("lzw: truncated length prefix");
    if (declared != orig_size) throw FormatError("lzw: length prefix disagrees with the archive");

    out.clear();
    out.reserve(size_t(orig_size));
    if (orig_size == 0) return;

    // Dictionary entry: prefix code (-1 for a root byte) plus its first byte.
    struct Entry {
      int32_t prefix;
      uint8_t first;
    };
    std::array<Entry, kMaxCodes> table{};
    for (uint32_t i = 0; i < 256; ++i) table[i] = Entry{-1, uint8_t(i)};

    auto expand = [&](uint32_t code, Bytes& scratch) {
      scratch.clear();
      uint32_t c = code;
      int guard = 0;
      while (c != uint32_t(-1)) {
        if (c >= kMaxCodes) throw FormatError("lzw: code outside the dictionary");
        scratch.push_back(table[c].first);
        c = uint32_t(table[c].prefix);
        if (++guard > int(kMaxCodes)) throw FormatError("lzw: cyclic dictionary entry");
      }
      std::reverse(scratch.begin(), scratch.end());
    };

    BitReader br(View(in.data() + pos, in.size() - pos));
    int width = kMinWidth;
    uint32_t next_code = kFirstFree;
    int32_t prev = -1;
    Bytes scratch;

    while (out.size() < orig_size) {
      uint32_t code = 0;
      if (!br.get(width, code)) throw FormatError("lzw: code stream ended early");
      if (code == kClearCode) {
        next_code = kFirstFree;
        width = kMinWidth;
        prev = -1;
        continue;
      }
      if (code == kEoiCode) break;

      if (prev < 0) {
        if (code >= 256) throw FormatError("lzw: first code must be a literal byte");
        out.push_back(uint8_t(code));
        prev = int32_t(code);
        continue;
      }

      if (code < next_code) {
        expand(code, scratch);
      } else if (code == next_code) {
        expand(uint32_t(prev), scratch);
        scratch.push_back(scratch.front());
      } else {
        throw FormatError("lzw: code beyond the next free entry");
      }

      out.insert(out.end(), scratch.begin(), scratch.end());
      if (next_code < kMaxCodes) {
        table[next_code] = Entry{prev, scratch.front()};
        ++next_code;
        if (next_code == (1u << width) - 1 && width < kMaxWidth) ++width;
      }
      prev = int32_t(code);
    }
    if (out.size() != orig_size) throw FormatError("lzw: decoded size disagrees with the archive");
  }

 private:
  AlgoParams p_{
      .id = "lzw",
      .name = "LZW",
      .supported_types = {FileType::Text, FileType::Binary, FileType::Image},
      .typical_ratio = 0.5,
      .typical_speed_mbps = 90.0,
      .memory_bytes = 2u << 20,
      .block_size = 1u << 24,
      .dictionary_size = 4096,
      .notes = "dictionary of substrings, 9..12 bit codes; the GIF/TIFF family method",
  };
};

}  // namespace

const Algorithm* algo_lzw() {
  static const LzwAlgorithm instance;
  return &instance;
}

}  // namespace kolma
