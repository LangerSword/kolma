# 30-second Kolma kinetic film — RBRC cut (fresh sheet)

All copy below is locked. All numbers and artifacts are read from the repo at build time;
nothing is invented. The film is 30.000 s, 1920x1080, seven beats.

## Sources of truth (read-only)

- `README.md` lines 45–59: real `kolma compress /usr/bin/cmake` readout —
  11.37 MiB original, 6.11 MiB compressed, ratio 0.5376 (46.2% saved),
  39.30 MB/s, 6 x 2.00 MiB chunks, CRC32 `16bc7172`.
- `README.md` lines 541–548: whole-ELF method table, same numbers.
- `README.md` line 556 + `docs/research/rbrc-prior-art.md` lines 109–115:
  measured RBRC on 256 KiB slices, 64-symbol runs of 3–8 bytes, level 6:
  `rbrc 0.2590 @ 57.1 MB/s`, `lzw 0.2880`, `rle 0.3577`, `huffman 0.7510`.
- `core/src/algorithm.cpp` lines 22–26: registry is exactly eight methods in
  order: `store`, `rle`, `huffman`, `lz77`, `lzw`, `bwt-mtf-ari`, `delta-rle`, `rbrc`.
- `README.md` lines 266–275: real commands `kolma compress`, `kolma bench`, `kolma info`.
- Real screenshots: `docs/screenshots/tui-bench.png` (analyser/bench evidence),
  `docs/screenshots/gui-compress.png` (desktop), `docs/screenshots/tui-inspect.png` (TUI).
- Fonts: `docs/assets/fonts/SpaceGrotesk.ttf`, `docs/assets/fonts/JetBrainsMono.ttf`.

## Beats

| # | Window | Exact on-screen copy | Visual / asset |
|---|---|---|---|
| 1 | 0.0–3.4 | `Compression is a black box.` | Near-black field, huge white display type, one green accent word `black`, rule sweep. |
| 2 | 3.4–7.0 | `Eight methods. No evidence.` | Eight real method-name chips stagger in: store, rle, huffman, lz77, lzw, bwt-mtf-ari, delta-rle, rbrc. |
| 3 | 7.0–11.2 | `Kolma measures first.` | Real `tui-bench.png` flies in with tilt + push-in; no recreated table. |
| 4 | 11.2–16.6 | `Evidence, not vibes.` | Real CLI proof: `11.37 MiB → 6.11 MiB`, giant `0.5376`, `39.30 MB/s`, `CRC32 16bc7172`, `6 x 2.00 MiB`. |
| 5 | 16.6–21.2 | `New: RBRC.` + `0.2590` + `57.1 MB/s` | Giant measured number, throughput, competitor rows `lzw 0.2880` / `rle 0.3577` / `huffman 0.7510`; label `256 KiB slices · 64 symbols · runs of 3–8 bytes · level 6`. |
| 6 | 21.2–25.6 | `One engine. Three surfaces.` | Three panels: real TUI screenshot, real desktop screenshot, mono CLI panel (`kolma compress`, `kolma bench`, `kolma info`). |
| 7 | 25.6–30.0 | `kolma` + `langersword.github.io/kolma` | Huge lockup, mono URL, small real tagline `COMPRESS / INSPECT / VERIFY`. |

## Grammar rules held

One accent (green `#55e6a5`), white display type, mono labels, near-black `#08090b`.
Every value is a pure function of `t` in `render(t)`; no CSS transitions, no timers.
Preview stills at t = 1, 5, 9, 13, 18, 23, 28 must OCR the expected phrase before recording.
Audio is an in-repo deterministic synth bed only — no voice, no external mix.
