// End-to-end engine tests: chunking, parallelism, encryption, verification.
#include <string>
#include <utility>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/archive.hpp"
#include "kolma/engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

namespace {

std::string round_trip_dir() {
  static std::string dir = kolma_test::temp_dir() + "/round-trip";
  std::filesystem::create_directories(dir);
  return dir;
}

// Compresses `input` and restores it, returning the restored bytes.
Bytes compress_and_restore(const std::string& tag, const Bytes& data, const std::string& algo,
                           int level = 6, size_t chunk_size = 0, size_t threads = 0,
                           const std::string& password = "") {
  const std::string in = round_trip_dir() + "/" + tag + ".in";
  const std::string arc = round_trip_dir() + "/" + tag + ".kolma";
  const std::string out = round_trip_dir() + "/" + tag + ".out";
  kolma_test::write_bytes(in, data);

  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = arc;
  job.algorithm = algo;
  job.level = level;
  job.chunk_size = chunk_size;
  job.threads = threads;
  job.password = password;
  Result r = run_job(job);
  if (!r.ok) throw kolma_test::Failure("compress failed for " + algo + ": " + r.error);
  if (r.stats.orig_size != data.size())
    throw kolma_test::Failure("compressed size bookkeeping is wrong for " + algo);

  Job back;
  back.mode = Mode::Decompress;
  back.input = arc;
  back.output = out;
  back.password = password;
  Result rb = run_job(back);
  if (!rb.ok) throw kolma_test::Failure("decompress failed for " + algo + ": " + rb.error);
  if (!rb.stats.verified) throw kolma_test::Failure("checksum was not verified for " + algo);
  return kolma_test::read_bytes(out);
}

std::vector<std::pair<std::string, Bytes>> engine_fixtures() {
  return {
      {"empty", Bytes{}},
      {"one-byte", Bytes{0x41}},
      {"text", kolma_test::make_repetitive(300000)},
      {"noise", kolma_test::make_random(200000, 3)},
      {"odd", kolma_test::make_random(300011, 5)},
      {"smooth", kolma_test::make_smooth_numeric(180000)},
  };
}

}  // namespace

KOLMA_TEST(engine_round_trips_every_algorithm_and_fixture) {
  for (const Algorithm* algo : all_algorithms()) {
    for (const auto& [name, data] : engine_fixtures()) {
      const std::string tag = "all-" + algo->params().id + "-" + name;
      Bytes restored = compress_and_restore(tag, data, algo->params().id);
      if (restored != data) {
        throw kolma_test::Failure(algo->params().id + "/" + name + ": " +
                                  kolma_test::bytes_diff(restored, data));
      }
    }
  }
}

KOLMA_TEST(engine_auto_mode_picks_a_winner_and_round_trips) {
  for (const auto& [name, data] : engine_fixtures()) {
    const std::string tag = "auto-" + name;
    Bytes restored = compress_and_restore(tag, data, "auto");
    if (restored != data) {
      throw kolma_test::Failure("auto/" + name + ": " + kolma_test::bytes_diff(restored, data));
    }
  }
}

KOLMA_TEST(auto_mode_compresses_text_and_vetoes_noise) {
  const std::string text_in = round_trip_dir() + "/veto-text.in";
  kolma_test::write_bytes(text_in, kolma_test::make_repetitive(200000));
  Job job;
  job.mode = Mode::Compress;
  job.input = text_in;
  job.algorithm = "auto";
  Result r = run_job(job);
  CHECK(r.ok);
  CHECK(r.stats.algorithm != "store");
  CHECK(r.stats.ratio < 0.5);
  CHECK(!r.analysis.rationale.empty());
  CHECK(!r.analysis.trials.empty());

  const std::string noise_in = round_trip_dir() + "/veto-noise.in";
  kolma_test::write_bytes(noise_in, kolma_test::make_random(262144, 17));
  Job noise_job;
  noise_job.mode = Mode::Compress;
  noise_job.input = noise_in;
  noise_job.algorithm = "auto";
  Result rn = run_job(noise_job);
  CHECK(rn.ok);
  CHECK(rn.analysis.store_raw);
  CHECK_EQ(rn.stats.algorithm, std::string("store"));
  CHECK(rn.stats.comp_size == rn.stats.orig_size);
}

KOLMA_TEST(thread_count_does_not_change_the_result) {
  Bytes data = kolma_test::make_text_like(250000);
  Bytes one = compress_and_restore("threads-1", data, "lz77", 6, 16384, 1);
  Bytes four = compress_and_restore("threads-4", data, "lz77", 6, 16384, 4);
  CHECK_BYTES_EQ(one, data);
  CHECK_BYTES_EQ(four, data);
  Bytes eight = compress_and_restore("threads-8", data, "lz77", 6, 4096, 8);
  CHECK_BYTES_EQ(eight, data);
}

KOLMA_TEST(many_small_chunks_round_trip) {
  Bytes data = kolma_test::make_random(100003, 23);  // prime length, tiny chunks
  Bytes restored = compress_and_restore("chunks-4k", data, "lz77", 6, 4096, 4);
  CHECK_BYTES_EQ(restored, data);

  const std::string arc = round_trip_dir() + "/chunks-4k.kolma";
  ArchiveInfo info = read_archive_info(arc);
  CHECK(info.ok);
  CHECK(info.header_consistent);
  CHECK(info.payload_crc_ok);
  CHECK_EQ(info.header.chunk_count, uint32_t((100003 + 4095) / 4096));
  CHECK_EQ(info.header.chunk_size, uint64_t(4096));
}

KOLMA_TEST(encrypted_archives_round_trip_and_reject_a_wrong_password) {
  Bytes data = kolma_test::make_repetitive(120000);
  const std::string tag = "encrypted";
  Bytes restored = compress_and_restore(tag, data, "lz77", 6, 0, 0, "correct horse battery staple");
  CHECK_BYTES_EQ(restored, data);

  const std::string arc = round_trip_dir() + "/" + tag + ".kolma";
  ArchiveInfo info = read_archive_info(arc);
  CHECK(info.ok);
  CHECK(info.encrypted);
  CHECK_EQ(info.header.kdf_iterations, kDefaultKdfIterations);
  CHECK(info.payload_crc_ok);

  // The plaintext must not appear anywhere in the archive.
  const Bytes raw = kolma_test::read_bytes(arc);
  Bytes needle(data.begin(), data.begin() + 64);
  bool found = std::search(raw.begin(), raw.end(), needle.begin(), needle.end()) != raw.end();
  CHECK(!found);

  Job back;
  back.mode = Mode::Decompress;
  back.input = arc;
  back.output = round_trip_dir() + "/encrypted-wrong.out";
  back.password = "wrong horse battery staple";
  Result wrong = run_job(back);
  CHECK(!wrong.ok);
  CHECK(!wrong.error.empty());

  Job missing;
  missing.mode = Mode::Decompress;
  missing.input = arc;
  missing.output = round_trip_dir() + "/encrypted-missing.out";
  Result none = run_job(missing);
  CHECK(!none.ok);
  CHECK(none.error.find("password") != std::string::npos);
}

KOLMA_TEST(corrupted_archives_fail_verification) {
  Bytes data = kolma_test::make_repetitive(150000);
  const std::string arc = round_trip_dir() + "/corrupt.kolma";
  const std::string in = round_trip_dir() + "/corrupt.in";
  kolma_test::write_bytes(in, data);

  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = arc;
  job.algorithm = "lz77";
  CHECK(run_job(job).ok);

  Bytes archive = kolma_test::read_bytes(arc);
  archive[archive.size() - 1] ^= 0x01;  // damage the payload
  const std::string damaged = round_trip_dir() + "/corrupt-damaged.kolma";
  kolma_test::write_bytes(damaged, archive);

  ArchiveInfo info = read_archive_info(damaged);
  CHECK(info.ok);
  CHECK(!info.payload_crc_ok);

  Job back;
  back.mode = Mode::Decompress;
  back.input = damaged;
  back.output = round_trip_dir() + "/corrupt.out";
  Result r = run_job(back);
  CHECK(!r.ok);
}

KOLMA_TEST(engine_refuses_to_overwrite_its_own_input) {
  const std::string in = round_trip_dir() + "/self.in";
  kolma_test::write_bytes(in, kolma_test::make_repetitive(1000));
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = in;
  Result r = run_job(job);
  CHECK(!r.ok);
  CHECK(r.error.find("refusing") != std::string::npos);
}

KOLMA_TEST(engine_reports_unknown_algorithms_and_missing_files) {
  Job job;
  job.mode = Mode::Compress;
  job.input = round_trip_dir() + "/self.in";
  job.algorithm = "quantum";
  Result r = run_job(job);
  CHECK(!r.ok);
  CHECK(r.error.find("unknown algorithm") != std::string::npos);

  Job missing;
  missing.mode = Mode::Compress;
  missing.input = round_trip_dir() + "/nothing-here.bin";
  Result rm = run_job(missing);
  CHECK(!rm.ok);
}

KOLMA_TEST(default_chunk_size_respects_the_algorithm_block_size) {
  const Algorithm* bwt = find_algorithm("bwt-mtf-ari");
  CHECK(default_chunk_size(9, 1u << 30, 1, bwt) <= bwt->params().block_size);
  const Algorithm* lz = find_algorithm("lz77");
  CHECK(default_chunk_size(9, 1u << 30, 1, lz) == (4u << 20));
  CHECK(default_chunk_size(1, 1u << 30, 1, lz) == (64u << 10));
  // The memory limit must be able to pull the chunk size down.
  CHECK(default_chunk_size(9, 64u << 10, 4, lz) < (4u << 20));
}

KOLMA_TEST(decompression_uses_the_recorded_source_name) {
  const std::string dir = round_trip_dir();
  const std::string in = dir + "/original-name.dat";
  kolma_test::write_bytes(in, kolma_test::make_text_like(50000));
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.algorithm = "lz77";
  Result r = run_job(job);
  CHECK(r.ok);
  CHECK_EQ(r.stats.output_path, in + ".kolma");

  // Rename the archive so the ".kolma" suffix trick cannot apply, and let the
  // engine fall back to the recorded source name.
  const std::string renamed = dir + "/mystery-archive.bin";
  std::filesystem::copy_file(r.stats.output_path, renamed,
                             std::filesystem::copy_options::overwrite_existing);
  Job back;
  back.mode = Mode::Decompress;
  back.input = renamed;
  Result rb = run_job(back);
  CHECK(rb.ok);
  CHECK_EQ(rb.stats.output_path, dir + "/original-name.dat");
}

KOLMA_TEST(progress_callbacks_report_monotonic_progress) {
  const std::string in = round_trip_dir() + "/progress.in";
  kolma_test::write_bytes(in, kolma_test::make_repetitive(400000));
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.algorithm = "lz77";
  job.chunk_size = 16384;
  job.output = round_trip_dir() + "/progress.kolma";

  uint32_t last_done = 0;
  uint64_t last_in = 0;
  int calls = 0;
  Result r = run_job(job, [&](const Progress& p) {
    ++calls;
    CHECK(p.chunks_done >= last_done);
    CHECK(p.bytes_in >= last_in);
    CHECK(p.chunks_done <= p.chunks_total);
    last_done = p.chunks_done;
    last_in = p.bytes_in;
  });
  CHECK(r.ok);
  CHECK(calls >= 1);
  CHECK_EQ(last_in, uint64_t(400000));
}

KOLMA_TEST(benchmark_covers_every_algorithm_and_ranks_by_ratio) {
  const std::string in = round_trip_dir() + "/bench.in";
  kolma_test::write_bytes(in, kolma_test::make_repetitive(200000));
  Result r = benchmark_file(in, 6, 4, 65536);
  CHECK(r.ok);
  CHECK_EQ(r.benchmark.size(), all_algorithms().size());
  for (size_t i = 1; i < r.benchmark.size(); ++i) {
    CHECK(r.benchmark[i].ratio >= r.benchmark[i - 1].ratio);
  }
  int best_ratio_flags = 0;
  for (const BenchmarkRow& row : r.benchmark) best_ratio_flags += row.best_ratio ? 1 : 0;
  CHECK_EQ(best_ratio_flags, 1);
  CHECK(r.benchmark.front().ratio < 0.4);  // repetitive input must actually shrink
}
