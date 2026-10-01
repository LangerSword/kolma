#include "kolma/analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace kolma {
namespace {

double elapsed_ms_since(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Which methods are worth trialling for a given priority. Max speed only tries
// the cheap ones; max ratio is allowed to pay for the slow transform.
std::vector<std::string> candidates_for(Priority priority) {
  switch (priority) {
    case Priority::MaxSpeed:
      return {"rle", "huffman", "lz77"};
    case Priority::MaxRatio:
      return {"lz77", "lzw", "bwt-mtf-ari", "delta-rle", "huffman"};
    case Priority::Balanced:
    default:
      return {"lz77", "lzw", "huffman", "delta-rle"};
  }
}

}  // namespace

const char* priority_name(Priority priority) {
  switch (priority) {
    case Priority::MaxRatio: return "MaxRatio";
    case Priority::MaxSpeed: return "MaxSpeed";
    case Priority::Balanced: default: return "Balanced";
  }
}

bool priority_from_name(std::string_view name, Priority& out) {
  std::string n;
  n.reserve(name.size());
  for (char c : name) n.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  if (n == "maxratio" || n == "ratio" || n == "max-ratio" || n == "max_ratio") {
    out = Priority::MaxRatio;
    return true;
  }
  if (n == "maxspeed" || n == "speed" || n == "max-speed" || n == "max_speed") {
    out = Priority::MaxSpeed;
    return true;
  }
  if (n == "balanced" || n == "balance" || n == "default") {
    out = Priority::Balanced;
    return true;
  }
  return false;
}

int level_for_priority(Priority priority) {
  switch (priority) {
    case Priority::MaxSpeed: return 1;
    case Priority::MaxRatio: return 9;
    case Priority::Balanced: default: return 6;
  }
}

Analysis analyze_file(const std::string& path, Priority priority, uint64_t sample_size) {
  Analysis a;
  a.file = inspect_file(path, size_t(std::min<uint64_t>(sample_size, 1u << 22)));
  a.sample_size = std::min<uint64_t>(a.file.size, sample_size);
  a.entropy = a.file.entropy;
  a.recommended_level = level_for_priority(priority);

  if (!a.file.exists) {
    a.rationale = a.file.error.empty() ? "file cannot be read" : a.file.error;
    a.store_raw = true;
    return a;
  }

  // The veto: formats that are already entropy coded.
  if (is_pretty_compressed(a.file.type, a.file.type_detail)) {
    a.store_raw = true;
    a.recommended_algo = "store";
    std::ostringstream os;
    os << a.file.type_detail << " is already entropy coded, so a second pass can only add "
       << "framing overhead; store it raw. Measured entropy " << a.entropy << " bits/byte.";
    a.rationale = os.str();
    return a;
  }

  Bytes sample = read_file_prefix(path, size_t(sample_size));
  if (sample.empty()) {
    a.store_raw = true;
    a.recommended_algo = "store";
    a.rationale = "empty input; nothing to compress";
    a.compressibility = 0.0;
    return a;
  }

  for (const std::string& id : candidates_for(priority)) {
    const Algorithm* algo = find_algorithm(id);
    if (!algo) continue;
    AlgoTrial trial;
    trial.algo_id = id;
    trial.algo_name = algo->params().name;
    trial.sample_bytes = sample.size();
    Bytes compressed;
    auto start = std::chrono::steady_clock::now();
    try {
      algo->compress_block(View(sample), compressed, a.recommended_level);
    } catch (const std::exception&) {
      continue;  // a method that cannot handle the sample is simply not a candidate
    }
    trial.elapsed_ms = elapsed_ms_since(start);
    trial.compressed_bytes = compressed.size();
    trial.ratio = double(compressed.size()) / double(sample.size());
    a.trials.push_back(trial);
  }

  if (a.trials.empty()) {
    a.store_raw = true;
    a.recommended_algo = "store";
    a.rationale = "no candidate method produced output; storing raw";
    return a;
  }

  const AlgoTrial* best_ratio = &a.trials.front();
  const AlgoTrial* best_speed = &a.trials.front();
  for (const AlgoTrial& t : a.trials) {
    if (t.ratio < best_ratio->ratio) best_ratio = &t;
    if (t.ratio <= best_ratio->ratio * 1.02 &&
        (t.elapsed_ms < best_speed->elapsed_ms || best_speed->ratio > best_ratio->ratio)) {
      if (t.elapsed_ms < best_speed->elapsed_ms) best_speed = &t;
    }
  }

  const AlgoTrial* chosen = best_ratio;
  if (priority == Priority::MaxSpeed) {
    // Within 15% of the best ratio, prefer the faster method.
    for (const AlgoTrial& t : a.trials) {
      if (t.ratio <= best_ratio->ratio * 1.15 && t.elapsed_ms < chosen->elapsed_ms) chosen = &t;
    }
  } else if (priority == Priority::Balanced) {
    // Within 5% of the best ratio, prefer the faster method.
    for (const AlgoTrial& t : a.trials) {
      if (t.ratio <= best_ratio->ratio * 1.05 && t.elapsed_ms < chosen->elapsed_ms) chosen = &t;
    }
  }

  a.compressibility = std::clamp(1.0 - best_ratio->ratio, 0.0, 1.0);
  a.recommended_algo = chosen->algo_id;

  if (best_ratio->ratio >= 0.98) {
    a.store_raw = true;
    a.recommended_algo = "store";
    std::ostringstream os;
    os << "best method reached " << best_ratio->ratio
       << " on the sample (no real gain), so this file is flagged incompressible and stored raw.";
    a.rationale = os.str();
    return a;
  }

  std::ostringstream os;
  os << a.file.type_detail << ", entropy " << a.entropy << " bits/byte. Over a "
     << a.sample_size << "-byte sample: ";
  for (size_t i = 0; i < a.trials.size(); ++i) {
    if (i) os << ", ";
    os << a.trials[i].algo_id << " -> " << a.trials[i].ratio;
  }
  os << ". Best ratio: " << best_ratio->algo_id << " (" << best_ratio->ratio << "). Chose "
     << a.recommended_algo << " at level " << a.recommended_level << " for "
     << priority_name(priority) << ".";
  a.rationale = os.str();
  return a;
}

}  // namespace kolma
