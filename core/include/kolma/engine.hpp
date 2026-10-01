// kolma -- the Engine entity: chunk the input, run the algorithm in parallel,
// merge, verify, and report progress, ratio and speed.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/analyzer.hpp"
#include "kolma/archive.hpp"

namespace kolma {

enum class Mode : uint8_t { Compress = 0, Decompress = 1, Benchmark = 2 };

const char* mode_name(Mode mode);
bool mode_from_name(std::string_view name, Mode& out);

// Everything the User decides before a job starts.
struct Job {
  Mode mode = Mode::Compress;
  Priority priority = Priority::Balanced;
  std::string algorithm = "auto";  // "auto" lets the Analyzer decide
  int level = 0;                   // 0 = derive from priority / recommendation
  std::string input;
  std::string output;  // empty = derived from the input name
  std::string password;
  size_t threads = 0;       // 0 = hardware concurrency
  size_t chunk_size = 0;    // 0 = derived from level, memory limit and the algorithm
  size_t memory_limit = 256u << 20;
  bool verify = true;
};

struct Progress {
  uint64_t bytes_in = 0;
  uint64_t bytes_out = 0;
  uint32_t chunks_done = 0;
  uint32_t chunks_total = 0;
  double elapsed_ms = 0.0;
  double throughput_mbps = 0.0;
  double ratio = 1.0;
};

struct Stats {
  std::string algorithm;
  std::string algorithm_name;
  int level = 0;
  uint64_t orig_size = 0;
  uint64_t comp_size = 0;
  double ratio = 1.0;
  double elapsed_ms = 0.0;
  double throughput_mbps = 0.0;
  uint32_t chunks = 0;
  size_t threads = 0;
  uint64_t chunk_size = 0;
  uint32_t orig_crc = 0;
  uint32_t restored_crc = 0;
  bool verified = false;
  bool restoring = false;  // true for decompression, so the report can say what was checked
  bool encrypted = false;
  std::string output_path;
};

struct BenchmarkRow {
  std::string algo_id;
  std::string algo_name;
  uint64_t input_bytes = 0;
  uint64_t output_bytes = 0;
  double ratio = 1.0;
  double elapsed_ms = 0.0;
  double throughput_mbps = 0.0;
  size_t memory_bytes = 0;
  uint64_t block_size = 0;
  bool best_ratio = false;
  bool best_speed = false;
};

struct Result {
  bool ok = false;
  std::string error;
  Stats stats;
  Analysis analysis;  // filled by compress jobs, including auto mode
  std::vector<BenchmarkRow> benchmark;
};

using ProgressFn = std::function<void(const Progress&)>;

// Runs one job. Progress is reported at most every ~40 ms plus a final call.
Result run_job(const Job& job, const ProgressFn& on_progress = nullptr);

// Compresses the input with every registered algorithm and reports the trade-off.
// `limit` caps how many bytes are benchmarked (0 = the whole file).
Result benchmark_file(const std::string& path, int level, size_t threads, uint64_t limit = 0,
                      const ProgressFn& on_progress = nullptr);

// Derived chunk size, clamped by the algorithm's preferred block size and the
// memory limit. Exposed so the CLI and the TUI can show what was decided.
size_t default_chunk_size(int level, size_t memory_limit, size_t threads, const Algorithm* algo);

// "<input>.kolma" for compression; for decompression the engine prefers the
// archive's recorded source name, so this is only the fallback.
std::string default_output_path(const std::string& input, Mode mode);

// Human-readable renderings shared by the CLI and the TUI's plain-text mode.
std::string format_stats(const Stats& stats);
std::string format_analysis(const Analysis& analysis);
std::string format_benchmark(const std::vector<BenchmarkRow>& rows);
std::string format_archive_info(const ArchiveInfo& info);

}  // namespace kolma
