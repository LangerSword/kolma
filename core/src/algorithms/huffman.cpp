// huffman -- order-0 canonical Huffman coding.
//
// Block layout:  varint(original length) | 256 code lengths (one byte each) | bitstream
//
// The length table costs 256 bytes per block, which is why the Engine keeps
// blocks large for this algorithm.
#include <algorithm>
#include <queue>
#include <unordered_map>

#include "kolma/algorithm.hpp"
#include "kolma/bitio.hpp"

namespace kolma {
namespace {

constexpr int kMaxCodeLength = 32;

struct Node {
  uint64_t freq = 0;
  int left = -1;
  int right = -1;
};

// Builds code lengths, rescaling the histogram if the tree comes out deeper
// than kMaxCodeLength (only reachable with astronomically skewed frequencies).
std::vector<uint8_t> build_code_lengths(const std::array<uint64_t, 256>& freq_in) {
  std::array<uint64_t, 256> freq = freq_in;
  for (int attempt = 0; attempt < 64; ++attempt) {
    std::vector<Node> nodes;
    nodes.reserve(512);
    for (int s = 0; s < 256; ++s) {
      nodes.push_back(Node{freq[size_t(s)], -1, -1});
    }
    // Min-heap over (frequency, node index); node index breaks ties so the
    // result is deterministic for identical input.
    using Item = std::pair<uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int s = 0; s < 256; ++s) {
      if (freq[size_t(s)] > 0) pq.emplace(freq[size_t(s)], s);
    }
    if (pq.empty()) return std::vector<uint8_t>(256, 0);
    if (pq.size() == 1) {
      std::vector<uint8_t> lengths(256, 0);
      lengths[size_t(pq.top().second)] = 1;
      return lengths;
    }
    while (pq.size() > 1) {
      auto a = pq.top();
      pq.pop();
      auto b = pq.top();
      pq.pop();
      nodes.push_back(Node{a.first + b.first, a.second, b.second});
      pq.emplace(nodes.back().freq, int(nodes.size()) - 1);
    }
    int root = pq.top().second;

    std::vector<uint8_t> lengths(256, 0);
    int max_len = 0;
    // Iterative DFS: depth == code length for a leaf.
    std::vector<std::pair<int, int>> stack{{root, 0}};
    while (!stack.empty()) {
      auto [node, depth] = stack.back();
      stack.pop_back();
      if (node < 256) {
        lengths[size_t(node)] = uint8_t(depth == 0 ? 1 : depth);
        max_len = std::max(max_len, depth == 0 ? 1 : depth);
        continue;
      }
      stack.emplace_back(nodes[size_t(node)].left, depth + 1);
      stack.emplace_back(nodes[size_t(node)].right, depth + 1);
    }
    if (max_len <= kMaxCodeLength) return lengths;
    // Halve and retry: keeps the tree valid, only costs a little optimality.
    for (auto& f : freq) f = (f + 1) / 2;
  }
  throw FormatError("huffman: could not bound code lengths");
}

void assign_canonical_codes(const std::vector<uint8_t>& lengths, std::array<uint32_t, 256>& codes) {
  std::vector<int> order;
  order.reserve(256);
  for (int s = 0; s < 256; ++s) {
    if (lengths[size_t(s)] > 0) order.push_back(s);
  }
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (lengths[size_t(a)] != lengths[size_t(b)]) return lengths[size_t(a)] < lengths[size_t(b)];
    return a < b;
  });
  uint32_t next_code = 0;
  int current_len = 0;
  for (int s : order) {
    next_code <<= (lengths[size_t(s)] - current_len);
    current_len = lengths[size_t(s)];
    codes[size_t(s)] = next_code++;
  }
}

class HuffmanAlgorithm final : public Algorithm {
 public:
  const AlgoParams& params() const override { return p_; }

  void compress_block(View in, Bytes& out, int) const override {
    std::array<uint64_t, 256> freq{};
    for (uint8_t b : in) freq[b]++;

    std::vector<uint8_t> lengths = build_code_lengths(freq);
    std::array<uint32_t, 256> codes{};
    assign_canonical_codes(lengths, codes);

    put_varint(out, in.size());
    out.insert(out.end(), lengths.begin(), lengths.end());

    BitWriter bw(out);
    for (uint8_t b : in) bw.put(codes[b], lengths[b]);
    bw.flush();
  }

  void decompress_block(View in, Bytes& out, uint64_t orig_size) const override {
    size_t pos = 0;
    uint64_t declared = 0;
    if (!get_varint(in, pos, declared)) throw FormatError("huffman: truncated length prefix");
    if (declared != orig_size) throw FormatError("huffman: length prefix disagrees with the archive");
    if (pos + 256 > in.size()) throw FormatError("huffman: truncated code-length table");

    std::vector<uint8_t> lengths(in.begin() + std::ptrdiff_t(pos), in.begin() + std::ptrdiff_t(pos + 256));
    pos += 256;
    std::array<uint32_t, 256> codes{};
    assign_canonical_codes(lengths, codes);

    // Decode table: code -> symbol, bucketed by code length.
    std::array<std::unordered_map<uint32_t, uint8_t>, kMaxCodeLength + 1> table;
    for (int s = 0; s < 256; ++s) {
      if (lengths[size_t(s)] > 0) table[size_t(lengths[size_t(s)])][codes[size_t(s)]] = uint8_t(s);
    }

    out.clear();
    out.reserve(size_t(orig_size));
    BitReader br(View(in.data() + pos, in.size() - pos));
    uint32_t code = 0;
    int len = 0;
    while (out.size() < orig_size) {
      uint32_t bit = 0;
      if (!br.get(1, bit)) throw FormatError("huffman: bitstream ended early");
      code = (code << 1) | bit;
      if (++len > kMaxCodeLength) throw FormatError("huffman: no code longer than 32 bits exists");
      auto& bucket = table[size_t(len)];
      if (auto it = bucket.find(code); it != bucket.end()) {
        out.push_back(it->second);
        code = 0;
        len = 0;
      }
    }
  }

 private:
  AlgoParams p_{
      .id = "huffman",
      .name = "Huffman (order-0)",
      .supported_types = {FileType::Text, FileType::Binary, FileType::Executable, FileType::Image},
      .typical_ratio = 0.62,
      .typical_speed_mbps = 180.0,
      .memory_bytes = 1u << 20,
      .block_size = 1u << 24,
      .dictionary_size = 0,
      .notes = "canonical prefix codes over byte frequencies; ignores context",
  };
};

}  // namespace

const Algorithm* algo_huffman() {
  static const HuffmanAlgorithm instance;
  return &instance;
}

}  // namespace kolma
