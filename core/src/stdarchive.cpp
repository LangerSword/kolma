// kolma -- standard-format archives via libarchive. See stdarchive.hpp.
#include "kolma/stdarchive.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace kolma {
namespace fs = std::filesystem;

#ifdef KOLMA_HAVE_LIBARCHIVE

#include <archive.h>
#include <archive_entry.h>

namespace {

struct WriteFree {
  void operator()(archive* a) const {
    if (a) {
      archive_write_close(a);
      archive_write_free(a);
    }
  }
};
struct ReadFree {
  void operator()(archive* a) const {
    if (a) {
      archive_read_close(a);
      archive_read_free(a);
    }
  }
};
using WritePtr = std::unique_ptr<archive, WriteFree>;
using ReadPtr = std::unique_ptr<archive, ReadFree>;

// Reject anything that could escape the destination directory.
bool safe_rel(const std::string& p) {
  if (p.empty()) return false;
  if (p.front() == '/' || p.front() == '\\') return false;
  if (p.size() >= 2 && p[1] == ':') return false;  // drive letter
  size_t start = 0;
  while (start < p.size()) {
    size_t end = p.find('/', start);
    if (end == std::string::npos) end = p.size();
    std::string comp = p.substr(start, end - start);
    if (comp == "..") return false;
    start = end + 1;
  }
  return true;
}

std::string format_from_extension(const std::string& output) {
  std::string name = fs::path(output).filename().string();
  auto ends = [&](const char* suf) {
    size_t n = std::strlen(suf);
    return name.size() >= n && name.compare(name.size() - n, n, suf) == 0;
  };
  if (ends(".tar.gz") || ends(".tgz")) return "tar.gz";
  if (ends(".tar.xz") || ends(".txz")) return "tar.xz";
  if (ends(".tar.bz2") || ends(".tbz2")) return "tar.bz2";
  if (ends(".tar")) return "tar";
  if (ends(".7z")) return "7z";
  return "zip";
}

// Collect (absolute, archive-relative, is_dir) for each input, stored under its
// basename; folders recurse; symlinks are skipped.
struct PackItem {
  fs::path abs;
  std::string rel;
  bool is_dir;
};
void collect(const fs::path& p, const std::string& rel, std::vector<PackItem>& out) {
  std::error_code ec;
  if (fs::is_symlink(p, ec)) return;
  if (fs::is_directory(p, ec)) {
    out.push_back({p, rel, true});
    for (const auto& child : fs::directory_iterator(p, ec)) {
      collect(child.path(), rel + "/" + child.path().filename().string(), out);
    }
  } else if (fs::is_regular_file(p, ec)) {
    out.push_back({p, rel, false});
  }
}

void support_read_formats(archive* a) {
  // Deliberately not archive_read_support_format_all(): that includes "raw",
  // which would swallow a native .kolma file as a single opaque entry.
  archive_read_support_format_zip(a);
  archive_read_support_format_7zip(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_gnutar(a);
  archive_read_support_format_rar(a);
  archive_read_support_format_rar5(a);
  archive_read_support_format_cpio(a);
  archive_read_support_format_iso9660(a);
  archive_read_support_format_xar(a);
  archive_read_support_format_ar(a);
  archive_read_support_format_warc(a);
  archive_read_support_format_mtree(a);
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_bzip2(a);
  archive_read_support_filter_lzma(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_lz4(a);
  archive_read_support_filter_compress(a);
  archive_read_support_filter_none(a);
}

}  // namespace

bool std_formats_available() { return true; }

std::string std_detect(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  unsigned char buf[512] = {0};
  in.read(reinterpret_cast<char*>(buf), sizeof buf);
  std::streamsize got = in.gcount();
  auto has = [&](std::streamsize n, const char* sig) {
    return got >= n && std::memcmp(buf, sig, static_cast<size_t>(n)) == 0;
  };
  std::string name = fs::path(path).filename().string();
  auto ends = [&](const char* suf) {
    size_t n = std::strlen(suf);
    return name.size() >= n && name.compare(name.size() - n, n, suf) == 0;
  };
  if (has(4, "PK\x03\x04") || has(4, "PK\x05\x06") || has(4, "PK\x07\x08")) return "zip";
  if (has(6, "7z\xBC\xAF\x27\x1C")) return "7z";
  if (has(6, "Rar!\x1A\x07")) return "rar";
  if (has(4, "\x28\xB5\x2F\xFD")) return "zst";
  if (has(2, "\x1F\x8B")) return (ends(".tar.gz") || ends(".tgz")) ? "tar.gz" : "gz";
  if (has(6, "\xFD" "7zXZ\x00")) return (ends(".tar.xz") || ends(".txz")) ? "tar.xz" : "xz";
  if (has(3, "BZh")) return (ends(".tar.bz2") || ends(".tbz2")) ? "tar.bz2" : "bz2";
  if (got >= 262 && std::memcmp(buf + 257, "ustar", 5) == 0) return "tar";
  return "";
}

bool std_is_standard(const std::string& path) { return !std_detect(path).empty(); }

std::string std_format_from_extension(const std::string& output) {
  std::string name = fs::path(output).filename().string();
  auto ends = [&](const char* suf) {
    size_t n = std::strlen(suf);
    return name.size() >= n && name.compare(name.size() - n, n, suf) == 0;
  };
  if (ends(".zip")) return "zip";
  if (ends(".tar.gz") || ends(".tgz")) return "tar.gz";
  if (ends(".tar.xz") || ends(".txz")) return "tar.xz";
  if (ends(".tar.bz2") || ends(".tbz2")) return "tar.bz2";
  if (ends(".tar")) return "tar";
  if (ends(".7z")) return "7z";
  return "";
}

bool std_pack(const std::vector<std::string>& inputs, const std::string& output,
              const StdPackOptions& opts, std::string& error, const ProgressFn& on_progress) {
  if (inputs.empty()) {
    error = "no inputs to pack";
    return false;
  }
  std::string fmt = opts.format.empty() ? format_from_extension(output) : opts.format;
  std::error_code ec;
  if (fs::exists(output, ec) && !opts.force) {
    error = "output already exists: " + output + " (use --force to overwrite)";
    return false;
  }

  std::vector<PackItem> items;
  for (const auto& in : inputs) {
    fs::path p(in);
    if (!fs::exists(p, ec)) {
      error = "input does not exist: " + in;
      return false;
    }
    collect(p, p.filename().string(), items);
  }

  WritePtr a(archive_write_new());
  if (!a) {
    error = "archive_write_new failed";
    return false;
  }
  int sr = ARCHIVE_OK;
  if (fmt == "zip") sr = archive_write_set_format_zip(a.get());
  else if (fmt == "7z") sr = archive_write_set_format_7zip(a.get());
  else if (fmt == "tar") sr = archive_write_set_format_pax_restricted(a.get());
  else if (fmt == "tar.gz") { sr = archive_write_set_format_pax_restricted(a.get()); archive_write_add_filter_gzip(a.get()); }
  else if (fmt == "tar.xz") { sr = archive_write_set_format_pax_restricted(a.get()); archive_write_add_filter_xz(a.get()); }
  else if (fmt == "tar.bz2") { sr = archive_write_set_format_pax_restricted(a.get()); archive_write_add_filter_bzip2(a.get()); }
  else {
    error = "unsupported format: " + fmt;
    return false;
  }
  if (sr != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }
  if (archive_write_open_filename(a.get(), output.c_str()) != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }

  uint64_t total = 0, done = 0;
  for (const auto& it : items) {
    if (!it.is_dir) total += fs::file_size(it.abs, ec);
  }

  char block[65536];
  for (const auto& it : items) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e(archive_entry_new(),
                                                                    &archive_entry_free);
    archive_entry_set_pathname(e.get(), it.rel.c_str());
    {
      std::error_code mec;
      time_t mt = 0;
      auto ftime = fs::last_write_time(it.abs, mec);
      if (!mec) {
        using namespace std::chrono;
        auto sys = time_point_cast<system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + system_clock::now());
        mt = static_cast<time_t>(duration_cast<seconds>(sys.time_since_epoch()).count());
      }
      archive_entry_set_mtime(e.get(), mt, 0);
    }
    if (it.is_dir) {
      archive_entry_set_filetype(e.get(), AE_IFDIR);
      archive_entry_set_perm(e.get(), 0755);
      archive_entry_set_size(e.get(), 0);
    } else {
      archive_entry_set_filetype(e.get(), AE_IFREG);
      archive_entry_set_perm(e.get(), 0644);
      archive_entry_set_size(e.get(), static_cast<la_int64_t>(fs::file_size(it.abs, ec)));
    }
    if (archive_write_header(a.get(), e.get()) != ARCHIVE_OK) {
      error = archive_error_string(a.get());
      return false;
    }
    if (!it.is_dir) {
      std::ifstream in(it.abs, std::ios::binary);
      while (in) {
        in.read(block, sizeof block);
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        la_ssize_t w = archive_write_data(a.get(), block, static_cast<size_t>(n));
        if (w < 0) {
          error = archive_error_string(a.get());
          return false;
        }
        done += static_cast<uint64_t>(n);
        if (on_progress) {
          Progress pr;
          pr.bytes_in = done;
          pr.bytes_out = done;
          pr.ratio = total ? double(done) / double(total) : 1.0;
          on_progress(pr);
        }
      }
    }
  }
  if (archive_write_close(a.get()) != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }
  return true;
}

bool std_extract(const std::string& archive, const std::string& dest_dir, bool force,
                 std::string& error, const ProgressFn& on_progress) {
  ReadPtr a(archive_read_new());
  if (!a) {
    error = "archive_read_new failed";
    return false;
  }
  support_read_formats(a.get());
  if (archive_read_open_filename(a.get(), archive.c_str(), 65536) != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  WritePtr disk(archive_write_disk_new());
  archive_write_disk_set_options(disk.get(), ARCHIVE_EXTRACT_TIME |
                                                 ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                 ARCHIVE_EXTRACT_SECURE_SYMLINKS);
  archive_write_disk_set_standard_lookup(disk.get());

  archive_entry* entry = nullptr;
  uint64_t done = 0;
  int r;
  while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    std::string rel = archive_entry_pathname(entry);
    if (!safe_rel(rel)) {
      error = "refusing unsafe path in archive: " + rel;
      return false;
    }
    fs::path full = fs::path(dest_dir) / rel;
    bool is_dir = archive_entry_filetype(entry) == AE_IFDIR;
    if (!force && !is_dir && fs::exists(full, ec)) {
      error = "already exists: " + full.string() + " (use --force to overwrite)";
      return false;
    }
    archive_entry_set_pathname(entry, full.string().c_str());
    if (archive_write_header(disk.get(), entry) != ARCHIVE_OK) {
      error = archive_error_string(disk.get());
      return false;
    }
    const void* buf = nullptr;
    size_t len = 0;
    la_int64_t off = 0;
    while (archive_read_data_block(a.get(), &buf, &len, &off) == ARCHIVE_OK) {
      if (archive_write_data_block(disk.get(), buf, len, off) != ARCHIVE_OK) {
        error = archive_error_string(disk.get());
        return false;
      }
      done += len;
      if (on_progress) {
        Progress pr;
        pr.bytes_in = done;
        pr.bytes_out = done;
        on_progress(pr);
      }
    }
    if (archive_write_finish_entry(disk.get()) != ARCHIVE_OK) {
      error = archive_error_string(disk.get());
      return false;
    }
  }
  if (r != ARCHIVE_EOF) {
    error = archive_error_string(a.get());
    return false;
  }
  return true;
}

bool std_list(const std::string& archive, std::vector<StdEntry>& entries, std::string& error) {
  ReadPtr a(archive_read_new());
  if (!a) {
    error = "archive_read_new failed";
    return false;
  }
  support_read_formats(a.get());
  if (archive_read_open_filename(a.get(), archive.c_str(), 65536) != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }
  archive_entry* entry = nullptr;
  int r;
  while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    StdEntry e;
    e.path = archive_entry_pathname(entry);
    e.size = static_cast<uint64_t>(archive_entry_size(entry));
    e.is_dir = archive_entry_filetype(entry) == AE_IFDIR;
    e.mtime = static_cast<int64_t>(archive_entry_mtime(entry));
    entries.push_back(std::move(e));
    archive_read_data_skip(a.get());
  }
  if (r != ARCHIVE_EOF) {
    error = archive_error_string(a.get());
    return false;
  }
  return true;
}

bool std_test(const std::string& archive, uint64_t& entries, std::string& error) {
  ReadPtr a(archive_read_new());
  if (!a) {
    error = "archive_read_new failed";
    return false;
  }
  support_read_formats(a.get());
  if (archive_read_open_filename(a.get(), archive.c_str(), 65536) != ARCHIVE_OK) {
    error = archive_error_string(a.get());
    return false;
  }
  entries = 0;
  archive_entry* entry = nullptr;
  int r;
  while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    ++entries;
    // Stream every data block: libarchive verifies stored CRCs as it reads, so
    // a corrupt entry fails here rather than at extract time.
    const void* buf = nullptr;
    size_t len = 0;
    int64_t off = 0;
    int dr;
    while ((dr = archive_read_data_block(a.get(), &buf, &len, &off)) == ARCHIVE_OK) {
    }
    if (dr != ARCHIVE_EOF) {
      error = std::string("entry ") + archive_entry_pathname(entry) + ": " + archive_error_string(a.get());
      return false;
    }
  }
  if (r != ARCHIVE_EOF) {
    error = archive_error_string(a.get());
    return false;
  }
  return true;
}

#else  // !KOLMA_HAVE_LIBARCHIVE — graceful stubs so the engine still builds.

bool std_formats_available() { return false; }
std::string std_detect(const std::string&) { return ""; }
bool std_is_standard(const std::string&) { return false; }
std::string std_format_from_extension(const std::string&) { return ""; }
bool std_pack(const std::vector<std::string>&, const std::string&, const StdPackOptions&,
              std::string& error, const ProgressFn&) {
  error = "standard-format support not built (libarchive missing)";
  return false;
}
bool std_extract(const std::string&, const std::string&, bool, std::string& error,
                 const ProgressFn&) {
  error = "standard-format support not built (libarchive missing)";
  return false;
}
bool std_list(const std::string&, std::vector<StdEntry>&, std::string& error) {
  error = "standard-format support not built (libarchive missing)";
  return false;
}
bool std_test(const std::string&, uint64_t&, std::string& error) {
  error = "standard-format support not built (libarchive missing)";
  return false;
}

#endif

}  // namespace kolma
