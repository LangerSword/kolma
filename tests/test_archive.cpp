// Archive header serialisation, validation and key derivation.
#include <algorithm>
#include <string>

#include "kolma/archive.hpp"
#include "kolma/algorithm.hpp"
#include "kolma/engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

namespace {

ArchiveHeader sample_header() {
  ArchiveHeader h;
  h.algo_index = algorithm_index("lz77");
  h.level = 7;
  h.file_type = uint8_t(FileType::Text);
  h.flags = kArchiveEncrypted;
  h.orig_size = 3 * 65536;   // must equal the chunk table's sum
  h.comp_size = 60003;       // 20000 + 20001 + 20002
  h.orig_crc = 0xDEADBEEF;
  h.created = 1700000000;
  h.chunk_size = 65536;
  h.chunk_count = 3;
  h.kdf_iterations = 200000;
  h.source_name = "notes.txt";
  h.type_detail = "markdown";
  for (uint32_t i = 0; i < h.chunk_count; ++i) {
    h.chunks.push_back(ChunkRecord{65536, 20000 + i, uint8_t(i == 1 ? kChunkStoredRaw : 0)});
  }
  h.payload_crc = 0x12345678;
  return h;
}

// Crafts a valid all-raw archive around `payload`, split into `chunk_size`
// pieces. Raw chunks restore the payload verbatim, so both checksums are the
// payload's CRC. Used to exercise the loader with headers of any size.
Bytes craft_raw_archive(const std::string& name, const Bytes& payload, size_t chunk_size) {
  ArchiveHeader h;
  h.algo_index = algorithm_index("store");
  h.level = 1;
  h.orig_size = payload.size();
  h.comp_size = payload.size();
  h.orig_crc = crc32(View(payload));
  h.payload_crc = crc32(View(payload));
  h.created = 1700000000;
  h.chunk_size = chunk_size;
  h.source_name = name;
  for (size_t off = 0; off < payload.size(); off += chunk_size) {
    const uint64_t len = std::min<uint64_t>(chunk_size, payload.size() - off);
    h.chunks.push_back(ChunkRecord{len, len, kChunkStoredRaw});
  }
  if (h.chunks.empty()) h.chunks.push_back(ChunkRecord{0, 0, kChunkStoredRaw});
  h.chunk_count = uint32_t(h.chunks.size());
  Bytes out = serialize_header(h);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Little-endian patch helpers: the fixed header fields live at known offsets.
void patch_u32(Bytes& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + size_t(i)] = uint8_t(v >> (i * 8));
}
void patch_u64(Bytes& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[off + size_t(i)] = uint8_t(v >> (i * 8));
}
constexpr size_t kOffOrigSize = 11;
constexpr size_t kOffChunkCount = 47;
constexpr size_t kOffKdfIterations = 51;

}  // namespace

KOLMA_TEST(archive_header_round_trips) {
  ArchiveHeader original = sample_header();
  Bytes bytes = serialize_header(original);

  ArchiveHeader parsed;
  size_t payload_offset = 0;
  std::string error;
  CHECK(parse_header(View(bytes), parsed, payload_offset, error));
  CHECK_EQ(payload_offset, bytes.size());
  CHECK_EQ(parsed.version, original.version);
  CHECK_EQ(int(parsed.algo_index), int(original.algo_index));
  CHECK_EQ(int(parsed.level), int(original.level));
  CHECK_EQ(int(parsed.file_type), int(original.file_type));
  CHECK_EQ(int(parsed.flags), int(original.flags));
  CHECK_EQ(parsed.orig_size, original.orig_size);
  CHECK_EQ(parsed.comp_size, original.comp_size);
  CHECK_EQ(parsed.orig_crc, original.orig_crc);
  CHECK_EQ(parsed.created, original.created);
  CHECK_EQ(parsed.chunk_size, original.chunk_size);
  CHECK_EQ(parsed.chunk_count, original.chunk_count);
  CHECK_EQ(parsed.kdf_iterations, original.kdf_iterations);
  CHECK_EQ(parsed.source_name, original.source_name);
  CHECK_EQ(parsed.type_detail, original.type_detail);
  CHECK_EQ(parsed.payload_crc, original.payload_crc);
  CHECK_EQ(parsed.chunks.size(), original.chunks.size());
  for (size_t i = 0; i < parsed.chunks.size(); ++i) {
    CHECK_EQ(parsed.chunks[i].orig_size, original.chunks[i].orig_size);
    CHECK_EQ(parsed.chunks[i].comp_size, original.chunks[i].comp_size);
    CHECK_EQ(int(parsed.chunks[i].flags), int(original.chunks[i].flags));
  }
  CHECK(archive_encrypted(parsed));
}

KOLMA_TEST(archive_header_rejects_bad_input) {
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;

  CHECK(!parse_header(View(Bytes(10, 0)), parsed, offset, error));
  CHECK(!error.empty());

  Bytes bytes = serialize_header(sample_header());
  bytes[0] = 'X';  // break the magic
  error.clear();
  CHECK(!parse_header(View(bytes), parsed, offset, error));

  Bytes truncated = serialize_header(sample_header());
  truncated.resize(20);
  error.clear();
  CHECK(!parse_header(View(truncated), parsed, offset, error));

  // An unsupported version must be refused rather than guessed at.
  Bytes versioned = serialize_header(sample_header());
  versioned[5] = 9;
  error.clear();
  CHECK(!parse_header(View(versioned), parsed, offset, error));
}

KOLMA_TEST(archive_serialisation_is_stable_for_the_same_input) {
  ArchiveHeader h = sample_header();
  CHECK_BYTES_EQ(serialize_header(h), serialize_header(h));
}

KOLMA_TEST(chunk_nonces_are_unique_per_chunk) {
  ArchiveHeader h = sample_header();
  for (size_t i = 0; i < h.nonce.size(); ++i) h.nonce[i] = 0xAA;
  auto first = nonce_for_chunk(h, 0);
  auto second = nonce_for_chunk(h, 1);
  auto big = nonce_for_chunk(h, 70000);
  CHECK_EQ(first.size(), kNonceSize);
  CHECK(first != second);
  CHECK(second != big);
  CHECK_EQ(first[0], uint8_t(0xAA));  // the shared prefix is untouched
  CHECK_EQ(big[8], uint8_t(0x70));
  CHECK_EQ(big[9], uint8_t(0x11));
}

KOLMA_TEST(key_derivation_depends_on_the_salt) {
  ArchiveHeader a = sample_header();
  ArchiveHeader b = sample_header();
  for (size_t i = 0; i < b.salt.size(); ++i) b.salt[i] = uint8_t(i + 1);
  Bytes key_a = derive_key("correct horse battery staple", a);
  Bytes key_b = derive_key("correct horse battery staple", b);
  Bytes key_a_again = derive_key("correct horse battery staple", a);
  CHECK_EQ(key_a.size(), size_t(32));
  CHECK_BYTES_EQ(key_a, key_a_again);
  CHECK(key_a != key_b);
  Bytes other = derive_key("correct horse battery stapl", a);
  CHECK(other != key_a);
}

KOLMA_TEST(archive_info_reports_a_missing_file) {
  ArchiveInfo info = read_archive_info(kolma_test::temp_path("definitely-not-here.kolma"));
  CHECK(!info.ok);
  CHECK(!info.error.empty());
}

KOLMA_TEST(archive_info_reports_a_non_kolma_file) {
  std::string path = kolma_test::temp_path("not-an-archive.kolma");
  kolma_test::write_bytes(path, kolma_test::make_random(4096, 1));
  ArchiveInfo info = read_archive_info(path);
  CHECK(!info.ok);
  CHECK(info.error.find("magic") != std::string::npos);
}

// ---------------------------------------------------------------- hardening --

KOLMA_TEST(archive_header_rejects_a_chunk_count_bomb) {
  // chunk_count is untrusted: 0xFFFFFFFF entries would reserve ~68 GiB if the
  // parser believed it. The buffer's own size must cap the table first.
  Bytes bytes = serialize_header(sample_header());
  patch_u32(bytes, kOffChunkCount, 0xFFFFFFFFu);
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
  CHECK(error.find("chunk table") != std::string::npos);
  CHECK(parsed.chunks.empty());  // refused before any reservation
}

KOLMA_TEST(archive_header_rejects_zero_chunks) {
  Bytes bytes = serialize_header(sample_header());
  patch_u32(bytes, kOffChunkCount, 0);
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
}

KOLMA_TEST(archive_header_rejects_inconsistent_sizes) {
  Bytes bytes = serialize_header(sample_header());
  patch_u64(bytes, kOffOrigSize, 3 * 65536 + 1);  // table no longer adds up
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
  CHECK(error.find("do not match") != std::string::npos);
}

KOLMA_TEST(archive_header_rejects_oversized_chunks) {
  Bytes bytes = serialize_header(sample_header());
  // First chunk record starts after the fixed part and both strings:
  // 83 + 2 + 9 ("notes.txt") + 2 + 8 ("markdown") = 104.
  patch_u64(bytes, 104, 65537);  // orig_size > chunk_size
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
  CHECK(error.find("claims more than the chunk size") != std::string::npos);
}

KOLMA_TEST(archive_header_rejects_extreme_kdf_iterations) {
  Bytes bytes = serialize_header(sample_header());
  patch_u32(bytes, kOffKdfIterations, 0xFFFFFFFFu);  // ~years of PBKDF2
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
  CHECK(error.find("kdf iterations") != std::string::npos);
}

KOLMA_TEST(archive_header_rejects_an_oversized_chunk_size) {
  Bytes bytes = serialize_header(sample_header());
  patch_u64(bytes, 39, kMaxChunkSize + 1);  // chunk_size field
  ArchiveHeader parsed;
  size_t offset = 0;
  std::string error;
  CHECK(!parse_header(View(bytes), parsed, offset, error));
  CHECK(error.find("chunk size out of range") != std::string::npos);
}

KOLMA_TEST(large_headers_load_and_round_trip) {
  // 5000 chunks -> an ~85 KB header, well past the old 64 KiB single read.
  const Bytes payload = kolma_test::make_random(500000, 41);
  const Bytes archive = craft_raw_archive("big.bin", payload, 100);
  const std::string path = kolma_test::temp_path("large-header.kolma");
  kolma_test::write_bytes(path, archive);

  ArchiveInfo info = read_archive_info(path);
  CHECK(info.ok);
  CHECK(info.header_consistent);
  CHECK(info.payload_crc_ok);
  CHECK_EQ(info.header.chunk_count, uint32_t(5000));

  Job back;
  back.mode = Mode::Decompress;
  back.input = path;
  back.output = kolma_test::temp_path("large-header.out");
  Result r = run_job(back);
  CHECK(r.ok);
  CHECK(r.stats.verified);
  CHECK_BYTES_EQ(kolma_test::read_bytes(back.output), payload);
}

KOLMA_TEST(truncated_payloads_are_refused_before_any_work) {
  const Bytes payload = kolma_test::make_repetitive(10000);
  Bytes archive = craft_raw_archive("cut.bin", payload, 4096);
  archive.resize(archive.size() - 100);  // lop off the tail of the payload
  const std::string path = kolma_test::temp_path("truncated.kolma");
  kolma_test::write_bytes(path, archive);

  ArchiveInfo info = read_archive_info(path);
  CHECK(!info.ok);
  CHECK(info.error.find("truncated") != std::string::npos);

  Job back;
  back.mode = Mode::Decompress;
  back.input = path;
  back.output = kolma_test::temp_path("truncated.out");
  Result r = run_job(back);
  CHECK(!r.ok);
  CHECK(!std::filesystem::exists(back.output));  // never even created
}

KOLMA_TEST(unsafe_source_names_never_become_output_paths) {
  const Bytes payload = kolma_test::make_text_like(2000);
  const std::vector<std::string> bad_names = {"../escape.txt", "a/b.txt", ".", "..",
                                              "evil\nname", std::string("nul\0name", 8)};
  for (const std::string& name : bad_names) {
    const Bytes archive = craft_raw_archive(name, payload, 4096);
    const std::string path = kolma_test::temp_path("mystery-name.bin");
    kolma_test::write_bytes(path, archive);
    Job back;
    back.mode = Mode::Decompress;
    back.input = path;
    back.force = true;  // the fallback output is reused across iterations
    Result r = run_job(back);
    CHECK(r.ok);
    CHECK_EQ(r.stats.output_path, path + ".out");  // fell back, did not escape
    CHECK(!std::filesystem::exists(kolma_test::temp_path("escape.txt")));
  }
}

KOLMA_TEST(name_helpers_reject_controls_and_separators) {
  CHECK(safe_file_name("notes.txt"));
  CHECK(safe_file_name("with space.dat"));
  CHECK(!safe_file_name(""));
  CHECK(!safe_file_name("."));
  CHECK(!safe_file_name(".."));
  CHECK(!safe_file_name("a/b"));
  CHECK(!safe_file_name("a\\b"));
  CHECK(!safe_file_name(std::string("a\0b", 3)));
  CHECK(!safe_file_name("a\nb"));
  CHECK(!safe_file_name("\x1b[31mred"));
  CHECK_EQ(sanitize_for_terminal("a\nb\x1b[31m"), std::string("a?b?[31m"));
  CHECK_EQ(sanitize_for_terminal("plain.txt"), std::string("plain.txt"));
}
