// Published test vectors: if these pass, the primitives are the real thing and
// not something that merely round-trips with itself.
#include <array>
#include <string>
#include <vector>

#include "kolma/bitio.hpp"
#include "kolma/util.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

KOLMA_TEST(crc32_matches_the_ieee_vector) {
  CHECK_EQ(crc32(View{}), 0x00000000u);
  const std::string check = "123456789";
  CHECK_EQ(crc32(View(reinterpret_cast<const uint8_t*>(check.data()), check.size())),
           0xCBF43926u);
  CHECK_EQ(crc32_hex(0xCBF43926u), std::string("cbf43926"));
}

KOLMA_TEST(crc32_is_incremental) {
  auto a = std::vector<uint8_t>{1, 2, 3, 4, 5};
  auto b = std::vector<uint8_t>{6, 7, 8, 9};
  auto both = std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint32_t running = crc32(View(a));
  running = crc32(View(b), running);
  CHECK_EQ(running, crc32(View(both)));
}

KOLMA_TEST(sha256_matches_fips_vectors) {
  CHECK_EQ(to_hex(sha256(std::string_view(""))),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(to_hex(sha256(std::string_view("abc"))),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(to_hex(sha256(std::string_view(
               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

KOLMA_TEST(hmac_sha256_matches_rfc4231) {
  Bytes key(20, 0x0b);
  const std::string data = "Hi There";
  CHECK_EQ(to_hex(hmac_sha256(View(key), View(reinterpret_cast<const uint8_t*>(data.data()),
                                              data.size()))),
           std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
}

KOLMA_TEST(pbkdf2_sha256_matches_rfc_vectors) {
  const std::string salt = "salt";
  View salt_view(reinterpret_cast<const uint8_t*>(salt.data()), salt.size());
  CHECK_EQ(to_hex(pbkdf2_sha256("password", salt_view, 1, 32)),
           std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
  CHECK_EQ(to_hex(pbkdf2_sha256("password", salt_view, 4096, 32)),
           std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
  const std::string long_salt = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
  CHECK_EQ(to_hex(pbkdf2_sha256(
               "passwordPASSWORDpassword",
               View(reinterpret_cast<const uint8_t*>(long_salt.data()), long_salt.size()), 4096, 40)),
           std::string("348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1"
                       "c635518c7dac47e9"));
}

KOLMA_TEST(chacha20_matches_rfc8439) {
  Bytes key;
  for (int i = 0; i < 32; ++i) key.push_back(uint8_t(i));
  Bytes nonce = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
  const std::string plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
      "future, sunscreen would be it.";
  Bytes input(plaintext.begin(), plaintext.end());
  Bytes output(input.size());
  chacha20_xor(View(key), 1, View(nonce), View(input), output.data());
  CHECK_EQ(to_hex(View(output)),
           std::string("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
                       "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
                       "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
                       "5af90bbf74a35be6b40b8eedf2785e42874d"));

  // Encryption and decryption are the same operation.
  Bytes roundtrip(input.size());
  chacha20_xor(View(key), 1, View(nonce), View(output), roundtrip.data());
  CHECK_BYTES_EQ(roundtrip, input);
}

KOLMA_TEST(chacha20_handles_non_block_sizes) {
  Bytes key(32, 0x5A);
  Bytes nonce(12, 0x11);
  for (size_t n : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(1000)}) {
    Bytes input = kolma_test::make_random(n, uint32_t(n));
    Bytes cipher(n), back(n);
    chacha20_xor(View(key), 7, View(nonce), View(input), cipher.data());
    chacha20_xor(View(key), 7, View(nonce), View(cipher), back.data());
    CHECK_BYTES_EQ(back, input);
    if (n > 0) CHECK(input != cipher);
  }
}

KOLMA_TEST(varints_round_trip) {
  for (uint64_t v : {uint64_t(0), uint64_t(1), uint64_t(127), uint64_t(128), uint64_t(300),
                     uint64_t(1) << 31, UINT64_MAX}) {
    Bytes buffer;
    put_varint(buffer, v);
    size_t pos = 0;
    uint64_t back = 0;
    CHECK(get_varint(View(buffer), pos, back));
    CHECK_EQ(back, v);
    CHECK_EQ(pos, buffer.size());
  }
  // Truncated varint must be rejected, not guessed.
  Bytes truncated{0x80};
  size_t pos = 0;
  uint64_t value = 0;
  CHECK(!get_varint(View(truncated), pos, value));
}

KOLMA_TEST(bit_io_round_trips_across_byte_boundaries) {
  const std::vector<std::pair<uint32_t, int>> values = {
      {0b1, 1}, {0b101, 3}, {0xFFFF, 16}, {0x1234, 16}, {0x7F, 7}, {0, 5}, {0x1FF, 9}};
  Bytes buffer;
  BitWriter writer(buffer);
  for (const auto& [value, bits] : values) writer.put(value, bits);
  writer.flush();

  BitReader reader{View(buffer)};
  for (const auto& [value, bits] : values) {
    uint32_t got = 0;
    CHECK(reader.get(bits, got));
    CHECK_EQ(got, value);
  }
  uint32_t extra = 0;
  CHECK(!reader.get(1, extra) || extra == 0);  // only padding bits remain
}

KOLMA_TEST(hex_and_size_helpers) {
  CHECK_EQ(to_hex(from_hex("deadbeef")), std::string("deadbeef"));
  CHECK_EQ(from_hex("00 FF").size(), size_t(2));
  CHECK_THROWS(from_hex("zz"));
  CHECK_EQ(human_size(0), std::string("0 B"));
  CHECK_EQ(human_size(1024), std::string("1.00 KiB"));
  CHECK_EQ(human_size(1536), std::string("1.50 KiB"));
  CHECK(constant_time_equal(View{}, View{}));
  CHECK(!constant_time_equal(View{}, View(std::vector<uint8_t>{1})));
}