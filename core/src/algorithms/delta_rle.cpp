// delta-rle -- a delta filter followed by run-length coding.
//
// Built for data where neighbouring values differ by small amounts: sensor
// logs, 16/32-bit PCM, image planes, sorted numeric columns. The stride is
// chosen by measuring the entropy of the delta stream at strides 1, 2 and 4 and
// keeping the best.
//
// Block layout: varint(original length) | u8 stride | RLE stream of the deltas
#include <algorithm>
#include <array>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr uint8_t kMaxRun = 255;

Bytes delta_filter(View in, size_t stride) {
  Bytes out(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    uint8_t prev = i >= stride ? in[i - stride] : 0;
    out[i] = uint8_t(in[i] - prev);
  }
  return out;
}

void delta_unfilter(Bytes& data, size_t stride) {
  for (size_t i = stride; i < data.size(); ++i) data[i] = uint8_t(data[i] + data[i - stride]);
}

Bytes rle_encode(View in) {
  Bytes out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    uint8_t byte = in[i];
    size_t run = 1;
    while (i + run < in.size() && in[i + run] == byte && run < kMaxRun) ++run;
    out.push_back(uint8_t(run));
    out.push_back(byte);
    i += run;
  }
  return out;
}

Bytes rle_decode(View in, uint64_t expected) {
  Bytes out;
  out.reserve(size_t(expected));
  size_t pos = 0;
  while (out.size() < expected) {
    if (pos + 2 > in.size()) throw FormatError("delta-rle: truncated run");
    uint8_t run = in[pos++];
    uint8_t byte = in[pos++];
    if (run == 0) throw FormatError("delta-rle: zero-length run");
    if (out.size() + run > expected) throw FormatError("delta-rle: run overruns the block");
    out.insert(out.end(), run, byte);
  }
  return out;
}

class DeltaRleAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    put_varint(out, in.size());
    if (in.empty()) return;

    size_t best_stride = 1;
    double best_entropy = 9.0;
    Bytes best_deltas;
    for (size_t stride : {size_t(1), size_t(2), size_t(4)}) {
      Bytes d = delta_filter(in, stride);
      double h = shannon_entropy(View(d));
      if (h < best_entropy) {
        best_entropy = h;
        best_stride = stride;
        best_deltas = std::move(d);
      }
    }
    out.push_back(uint8_t(best_stride));
    Bytes rle = rle_encode(View(best_deltas));
    out.insert(out.end(), rle.begin(), rle.end());
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("delta-rle: truncated length prefix");
    if (declared != orig_size) throw FormatError("delta-rle: length prefix disagrees with the archive");
    if (orig_size == 0) {
      out.clear();
      return;
    }
    if (pos >= in.size()) throw FormatError("delta-rle: missing stride");
    uint8_t stride = in[pos++];
    if (stride != 1 && stride != 2 && stride != 4) throw FormatError("delta-rle: bad stride");
    out = rle_decode(View(in.data() + pos, in.size() - pos), orig_size);
    delta_unfilter(out, stride);
  }

 private:
  AlgoParams p_{
      .id = "delta-rle",
      .name = "Delta + RLE",
      .supported_types = {FileType::Text, FileType::Binary, FileType::Image, FileType::Audio},
      .typical_ratio = 0.35,
      .typical_speed_mbps = 400.0,
      .memory_bytes = 4u << 20,
      .block_size = 1u << 22,
      .dictionary_size = 0,
      .notes = "stride chosen by delta entropy; only pays off on smooth numeric data",
  };
};

}  // namespace

const Algorithm* algo_delta_rle() {
  static const DeltaRleAlgorithm instance;
  return &instance;
}

}  // namespace kolma