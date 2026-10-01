// kolma -- shared primitives: hashing, checksums, stream crypto, small helpers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kolma {

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

// ------------------------------------------------------------------- checksum
// Standard CRC-32 (IEEE 802.3, polynomial 0xEDB88320, reflected).
// crc32({}) == 0 and crc32("123456789") == 0xCBF43926.
uint32_t crc32(View data, uint32_t seed = 0);
std::string crc32_hex(uint32_t value);

// ------------------------------------------------------------------- SHA-256
std::array<uint8_t, 32> sha256(View data);
std::array<uint8_t, 32> sha256(std::string_view text);
std::array<uint8_t, 32> hmac_sha256(View key, View message);
// PBKDF2-HMAC-SHA256 (RFC 8018). out_len must be <= 32 * 2^32 / 1.
Bytes pbkdf2_sha256(std::string_view password, View salt, uint32_t iterations, size_t out_len = 32);

// ------------------------------------------------------------------- ChaCha20
// RFC 8439 ChaCha20 keystream. key must be exactly 32 bytes, nonce exactly 12.
// `counter` is the initial 32-bit block counter (usually 1 or 0).
void chacha20_xor(View key, uint32_t counter, View nonce, View input, uint8_t* output);
void chacha20_xor_inplace(View key, uint32_t counter, View nonce, uint8_t* buffer, size_t length);

// ------------------------------------------------------------------- helpers
std::string to_hex(View data);
std::string to_hex(const std::array<uint8_t, 32>& data);
Bytes from_hex(std::string_view hex);
uint64_t now_unix();
std::string human_size(uint64_t bytes);
std::string human_rate(double megabytes_per_second);
std::string format_ratio(double ratio);
// Constant-time comparison; never short-circuits on the first differing byte.
bool constant_time_equal(View a, View b);

}  // namespace kolma