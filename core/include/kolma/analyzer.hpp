// kolma -- the Analyzer entity: look before you compress.
//
// It samples the input, measures entropy, runs the candidate algorithms over
// that sample for real (no guessing), and returns a recommendation plus the
// evidence behind it. Its second job is the veto: already-entropy-coded data is
// flagged for raw storage instead of being run through a method that cannot win.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/file.hpp"

namespace kolma {

enum class Priority : uint8_t { MaxRatio = 0, Balanced = 1, MaxSpeed = 2 };

const char* priority_name(Priority priority);
bool priority_from_name(std::string_view name, Priority& out);

// One candidate measured against the sample.
struct AlgoTrial {
  std::string algo_id;
  std::string algo_name;
  uint64_t sample_bytes = 0;
  uint64_t compressed_bytes = 0;
  double ratio = 1.0;
  double elapsed_ms = 0.0;
};

struct Analysis {
  FileInfo file;
  uint64_t sample_size = 0;
  double entropy = 0.0;          // bits per byte over the sample
  double compressibility = 0.0;  // 0 = incompressible, 1 = shrinks to nothing
  std::string recommended_algo = "store";
  int recommended_level = 6;
  bool store_raw = false;        // the veto: do not waste time, store as-is
  std::string rationale;
  std::vector<AlgoTrial> trials;
};

// `sample_size` caps how many bytes are inspected and trialled.
Analysis analyze_file(const std::string& path, Priority priority, uint64_t sample_size = 1u << 20);

// Maps a priority onto the level that expresses it.
int level_for_priority(Priority priority);

}  // namespace kolma
