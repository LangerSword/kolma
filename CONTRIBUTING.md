# Contributing to kolma

Thanks for considering a contribution. kolma is a small, carefully tested
codebase; the bar is that every claim is exercised by a test.

## Build and test

```sh
make            # C++ engine + Go TUI (needs CMake 3.20+, Ninja, Go 1.24+)
make check      # 75 C++ tests, 32 Go integration tests, pty-driven TUI smoke
make fuzz       # libFuzzer under ASan/UBSan (needs clang; FUZZ_TIME=seconds)
```

The Go tests drive the real engine binary — they are integration tests, not
mocks. The smoke test drives the TUI through a pseudo-terminal and asserts on
rendered frames. CI additionally builds with `-Werror` and runs the suite
under AddressSanitizer + UndefinedBehaviorSanitizer, plus a 60-second fuzz
campaign on every push.

## Rules of the road

- **Round-trip or it didn't happen.** New algorithms or format changes need
  round-trip tests over the fixture set (empty, one byte, prime-length,
  random, periodic, skewed), a corruption test, and level monotonicity where
  levels apply. Primitives are checked against published vectors, never
  against themselves.
- **Untrusted input is hostile.** Anything read from an archive must be
  bounds-checked before it drives an allocation, an index, or a shift. If you
  touch the parser or a decoder, run `make fuzz` and keep any crash input as
  a corpus seed in `tests/fuzz_corpus/`.
- **Honest numbers.** Ratios are compressed/original; "percent saved" is
  `(1 - ratio) * 100`. Benchmark claims must state the machine, compiler,
  level, and whether the figure is per-method or engine throughput.
- **One implementation.** The TUI is a client of the engine's JSON contract
  (`--json`). Never reimplement an algorithm in Go; extend the contract if
  the UI needs new data, and keep it backward compatible.
- **Style.** C++20, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` clean
  (CI enforces `-Werror`). Go code passes `go vet`. Match the surrounding
  code; no drive-by refactors.
- **TUI changes need eyes.** Run `python3 scripts/tui_gallery.py` and look at
  the rendered frames at 120x40, 80x24 and 60x20 — overflow and misalignment
  are regressions, and `TestEveryScreenFitsTheTerminal` guards them.

## Commits and pull requests

- Small, focused commits; the subject line says what changed and why.
- Include the test output (or the fuzz stat line) in the PR description.
- If a change alters the archive format or the JSON contract, say so
  explicitly and bump the format version / keep old readers working.

## Licensing

MIT. By contributing, you agree your contributions are licensed under it.
