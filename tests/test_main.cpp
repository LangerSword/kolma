#include <iostream>

#include "test_framework.hpp"

int main() {
  int failed = 0;
  int total = 0;
  for (const kolma_test::TestCase& test : kolma_test::registry()) {
    ++total;
    try {
      test.fn();
      std::cout << "  ok    " << test.name << "\n";
    } catch (const std::exception& e) {
      ++failed;
      std::cout << "  FAIL  " << test.name << "\n        " << e.what() << "\n";
    }
  }
  std::cout << "\n" << (total - failed) << "/" << total << " tests passed\n";
  return failed == 0 ? 0 : 1;
}
