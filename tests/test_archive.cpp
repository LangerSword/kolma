// Archive header serialisation, validation and key derivation.
#include <string>

#include "kolma/archive.hpp"
#include "kolma/algorithm.hpp"
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
  h.orig_size = 1234567;
  h.comp_size = 456789;
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
