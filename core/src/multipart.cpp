// kolma -- multipart archives and in-place extraction. See multipart.hpp.
#include "kolma/multipart.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace kolma {
namespace fs = std::filesystem;

namespace {

// foo.zip.003 -> { "foo.zip", 3 }. Returns false when there is no .NNN tail.
bool split_volume_name(const std::string& path, std::string& base, int& num) {
  size_t dot = path.rfind('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return false;
  std::string tail = path.substr(dot + 1);
  if (tail.size() != 3) return false;
  for (char c : tail)
    if (c < '0' || c > '9') return false;
  base = path.substr(0, dot);
  num = std::stoi(tail);
  return num >= 1;
}

std::string volume_name(const std::string& base, int num) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%03d", num);
  return base + "." + buf;
}

}  // namespace

uint32_t split_file(const std::string& input, uint64_t part_size, std::string& error) {
  if (part_size < 1024) {
    error = "part size must be at least 1 KiB";
    return 0;
  }
  std::error_code ec;
  if (!fs::is_regular_file(input, ec)) {
    error = "input does not exist: " + input;
    return 0;
  }
  uint64_t size = fs::file_size(input, ec);
  if (ec) {
    error = "cannot read input size";
    return 0;
  }
  uint64_t count64 = size / part_size + (size % part_size ? 1 : 0);
  if (count64 == 0) count64 = 1;  // an empty file still yields one empty volume
  if (count64 > 999) {
    error = "would need more than 999 volumes; use a larger part size";
    return 0;
  }
  // Refuse to clobber an existing series.
  for (uint64_t i = 1; i <= count64; ++i) {
    std::string v = volume_name(input, int(i));
    if (fs::exists(v, ec)) {
      error = "volume already exists: " + v + " (remove it first)";
      return 0;
    }
  }
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    error = "cannot open input";
    return 0;
  }
  std::vector<char> buf(size_t(std::min<uint64_t>(part_size, 1ull << 20)));
  uint32_t written = 0;
  uint64_t remaining = size;
  for (uint64_t i = 1; i <= count64; ++i) {
    std::string v = volume_name(input, int(i));
    std::ofstream out(v, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write volume: " + v;
      return 0;
    }
    uint64_t left = std::min<uint64_t>(part_size, remaining);
    while (left > 0) {
      size_t want = size_t(std::min<uint64_t>(left, buf.size()));
      in.read(buf.data(), std::streamsize(want));
      std::streamsize got = in.gcount();
      if (got <= 0) break;
      out.write(buf.data(), got);
      left -= uint64_t(got);
      remaining -= uint64_t(got);
    }
    out.close();
    if (!out) {
      error = "failed writing volume: " + v;
      return 0;
    }
    ++written;
  }
  return written;
}

VolumeSet detect_volume_set(const std::string& path) {
  VolumeSet vs;
  std::string base;
  int num = 0;
  if (!split_volume_name(path, base, num)) return vs;
  std::error_code ec;
  // Walk the series from .001 upward until a gap.
  for (int i = 1; i <= 999; ++i) {
    std::string v = volume_name(base, i);
    if (!fs::is_regular_file(v, ec)) break;
    vs.paths.push_back(v);
    vs.sizes.push_back(fs::file_size(v, ec));
    vs.total_size += vs.sizes.back();
  }
  if (vs.paths.empty()) return vs;
  // The member we were given must actually be in the series.
  bool found = false;
  for (const auto& p : vs.paths)
    if (p == path) found = true;
  if (!found) {
    vs.paths.clear();
    vs.sizes.clear();
    vs.total_size = 0;
    return vs;
  }
  vs.base = base;
  return vs;
}

bool is_multipart(const std::string& path) { return !detect_volume_set(path).paths.empty(); }

bool join_multipart(const VolumeSet& vs, const std::string& output, bool force,
                    std::string& error, const ProgressFn& on_progress) {
  if (vs.paths.empty()) {
    error = "no volumes to join";
    return false;
  }
  std::error_code ec;
  if (fs::exists(output, ec) && !force) {
    error = "output already exists: " + output + " (use --force to overwrite)";
    return false;
  }
  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot open output: " + output;
    return false;
  }
  std::vector<char> buf(1ull << 20);
  uint64_t done = 0;
  for (const auto& v : vs.paths) {
    std::ifstream in(v, std::ios::binary);
    if (!in) {
      error = "cannot read volume: " + v;
      return false;
    }
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      std::streamsize got = in.gcount();
      if (got <= 0) break;
      out.write(buf.data(), got);
      done += uint64_t(got);
      if (on_progress) {
        Progress pr;
        pr.bytes_in = done;
        pr.bytes_out = done;
        pr.ratio = vs.total_size ? double(done) / double(vs.total_size) : 1.0;
        on_progress(pr);
      }
    }
  }
  out.close();
  if (!out) {
    error = "failed writing output";
    return false;
  }
  return true;
}

#ifdef KOLMA_HAVE_LIBARCHIVE

#include <archive.h>
#include <archive_entry.h>

namespace {

bool safe_rel(const std::string& p) {
  if (p.empty()) return false;
  if (p.front() == '/' || p.front() == '\\') return false;
  if (p.size() >= 2 && p[1] == ':') return false;
  size_t start = 0;
  while (start < p.size()) {
    size_t end = p.find('/', start);
    if (end == std::string::npos) end = p.size();
    if (p.substr(start, end - start) == "..") return false;
    start = end + 1;
  }
  return true;
}

// Logical reader over the volume set, presented to libarchive via callbacks.
struct ReadCtx {
  const VolumeSet* vs = nullptr;
  std::vector<uint64_t> ends;  // cumulative end offset per volume
  uint64_t pos = 0;
  std::vector<char> buf;
  std::ifstream in;
  size_t cur_vol = SIZE_MAX;
  bool in_place = false;
  std::vector<bool> deleted;
};

size_t volume_index(const ReadCtx* c, uint64_t pos) {
  // first volume whose cumulative end is > pos
  size_t lo = 0, hi = c->ends.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c->ends[mid] <= pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < c->ends.size() ? lo : c->ends.size() - 1;
}

la_ssize_t mp_read(archive* a, void* client, const void** buffer) {
  (void)a;
  ReadCtx* c = static_cast<ReadCtx*>(client);
  if (c->pos >= c->vs->total_size) {
    *buffer = nullptr;
    return 0;
  }
  size_t vi = volume_index(c, c->pos);
  uint64_t start = vi == 0 ? 0 : c->ends[vi - 1];
  uint64_t within = c->pos - start;
  uint64_t left = c->vs->sizes[vi] - within;
  size_t want = size_t(std::min<uint64_t>(c->buf.size(), left));
  if (c->cur_vol != vi) {
    if (c->in.is_open()) c->in.close();
    c->in.open(c->vs->paths[vi], std::ios::binary);
    c->cur_vol = vi;
  }
  if (!c->in) {
    *buffer = nullptr;
    return ARCHIVE_FATAL;
  }
  c->in.seekg(std::streamoff(within));
  c->in.read(c->buf.data(), std::streamsize(want));
  std::streamsize got = c->in.gcount();
  if (got <= 0) {
    *buffer = nullptr;
    return 0;
  }
  *buffer = c->buf.data();
  c->pos += uint64_t(got);
  return la_ssize_t(got);
}

la_int64_t mp_seek(archive* a, void* client, la_int64_t offset, int whence) {
  (void)a;
  ReadCtx* c = static_cast<ReadCtx*>(client);
  la_int64_t base = whence == SEEK_SET ? 0
                    : whence == SEEK_CUR ? la_int64_t(c->pos)
                                         : la_int64_t(c->vs->total_size);
  la_int64_t np = base + offset;
  if (np < 0 || np > la_int64_t(c->vs->total_size)) return ARCHIVE_FATAL;
  if (c->in_place && np < la_int64_t(c->vs->total_size)) {
    size_t vi = volume_index(c, uint64_t(np));
    if (c->deleted[vi]) return ARCHIVE_FATAL;  // never seek into reclaimed space
  }
  c->pos = uint64_t(np);
  return np;
}

int mp_close(archive* a, void* client) {
  (void)a;
  ReadCtx* c = static_cast<ReadCtx*>(client);
  if (c->in.is_open()) c->in.close();
  return ARCHIVE_OK;
}

void init_ctx(ReadCtx& c, const VolumeSet& vs, bool in_place) {
  c.vs = &vs;
  c.pos = 0;
  c.buf.resize(65536);
  c.in_place = in_place;
  c.cur_vol = SIZE_MAX;
  c.ends.clear();
  uint64_t acc = 0;
  for (uint64_t s : vs.sizes) {
    acc += s;
    c.ends.push_back(acc);
  }
  c.deleted.assign(vs.paths.size(), false);
}

archive* open_logical(const VolumeSet& vs, ReadCtx& c, bool in_place, std::string& error) {
  init_ctx(c, vs, in_place);
  archive* a = archive_read_new();
  if (!a) {
    error = "archive_read_new failed";
    return nullptr;
  }
  archive_read_support_format_zip(a);
  archive_read_support_format_7zip(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_gnutar(a);
  archive_read_support_format_rar(a);
  archive_read_support_format_rar5(a);
  archive_read_support_format_cpio(a);
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_bzip2(a);
  archive_read_support_filter_lzma(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_lz4(a);
  archive_read_support_filter_none(a);
  archive_read_set_seek_callback(a, mp_seek);
  if (archive_read_open(a, &c, nullptr, mp_read, mp_close) != ARCHIVE_OK) {
    error = archive_error_string(a);
    archive_read_free(a);
    return nullptr;
  }
  return a;
}

// "stream" (forward tar/compressed-tar), "indexed" (zip/7z/rar), or "".
std::string logical_kind(const VolumeSet& vs) {
  std::ifstream in(vs.paths[0], std::ios::binary);
  unsigned char buf[512] = {0};
  in.read(reinterpret_cast<char*>(buf), sizeof buf);
  std::streamsize got = in.gcount();
  auto has = [&](std::streamsize n, const char* sig) {
    return got >= n && std::memcmp(buf, sig, size_t(n)) == 0;
  };
  if (has(4, "PK\x03\x04") || has(4, "PK\x05\x06") || has(4, "PK\x07\x08")) return "indexed";
  if (has(6, "7z\xBC\xAF\x27\x1C")) return "indexed";
  if (has(6, "Rar!\x1A\x07")) return "indexed";
  if (has(2, "\x1F\x8B") || has(3, "BZh") || has(6, "\xFD" "7zXZ\x00")) return "stream";
  if (got >= 262 && std::memcmp(buf + 257, "ustar", 5) == 0) return "stream";
  return "";
}

// Delete volumes whose data is committed plus a safety margin (in-place only).
void reclaim(ReadCtx& c, bool final) {
  if (!c.in_place) return;
  // Windows refuses to delete a file that still has an open handle; release ours
  // before removing volumes. mp_read reopens the current volume on demand.
  if (c.in.is_open()) c.in.close();
  c.cur_vol = SIZE_MAX;
  const uint64_t margin = 16ull << 20;  // far larger than libarchive read-ahead
  for (size_t i = 0; i < c.vs->paths.size(); ++i) {
    if (c.deleted[i]) continue;
    bool safe = final || c.ends[i] + margin <= c.pos;
    if (safe) {
      std::error_code ec;
      fs::remove(c.vs->paths[i], ec);
      if (!ec) c.deleted[i] = true;
    }
  }
}

}  // namespace

bool test_multipart(const VolumeSet& vs, uint64_t& entries, std::string& error) {
  if (vs.paths.empty()) {
    error = "no volumes to test";
    return false;
  }
  if (logical_kind(vs).empty()) {
    error = "the split archive is not a recognised format";
    return false;
  }
  ReadCtx c;
  archive* a = open_logical(vs, c, false, error);
  if (!a) return false;
  entries = 0;
  archive_entry* entry = nullptr;
  int r;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    ++entries;
    const void* buf = nullptr;
    size_t len = 0;
    int64_t off = 0;
    int dr;
    while ((dr = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
    }
    if (dr != ARCHIVE_EOF) {
      error = std::string("entry ") + archive_entry_pathname(entry) + ": " + archive_error_string(a);
      archive_read_free(a);
      return false;
    }
  }
  if (r != ARCHIVE_EOF) {
    error = archive_error_string(a);
    archive_read_free(a);
    return false;
  }
  archive_read_free(a);
  return true;
}

bool extract_multipart(const VolumeSet& vs, const std::string& dest_dir,
                       const MultipartExtractOptions& opts, std::string& error,
                       const ProgressFn& on_progress) {
  if (vs.paths.empty()) {
    error = "no volumes to extract";
    return false;
  }
  std::string kind = logical_kind(vs);
  if (kind.empty()) {
    error = "the split archive is not a recognised format";
    return false;
  }
  if (opts.in_place && kind != "stream") {
    error = "in-place extraction needs a forward-streaming tar archive; "
            "zip/7z keep their index at the end — extract them normally (no --in-place)";
    return false;
  }

  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  // Pre-flight: for in-place, make sure the growth (extracted - archive) plus the
  // reserve fits in free space before we start deleting anything.
  if (opts.in_place) {
    ReadCtx lc;
    archive* la = open_logical(vs, lc, false, error);
    if (!la) return false;
    uint64_t extracted = 0;
    archive_entry* e = nullptr;
    int r;
    while ((r = archive_read_next_header(la, &e)) == ARCHIVE_OK) {
      if (archive_entry_filetype(e) == AE_IFREG)
        extracted += uint64_t(archive_entry_size(e));
      archive_read_data_skip(la);
    }
    archive_read_free(la);
    if (r != ARCHIVE_EOF) {
      error = "could not pre-scan the split archive: " + error;
      return false;
    }
    uint64_t growth = extracted > vs.total_size ? extracted - vs.total_size : 0;
    uint64_t need = growth + opts.keep_free + (16ull << 20);
    auto sp = fs::space(dest_dir, ec);
    if (!ec && sp.available < need) {
      error = "not enough free space for in-place extraction (need ~" +
              std::to_string(need >> 20) + " MiB free, have " +
              std::to_string(sp.available >> 20) + " MiB); free space or extract normally";
      return false;
    }
  }

  ReadCtx c;
  archive* a = open_logical(vs, c, opts.in_place, error);
  if (!a) return false;

  archive* disk = archive_write_disk_new();
  archive_write_disk_set_options(disk, ARCHIVE_EXTRACT_TIME |
                                           ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                           ARCHIVE_EXTRACT_SECURE_SYMLINKS);
  archive_write_disk_set_standard_lookup(disk);

  archive_entry* entry = nullptr;
  uint64_t done = 0;
  int r;
  bool ok = true;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    std::string rel = archive_entry_pathname(entry);
    if (!safe_rel(rel)) {
      error = "refusing unsafe path in archive: " + rel;
      ok = false;
      break;
    }
    fs::path full = fs::path(dest_dir) / rel;
    bool is_dir = archive_entry_filetype(entry) == AE_IFDIR;
    if (!opts.force && !is_dir && fs::exists(full, ec)) {
      error = "already exists: " + full.string() + " (use --force to overwrite)";
      ok = false;
      break;
    }
    archive_entry_set_pathname(entry, full.string().c_str());
    if (archive_write_header(disk, entry) != ARCHIVE_OK) {
      error = archive_error_string(disk);
      ok = false;
      break;
    }
    const void* buf = nullptr;
    size_t len = 0;
    la_int64_t off = 0;
    while (archive_read_data_block(a, &buf, &len, &off) == ARCHIVE_OK) {
      if (archive_write_data_block(disk, buf, len, off) != ARCHIVE_OK) {
        error = archive_error_string(disk);
        ok = false;
        break;
      }
      done += len;
      if (on_progress) {
        Progress pr;
        pr.bytes_in = done;
        pr.bytes_out = done;
        on_progress(pr);
      }
    }
    if (!ok) break;
    if (archive_write_finish_entry(disk) != ARCHIVE_OK) {
      error = archive_error_string(disk);
      ok = false;
      break;
    }
    reclaim(c, false);  // data up to here is committed; reclaim with a margin
  }
  if (ok && r != ARCHIVE_EOF) {
    error = archive_error_string(a);
    ok = false;
  }
  if (ok) reclaim(c, true);  // fully done: reclaim the remaining volumes
  archive_write_free(disk);
  archive_read_free(a);
  return ok;
}

#else  // !KOLMA_HAVE_LIBARCHIVE

bool extract_multipart(const VolumeSet&, const std::string&, const MultipartExtractOptions&,
                       std::string& error, const ProgressFn&) {
  error = "multipart extraction needs libarchive (not built)";
  return false;
}

#endif

}  // namespace kolma
