// Tests for the standard-format layer (zip / tar.gz / 7z via libarchive).
// These prove kolma can produce archives that anything else can open, and open
// archives made by anything else — the fix for ".kolma is a dead end".
#include <filesystem>

#include "kolma/stdarchive.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

#ifdef KOLMA_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace fs = std::filesystem;

KOLMA_TEST(std_zip_round_trips_files_and_folders) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-zip");
  fs::create_directories(dir + "/src/sub");
  kolma_test::write_bytes(dir + "/src/a.txt", kolma_test::make_repetitive(5000));
  kolma_test::write_bytes(dir + "/src/sub/b.bin", kolma_test::make_random(3000, 7));

  const std::string zip = dir + "/out.zip";
  kolma::StdPackOptions opts;
  opts.force = true;
  std::string err;
  CHECK(kolma::std_pack({dir + "/src"}, zip, opts, err));
  CHECK(kolma::std_is_standard(zip));
  CHECK_EQ(kolma::std_detect(zip), std::string("zip"));

  std::vector<kolma::StdEntry> entries;
  CHECK(kolma::std_list(zip, entries, err));
  CHECK(entries.size() >= 4);  // src/, a.txt, sub/, b.bin

  const std::string dest = dir + "/extracted";
  CHECK(kolma::std_extract(zip, dest, true, err));
  CHECK_BYTES_EQ(kolma_test::read_bytes(dest + "/src/a.txt"),
                 kolma_test::read_bytes(dir + "/src/a.txt"));
  CHECK_BYTES_EQ(kolma_test::read_bytes(dest + "/src/sub/b.bin"),
                 kolma_test::read_bytes(dir + "/src/sub/b.bin"));
}

KOLMA_TEST(std_test_passes_on_a_valid_zip) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-test-ok");
  fs::create_directories(dir + "/src");
  kolma_test::write_bytes(dir + "/src/a.txt", kolma_test::make_repetitive(4000));
  const std::string zip = dir + "/ok.zip";
  kolma::StdPackOptions opts;
  opts.force = true;
  std::string err;
  CHECK(kolma::std_pack({dir + "/src"}, zip, opts, err));
  uint64_t n = 0;
  CHECK(kolma::std_test(zip, n, err));
  CHECK(n >= 2);
}

KOLMA_TEST(std_test_detects_a_corrupted_zip) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-test-bad");
  fs::create_directories(dir);
  kolma_test::write_bytes(dir + "/f.bin", kolma_test::make_random(4000, 11));
  const std::string zip = dir + "/bad.zip";
  kolma::StdPackOptions opts;
  opts.force = true;
  std::string err;
  CHECK(kolma::std_pack({dir + "/f.bin"}, zip, opts, err));
  // Flip bytes inside the entry data (past the 30-byte local header + "f.bin").
  auto bytes = kolma_test::read_bytes(zip);
  CHECK(bytes.size() > 60);
  for (size_t i = 40; i < 48; ++i) bytes[i] ^= 0xFF;
  kolma_test::write_bytes(zip, bytes);
  uint64_t n = 0;
  CHECK(!kolma::std_test(zip, n, err));
  CHECK(!err.empty());
}

KOLMA_TEST(std_tar_gz_round_trips) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-tgz");
  fs::create_directories(dir);
  kolma_test::write_bytes(dir + "/data.bin", kolma_test::make_random(8000, 3));
  const std::string tgz = dir + "/out.tar.gz";
  kolma::StdPackOptions opts;
  opts.format = "tar.gz";
  opts.force = true;
  std::string err;
  CHECK(kolma::std_pack({dir + "/data.bin"}, tgz, opts, err));
  CHECK_EQ(kolma::std_detect(tgz), std::string("tar.gz"));
  const std::string dest = dir + "/out";
  CHECK(kolma::std_extract(tgz, dest, true, err));
  CHECK_BYTES_EQ(kolma_test::read_bytes(dest + "/data.bin"),
                 kolma_test::read_bytes(dir + "/data.bin"));
}

KOLMA_TEST(std_detect_ignores_native_kolma) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-detect");
  fs::create_directories(dir);
  const std::string in = dir + "/native.in";
  kolma_test::write_bytes(in, kolma_test::make_repetitive(2000));
  kolma::Job job;
  job.mode = kolma::Mode::Compress;
  job.input = in;
  job.output = dir + "/native.kolma";
  job.algorithm = "store";
  CHECK(kolma::run_job(job).ok);
  // A native .kolma must NOT be mistaken for a standard archive.
  CHECK_EQ(kolma::std_detect(dir + "/native.kolma"), std::string(""));
  CHECK(!kolma::std_is_standard(dir + "/native.kolma"));
}

#ifdef KOLMA_HAVE_LIBARCHIVE
KOLMA_TEST(std_extract_refuses_path_traversal) {
  const std::string dir = kolma_test::temp_path("std-trav");
  fs::create_directories(dir);
  const std::string zip = dir + "/evil.zip";
  // Hand-craft a zip whose entry tries to escape the destination.
  archive* a = archive_write_new();
  archive_write_set_format_zip(a);
  archive_write_open_filename(a, zip.c_str());
  archive_entry* e = archive_entry_new();
  archive_entry_set_pathname(e, "../escaped.txt");
  archive_entry_set_size(e, 4);
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  archive_write_header(a, e);
  archive_write_data(a, "evil", 4);
  archive_entry_free(e);
  archive_write_close(a);
  archive_write_free(a);

  std::string err;
  CHECK(!kolma::std_extract(zip, dir + "/dest", true, err));
  CHECK(!fs::exists(dir + "/escaped.txt"));
}
#endif

KOLMA_TEST(std_extract_refuses_overwrite_without_force) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-over");
  fs::create_directories(dir);
  kolma_test::write_bytes(dir + "/f.txt", kolma_test::make_repetitive(1000));
  const std::string zip = dir + "/o.zip";
  kolma::StdPackOptions opts;
  opts.force = true;
  std::string err;
  CHECK(kolma::std_pack({dir + "/f.txt"}, zip, opts, err));
  const std::string dest = dir + "/x";
  CHECK(kolma::std_extract(zip, dest, true, err));
  // Second extract without force must refuse (the file now exists).
  CHECK(!kolma::std_extract(zip, dest, false, err));
  CHECK(err.find("already exists") != std::string::npos);
}

KOLMA_TEST(std_pack_refuses_overwrite_without_force) {
  if (!kolma::std_formats_available()) return;
  const std::string dir = kolma_test::temp_path("std-packover");
  fs::create_directories(dir);
  kolma_test::write_bytes(dir + "/f.txt", kolma_test::make_repetitive(1000));
  const std::string zip = dir + "/o.zip";
  kolma::StdPackOptions opts;
  std::string err;
  CHECK(kolma::std_pack({dir + "/f.txt"}, zip, opts, err));
  CHECK(!kolma::std_pack({dir + "/f.txt"}, zip, opts, err));
  CHECK(err.find("already exists") != std::string::npos);
}
