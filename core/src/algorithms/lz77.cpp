// lz77 -- LZSS: a 32 KiB sliding window, 3..258 byte matches, greedy with
// optional lazy matching at higher levels.
//
// Block layout: varint(original length) | groups of (flag byte + up to 8 tokens)
// Flag bit k describes token k in the group: 1 = match, 0 = literal.
// Match token = u16 offset (1-based, distance back) | u8 (length - 3).
#include <algorithm>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr size_t kWindow = 32768;
constexpr size_t kMinMatch = 3;
constexpr size_t kMaxMatch = 258;
constexpr size_t kHashBits = 16;
constexpr size_t kHashSize = size_t(1) << kHashBits;

class Lz77Algorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int level) const override {
    level = clamp_level(level);
    const size_t n = in.size();
    put_varint(out, n);
    if (n == 0) return;

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(n, -1);
    const int max_chain = 16 + level * 32;
    const bool lazy = level >= 5;
    // Lazy matching inserts a position while probing ahead, so the insertion
    // point has to be remembered: inserting the same position twice would put
    // the position in its own chain and poison every later search.
    size_t inserted_upto = 0;

    auto hash_at = [&](size_t i) -> uint32_t {
      uint32_t h = (uint32_t(in[i]) << 16) ^ (uint32_t(in[i + 1]) << 8) ^ uint32_t(in[i + 2]);
      return (h * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
      if (i + kMinMatch > n || i < inserted_upto) return;
      uint32_t h = hash_at(i);
      prev[i] = head[h];
      head[h] = int32_t(i);
      inserted_upto = i + 1;
    };
    // Longest match for position i; returns length (0 when none) and distance.
    auto find_match = [&](size_t i, size_t& distance) -> size_t {
      if (i + kMinMatch > n) return 0;
      const size_t limit = std::min(n - i, kMaxMatch);
      int32_t cand = head[hash_at(i)];
      size_t best = 0;
      int chain = 0;
      while (cand >= 0 && chain < max_chain) {
        const size_t c = size_t(cand);
        if (i - c > kWindow) break;
        size_t len = 0;
        while (len < limit && in[c + len] == in[i + len]) ++len;
        if (len > best) {
          best = len;
          distance = i - c;
          if (len >= limit) break;
        }
        cand = prev[c];
        ++chain;
      }
      return (best >= kMinMatch && distance > 0) ? best : 0;
    };

    Bytes body;
    size_t flag_pos = 0;
    uint8_t flags = 0;
    int in_group = 0;
    auto begin_token = [&]() {
      if (in_group == 0) {
        flag_pos = body.size();
        body.push_back(0);
      }
    };
    auto end_token = [&]() {
      if (++in_group == 8) {
        body[flag_pos] = flags;
        flags = 0;
        in_group = 0;
      }
    };
    auto emit_literal = [&](uint8_t b) {
      begin_token();
      body.push_back(b);
      end_token();
    };
    auto emit_match = [&](size_t distance, size_t length) {
      begin_token();
      flags = uint8_t(flags | (1u << in_group));
      body.push_back(uint8_t(distance & 0xFF));
      body.push_back(uint8_t((distance >> 8) & 0xFF));
      body.push_back(uint8_t(length - kMinMatch));
      end_token();
    };

    size_t i = 0;
    while (i < n) {
      size_t distance = 0;
      size_t length = find_match(i, distance);
      if (length == 0) {
        insert(i);
        emit_literal(in[i]);
        ++i;
        continue;
      }
      if (lazy && i + 1 < n) {
        // Look one byte ahead: a strictly longer match there is worth a literal.
        size_t next_distance = 0;
        insert(i);
        size_t next_length = find_match(i + 1, next_distance);
        if (next_length > length) {
          emit_literal(in[i]);
          ++i;
          continue;
        }
      }
      emit_match(distance, length);
      for (size_t k = 0; k < length; ++k) insert(i + k);
      i += length;
    }
    if (in_group > 0) body[flag_pos] = flags;

    out.insert(out.end(), body.begin(), body.end());
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("lz77: truncated length prefix");
    if (declared != orig_size) throw FormatError("lz77: length prefix disagrees with the archive");

    out.clear();
    out.reserve(size_t(orig_size));
    while (out.size() < orig_size) {
      if (pos >= in.size()) throw FormatError("lz77: token stream ended early");
      uint8_t flags = in[pos++];
      for (int k = 0; k < 8 && out.size() < orig_size; ++k) {
        if ((flags >> k) & 1u) {
          if (pos + 3 > in.size()) throw FormatError("lz77: truncated match token");
          uint32_t distance = uint32_t(in[pos]) | (uint32_t(in[pos + 1]) << 8);
          size_t length = size_t(in[pos + 2]) + kMinMatch;
          pos += 3;
          if (distance == 0 || distance > out.size())
            throw FormatError("lz77: match points outside the decoded window");
          if (out.size() + length > orig_size) throw FormatError("lz77: match overruns the block");
          for (size_t t = 0; t < length; ++t) out.push_back(out[out.size() - distance]);
        } else {
          if (pos >= in.size()) throw FormatError("lz77: truncated literal");
          out.push_back(in[pos++]);
        }
      }
    }
  }

 private:
  AlgoParams p_{
      .id = "lz77",
      .name = "LZ77 (LZSS)",
      .supported_types = {FileType::Text, FileType::Executable, FileType::Binary, FileType::Image},
      .typical_ratio = 0.42,
      .typical_speed_mbps = 60.0,
      .memory_bytes = 8u << 20,
      .block_size = 1u << 24,
      .dictionary_size = kWindow,
      .notes = "32 KiB window, lazy matching above level 4; the workhorse default",
  };
};

}  // namespace

const Algorithm* algo_lz77() {
  static const Lz77Algorithm instance;
  return &instance;
}

}  // namespace kolma
