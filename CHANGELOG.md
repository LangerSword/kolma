# Changelog

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
