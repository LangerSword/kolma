// Bundle layer: tar round-trips, folder/multi-file compress, safe extraction.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kolma/archive.hpp"
#include "kolma/bundle.hpp"
#include "kolma/engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace kolma;
namespace fs = std::filesystem;

namespace {

std::string bundle_dir() {
  static std::string dir = kolma_test::temp_dir() + "/bundle";
  fs::create_directories(dir);
  return dir;
}

void put_text(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::string get_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Build a single-file tar with an arbitrary (possibly hostile) stored name.
void craft_tar(const std::string& path, const std::string& stored_name,
               const std::string& data) {
  auto octal = [](char* dst, size_t width, uint64_t v) {
    char tmp[32];
    std::snprintf(tmp, sizeof tmp, "%0*o", static_cast<int>(width - 1),
                  static_cast<unsigned>(v));
    std::memcpy(dst, tmp, width - 1);
    dst[width - 1] = '\0';
  };
  std::array<char, 512> h{};
  std::memcpy(h.data(), stored_name.data(), std::min(stored_name.size(), size_t(100)));
  octal(h.data() + 100, 8, 0644);
  octal(h.data() + 124, 12, data.size());
  octal(h.data() + 136, 12, 0);
  h[156] = '0';
  std::memcpy(h.data() + 257, "ustar\0", 6);
  std::memcpy(h.data() + 263, "00", 2);
  std::memset(h.data() + 148, ' ', 8);
  uint32_t sum = 0;
  for (size_t i = 0; i < 512; ++i) sum += static_cast<unsigned char>(h[i]);
  char cs[16];
  std::snprintf(cs, sizeof cs, "%06o", sum);
  std::memcpy(h.data() + 148, cs, 6);
  h[154] = '\0';
  h[155] = ' ';
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(h.data(), 512);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  size_t pad = (512 - (data.size() % 512)) % 512;
  std::array<char, 512> zeros{};
  for (size_t i = 0; i < pad; ++i) out.write(zeros.data(), 1);
  out.write(zeros.data(), 512);
  out.write(zeros.data(), 512);
}

}  // namespace

KOLMA_TEST(tar_round_trips_files_dirs_and_long_names) {
  std::string root = bundle_dir() + "/src";
  fs::remove_all(root);
  put_text(root + "/a.txt", "alpha");
  put_text(root + "/sub/b.txt", "beta");
  put_text(root + "/empty.bin", "");
  std::string longname = std::string(180, 'x') + ".dat";
  put_text(root + "/" + longname, "long-name-content");

  std::string tar = bundle_dir() + "/rt.tar";
  std::string err;
  CHECK(tar_write({root}, tar, err));
  CHECK(looks_like_tar(tar));

  std::vector<BundleEntry> entries;
  CHECK(tar_list(tar, entries, err));
  bool saw_long = false;
  for (const auto& e : entries)
    if (e.path.find(longname) != std::string::npos) saw_long = true;
  CHECK(saw_long);

  std::string dest = bundle_dir() + "/rt-out";
  fs::remove_all(dest);
  CHECK(tar_extract(tar, dest, false, err));
  CHECK_EQ(get_text(dest + "/src/a.txt"), std::string("alpha"));
  CHECK_EQ(get_text(dest + "/src/sub/b.txt"), std::string("beta"));
  CHECK_EQ(get_text(dest + "/src/" + longname), std::string("long-name-content"));
  CHECK(fs::exists(dest + "/src/empty.bin"));
}

KOLMA_TEST(bundle_compress_list_extract_round_trip) {
  std::string root = bundle_dir() + "/proj";
  fs::remove_all(root);
  put_text(root + "/readme.md", "# project\nhello");
  put_text(root + "/data/nums.bin", std::string(5000, '\x07'));

  std::string arc = bundle_dir() + "/proj.kolma";
  BundleOptions opts;
  opts.algorithm = "lz77";
  Result r = compress_bundle({root}, arc, opts);
  CHECK(r.ok);
  CHECK(r.stats.bundle);

  ArchiveInfo info = read_archive_info(arc);
  CHECK(info.ok);
  CHECK(info.bundle);

  std::vector<BundleEntry> entries;
  std::string err;
  CHECK(list_bundle(arc, "", entries, err));
  CHECK(entries.size() >= 2);

  std::string dest = bundle_dir() + "/proj-out";
  fs::remove_all(dest);
  Result ex = extract_archive(arc, dest, "", false);
  CHECK(ex.ok);
  CHECK_EQ(get_text(dest + "/proj/readme.md"), std::string("# project\nhello"));
  CHECK_EQ(get_text(dest + "/proj/data/nums.bin"), std::string(5000, '\x07'));
}

KOLMA_TEST(encrypted_bundle_round_trip_and_wrong_password) {
  std::string root = bundle_dir() + "/secret";
  fs::remove_all(root);
  put_text(root + "/key.txt", "top-secret");

  std::string arc = bundle_dir() + "/secret.kolma";
  BundleOptions opts;
  opts.password = "bundle-password";
  Result r = compress_bundle({root}, arc, opts);
  CHECK(r.ok);

  std::vector<BundleEntry> entries;
  std::string err;
  CHECK(list_bundle(arc, "bundle-password", entries, err));
  CHECK(!entries.empty());
  CHECK(!list_bundle(arc, "wrong", entries, err));

  std::string dest = bundle_dir() + "/secret-out";
  fs::remove_all(dest);
  Result bad = extract_archive(arc, dest, "wrong", false);
  CHECK(!bad.ok);
  Result good = extract_archive(arc, dest, "bundle-password", false);
  CHECK(good.ok);
  CHECK_EQ(get_text(dest + "/secret/key.txt"), std::string("top-secret"));
}

KOLMA_TEST(bundle_payload_is_a_real_tar) {
  std::string root = bundle_dir() + "/interop";
  fs::remove_all(root);
  put_text(root + "/f.txt", "interop");
  std::string arc = bundle_dir() + "/interop.kolma";
  CHECK(compress_bundle({root}, arc, BundleOptions{}).ok);

  // A plain decompress must yield a standard tar anyone can read.
  std::string raw = bundle_dir() + "/interop.tar";
  Job job;
  job.mode = Mode::Decompress;
  job.input = arc;
  job.output = raw;
  job.force = true;
  CHECK(run_job(job).ok);
  CHECK(looks_like_tar(raw));
}

KOLMA_TEST(extract_refuses_path_traversal) {
  std::string tar = bundle_dir() + "/evil.tar";
  craft_tar(tar, "../escaped.txt", "pwned");
  std::string dest = bundle_dir() + "/evil-out";
  fs::remove_all(dest);
  std::string err;
  CHECK(!tar_extract(tar, dest, false, err));
  CHECK(err.find("unsafe") != std::string::npos);
  CHECK(!fs::exists(bundle_dir() + "/escaped.txt"));

  std::string tar2 = bundle_dir() + "/evil2.tar";
  craft_tar(tar2, "/absolute.txt", "pwned");
  CHECK(!tar_extract(tar2, dest, false, err));
}

KOLMA_TEST(single_file_archive_extracts_under_its_name) {
  std::string in = bundle_dir() + "/single.dat";
  put_text(in, "just one file");
  std::string arc = bundle_dir() + "/single.kolma";
  Job job;
  job.mode = Mode::Compress;
  job.input = in;
  job.output = arc;
  CHECK(run_job(job).ok);

  std::string dest = bundle_dir() + "/single-out";
  fs::remove_all(dest);
  Result ex = extract_archive(arc, dest, "", false);
  CHECK(ex.ok);
  CHECK(!ex.stats.bundle);
  CHECK_EQ(get_text(dest + "/single.dat"), std::string("just one file"));
}

KOLMA_TEST(bundle_refuses_to_overwrite_without_force) {
  std::string root = bundle_dir() + "/ow";
  fs::remove_all(root);
  put_text(root + "/x.txt", "one");
  std::string arc = bundle_dir() + "/ow.kolma";
  CHECK(compress_bundle({root}, arc, BundleOptions{}).ok);

  std::string dest = bundle_dir() + "/ow-out";
  fs::remove_all(dest);
  CHECK(extract_archive(arc, dest, "", false).ok);
  Result again = extract_archive(arc, dest, "", false);
  CHECK(!again.ok);
  CHECK(again.error.find("already exists") != std::string::npos);
  CHECK(extract_archive(arc, dest, "", true).ok);
}
