// kolma -- the bundle layer: many files and whole folders in one archive.
//
// A bundle is a standard ustar tar stream (with GNU long-name entries for paths
// over 100 chars) that the engine compresses as a single payload and marks with
// the kArchiveBundle flag. Because the payload is a real tar, anything that can
// read tar can recover the contents after a plain `kolma decompress`, and the
// GUI/TUI can list and selectively extract entries.
//
// Extraction is path-traversal safe: absolute paths and ".." components are
// refused, and symlinks/devices in the tar are skipped rather than recreated.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kolma/analyzer.hpp"
#include "kolma/engine.hpp"

namespace kolma {

// One entry inside a bundle. `path` is relative, '/'-separated, no leading slash.
struct BundleEntry {
  std::string path;
  uint64_t size = 0;
  bool is_dir = false;
  int64_t mtime = 0;
  uint32_t mode = 0644;
};

// Compression settings shared with the engine Job.
struct BundleOptions {
  std::string algorithm = "auto";
  int level = 0;  // 0 = derive from priority
  Priority priority = Priority::Balanced;
  std::string password;
  size_t threads = 0;
  size_t chunk_size = 0;
  size_t memory_limit = 256u << 20;
};

// Compress files/folders into one .kolma bundle. Each input is stored under its
// basename; folders recurse. Symlinks are skipped. The result has stats.bundle.
Result compress_bundle(const std::vector<std::string>& inputs, const std::string& output,
                       const BundleOptions& opts, const ProgressFn& on_progress = nullptr);

// Decompress an archive and, if it is a bundle, untar it into dest_dir; a
// single-file archive is written under its recorded name inside dest_dir.
Result extract_archive(const std::string& archive, const std::string& dest_dir,
                       const std::string& password, bool force,
                       const ProgressFn& on_progress = nullptr);

// List entries without extracting to the final destination (decompresses to a
// temp file, reads the tar table). Single-file archives yield one entry.
bool list_bundle(const std::string& archive, const std::string& password,
                 std::vector<BundleEntry>& entries, std::string& error);

// --- tar primitives (exposed for tests and reuse) ---

// Write `inputs` (files/dirs) as a ustar tar at tar_path. Returns false + error.
bool tar_write(const std::vector<std::string>& inputs, const std::string& tar_path,
               std::string& error);
// Read a tar table without extracting.
bool tar_list(const std::string& tar_path, std::vector<BundleEntry>& entries, std::string& error);
// Extract a tar into dest_dir, refusing traversal and skipping symlinks/devices.
bool tar_extract(const std::string& tar_path, const std::string& dest_dir, bool force,
                 std::string& error);
// True when the file begins with a plausible ustar header.
bool looks_like_tar(const std::string& path);

}  // namespace kolma
