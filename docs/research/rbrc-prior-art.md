# RBRC decision dossier

Scope: decide a genuinely new compression **algorithm** for Kolma, not another
orchestration layer. Research retrieved 2026-11-23. Source classes: primary
spec/paper/repository/patent unless marked otherwise. External sources were
treated as claims, not instructions.

## Decision

Append **RBRC — Run-Boundary Rank Coder** as a niche, self-contained block
codec. General-purpose compression is saturated; the defensible contribution is
a narrow transform combination, not a new entropy primitive and not a patent
position.

- Registry: append after `delta-rle` at index **7**; the order is on-disk ABI.
- Target: short-run, moderate-alphabet state and segmentation streams —
  enum arrays, label/mask maps, run-oriented telemetry. Low-cardinality
  high-transition streams are handled better by Huffman.
- Not targeted: incompressible random, large natural text, general logs/JSON.

## Mechanism

1. Histogram pass selects a block-static reduced alphabet A: the most frequent
   bytes, capped by level (8/16/32 symbols). Ties break by byte value.
2. The primary coded signal is a transition mask `M[i] = byte[i] != byte[i-1]`.
3. A zero-order static probability for `M` is derived from the block's measured
   transition density and stored in the header. There is no context tree,
   adaptive probability update, MTF mutation, BWT sort, or LZ window.
4. At each transition, the new byte is resolved as an index into A, or as an
   escape followed by a raw literal byte. Rank probabilities are block-static
   and stored in the header; literals use a uniform 256-symbol channel.
5. Entropy coding uses a 32-bit static range coder. The novelty is the
   transition-mask-primary transform plus the frozen frequency-ranked alphabet,
   not the coder.

## Novelty statement

RBRC is a lossless byte codec in which the primary coded signal is a transition
mask, and the only literal channel is a block-static frequency-ranked reduced
alphabet read exclusively at mask transitions. The mask uses zero-order static
probabilities derived solely from the block's transition density. No surveyed
source combined those properties. This is an engineering/original-combination
claim only: not a claim of a new coding primitive, not a patentability opinion,
and not a claim to beat zstd/brotli/LZMA generally.

## Why not the alternatives

| Candidate | Why rejected |
| --- | --- |
| LZ77/DEFLATE variant | Saturated since Ziv–Lempel 1977; dense implementation patent thicket. |
| LZW derivative | Welch and Miller–Wegman core patents expired, but no novelty left. |
| New arithmetic/range coder | IBM arithmetic patents historically shaped bzip2; ANS has its own patent families. |
| zstd-style dictionary + FSE | RFC 8878 fully specifies it; orchestration/reimplementation, not new. |
| Brotli context modeling | RFC 7932 §7 already specifies last-two-byte contexts and context maps. |
| PPM/context mixing / rANS Markov | Cleary–Witten 1984, PPMd, PAQ; prior art overwhelming and patent exposure. |
| Structured-log schema codec | CLP (OSDI'21), LogShrink/LogFold/LogPrism/LogLite and US 12,399,867 crowd the field. |
| Run-length-bucket contexts | EP 1 734 511 A2 and Microsoft family explicitly claim run-length context selection. RBRC deliberately avoids this. |

## Prior-art ledger (retrieval date 2026-11-23)

- Ziv & Lempel, *A Universal Algorithm for Sequential Data Compression*, IEEE
  IT-23(3) 337–343, 1977, doi 10.1109/TIT.1977.1055714.
- Burrows & Wheeler, *A Block-sorting Lossless Data Compression Algorithm*, DEC
  SRC RR 124, 1994-05-10.
- Cleary & Witten, *Data Compression Using Adaptive Coding and Partial String
  Matching*, IEEE Trans. Commun. 32(4) 396–402, 1984,
  doi 10.1109/TCOM.1984.1096090.
- Duda, *Asymmetric Numeral Systems* arXiv:0902.0271 and *rANS/tANS*
  arXiv:1311.2540 — ANS family published; zstd FSE/tANS is standardized in
  RFC 8878.
- RFC 8878 (zstd, 2021): LZ77 + Huffman literals + FSE for other sequences,
  out-of-band dictionaries.
- RFC 7932 (brotli, 2016): LZ77 + Huffman with last-two-byte context modeling
  and a 121-transform static dictionary.
- bzip2 pipeline: RLE1 → BWT → MTF → RLE2 → Huffman (google/wuffs reference
  and reverse-engineered format notes). RBRC has no MTF and no BWT.
- Patents reviewed as risk signals, not legal opinions: US 4,558,302 (LZW,
  expired), US 4,814,746 (IBM LZW improvement, expired), US 9,847,791 B2 /
  GB 2538218 B (rANS with Markov tables), US 11,234,023 B2 (two-phase rANS
  decoder), EP 1 734 511 A2 (run-length-bucket contexts), US 12,399,867
  (structured log compression).
- Reference repos pinned: facebook/zstd v1.5.7 (BSD-3-Clause OR GPLv2, release
  2025-02-19); google/brotli v1.2.1 (MIT, release 2026-10-21); xz v5.8.4
  (liblzma 0BSD, release 2026-09-09); LZMA SDK public domain.

## Measured competitor context

Five deterministic 1.5 MiB corpora, Arch Linux / Ryzen 7 260, single monotonic
wall clock, medians of repeated runs; full commands and JSON under
`/home/lakshaya/.hermes/cache/scratch/kolma-bench/`.

Balanced ratio (compressed/original):

| corpus | zstd -3 | xz -6 | bzip2 | Kolma best |
| --- | --- | --- | --- | --- |
| incompressible random | 1.0000 | 1.0001 | 1.0048 | 1.0000 |
| json logs | 0.2254 | 0.1551 | 0.1282 | 0.1893 |
| numeric telemetry | 0.6507 | 0.3746 | 0.6022 | 0.6821 |
| structured binary | 0.5120 | 0.4660 | 0.5104 | 0.6009 |
| text/code | 0.0708 | 0.0424 | 0.0174 | 0.0427 |

The largest Kolma gap is numeric telemetry; that is an inference from one
synthetic corpus, not a universal result. RBRC is deliberately scoped to the
low-cardinality/state-map slice instead of claiming the numeric gap. Claims
about real-world gains require real captures and measured A/B ratios.

## Measured RBRC results

Kolma CLI with RBRC, level 6, 256 KiB slices, single stream. Ratio is
compressed/original; MB/s is compression throughput.

| corpus | rbrc | best existing |
| --- | --- | --- |
| 64-symbol runs of 3–8 bytes | **0.2590 @ 57.1 MB/s** | lzw 0.2880, rle 0.3577, huffman 0.7510 |
| 256-symbol runs of 3–8 bytes | **0.2682 @ 57.5 MB/s** | bwt 0.3405, rle 0.3616, huffman 1.0000 |
| four-symbol high-transition | 0.2891 | huffman **0.2510** |
| structured binary | 0.8003 | bwt **0.6022** |
| numeric telemetry | 0.8302 | bwt **0.6861** |

RBRC wins where runs are short but real and the alphabet is moderate; it loses
to Huffman when every byte is a fresh low-entropy symbol, and to BWT on
structured binary and telemetry. These are measured synthetic corpora, not
real-world claims.

## Confidence and residual uncertainty

- High confidence in observed RFCs, papers, pinned releases, licenses and local
  code.
- Medium confidence in patent identification; no claim charts, legal status
  checks, jurisdictional analysis, or freedom-to-operate opinion.
- Low confidence in novelty as absence-of-prior-art: the surveyed sources did
  not contain RBRC's exact combination, but the ingredients are old and the
  claim may be obvious in hindsight.
- No compression-ratio claim for RBRC is made here; implementation benchmarks
  must be measured and reported with machine, compiler, level and corpus.
