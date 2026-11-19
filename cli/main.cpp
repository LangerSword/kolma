// kolma -- command line front end.
//
// The TUI and any other client talk to the engine through this binary: with
// --json it emits one JSON object per progress tick on stderr and a final JSON
// object on stdout, which is a stable, parseable contract.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define KOLMA_ISATTY(fd) _isatty(fd)
#define KOLMA_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define KOLMA_ISATTY(fd) isatty(fd)
#define KOLMA_FILENO(f) fileno(f)
#endif

#include "kolma/algorithm.hpp"
#include "kolma/analyzer.hpp"
#include "kolma/archive.hpp"
#include "kolma/bundle.hpp"
#include "kolma/engine.hpp"
#include "kolma/file.hpp"
#include "kolma/media.hpp"
#include "kolma/multipart.hpp"
#include "kolma/stdarchive.hpp"

#include <filesystem>

namespace {

const char* kVersion = "1.2.0";

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string jstr(const std::string& s) { return "\"" + json_escape(s) + "\""; }

struct Options {
  bool json = false;
  bool quiet = false;
  bool no_check = false;
  bool verify = true;
  bool force = false;
  bool password_flag = false;
  bool stdout_output = false;  // -o -: payload to stdout, reports to stderr
  std::string algorithm = "auto";
  std::string priority = "balanced";
  std::string output;
  std::string format;  // standard archive format: zip, tar.gz, tar.xz, tar.bz2, 7z
  std::string quality;  // shrink preset: quality | balanced | smallest
  std::string password;
  int level = 0;
  long long threads = 0;
  long long chunk_size = 0;
  long long memory_limit = 256ll << 20;
  long long limit = 0;
  long long part_size = 0;
  bool in_place = false;
  long long keep_free = -1;  // -1 = engine default reserve
  std::vector<std::string> positionals;
  std::string error;
};

bool parse_size(const std::string& text, long long& out) {
  if (text.empty()) return false;
  size_t i = 0;
  long long value = 0;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
    value = value * 10 + (text[i] - '0');
    ++i;
  }
  if (i == 0) return false;
  std::string suffix = text.substr(i);
  for (char& c : suffix) c = char(std::tolower(static_cast<unsigned char>(c)));
  long long mult = 1;
  if (suffix == "k" || suffix == "kb" || suffix == "kib")
    mult = 1024;
  else if (suffix == "m" || suffix == "mb" || suffix == "mib")
    mult = 1024 * 1024;
  else if (suffix == "g" || suffix == "gb" || suffix == "gib")
    mult = 1024 * 1024 * 1024;
  else if (!suffix.empty())
    return false;
  out = value * mult;
  return true;
}

Options parse_options(int argc, char** argv, int start) {
  Options o;
  for (int i = start; i < argc; ++i) {
    std::string arg = argv[i];
    auto value_of = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        o.error = std::string("missing value for ") + flag;
        return {};
      }
      return argv[++i];
    };
    if (arg == "--json") {
      o.json = true;
    } else if (arg == "--quiet" || arg == "-q") {
      o.quiet = true;
    } else if (arg == "--no-check") {
      o.no_check = true;
    } else if (arg == "--no-verify") {
      o.verify = false;
    } else if (arg == "--force" || arg == "-f") {
      o.force = true;
    } else if (arg == "--algo" || arg == "-a") {
      o.algorithm = value_of("--algo");
    } else if (arg == "--priority" || arg == "-p") {
      o.priority = value_of("--priority");
    } else if (arg == "--out" || arg == "-o") {
      o.output = value_of("--out");
    } else if (arg == "--format") {
      o.format = value_of("--format");
    } else if (arg == "--quality") {
      o.quality = value_of("--quality");
    } else if (arg == "--part-size") {
      std::string v = value_of("--part-size");
      if (!v.empty() && !parse_size(v, o.part_size)) o.error = "--part-size expects a size (e.g. 100M)";
    } else if (arg == "--in-place") {
      o.in_place = true;
    } else if (arg == "--keep-free") {
      std::string v = value_of("--keep-free");
      if (!v.empty() && !parse_size(v, o.keep_free)) o.error = "--keep-free expects a size";
    } else if (arg == "--password") {
      o.password = value_of("--password");
      o.password_flag = true;
    } else if (arg == "--level" || arg == "-l") {
      std::string v = value_of("--level");
      if (v.empty()) continue;
      o.level = std::atoi(v.c_str());
      if (o.level < 1 || o.level > 9) {
        o.error = "--level must be between 1 and 9";
      }
    } else if (arg == "--threads" || arg == "-t") {
      std::string v = value_of("--threads");
      if (!v.empty() && !parse_size(v, o.threads)) o.error = "--threads expects a number";
    } else if (arg == "--chunk-size") {
      std::string v = value_of("--chunk-size");
      if (!v.empty() && !parse_size(v, o.chunk_size)) o.error = "--chunk-size expects a size (e.g. 1M)";
      if (o.error.empty() && o.chunk_size > (64ll << 20))
        o.error = "--chunk-size exceeds the 64 MiB maximum";
    } else if (arg == "--memory-limit") {
      std::string v = value_of("--memory-limit");
      if (!v.empty() && !parse_size(v, o.memory_limit)) o.error = "--memory-limit expects a size";
    } else if (arg == "--limit") {
      std::string v = value_of("--limit");
      if (!v.empty() && !parse_size(v, o.limit)) o.error = "--limit expects a size";
    } else if (arg == "--help" || arg == "-h") {
      o.positionals.push_back("--help");
    } else if (!arg.empty() && arg[0] == '-' && arg.size() > 1) {
      o.error = "unknown flag: " + arg;
    } else {
      o.positionals.push_back(arg);
    }
    if (!o.error.empty()) return o;
  }
  // A password on the command line is visible to every other process; the
  // environment variable is the safer channel and is what the TUI uses.
  if (o.password.empty()) {
    if (const char* env = std::getenv("KOLMA_PASSWORD")) o.password = env;
  }
  return o;
}

void print_usage(std::ostream& os) {
  os << "kolma " << kVersion << " -- a pluggable compression engine\n\n"
     << "usage:\n"
     << "  kolma compress   <file|folder>... [options]  # --format zip for a universal archive\n"
     << "  kolma decompress <archive> [options]   # -o - writes the payload to stdout\n"
     << "  kolma extract    <archive> [dest-dir]  # opens zip/7z/tar.gz/... and .kolma\n"
     << "  kolma shrink     <video|image>         # smaller file, still playable (needs ffmpeg)\n"
     << "  kolma split      <file> --part-size N  # cut into .001/.002/… volumes\n"
     << "  kolma join       <file.001> [-o out]   # reassemble volumes\n"
     << "  kolma list       <archive>             # show the contents of an archive\n"
     << "  kolma analyze    <file> [--priority P] [--json]\n"
     << "  kolma bench      <file> [--level N] [--limit SIZE] [--json]\n"
     << "  kolma info       <archive> [--no-check] [--json]\n"
     << "  kolma algos      [--json]\n"
     << "  kolma version\n\n"
     << "options:\n"
     << "  -a, --algo <id>        store | rle | huffman | lz77 | lzw | bwt-mtf-ari | delta-rle | auto\n"
     << "  -l, --level <1-9>      effort; 1 = fastest, 9 = best ratio\n"
     << "  -p, --priority <p>     maxratio | balanced | maxspeed (used when --algo is auto)\n"
     << "  -o, --out <path>       output path; - writes to stdout (reports go to stderr)\n"
     << "      --format <fmt>     zip | tar | tar.gz | tar.xz | tar.bz2 | 7z (universal archive)\n"
     << "      --quality <q>      shrink preset: quality | balanced | smallest\n"
     << "      --part-size <s>    volume size for split (e.g. 100M, 4G)\n"
     << "      --in-place         extract tar volumes, reclaiming space as it goes\n"
     << "      --keep-free <s>    free-space reserve for --in-place (default 2G)\n"
     << "      --password <pw>    encrypt (visible in ps; prefer KOLMA_PASSWORD)\n"
     << "  -t, --threads <n>      worker threads (default: hardware concurrency)\n"
     << "      --chunk-size <s>   bytes per chunk, accepts 64K / 4M suffixes (max 64M)\n"
     << "      --memory-limit <s> working-set ceiling (default 256M)\n"
     << "      --no-verify        skip the checksum check on decompression\n"
     << "  -f, --force            overwrite an existing output file\n"
     << "      --json             machine-readable progress and result\n"
     << "  -q, --quiet            suppress the progress line\n";
}

void emit_progress_json(const kolma::Progress& p) {
  std::fprintf(stderr,
               "{\"event\":\"progress\",\"bytes_in\":%llu,\"bytes_out\":%llu,\"chunks_done\":%u,"
               "\"chunks_total\":%u,\"elapsed_ms\":%.3f,\"throughput_mbps\":%.3f,\"ratio\":%.6f}\n",
               static_cast<unsigned long long>(p.bytes_in),
               static_cast<unsigned long long>(p.bytes_out), p.chunks_done, p.chunks_total,
               p.elapsed_ms, p.throughput_mbps, p.ratio);
}

void emit_progress_human(const kolma::Progress& p, bool quiet) {
  if (quiet || !KOLMA_ISATTY(KOLMA_FILENO(stderr))) return;
  double pct = p.chunks_total > 0 ? 100.0 * double(p.chunks_done) / double(p.chunks_total) : 0.0;
  std::fprintf(stderr, "\r  %5.1f%%  %s  ratio %s   ", pct,
               kolma::human_rate(p.throughput_mbps).c_str(), kolma::format_ratio(p.ratio).c_str());
  std::fflush(stderr);
}

void clear_progress_line(bool quiet) {
  if (quiet || !KOLMA_ISATTY(KOLMA_FILENO(stderr))) return;
  std::fprintf(stderr, "\r\033[K");
}

std::string stats_json(const kolma::Stats& s) {
  std::ostringstream os;
  os << "{\"algorithm\":" << jstr(s.algorithm) << ",\"algorithm_name\":" << jstr(s.algorithm_name)
     << ",\"level\":" << s.level << ",\"orig_size\":" << s.orig_size
     << ",\"comp_size\":" << s.comp_size << ",\"ratio\":" << s.ratio
     << ",\"elapsed_ms\":" << s.elapsed_ms << ",\"throughput_mbps\":" << s.throughput_mbps
     << ",\"chunks\":" << s.chunks << ",\"threads\":" << s.threads
     << ",\"chunk_size\":" << s.chunk_size << ",\"orig_crc\":" << jstr(kolma::crc32_hex(s.orig_crc))
     << ",\"restored_crc\":" << jstr(kolma::crc32_hex(s.restored_crc))
     << ",\"verified\":" << (s.verified ? "true" : "false")
     << ",\"restoring\":" << (s.restoring ? "true" : "false")
     << ",\"encrypted\":" << (s.encrypted ? "true" : "false")
     << ",\"authenticated\":" << (s.authenticated ? "true" : "false")
     << ",\"output\":" << jstr(s.output_path) << "}";
  return os.str();
}

std::string analysis_json(const kolma::Analysis& a) {
  std::ostringstream os;
  os << "{\"path\":" << jstr(a.file.path) << ",\"size\":" << a.file.size
     << ",\"type\":" << jstr(kolma::file_type_name(a.file.type))
     << ",\"type_detail\":" << jstr(a.file.type_detail) << ",\"entropy\":" << a.entropy
     << ",\"compressibility\":" << a.compressibility
     << ",\"recommended_algo\":" << jstr(a.recommended_algo)
     << ",\"recommended_level\":" << a.recommended_level
     << ",\"store_raw\":" << (a.store_raw ? "true" : "false")
     << ",\"rationale\":" << jstr(a.rationale) << ",\"trials\":[";
  for (size_t i = 0; i < a.trials.size(); ++i) {
    const kolma::AlgoTrial& t = a.trials[i];
    if (i) os << ",";
    os << "{\"algo\":" << jstr(t.algo_id) << ",\"name\":" << jstr(t.algo_name)
       << ",\"ratio\":" << t.ratio << ",\"elapsed_ms\":" << t.elapsed_ms
       << ",\"compressed_bytes\":" << t.compressed_bytes << "}";
  }
  os << "]}";
  return os.str();
}

std::string benchmark_json(const std::vector<kolma::BenchmarkRow>& rows) {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    const kolma::BenchmarkRow& b = rows[i];
    if (i) os << ",";
    os << "{\"algo\":" << jstr(b.algo_id) << ",\"name\":" << jstr(b.algo_name)
       << ",\"ratio\":" << b.ratio << ",\"input_bytes\":" << b.input_bytes
       << ",\"output_bytes\":" << b.output_bytes << ",\"elapsed_ms\":" << b.elapsed_ms
       << ",\"throughput_mbps\":" << b.throughput_mbps << ",\"memory_bytes\":" << b.memory_bytes
       << ",\"block_size\":" << b.block_size
       << ",\"best_ratio\":" << (b.best_ratio ? "true" : "false")
       << ",\"best_speed\":" << (b.best_speed ? "true" : "false") << "}";
  }
  os << "]";
  return os.str();
}

std::string archive_info_json(const kolma::ArchiveInfo& info) {
  std::ostringstream os;
  if (!info.ok) {
    os << "{\"ok\":false,\"error\":" << jstr(info.error) << "}";
    return os.str();
  }
  const kolma::ArchiveHeader& h = info.header;
  os << "{\"ok\":true,\"version\":" << h.version << ",\"source\":" << jstr(h.source_name)
     << ",\"type\":" << jstr(info.file_type_name) << ",\"type_detail\":" << jstr(h.type_detail)
     << ",\"algorithm\":" << jstr(info.algorithm_id) << ",\"algorithm_name\":" << jstr(info.algorithm_name)
     << ",\"level\":" << int(h.level) << ",\"orig_size\":" << h.orig_size
     << ",\"comp_size\":" << h.comp_size << ",\"chunk_count\":" << h.chunk_count
     << ",\"chunk_size\":" << h.chunk_size << ",\"created\":" << h.created
     << ",\"orig_crc\":" << jstr(kolma::crc32_hex(h.orig_crc))
     << ",\"encrypted\":" << (info.encrypted ? "true" : "false")
     << ",\"authenticated\":" << (info.authenticated ? "true" : "false")
     << ",\"kdf_iterations\":" << h.kdf_iterations
     << ",\"header_consistent\":" << (info.header_consistent ? "true" : "false")
     << ",\"payload_crc_ok\":" << (info.payload_crc_ok ? "true" : "false")
     << ",\"payload_read\":" << info.payload_read << "}";
  return os.str();
}

int finish(const kolma::Result& r, const Options& o, bool human_report) {
  if (o.json) {
    std::ostringstream os;
    os << "{\"event\":\"done\",\"ok\":" << (r.ok ? "true" : "false");
    if (!r.error.empty()) os << ",\"error\":" << jstr(r.error);
    if (r.ok) {
      os << ",\"stats\":" << stats_json(r.stats);
      if (r.analysis.file.exists || !r.analysis.rationale.empty())
        os << ",\"analysis\":" << analysis_json(r.analysis);
      if (!r.benchmark.empty()) os << ",\"benchmark\":" << benchmark_json(r.benchmark);
    }
    os << "}";
    std::cout << os.str() << "\n";
  } else {
    clear_progress_line(o.quiet);
    if (r.ok) {
      // With -o - the payload owns stdout; reports go to stderr.
      if (human_report) {
        std::ostream& os = o.stdout_output ? std::cerr : std::cout;
        os << kolma::format_stats(r.stats);
      }
    } else {
      std::cerr << "kolma: " << r.error << "\n";
    }
  }
  return r.ok ? 0 : 1;
}

// Plain-language report for standard-format (zip/tar/7z) operations. The engine's
// format_stats is full of chunk/CRC/thread detail that means nothing to someone
// who just wants a .zip they can double-click, so keep this short and human.
int report_std(const kolma::Result& r, const Options& o, bool is_extract) {
  if (o.json) {
    std::ostringstream os;
    os << "{\"event\":\"done\",\"ok\":" << (r.ok ? "true" : "false");
    if (!r.error.empty()) os << ",\"error\":" << jstr(r.error);
    if (r.ok) {
      os << ",\"output\":" << jstr(r.stats.output_path)
         << ",\"format\":" << jstr(r.stats.algorithm)
         << ",\"orig_size\":" << r.stats.orig_size
         << ",\"comp_size\":" << r.stats.comp_size;
      if (r.stats.orig_size) os << ",\"ratio\":" << r.stats.ratio;
    }
    os << "}";
    std::cout << os.str() << "\n";
    return r.ok ? 0 : 1;
  }
  clear_progress_line(o.quiet);
  if (!r.ok) {
    std::cerr << "kolma: " << r.error << "\n";
    return 1;
  }
  if (is_extract) {
    std::cout << "Extracted to " << r.stats.output_path << "\n";
  } else {
    std::cout << "Created " << r.stats.output_path << "  (" << r.stats.algorithm << ")\n";
    if (r.stats.orig_size) {
      std::cout << "  " << kolma::human_size(r.stats.orig_size) << " -> "
                << kolma::human_size(r.stats.comp_size);
      double saved = 100.0 * (1.0 - r.stats.ratio);
      if (saved > 0.05) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", saved);
        std::cout << "  (" << buf << "% smaller)";
      }
      std::cout << "\n";
    }
    std::cout << "  Opens in any archive tool or file manager.\n";
  }
  return 0;
}

// A named temporary file that cleans itself up. Used to spool stdin/stdout
// around the engine, which works on real files (pread, parallel chunks).
struct TempFile {
  std::string path;
  bool create() {
#ifdef _WIN32
    char dir[MAX_PATH + 1];
    char name[MAX_PATH + 1];
    if (GetTempPathA(MAX_PATH + 1, dir) == 0) return false;
    if (GetTempFileNameA(dir, "kol", 0, name) == 0) return false;
    path = name;  // GetTempFileNameA atomically creates the (empty) file
    return true;
#else
    char tmpl[] = "/tmp/kolma-pipe-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return false;
    ::close(fd);
    path = tmpl;
    return true;
#endif
  }
  ~TempFile() {
    if (!path.empty()) std::remove(path.c_str());
  }
};

bool spool_stdin_to(const std::string& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  char buf[65536];
  while (std::cin) {
    std::cin.read(buf, sizeof buf);
    std::streamsize n = std::cin.gcount();
    if (n > 0) out.write(buf, n);
  }
  out.flush();
  return out.good();
}

bool stream_file_to_stdout(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char buf[65536];
  while (in) {
    in.read(buf, sizeof buf);
    std::streamsize n = in.gcount();
    if (n > 0) std::cout.write(buf, n);
  }
  std::cout.flush();
  return std::cout.good();
}

bool is_file_command(const std::string& c) {
  return c == "analyze" || c == "inspect" || c == "info" || c == "bench" || c == "benchmark" ||
         c == "compress" || c == "c" || c == "pack" || c == "decompress" || c == "d" ||
         c == "unpack" || c == "extract" || c == "shrink" || c == "split" || c == "join";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(std::cerr);
    return 2;
  }
  std::string command = argv[1];
  if (command == "version" || command == "--version" || command == "-v") {
    std::cout << "kolma " << kVersion << " (algorithms: " << kolma::algorithm_ids_csv() << ")\n";
    return 0;
  }
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage(std::cout);
    return 0;
  }

  Options o = parse_options(argc, argv, 2);
  if (o.error.empty()) {
    for (const std::string& p : o.positionals) {
      if (p == "--help") {
        print_usage(std::cout);
        return 0;
      }
    }
  }
  if (!o.error.empty()) {
    std::cerr << "kolma: " << o.error << "\n";
    return 2;
  }
  if (o.password_flag) {
    std::cerr << "kolma: warning: --password is visible in process listings; "
                 "prefer the KOLMA_PASSWORD environment variable\n";
  }

  auto progress = [&](const kolma::Progress& p) {
    if (o.json)
      emit_progress_json(p);
    else
      emit_progress_human(p, o.quiet);
  };

  if (command == "algos") {
    if (o.json) {
      std::cout << "[";
      const auto& algos = kolma::all_algorithms();
      for (size_t i = 0; i < algos.size(); ++i) {
        const kolma::AlgoParams& p = algos[i]->params();
        if (i) std::cout << ",";
        std::cout << "{\"id\":" << jstr(p.id) << ",\"name\":" << jstr(p.name)
                  << ",\"typical_ratio\":" << p.typical_ratio
                  << ",\"typical_speed_mbps\":" << p.typical_speed_mbps
                  << ",\"memory_bytes\":" << p.memory_bytes << ",\"block_size\":" << p.block_size
                  << ",\"dictionary_size\":" << p.dictionary_size << ",\"notes\":" << jstr(p.notes)
                  << "}";
      }
      std::cout << "]\n";
    } else {
      std::cout << "id             name                        ratio  speed       memory     block      dict\n";
      for (const kolma::Algorithm* a : kolma::all_algorithms()) {
        const kolma::AlgoParams& p = a->params();
        char line[256];
        std::snprintf(line, sizeof(line), "%-14s %-27s %-6.2f %-11s %-10s %-10s %s\n", p.id.c_str(),
                      p.name.c_str(), p.typical_ratio, kolma::human_rate(p.typical_speed_mbps).c_str(),
                      kolma::human_size(p.memory_bytes).c_str(), kolma::human_size(p.block_size).c_str(),
                      p.dictionary_size ? kolma::human_size(p.dictionary_size).c_str() : "-");
        std::cout << line;
      }
      std::cout << "\n";
      for (const kolma::Algorithm* a : kolma::all_algorithms()) {
        std::cout << "  " << a->params().id << ": " << a->params().notes << "\n";
      }
    }
    return 0;
  }

  if (o.positionals.empty()) {
    std::cerr << "kolma: " << command << " needs a file argument\n";
    return 2;
  }
  std::string input = o.positionals.front();

  // Pipes: "-" spools stdin to a temp file (the engine needs a real file for
  // pread and parallel chunks); "-o -" spools the payload back out to stdout.
  TempFile spool_in;
  TempFile spool_out;
  if (input == "-" && is_file_command(command)) {
    if (!spool_in.create() || !spool_stdin_to(spool_in.path)) {
      std::cerr << "kolma: cannot spool stdin\n";
      return 1;
    }
    input = spool_in.path;
    kolma::Mode m;
    if (o.output.empty() && kolma::mode_from_name(command, m)) o.output = "-";
  }
  if (o.output == "-" && is_file_command(command)) {
    if (o.json) {
      std::cerr << "kolma: --json cannot be combined with stdout output (-o -)\n";
      return 2;
    }
    if (!spool_out.create()) {
      std::cerr << "kolma: cannot create a temporary output file\n";
      return 1;
    }
    o.output = spool_out.path;
    o.stdout_output = true;
  }

  kolma::Priority priority = kolma::Priority::Balanced;
  if (!kolma::priority_from_name(o.priority, priority)) {
    std::cerr << "kolma: unknown priority '" << o.priority << "' (maxratio | balanced | maxspeed)\n";
    return 2;
  }

  // list: show what is inside an archive (a bundle or a single file).
  if (command == "list" || command == "ls") {
    if (kolma::std_is_standard(input)) {
      std::vector<kolma::StdEntry> se;
      std::string serr;
      if (!kolma::std_list(input, se, serr)) {
        std::cerr << "kolma: " << serr << "\n";
        return 1;
      }
      if (o.json) {
        std::cout << "{\"event\":\"done\",\"ok\":true,\"entries\":[";
        for (size_t i = 0; i < se.size(); ++i) {
          if (i) std::cout << ",";
          std::cout << "{\"path\":" << jstr(se[i].path) << ",\"size\":" << se[i].size
                    << ",\"is_dir\":" << (se[i].is_dir ? "true" : "false") << "}";
        }
        std::cout << "]}\n";
      } else {
        uint64_t total = 0;
        for (const auto& e : se) {
          std::printf("%12s  %s\n", kolma::human_size(e.size).c_str(), e.path.c_str());
          total += e.size;
        }
        std::printf("\n%zu entries, %s uncompressed\n", se.size(),
                    kolma::human_size(total).c_str());
      }
      return 0;
    }
    std::vector<kolma::BundleEntry> entries;
    std::string err;
    if (!kolma::list_bundle(input, o.password, entries, err)) {
      std::cerr << "kolma: " << err << "\n";
      return 1;
    }
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":true,\"entries\":[";
      for (size_t i = 0; i < entries.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "{\"path\":" << jstr(entries[i].path) << ",\"size\":" << entries[i].size
                  << ",\"is_dir\":" << (entries[i].is_dir ? "true" : "false") << "}";
      }
      std::cout << "]}\n";
    } else {
      uint64_t total = 0;
      for (const auto& e : entries) {
        std::printf("%12s  %s\n", kolma::human_size(e.size).c_str(), e.path.c_str());
        total += e.size;
      }
      std::printf("\n%zu entries, %s uncompressed\n", entries.size(),
                  kolma::human_size(total).c_str());
    }
    return 0;
  }

  // extract: decompress and, for a bundle, untar into a destination directory.
  if (command == "extract") {
    std::string dest = o.positionals.size() > 1
                           ? o.positionals[1]
                           : std::filesystem::path(input).parent_path().string();
    if (dest.empty()) dest = ".";
    // Multipart (split-volume) archive: stream the volumes as one logical file.
    if (kolma::is_multipart(input)) {
      kolma::VolumeSet vs = kolma::detect_volume_set(input);
      kolma::MultipartExtractOptions mo;
      mo.force = o.force;
      mo.in_place = o.in_place;
      if (o.keep_free >= 0) mo.keep_free = uint64_t(o.keep_free);
      std::string err;
      if (!kolma::extract_multipart(vs, dest, mo, err, progress)) {
        std::cerr << "kolma: " << err << "\n";
        return 1;
      }
      if (o.json) {
        std::cout << "{\"event\":\"done\",\"ok\":true,\"output\":" << jstr(dest) << "}\n";
      } else {
        std::printf("Extracted to %s%s\n", dest.c_str(),
                    o.in_place ? "  (in-place: source volumes reclaimed)" : "");
      }
      return 0;
    }
    if (kolma::std_is_standard(input)) {
      std::string serr;
      bool ok = kolma::std_extract(input, dest, o.force, serr, progress);
      kolma::Result r;
      r.ok = ok;
      r.error = serr;
      if (ok) {
        r.stats.output_path = dest;
        r.stats.restoring = true;
        r.stats.verified = true;
        r.stats.algorithm_name = "standard archive";
      }
      return report_std(r, o, true);
    }
    kolma::Result r = kolma::extract_archive(input, dest, o.password, o.force, progress);
    return finish(r, o, true);
  }

  // shrink: perceptually re-encode a video/image smaller; it stays playable.
  if (command == "shrink") {
    if (!kolma::media_available()) {
      std::cerr << "kolma: media shrinking needs ffmpeg installed and on PATH\n";
      return 1;
    }
    kolma::MediaOptions mo;
    mo.output = o.output;
    mo.preset = o.quality.empty() ? "balanced" : o.quality;
    mo.force = o.force;
    kolma::Result r = kolma::shrink_media(input, mo, progress);
    if (!r.ok) {
      std::cerr << "kolma: " << r.error << "\n";
      return 1;
    }
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":true,\"output\":" << jstr(r.stats.output_path)
                << ",\"orig_size\":" << r.stats.orig_size
                << ",\"new_size\":" << r.stats.comp_size
                << ",\"ratio\":" << r.stats.ratio << "}\n";
    } else {
      double saved = (1.0 - r.stats.ratio) * 100.0;
      std::printf("Shrunk %s -> %s  (%.1f%% smaller)\n",
                  kolma::human_size(r.stats.orig_size).c_str(),
                  kolma::human_size(r.stats.comp_size).c_str(), saved);
      std::printf("Saved to %s - still plays in any player\n", r.stats.output_path.c_str());
    }
    return 0;
  }

  // split: cut a file into numbered byte volumes (for sharing or FAT32's 4 GiB cap).
  if (command == "split") {
    if (o.part_size <= 0) {
      std::cerr << "kolma: split needs --part-size (e.g. --part-size 100M)\n";
      return 1;
    }
    std::string err;
    uint32_t n = kolma::split_file(input, uint64_t(o.part_size), err);
    if (n == 0) {
      std::cerr << "kolma: " << err << "\n";
      return 1;
    }
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":true,\"volumes\":" << n << "}\n";
    } else {
      std::printf("Split into %u volumes: %s.001 … %s.%03u\n", n, input.c_str(), input.c_str(), n);
    }
    return 0;
  }

  // join: reassemble numbered volumes back into one file.
  if (command == "join") {
    kolma::VolumeSet vs = kolma::detect_volume_set(input);
    if (vs.paths.empty()) {
      std::cerr << "kolma: not a recognised volume set: " << input << "\n";
      return 1;
    }
    std::string out = o.output.empty() ? vs.base : o.output;
    std::string err;
    if (!kolma::join_multipart(vs, out, o.force, err, progress)) {
      std::cerr << "kolma: " << err << "\n";
      return 1;
    }
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":true,\"output\":" << jstr(out) << "}\n";
    } else {
      std::printf("Joined %zu volumes into %s (%s)\n", vs.paths.size(), out.c_str(),
                  kolma::human_size(vs.total_size).c_str());
    }
    return 0;
  }

  // compress: a whole folder or several inputs become one bundle archive.
  if (command == "compress" || command == "c" || command == "pack") {
    // Standard-format routing: --format zip/tar.gz/... or a standard output
    // extension produces a universally recognised archive that any OS or tool
    // can open. Without it, compress keeps using the native .kolma engine.
    std::string std_fmt = o.format;
    if (std_fmt.empty() && !o.output.empty())
      std_fmt = kolma::std_format_from_extension(o.output);
    if (!std_fmt.empty() && std_fmt != "kolma") {
      if (!kolma::std_formats_available()) {
        std::cerr << "kolma: standard formats need libarchive (not built in)\n";
        return 1;
      }
      std::vector<std::string> pack_inputs = o.positionals;
      for (auto& p : pack_inputs)
        if (p == "-") p = input;  // stdin was spooled to a real file
      std::string out = o.output;
      if (out.empty()) {
        std::filesystem::path first(pack_inputs.front());
        std::string name = first.filename().string();
        if (name.empty()) name = "archive";
        std::string ext = std_fmt == "tar.gz" ? ".tar.gz"
                        : std_fmt == "tar.xz" ? ".tar.xz"
                        : std_fmt == "tar.bz2" ? ".tar.bz2"
                                               : "." + std_fmt;
        std::filesystem::path parent = first.parent_path();
        out = (parent.empty() ? std::filesystem::path(name + ext) : parent / (name + ext)).string();
      }
      kolma::StdPackOptions sopts;
      sopts.format = std_fmt;
      sopts.level = o.level > 0 ? o.level : 6;
      sopts.force = o.force;
      std::string serr;
      bool ok = kolma::std_pack(pack_inputs, out, sopts, serr, progress);
      kolma::Result r;
      r.ok = ok;
      r.error = serr;
      if (ok) {
        std::error_code ec;
        r.stats.output_path = out;
        r.stats.comp_size = std::filesystem::file_size(out, ec);
        r.stats.algorithm = std_fmt;
        r.stats.algorithm_name = std_fmt + " (standard)";
        r.stats.verified = true;
        r.stats.bundle = pack_inputs.size() > 1;
        uint64_t orig = 0;
        for (const auto& p : pack_inputs) {
          std::error_code ec2;
          if (std::filesystem::is_directory(p, ec2)) {
            for (auto it = std::filesystem::recursive_directory_iterator(p, ec2);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec2)) {
              if (it->is_regular_file(ec2)) orig += it->file_size(ec2);
            }
          } else if (std::filesystem::is_regular_file(p, ec2)) {
            orig += std::filesystem::file_size(p, ec2);
          }
        }
        r.stats.orig_size = orig;
        r.stats.ratio = orig ? double(r.stats.comp_size) / double(orig) : 1.0;
      }
      return report_std(r, o, false);
    }
    bool bundle = o.positionals.size() > 1;
    if (!bundle && input != "-") {
      std::error_code ec;
      bundle = std::filesystem::is_directory(input, ec);
    }
    if (bundle) {
      std::string out = o.output;
      if (out.empty()) {
        std::filesystem::path first(o.positionals.front());
        std::string name = first.filename().string();
        if (name.empty()) name = "archive";
        std::filesystem::path parent = first.parent_path();
        out = (parent.empty() ? std::filesystem::path(name + ".kolma")
                              : parent / (name + ".kolma"))
                  .string();
      }
      kolma::BundleOptions opts;
      opts.algorithm = o.algorithm;
      opts.level = o.level;
      opts.priority = priority;
      opts.password = o.password;
      opts.threads = size_t(std::max(0ll, o.threads));
      opts.chunk_size = size_t(std::max(0ll, o.chunk_size));
      opts.memory_limit = size_t(std::max(0ll, o.memory_limit));
      opts.force = o.force;
      kolma::Result r = kolma::compress_bundle(o.positionals, out, opts, progress);
      return finish(r, o, true);
    }
  }

  if (command == "analyze" || command == "inspect") {
    kolma::Analysis a = kolma::analyze_file(input, priority);
    if (!a.file.exists) {
      if (o.json)
        std::cout << "{\"event\":\"done\",\"ok\":false,\"error\":" << jstr(a.rationale) << "}\n";
      else
        std::cerr << "kolma: " << a.rationale << "\n";
      return 1;
    }
    if (o.json)
      std::cout << "{\"event\":\"done\",\"ok\":true,\"analysis\":" << analysis_json(a) << "}\n";
    else
      std::cout << kolma::format_analysis(a);
    return 0;
  }

  if (command == "info") {
    kolma::ArchiveInfo info = kolma::read_archive_info(input, !o.no_check);
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":" << (info.ok ? "true" : "false")
                << ",\"archive\":" << archive_info_json(info) << "}\n";
    } else {
      std::cout << kolma::format_archive_info(info);
    }
    return info.ok ? 0 : 1;
  }

  if (command == "bench" || command == "benchmark") {
    kolma::Result r =
        kolma::benchmark_file(input, o.level, size_t(std::max(0ll, o.threads)),
                              uint64_t(std::max(0ll, o.limit)), progress);
    if (!r.ok) return finish(r, o, false);
    if (o.json) {
      std::cout << "{\"event\":\"done\",\"ok\":true,\"benchmark\":" << benchmark_json(r.benchmark)
                << "}\n";
    } else {
      clear_progress_line(o.quiet);
      std::cout << "benchmark of " << input << " (" << kolma::human_size(r.stats.orig_size)
                << " sampled, level " << r.stats.level << ", " << r.stats.threads << " threads)\n\n"
                << kolma::format_benchmark(r.benchmark);
    }
    return 0;
  }

  kolma::Mode mode;
  if (!kolma::mode_from_name(command, mode)) {
    std::cerr << "kolma: unknown command '" << command << "'\n\n";
    print_usage(std::cerr);
    return 2;
  }

  kolma::Job job;
  job.mode = mode;
  job.priority = priority;
  job.algorithm = o.algorithm;
  job.level = o.level;
  job.input = input;
  job.output = o.output;
  job.password = o.password;
  job.threads = size_t(std::max(0ll, o.threads));
  job.chunk_size = size_t(std::max(0ll, o.chunk_size));
  job.memory_limit = size_t(std::max(0ll, o.memory_limit));
  job.verify = o.verify;
  job.force = o.force || o.stdout_output;  // the spool file is ours to overwrite

  kolma::Result r = kolma::run_job(job, progress);
  if (!r.ok) return finish(r, o, false);

  if (o.stdout_output && !stream_file_to_stdout(spool_out.path)) {
    std::cerr << "kolma: failed to write the payload to stdout\n";
    return 1;
  }
  if (!o.json && !o.quiet && !r.analysis.rationale.empty() && mode == kolma::Mode::Compress) {
    clear_progress_line(o.quiet);
    std::ostream& os = o.stdout_output ? std::cerr : std::cout;
    os << "analysis    : " << r.analysis.rationale << "\n\n";
  }
  return finish(r, o, true);
}
