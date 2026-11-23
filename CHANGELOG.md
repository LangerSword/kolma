# Changelog

## 1.3.0 — 2026-11-24

### Added

- **RBRC — Run-Boundary Rank Coder**, an eighth compression method. It codes a
  transition mask as the primary signal and resolves changed bytes through a
  block-static, frequency-ranked alphabet, with a raw-literal escape. Built for
  short-run, moderate-alphabet state and segmentation streams; on 256 KiB
  synthetic slices it measured 0.2590 at 57.1 MB/s on 64-symbol runs of 3–8
  bytes and 0.2682 at 57.5 MB/s on 256-symbol runs, beating LZW, RLE and Huffman
  on those shapes. It is intentionally not claimed as a general text or binary
  winner. See `docs/research/rbrc-prior-art.md` for mechanism, prior art and
  measured limits.
- Regenerated TUI/Qt method lists, benchmark counts, fuzz seeds and docs for the
  eighth method.

## 1.2.0

Universal formats, perceptual media shrinking, and split-volume archives with
in-place extraction — kolma stops being a `.kolma`-only tool and becomes a
general-purpose file utility.

### Added

- **Universal archive formats (libarchive).** `compress --format zip` (the new
  default) writes standard `.zip` that Explorer, Finder and `unzip` open
  directly; `tar`, `tar.gz`, `tar.xz`, `tar.bz2` and `7z` too. `extract` and
  `list` open `.zip`, `.7z`, `.tar.gz`, `.tar.xz`, `.tar.bz2` and more — not just
  `.kolma`. The native authenticated `.kolma` container remains for when you
  want encryption and per-chunk verification.
- **Perceptual media shrinking.** `kolma shrink <video|image>` re-encodes through
  an FFmpeg sidecar (`quality` / `balanced` / `smallest` presets) so a huge video
  gets dramatically smaller and stays directly playable — something archiving
  alone can never do for already-compressed media. New **Shrink media** desktop
  view with drag-and-drop and a before/after size card.
- **Split volumes + in-place extraction.** `kolma split <file> --part-size N`
  cuts any file into `.001/.002/…` volumes (FAT32 / sharing); `join` reassembles
  them byte-for-byte. `extract` streams a volume set as one logical file (no
  joined temporary copy), and `--in-place` reclaims volume space as data is
  committed (the HatchZip / Bavariya idea) for forward-streaming tar — with a
  pre-flight free-space check, a `--keep-free` reserve (default 2 GiB) and a
  safety margin before any volume is deleted. zip/7z keep their index at the end
  and need backward seeks, so in-place is safely refused for them.
- **Desktop Tools view.** Inspect any file (type, entropy, compressibility
  verdict, streaming CRC-32 + SHA-256), peek inside any archive without
  extracting, and split a file into volumes with friendly part-size presets
  (1 MB … 4 GB FAT32-safe).
- **Desktop Compress defaults to ZIP** and hides the engine knobs unless you
  choose the native format; plain-language result reports replace engine jargon.
- Smart drag-and-drop routing: a lone video/image opens Shrink, an archive opens
  Extract, folders/many files open Compress.

### Tests

- 100 C++ tests (adds universal-format round-trips and traversal refusal,
  streaming SHA-256 against RFC vectors, media shrink, and 7 multipart tests)
  and 44 Go tests. CI adds zip-interop, media-shrink and split/join/in-place
  smoke steps, and builds the desktop app on every push.

## 1.1.0

A desktop app, folder bundles, and Windows support — the same engine, three
friendly faces, three operating systems.

### Added

- **Desktop app (`kolma-desktop`, Qt6).** Links the engine directly. Drag files
  or folders in to Compress; drop a `.kolma` to open its metadata, integrity and
  contents, then Extract. Benchmark all seven methods, and a Preferences dialog
  that persists. The engine runs on a worker `QThread` so the UI never freezes.
  Packaged as a Linux AppImage, macOS DMG and Windows zip.
- **Bundles.** `compress` accepts a folder or several inputs and packs them into
  one archive (a ustar tar inside the same authenticated container). New `list`
  and `extract` commands; `extract` untars into a destination directory.
- **TUI multi-select.** `space` marks entries, `c` compresses the selection
  (folder/many → bundle, lone file → analyser), `d`/`x` extracts an archive, and
  the info screen lists a bundle's contents.
- **Windows.** Engine, CLI, TUI and desktop app build on Windows (MSVC). Parallel
  reads map to `_pread`; temp files and the tty check are platform-guarded.
- Cross-platform release pipeline: CLI+TUI for Linux (amd64/arm64), macOS
  (arm64/amd64) and Windows (x64); GUI for Linux/macOS/Windows.

### Changed

- `compress_bundle` respects a `force` option instead of always overwriting.
- Archive info's checksum row is labelled `integrity` (was a duplicate `payload`).
- README and landing page rebuilt around the desktop app and bundles.

### Tests

- 82 C++ tests (bundle round-trip, path-traversal refusal, encrypted bundles,
  overwrite guard) and 44 Go tests (multi-select, bundle compress, extract,
  contents, layout guards). GUI vision-verified headlessly.

## 1.0.0

First stable release. The engine, the archive format and the JSON contract
are now covered by a security policy and a four-platform release pipeline.

### Security

- **Authenticated encryption (format v2).** Encrypt-then-MAC: PBKDF2 derives
  separate encryption and MAC keys; an HMAC-SHA256 tag covers the payload and
  the whole header. Verified in constant time before any output is created.
  Wrong passwords and tampering fail cheaply; v1 archives remain readable and
  are reported as unauthenticated.
- **Hardened parser.** Chunk tables bounded before allocation (a crafted
  4-billion-entry table used to request ~68 GiB), chunk sizes capped at
  64 MiB, KDF iterations capped at 10 000 000, headers capped at 64 MiB,
  inconsistent size tables rejected, truncated payloads refused before work.
- **Large headers actually load.** The header read is sized from the header's
  own fields; archives with hundreds of thousands of chunks work.
- **Hostile names neutralised.** Recorded source names never become output
  paths unless control-free and separator-free; control characters are
  stripped from everything displayed (terminal escape injection).
- **Overwrite protection.** Existing outputs (including dangling symlinks)
  are refused unless `--force`; the TUI asks for a second keypress.
- **Fuzzing.** libFuzzer harness over the parser and every decoder under
  ASan/UBSan, in CI and via `make fuzz`. Found and fixed a hostile Huffman
  code-length table (UB shift + out-of-bounds table index); the crash input
  is a permanent regression seed.
- ChaCha20 block-counter wrap now throws instead of carrying into the nonce.

### Added

- **CLI pipes.** `kolma compress -` reads stdin; `-o -` writes the payload to
  stdout with reports on stderr. `--json` + stdout output is refused.
- **Redesigned TUI** on bubbletea/lipgloss/bubbles: panels with embedded
  titles, spinner + progress bars, scrollable viewports, mouse support,
  priority-ordered key bar, word-aware wrapping. Vision-verified at
  120x40 / 80x24 / 60x20 with permanent layout-invariant tests.
- Benchmark screen actually shows results (rows were dropped before).
- `authenticated` and `restoring` fields in the JSON contract.
- Release pipeline: Linux amd64/arm64, macOS amd64/arm64 tarballs + sha256
  checksums; `scripts/install.sh` verifies checksums before installing.
- Demo GIF and screen gallery tooling (`scripts/tui_demo.py`,
  `scripts/tui_gallery.py`, `scripts/render_ansi.py`).
- SECURITY.md, CONTRIBUTING.md.

### Fixed

- Compress results no longer display a bogus "restored 00000000" checksum.
- Archive targets prefill the stripped output name, not `name.kolma`.
- Stale status lines no longer follow the user across screens.
- The pty smoke harness sets a real window size (0x0 ptys rendered
  degenerate frames).

## 0.1.0

Initial public version: seven compression methods behind one interface, the
Analyzer, the parallel chunk engine, CRC-verified archives, ChaCha20 +
PBKDF2 encryption (unauthenticated), the JSON contract and the first Go TUI.
51 C++ tests, 24 Go tests, pty smoke test, sanitizer CI.
