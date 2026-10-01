#include "kolma/engine.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace kolma {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(const Clock::time_point& start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double rate_mbps(uint64_t bytes, double elapsed_ms) {
  if (elapsed_ms <= 0.0) return 0.0;
  return double(bytes) / (elapsed_ms / 1000.0) / 1e6;
}

// Small RAII wrapper so the read path can use pread from several threads
// without sharing a file position.
class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(const std::string& path) : fd_(::open(path.c_str(), O_RDONLY)) {}
  ~FileDescriptor() { close(); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  bool valid() const { return fd_ >= 0; }
  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  bool read_exact(uint64_t offset, uint8_t* dst, size_t len) const {
    size_t done = 0;
    while (done < len) {
      ssize_t got = ::pread(fd_, dst + done, len - done, off_t(offset + done));
      if (got <= 0) return false;
      done += size_t(got);
    }
    return true;
  }

 private:
  int fd_ = -1;
};

// Runs `produce(i)` on `threads` workers and hands the results to `consume(i)`
// strictly in index order. At most `threads + lookahead` results exist at once,
// which is what keeps a 40 GiB input inside the memory limit.
class OrderedPipeline {
 public:
  using ProduceFn = std::function<void(size_t index, Bytes& out, uint8_t& chunk_flags)>;
  using ConsumeFn = std::function<void(size_t index, Bytes& payload, uint8_t chunk_flags)>;

  // Returns the first error raised by any worker, or nullptr.
  static std::exception_ptr run(size_t count, size_t threads, size_t lookahead,
                                const ProduceFn& produce, const ConsumeFn& consume) {
    if (count == 0) return nullptr;
    threads = std::max<size_t>(1, std::min(threads, count));

    std::mutex m;
    std::condition_variable cv_ready;
    std::condition_variable cv_slot;
    std::vector<Bytes> slots(count);
    std::vector<uint8_t> slot_flags(count, 0);
    std::vector<bool> ready(count, false);
    size_t job_pos = 0;
    size_t write_pos = 0;
    bool stop = false;
    std::exception_ptr first_error;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (size_t t = 0; t < threads; ++t) {
      pool.emplace_back([&]() {
        for (;;) {
          size_t index = 0;
          {
            std::unique_lock<std::mutex> lock(m);
            cv_slot.wait(lock, [&]() {
              return stop || job_pos >= count || job_pos < write_pos + lookahead;
            });
            if (stop || job_pos >= count) return;
            index = job_pos++;
          }
          Bytes out;
          uint8_t flags = 0;
          std::exception_ptr err;
          try {
            produce(index, out, flags);
          } catch (...) {
            err = std::current_exception();
          }
          {
            std::lock_guard<std::mutex> lock(m);
            if (err) {
              if (!first_error) first_error = err;
              stop = true;
              cv_ready.notify_all();
              cv_slot.notify_all();
              return;
            }
            slots[index] = std::move(out);
            slot_flags[index] = flags;
            ready[index] = true;
          }
          cv_ready.notify_all();
        }
      });
    }

    for (size_t i = 0; i < count; ++i) {
      Bytes payload;
      uint8_t flags = 0;
      {
        std::unique_lock<std::mutex> lock(m);
        cv_ready.wait(lock, [&]() { return ready[i] || stop; });
        if (!ready[i]) break;  // a worker failed: give up on the rest
        payload = std::move(slots[i]);
        flags = slot_flags[i];
        slots[i].clear();
        slots[i].shrink_to_fit();
        ready[i] = false;
      }
      consume(i, payload, flags);
      {
        std::lock_guard<std::mutex> lock(m);
        write_pos = i + 1;
      }
      cv_slot.notify_all();
    }

    for (std::thread& th : pool) th.join();
    return first_error;
  }
};

std::string exception_text(const std::exception_ptr& err) {
  if (!err) return {};
  try {
    std::rethrow_exception(err);
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown error";
  }
}

bool same_file(const std::string& a, const std::string& b) {
  std::error_code ec;
  auto ca = std::filesystem::weakly_canonical(a, ec);
  auto cb = std::filesystem::weakly_canonical(b, ec);
  if (!ec && ca == cb) return true;
  return a == b;
}

std::string strip_kolma_suffix(const std::string& path) {
  constexpr std::string_view suffix = ".kolma";
  if (path.size() > suffix.size() &&
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return path.substr(0, path.size() - suffix.size());
  }
  return {};
}

// Loads a whole archive header, growing the read when the chunk table is large.
bool load_header(const std::string& path, ArchiveHeader& header, size_t& payload_offset,
                 std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  Bytes buffer(64 * 1024);
  in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
  buffer.resize(size_t(in.gcount()));
  if (!parse_header(View(buffer), header, payload_offset, error)) return false;
  if (payload_offset > buffer.size()) {
    buffer.resize(payload_offset);
    in.clear();
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
    buffer.resize(size_t(in.gcount()));
    if (!parse_header(View(buffer), header, payload_offset, error)) return false;
  }
  return true;
}

struct ProgressReporter {
  const ProgressFn* fn;
  Clock::time_point start;
  Clock::time_point last = Clock::now();
  bool enabled = false;

  void report(const Progress& p) {
    if (!enabled || !*fn) return;
    auto now = Clock::now();
    if (std::chrono::duration<double, std::milli>(now - last).count() < 40.0) return;
    last = now;
    (*fn)(p);
  }
  void final_report(const Progress& p) {
    if (!enabled || !*fn) return;
    (*fn)(p);
  }
};

// ------------------------------------------------------------- compression --
Result run_compress(const Job& job, const ProgressFn& on_progress) {
  Result r;
  FileInfo info = inspect_file(job.input);
  if (!info.exists) {
    r.error = info.error.empty() ? ("cannot read " + job.input) : info.error;
    return r;
  }

  std::string algo_id = job.algorithm;
  int level = job.level;
  const bool auto_mode = algo_id.empty() || algo_id == "auto";

  if (auto_mode) {
    r.analysis = analyze_file(job.input, job.priority);
    algo_id = r.analysis.store_raw ? "store" : r.analysis.recommended_algo;
    if (level <= 0) level = r.analysis.recommended_level;
  } else {
    r.analysis.file = info;
    r.analysis.recommended_algo = algo_id;
    r.analysis.recommended_level = job.level > 0 ? job.level : level_for_priority(job.priority);
    r.analysis.entropy = info.entropy;
    r.analysis.sample_size = info.size;
    r.analysis.rationale = "algorithm chosen by the caller; the Analyzer was not consulted";
    r.analysis.store_raw = algo_id == "store";
    if (level <= 0) level = level_for_priority(job.priority);
  }

  const Algorithm* algo = find_algorithm(algo_id);
  if (algo == nullptr) {
    r.error = "unknown algorithm '" + algo_id + "'; known: " + algorithm_ids_csv();
    return r;
  }
  level = clamp_level(level);

  const size_t threads =
      job.threads > 0 ? job.threads : std::max<size_t>(1, std::thread::hardware_concurrency());
  size_t chunk_size =
      job.chunk_size > 0 ? job.chunk_size : default_chunk_size(level, job.memory_limit, threads, algo);
  chunk_size = std::max<size_t>(chunk_size, 1);
  uint32_t chunk_count = uint32_t((info.size + chunk_size - 1) / chunk_size);
  if (chunk_count == 0) chunk_count = 1;

  const std::string output =
      job.output.empty() ? default_output_path(job.input, Mode::Compress) : job.output;
  if (same_file(output, job.input)) {
    r.error = "refusing to write the archive over its own input (" + output + ")";
    return r;
  }

  ArchiveHeader header;
  header.algo_index = algorithm_index(algo_id);
  header.level = uint8_t(level);
  header.file_type = uint8_t(info.type);
  header.orig_size = info.size;
  header.created = now_unix();
  header.chunk_size = chunk_size;
  header.chunk_count = chunk_count;
  header.source_name = info.name;
  header.type_detail = info.type_detail;
  header.chunks.assign(chunk_count, ChunkRecord{});

  Bytes key;
  const bool encrypt = !job.password.empty();
  if (encrypt) {
    header.flags |= kArchiveEncrypted;
    header.kdf_iterations = kDefaultKdfIterations;
    std::random_device rd;
    for (uint8_t& b : header.salt) b = uint8_t(rd());
    for (uint8_t& b : header.nonce) b = uint8_t(rd());
    key = derive_key(job.password, header);
  }

  FileDescriptor source(job.input);
  if (!source.valid()) {
    r.error = "cannot open " + job.input;
    return r;
  }

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    r.error = "cannot write " + output;
    return r;
  }
  Bytes header_bytes = serialize_header(header);
  out.write(reinterpret_cast<const char*>(header_bytes.data()), std::streamsize(header_bytes.size()));

  ProgressReporter reporter{&on_progress, Clock::now()};
  reporter.enabled = true;
  Progress progress;
  progress.chunks_total = chunk_count;

  uint64_t bytes_in = 0;
  uint64_t bytes_out = 0;
  uint32_t payload_crc = 0;
  uint32_t orig_crc = 0;
  const auto start = Clock::now();
  const size_t lookahead = threads + 4;
  std::string write_error;

  std::exception_ptr err = OrderedPipeline::run(
      chunk_count, threads, lookahead,
      [&](size_t index, Bytes& produced, uint8_t& chunk_flags) {
        const uint64_t offset = uint64_t(index) * chunk_size;
        const size_t want = size_t(info.size > offset ? std::min<uint64_t>(chunk_size, info.size - offset) : 0);
        Bytes raw(want);
        if (want > 0 && !source.read_exact(offset, raw.data(), want))
          throw FormatError("short read while chunking the input");
        algo->compress_block(View(raw), produced, level);
        // Per-chunk fallback: never store a block that grew.
        if (produced.size() >= raw.size()) {
          produced = std::move(raw);
          chunk_flags |= kChunkStoredRaw;
        }
        if (encrypt && !produced.empty()) {
          auto nonce = nonce_for_chunk(header, uint32_t(index));
          chacha20_xor_inplace(View(key), 1, View(nonce), produced.data(), produced.size());
        }
      },
      [&](size_t index, Bytes& payload, uint8_t chunk_flags) {
        const uint64_t offset = uint64_t(index) * chunk_size;
        const size_t want = size_t(info.size > offset ? std::min<uint64_t>(chunk_size, info.size - offset) : 0);
        if (want > 0) {
          Bytes original(want);
          if (!source.read_exact(offset, original.data(), want))
            throw FormatError("short read while checksumming the input");
          orig_crc = crc32(View(original), orig_crc);
        }
        header.chunks[index].orig_size = want;
        header.chunks[index].comp_size = payload.size();
        header.chunks[index].flags = chunk_flags;
        if (!payload.empty()) {
          out.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size()));
          payload_crc = crc32(View(payload), payload_crc);
        }
        if (!out) {
          write_error = "write failed at chunk " + std::to_string(index) +
                        " (out of disk space?)";
          throw FormatError(write_error);
        }
        bytes_in += want;
        bytes_out += payload.size();
        progress.bytes_in = bytes_in;
        progress.bytes_out = bytes_out;
        progress.chunks_done = uint32_t(index) + 1;
        progress.elapsed_ms = ms_since(start);
        progress.throughput_mbps = rate_mbps(bytes_in, progress.elapsed_ms);
        progress.ratio = bytes_in > 0 ? double(bytes_out) / double(bytes_in) : 1.0;
        reporter.report(progress);
      });

  if (err) {
    out.close();
    std::filesystem::remove(output);
    r.error = exception_text(err);
    if (r.error.empty()) r.error = "compression failed";
    return r;
  }

  header.comp_size = bytes_out;
  header.orig_crc = orig_crc;
  header.payload_crc = payload_crc;
  Bytes final_header = serialize_header(header);
  if (final_header.size() != header_bytes.size()) {
    r.error = "internal error: header size changed while finalising";
    return r;
  }
  out.seekp(0);
  out.write(reinterpret_cast<const char*>(final_header.data()),
            std::streamsize(final_header.size()));
  out.flush();
  if (!out) {
    r.error = "failed to finalise " + output;
    return r;
  }
  out.close();

  const double elapsed = ms_since(start);
  progress.elapsed_ms = elapsed;
  progress.throughput_mbps = rate_mbps(bytes_in, elapsed);
  reporter.final_report(progress);

  r.ok = true;
  r.stats.algorithm = algo_id;
  r.stats.algorithm_name = algo->params().name;
  r.stats.level = level;
  r.stats.orig_size = info.size;
  r.stats.comp_size = bytes_out;
  r.stats.ratio = info.size > 0 ? double(bytes_out) / double(info.size) : 1.0;
  r.stats.elapsed_ms = elapsed;
  r.stats.throughput_mbps = rate_mbps(info.size, elapsed);
  r.stats.chunks = chunk_count;
  r.stats.threads = threads;
  r.stats.chunk_size = chunk_size;
  r.stats.orig_crc = orig_crc;
  r.stats.verified = true;  // written from the same bytes that produced the checksum
  r.stats.restoring = false;
  r.stats.encrypted = encrypt;
  r.stats.output_path = output;
  return r;
}

// ----------------------------------------------------------- decompression --
Result run_decompress(const Job& job, const ProgressFn& on_progress) {
  Result r;
  ArchiveHeader header;
  size_t payload_offset = 0;
  std::string error;
  if (!load_header(job.input, header, payload_offset, error)) {
    r.error = error;
    return r;
  }

  const bool encrypted = archive_encrypted(header);
  if (encrypted && job.password.empty()) {
    r.error = "archive is encrypted; a password is required";
    return r;
  }
  Bytes key;
  if (encrypted) key = derive_key(job.password, header);

  const Algorithm* algo = algorithm_by_index(header.algo_index);
  if (algo == nullptr && header.chunk_count > 0) {
    // A fully raw archive is still readable even if the id is unknown.
    bool all_raw = true;
    for (const ChunkRecord& c : header.chunks) {
      if ((c.flags & kChunkStoredRaw) == 0) all_raw = false;
    }
    if (!all_raw) {
      r.error = "archive names algorithm id " + std::to_string(header.algo_index) +
                ", which this build does not implement";
      return r;
    }
  }

  std::string output = job.output;
  if (output.empty()) {
    output = strip_kolma_suffix(job.input);
    if (output.empty()) {
      std::string name = header.source_name;
      bool safe_name = !name.empty() && name.find('/') == std::string::npos &&
                       name != "." && name != "..";
      if (safe_name) {
        output = (std::filesystem::path(job.input).parent_path() / name).string();
      } else {
        output = job.input + ".out";
      }
    }
  }
  if (same_file(output, job.input)) {
    r.error = "refusing to write the restored file over the archive (" + output + ")";
    return r;
  }

  // Payload offsets: chunk i starts at the sum of the compressed sizes before it.
  std::vector<uint64_t> offsets(header.chunks.size() + 1, 0);
  for (size_t i = 0; i < header.chunks.size(); ++i) {
    offsets[i + 1] = offsets[i] + header.chunks[i].comp_size;
  }

  FileDescriptor source(job.input);
  if (!source.valid()) {
    r.error = "cannot open " + job.input;
    return r;
  }

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    r.error = "cannot write " + output;
    return r;
  }

  const size_t threads =
      job.threads > 0 ? job.threads : std::max<size_t>(1, std::thread::hardware_concurrency());
  const size_t lookahead = threads + 4;

  ProgressReporter reporter{&on_progress, Clock::now()};
  reporter.enabled = true;
  Progress progress;
  progress.chunks_total = header.chunk_count;

  uint64_t bytes_in = 0;
  uint64_t bytes_out = 0;
  uint32_t restored_crc = 0;
  std::string write_error;
  const auto start = Clock::now();

  std::exception_ptr err = OrderedPipeline::run(
      header.chunks.size(), threads, lookahead,
      [&](size_t index, Bytes& produced, uint8_t& chunk_flags) {
        const ChunkRecord& rec = header.chunks[index];
        Bytes payload(size_t(rec.comp_size));
        if (rec.comp_size > 0 &&
            !source.read_exact(uint64_t(payload_offset) + offsets[index], payload.data(),
                               payload.size())) {
          throw FormatError("short read on chunk " + std::to_string(index));
        }
        if (encrypted && !payload.empty()) {
          auto nonce = nonce_for_chunk(header, uint32_t(index));
          chacha20_xor_inplace(View(key), 1, View(nonce), payload.data(), payload.size());
        }
        if ((rec.flags & kChunkStoredRaw) != 0) {
          produced = std::move(payload);
        } else if (algo != nullptr) {
          algo->decompress_block(View(payload), produced, rec.orig_size);
        } else {
          throw FormatError("chunk " + std::to_string(index) + " needs an algorithm this build lacks");
        }
        if (produced.size() != rec.orig_size) {
          throw FormatError("chunk " + std::to_string(index) + " restored " +
                            std::to_string(produced.size()) + " bytes, header says " +
                            std::to_string(rec.orig_size));
        }
        chunk_flags = 0;
      },
      [&](size_t index, Bytes& payload, uint8_t) {
        if (!payload.empty()) {
          out.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size()));
          restored_crc = crc32(View(payload), restored_crc);
        }
        if (!out) {
          write_error = "write failed at chunk " + std::to_string(index) + " (out of disk space?)";
          throw FormatError(write_error);
        }
        bytes_in += header.chunks[index].comp_size;
        bytes_out += payload.size();
        progress.bytes_in = bytes_in;
        progress.bytes_out = bytes_out;
        progress.chunks_done = uint32_t(index) + 1;
        progress.elapsed_ms = ms_since(start);
        progress.throughput_mbps = rate_mbps(bytes_out, progress.elapsed_ms);
        progress.ratio = bytes_out > 0 ? double(bytes_in) / double(bytes_out) : 1.0;
        reporter.report(progress);
      });

  if (err) {
    out.close();
    std::filesystem::remove(output);
    r.error = exception_text(err);
    if (r.error.empty()) r.error = "decompression failed";
    return r;
  }
  out.flush();
  if (!out) {
    r.error = "failed to write " + output;
    return r;
  }
  out.close();

  const double elapsed = ms_since(start);
  progress.elapsed_ms = elapsed;
  reporter.final_report(progress);

  r.stats.algorithm = algo ? algo->params().id : "store";
  r.stats.algorithm_name = algo ? algo->params().name : "Store (raw)";
  r.stats.level = header.level;
  r.stats.orig_size = header.orig_size;
  r.stats.comp_size = header.comp_size;
  r.stats.ratio = header.orig_size > 0 ? double(header.comp_size) / double(header.orig_size) : 1.0;
  r.stats.elapsed_ms = elapsed;
  r.stats.throughput_mbps = rate_mbps(bytes_out, elapsed);
  r.stats.chunks = header.chunk_count;
  r.stats.threads = threads;
  r.stats.chunk_size = header.chunk_size;
  r.stats.orig_crc = header.orig_crc;
  r.stats.restored_crc = restored_crc;
  r.stats.restoring = true;
  r.stats.encrypted = encrypted;
  r.stats.output_path = output;

  if (bytes_out != header.orig_size) {
    r.error = "restored " + std::to_string(bytes_out) + " bytes but the header records " +
              std::to_string(header.orig_size);
    return r;
  }
  r.stats.verified = (restored_crc == header.orig_crc);
  if (job.verify && !r.stats.verified) {
    r.error = "checksum mismatch: the restored data does not match the original (" +
              crc32_hex(restored_crc) + " vs " + crc32_hex(header.orig_crc) +
              "); the output file was left in place for inspection";
    return r;
  }
  r.ok = true;
  return r;
}

// ------------------------------------------------------------- benchmark ----
Result run_benchmark(const Job& job, uint64_t limit, const ProgressFn& on_progress) {
  Result r;
  FileInfo info = inspect_file(job.input);
  if (!info.exists) {
    r.error = info.error.empty() ? ("cannot read " + job.input) : info.error;
    return r;
  }
  const uint64_t want = limit == 0 ? info.size : std::min<uint64_t>(limit, info.size);
  Bytes sample(static_cast<size_t>(want));
  {
    FileDescriptor source(job.input);
    if (!source.valid()) {
      r.error = "cannot open " + job.input;
      return r;
    }
    if (want > 0 && !source.read_exact(0, sample.data(), size_t(want))) {
      r.error = "short read while loading the benchmark sample";
      return r;
    }
  }

  const size_t threads =
      job.threads > 0 ? job.threads : std::max<size_t>(1, std::thread::hardware_concurrency());
  const int level = clamp_level(job.level > 0 ? job.level : level_for_priority(job.priority));

  Progress progress;
  progress.chunks_total = uint32_t(algorithm_count());
  uint32_t done = 0;

  for (const Algorithm* algo : all_algorithms()) {
    const size_t block = std::max<size_t>(
        4096, std::min<size_t>(algo->params().block_size, std::max<size_t>(sample.size(), 4096)));
    const size_t count = sample.empty() ? 1 : (sample.size() + block - 1) / block;

    BenchmarkRow row;
    row.algo_id = algo->params().id;
    row.algo_name = algo->params().name;
    row.input_bytes = sample.size();
    row.memory_bytes = algo->params().memory_bytes;
    row.block_size = block;

    uint64_t produced_total = 0;
    const auto start = Clock::now();
    std::exception_ptr err = OrderedPipeline::run(
        count, threads, threads + 4,
        [&](size_t index, Bytes& produced, uint8_t& chunk_flags) {
          const size_t offset = index * block;
          const size_t len = std::min(block, sample.size() - std::min(offset, sample.size()));
          View slice(sample.data() + offset, len);
          algo->compress_block(slice, produced, level);
          if (produced.size() >= len) {
            produced.assign(slice.begin(), slice.end());
            chunk_flags |= kChunkStoredRaw;
          }
        },
        [&](size_t, Bytes& payload, uint8_t) { produced_total += payload.size(); });
    if (err) {
      row.ratio = 1.0;
      row.throughput_mbps = 0.0;
    } else {
      row.output_bytes = produced_total;
      row.ratio = sample.empty() ? 1.0 : double(produced_total) / double(sample.size());
      row.elapsed_ms = ms_since(start);
      row.throughput_mbps = rate_mbps(sample.size(), row.elapsed_ms);
    }
    r.benchmark.push_back(row);

    ++done;
    progress.chunks_done = done;
    progress.bytes_in = sample.size();
    progress.elapsed_ms = ms_since(start);
    if (on_progress) on_progress(progress);
  }

  std::sort(r.benchmark.begin(), r.benchmark.end(),
            [](const BenchmarkRow& a, const BenchmarkRow& b) { return a.ratio < b.ratio; });
  if (!r.benchmark.empty()) {
    r.benchmark.front().best_ratio = true;
    const BenchmarkRow* fastest = &r.benchmark.front();
    for (const BenchmarkRow& b : r.benchmark) {
      if (b.throughput_mbps > fastest->throughput_mbps) fastest = &b;
    }
    // Point at the stored row, not the copy.
    for (BenchmarkRow& b : r.benchmark) {
      if (&b == fastest) b.best_speed = true;
    }
  }
  r.ok = true;
  r.stats.algorithm = "benchmark";
  r.stats.orig_size = sample.size();
  r.stats.level = level;
  r.stats.threads = threads;
  r.stats.output_path = job.input;
  return r;
}

}  // namespace

// ------------------------------------------------------------------ public --

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::Decompress: return "decompress";
    case Mode::Benchmark: return "benchmark";
    case Mode::Compress: default: return "compress";
  }
}

bool mode_from_name(std::string_view name, Mode& out) {
  if (name == "compress" || name == "c" || name == "pack") {
    out = Mode::Compress;
    return true;
  }
  if (name == "decompress" || name == "d" || name == "unpack" || name == "extract") {
    out = Mode::Decompress;
    return true;
  }
  if (name == "benchmark" || name == "bench" || name == "b") {
    out = Mode::Benchmark;
    return true;
  }
  return false;
}

Result run_job(const Job& job, const ProgressFn& on_progress) {
  switch (job.mode) {
    case Mode::Compress: return run_compress(job, on_progress);
    case Mode::Decompress: return run_decompress(job, on_progress);
    case Mode::Benchmark: return run_benchmark(job, 0, on_progress);
  }
  Result r;
  r.error = "unreachable mode";
  return r;
}

Result benchmark_file(const std::string& path, int level, size_t threads, uint64_t limit,
                      const ProgressFn& on_progress) {
  Job job;
  job.mode = Mode::Benchmark;
  job.input = path;
  job.level = level;
  job.threads = threads;
  return run_benchmark(job, limit, on_progress);
}

size_t default_chunk_size(int level, size_t memory_limit, size_t threads, const Algorithm* algo) {
  level = clamp_level(level);
  // 64 KiB at level 1, doubling per level, capped at 4 MiB.
  size_t chunk = size_t(64) << 10;
  for (int i = 1; i < level && chunk < (4u << 20); ++i) chunk <<= 1;
  chunk = std::min(chunk, size_t(4) << 20);
  if (algo != nullptr && algo->params().block_size > 0) {
    chunk = std::min(chunk, algo->params().block_size);
  }
  // Keep roughly three buffers per worker inside the memory limit.
  const size_t per_worker = std::max<size_t>(1, memory_limit / std::max<size_t>(1, threads) / 3);
  chunk = std::min(chunk, per_worker);
  chunk = std::max<size_t>(chunk, 4096);
  return chunk;
}

std::string default_output_path(const std::string& input, Mode mode) {
  if (mode == Mode::Compress) return input + ".kolma";
  return input + ".out";
}

std::string format_stats(const Stats& s) {
  std::ostringstream os;
  os << "algorithm   : " << s.algorithm << " (" << s.algorithm_name << ") at level " << s.level << "\n";
  os << "original    : " << human_size(s.orig_size) << "\n";
  os << "compressed  : " << human_size(s.comp_size) << "\n";
  os << "ratio       : " << format_ratio(s.ratio);
  if (s.orig_size > 0) {
    char pct[32];
    std::snprintf(pct, sizeof(pct), "%.1f", (1.0 - s.ratio) * 100.0);
    os << "  (" << pct << "% of the original saved)";
  }
  os << "\n";
  os << "throughput  : " << human_rate(s.throughput_mbps) << "\n";
  os << "elapsed     : " << s.elapsed_ms << " ms\n";
  os << "chunks      : " << s.chunks << " x " << human_size(s.chunk_size) << " on " << s.threads
     << " thread(s)\n";
  if (s.restoring) {
    os << "crc32       : " << crc32_hex(s.orig_crc) << " recorded, " << crc32_hex(s.restored_crc)
       << " restored  " << (s.verified ? "match" : "MISMATCH") << "\n";
  } else {
    os << "crc32       : " << crc32_hex(s.orig_crc) << " recorded in the header\n";
  }
  os << "encrypted   : " << (s.encrypted ? "yes (ChaCha20 + PBKDF2-HMAC-SHA256)" : "no") << "\n";
  if (!s.output_path.empty()) os << "output      : " << s.output_path << "\n";
  return os.str();
}

std::string format_analysis(const Analysis& a) {
  std::ostringstream os;
  os << "file        : " << a.file.path << "\n";
  os << "size        : " << human_size(a.file.size) << "\n";
  os << "type        : " << file_type_name(a.file.type) << " (" << a.file.type_detail << ")\n";
  os << "entropy     : " << a.entropy << " bits/byte\n";
  os << "sampled     : " << human_size(a.sample_size) << "\n";
  os << "compressible: " << (a.compressibility * 100.0) << "%\n";
  os << "recommended : " << a.recommended_algo << " at level " << a.recommended_level
     << (a.store_raw ? " (flagged incompressible, stored raw)" : "") << "\n";
  if (!a.trials.empty()) {
    os << "sample trials:\n";
    for (const AlgoTrial& t : a.trials) {
      os << "  " << t.algo_id << " -> ratio " << t.ratio << " in " << t.elapsed_ms << " ms\n";
    }
  }
  os << "why         : " << a.rationale << "\n";
  return os.str();
}

std::string format_benchmark(const std::vector<BenchmarkRow>& rows) {
  std::ostringstream os;
  os << "algorithm              ratio     size       throughput   memory    block\n";
  for (const BenchmarkRow& b : rows) {
    char line[256];
    std::snprintf(line, sizeof(line), "%-22s %-9.4f %-10s %-12s %-9s %s%s\n", b.algo_id.c_str(),
                  b.ratio, human_size(b.output_bytes).c_str(), human_rate(b.throughput_mbps).c_str(),
                  human_size(b.memory_bytes).c_str(), human_size(b.block_size).c_str(),
                  b.best_ratio ? "  <- best ratio" : (b.best_speed ? "  <- fastest" : ""));
    os << line;
  }
  return os.str();
}

std::string format_archive_info(const ArchiveInfo& info) {
  std::ostringstream os;
  if (!info.ok) {
    os << "error       : " << info.error << "\n";
    return os.str();
  }
  const ArchiveHeader& h = info.header;
  os << "archive     : version " << h.version << "\n";
  os << "source      : " << h.source_name << " (" << info.file_type_name << " / " << h.type_detail
     << ")\n";
  os << "algorithm   : " << info.algorithm_id << " (" << info.algorithm_name << ") at level "
     << int(h.level) << "\n";
  os << "original    : " << human_size(h.orig_size) << "\n";
  os << "payload     : " << human_size(h.comp_size) << "\n";
  os << "ratio       : " << format_ratio(h.orig_size > 0 ? double(h.comp_size) / double(h.orig_size) : 1.0)
     << "\n";
  os << "chunks      : " << h.chunk_count << " x " << human_size(h.chunk_size) << "\n";
  os << "encrypted   : " << (info.encrypted ? "yes" : "no");
  if (info.encrypted) os << " (PBKDF2-HMAC-SHA256, " << h.kdf_iterations << " iterations)";
  os << "\n";
  os << "created     : " << h.created << "\n";
  os << "crc32       : " << crc32_hex(h.orig_crc) << "\n";
  os << "header      : " << (info.header_consistent ? "consistent" : "INCONSISTENT") << "\n";
  os << "payload     : " << (info.payload_crc_ok ? "checksum ok" : "CHECKSUM MISMATCH")
     << " (" << human_size(info.payload_read) << " read back)\n";
  return os.str();
}

}  // namespace kolma