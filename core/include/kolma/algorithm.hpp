// kolma -- the Algorithm entity: a pluggable, self-describing compression method.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "kolma/file.hpp"

namespace kolma {

// Thrown when a block or archive does not decode. The Engine catches this,
// attributes the failure to a chunk and reports it instead of crashing.
class FormatError : public std::runtime_error {
 public:
  explicit FormatError(const std::string& what) : std::runtime_error(what) {}
};

// Static, honest description of what an algorithm costs and what it is for.
struct AlgoParams {
  std::string id;                        // stable id, stored in archive headers
  std::string name;                      // human-readable name
  std::vector<FileType> supported_types; // where this method is worth trying
  double typical_ratio = 1.0;            // compressed/original on typical input
  double typical_speed_mbps = 0.0;       // single-thread throughput
  size_t memory_bytes = 0;               // working set for one block
  size_t block_size = 0;                 // largest block this method likes
  size_t dictionary_size = 0;            // 0 when the method has no dictionary
  std::string notes;
};

class Algorithm {
 public:
  virtual ~Algorithm() = default;
  virtual const AlgoParams& params() const = 0;
  // Compresses one self-contained block. `level` is 1..9; 1 is fastest.
  virtual void compress_block(View in, Bytes& out, int level) const = 0;
  // Decompresses one block. `orig_size` is the exact expected output length.
  virtual void decompress_block(View in, Bytes& out, uint64_t orig_size) const = 0;
};

// Registry, in a frozen order: the index is what goes into the archive header.
const std::vector<const Algorithm*>& all_algorithms();
const Algorithm* find_algorithm(const std::string& id);
const Algorithm* algorithm_by_index(uint8_t index);
uint8_t algorithm_index(const std::string& id);   // 0xFF when unknown
std::string algorithm_ids_csv();
size_t algorithm_count();
std::vector<FileType> all_file_types();
int clamp_level(int level);

}  // namespace kolma