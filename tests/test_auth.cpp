// Format v2 authentication: Encrypt-then-MAC, tamper rejection, v1 legacy reads.
#include <algorithm>
#include <string>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/archive.hpp"
#include "kolma/engine.hpp"
#include "kolma/util.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

namespace {

std::string auth_dir() {
  static std::string dir = kolma_test::temp_dir() + "/auth";
  std::filesystem::create_directories(dir);
  return dir;
}

// Crafts a version-1 encrypted archive by hand: raw chunks, ChaCha20 with the
// legacy single-key schedule, no MAC. This is what older kolma builds wrote.
Bytes craft_v1_encrypted(const std::string& password, const Bytes& payload) {
  ArchiveHeader h;
  h.version = 1;
  h.algo_index = algorithm_index("store");
  h.level = 1;
  h.flags = kArchiveEncrypted;
  h.kdf_iterations = 1000;  // fast, and within the supported range
  h.orig_size = payload.size();
  h.orig_crc = crc32(View(payload));
  h.chunk_size = 4096;
  h.source_name = "legacy.dat";
  for (size_t i = 0; i < h.salt.size(); ++i) h.salt[i] = uint8_t(i * 7 + 1);
  for (size_t i = 0; i < h.nonce.size(); ++i) h.nonce[i] = uint8_t(i * 13 + 5);
  Bytes key = derive_key(password, h);
  Bytes ciphertext;
  size_t off = 0;
  uint32_t idx = 0;
  while (off < payload.size()) {
    const size_t len = std::min<size_t>(4096, payload.size() - off);
    Bytes chunk(payload.begin() + std::ptrdiff_t(off),
                payload.begin() + std::ptrdiff_t(off + len));
    auto nonce = nonce_for_chunk(h, idx);
    chacha20_xor_inplace(View(key), 1, View(nonce), chunk.data(), chunk.size());
    ciphertext.insert(ciphertext.end(), chunk.begin(), chunk.end());
    h.chunks.push_back(ChunkRecord{len, len, kChunkStoredRaw});
    off += len;
    ++idx;
  }
  if (h.chunks.empty()) h.chunks.push_back(ChunkRecord{0, 0, kChunkStoredRaw});
  h.chunk_count = uint32_t(h.chunks.size());
  h.comp_size = ciphertext.size();
  h.payload_crc = crc32(View(ciphertext));
  Bytes out = serialize_header(h);
  out.insert(out.end(), ciphertext.begin(), ciphertext.end());
  return out;
}

std::string compress_encrypted(const std::string& tag, const Bytes& data,
                               const std::string& password, const std::string& algo = "lz77",
                               size_t chunk_size = 0) {
  const std::string in = auth_dir() + "/" + tag + ".in";
  const std::string arc = auth_dir() + "/" + tag + ".kolma";
  kolma_test::write_bytes(in, data);
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = arc;
  job.algorithm = algo;
  job.chunk_size = chunk_size;
  job.password = password;
  Result r = run_job(job);
  if (!r.ok) throw kolma_test::Failure("compress failed: " + r.error);
  if (!r.stats.authenticated)
    throw kolma_test::Failure("a new encrypted archive must be authenticated");
  return arc;
}

}  // namespace

KOLMA_TEST(authenticated_archives_round_trip) {
  const Bytes data = kolma_test::make_repetitive(120000);
  const std::string arc = compress_encrypted("roundtrip", data, "correct horse battery staple");

  ArchiveInfo info = read_archive_info(arc);
  CHECK(info.ok);
  CHECK(info.encrypted);
  CHECK(info.authenticated);

  Job back;
  back.mode = Mode::Decompress;
  back.input = arc;
  back.output = auth_dir() + "/roundtrip.out";
  back.password = "correct horse battery staple";
  Result r = run_job(back);
  CHECK(r.ok);
  CHECK(r.stats.verified);
  CHECK(r.stats.authenticated);
  CHECK_BYTES_EQ(kolma_test::read_bytes(back.output), data);
}

KOLMA_TEST(tampered_payload_fails_authentication_before_output) {
  const std::string arc =
      compress_encrypted("tamper-payload", kolma_test::make_repetitive(60000), "pw");
  Bytes raw = kolma_test::read_bytes(arc);
  raw[raw.size() - 1] ^= 0x01;  // one bit in the ciphertext
  const std::string damaged = auth_dir() + "/tamper-payload.kolma";
  kolma_test::write_bytes(damaged, raw);

  Job back;
  back.mode = Mode::Decompress;
  back.input = damaged;
  back.output = auth_dir() + "/tamper-payload.out";
  back.password = "pw";
  Result r = run_job(back);
  CHECK(!r.ok);
  CHECK(r.error.find("authentication") != std::string::npos);
  CHECK(!std::filesystem::exists(back.output));  // refused before writing anything
}

KOLMA_TEST(tampered_header_fails_authentication) {
  const std::string arc =
      compress_encrypted("tamper-header", kolma_test::make_repetitive(60000), "pw");
  Bytes raw = kolma_test::read_bytes(arc);
  // orig_crc lives at offset 27; flipping it keeps the size table consistent,
  // so only the MAC can catch it.
  raw[27] ^= 0x01;
  const std::string damaged = auth_dir() + "/tamper-header.kolma";
  kolma_test::write_bytes(damaged, raw);

  ArchiveInfo info = read_archive_info(damaged);
  CHECK(info.ok);               // structurally fine...
  CHECK(info.header_consistent);

  Job back;
  back.mode = Mode::Decompress;
  back.input = damaged;
  back.output = auth_dir() + "/tamper-header.out";
  back.password = "pw";
  Result r = run_job(back);
  CHECK(!r.ok);                 // ...but not authentic
  CHECK(r.error.find("authentication") != std::string::npos);
  CHECK(!std::filesystem::exists(back.output));
}

KOLMA_TEST(tampered_salt_fails_authentication) {
  const std::string arc =
      compress_encrypted("tamper-salt", kolma_test::make_repetitive(60000), "pw");
  Bytes raw = kolma_test::read_bytes(arc);
  raw[55] ^= 0x01;  // salt: changes the derived keys, MAC must notice
  const std::string damaged = auth_dir() + "/tamper-salt.kolma";
  kolma_test::write_bytes(damaged, raw);

  Job back;
  back.mode = Mode::Decompress;
  back.input = damaged;
  back.output = auth_dir() + "/tamper-salt.out";
  back.password = "pw";
  Result r = run_job(back);
  CHECK(!r.ok);
  CHECK(r.error.find("authentication") != std::string::npos);
}

KOLMA_TEST(wrong_password_fails_authentication) {
  const std::string arc =
      compress_encrypted("wrong-pw", kolma_test::make_repetitive(60000), "right");
  Job back;
  back.mode = Mode::Decompress;
  back.input = arc;
  back.output = auth_dir() + "/wrong-pw.out";
  back.password = "wrong";
  Result r = run_job(back);
  CHECK(!r.ok);
  CHECK(r.error.find("authentication") != std::string::npos);
  CHECK(!std::filesystem::exists(back.output));
}

KOLMA_TEST(chunk_reordering_fails_authentication) {
  // Two equal-size raw chunks: swapping their ciphertext keeps every size
  // consistent, so only the MAC stands between the reorder and the output.
  const Bytes data = kolma_test::make_random(8192, 77);
  const std::string arc = compress_encrypted("reorder", data, "pw", "store", 4096);
  Bytes raw = kolma_test::read_bytes(arc);
  ArchiveHeader h;
  size_t payload_offset = 0;
  std::string error;
  CHECK(load_archive_header(arc, h, payload_offset, error));
  CHECK_EQ(h.chunk_count, uint32_t(2));
  CHECK_EQ(h.chunks[0].comp_size, uint64_t(4096));
  CHECK_EQ(h.chunks[1].comp_size, uint64_t(4096));
  std::swap_ranges(raw.begin() + std::ptrdiff_t(payload_offset),
                   raw.begin() + std::ptrdiff_t(payload_offset + 4096),
                   raw.begin() + std::ptrdiff_t(payload_offset + 4096));
  const std::string damaged = auth_dir() + "/reorder.kolma";
  kolma_test::write_bytes(damaged, raw);

  Job back;
  back.mode = Mode::Decompress;
  back.input = damaged;
  back.output = auth_dir() + "/reorder.out";
  back.password = "pw";
  Result r = run_job(back);
  CHECK(!r.ok);
  CHECK(r.error.find("authentication") != std::string::npos);
}

KOLMA_TEST(v1_legacy_archives_still_read) {
  const Bytes data = kolma_test::make_text_like(50000);
  const std::string password = "legacy password";
  const Bytes archive = craft_v1_encrypted(password, data);
  const std::string path = auth_dir() + "/legacy.kolma";
  kolma_test::write_bytes(path, archive);

  ArchiveInfo info = read_archive_info(path);
  CHECK(info.ok);
  CHECK(info.encrypted);
  CHECK(!info.authenticated);  // honestly reported as unauthenticated
  CHECK_EQ(info.header.version, uint16_t(1));
  CHECK(info.payload_crc_ok);

  Job back;
  back.mode = Mode::Decompress;
  back.input = path;
  back.output = auth_dir() + "/legacy.out";
  back.password = password;
  Result r = run_job(back);
  CHECK(r.ok);
  CHECK(r.stats.verified);
  CHECK(!r.stats.authenticated);
  CHECK_BYTES_EQ(kolma_test::read_bytes(back.output), data);

  // A wrong password on a legacy archive still fails (via the CRC path).
  Job wrong;
  wrong.mode = Mode::Decompress;
  wrong.input = path;
  wrong.output = auth_dir() + "/legacy-wrong.out";
  wrong.password = "not it";
  Result rw = run_job(wrong);
  CHECK(!rw.ok);
}

KOLMA_TEST(unencrypted_v2_archives_carry_a_zero_mac) {
  const std::string in = auth_dir() + "/plain.in";
  const std::string arc = auth_dir() + "/plain.kolma";
  kolma_test::write_bytes(in, kolma_test::make_repetitive(30000));
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = arc;
  job.algorithm = "lz77";
  Result r = run_job(job);
  CHECK(r.ok);
  CHECK(!r.stats.encrypted);
  CHECK(!r.stats.authenticated);

  ArchiveHeader h;
  size_t payload_offset = 0;
  std::string error;
  CHECK(load_archive_header(arc, h, payload_offset, error));
  CHECK_EQ(h.version, kArchiveVersion);
  CHECK_EQ(h.mac.size(), kMacSize);
  for (uint8_t b : h.mac) CHECK_EQ(int(b), 0);

  Job back;
  back.mode = Mode::Decompress;
  back.input = arc;
  back.output = auth_dir() + "/plain.out";
  Result rb = run_job(back);
  CHECK(rb.ok);
  CHECK(rb.stats.verified);
  CHECK(!rb.stats.authenticated);
}

KOLMA_TEST(streaming_hmac_matches_the_one_shot) {
  const Bytes key = kolma_test::make_random(48, 9);  // > 32 bytes exercises key hashing
  const Bytes message = kolma_test::make_text_like(100003);
  const auto expected = hmac_sha256(View(key), View(message));

  HmacSha256 streamed{View(key)};
  size_t off = 0;
  size_t step = 1;
  while (off < message.size()) {  // deliberately uneven, tiny updates
    const size_t len = std::min(step, message.size() - off);
    streamed.update(View(message.data() + off, len));
    off += len;
    step = step * 2 + 1;
  }
  auto tag = streamed.final();
  CHECK_BYTES_EQ(Bytes(tag.begin(), tag.end()), Bytes(expected.begin(), expected.end()));

  // Empty message and empty key must agree with the one-shot too.
  HmacSha256 empty{View(Bytes{})};
  empty.update(View(Bytes{}));
  const auto e = hmac_sha256(View(Bytes{}), View(Bytes{}));
  auto empty_tag = empty.final();
  CHECK_BYTES_EQ(Bytes(empty_tag.begin(), empty_tag.end()), Bytes(e.begin(), e.end()));
}

KOLMA_TEST(v2_key_schedule_separates_encryption_and_mac_keys) {
  ArchiveHeader h;
  h.flags = kArchiveEncrypted;
  h.kdf_iterations = 1000;
  for (size_t i = 0; i < h.salt.size(); ++i) h.salt[i] = uint8_t(i);
  Bytes enc, mac;
  derive_keys("password", h, enc, mac);
  CHECK_EQ(enc.size(), size_t(32));
  CHECK_EQ(mac.size(), size_t(32));
  CHECK(enc != mac);

  // v1: one key, no MAC key.
  h.version = 1;
  Bytes enc1, mac1;
  derive_keys("password", h, enc1, mac1);
  CHECK_EQ(enc1.size(), size_t(32));
  CHECK(mac1.empty());
  CHECK_BYTES_EQ(enc1, derive_key("password", h));
}
