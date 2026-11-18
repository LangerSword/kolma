#include "kolma/archive.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr size_t kFixedHeaderBytes =
    5 /*magic*/ + 2 + 1 + 1 + 1 + 1 /*version, algo, level, type, flags*/ +
    8 + 8 + 4 + 8 /*orig_size, comp_size, orig_crc, created*/ +
    8 + 4 + 4 /*chunk_size, chunk_count, kdf_iterations*/ +
    kSaltSize + kNonceSize;

}  // namespace

Bytes serialize_header(const ArchiveHeader& header) {
  Bytes out;
  out.reserve(kFixedHeaderBytes + header.source_name.size() + header.type_detail.size() +
              header.chunks.size() * 17 + 4);
  out.insert(out.end(), kArchiveMagic.begin(), kArchiveMagic.end());
  put_u16(out, header.version);
  out.push_back(header.algo_index);
  out.push_back(header.level);
  out.push_back(header.file_type);
  out.push_back(header.flags);
  put_u64(out, header.orig_size);
  put_u64(out, header.comp_size);
  put_u32(out, header.orig_crc);
  put_u64(out, header.created);
  put_u64(out, header.chunk_size);
  put_u32(out, header.chunk_count);
  put_u32(out, header.kdf_iterations);
  out.insert(out.end(), header.salt.begin(), header.salt.end());
  out.insert(out.end(), header.nonce.begin(), header.nonce.end());
  put_u16(out, uint16_t(header.source_name.size()));
  out.insert(out.end(), header.source_name.begin(), header.source_name.end());
  put_u16(out, uint16_t(header.type_detail.size()));
  out.insert(out.end(), header.type_detail.begin(), header.type_detail.end());
  for (const ChunkRecord& c : header.chunks) {
    put_u64(out, c.orig_size);
    put_u64(out, c.comp_size);
    out.push_back(c.flags);
  }
  put_u32(out, header.payload_crc);
  return out;
}

bool parse_header(View data, ArchiveHeader& out, size_t& payload_offset, std::string& error) {
  if (data.size() < kFixedHeaderBytes) {
    error = "file is too short to contain a kolma header";
    return false;
  }
  if (std::memcmp(data.data(), kArchiveMagic.data(), kArchiveMagic.size()) != 0) {
    error = "not a kolma archive (bad magic)";
    return false;
  }
  size_t pos = kArchiveMagic.size();
  if (!get_u16(data, pos, out.version)) {
    error = "truncated version";
    return false;
  }
  if (out.version != kArchiveVersion) {
    error = "unsupported archive version " + std::to_string(out.version);
    return false;
  }
  out.algo_index = data[pos++];
  out.level = data[pos++];
  out.file_type = data[pos++];
  out.flags = data[pos++];
  if (!get_u64(data, pos, out.orig_size) || !get_u64(data, pos, out.comp_size) ||
      !get_u32(data, pos, out.orig_crc) || !get_u64(data, pos, out.created) ||
      !get_u64(data, pos, out.chunk_size) || !get_u32(data, pos, out.chunk_count) ||
      !get_u32(data, pos, out.kdf_iterations)) {
    error = "truncated header fields";
    return false;
  }
  // Bound the chunk table BEFORE reserving anything: chunk_count is untrusted
  // and a 4-billion-entry reserve would ask for ~68 GiB. Every record needs 17
  // bytes plus the trailing payload checksum, so the buffer itself caps it.
  if (out.chunk_count == 0) {
    error = "archive claims zero chunks";
    return false;
  }
  if (uint64_t(out.chunk_count) * 17 + 4 > uint64_t(data.size() - pos)) {
    error = "chunk table claims more data than the archive holds";
    return false;
  }
  if (out.chunk_size == 0 || out.chunk_size > kMaxChunkSize) {
    error = "chunk size out of range (1 byte to 64 MiB)";
    return false;
  }
  if (archive_encrypted(out) && out.kdf_iterations > kMaxKdfIterations) {
    error = "kdf iterations exceed the supported maximum (10000000)";
    return false;
  }
  if (pos + kSaltSize + kNonceSize > data.size()) {
    error = "truncated salt/nonce";
    return false;
  }
  out.salt.assign(data.begin() + std::ptrdiff_t(pos), data.begin() + std::ptrdiff_t(pos + kSaltSize));
  pos += kSaltSize;
  out.nonce.assign(data.begin() + std::ptrdiff_t(pos), data.begin() + std::ptrdiff_t(pos + kNonceSize));
  pos += kNonceSize;

  uint16_t name_len = 0;
  if (!get_u16(data, pos, name_len) || pos + name_len > data.size()) {
    error = "truncated source name";
    return false;
  }
  out.source_name.assign(reinterpret_cast<const char*>(data.data() + pos), name_len);
  pos += name_len;

  uint16_t detail_len = 0;
  if (!get_u16(data, pos, detail_len) || pos + detail_len > data.size()) {
    error = "truncated type detail";
    return false;
  }
  out.type_detail.assign(reinterpret_cast<const char*>(data.data() + pos), detail_len);
  pos += detail_len;

  out.chunks.clear();
  out.chunks.reserve(out.chunk_count);
  uint64_t total_orig = 0;
  uint64_t total_comp = 0;
  for (uint32_t i = 0; i < out.chunk_count; ++i) {
    ChunkRecord c;
    if (!get_u64(data, pos, c.orig_size) || !get_u64(data, pos, c.comp_size)) {
      error = "truncated chunk table";
      return false;
    }
    if (pos >= data.size()) {
      error = "truncated chunk table";
      return false;
    }
    c.flags = data[pos++];
    // No chunk may claim more than the declared chunk size: this is what
    // bounds every allocation the decompressor makes from untrusted input.
    if (c.orig_size > out.chunk_size || c.comp_size > out.chunk_size) {
      error = "chunk " + std::to_string(i) + " claims more than the chunk size";
      return false;
    }
    if (c.orig_size > UINT64_MAX - total_orig || c.comp_size > UINT64_MAX - total_comp) {
      error = "chunk sizes overflow";
      return false;
    }
    total_orig += c.orig_size;
    total_comp += c.comp_size;
    out.chunks.push_back(c);
  }
  if (!get_u32(data, pos, out.payload_crc)) {
    error = "missing payload checksum";
    return false;
  }
  // The table must describe exactly the payload, or the archive is damaged.
  if (total_orig != out.orig_size || total_comp != out.comp_size ||
      out.chunks.size() != out.chunk_count) {
    error = "header sizes do not match the chunk table";
    return false;
  }
  payload_offset = pos;
  return true;
}

bool load_archive_header(const std::string& path, ArchiveHeader& header, size_t& payload_offset,
                         std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end < 0) {
    error = "cannot determine the size of " + path;
    return false;
  }
  const uint64_t file_size = uint64_t(end);
  in.seekg(0);

  // Probe enough to hold the fixed part and both length-prefixed strings, then
  // size the real read from the header's own fields. The chunk table can be
  // far larger than one probe (a 40 GiB file can carry hundreds of thousands
  // of chunks), so a fixed-size read is not enough.
  constexpr uint64_t kProbeBytes = kFixedHeaderBytes + 2 + 65535 + 2 + 65535;
  Bytes head(size_t(std::min<uint64_t>(file_size, kProbeBytes)));
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  head.resize(size_t(in.gcount()));

  const auto payload_fits = [&]() {
    if (header.comp_size > file_size - payload_offset) {
      error = "archive payload is truncated";
      return false;
    }
    return true;
  };

  if (parse_header(View(head), header, payload_offset, error)) return payload_fits();

  // A failed parse is only worth retrying with a bigger read if this really is
  // a kolma header whose chunk table did not fit the probe. Anything else
  // (bad magic, wrong version, inconsistent sizes) keeps its first error.
  if (head.size() < kFixedHeaderBytes ||
      std::memcmp(head.data(), kArchiveMagic.data(), kArchiveMagic.size()) != 0) {
    return false;
  }
  size_t probe_pos = kFixedHeaderBytes;
  uint16_t name_len = 0;
  uint32_t chunk_count = 0;
  // chunk_count lives in the fixed part, before the strings.
  size_t count_pos = kArchiveMagic.size() + 2 + 4 + 8 + 8 + 4 + 8 + 8;
  if (!get_u32(View(head), count_pos, chunk_count) ||
      !get_u16(View(head), probe_pos, name_len)) {
    error = "truncated header fields";
    return false;
  }
  const uint64_t detail_len_pos = uint64_t(kFixedHeaderBytes) + 2 + name_len;
  if (head.size() < detail_len_pos + 2) {
    error = "truncated header";
    return false;
  }
  uint16_t detail_len = 0;
  size_t detail_pos = size_t(detail_len_pos);
  if (!get_u16(View(head), detail_pos, detail_len)) {
    error = "truncated header";
    return false;
  }
  const uint64_t total = detail_len_pos + 2 + detail_len + uint64_t(chunk_count) * 17 + 4;
  if (total > kMaxHeaderBytes) {
    error = "archive header exceeds the 64 MiB limit";
    return false;
  }
  if (total > file_size) {
    error = "truncated header";
    return false;
  }
  if (total <= head.size()) return false;  // the first parse already saw it all
  head.resize(size_t(total));
  in.clear();
  in.seekg(0);
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  head.resize(size_t(in.gcount()));
  if (!parse_header(View(head), header, payload_offset, error)) return false;
  return payload_fits();
}

Bytes derive_key(const std::string& password, const ArchiveHeader& header) {
  uint32_t iterations = header.kdf_iterations == 0 ? kDefaultKdfIterations : header.kdf_iterations;
  return pbkdf2_sha256(password, View(header.salt), iterations, 32);
}

std::vector<uint8_t> nonce_for_chunk(const ArchiveHeader& header, uint32_t index) {
  std::vector<uint8_t> nonce = header.nonce;
  if (nonce.size() != kNonceSize) nonce.resize(kNonceSize, 0);
  for (int i = 0; i < 4; ++i) nonce[8 + size_t(i)] = uint8_t(index >> (i * 8));
  return nonce;
}

ArchiveInfo read_archive_info(const std::string& path, bool check_payload) {
  ArchiveInfo info;
  size_t payload_offset = 0;
  if (!load_archive_header(path, info.header, payload_offset, info.error)) return info;

  const ArchiveHeader& h = info.header;
  info.ok = true;
  info.encrypted = archive_encrypted(h);
  if (const Algorithm* a = algorithm_by_index(h.algo_index)) {
    info.algorithm_id = a->params().id;
    info.algorithm_name = a->params().name;
  } else {
    info.algorithm_id = "unknown";
    info.algorithm_name = "unknown algorithm id " + std::to_string(h.algo_index);
  }
  info.file_type_name = file_type_name(FileType(h.file_type));

  // parse_header already enforced this; report it for the contract's sake.
  uint64_t total_orig = 0;
  uint64_t total_comp = 0;
  for (const ChunkRecord& c : h.chunks) {
    total_orig += c.orig_size;
    total_comp += c.comp_size;
  }
  info.header_consistent = (total_orig == h.orig_size) && (total_comp == h.comp_size) &&
                           (h.chunks.size() == h.chunk_count);

  if (!check_payload) return info;

  // Stream the payload and check its checksum without decompressing.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    info.error = "cannot reopen " + path;
    info.ok = false;
    return info;
  }
  in.seekg(std::streamoff(payload_offset));
  Bytes buffer(1u << 20);
  uint32_t crc = 0;
  uint64_t read_total = 0;
  while (in && read_total < h.comp_size) {
    size_t want = size_t(std::min<uint64_t>(buffer.size(), h.comp_size - read_total));
    in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(want));
    size_t got = size_t(in.gcount());
    if (got == 0) break;
    crc = crc32(View(buffer.data(), got), crc);
    read_total += got;
  }
  info.payload_read = read_total;
  info.payload_crc_ok = (read_total == h.comp_size) && (crc == h.payload_crc);
  return info;
}

}  // namespace kolma
