// kolma -- multipart (split-volume) archives, and space-saving in-place extraction.
//
// A split archive is one logical archive cut into fixed-size byte volumes:
//   foo.zip.001, foo.zip.002, ...   (the byte-split sets 7-Zip writes)
// kolma can create these (split), reassemble them (join), and extract them
// WITHOUT first writing a joined temporary copy — the volumes are streamed as
// one logical file straight into the extractor.
//
// In-place extraction (the HatchZip/Bavariya idea): as files are extracted and
// committed to disk, the volume bytes they came from are reclaimed, so you do
// not need archive-size + extracted-size free at once. This is destructive to
// the source volumes, so it is opt-in, restricted to forward-streaming tar
// archives (where reads never seek back), deletes a volume only after its data
// is committed plus a safety margin, and refuses to start without enough free
// space. zip/7z keep their index at the end and need backward seeks, so in-place
// is safely refused for them — they still extract normally from volumes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kolma/engine.hpp"  // ProgressFn

namespace kolma {

// Cut input into part_size-byte volumes named input.001, input.002, ...
// Returns the number of volumes written, or 0 and fills error. Never modifies
// the input. Refuses to overwrite existing volumes.
uint32_t split_file(const std::string& input, uint64_t part_size, std::string& error);

// An ordered set of volumes representing one logical byte stream.
struct VolumeSet {
  std::vector<std::string> paths;  // .001 .. .NNN, in order
  std::vector<uint64_t> sizes;     // size of each volume
  uint64_t total_size = 0;
  std::string base;                // logical archive path (the name minus .NNN)
};

// If path is (or belongs to) a foo.NNN volume set, return the full ordered set.
// Returns an empty set when path is not a recognised volume. Accepts any member
// (foo.003) and resolves the whole series starting at foo.001.
VolumeSet detect_volume_set(const std::string& path);
bool is_multipart(const std::string& path);

// Reassemble volumes into output. Safe: never deletes or alters the volumes.
bool join_multipart(const VolumeSet& vs, const std::string& output, bool force,
                    std::string& error, const ProgressFn& on_progress = nullptr);

struct MultipartExtractOptions {
  bool force = false;       // overwrite existing extracted files
  bool in_place = false;    // reclaim volume space as data is committed (tar only)
  uint64_t keep_free = 2ull << 30;  // never let free space drop below this (2 GiB)
};

// Extract a split archive by streaming its volumes as one logical file (no
// joined temp copy). With in_place, volumes are deleted as their data is
// committed — only for forward-streaming tar archives; refused otherwise.
bool extract_multipart(const VolumeSet& vs, const std::string& dest_dir,
                       const MultipartExtractOptions& opts, std::string& error,
                       const ProgressFn& on_progress = nullptr);

// Verify a split archive by streaming its volumes as one logical file and
// reading every entry (libarchive checks CRCs). No joined temp copy, no files
// written, volumes untouched. The WinRAR-style "test" for split archives.
bool test_multipart(const VolumeSet& vs, uint64_t& entries, std::string& error);

}  // namespace kolma
