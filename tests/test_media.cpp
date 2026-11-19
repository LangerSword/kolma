// Tests for perceptual media shrinking. These run only where ffmpeg is present;
// elsewhere they no-op so the suite stays green on machines without it.
#include <cstdlib>
#include <filesystem>

#include "kolma/media.hpp"
#include "kolma/util.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace fs = std::filesystem;

KOLMA_TEST(media_availability_matches_find_ffmpeg) {
  CHECK_EQ(kolma::media_available(), !kolma::find_ffmpeg().empty());
}

KOLMA_TEST(media_refuses_non_media_input) {
  if (!kolma::media_available()) return;
  const std::string dir = kolma_test::temp_path("media-nonmedia");
  fs::create_directories(dir);
  kolma_test::write_bytes(dir + "/f.txt", kolma_test::make_repetitive(1000));
  kolma::MediaOptions o;
  o.output = dir + "/out.mp4";
  kolma::Result r = kolma::shrink_media(dir + "/f.txt", o);
  CHECK(!r.ok);
  CHECK(!r.error.empty());
}

KOLMA_TEST(media_shrinks_a_real_video_and_keeps_it_playable) {
  if (!kolma::media_available()) return;
  const std::string dir = kolma_test::temp_path("media-video");
  fs::create_directories(dir);
  const std::string in = dir + "/in.mp4";
  // Generate a high-quality clip that has plenty of room to shrink.
  std::string gen = "'" + kolma::find_ffmpeg() +
                    "' -hide_banner -loglevel error -nostdin -y -f lavfi "
                    "-i testsrc2=duration=2:size=320x240:rate=15 "
                    "-c:v libx264 -crf 10 -pix_fmt yuv420p '" + in + "' 2>/dev/null";
  CHECK_EQ(std::system(gen.c_str()), 0);
  CHECK(fs::exists(in));
  uint64_t orig = fs::file_size(in);
  CHECK(orig > 0);

  kolma::MediaOptions o;
  o.preset = "smallest";
  o.force = true;
  kolma::Result r = kolma::shrink_media(in, o);
  CHECK(r.ok);
  CHECK(fs::exists(r.stats.output_path));
  CHECK(r.stats.comp_size < r.stats.orig_size);
  CHECK(r.stats.ratio < 1.0);
  CHECK_EQ(r.stats.orig_size, orig);

  // The output must still be a real, probe-able video (not a truncated file).
  std::string probe = "'" + kolma::find_ffmpeg() + "' -hide_banner -nostdin -i '" +
                      r.stats.output_path + "' -f null - 2>/dev/null";
  CHECK_EQ(std::system(probe.c_str()), 0);
}

KOLMA_TEST(media_refuses_overwrite_without_force) {
  if (!kolma::media_available()) return;
  const std::string dir = kolma_test::temp_path("media-over");
  fs::create_directories(dir);
  const std::string in = dir + "/in.mp4";
  std::string gen = "'" + kolma::find_ffmpeg() +
                    "' -hide_banner -loglevel error -nostdin -y -f lavfi "
                    "-i testsrc2=duration=1:size=160x120:rate=10 "
                    "-c:v libx264 -crf 15 -pix_fmt yuv420p '" + in + "' 2>/dev/null";
  CHECK_EQ(std::system(gen.c_str()), 0);
  kolma::MediaOptions o;
  o.output = dir + "/fixed.mp4";
  o.force = true;
  CHECK(kolma::shrink_media(in, o).ok);
  o.force = false;
  kolma::Result r = kolma::shrink_media(in, o);
  CHECK(!r.ok);
  CHECK(r.error.find("already exists") != std::string::npos);
}
