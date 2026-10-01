// Analyzer behaviour: type detection, entropy, the incompressible veto and the
// recommendation itself.
#include <string>

#include "kolma/analyzer.hpp"
#include "kolma/file.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;

KOLMA_TEST(type_detection_uses_magic_bytes_over_extensions) {
  auto view = [](const Bytes& b) { return View(b); };
  CHECK_EQ(int(detect_type_from_header(view({0xFF, 0xD8, 0xFF, 0xE0}), "photo.txt")),
           int(FileType::Image));
  CHECK_EQ(int(detect_type_from_header(view({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}), "x")),
           int(FileType::Image));
  CHECK_EQ(int(detect_type_from_header(view({0x7F, 'E', 'L', 'F', 2, 1, 1}), "x.txt")),
           int(FileType::Executable));
  CHECK_EQ(int(detect_type_from_header(view({'P', 'K', 3, 4}), "x")), int(FileType::Archive));
  CHECK_EQ(int(detect_type_from_header(view({0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'm'}),
                                       "x")),
           int(FileType::Video));
  CHECK_EQ(int(detect_type_from_header(view({'%', 'P', 'D', 'F', '-'}), "x")), int(FileType::Binary));
  // Extension fallback when there is no magic number.
  CHECK_EQ(int(detect_type_from_header(view({}), "notes.md")), int(FileType::Text));
  CHECK_EQ(int(detect_type_from_header(view({}), "data.parquet")), int(FileType::Binary));
  // Printability heuristic as the last resort.
  CHECK_EQ(int(detect_type_from_header(view({'h', 'e', 'l', 'l', 'o', ' ', 'w'}), "noext")),
           int(FileType::Text));
}

KOLMA_TEST(entropy_matches_the_extremes) {
  CHECK(shannon_entropy(View{}) == 0.0);
  Bytes constant(4096, 0x7F);
  CHECK(shannon_entropy(View(constant)) == 0.0);
  Bytes uniform;
  for (int i = 0; i < 256; ++i) uniform.push_back(uint8_t(i));
  CHECK(shannon_entropy(View(uniform)) > 7.99);
  Bytes noise = kolma_test::make_random(65536, 5);
  CHECK(shannon_entropy(View(noise)) > 7.9);
  Bytes text = kolma_test::make_text_like(65536);
  CHECK(shannon_entropy(View(text)) < 6.0);
}

KOLMA_TEST(inspect_file_reports_size_type_and_checksum) {
  const std::string path = kolma_test::temp_path("inspect.md");
  Bytes data = kolma_test::make_repetitive(12345);
  kolma_test::write_bytes(path, data);
  FileInfo info = inspect_file(path);
  CHECK(info.exists);
  CHECK_EQ(info.size, uint64_t(12345));
  CHECK_EQ(info.name, std::string("inspect.md"));
  CHECK_EQ(int(info.type), int(FileType::Text));
  CHECK_EQ(info.checksum, crc32(View(data)));
  CHECK(info.mtime > 0);

  FileInfo missing = inspect_file(kolma_test::temp_path("inspect-nope.md"));
  CHECK(!missing.exists);
  CHECK(!missing.error.empty());
}

KOLMA_TEST(analyzer_recommends_a_real_method_for_compressible_data) {
  const std::string path = kolma_test::temp_path("analyze-text.txt");
  kolma_test::write_bytes(path, kolma_test::make_repetitive(300000));

  Analysis a = analyze_file(path, Priority::Balanced);
  CHECK(a.file.exists);
  CHECK(!a.store_raw);
  CHECK(a.recommended_algo != "store");
  CHECK(a.compressibility > 0.5);
  CHECK(a.recommended_level == 6);
  CHECK(!a.trials.empty());
  CHECK(a.rationale.find("entropy") != std::string::npos);

  Analysis fast = analyze_file(path, Priority::MaxSpeed);
  CHECK_EQ(fast.recommended_level, 1);
  Analysis ratio = analyze_file(path, Priority::MaxRatio);
  CHECK_EQ(ratio.recommended_level, 9);
}

KOLMA_TEST(analyzer_vetoes_already_compressed_formats) {
  const std::string path = kolma_test::temp_path("analyze-photo.jpg");
  Bytes fake_jpeg = {0xFF, 0xD8, 0xFF, 0xE0};
  Bytes body = kolma_test::make_random(200000, 8);
  fake_jpeg.insert(fake_jpeg.end(), body.begin(), body.end());
  kolma_test::write_bytes(path, fake_jpeg);

  Analysis a = analyze_file(path, Priority::MaxRatio);
  CHECK(a.store_raw);
  CHECK_EQ(a.recommended_algo, std::string("store"));
  CHECK(a.rationale.find("entropy coded") != std::string::npos);
  CHECK(a.trials.empty());  // no point trialling anything
}

KOLMA_TEST(analyzer_vetoes_high_entropy_data_without_a_known_format) {
  const std::string path = kolma_test::temp_path("analyze-noise.dat");
  kolma_test::write_bytes(path, kolma_test::make_random(262144, 12));
  Analysis a = analyze_file(path, Priority::Balanced);
  CHECK(a.store_raw);
  CHECK_EQ(a.recommended_algo, std::string("store"));
  CHECK(a.entropy > 7.9);
}

KOLMA_TEST(analyzer_handles_an_empty_file) {
  const std::string path = kolma_test::temp_path("analyze-empty.txt");
  kolma_test::write_bytes(path, {});
  Analysis a = analyze_file(path, Priority::Balanced);
  CHECK(a.file.exists);
  CHECK(a.store_raw);
  CHECK_EQ(a.recommended_algo, std::string("store"));
}

KOLMA_TEST(analyzer_reports_a_missing_file) {
  Analysis a = analyze_file(kolma_test::temp_path("analyze-missing.bin"), Priority::Balanced);
  CHECK(!a.file.exists);
  CHECK(!a.rationale.empty());
}

KOLMA_TEST(priority_parsing_accepts_the_documented_spellings) {
  Priority p = Priority::Balanced;
  CHECK(priority_from_name("maxratio", p) && p == Priority::MaxRatio);
  CHECK(priority_from_name("Max-Ratio", p) && p == Priority::MaxRatio);
  CHECK(priority_from_name("maxspeed", p) && p == Priority::MaxSpeed);
  CHECK(priority_from_name("balanced", p) && p == Priority::Balanced);
  CHECK(!priority_from_name("turbo", p));
  CHECK_EQ(std::string(priority_name(Priority::MaxRatio)), std::string("MaxRatio"));
}

KOLMA_TEST(small_files_are_not_worth_compressing) {
  const std::string path = kolma_test::temp_path("analyze-tiny.txt");
  kolma_test::write_bytes(path, Bytes{'h', 'i'});
  Analysis a = analyze_file(path, Priority::Balanced);
  // Two bytes cannot beat their own framing, so the veto must fire.
  CHECK(a.store_raw);
}