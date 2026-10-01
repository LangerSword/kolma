// Round-trip and corruption tests for every algorithm in the registry.
#include <string>
#include <utility>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

namespace {

using Fixture = std::pair<std::string, Bytes>;

std::vector<Fixture> fixtures() {
  std::vector<Fixture> f;
  f.emplace_back("empty", Bytes{});
  f.emplace_back("one byte", Bytes{0x42});
  f.emplace_back("banana", Bytes{'b', 'a', 'n', 'a', 'n', 'a'});
  f.emplace_back("flat runs 64K", kolma_test::make_flat_runs(65536));
  f.emplace_back("text-like 64K", kolma_test::make_text_like(65536));
  f.emplace_back("repetitive 96K", kolma_test::make_repetitive(98304));
  f.emplace_back("smooth numeric 64K", kolma_test::make_smooth_numeric(65536));
  f.emplace_back("random 64K", kolma_test::make_random(65536, 7));
  f.emplace_back("random 70K", kolma_test::make_random(70000, 11));  // crosses LZW boundaries
  f.emplace_back("prime size 100003", kolma_test::make_repetitive(100003));
  return f;
}

void round_trip(const Algorithm* algo, const Bytes& input, int level) {
  Bytes compressed;
  algo->compress_block(View(input), compressed, level);
  Bytes restored;
  algo->decompress_block(View(compressed), restored, input.size());
  if (restored != input) {
    throw kolma_test::Failure("round-trip failed for " + algo->params().id + " at level " +
                              std::to_string(level) + ": " +
                              kolma_test::bytes_diff(restored, input));
  }
}

}  // namespace

KOLMA_TEST(registry_is_stable_and_complete) {
  CHECK_EQ(all_algorithms().size(), size_t(7));
  CHECK_EQ(algorithm_index("store"), uint8_t(0));
  CHECK_EQ(algorithm_index("rle"), uint8_t(1));
  CHECK_EQ(algorithm_index("huffman"), uint8_t(2));
  CHECK_EQ(algorithm_index("lz77"), uint8_t(3));
  CHECK_EQ(algorithm_index("lzw"), uint8_t(4));
  CHECK_EQ(algorithm_index("bwt-mtf-ari"), uint8_t(5));
  CHECK_EQ(algorithm_index("delta-rle"), uint8_t(6));
  CHECK_EQ(algorithm_by_index(6)->params().id, std::string("delta-rle"));
  CHECK(algorithm_by_index(200) == nullptr);
  CHECK(find_algorithm("nope") == nullptr);
  CHECK_EQ(clamp_level(0), 1);
  CHECK_EQ(clamp_level(99), 9);
}

KOLMA_TEST(every_algorithm_round_trips_every_fixture) {
  for (const Algorithm* algo : all_algorithms()) {
    for (const Fixture& f : fixtures()) {
      for (int level : {1, 6, 9}) {
        round_trip(algo, f.second, level);
      }
    }
  }
}

KOLMA_TEST(algorithms_shrink_the_data_they_are_built_for) {
  Bytes flat = kolma_test::make_flat_runs(65536);
  Bytes text = kolma_test::make_repetitive(65536);
  Bytes smooth = kolma_test::make_smooth_numeric(65536);
  Bytes noise = kolma_test::make_random(65536, 99);

  auto compressed_size = [](const char* id, const Bytes& data, int level) {
    Bytes out;
    find_algorithm(id)->compress_block(View(data), out, level);
    return out.size();
  };

  CHECK(compressed_size("store", text, 6) == text.size());
  CHECK(compressed_size("rle", flat, 6) < flat.size() / 20);
  CHECK(compressed_size("huffman", text, 6) < text.size() * 3 / 4);
  CHECK(compressed_size("lz77", text, 6) < text.size() / 4);
  CHECK(compressed_size("lzw", text, 6) < text.size() / 3);
  CHECK(compressed_size("bwt-mtf-ari", text, 6) < text.size() / 4);
  CHECK(compressed_size("delta-rle", smooth, 6) < smooth.size() / 3);
  // Nothing may claim to shrink pure noise by a meaningful margin.
  CHECK(compressed_size("lz77", noise, 6) > noise.size() * 9 / 10);
}

KOLMA_TEST(corruption_is_detected_or_changes_the_output) {
  Bytes text = kolma_test::make_text_like(8192);
  for (const Algorithm* algo : all_algorithms()) {
    Bytes compressed;
    algo->compress_block(View(text), compressed, 6);
    if (compressed.size() < 16) continue;
    Bytes damaged = compressed;
    damaged[damaged.size() / 2] ^= 0x40;
    bool noticed = false;
    try {
      Bytes restored;
      algo->decompress_block(View(damaged), restored, text.size());
      noticed = (restored != text);
    } catch (const std::exception&) {
      noticed = true;
    }
    if (!noticed) {
      throw kolma_test::Failure(algo->params().id +
                                ": a flipped bit produced identical output, so corruption would "
                                "go unnoticed");
    }
  }
}

KOLMA_TEST(truncated_blocks_are_rejected) {
  Bytes text = kolma_test::make_text_like(4096);
  for (const Algorithm* algo : all_algorithms()) {
    if (algo->params().id == "bwt-mtf-ari") continue;  // an arithmetic stream degrades silently
    Bytes compressed;
    algo->compress_block(View(text), compressed, 6);
    if (compressed.size() < 4) continue;
    Bytes cut(compressed.begin(), compressed.begin() + std::ptrdiff_t(compressed.size() / 2));
    bool rejected = false;
    try {
      Bytes restored;
      algo->decompress_block(View(cut), restored, text.size());
      rejected = (restored != text);
    } catch (const std::exception&) {
      rejected = true;
    }
    CHECK(rejected);
  }
}

KOLMA_TEST(decompress_rejects_a_length_that_disagrees_with_the_header) {
  Bytes text = kolma_test::make_repetitive(4096);
  Bytes compressed;
  find_algorithm("lz77")->compress_block(View(text), compressed, 6);
  Bytes restored;
  CHECK_THROWS(find_algorithm("lz77")->decompress_block(View(compressed), restored, 4000));
}

KOLMA_TEST(lzw_survives_dictionary_resets) {
  // 300 KB of low-entropy text forces the 9 -> 10 -> 11 -> 12 bit width
  // transitions and at least one dictionary reset.
  Bytes text = kolma_test::make_text_like(300000);
  round_trip(find_algorithm("lzw"), text, 6);
  Bytes noise = kolma_test::make_random(300000, 42);
  round_trip(find_algorithm("lzw"), noise, 6);
}

KOLMA_TEST(huffman_handles_a_single_distinct_byte) {
  Bytes one(100000, 0xAB);
  round_trip(find_algorithm("huffman"), one, 6);
  Bytes out;
  find_algorithm("huffman")->compress_block(View(one), out, 6);
  // One distinct symbol costs a single bit per byte, plus the 256-byte table.
  CHECK(out.size() < one.size() / 8 + 512);
}

KOLMA_TEST(bwt_handles_periodic_and_pathological_input) {
  Bytes periodic;
  const std::string unit = "abcabcabcabc";
  for (int i = 0; i < 2000; ++i) periodic.insert(periodic.end(), unit.begin(), unit.end());
  round_trip(find_algorithm("bwt-mtf-ari"), periodic, 6);

  Bytes single(5000, 0x00);
  round_trip(find_algorithm("bwt-mtf-ari"), single, 6);

  Bytes two_symbols;
  for (int i = 0; i < 5000; ++i) two_symbols.push_back(i % 2 ? 0xFF : 0x00);
  round_trip(find_algorithm("bwt-mtf-ari"), two_symbols, 6);
}

KOLMA_TEST(delta_rle_picks_a_stride_and_reverses_it) {
  Bytes smooth = kolma_test::make_smooth_numeric(65536);
  Bytes compressed;
  find_algorithm("delta-rle")->compress_block(View(smooth), compressed, 6);
  size_t pos = 0;
  uint64_t declared = 0;
  CHECK(get_varint(View(compressed), pos, declared));
  CHECK_EQ(declared, uint64_t(smooth.size()));
  CHECK(pos < compressed.size());
  CHECK(compressed[pos] == 1 || compressed[pos] == 2 || compressed[pos] == 4);
  round_trip(find_algorithm("delta-rle"), smooth, 6);
}
KOLMA_TEST(higher_levels_do_not_lose_ratio) {
  // Regression guard: lazy matching used to insert a position into the hash
  // chain twice, which put the position in its own chain and made level 6
  // compress worse than level 3. Effort may cost speed, never ratio.
  Bytes text = kolma_test::make_text_like(200000);
  auto size_at = [&](int level) {
    Bytes out;
    find_algorithm("lz77")->compress_block(View(text), out, level);
    return out.size();
  };
  const size_t l1 = size_at(1);
  const size_t l3 = size_at(3);
  const size_t l6 = size_at(6);
  const size_t l9 = size_at(9);
  if (l6 > l3) {
    throw kolma_test::Failure("level 6 (" + std::to_string(l6) + " bytes) is worse than level 3 (" +
                              std::to_string(l3) + " bytes)");
  }
  if (l9 > l6) {
    throw kolma_test::Failure("level 9 (" + std::to_string(l9) + " bytes) is worse than level 6 (" +
                              std::to_string(l6) + " bytes)");
  }
  if (l3 > l1) {
    throw kolma_test::Failure("level 3 (" + std::to_string(l3) + ") is worse than level 1 (" +
                              std::to_string(l1) + ")");
  }
}
