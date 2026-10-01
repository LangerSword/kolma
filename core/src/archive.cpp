#include "kolma/archive.hpp"

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
    out.chunks.push_back(c);
  }
  if (!get_u32(data, pos, out.payload_crc)) {
    error = "missing payload checksum";
    return false;
  }
  payload_offset = pos;
  return true;
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
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    info.error = "cannot open " + path;
    return info;
  }

  // Read enough for the fixed part plus the two length-prefixed strings.
  Bytes head(64 * 1024);
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  head.resize(size_t(in.gcount()));

  size_t payload_offset = 0;
  if (!parse_header(View(head), info.header, payload_offset, info.error)) return info;

  // The chunk table can extend past the first read: pull in whatever is missing.
  if (payload_offset > head.size()) {
    head.resize(payload_offset);
    in.clear();
    in.seekg(0);
    in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    head.resize(size_t(in.gcount()));
    if (!parse_header(View(head), info.header, payload_offset, info.error)) return info;
  }

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

  // Header self-consistency: the table must describe exactly the payload.
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
  in.clear();
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