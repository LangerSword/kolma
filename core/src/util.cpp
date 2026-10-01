#include "kolma/util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace kolma {

// ------------------------------------------------------------------ CRC-32 ---
namespace {
const uint32_t* crc_table() {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = true;
  }
  return table;
}
}  // namespace

uint32_t crc32(View data, uint32_t seed) {
  const uint32_t* t = crc_table();
  uint32_t c = seed ^ 0xFFFFFFFFu;
  for (uint8_t b : data) c = t[(c ^ b) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::string crc32_hex(uint32_t value) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", value);
  return buf;
}

// ----------------------------------------------------------------- SHA-256 ---
namespace {

constexpr uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256Ctx {
  uint32_t h[8];
  uint64_t total = 0;
  uint8_t buf[64];
  size_t buflen = 0;
};

void sha256_init(Sha256Ctx& c) {
  c.h[0] = 0x6a09e667u;
  c.h[1] = 0xbb67ae85u;
  c.h[2] = 0x3c6ef372u;
  c.h[3] = 0xa54ff53au;
  c.h[4] = 0x510e527fu;
  c.h[5] = 0x9b05688cu;
  c.h[6] = 0x1f83d9abu;
  c.h[7] = 0x5be0cd19u;
}

void sha256_block(Sha256Ctx& c, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
           (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
  uint32_t e = c.h[4], f = c.h[5], g = c.h[6], hh = c.h[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
  c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += hh;
}

void sha256_update(Sha256Ctx& c, const uint8_t* p, size_t n) {
  c.total += n;
  while (n > 0) {
    size_t take = std::min<size_t>(64 - c.buflen, n);
    std::memcpy(c.buf + c.buflen, p, take);
    c.buflen += take;
    p += take;
    n -= take;
    if (c.buflen == 64) {
      sha256_block(c, c.buf);
      c.buflen = 0;
    }
  }
}

std::array<uint8_t, 32> sha256_final(Sha256Ctx& c) {
  uint64_t bits = c.total * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c.buflen != 56) sha256_update(c, &zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - i * 8));
  // Bypass the length accounting: the padding bytes are not message bytes.
  c.total -= (c.buflen == 0 ? 0 : 0);
  std::memcpy(c.buf + c.buflen, len, 8);
  c.buflen = 64;
  sha256_block(c, c.buf);
  std::array<uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = uint8_t(c.h[i] >> 24);
    out[i * 4 + 1] = uint8_t(c.h[i] >> 16);
    out[i * 4 + 2] = uint8_t(c.h[i] >> 8);
    out[i * 4 + 3] = uint8_t(c.h[i]);
  }
  return out;
}

}  // namespace

std::array<uint8_t, 32> sha256(View data) {
  Sha256Ctx c;
  sha256_init(c);
  sha256_update(c, data.data(), data.size());
  return sha256_final(c);
}

std::array<uint8_t, 32> sha256(std::string_view text) {
  return sha256(View(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::array<uint8_t, 32> hmac_sha256(View key, View message) {
  constexpr size_t B = 64;
  uint8_t k[B] = {0};
  if (key.size() > B) {
    auto d = sha256(key);
    std::memcpy(k, d.data(), d.size());
  } else {
    std::memcpy(k, key.data(), key.size());
  }
  uint8_t ipad[B], opad[B];
  for (size_t i = 0; i < B; ++i) {
    ipad[i] = uint8_t(k[i] ^ 0x36);
    opad[i] = uint8_t(k[i] ^ 0x5C);
  }
  Sha256Ctx inner;
  sha256_init(inner);
  sha256_update(inner, ipad, B);
  sha256_update(inner, message.data(), message.size());
  auto ih = sha256_final(inner);

  Sha256Ctx outer;
  sha256_init(outer);
  sha256_update(outer, opad, B);
  sha256_update(outer, ih.data(), ih.size());
  return sha256_final(outer);
}

Bytes pbkdf2_sha256(std::string_view password, View salt, uint32_t iterations, size_t out_len) {
  if (iterations == 0) throw std::invalid_argument("pbkdf2: iterations must be >= 1");
  const auto* pw = reinterpret_cast<const uint8_t*>(password.data());
  View pw_view(pw, password.size());
  Bytes out;
  out.reserve(out_len);
  uint32_t block_index = 1;
  while (out.size() < out_len) {
    Bytes msg;
    msg.reserve(salt.size() + 4);
    msg.insert(msg.end(), salt.begin(), salt.end());
    msg.push_back(uint8_t(block_index >> 24));
    msg.push_back(uint8_t(block_index >> 16));
    msg.push_back(uint8_t(block_index >> 8));
    msg.push_back(uint8_t(block_index));
    auto u = hmac_sha256(pw_view, View(msg));
    std::array<uint8_t, 32> acc = u;
    for (uint32_t i = 1; i < iterations; ++i) {
      u = hmac_sha256(pw_view, View(u));
      for (size_t j = 0; j < 32; ++j) acc[j] ^= u[j];
    }
    out.insert(out.end(), acc.begin(), acc.end());
    ++block_index;
  }
  out.resize(out_len);
  return out;
}

// ---------------------------------------------------------------- ChaCha20 ---
namespace {

constexpr uint32_t SIGMA[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};

inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void qr(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
  a += b; d ^= a; d = rotl32(d, 16);
  c += d; b ^= c; b = rotl32(b, 12);
  a += b; d ^= a; d = rotl32(d, 8);
  c += d; b ^= c; b = rotl32(b, 7);
}

inline uint32_t load32le(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void store32le(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

void chacha20_block(const uint32_t state[16], uint8_t out[64]) {
  uint32_t x[16];
  std::memcpy(x, state, sizeof(x));
  for (int i = 0; i < 10; ++i) {
    qr(x[0], x[4], x[8], x[12]);
    qr(x[1], x[5], x[9], x[13]);
    qr(x[2], x[6], x[10], x[14]);
    qr(x[3], x[7], x[11], x[15]);
    qr(x[0], x[5], x[10], x[15]);
    qr(x[1], x[6], x[11], x[12]);
    qr(x[2], x[7], x[8], x[13]);
    qr(x[3], x[4], x[9], x[14]);
  }
  for (int i = 0; i < 16; ++i) store32le(out + i * 4, x[i] + state[i]);
}

void chacha20_setup(uint32_t state[16], View key, uint32_t counter, View nonce) {
  if (key.size() != 32) throw std::invalid_argument("chacha20: key must be 32 bytes");
  if (nonce.size() != 12) throw std::invalid_argument("chacha20: nonce must be 12 bytes");
  state[0] = SIGMA[0]; state[1] = SIGMA[1]; state[2] = SIGMA[2]; state[3] = SIGMA[3];
  for (int i = 0; i < 8; ++i) state[4 + i] = load32le(key.data() + i * 4);
  state[12] = counter;
  for (int i = 0; i < 3; ++i) state[13 + i] = load32le(nonce.data() + i * 4);
}

}  // namespace

void chacha20_xor(View key, uint32_t counter, View nonce, View input, uint8_t* output) {
  uint32_t state[16];
  chacha20_setup(state, key, counter, nonce);
  uint8_t keystream[64];
  size_t off = 0;
  while (off < input.size()) {
    chacha20_block(state, keystream);
    ++state[12];
    if (state[12] == 0) ++state[13];  // carry, never triggered below 256 GiB
    size_t take = std::min<size_t>(64, input.size() - off);
    for (size_t i = 0; i < take; ++i) output[off + i] = uint8_t(input[off + i] ^ keystream[i]);
    off += take;
  }
}

void chacha20_xor_inplace(View key, uint32_t counter, View nonce, uint8_t* buffer, size_t length) {
  chacha20_xor(key, counter, nonce, View(buffer, length), buffer);
}

// ---------------------------------------------------------------- helpers ----
std::string to_hex(View data) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xF]);
  }
  return out;
}

std::string to_hex(const std::array<uint8_t, 32>& data) { return to_hex(View(data)); }

Bytes from_hex(std::string_view hex) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Bytes out;
  out.reserve(hex.size() / 2);
  int hi = -1;
  for (char c : hex) {
    if (c == ' ' || c == '\n' || c == '\t') continue;
    int v = nib(c);
    if (v < 0) throw std::invalid_argument("from_hex: bad hex digit");
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(uint8_t((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

uint64_t now_unix() {
  return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
}

std::string human_size(uint64_t bytes) {
  static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = double(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 5) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u == 0)
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  else
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
  return buf;
}

std::string human_rate(double mbps) {
  char buf[64];
  if (mbps >= 1000.0)
    std::snprintf(buf, sizeof(buf), "%.2f GB/s", mbps / 1000.0);
  else
    std::snprintf(buf, sizeof(buf), "%.2f MB/s", mbps);
  return buf;
}

std::string format_ratio(double ratio) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", ratio);
  return buf;
}

bool constant_time_equal(View a, View b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
  return diff == 0;
}

}  // namespace kolma
