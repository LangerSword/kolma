# Design notes

This is the design `kolma` implements: six entities, their properties, their
actions, and the messages that pass between them. It is the source of truth for
what exists in the code and for what deliberately does not.

## Entities and properties

**User.** The person running the program. A user has a Mode {Compress,
Decompress, Benchmark}, a Priority {Max Ratio, Balanced, Max Speed}, a chosen
Algorithm (or Auto), a Compression Level (1 to 9), an optional Password for
encryption, and an Output Location.

*In the code:* `kolma::Job` (`core/include/kolma/engine.hpp`) carries exactly
these fields. Mode and Priority are the enums `Mode` and `Priority`; `algorithm`
is the string `"auto"` or a registry id; `level` of 0 means "derive it".

**File.** The input being compressed or the output being restored. A file has a
Name, Path, Size in bytes, Type {Text, Image, Audio, Video, Executable, Archive,
Binary, ...}, an Entropy Estimate, a Checksum, and a Modified Timestamp.

*In the code:* `kolma::FileInfo` and `FileType` (`core/include/kolma/file.hpp`).
Type detection prefers magic bytes over the extension and falls back to a
printability heuristic, because extensions lie. Entropy is Shannon entropy over
a sample, in bits per byte.

**Analyzer.** The component that inspects a file before compression. It has a
Sample Size, a Detected Type, a Compressibility Score, and a Recommended
Algorithm. Its job is to avoid wasting time on data that will not shrink, such
as already-compressed JPEG or MP4 files.

*In the code:* `kolma::Analysis` (`core/include/kolma/analyzer.hpp`). The
recommendation is not a heuristic table: the candidate methods are actually run
over the sample and the measured ratios and times are returned in `trials`, so
the decision can be inspected. `store_raw` is the veto.

**Algorithm.** A pluggable compression method. Each has a Name, a Supported File
Types list, a Typical Ratio, a Typical Speed, Memory Usage, a Block Size, and a
Dictionary Size.

*In the code:* the abstract `kolma::Algorithm` and its `AlgoParams`
(`core/include/kolma/algorithm.hpp`), with seven implementations in
`core/src/algorithms/`. The registry order is frozen because the index is what
goes into the archive header.

**Engine.** The core worker that runs an algorithm on a file. It has a Thread
Count, a Buffer Size, a Chunk Size for streaming large files, a Memory Limit, and
a Progress counter. It tracks Time Taken, Throughput (MB/s), and Compression
Ratio once finished.

*In the code:* `kolma::run_job` and the private `OrderedPipeline`
(`core/src/engine.cpp`). The pipeline is the memory limit made concrete: at most
`threads + 4` chunks exist at once, and results are written in order.

**Archive.** The compressed output plus its metadata record. It has an Original
Size, Compressed Size, Ratio, Algorithm Used, Level Used, Original File Type,
Checksum, a Header/Format Version, an Encrypted flag, and a Created Timestamp.

*In the code:* `kolma::ArchiveHeader` and `kolma::ChunkRecord`
(`core/include/kolma/archive.hpp`). The header is written twice: a placeholder
first so the payload can stream to disk, then rewritten in place once the real
sizes and checksums are known. Its length never depends on those values.

## Actions for each entity

| Entity | Actions | Where |
| --- | --- | --- |
| User | select input, choose algorithm or auto, set level and priority, start compression, start decompression, run benchmark, set password, view statistics | `cli/main.cpp`, `tui/` |
| File | show size, show type, compute checksum, be read in chunks, be written out | `core/src/file.cpp`, `FileDescriptor` in `core/src/engine.cpp` |
| Analyzer | detect type, sample data, estimate entropy, score compressibility, recommend algorithm and level, flag incompressible files | `core/src/analyzer.cpp` |
| Algorithm | compress a block, decompress a block, report parameters, build a dictionary or model, report memory needs | `core/src/algorithms/*.cpp` |
| Engine | split into chunks, run the algorithm in parallel, merge chunks, verify the checksum, report progress, report ratio and speed, handle errors and corrupted data | `core/src/engine.cpp` |
| Archive | show metadata, validate header and checksum, report ratio, be stored and read back, be encrypted or decrypted, be listed or extracted | `core/src/archive.cpp` |

## Communication between entities

- **User → Analyzer**: the user submits a file and a priority; the analyzer
  returns the detected type and a recommended algorithm.
- **User → Engine**: the user starts a compress or decompress job with chosen
  settings; the engine returns progress and final statistics.
- **Analyzer → Algorithm**: the analyzer picks an algorithm and level based on
  file type and entropy.
- **Engine → File**: the engine reads the file in chunks and writes restored data
  back on decompression.
- **Engine → Algorithm**: the engine hands each chunk to the algorithm and
  receives compressed or decompressed bytes.
- **Engine → Archive**: the engine writes the compressed data and metadata, and
  later reads the header and checksum to decompress and verify.
- **Archive → User**: the archive reports ratio, sizes and integrity status.

## What was deliberately left out

- **Compression of multiple files into one archive.** `kolma` compresses one
  input per archive. The chunk table would support concatenation, but a
  directory walker is a different entity and the design does not have one.
- **Authentication of encrypted archives.** The design asks for an `Encrypted`
  flag and the ability to encrypt and decrypt, and that is what is implemented.
  A MAC is a separate property that the design does not list, and pretending
  otherwise would be dishonest. See the README's limitations.
- **Automatic decompression by file extension alone.** The archive says which
  algorithm it used; the engine trusts the header and the checksum, not the
  filename.