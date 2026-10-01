#include "kolma/algorithm.hpp"

#include <algorithm>
#include <memory>

namespace kolma {

// Defined in core/src/algorithms/*.cpp
const Algorithm* algo_store();
const Algorithm* algo_rle();
const Algorithm* algo_huffman();
const Algorithm* algo_lz77();
const Algorithm* algo_lzw();
const Algorithm* algo_bwt_mtf_ari();
const Algorithm* algo_delta_rle();

namespace {

// The registry order is part of the on-disk format: never reorder, only append.
const std::vector<const Algorithm*>& registry() {
  static const std::vector<const Algorithm*> r = {
      algo_store(),  algo_rle(),    algo_huffman(), algo_lz77(),
      algo_lzw(),    algo_bwt_mtf_ari(), algo_delta_rle(),
  };
  return r;
}

}  // namespace

const std::vector<const Algorithm*>& all_algorithms() { return registry(); }

const Algorithm* find_algorithm(const std::string& id) {
  for (const Algorithm* a : registry()) {
    if (a->params().id == id) return a;
  }
  return nullptr;
}

const Algorithm* algorithm_by_index(uint8_t index) {
  if (index >= registry().size()) return nullptr;
  return registry()[index];
}

uint8_t algorithm_index(const std::string& id) {
  for (size_t i = 0; i < registry().size(); ++i) {
    if (registry()[i]->params().id == id) return uint8_t(i);
  }
  return 0xFF;
}

std::string algorithm_ids_csv() {
  std::string out;
  for (const Algorithm* a : registry()) {
    if (!out.empty()) out += ", ";
    out += a->params().id;
  }
  return out;
}

size_t algorithm_count() { return registry().size(); }

std::vector<FileType> all_file_types() {
  return {FileType::Unknown, FileType::Text,  FileType::Image, FileType::Audio,
          FileType::Video,   FileType::Executable, FileType::Archive, FileType::Binary};
}

int clamp_level(int level) { return std::clamp(level, 1, 9); }

}  // namespace kolma