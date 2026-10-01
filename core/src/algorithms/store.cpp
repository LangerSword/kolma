// store -- the raw passthrough. Used for data that will not shrink, and as the
// safety net when a real algorithm produces a larger block than it consumed.
#include "kolma/algorithm.hpp"

namespace kolma {
namespace {

class StoreAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    out.assign(in.begin(), in.end());
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    if (in.size() != orig_size)
      throw FormatError("store: block length does not match the recorded original size");
    out.assign(in.begin(), in.end());
  }

 private:
  AlgoParams p_{
      .id = "store",
      .name = "Store (raw)",
      .supported_types = all_file_types(),
      .typical_ratio = 1.0,
      .typical_speed_mbps = 3000.0,
      .memory_bytes = 0,
      .block_size = 1u << 30,
      .dictionary_size = 0,
      .notes = "no transform; the honest answer for entropy-coded input",
  };
};

}  // namespace

const Algorithm* algo_store() {
  static const StoreAlgorithm instance;
  return &instance;
}

}  // namespace kolma
