// kolma -- the Archive entity: the container that carries the compressed data
// and everything needed to reverse the job.
//
// Layout (all integers little-endian):
//
//   magic "KOLMA" | version u16 | algo u8 | level u8 | file type u8 | flags u8
//   orig_size u64 | comp_size u64 | orig_crc u32 | created u64
//   chunk_size u64 | chunk_count u32 | kdf_iterations u32
//   salt[16] | nonce[12]
//   name_len u16 | name | detail_len u16 | detail
//   chunk_count x { orig_size u64 | comp_size u64 | flags u8 }
//   payload_crc u32
//   payload: the compressed chunks, concatenated in order
//
// Chunk flag bit 0 means "stored raw": the Analyzer's veto and the per-chunk
// fallback both land here, so a single archive can mix methods.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kolma/file.hpp"
#include "kolma/util.hpp"

namespace kolma {

inline constexpr std::string_view kArchiveMagic = "KOLMA";
inline constexpr uint16_t kArchiveVersion = 1;
inline constexpr size_t kSaltSize = 16;
inline constexpr size_t kNonceSize = 12;
inline constexpr uint32_t kDefaultKdfIterations = 200000;

enum ArchiveFlags : uint8_t {
  kArchiveEncrypted = 1u << 0,
};

enum ChunkFlags : uint8_t {
  kChunkStoredRaw = 1u << 0,
};

struct ChunkRecord {
  uint64_t orig_size = 0;
  uint64_t comp_size = 0;
  uint8_t flags = 0;
};

struct ArchiveHeader {
  uint16_t version = kArchiveVersion;
  uint8_t algo_index = 0;
  uint8_t level = 6;
  uint8_t file_type = 0;  // FileType
  uint8_t flags = 0;
  uint64_t orig_size = 0;
  uint64_t comp_size = 0;  // payload bytes only
  uint32_t orig_crc = 0;
  uint64_t created = 0;
  uint64_t chunk_size = 0;
  uint32_t chunk_count = 0;
  uint32_t kdf_iterations = 0;
  std::vector<uint8_t> salt = std::vector<uint8_t>(kSaltSize, 0);
  std::vector<uint8_t> nonce = std::vector<uint8_t>(kNonceSize, 0);
  std::string source_name;
  std::string type_detail;
  std::vector<ChunkRecord> chunks;
  uint32_t payload_crc = 0;
};

inline bool archive_encrypted(const ArchiveHeader& h) { return (h.flags & kArchiveEncrypted) != 0; }

// Serialises the whole header (magic through payload_crc) with no payload.
Bytes serialize_header(const ArchiveHeader& header);

// Parses a header from the front of `data`. On success `payload_offset` points
// at the first payload byte. Returns false and fills `error` on any problem.
bool parse_header(View data, ArchiveHeader& out, size_t& payload_offset, std::string& error);

// PBKDF2-HMAC-SHA256 over the header's salt. Never logs or stores the password.
Bytes derive_key(const std::string& password, const ArchiveHeader& header);

// Nonce for chunk `index`: the header nonce with the last 4 bytes replaced by
// the little-endian chunk index, so no two chunks share a keystream.
std::vector<uint8_t> nonce_for_chunk(const ArchiveHeader& header, uint32_t index);

// Everything a caller needs to describe an archive without decompressing it.
struct ArchiveInfo {
  bool ok = false;
  std::string error;
  ArchiveHeader header;
  std::string algorithm_id;
  std::string algorithm_name;
  std::string file_type_name;
  bool encrypted = false;
  bool header_consistent = false;   // sizes and chunk table add up
  bool payload_crc_ok = false;      // payload read back and hashed
  uint64_t payload_read = 0;
};

// Reads the header and, when `check_payload` is set, streams the payload to
// verify payload_crc without decompressing anything.
ArchiveInfo read_archive_info(const std::string& path, bool check_payload = true);

}  // namespace kolma