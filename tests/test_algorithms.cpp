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
  CHECK_EQ(all_algorithms().size(), size_t(8));
  CHECK_EQ(algorithm_index("store"), uint8_t(0));
  CHECK_EQ(algorithm_index("rle"), uint8_t(1));
  CHECK_EQ(algorithm_index("huffman"), uint8_t(2));
  CHECK_EQ(algorithm_index("lz77"), uint8_t(3));
  CHECK_EQ(algorithm_index("lzw"), uint8_t(4));
  CHECK_EQ(algorithm_index("bwt-mtf-ari"), uint8_t(5));
  CHECK_EQ(algorithm_index("delta-rle"), uint8_t(6));
  CHECK_EQ(algorithm_index("rbrc"), uint8_t(7));
  CHECK_EQ(algorithm_by_index(6)->params().id, std::string("delta-rle"));
  CHECK_EQ(algorithm_by_index(7)->params().id, std::string("rbrc"));
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

KOLMA_TEST(huffman_rejects_hostile_code_length_tables) {
  // Regression: the stored length table is untrusted. A length byte of 255
  // used to shift a 32-bit code past its width (UB) and index the 33-entry
  // decode table out of bounds (found by the libFuzzer harness under ASan).
  const Algorithm* h = find_algorithm("huffman");
  auto block_with_lengths = [](const std::vector<uint8_t>& lengths) {
    Bytes b;
    put_varint(b, 16);  // declared original size
    b.insert(b.end(), lengths.begin(), lengths.end());
    b.insert(b.end(), 8, 0);  // a little bitstream
    return b;
  };
  Bytes restored;
  std::vector<uint8_t> lengths(256, 0);

  lengths[65] = 255;  // far past 32 bits
  CHECK_THROWS(h->decompress_block(View(block_with_lengths(lengths)), restored, 16));
  lengths[65] = 33;  // one past the maximum
  CHECK_THROWS(h->decompress_block(View(block_with_lengths(lengths)), restored, 16));
  lengths[65] = 32;  // exactly 32: legal width, must not crash on the shift
  CHECK_THROWS(h->decompress_block(View(block_with_lengths(lengths)), restored, 16));

  std::fill(lengths.begin(), lengths.end(), uint8_t(0));
  lengths[65] = lengths[66] = lengths[67] = 1;  // three 1-bit codes cannot exist
  CHECK_THROWS(h->decompress_block(View(block_with_lengths(lengths)), restored, 16));
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

// ------------------------------------------------------- rbrc (index 7) ----
// A static run-boundary rank coder: the header carries the whole model, so the
// decoder never adapts. These tests pin the format as well as the behaviour.

KOLMA_TEST(rbrc_round_trips_every_fixture_at_every_level) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);  // clean RED failure until the algorithm is registered
  for (const Fixture& f : fixtures()) {
    for (int level : {1, 6, 9}) round_trip(rbrc, f.second, level);
  }
}

KOLMA_TEST(rbrc_empty_block_is_only_a_zero_varint) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes out;
  rbrc->compress_block(View{}, out, 6);
  CHECK_BYTES_EQ(out, Bytes{0});
  Bytes restored;
  rbrc->decompress_block(View(out), restored, 0);
  CHECK(restored.empty());
}

KOLMA_TEST(rbrc_level_chooses_the_alphabet_cap) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes all_bytes;
  for (int i = 0; i < 256; ++i) all_bytes.push_back(uint8_t(i));

  for (auto [level, cap] : {std::pair<int, int>{1, 8}, {3, 8}, {4, 16}, {6, 16}, {7, 32}, {9, 32}}) {
    Bytes out;
    rbrc->compress_block(View(all_bytes), out, level);
    size_t pos = 0;
    uint64_t declared = 0;
    CHECK(get_varint(View(out), pos, declared));
    CHECK_EQ(declared, uint64_t(all_bytes.size()));
    uint16_t n = 0;
    CHECK(get_u16(View(out), pos, n));
    CHECK_EQ(n, uint16_t(cap));
  }
}

KOLMA_TEST(rbrc_shrinks_low_cardinality_high_transition_data) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  // Four symbols, changing on roughly three of every four bytes: the shape
  // RBRC is built for. Frequencies are near-uniform, so the rank model needs
  // about two bits per transition and the escape is essentially never used.
  Bytes stream;
  uint32_t x = 0x9E3779B9u;
  const char alphabet[] = {'A', 'C', 'G', 'T'};
  stream.reserve(65536);
  for (size_t i = 0; i < 65536; ++i) {
    x = x * 1664525u + 1013904223u;
    stream.push_back(uint8_t(alphabet[(x >> 8) & 3u]));
  }
  Bytes out;
  rbrc->compress_block(View(stream), out, 6);
  CHECK(out.size() < stream.size() / 2);
  round_trip(rbrc, stream, 6);
}

KOLMA_TEST(rbrc_does_not_meaningfully_shrink_random_data) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes noise = kolma_test::make_random(65536, 99);
  Bytes out;
  rbrc->compress_block(View(noise), out, 6);
  // The literal fallback costs eight bits plus framing; a static model must
  // not pretend that incompressible input got smaller.
  CHECK(out.size() > noise.size() * 95 / 100);
}

KOLMA_TEST(rbrc_rejects_hostile_headers) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes restored;  // must never be filled from a bad header

  auto header = [](uint64_t declared, const std::vector<uint8_t>& syms,
                   const std::vector<uint8_t>& weights, uint16_t p1, uint16_t esc) {
    Bytes b;
    put_varint(b, declared);
    put_u16(b, uint16_t(syms.size()));
    b.insert(b.end(), syms.begin(), syms.end());
    b.insert(b.end(), weights.begin(), weights.end());
    put_u16(b, p1);
    put_u16(b, esc);
    b.insert(b.end(), 16, 0);  // nominal range-coded payload
    return b;
  };

  const std::vector<uint8_t> two{'A', 'B'};
  const std::vector<uint8_t> flat{127, 128};
  const std::vector<uint8_t> eight{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
  const std::vector<uint8_t> eight_weights{32, 32, 32, 32, 32, 32, 32, 31};

  Bytes declared_wrong = header(15, two, flat, 32768, 1);
  CHECK_THROWS(rbrc->decompress_block(View(declared_wrong), restored, 16));

  Bytes truncated_header;
  put_varint(truncated_header, 16);
  CHECK_THROWS(rbrc->decompress_block(View(truncated_header), restored, 16));

  Bytes oversized_n;
  put_varint(oversized_n, 16);
  put_u16(oversized_n, 257);
  CHECK_THROWS(rbrc->decompress_block(View(oversized_n), restored, 16));

  Bytes empty_alphabet = header(16, {}, {}, 32768, 1);
  CHECK_THROWS(rbrc->decompress_block(View(empty_alphabet), restored, 16));

  Bytes duplicate = header(16, {'A', 'A'}, flat, 32768, 1);
  CHECK_THROWS(rbrc->decompress_block(View(duplicate), restored, 16));

  Bytes zero_weight = header(16, two, {0, 255}, 32768, 1);
  CHECK_THROWS(rbrc->decompress_block(View(zero_weight), restored, 16));

  Bytes zero_transition = header(16, two, flat, 0, 1);
  CHECK_THROWS(rbrc->decompress_block(View(zero_transition), restored, 16));

  Bytes zero_escape = header(16, two, flat, 32768, 0);
  CHECK_THROWS(rbrc->decompress_block(View(zero_escape), restored, 16));

  // Escape 65535 leaves a rank total of 1, too small for an eight-symbol rank
  // model: every rank would have zero frequency.
  Bytes no_room = header(16, eight, eight_weights, 32768, 65535);
  CHECK_THROWS(rbrc->decompress_block(View(no_room), restored, 16));
}

KOLMA_TEST(rbrc_corruption_changes_output_or_throws) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes text = kolma_test::make_text_like(8192);
  Bytes compressed;
  rbrc->compress_block(View(text), compressed, 6);
  CHECK(compressed.size() > 16);
  Bytes damaged = compressed;
  damaged[damaged.size() / 2] ^= 0x40;
  bool noticed = false;
  try {
    Bytes restored;
    rbrc->decompress_block(View(damaged), restored, text.size());
    noticed = (restored != text);
  } catch (const std::exception&) {
    noticed = true;
  }
  CHECK(noticed);
}

KOLMA_TEST(rbrc_truncated_block_is_rejected) {
  const Algorithm* rbrc = find_algorithm("rbrc");
  CHECK(rbrc != nullptr);
  Bytes text = kolma_test::make_text_like(4096);
  Bytes compressed;
  rbrc->compress_block(View(text), compressed, 6);
  CHECK(compressed.size() > 16);
  Bytes cut(compressed.begin(), compressed.begin() + std::ptrdiff_t(compressed.size() / 2));
  // Unlike the adaptive arithmetic stream in bwt-mtf-ari, RBRC's decoder
  // checks its range-coder reads and refuses to invent missing bytes.
  Bytes restored;
  CHECK_THROWS(rbrc->decompress_block(View(cut), restored, text.size()));
}
