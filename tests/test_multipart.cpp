// Tests for multipart (split-volume) archives and in-place extraction.
#include <filesystem>
#include <fstream>

#include "kolma/multipart.hpp"
#include "kolma/stdarchive.hpp"
#include "kolma/util.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace fs = std::filesystem;

KOLMA_TEST(split_creates_numbered_volumes_that_sum_to_the_input) {
  const std::string dir = kolma_test::temp_path("mp-split");
  fs::create_directories(dir);
  const std::string in = dir + "/blob.bin";
  auto data = kolma_test::make_random(10000, 7);
  kolma_test::write_bytes(in, data);
  std::string err;
  uint32_t n = kolma::split_file(in, 4096, err);
  CHECK_EQ(n, 3u);  // 10000 = 4096 + 4096 + 1808
  uint64_t total = 0;
  for (uint32_t i = 1; i <= n; ++i) {
    std::string v = in + "." + (i < 10 ? "00" : i < 100 ? "0" : "") + std::to_string(i);
    CHECK(fs::exists(v));
    total += fs::file_size(v);
  }
  CHECK_EQ(total, data.size());
  // The input itself is untouched.
  CHECK(kolma_test::read_bytes(in) == data);
}

KOLMA_TEST(split_refuses_to_clobber_existing_volumes) {
  const std::string dir = kolma_test::temp_path("mp-clobber");
  fs::create_directories(dir);
  const std::string in = dir + "/blob.bin";
  kolma_test::write_bytes(in, kolma_test::make_random(5000, 1));
  std::string err;
  CHECK_EQ(kolma::split_file(in, 2048, err), 3u);
  uint32_t again = kolma::split_file(in, 2048, err);
  CHECK_EQ(again, 0u);
  CHECK(err.find("already exists") != std::string::npos);
}

KOLMA_TEST(detect_volume_set_resolves_the_whole_series_from_any_member) {
  const std::string dir = kolma_test::temp_path("mp-detect");
  fs::create_directories(dir);
  const std::string in = dir + "/arc.zip";
  kolma_test::write_bytes(in, kolma_test::make_random(9000, 2));
  std::string err;
  CHECK_EQ(kolma::split_file(in, 3000, err), 3u);
  CHECK(kolma::is_multipart(in + ".001"));
  CHECK(kolma::is_multipart(in + ".002"));
  CHECK(!kolma::is_multipart(in));  // the un-split file is not a volume
  kolma::VolumeSet vs = kolma::detect_volume_set(in + ".002");
  CHECK_EQ(vs.paths.size(), 3u);
  CHECK_EQ(vs.total_size, 9000u);
  CHECK_EQ(vs.base, in);
  CHECK_EQ(vs.paths[0], in + ".001");
}

KOLMA_TEST(join_round_trips_a_split_file_byte_for_byte) {
  const std::string dir = kolma_test::temp_path("mp-join");
  fs::create_directories(dir);
  const std::string in = dir + "/blob.bin";
  auto data = kolma_test::make_random(20000, 3);
  kolma_test::write_bytes(in, data);
  std::string err;
  CHECK_EQ(kolma::split_file(in, 8192, err), 3u);
  kolma::VolumeSet vs = kolma::detect_volume_set(in + ".001");
  const std::string out = dir + "/joined.bin";
  CHECK(kolma::join_multipart(vs, out, false, err));
  CHECK(kolma_test::read_bytes(out) == data);
  // join refuses overwrite without force.
  CHECK(!kolma::join_multipart(vs, out, false, err));
  CHECK(kolma::join_multipart(vs, out, true, err));
}

KOLMA_TEST(extract_multipart_zip_streams_without_a_joined_copy) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("mp-zip");
  fs::create_directories(dir);
  const std::string src = dir + "/proj";
  fs::create_directories(src + "/sub");
  kolma_test::write_bytes(src + "/a.txt", kolma_test::make_repetitive(3000));
  kolma_test::write_bytes(src + "/sub/b.txt", kolma_test::make_random(2500, 5));
  const std::string zip = dir + "/proj.zip";
  std::string err;
  kolma::StdPackOptions po;
  po.format = "zip";
  CHECK(kolma::std_pack({src}, zip, po, err));
  CHECK_EQ(kolma::split_file(zip, 2048, err), 3u);
  fs::remove(zip);  // only the volumes remain; extraction must stream them
  kolma::VolumeSet vs = kolma::detect_volume_set(zip + ".001");
  CHECK_EQ(vs.paths.size(), 3u);
  const std::string dest = dir + "/out";
  kolma::MultipartExtractOptions mo;
  mo.force = true;
  CHECK(kolma::extract_multipart(vs, dest, mo, err));
  CHECK(kolma_test::read_bytes(dest + "/proj/a.txt") == kolma_test::make_repetitive(3000));
  CHECK(kolma_test::read_bytes(dest + "/proj/sub/b.txt") == kolma_test::make_random(2500, 5));
}

KOLMA_TEST(extract_multipart_tar_in_place_reclaims_volumes) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("mp-inplace");
  fs::create_directories(dir);
  const std::string src = dir + "/data";
  fs::create_directories(src);
  kolma_test::write_bytes(src + "/one.txt", kolma_test::make_repetitive(4000));
  kolma_test::write_bytes(src + "/two.txt", kolma_test::make_random(3000, 9));
  const std::string tar = dir + "/data.tar";
  std::string err;
  kolma::StdPackOptions po;
  po.format = "tar";
  CHECK(kolma::std_pack({src}, tar, po, err));
  CHECK_EQ(kolma::split_file(tar, 2048, err), 4u);
  fs::remove(tar);
  kolma::VolumeSet vs = kolma::detect_volume_set(tar + ".001");
  const std::string dest = dir + "/out";
  kolma::MultipartExtractOptions mo;
  mo.force = true;
  mo.in_place = true;
  mo.keep_free = 0;  // small fixture: don't demand the 2 GiB reserve in tests
  CHECK(kolma::extract_multipart(vs, dest, mo, err));
  CHECK(kolma_test::read_bytes(dest + "/data/one.txt") == kolma_test::make_repetitive(4000));
  CHECK(kolma_test::read_bytes(dest + "/data/two.txt") == kolma_test::make_random(3000, 9));
  // Every source volume must have been reclaimed.
  for (const auto& v : vs.paths) CHECK(!fs::exists(v));
}

KOLMA_TEST(in_place_is_refused_for_indexed_zip) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("mp-refuse");
  fs::create_directories(dir);
  const std::string src = dir + "/f";
  fs::create_directories(src);
  kolma_test::write_bytes(src + "/x.txt", kolma_test::make_repetitive(1000));
  const std::string zip = dir + "/f.zip";
  std::string err;
  kolma::StdPackOptions po;
  po.format = "zip";
  CHECK(kolma::std_pack({src}, zip, po, err));
  CHECK_EQ(kolma::split_file(zip, 1024, err), 2u);
  kolma::VolumeSet vs = kolma::detect_volume_set(zip + ".001");
  kolma::MultipartExtractOptions mo;
  mo.in_place = true;
  mo.keep_free = 0;
  CHECK(!kolma::extract_multipart(vs, dir + "/out", mo, err));
  CHECK(err.find("in-place") != std::string::npos);
  // The volumes must still be intact after the refusal.
  for (const auto& v : vs.paths) CHECK(fs::exists(v));
}
