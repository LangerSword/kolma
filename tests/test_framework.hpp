// A very small test framework: registration by static initialisation, no deps.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kolma_test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const std::string& name, std::function<void()> fn) {
    registry().push_back(TestCase{name, std::move(fn)});
  }
};

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return std::string("\"") + (v ? v : "(null)") + "\""; }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(char v) { return std::string("'") + v + "'"; }
inline std::string show(signed char v) { return std::to_string(int(v)); }
inline std::string show(unsigned char v) { return std::to_string(int(v)); }
inline std::string show(unsigned long long v) { return std::to_string(v); }
inline std::string show(long long v) { return std::to_string(v); }

template <typename T>
std::string show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

inline std::string bytes_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  std::ostringstream os;
  os << "sizes " << a.size() << " vs " << b.size();
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      os << ", first difference at byte " << i << " (" << int(a[i]) << " vs " << int(b[i]) << ")";
      return os.str();
    }
  }
  return os.str();
}

}  // namespace kolma_test

#define KOLMA_TEST(name)                                        \
  static void name();                                           \
  static ::kolma_test::Registrar kolma_reg_##name(#name, name);  \
  static void name()

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond))                                                                        \
      throw ::kolma_test::Failure(std::string(__FILE__) + ":" +                         \
                                  std::to_string(__LINE__) + ": CHECK failed: " #cond);  \
  } while (0)

#define CHECK_EQ(actual, expected)                                                        \
  do {                                                                                    \
    const auto& a_ = (actual);                                                            \
    const auto& e_ = (expected);                                                          \
    if (!(a_ == e_))                                                                      \
      throw ::kolma_test::Failure(std::string(__FILE__) + ":" +                           \
                                  std::to_string(__LINE__) + ": CHECK_EQ failed: " #actual \
                                  " = " + ::kolma_test::show(a_) + ", expected " +        \
                                  ::kolma_test::show(e_));                                \
  } while (0)

#define CHECK_THROWS(expr)                                                              \
  do {                                                                                  \
    bool threw_ = false;                                                                \
    try {                                                                               \
      (void)(expr);                                                                     \
    } catch (const std::exception&) {                                                   \
      threw_ = true;                                                                    \
    }                                                                                   \
    if (!threw_)                                                                        \
      throw ::kolma_test::Failure(std::string(__FILE__) + ":" +                         \
                                  std::to_string(__LINE__) + ": expected an exception from " #expr); \
  } while (0)

#define CHECK_BYTES_EQ(actual, expected)                                             \
  do {                                                                               \
    const auto& a_ = (actual);                                                       \
    const auto& e_ = (expected);                                                     \
    if (!(a_ == e_))                                                                 \
      throw ::kolma_test::Failure(std::string(__FILE__) + ":" +                      \
                                  std::to_string(__LINE__) + ": byte mismatch: " +   \
                                  ::kolma_test::bytes_diff(a_, e_));                 \
  } while (0)
