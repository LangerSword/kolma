// rbrc -- Run-Boundary Rank Coder.
//
// The primary signal is a transition mask, not a symbol stream: M[i] is 1 when
// byte[i] differs from byte[i-1]. At each transition a new byte is resolved
// through a block-static, frequency-ranked reduced alphabet, with an escape to
// a raw literal. All probabilities are stored in the header and never adapt.
//
// Block layout:
//   varint(original length)
//   u16 alphabet size N (0..256)
//   N alphabet bytes, sorted by frequency then byte value
//   N u8 rank weights (non-zero)
//   u16 transition probability p1 in [1,65535]
//   u16 escape probability in [1, 65536-N]
//   static range-coded payload
//
// There is no context tree, no MTF mutation, no BWT sort, no LZ window and no
// run-length-bucket context. See docs/research/rbrc-prior-art.md for scope.
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr uint32_t kScale = 1u << 16;

// ----------------------------------------------------------- range coder ----
// A strict variant of the LZMA-style coder used by bwt-mtf-ari: the decoder
// refuses to invent bytes past the end of a block.
class RangeEncoder {
 public:
  explicit RangeEncoder(Bytes& out) : out_(out) {}

  void encode(uint32_t start, uint32_t size, uint32_t total) {
    if (size == 0 || total == 0 || start > total || size > total - start)
      throw FormatError("rbrc: invalid range model");
    range_ /= total;
    low_ += uint64_t(start) * range_;
    range_ *= size;
    while (range_ < (1u << 24)) {
      range_ <<= 8;
      shift_low();
    }
  }

  void flush() {
    for (int i = 0; i < 5; ++i) shift_low();
  }

 private:
  void shift_low() {
    if (uint32_t(low_ >> 32) != 0 || uint32_t(low_) < 0xFF000000u) {
      uint8_t carry = uint8_t(low_ >> 32);
      if (cache_size_ != 0) {
        out_.push_back(uint8_t(cache_ + carry));
        while (--cache_size_ != 0) out_.push_back(uint8_t(0xFF + carry));
      }
      cache_ = uint8_t(low_ >> 24);
    }
    ++cache_size_;
    low_ = uint64_t(uint32_t(uint32_t(low_) << 8));
  }

  Bytes& out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

class RangeDecoder {
 public:
  explicit RangeDecoder(View in) : in_(in) {
    for (int i = 0; i < 5; ++i) code_ = (code_ << 8) | read_byte();
  }

  uint32_t get_freq(uint32_t total) {
    range_ /= total;
    return code_ / range_;
  }

  void update(uint32_t start, uint32_t size) {
    code_ -= start * range_;
    range_ *= size;
    while (range_ < (1u << 24)) {
      code_ = (code_ << 8) | read_byte();
      range_ <<= 8;
    }
  }

 private:
  uint8_t read_byte() {
    if (pos_ >= in_.size()) throw FormatError("rbrc: truncated range-coded stream");
    return in_[pos_++];
  }

  View in_;
  size_t pos_ = 0;
  uint32_t code_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
};

struct StaticModel {
  std::vector<uint32_t> freq;
  uint32_t total = 0;

  uint32_t start_of(uint32_t symbol) const {
    uint32_t start = 0;
    for (uint32_t i = 0; i < symbol; ++i) start += freq[size_t(i)];
    return start;
  }

  uint32_t symbol_for(uint32_t value) const {
    uint32_t start = 0;
    for (uint32_t i = 0; i < freq.size(); ++i) {
      if (start + freq[size_t(i)] > value) return i;
      start += freq[size_t(i)];
    }
    throw FormatError("rbrc: symbol outside model range");
  }
};

void encode(RangeEncoder& enc, const StaticModel& model, uint32_t symbol) {
  if (symbol >= model.freq.size()) throw FormatError("rbrc: symbol outside model");
  enc.encode(model.start_of(symbol), model.freq[size_t(symbol)], model.total);
}

uint32_t decode(RangeDecoder& dec, const StaticModel& model) {
  uint32_t value = dec.get_freq(model.total);
  uint32_t symbol = model.symbol_for(value);
  dec.update(model.start_of(symbol), model.freq[size_t(symbol)]);
  return symbol;
}

StaticModel binary_model(uint16_t p1) {
  StaticModel m;
  m.freq = {kScale - uint32_t(p1), uint32_t(p1)};
  m.total = kScale;
  return m;
}

StaticModel uniform_model(uint32_t symbols) {
  StaticModel m;
  m.freq.assign(size_t(symbols), 1);
  m.total = symbols;
  return m;
}

StaticModel rank_model(const Bytes& weights, uint16_t escape) {
  const uint32_t count = uint32_t(weights.size());
  if (count == 0 || escape == 0 || uint32_t(escape) >= kScale)
    throw FormatError("rbrc: invalid rank model header");
  const uint32_t rank_scale = kScale - uint32_t(escape);
  if (rank_scale < count) throw FormatError("rbrc: rank model has no room for its symbols");

  uint64_t weight_sum = 0;
  for (uint8_t w : weights) {
    if (w == 0) throw FormatError("rbrc: zero rank weight");
    weight_sum += w;
  }
  StaticModel m;
  m.freq.reserve(size_t(count) + 1);
  uint32_t assigned = 0;
  for (uint8_t w : weights) {
    uint32_t f = uint32_t((uint64_t(w) * rank_scale) / weight_sum);
    if (f == 0) throw FormatError("rbrc: rank symbol has zero probability");
    m.freq.push_back(f);
    assigned += f;
  }
  m.freq[0] += rank_scale - assigned;  // integer division must not leave a hole
  m.freq.push_back(uint32_t(escape));  // escape is the last rank-model symbol
  m.total = kScale;
  return m;
}

size_t alphabet_cap(int level) {
  if (level <= 3) return 8;
  if (level <= 6) return 16;
  return 32;
}

uint16_t clamp_probability(uint64_t value, uint64_t max_value) {
  if (value < 1) return 1;
  if (value > max_value) return uint16_t(max_value);
  return uint16_t(value);
}

class RbRcAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int level) const override {
    put_varint(out, in.size());
    if (in.empty()) return;

    std::array<uint64_t, 256> histogram{};
    for (uint8_t b : in) ++histogram[size_t(b)];

    std::vector<int> symbols;
    for (int s = 0; s < 256; ++s) {
      if (histogram[size_t(s)] != 0) symbols.push_back(s);
    }
    std::sort(symbols.begin(), symbols.end(), [&](int a, int b) {
      if (histogram[size_t(a)] != histogram[size_t(b)])
        return histogram[size_t(a)] > histogram[size_t(b)];
      return a < b;
    });
    const size_t n = std::min(alphabet_cap(level), symbols.size());

    Bytes alphabet;
    Bytes weights;
    uint64_t max_frequency = histogram[size_t(symbols[0])];
    for (size_t i = 0; i < n; ++i) {
      const uint8_t s = uint8_t(symbols[i]);
      alphabet.push_back(s);
      uint64_t scaled = 1 + (histogram[size_t(s)] * 254u) / max_frequency;
      weights.push_back(uint8_t(scaled));
    }

    uint64_t transitions = 0;
    uint64_t outside = 0;
    std::array<bool, 256> in_alphabet{};
    for (uint8_t s : alphabet) in_alphabet[size_t(s)] = true;
    for (size_t i = 0; i < in.size(); ++i) {
      if (i != 0 && in[i] != in[i - 1]) ++transitions;
      if (!in_alphabet[size_t(in[i])]) ++outside;
    }

    const uint64_t denominator = in.size() > 1 ? in.size() - 1 : 1;
    const uint16_t p1 = clamp_probability((transitions * (kScale - 1u)) / denominator,
                                           kScale - 1u);
    const uint64_t max_escape = kScale - n - 1;
    const uint16_t escape = clamp_probability((outside * max_escape) / in.size(), max_escape);

    put_u16(out, uint16_t(n));
    out.insert(out.end(), alphabet.begin(), alphabet.end());
    out.insert(out.end(), weights.begin(), weights.end());
    put_u16(out, p1);
    put_u16(out, escape);

    const StaticModel mask = binary_model(p1);
    const StaticModel ranks = rank_model(weights, escape);
    const StaticModel literals = uniform_model(256);
    std::array<uint16_t, 256> rank_of{};
    rank_of.fill(uint16_t(n));  // escape by default
    for (size_t i = 0; i < n; ++i) rank_of[size_t(alphabet[i])] = uint16_t(i);

    RangeEncoder enc(out);
    for (size_t i = 0; i < in.size(); ++i) {
      const uint8_t b = in[i];
      const bool changed = (i == 0) || b != in[i - 1];
      if (i != 0) encode(enc, mask, changed ? 1u : 0u);
      if (!changed) continue;
      const uint32_t rank = rank_of[size_t(b)];
      encode(enc, ranks, rank);
      if (rank == n) {
        const uint32_t value = b;
        encode(enc, literals, value);
      }
    }
    enc.flush();
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("rbrc: truncated length prefix");
    if (declared != orig_size) throw FormatError("rbrc: length prefix disagrees with the archive");
    if (orig_size == 0) {
      out.clear();
      return;
    }
    if (orig_size > (1u << 30)) throw FormatError("rbrc: implausible block size");

    uint16_t n16 = 0;
    if (!get_u16(in, pos, n16)) throw FormatError("rbrc: missing alphabet size");
    if (n16 == 0 || n16 > 256) throw FormatError("rbrc: invalid alphabet size");
    if (in.size() - pos < size_t(n16) * 2) throw FormatError("rbrc: truncated alphabet");

    Bytes alphabet(in.begin() + std::ptrdiff_t(pos),
                   in.begin() + std::ptrdiff_t(pos + size_t(n16)));
    pos += size_t(n16);
    Bytes weights(in.begin() + std::ptrdiff_t(pos),
                  in.begin() + std::ptrdiff_t(pos + size_t(n16)));
    pos += size_t(n16);

    std::array<bool, 256> seen{};
    for (uint8_t b : alphabet) {
      if (seen[size_t(b)]) throw FormatError("rbrc: duplicate alphabet symbol");
      seen[size_t(b)] = true;
    }

    uint16_t p1 = 0;
    uint16_t escape = 0;
    if (!get_u16(in, pos, p1) || !get_u16(in, pos, escape))
      throw FormatError("rbrc: truncated model header");
    if (p1 == 0 || escape == 0 || uint32_t(escape) >= kScale - size_t(n16))
      throw FormatError("rbrc: invalid model probability");

    const StaticModel mask = binary_model(p1);
    const StaticModel ranks = rank_model(weights, escape);
    const StaticModel literals = uniform_model(256);
    RangeDecoder dec(View(in.data() + pos, in.size() - pos));

    out.clear();
    out.reserve(size_t(orig_size));
    uint8_t previous = 0;
    for (uint64_t i = 0; i < orig_size; ++i) {
      uint8_t b = previous;
      bool changed = (i == 0);
      uint32_t rank = uint32_t(n16);
      if (i == 0) {
        rank = decode(dec, ranks);
      } else {
        const uint32_t bit = decode(dec, mask);
        changed = (bit == 1);
        if (changed) rank = decode(dec, ranks);
      }
      if (changed) {
        if (rank < size_t(n16)) {
          b = alphabet[size_t(rank)];
        } else if (rank == size_t(n16)) {
          b = uint8_t(decode(dec, literals));
        } else {
          throw FormatError("rbrc: invalid rank symbol");
        }
      }
      out.push_back(b);
      previous = b;
    }
  }

 private:
  AlgoParams p_{
      .id = "rbrc",
      .name = "Run-Boundary Rank Coder",
      .supported_types = {FileType::Binary, FileType::Text, FileType::Executable},
      .typical_ratio = 0.26,
      .typical_speed_mbps = 60.0,
      .memory_bytes = 1u << 20,
      .block_size = 1u << 21,
      .dictionary_size = 0,
      .notes = "transition-mask transform over a block-static ranked alphabet; short-run state streams",
  };
};

}  // namespace

const Algorithm* algo_rbrc() {
  static const RbRcAlgorithm instance;
  return &instance;
}

}  // namespace kolma
