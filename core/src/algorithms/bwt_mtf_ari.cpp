// bwt-mtf-ari -- Burrows-Wheeler transform, move-to-front, then an order-0
// adaptive arithmetic (range) coder.
//
// Block layout: varint(original length) | u32 primary index | range-coded stream
//
// The transform itself is what does the work: BWT gathers equal contexts
// together, MTF turns the resulting locality into a stream dominated by zeros,
// and the range coder spends under a bit on each of those zeros. This is the
// bzip2 pipeline without the second-stage Huffman.
#include <algorithm>
#include <array>
#include <numeric>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

// ---------------------------------------------------------- suffix array ----
// Cyclic-shift suffix array by prefix doubling with counting sort: O(n log n).
std::vector<int32_t> sort_cyclic_shifts(View s) {
  const int n = int(s.size());
  std::vector<int32_t> p(static_cast<size_t>(n)), c(static_cast<size_t>(n)), cn(static_cast<size_t>(n));
  std::vector<int32_t> cnt(std::max<size_t>(256, static_cast<size_t>(n)), 0);

  for (int i = 0; i < n; ++i) ++cnt[s[size_t(i)]];
  for (int i = 1; i < 256; ++i) cnt[size_t(i)] += cnt[size_t(i - 1)];
  for (int i = 0; i < n; ++i) p[size_t(--cnt[s[size_t(i)]])] = i;

  c[size_t(p[0])] = 0;
  int classes = 1;
  for (int i = 1; i < n; ++i) {
    if (s[size_t(p[size_t(i)])] != s[size_t(p[size_t(i - 1)])]) ++classes;
    c[size_t(p[size_t(i)])] = classes - 1;
  }

  std::vector<int32_t> pn(static_cast<size_t>(n));
  for (int h = 0; (1 << h) < n; ++h) {
    const int step = 1 << h;
    for (int i = 0; i < n; ++i) {
      pn[size_t(i)] = p[size_t(i)] - step;
      if (pn[size_t(i)] < 0) pn[size_t(i)] += n;
    }
    std::fill(cnt.begin(), cnt.begin() + classes, 0);
    for (int i = 0; i < n; ++i) ++cnt[size_t(c[size_t(pn[size_t(i)])])];
    for (int i = 1; i < classes; ++i) cnt[size_t(i)] += cnt[size_t(i - 1)];
    for (int i = n - 1; i >= 0; --i) p[size_t(--cnt[size_t(c[size_t(pn[size_t(i)])])])] = pn[size_t(i)];

    cn[size_t(p[0])] = 0;
    classes = 1;
    for (int i = 1; i < n; ++i) {
      std::pair<int32_t, int32_t> cur{c[size_t(p[size_t(i)])], c[size_t((p[size_t(i)] + step) % n)]};
      std::pair<int32_t, int32_t> prev{c[size_t(p[size_t(i - 1)])],
                                       c[size_t((p[size_t(i - 1)] + step) % n)]};
      if (cur != prev) ++classes;
      cn[size_t(p[size_t(i)])] = classes - 1;
    }
    c.swap(cn);
  }
  return p;
}

// Returns the primary index; `bwt` receives the last column.
int32_t bwt_forward(View in, Bytes& bwt) {
  const size_t n = in.size();
  bwt.resize(n);
  auto p = sort_cyclic_shifts(in);
  int32_t primary = 0;
  for (size_t i = 0; i < n; ++i) {
    bwt[i] = in[(size_t(p[i]) + n - 1) % n];
    if (p[i] == 0) primary = int32_t(i);
  }
  return primary;
}

void bwt_inverse(View bwt, int32_t primary, Bytes& out) {
  const size_t n = bwt.size();
  out.resize(n);
  if (n == 0) return;
  if (primary < 0 || size_t(primary) >= n) throw FormatError("bwt: primary index out of range");

  std::array<uint32_t, 256> start{};
  for (uint8_t b : bwt) ++start[b];
  uint32_t running = 0;
  for (auto& c : start) {
    uint32_t count = c;
    c = running;
    running += count;
  }
  std::vector<int32_t> lf(n);
  for (size_t i = 0; i < n; ++i) lf[i] = int32_t(start[bwt[i]]++);

  int32_t row = primary;
  for (size_t pos = n; pos-- > 0;) {
    out[pos] = bwt[size_t(row)];
    row = lf[size_t(row)];
  }
}

// ----------------------------------------------------------- range coder ----
class RangeEncoder {
 public:
  explicit RangeEncoder(Bytes& out) : out_(out) {}

  void encode(uint32_t start, uint32_t size, uint32_t total) {
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
    // The shift must wrap in 32-bit arithmetic, exactly as the LZMA range coder
    // does: widening to 64 bits first would let the carry grow past one bit.
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
  uint8_t read_byte() { return pos_ < in_.size() ? in_[pos_++] : 0; }

  View in_;
  size_t pos_ = 0;
  uint32_t code_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
};

// Order-0 adaptive model over the 256 byte values, initialised uniform.
struct Order0Model {
  std::array<uint32_t, 256> freq{};
  uint32_t total = 0;

  Order0Model() {
    freq.fill(1);
    total = 256;
  }

  void encode(RangeEncoder& enc, uint8_t sym) {
    uint32_t start = 0;
    for (int i = 0; i < sym; ++i) start += freq[size_t(i)];
    enc.encode(start, freq[sym], total);
    bump(sym);
  }

  uint8_t decode(RangeDecoder& dec) {
    uint32_t value = dec.get_freq(total);
    uint32_t start = 0;
    uint8_t sym = 0;
    for (int i = 0; i < 256; ++i) {
      if (start + freq[size_t(i)] > value) {
        sym = uint8_t(i);
        break;
      }
      start += freq[size_t(i)];
    }
    dec.update(start, freq[sym]);
    bump(sym);
    return sym;
  }

 private:
  void bump(uint8_t sym) {
    ++freq[sym];
    if (++total > (1u << 15)) rescale();
  }

  void rescale() {
    total = 0;
    for (auto& f : freq) {
      f = (f + 1) / 2;
      total += f;
    }
  }
};

class BwtMtfAriAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    put_varint(out, in.size());
    if (in.empty()) return;

    Bytes bwt;
    int32_t primary = bwt_forward(in, bwt);
    put_u32(out, uint32_t(primary));

    // Move-to-front: the BWT output has strong local repetition, so the index
    // stream is mostly small numbers, and mostly zero.
    std::array<uint8_t, 256> mtf{};
    std::iota(mtf.begin(), mtf.end(), uint8_t(0));

    RangeEncoder enc(out);
    Order0Model model;
    for (uint8_t b : bwt) {
      int index = 0;
      while (mtf[size_t(index)] != b) ++index;
      model.encode(enc, uint8_t(index));
      for (int j = index; j > 0; --j) mtf[size_t(j)] = mtf[size_t(j - 1)];
      mtf[0] = b;
    }
    enc.flush();
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("bwt: truncated length prefix");
    if (declared != orig_size) throw FormatError("bwt: length prefix disagrees with the archive");
    if (orig_size == 0) {
      out.clear();
      return;
    }
    uint32_t primary = 0;
    if (!get_u32(in, pos, primary)) throw FormatError("bwt: missing primary index");

    RangeDecoder dec(View(in.data() + pos, in.size() - pos));
    Order0Model model;
    std::array<uint8_t, 256> mtf{};
    std::iota(mtf.begin(), mtf.end(), uint8_t(0));

    Bytes bwt(static_cast<size_t>(orig_size));
    for (size_t i = 0; i < orig_size; ++i) {
      uint8_t index = model.decode(dec);
      uint8_t b = mtf[index];
      bwt[i] = b;
      for (int j = index; j > 0; --j) mtf[size_t(j)] = mtf[size_t(j - 1)];
      mtf[0] = b;
    }
    bwt_inverse(bwt, int32_t(primary), out);
  }

 private:
  AlgoParams p_{
      .id = "bwt-mtf-ari",
      .name = "BWT + MTF + Arithmetic",
      .supported_types = {FileType::Text, FileType::Binary},
      .typical_ratio = 0.30,
      .typical_speed_mbps = 6.0,
      .memory_bytes = 16u << 20,
      .block_size = 1u << 18,  // 256 KiB: the suffix sort is superlinear in time
      .dictionary_size = 0,
      .notes = "best ratio on text, slowest method here; block size is capped by design",
  };
};

}  // namespace

const Algorithm* algo_bwt_mtf_ari() {
  static const BwtMtfAriAlgorithm instance;
  return &instance;
}

}  // namespace kolma
