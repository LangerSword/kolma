// kolma -- perceptual media shrinking via the system ffmpeg. See media.hpp.
#include "kolma/media.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "kolma/file.hpp"
#include "kolma/util.hpp"

namespace kolma {
namespace fs = std::filesystem;

#ifdef _WIN32
#define KOLMA_POPEN _popen
#define KOLMA_PCLOSE _pclose
#else
#define KOLMA_POPEN popen
#define KOLMA_PCLOSE pclose
#endif

namespace {

// Quote a path for the platform shell so spaces and metacharacters are literal.
std::string shq(const std::string& s) {
#ifdef _WIN32
  std::string r = "\"";
  for (char c : s) {
    if (c == '"') r += '\\';
    r += c;
  }
  r += '"';
  return r;
#else
  std::string r = "'";
  for (char c : s) {
    if (c == '\'')
      r += "'\\''";
    else
      r += c;
  }
  r += "'";
  return r;
#endif
}

double parse_time(const std::string& s) {
  int h = 0, m = 0;
  double sec = 0;
  if (std::sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3)
    return h * 3600.0 + m * 60.0 + sec;
  if (std::sscanf(s.c_str(), "%lf", &sec) == 1) return sec;
  return 0;
}

}  // namespace

std::string find_ffmpeg() {
  const char* name = "ffmpeg";
#ifdef _WIN32
  name = "ffmpeg.exe";
#endif
  const char* pathenv = std::getenv("PATH");
  if (!pathenv) return "";
  std::string paths = pathenv;
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  size_t start = 0;
  while (start <= paths.size()) {
    size_t end = paths.find(sep, start);
    if (end == std::string::npos) end = paths.size();
    std::string dir = paths.substr(start, end - start);
    if (!dir.empty()) {
      std::error_code ec;
      fs::path cand = fs::path(dir) / name;
      if (fs::exists(cand, ec) && !fs::is_directory(cand, ec)) return cand.string();
    }
    if (end == paths.size()) break;
    start = end + 1;
  }
  return "";
}

bool media_available() { return !find_ffmpeg().empty(); }

Result shrink_media(const std::string& input, const MediaOptions& opts,
                    const ProgressFn& on_progress) {
  Result r;
  std::string ffmpeg = find_ffmpeg();
  if (ffmpeg.empty()) {
    r.error = "media shrinking needs ffmpeg installed and on PATH";
    return r;
  }
  FileInfo fi = inspect_file(input);
  if (!fi.exists) {
    r.error = "input does not exist: " + input;
    return r;
  }
  const bool is_video = fi.type == FileType::Video;
  const bool is_image = fi.type == FileType::Image;
  if (!is_video && !is_image) {
    r.error = "not a video or image: " + input + " (use compress for other files)";
    return r;
  }

  std::string preset = opts.preset.empty() ? "balanced" : opts.preset;
  std::string out = opts.output;
  if (out.empty()) {
    fs::path p(input);
    std::string base = p.stem().string();
    std::string ext = is_video ? ".mp4" : ".jpg";
    fs::path parent = p.parent_path();
    out = (parent.empty() ? fs::path(base + "-shrunk" + ext)
                          : parent / (base + "-shrunk" + ext)).string();
  }
  std::error_code ec;
  if (fs::exists(out, ec) && !opts.force) {
    r.error = "output already exists: " + out + " (use --force to overwrite)";
    return r;
  }

  std::string args;
  if (is_video) {
    int crf = preset == "quality" ? 18 : preset == "smallest" ? 28 : 23;
    const char* speed = preset == "quality" ? "slow" : "medium";
    const char* ab = preset == "quality" ? "160k" : preset == "smallest" ? "96k" : "128k";
    args = "-c:v libx264 -crf " + std::to_string(crf) + " -preset " + speed +
           " -pix_fmt yuv420p -c:a aac -b:a " + ab + " -movflags +faststart";
    r.stats.algorithm = "h264";
    r.stats.algorithm_name = "video (H.264, " + preset + ")";
  } else {
    int q = preset == "quality" ? 2 : preset == "smallest" ? 10 : 5;
    args = "-q:v " + std::to_string(q);
    r.stats.algorithm = "jpeg";
    r.stats.algorithm_name = "image (JPEG, " + preset + ")";
  }

  std::string cmd = shq(ffmpeg) + " -hide_banner -nostdin -y -i " + shq(input) + " " +
                    args + " " + shq(out) + " 2>&1";
  FILE* pipe = KOLMA_POPEN(cmd.c_str(), "r");
  if (!pipe) {
    r.error = "could not start ffmpeg";
    return r;
  }

  double total_sec = 0;
  int last_pct = -1;
  char line[4096];
  std::string tail;
  while (std::fgets(line, sizeof line, pipe)) {
    std::string s = line;
    size_t dpos = s.find("Duration:");
    if (dpos != std::string::npos && total_sec == 0)
      total_sec = parse_time(s.substr(dpos + 9));
    size_t tpos = s.find("time=");
    if (tpos != std::string::npos && total_sec > 0 && on_progress) {
      double cur = parse_time(s.substr(tpos + 5));
      int pct = int(cur / total_sec * 100.0);
      if (pct < 0) pct = 0;
      if (pct > 99) pct = 99;
      if (pct != last_pct) {
        last_pct = pct;
        Progress pr;
        pr.chunks_total = 100;
        pr.chunks_done = uint32_t(pct);
        on_progress(pr);
      }
    }
    tail += s;
    if (tail.size() > 4000) tail = tail.substr(tail.size() - 4000);
  }
  int rc = KOLMA_PCLOSE(pipe);
  if (rc != 0) {
    r.error = "ffmpeg could not shrink this file";
    std::string t = tail;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    size_t nl = t.rfind('\n');
    std::string last = (nl == std::string::npos) ? t : t.substr(nl + 1);
    if (!last.empty()) r.error += ": " + last;
    return r;
  }

  uint64_t new_size = fs::file_size(out, ec);
  r.ok = !ec;
  if (ec) {
    r.error = "ffmpeg finished but the output is missing";
    return r;
  }
  r.stats.orig_size = fi.size;
  r.stats.comp_size = new_size;
  r.stats.ratio = fi.size ? double(new_size) / double(fi.size) : 1.0;
  r.stats.output_path = out;
  r.stats.verified = true;
  if (on_progress) {
    Progress pr;
    pr.chunks_total = 100;
    pr.chunks_done = 100;
    pr.ratio = r.stats.ratio;
    on_progress(pr);
  }
  return r;
}

}  // namespace kolma
