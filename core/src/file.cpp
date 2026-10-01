#include "kolma/file.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace kolma {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool starts_with(View h, std::initializer_list<uint8_t> magic) {
  if (h.size() < magic.size()) return false;
  size_t i = 0;
  for (uint8_t b : magic) {
    if (h[i++] != b) return false;
  }
  return true;
}

struct TypeEntry {
  FileType type;
  const char* detail;
};

// Formats whose payload is already entropy coded: a second pass cannot help.
const std::unordered_map<std::string, TypeEntry>& compressed_formats() {
  static const std::unordered_map<std::string, TypeEntry> m = {
      {"jpeg", {FileType::Image, "jpeg"}},      {"png", {FileType::Image, "png"}},
      {"gif", {FileType::Image, "gif"}},        {"webp", {FileType::Image, "webp"}},
      {"heic", {FileType::Image, "heic"}},      {"avif", {FileType::Image, "avif"}},
      {"mp3", {FileType::Audio, "mp3"}},        {"aac", {FileType::Audio, "aac"}},
      {"ogg", {FileType::Audio, "ogg"}},        {"opus", {FileType::Audio, "opus"}},
      {"flac", {FileType::Audio, "flac"}},      {"mp4", {FileType::Video, "mp4"}},
      {"m4v", {FileType::Video, "m4v"}},        {"mkv", {FileType::Video, "matroska"}},
      {"webm", {FileType::Video, "webm"}},      {"mov", {FileType::Video, "mov"}},
      {"zip", {FileType::Archive, "zip"}},      {"gz", {FileType::Archive, "gzip"}},
      {"bz2", {FileType::Archive, "bzip2"}},    {"xz", {FileType::Archive, "xz"}},
      {"zst", {FileType::Archive, "zstd"}},     {"7z", {FileType::Archive, "7z"}},
      {"rar", {FileType::Archive, "rar"}},      {"kolma", {FileType::Archive, "kolma"}},
      {"docx", {FileType::Archive, "zip"}},     {"xlsx", {FileType::Archive, "zip"}},
      {"pptx", {FileType::Archive, "zip"}},     {"jar", {FileType::Archive, "zip"}},
      {"apk", {FileType::Archive, "zip"}},
  };
  return m;
}

}  // namespace

const char* file_type_name(FileType type) {
  switch (type) {
    case FileType::Text: return "Text";
    case FileType::Image: return "Image";
    case FileType::Audio: return "Audio";
    case FileType::Video: return "Video";
    case FileType::Executable: return "Executable";
    case FileType::Archive: return "Archive";
    case FileType::Binary: return "Binary";
    default: return "Unknown";
  }
}

bool file_type_from_name(std::string_view name, FileType& out) {
  std::string n = lower(std::string(name));
  for (uint8_t i = 0; i < uint8_t(FileType::Count); ++i) {
    if (lower(file_type_name(FileType(i))) == n) {
      out = FileType(i);
      return true;
    }
  }
  return false;
}

bool is_pretty_compressed(FileType type, const std::string& detail) {
  if (type == FileType::Video) return true;
  const auto& m = compressed_formats();
  auto it = m.find(detail);
  return it != m.end();
}

FileType detect_type_from_header(View header, const std::string& path, std::string* detail) {
  auto set = [&](FileType t, const char* d) {
    if (detail) *detail = d;
    return t;
  };

  // ---- magic bytes first: extensions lie, headers do not.
  if (starts_with(header, {0xFF, 0xD8, 0xFF})) return set(FileType::Image, "jpeg");
  if (starts_with(header, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return set(FileType::Image, "png");
  if (starts_with(header, {'G', 'I', 'F', '8'})) return set(FileType::Image, "gif");
  if (starts_with(header, {'B', 'M'})) return set(FileType::Image, "bmp");
  if (starts_with(header, {'I', 'I', 0x2A, 0x00}) || starts_with(header, {'M', 'M', 0x00, 0x2A}))
    return set(FileType::Image, "tiff");
  if (header.size() >= 12 && starts_with(header, {'R', 'I', 'F', 'F'})) {
    std::string tag(reinterpret_cast<const char*>(header.data() + 8), 4);
    if (tag == "WEBP") return set(FileType::Image, "webp");
    if (tag == "WAVE") return set(FileType::Audio, "wav");
    if (tag == "AVI ") return set(FileType::Video, "avi");
    return set(FileType::Binary, "riff");
  }
  if (header.size() >= 8 && starts_with(header, {0x00, 0x00, 0x00}) &&
      std::memcmp(header.data() + 4, "ftyp", 4) == 0)
    return set(FileType::Video, "mp4");
  if (starts_with(header, {0x1A, 0x45, 0xDF, 0xA3})) return set(FileType::Video, "matroska");
  if (starts_with(header, {'I', 'D', '3'}) || starts_with(header, {0xFF, 0xFB}) ||
      starts_with(header, {0xFF, 0xF3}) || starts_with(header, {0xFF, 0xF2}))
    return set(FileType::Audio, "mp3");
  if (starts_with(header, {'O', 'g', 'g', 'S'})) return set(FileType::Audio, "ogg");
  if (starts_with(header, {'f', 'L', 'a', 'C'})) return set(FileType::Audio, "flac");
  if (starts_with(header, {0x7F, 'E', 'L', 'F'})) return set(FileType::Executable, "elf");
  if (starts_with(header, {'M', 'Z'})) return set(FileType::Executable, "pe");
  if (starts_with(header, {0xCA, 0xFE, 0xBA, 0xBE}) || starts_with(header, {0xCF, 0xFA, 0xED, 0xFE}) ||
      starts_with(header, {0xCE, 0xFA, 0xED, 0xFE}) || starts_with(header, {0xFE, 0xED, 0xFA, 0xCE}))
    return set(FileType::Executable, "mach-o");
  if (starts_with(header, {0x00, 'a', 's', 'm'})) return set(FileType::Executable, "wasm");
  if (starts_with(header, {'P', 'K', 0x03, 0x04}) || starts_with(header, {'P', 'K', 0x05, 0x06}))
    return set(FileType::Archive, "zip");
  if (starts_with(header, {0x1F, 0x8B})) return set(FileType::Archive, "gzip");
  if (starts_with(header, {'B', 'Z', 'h'})) return set(FileType::Archive, "bzip2");
  if (starts_with(header, {0xFD, '7', 'z', 'X', 'Z', 0x00})) return set(FileType::Archive, "xz");
  if (starts_with(header, {0x28, 0xB5, 0x2F, 0xFD})) return set(FileType::Archive, "zstd");
  if (starts_with(header, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) return set(FileType::Archive, "7z");
  if (starts_with(header, {'%', 'P', 'D', 'F'})) return set(FileType::Binary, "pdf");
  if (starts_with(header, {0xEF, 0xBB, 0xBF})) return set(FileType::Text, "utf8-bom");
  if (starts_with(header, {'#', '!'})) return set(FileType::Text, "script");
  if (starts_with(header, {0x1F, 0x9D})) return set(FileType::Archive, "compress");

  // ---- extension table for everything without a magic number.
  std::string ext;
  if (auto dot = path.rfind('.'); dot != std::string::npos) ext = lower(path.substr(dot + 1));

  static const std::unordered_map<std::string, TypeEntry> by_ext = {
      {"txt", {FileType::Text, "text"}},      {"md", {FileType::Text, "markdown"}},
      {"markdown", {FileType::Text, "markdown"}}, {"csv", {FileType::Text, "csv"}},
      {"tsv", {FileType::Text, "tsv"}},       {"json", {FileType::Text, "json"}},
      {"jsonl", {FileType::Text, "jsonl"}},   {"yaml", {FileType::Text, "yaml"}},
      {"yml", {FileType::Text, "yaml"}},      {"toml", {FileType::Text, "toml"}},
      {"ini", {FileType::Text, "ini"}},       {"cfg", {FileType::Text, "config"}},
      {"log", {FileType::Text, "log"}},       {"xml", {FileType::Text, "xml"}},
      {"html", {FileType::Text, "html"}},     {"htm", {FileType::Text, "html"}},
      {"css", {FileType::Text, "css"}},       {"js", {FileType::Text, "javascript"}},
      {"mjs", {FileType::Text, "javascript"}},{"ts", {FileType::Text, "typescript"}},
      {"tsx", {FileType::Text, "typescript"}},{"jsx", {FileType::Text, "javascript"}},
      {"py", {FileType::Text, "python"}},     {"go", {FileType::Text, "go"}},
      {"rs", {FileType::Text, "rust"}},       {"c", {FileType::Text, "c"}},
      {"h", {FileType::Text, "c-header"}},    {"hpp", {FileType::Text, "cpp-header"}},
      {"cc", {FileType::Text, "cpp"}},        {"cpp", {FileType::Text, "cpp"}},
      {"cxx", {FileType::Text, "cpp"}},       {"kt", {FileType::Text, "kotlin"}},
      {"java", {FileType::Text, "java"}},     {"rb", {FileType::Text, "ruby"}},
      {"sh", {FileType::Text, "shell"}},      {"bash", {FileType::Text, "shell"}},
      {"zsh", {FileType::Text, "shell"}},     {"fish", {FileType::Text, "shell"}},
      {"nix", {FileType::Text, "nix"}},       {"sql", {FileType::Text, "sql"}},
      {"tex", {FileType::Text, "latex"}},     {"svg", {FileType::Text, "svg"}},
      {"gv", {FileType::Text, "graphviz"}},   {"mermaid", {FileType::Text, "mermaid"}},
      {"bmp", {FileType::Image, "bmp"}},      {"tiff", {FileType::Image, "tiff"}},
      {"tif", {FileType::Image, "tiff"}},     {"ppm", {FileType::Image, "ppm"}},
      {"pgm", {FileType::Image, "pgm"}},      {"pbm", {FileType::Image, "pbm"}},
      {"wav", {FileType::Audio, "wav"}},      {"aiff", {FileType::Audio, "aiff"}},
      {"au", {FileType::Audio, "au"}},        {"mid", {FileType::Audio, "midi"}},
      {"csv.gz", {FileType::Archive, "gzip"}},{"tar", {FileType::Archive, "tar"}},
      {"iso", {FileType::Archive, "iso"}},    {"deb", {FileType::Archive, "ar"}},
      {"rpm", {FileType::Archive, "rpm"}},    {"o", {FileType::Executable, "object"}},
      {"a", {FileType::Archive, "ar"}},       {"so", {FileType::Executable, "elf"}},
      {"dll", {FileType::Executable, "pe"}},  {"bin", {FileType::Binary, "raw"}},
      {"dat", {FileType::Binary, "data"}},    {"db", {FileType::Binary, "database"}},
      {"sqlite", {FileType::Binary, "sqlite"}},
  };
  if (!ext.empty()) {
    if (auto it = by_ext.find(ext); it != by_ext.end()) return set(it->second.type, it->second.detail);
    if (auto it = compressed_formats().find(ext); it != compressed_formats().end())
      return set(it->second.type, it->second.detail);
  }

  // ---- last resort: is the head printable?
  if (!header.empty()) {
    size_t printable = 0;
    size_t consider = std::min<size_t>(header.size(), 4096);
    for (size_t i = 0; i < consider; ++i) {
      uint8_t b = header[i];
      if (b == 0) return set(FileType::Binary, "binary");
      if (b == '\n' || b == '\r' || b == '\t' || (b >= 32 && b < 127) || b >= 0x80) ++printable;
    }
    if (printable * 100 >= consider * 95) return set(FileType::Text, "text");
  }
  return set(FileType::Binary, "binary");
}

double shannon_entropy(View data) {
  if (data.empty()) return 0.0;
  std::array<uint64_t, 256> hist{};
  for (uint8_t b : data) hist[b]++;
  double total = double(data.size());
  double h = 0.0;
  for (uint64_t c : hist) {
    if (c == 0) continue;
    double p = double(c) / total;
    h -= p * std::log2(p);
  }
  return h;
}

Bytes read_file_prefix(const std::string& path, size_t n) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  Bytes buf(n);
  in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n));
  buf.resize(size_t(in.gcount()));
  return buf;
}

FileInfo inspect_file(const std::string& path, size_t sample_size) {
  FileInfo info;
  info.path = path;
  std::error_code ec;
  info.name = std::filesystem::path(path).filename().string();
  if (info.name.empty()) info.name = path;

  auto st = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(st)) {
    info.error = "no such file: " + path;
    return info;
  }
  if (!std::filesystem::is_regular_file(st)) {
    info.error = "not a regular file: " + path;
    return info;
  }
  info.exists = true;
  info.size = std::filesystem::file_size(path, ec);
  if (ec) info.size = 0;

  auto ftime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    info.mtime = int64_t(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
  }

  Bytes sample = read_file_prefix(path, sample_size);
  info.type = detect_type_from_header(View(sample), path, &info.type_detail);
  info.entropy = shannon_entropy(View(sample));
  info.checksum = crc32(View(sample));
  return info;
}

}  // namespace kolma
