#include "kolma/bundle.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "kolma/archive.hpp"
#include "kolma/file.hpp"

namespace kolma {
namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ temp ----
std::string make_temp_path() {
#ifdef _WIN32
  char dir[MAX_PATH + 1];
  char name[MAX_PATH + 1];
  if (GetTempPathA(MAX_PATH + 1, dir) == 0) return {};
  if (GetTempFileNameA(dir, "kol", 0, name) == 0) return {};
  return name;  // GetTempFileNameA atomically creates the (empty) file
#else
  char tmpl[] = "/tmp/kolma-bundle-XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) return {};
  ::close(fd);
  return tmpl;
#endif
}

struct TempFile {
  std::string path;
  TempFile() : path(make_temp_path()) {}
  ~TempFile() {
    if (!path.empty()) std::remove(path.c_str());
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

// ------------------------------------------------------------------ tar -----
constexpr size_t kBlock = 512;
// ustar header field offsets.
constexpr size_t kName = 0, kMode = 100, kUid = 108, kGid = 116, kSize = 124,
                 kMtime = 136, kChksum = 148, kTypeflag = 156, kMagic = 257,
                 kVersion = 263, kPrefix = 345;

void put_octal(char* block, size_t off, size_t width, uint64_t value) {
  // width-1 octal digits then NUL (the chksum field is special-cased below).
  char fmt[16];
  std::snprintf(fmt, sizeof fmt, "%%0%lluo", static_cast<unsigned long long>(width - 1));
  char tmp[32];
  std::snprintf(tmp, sizeof tmp, fmt, static_cast<unsigned long long>(value));
  std::memcpy(block + off, tmp, width - 1);
  block[off + width - 1] = '\0';
}

uint64_t get_octal(const char* block, size_t off, size_t width) {
  uint64_t v = 0;
  for (size_t i = 0; i < width; ++i) {
    char c = block[off + i];
    if (c == '\0' || c == ' ') break;
    if (c < '0' || c > '7') break;
    v = (v << 3) | uint64_t(c - '0');
  }
  return v;
}

void put_str(char* block, size_t off, size_t width, const std::string& s) {
  size_t n = std::min(s.size(), width);
  std::memcpy(block + off, s.data(), n);
  if (n < width) std::memset(block + off + n, 0, width - n);
}

std::string get_str(const char* block, size_t off, size_t width) {
  size_t n = 0;
  while (n < width && block[off + n] != '\0') ++n;
  return std::string(block + off, n);
}

uint32_t header_checksum(const std::array<char, kBlock>& h) {
  uint32_t sum = 0;
  for (size_t i = 0; i < kBlock; ++i) {
    // The checksum field itself is counted as spaces.
    unsigned char c = (i >= kChksum && i < kChksum + 8) ? ' ' : static_cast<unsigned char>(h[i]);
    sum += c;
  }
  return sum;
}

void finish_header(std::array<char, kBlock>& h) {
  std::memcpy(h.data() + kMagic, "ustar\0", 6);
  std::memcpy(h.data() + kVersion, "00", 2);
  std::memset(h.data() + kChksum, ' ', 8);
  uint32_t sum = header_checksum(h);
  char tmp[16];
  std::snprintf(tmp, sizeof tmp, "%06o", sum);
  std::memcpy(h.data() + kChksum, tmp, 6);
  h[kChksum + 6] = '\0';
  h[kChksum + 7] = ' ';
}

// Split a long path into ustar prefix (<=155) + name (<=100) at a '/'.
bool split_prefix(const std::string& path, std::string& prefix, std::string& name) {
  if (path.size() <= 100) {
    prefix.clear();
    name = path;
    return true;
  }
  for (size_t i = path.size() - 1; i > 0; --i) {
    if (path[i] != '/') continue;
    std::string p = path.substr(0, i);
    std::string n = path.substr(i + 1);
    if (p.size() <= 155 && n.size() <= 100 && !n.empty()) {
      prefix = p;
      name = n;
      return true;
    }
  }
  return false;
}

bool write_block(std::ostream& out, const std::array<char, kBlock>& h) {
  out.write(h.data(), kBlock);
  return out.good();
}

bool write_data_padded(std::ostream& out, const char* data, uint64_t size) {
  if (size > 0) out.write(data, static_cast<std::streamsize>(size));
  uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
  if (pad > 0) {
    std::array<char, kBlock> zeros{};
    out.write(zeros.data(), static_cast<std::streamsize>(pad));
  }
  return out.good();
}

// Emit a GNU long-name entry ('L') carrying the full path, for paths that do
// not fit ustar name+prefix. The following real header uses a truncated name.
bool write_longname(std::ostream& out, const std::string& path) {
  std::array<char, kBlock> h{};
  put_str(h.data(), kName, 100, "././@LongLink");
  put_octal(h.data(), kMode, 8, 0);
  put_octal(h.data(), kUid, 8, 0);
  put_octal(h.data(), kGid, 8, 0);
  put_octal(h.data(), kSize, 12, path.size() + 1);
  put_octal(h.data(), kMtime, 12, 0);
  h[kTypeflag] = 'L';
  finish_header(h);
  if (!write_block(out, h)) return false;
  std::string data = path;
  data.push_back('\0');
  return write_data_padded(out, data.data(), data.size());
}

bool write_entry_header(std::ostream& out, const std::string& path, uint64_t size,
                        bool is_dir, uint32_t mode, int64_t mtime) {
  std::string prefix, name;
  bool use_long = !split_prefix(path, prefix, name);
  if (use_long) {
    if (!write_longname(out, path)) return false;
    name = path.substr(0, 100);
    prefix.clear();
  }
  std::array<char, kBlock> h{};
  put_str(h.data(), kName, 100, name);
  put_octal(h.data(), kMode, 8, mode & 07777u);
  put_octal(h.data(), kUid, 8, 0);
  put_octal(h.data(), kGid, 8, 0);
  put_octal(h.data(), kSize, 12, is_dir ? 0 : size);
  put_octal(h.data(), kMtime, 12, static_cast<uint64_t>(mtime < 0 ? 0 : mtime));
  h[kTypeflag] = is_dir ? '5' : '0';
  if (!prefix.empty()) put_str(h.data(), kPrefix, 155, prefix);
  finish_header(h);
  return write_block(out, h);
}

uint32_t perms_mode(const fs::path& p) {
  std::error_code ec;
  auto perms = fs::status(p, ec).permissions();
  uint32_t m = 0;
  if ((perms & fs::perms::owner_read) != fs::perms::none) m |= 0400;
  if ((perms & fs::perms::owner_write) != fs::perms::none) m |= 0200;
  if ((perms & fs::perms::owner_exec) != fs::perms::none) m |= 0100;
  if ((perms & fs::perms::group_read) != fs::perms::none) m |= 0040;
  if ((perms & fs::perms::group_write) != fs::perms::none) m |= 0020;
  if ((perms & fs::perms::group_exec) != fs::perms::none) m |= 0010;
  if ((perms & fs::perms::others_read) != fs::perms::none) m |= 0004;
  if ((perms & fs::perms::others_write) != fs::perms::none) m |= 0002;
  if ((perms & fs::perms::others_exec) != fs::perms::none) m |= 0001;
  return m == 0 ? 0644 : m;
}

int64_t file_mtime(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}

// Reject anything that could escape the destination directory.
bool safe_rel_path(const std::string& path) {
  if (path.empty()) return false;
  if (path.front() == '/' || path.front() == '\\') return false;
  if (path.size() >= 2 && path[1] == ':') return false;  // drive letter
  size_t start = 0;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    std::string comp = path.substr(start, end - start);
    if (comp == "..") return false;
    if (comp.find('\\') != std::string::npos) return false;
    start = end + 1;
  }
  return true;
}

struct RawEntry {
  std::string path;
  uint64_t size = 0;
  char type = '0';
  uint32_t mode = 0644;
  int64_t mtime = 0;
};

// Iterate tar entries, invoking fn(RawEntry, data_offset) for each real header.
// data_offset is the absolute file offset where the entry's data begins.
// Returns false + error on a malformed stream. fn returns false to stop early.
template <typename Fn>
bool tar_iterate(const std::string& tar_path, Fn fn, std::string& error) {
  std::ifstream in(tar_path, std::ios::binary);
  if (!in) {
    error = "cannot open tar " + tar_path;
    return false;
  }
  std::string pending_long;
  int zero_blocks = 0;
  uint64_t offset = 0;
  std::array<char, kBlock> h{};
  while (in) {
    in.read(h.data(), kBlock);
    if (in.gcount() == 0) break;
    if (in.gcount() != static_cast<std::streamsize>(kBlock)) {
      error = "truncated tar header";
      return false;
    }
    bool all_zero = true;
    for (char c : h)
      if (c != '\0') {
        all_zero = false;
        break;
      }
    if (all_zero) {
      if (++zero_blocks >= 2) break;
      offset += kBlock;
      continue;
    }
    zero_blocks = 0;

    uint32_t stored = static_cast<uint32_t>(get_octal(h.data(), kChksum, 8));
    if (stored != header_checksum(h)) {
      error = "tar header checksum mismatch (corrupt archive)";
      return false;
    }

    uint64_t size = get_octal(h.data(), kSize, 12);
    char type = h[kTypeflag];
    if (type == '\0') type = '0';
    uint64_t data_offset = offset + kBlock;
    uint64_t padded = ((size + kBlock - 1) / kBlock) * kBlock;

    if (type == 'L' || type == 'K') {
      // GNU long name / long link target: the data is the string for the next entry.
      std::string buf(static_cast<size_t>(size), '\0');
      if (size > 0) {
        in.seekg(static_cast<std::streamoff>(data_offset));
        in.read(buf.data(), static_cast<std::streamsize>(size));
      }
      if (type == 'L') {
        pending_long = buf.c_str();  // stop at NUL
      }
      in.seekg(static_cast<std::streamoff>(data_offset + padded));
      offset = data_offset + padded;
      continue;
    }

    std::string name = get_str(h.data(), kName, 100);
    std::string prefix = get_str(h.data(), kPrefix, 155);
    std::string full = pending_long.empty() ? (prefix.empty() ? name : prefix + "/" + name)
                                            : pending_long;
    pending_long.clear();

    RawEntry e;
    e.path = full;
    e.size = size;
    e.type = type;
    e.mode = static_cast<uint32_t>(get_octal(h.data(), kMode, 8));
    e.mtime = static_cast<int64_t>(get_octal(h.data(), kMtime, 12));

    if (!fn(e, data_offset)) return false;  // callback hit an error; it set `error`

    in.seekg(static_cast<std::streamoff>(data_offset + padded));
    offset = data_offset + padded;
  }
  return true;
}

}  // namespace

// -------------------------------------------------------------- tar_write --
bool tar_write(const std::vector<std::string>& inputs, const std::string& tar_path,
               std::string& error) {
  if (inputs.empty()) {
    error = "nothing to bundle";
    return false;
  }
  std::ofstream out(tar_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + tar_path;
    return false;
  }

  auto emit_file = [&](const fs::path& p, const std::string& stored) -> bool {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) {
      error = "cannot size " + p.string();
      return false;
    }
    if (!write_entry_header(out, stored, sz, false, perms_mode(p), file_mtime(p))) {
      error = "write failed for " + stored;
      return false;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
      error = "cannot read " + p.string();
      return false;
    }
    std::array<char, 65536> buf{};
    uint64_t remaining = sz;
    while (remaining > 0) {
      size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), remaining));
      in.read(buf.data(), static_cast<std::streamsize>(want));
      auto got = in.gcount();
      if (got <= 0) {
        error = "short read on " + p.string();
        return false;
      }
      out.write(buf.data(), got);
      remaining -= static_cast<uint64_t>(got);
    }
    uint64_t pad = (kBlock - (sz % kBlock)) % kBlock;
    if (pad > 0) {
      std::array<char, kBlock> zeros{};
      out.write(zeros.data(), static_cast<std::streamsize>(pad));
    }
    return out.good();
  };

  for (const std::string& input : inputs) {
    fs::path p(input);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
      error = "input does not exist: " + input;
      return false;
    }
    if (fs::is_symlink(p, ec)) continue;  // never follow or store symlinks
    std::string base = p.filename().string();
    if (base.empty()) base = "root";

    if (fs::is_directory(p, ec)) {
      std::string root = base + "/";
      if (!write_entry_header(out, root, 0, true, perms_mode(p) | 0111, file_mtime(p))) {
        error = "write failed for " + root;
        return false;
      }
      for (auto it = fs::recursive_directory_iterator(
               p, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
          error = "cannot walk " + p.string();
          return false;
        }
        const fs::path& child = it->path();
        if (it->is_symlink()) continue;
        std::string rel = fs::relative(child, p).generic_string();
        std::string stored = base + "/" + rel;
        if (it->is_directory()) {
          if (!write_entry_header(out, stored + "/", 0, true, perms_mode(child) | 0111,
                                  file_mtime(child))) {
            error = "write failed for " + stored;
            return false;
          }
        } else if (it->is_regular_file()) {
          if (!emit_file(child, stored)) return false;
        }
      }
    } else if (fs::is_regular_file(p, ec)) {
      if (!emit_file(p, base)) return false;
    }
  }

  std::array<char, kBlock> zeros{};
  out.write(zeros.data(), kBlock);
  out.write(zeros.data(), kBlock);
  out.flush();
  if (!out) {
    error = "failed to finalise tar";
    return false;
  }
  return true;
}

// --------------------------------------------------------------- tar_list --
bool tar_list(const std::string& tar_path, std::vector<BundleEntry>& entries,
              std::string& error) {
  entries.clear();
  return tar_iterate(
      tar_path,
      [&](const RawEntry& e, uint64_t) {
        if (e.type == '5') {
          BundleEntry b;
          b.path = e.path;
          b.is_dir = true;
          b.mode = e.mode;
          b.mtime = e.mtime;
          entries.push_back(b);
        } else if (e.type == '0' || e.type == '7') {
          BundleEntry b;
          b.path = e.path;
          b.size = e.size;
          b.mode = e.mode;
          b.mtime = e.mtime;
          entries.push_back(b);
        }
        return true;
      },
      error);
}

// ------------------------------------------------------------ tar_extract --
bool tar_extract(const std::string& tar_path, const std::string& dest_dir, bool force,
                 std::string& error) {
  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec) {
    error = "cannot create " + dest_dir;
    return false;
  }
  std::ifstream in(tar_path, std::ios::binary);
  if (!in) {
    error = "cannot open tar";
    return false;
  }
  bool ok = tar_iterate(
      tar_path,
      [&](const RawEntry& e, uint64_t data_offset) {
        if (e.type == '5') {
          if (!safe_rel_path(e.path)) {
            error = "refusing unsafe directory path: " + e.path;
            return false;
          }
          fs::create_directories(fs::path(dest_dir) / e.path, ec);
          return true;
        }
        if (e.type != '0' && e.type != '7') return true;  // skip symlinks/devices/fifos
        if (!safe_rel_path(e.path)) {
          error = "refusing unsafe path in archive: " + e.path;
          return false;
        }
        fs::path target = fs::path(dest_dir) / e.path;
        fs::create_directories(target.parent_path(), ec);
        if (!force && fs::exists(target, ec)) {
          error = "output already exists: " + target.string() + " (use force to overwrite)";
          return false;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
          error = "cannot write " + target.string();
          return false;
        }
        in.seekg(static_cast<std::streamoff>(data_offset));
        std::array<char, 65536> buf{};
        uint64_t remaining = e.size;
        while (remaining > 0) {
          size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), remaining));
          in.read(buf.data(), static_cast<std::streamsize>(want));
          auto got = in.gcount();
          if (got <= 0) {
            error = "truncated file data for " + e.path;
            return false;
          }
          out.write(buf.data(), got);
          remaining -= static_cast<uint64_t>(got);
        }
        out.flush();
        if (!out) {
          error = "write failed for " + target.string();
          return false;
        }
        return true;
      },
      error);
  return ok;
}

bool looks_like_tar(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::array<char, kBlock> h{};
  in.read(h.data(), kBlock);
  if (in.gcount() != static_cast<std::streamsize>(kBlock)) return false;
  return std::memcmp(h.data() + kMagic, "ustar", 5) == 0;
}

// --------------------------------------------------------- high-level API --
Result compress_bundle(const std::vector<std::string>& inputs, const std::string& output,
                       const BundleOptions& opts, const ProgressFn& on_progress) {
  Result r;
  TempFile tar;
  if (tar.path.empty()) {
    r.error = "cannot create a temporary file";
    return r;
  }
  std::string error;
  if (!tar_write(inputs, tar.path, error)) {
    r.error = error;
    return r;
  }
  Job job;
  job.mode = Mode::Compress;
  job.input = tar.path;
  job.output = output;
  job.algorithm = opts.algorithm;
  job.level = opts.level;
  job.priority = opts.priority;
  job.password = opts.password;
  job.threads = opts.threads;
  job.chunk_size = opts.chunk_size;
  job.memory_limit = opts.memory_limit;
  job.bundle = true;
  job.force = opts.force;  // respect the caller's overwrite choice for the output
  return run_job(job, on_progress);
}

Result extract_archive(const std::string& archive, const std::string& dest_dir,
                       const std::string& password, bool force, const ProgressFn& on_progress) {
  Result r;
  ArchiveInfo info = read_archive_info(archive, false);
  if (!info.ok) {
    r.error = info.error;
    return r;
  }
  TempFile tmp;
  if (tmp.path.empty()) {
    r.error = "cannot create a temporary file";
    return r;
  }
  Job job;
  job.mode = Mode::Decompress;
  job.input = archive;
  job.output = tmp.path;
  job.password = password;
  job.force = true;
  job.verify = true;
  Result dec = run_job(job, on_progress);
  if (!dec.ok) {
    return dec;  // wrong password / tamper / corrupt surfaces here
  }

  std::string error;
  bool is_bundle = info.bundle || looks_like_tar(tmp.path);
  if (is_bundle) {
    if (!tar_extract(tmp.path, dest_dir, force, error)) {
      dec.ok = false;
      dec.error = error;
      return dec;
    }
  } else {
    std::string name = info.header.source_name;
    if (!safe_rel_path(name)) name = fs::path(archive).filename().string() + ".out";
    fs::path target = fs::path(dest_dir) / name;
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (!force && fs::exists(target, ec)) {
      dec.ok = false;
      dec.error = "output already exists: " + target.string() + " (use force to overwrite)";
      return dec;
    }
    fs::copy_file(tmp.path, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      dec.ok = false;
      dec.error = "cannot write " + target.string();
      return dec;
    }
    dec.stats.output_path = target.string();
  }
  dec.stats.bundle = is_bundle;
  dec.stats.output_path = is_bundle ? dest_dir : dec.stats.output_path;
  return dec;
}

bool list_bundle(const std::string& archive, const std::string& password,
                 std::vector<BundleEntry>& entries, std::string& error) {
  entries.clear();
  ArchiveInfo info = read_archive_info(archive, false);
  if (!info.ok) {
    error = info.error;
    return false;
  }
  TempFile tmp;
  if (tmp.path.empty()) {
    error = "cannot create a temporary file";
    return false;
  }
  Job job;
  job.mode = Mode::Decompress;
  job.input = archive;
  job.output = tmp.path;
  job.password = password;
  job.force = true;
  Result dec = run_job(job);
  if (!dec.ok) {
    error = dec.error;
    return false;
  }
  if (info.bundle || looks_like_tar(tmp.path)) {
    return tar_list(tmp.path, entries, error);
  }
  BundleEntry b;
  b.path = info.header.source_name.empty() ? fs::path(archive).filename().string()
                                           : info.header.source_name;
  b.size = info.header.orig_size;
  entries.push_back(b);
  return true;
}

}  // namespace kolma
