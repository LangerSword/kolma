// kolma -- the Archive entity: the container that carries the compressed data
// and everything needed to reverse the job.
//
// Layout (all integers little-endian), version 2:
//
//   magic "KOLMA" | version u16 | algo u8 | level u8 | file type u8 | flags u8
//   orig_size u64 | comp_size u64 | orig_crc u32 | created u64
//   chunk_size u64 | chunk_count u32 | kdf_iterations u32
//   salt[16] | nonce[12]
//   name_len u16 | name | detail_len u16 | detail
//   chunk_count x { orig_size u64 | comp_size u64 | flags u8 }
//   payload_crc u32 | mac[32]
//   payload: the compressed chunks, concatenated in order
//
// Version 1 is identical but has no mac field. It is still readable; new
// archives are always written as version 2.
//
// Encryption (v2): PBKDF2-HMAC-SHA256 derives 64 bytes from the password and
// the header salt; the first 32 are the ChaCha20 key, the last 32 the MAC key.
// The payload is encrypted per chunk (Encrypt), then authenticated as a whole
// (then-MAC): mac = HMAC-SHA256(mac_key, payload || header-without-the-mac).
// The header prefix covers every metadata byte, so tampering with sizes,
// flags, salt or the chunk table fails authentication too. Verification
// happens before any output file is created.
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
inline constexpr uint16_t kArchiveVersion = 2;
inline constexpr size_t kSaltSize = 16;
inline constexpr size_t kNonceSize = 12;
inline constexpr size_t kMacSize = 32;
inline constexpr uint32_t kDefaultKdfIterations = 200000;
// Hard limits on what a header may claim. They bound every allocation the
// decompressor makes from untrusted input: a chunk can never be larger than
// kMaxChunkSize, the KDF can never be asked for more than kMaxKdfIterations
// rounds, and a header can never be larger than kMaxHeaderBytes. Archives
// kolma writes always satisfy them; anything else is refused, not attempted.
inline constexpr uint64_t kMaxChunkSize = 64ull << 20;          // 64 MiB
inline constexpr uint32_t kMaxKdfIterations = 10000000;         // ~seconds, not years
inline constexpr uint64_t kMaxHeaderBytes = 64ull << 20;        // ~3.9M chunks

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
  std::vector<uint8_t> mac = std::vector<uint8_t>(kMacSize, 0);  // v2 only
};

inline bool archive_encrypted(const ArchiveHeader& h) { return (h.flags & kArchiveEncrypted) != 0; }

// Serialises the whole header (magic through payload_crc) with no payload.
Bytes serialize_header(const ArchiveHeader& header);

// Parses a header from the front of `data`. On success `payload_offset` points
// at the first payload byte. Returns false and fills `error` on any problem.
bool parse_header(View data, ArchiveHeader& out, size_t& payload_offset, std::string& error);

// PBKDF2-HMAC-SHA256 over the header's salt. Never logs or stores the password.
// v1 archives derive a single 32-byte ChaCha20 key with this.
Bytes derive_key(const std::string& password, const ArchiveHeader& header);

// v2 key schedule: 64 derived bytes split into the encryption key and a
// separate MAC key. For v1 (or unencrypted) archives mac_key comes back empty.
// Throws FormatError when the header asks for more than kMaxKdfIterations.
void derive_keys(const std::string& password, const ArchiveHeader& header, Bytes& enc_key,
                 Bytes& mac_key);

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
  bool authenticated = false;       // v2 + encrypted: an HMAC protects the archive
  bool header_consistent = false;   // sizes and chunk table add up
  bool payload_crc_ok = false;      // payload read back and hashed
  uint64_t payload_read = 0;
};

// Reads the header and, when `check_payload` is set, streams the payload to
// verify payload_crc without decompressing anything.
ArchiveInfo read_archive_info(const std::string& path, bool check_payload = true);

// Loads and validates a whole archive header from disk, sizing the read from
// the header's own length fields (the chunk table can be far larger than one
// read). Also checks that the file actually contains the claimed payload.
// This is the only header-loading path: the engine and `info` share it.
bool load_archive_header(const std::string& path, ArchiveHeader& header, size_t& payload_offset,
                         std::string& error);

}  // namespace kolma
