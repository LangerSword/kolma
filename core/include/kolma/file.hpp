// kolma -- the File entity: identity, type, entropy and checksum of an input.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kolma/util.hpp"

namespace kolma {

enum class FileType : uint8_t {
  Unknown = 0,
  Text = 1,
  Image = 2,
  Audio = 3,
  Video = 4,
  Executable = 5,
  Archive = 6,
  Binary = 7,
  Count = 8,
};

const char* file_type_name(FileType type);
// Parses a type name (case-insensitive); returns false when unknown.
bool file_type_from_name(std::string_view name, FileType& out);

// What we know about a file after inspecting it.
struct FileInfo {
  std::string name;    // basename
  std::string path;    // path as given by the caller
  uint64_t size = 0;   // bytes
  FileType type = FileType::Unknown;
  std::string type_detail;  // "jpeg", "elf", "mp4", ...
  double entropy = 0.0;     // Shannon entropy of the sample, bits/byte (0..8)
  uint32_t checksum = 0;    // CRC-32 over the sample (full-file CRC is done by the Engine)
  int64_t mtime = 0;        // unix seconds
  bool exists = false;
  std::string error;
};

// Sniffs the file type from magic bytes, falling back to the extension and then
// to a printability heuristic. `detail` receives a short format name when known.
FileType detect_type_from_header(View header, const std::string& path, std::string* detail = nullptr);

// Shannon entropy over the byte histogram of `data`, in bits per byte (0..8).
double shannon_entropy(View data);

// Reads up to `sample_size` bytes and fills everything except the full-file checksum.
FileInfo inspect_file(const std::string& path, size_t sample_size = 1u << 20);

// Reads the first `n` bytes of a file (fewer if the file is shorter).
Bytes read_file_prefix(const std::string& path, size_t n);

// True when the format is already entropy-coded (jpeg, png, mp4, zip, ...) so
// recompressing it can only add overhead.
bool is_pretty_compressed(FileType type, const std::string& detail);

}  // namespace kolma
