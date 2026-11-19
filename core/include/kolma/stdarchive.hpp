// kolma -- standard, universally recognised archive formats.
//
// The native .kolma format showcases the engine, but no OS or third-party tool
// opens it. This layer speaks the formats everyone already uses — zip, tar,
// tar.gz, tar.xz, tar.bz2, 7z — so an archive kolma produces can be opened by
// Finder / Explorer / file-roller / `unzip` / `tar`, and kolma can open archives
// made by anything else. Backed by libarchive; compiled only when it is present
// (KOLMA_HAVE_LIBARCHIVE), with graceful stubs otherwise.
//
// Extraction is path-traversal safe (libarchive SECURE_NODOTDOT / SYMLINKS /
// NOABSOLUTEPATHS) and refuses to overwrite unless `force` is set.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kolma/engine.hpp"  // ProgressFn

namespace kolma {

// True when the build includes standard-format support (libarchive found).
bool std_formats_available();

// One entry inside a standard archive. `path` is relative, '/'-separated.
struct StdEntry {
  std::string path;
  uint64_t size = 0;
  bool is_dir = false;
  int64_t mtime = 0;
};

// Identify a standard archive by magic bytes (and extension for tar variants).
// Returns a format tag — "zip", "7z", "tar", "tar.gz", "tar.xz", "tar.bz2",
// "gz", "xz", "bz2", "zst", "rar" — or "" when it is not a recognised standard
// archive (e.g. a native .kolma file). Does not read the whole file.
std::string std_detect(const std::string& path);

// True when std_detect recognises the file (i.e. kolma should route it here
// rather than to the native engine).
bool std_is_standard(const std::string& path);

// The standard format implied by an output path's extension (".zip" -> "zip",
// ".tar.gz" -> "tar.gz", ...), or "" when the extension is not a standard
// archive (e.g. ".kolma"). Used to route `compress --out foo.zip`.
std::string std_format_from_extension(const std::string& output);

struct StdPackOptions {
  // "zip" (default), "tar", "tar.gz", "tar.xz", "tar.bz2", "7z". When empty it
  // is inferred from the output extension, falling back to zip.
  std::string format;
  int level = 6;       // compressor effort where the format supports it
  bool force = false;  // overwrite an existing output (refused by default)
};

// Pack files/folders into a standard archive. Each input is stored under its
// basename; folders recurse; symlinks are skipped. Returns false + error.
bool std_pack(const std::vector<std::string>& inputs, const std::string& output,
              const StdPackOptions& opts, std::string& error,
              const ProgressFn& on_progress = nullptr);

// Extract any libarchive-readable archive into dest_dir. Refuses traversal and
// overwrites (unless force). Returns false + error.
bool std_extract(const std::string& archive, const std::string& dest_dir, bool force,
                 std::string& error, const ProgressFn& on_progress = nullptr);

// List entries without extracting. Returns false + error.
bool std_list(const std::string& archive, std::vector<StdEntry>& entries, std::string& error);

// Verify integrity by streaming every entry's data (libarchive checks stored
// CRCs as it reads). Returns false + error on corruption; `entries` counts what
// was read. The WinRAR-style "test archive" operation — no files are written.
bool std_test(const std::string& archive, uint64_t& entries, std::string& error);

}  // namespace kolma
