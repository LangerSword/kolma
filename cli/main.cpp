// kolma -- command line front end.
//
// The TUI and any other client talk to the engine through this binary: with
// --json it emits one JSON object per progress tick on stderr and a final JSON
// object on stdout, which is a stable, parseable contract.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "kolma/algorithm.hpp"
#include "kolma/analyzer.hpp"
#include "kolma/archive.hpp"
#include "kolma/engine.hpp"
#include "kolma/file.hpp"

namespace {

const char* kVersion = "0.1.0";

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
  std::string algorithm = "auto";
  std::string priority = "balanced";
  std::string output;
  std::string password;
  int level = 0;
  long long threads = 0;
  long long chunk_size = 0;
  long long memory_limit = 256ll << 20;
  long long limit = 0;
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
    } else if (arg == "--algo" || arg == "-a") {
      o.algorithm = value_of("--algo");
    } else if (arg == "--priority" || arg == "-p") {
      o.priority = value_of("--priority");
    } else if (arg == "--out" || arg == "-o") {
      o.output = value_of("--out");
    } else if (arg == "--password") {
      o.password = value_of("--password");
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
     << "  kolma compress   <file> [options]\n"
     << "  kolma decompress <archive> [options]\n"
     << "  kolma analyze    <file> [--priority P] [--json]\n"
     << "  kolma bench      <file> [--level N] [--limit SIZE] [--json]\n"
     << "  kolma info       <archive> [--no-check] [--json]\n"
     << "  kolma algos      [--json]\n"
     << "  kolma version\n\n"
     << "options:\n"
     << "  -a, --algo <id>        store | rle | huffman | lz77 | lzw | bwt-mtf-ari | delta-rle | auto\n"
     << "  -l, --level <1-9>      effort; 1 = fastest, 9 = best ratio\n"
     << "  -p, --priority <p>     maxratio | balanced | maxspeed (used when --algo is auto)\n"
     << "  -o, --out <path>       output path\n"
     << "      --password <pw>    encrypt (visible in ps; prefer KOLMA_PASSWORD)\n"
     << "  -t, --threads <n>      worker threads (default: hardware concurrency)\n"
     << "      --chunk-size <s>   bytes per chunk, accepts 64K / 4M suffixes\n"
     << "      --memory-limit <s> working-set ceiling (default 256M)\n"
     << "      --no-verify        skip the checksum check on decompression\n"
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
  if (quiet || !isatty(fileno(stderr))) return;
  double pct = p.chunks_total > 0 ? 100.0 * double(p.chunks_done) / double(p.chunks_total) : 0.0;
  std::fprintf(stderr, "\r  %5.1f%%  %s  ratio %s   ", pct,
               kolma::human_rate(p.throughput_mbps).c_str(), kolma::format_ratio(p.ratio).c_str());
  std::fflush(stderr);
}

void clear_progress_line(bool quiet) {
  if (quiet || !isatty(fileno(stderr))) return;
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
     << ",\"encrypted\":" << (s.encrypted ? "true" : "false")
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
      if (human_report) std::cout << kolma::format_stats(r.stats);
    } else {
      std::cerr << "kolma: " << r.error << "\n";
    }
  }
  return r.ok ? 0 : 1;
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
  const std::string input = o.positionals.front();

  kolma::Priority priority = kolma::Priority::Balanced;
  if (!kolma::priority_from_name(o.priority, priority)) {
    std::cerr << "kolma: unknown priority '" << o.priority << "' (maxratio | balanced | maxspeed)\n";
    return 2;
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

  kolma::Result r = kolma::run_job(job, progress);
  if (!r.ok) return finish(r, o, false);

  if (!o.json && !o.quiet && !r.analysis.rationale.empty() && mode == kolma::Mode::Compress) {
    clear_progress_line(o.quiet);
    std::cout << "analysis    : " << r.analysis.rationale << "\n\n";
  }
  return finish(r, o, true);
}
