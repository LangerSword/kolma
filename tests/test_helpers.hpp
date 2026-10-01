// Shared fixtures for the test suite: temp files and realistic input shapes.
#pragma once

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace kolma_test {

inline std::string temp_dir() {
  static std::string base;
  if (base.empty()) {
    base = (std::filesystem::temp_directory_path() /
            ("kolma-tests-" + std::to_string(::getpid())))
               .string();
    std::filesystem::create_directories(base);
  }
  return base;
}

inline std::string temp_path(const std::string& name) { return temp_dir() + "/" + name; }

inline void write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

inline std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data;
}

inline std::vector<uint8_t> make_random(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> data(n);
  for (uint8_t& b : data) b = uint8_t(rng() & 0xFF);
  return data;
}

inline std::vector<uint8_t> make_repetitive(size_t n) {
  const std::string unit =
      "the quick brown fox jumps over the lazy dog. kolma chunks the input and compresses it. ";
  std::vector<uint8_t> data;
  data.reserve(n);
  while (data.size() < n) {
    size_t take = std::min(unit.size(), n - data.size());
    data.insert(data.end(), unit.begin(), unit.begin() + std::ptrdiff_t(take));
  }
  return data;
}

// Sensor-log shaped 16-bit samples: a value that drifts upward and then holds
// flat for long stretches. delta-rle is the "Delta/RLE for specific data"
// method in the design, so this is the shape it must be judged on: piecewise
// constant, not merely smooth. A signal whose bytes change on every sample
// gives RLE nothing to work with.
inline std::vector<uint8_t> make_smooth_numeric(size_t n) {
  std::vector<uint8_t> data;
  data.reserve(n);
  uint16_t value = 1000;
  for (size_t i = 0; i < n / 2; ++i) {
    if (i % 200 < 4) value = uint16_t(value + 1);  // short ramp
    // then 196 flat samples, so the delta stream is a long run of zeros
    data.push_back(uint8_t(value & 0xFF));
    data.push_back(uint8_t(value >> 8));
  }
  return data;
}

// Repeating byte runs: the shape rle is built for.
inline std::vector<uint8_t> make_flat_runs(size_t n) {
  std::vector<uint8_t> data;
  data.reserve(n);
  uint8_t value = 0;
  while (data.size() < n) {
    size_t run = std::min<size_t>(200, n - data.size());
    data.insert(data.end(), run, value);
    ++value;
  }
  return data;
}

inline std::vector<uint8_t> make_text_like(size_t n) {
  // A low-alphabet, locally repetitive byte stream: realistic text statistics
  // without shipping a large fixture.
  static const char* words[] = {"compression", "archive",  "engine", "analyzer", "algorithm",
                               "chunk",       "checksum", "ratio",  "thread",   "stream"};
  std::vector<uint8_t> data;
  data.reserve(n);
  std::mt19937 rng(1234);
  while (data.size() < n) {
    const char* w = words[rng() % 10];
    for (const char* p = w; *p; ++p) data.push_back(uint8_t(*p));
    data.push_back(' ');
  }
  data.resize(n);
  return data;
}

}  // namespace kolma_test