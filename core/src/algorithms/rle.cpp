// rle -- run-length encoding. Byte runs only, so it shines on bitmaps, sparse
// masks and column data, and does nothing for text.
#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr uint8_t kMaxRun = 255;

class RleAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    put_varint(out, in.size());
    size_t i = 0;
    while (i < in.size()) {
      uint8_t byte = in[i];
      size_t run = 1;
      while (i + run < in.size() && in[i + run] == byte && run < kMaxRun) ++run;
      out.push_back(uint8_t(run));
      out.push_back(byte);
      i += run;
    }
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("rle: truncated length prefix");
    if (declared != orig_size) throw FormatError("rle: length prefix disagrees with the archive");
    out.clear();
    out.reserve(size_t(orig_size));
    while (out.size() < orig_size) {
      if (pos + 2 > in.size()) throw FormatError("rle: truncated run");
      uint8_t run = in[pos++];
      uint8_t byte = in[pos++];
      if (run == 0) throw FormatError("rle: zero-length run");
      if (out.size() + run > orig_size) throw FormatError("rle: run overruns the block");
      out.insert(out.end(), run, byte);
    }
  }

 private:
  AlgoParams p_{
      .id = "rle",
      .name = "Run-Length Encoding",
      .supported_types = {FileType::Image, FileType::Binary, FileType::Text, FileType::Executable},
      .typical_ratio = 0.55,
      .typical_speed_mbps = 900.0,
      .memory_bytes = 0,
      .block_size = 1u << 24,
      .dictionary_size = 0,
      .notes = "count/byte pairs; excellent on flat regions, useless on high-entropy data",
  };
};

}  // namespace

const Algorithm* algo_rle() {
  static const RleAlgorithm instance;
  return &instance;
}

}  // namespace kolma
