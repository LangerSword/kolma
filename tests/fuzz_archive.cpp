// libFuzzer harness: untrusted bytes must never crash the engine.
//
// Three surfaces per iteration:
//   1. parse_header on the raw bytes (the in-memory parser)
//   2. load_archive_header + read_archive_info + run_decompress on the bytes
//      as a file (the loader, the MAC/CRC gates and the chunk pipeline)
//   3. every algorithm's decompress_block directly, with a fuzz-derived
//      expected output size (the decoders, without the container's protection)
//
// Build (Clang only):
//   cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
//         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKOLMA_FUZZ=ON
//   cmake --build build-fuzz --target kolma_fuzz
//   ./build-fuzz/kolma_fuzz tests/fuzz_corpus -max_total_time=120
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "kolma/algorithm.hpp"
#include "kolma/archive.hpp"
#include "kolma/engine.hpp"

namespace {

// Fixed scratch paths: rewritten every iteration, never accumulated.
const char* kArchivePath = "/tmp/kolma-fuzz-input.kolma";
const char* kOutputPath = "/tmp/kolma-fuzz-output.bin";

uint64_t load_le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (i * 8);
  return v;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 4u << 20) return 0;  // keep iterations fast; the parser caps itself

  // 1. The in-memory parser. Must return cleanly on anything.
  {
    kolma::ArchiveHeader h;
    size_t offset = 0;
    std::string error;
    kolma::parse_header(kolma::View(data, size), h, offset, error);
  }

  // 2. The file path: loader, info, full decompression.
  {
    std::ofstream out(kArchivePath, std::ios::binary | std::ios::trunc);
    if (size > 0) out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    out.close();

    kolma::ArchiveHeader h;
    size_t offset = 0;
    std::string error;
    if (kolma::load_archive_header(kArchivePath, h, offset, error)) {
      // PBKDF2 is deliberately slow. A crafted header may claim up to
      // kMaxKdfIterations rounds, which blows libFuzzer's per-input timeout.
      // Cap the rounds so the KDF/MAC path stays fast. kdf_iterations is a
      // fixed-width field, so the serialized header size (and payload offset)
      // is unchanged. The MAC generally won't match after the rewrite; that is
      // fine here, the fuzzer hunts crashes, not valid authentication tags.
      if (h.kdf_iterations > 4096) {
        h.kdf_iterations = 4096;
        const kolma::Bytes hdr = kolma::serialize_header(h);
        std::ofstream rw(kArchivePath, std::ios::binary | std::ios::trunc);
        rw.write(reinterpret_cast<const char*>(hdr.data()), std::streamsize(hdr.size()));
        if (size > offset) {
          rw.write(reinterpret_cast<const char*>(data + offset), std::streamsize(size - offset));
        }
        rw.close();
      }
      kolma::read_archive_info(kArchivePath, false);
      kolma::Job job;
      job.mode = kolma::Mode::Decompress;
      job.input = kArchivePath;
      job.output = kOutputPath;
      job.force = true;
      job.threads = 2;
      job.password = "fuzz";  // exercises the KDF/MAC path on crafted headers
      kolma::run_job(job);
      std::remove(kOutputPath);
    }
  }

  // 3. The decoders directly: first 8 bytes name the expected output size
  // (capped), the rest is the compressed block. Every method must throw or
  // produce exactly that many bytes -- never read or write out of bounds.
  if (size >= 8) {
    const uint64_t orig = load_le64(data) % (1u << 20);
    const kolma::View block(data + 8, size - 8);
    for (const kolma::Algorithm* algo : kolma::all_algorithms()) {
      try {
        kolma::Bytes restored;
        algo->decompress_block(block, restored, orig);
      } catch (...) {
        // FormatError and friends are the expected outcome on garbage.
      }
    }
  }
  return 0;
}
