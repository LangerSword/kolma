#!/usr/bin/env python3
"""Capture every kolma TUI screen at several terminal sizes, as PNGs.

Drives the real binary inside a private tmux server, walks the whole workflow
(browse -> inspect -> edit -> compress -> archive info -> error state ->
benchmark -> help), captures each frame with `capture-pane -e`, and renders it
with render_ansi.py. The PNGs are what a human (or a vision model) reviews to
judge whether the interface is broken or beautiful.

usage: tui_gallery.py [--tui PATH] [--engine PATH] [--out DIR]
                      [--sizes 120x40,80x24,60x20]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

SOCKET = "kolma-gallery"
SESSION = "gal"
HERE = os.path.dirname(os.path.abspath(__file__))


def tmux(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["tmux", "-L", SOCKET, *args], check=check,
                          capture_output=True, text=True)


def capture(cols: int, rows: int, out_base: str) -> str:
    ansi_path = out_base + ".ansi"
    png_path = out_base + ".png"
    frame = tmux("capture-pane", "-t", SESSION, "-e", "-p").stdout
    with open(ansi_path, "w", encoding="utf-8") as fh:
        fh.write(frame)
    subprocess.run([sys.executable, os.path.join(HERE, "render_ansi.py"),
                    ansi_path, png_path, "--cols", str(cols), "--rows", str(rows)],
                   check=True, capture_output=True)
    return png_path


def send(keys: str, settle: float = 0.5) -> None:
    tmux("send-keys", "-t", SESSION, keys)
    time.sleep(settle)


def special(key: str, settle: float = 0.5) -> None:
    tmux("send-keys", "-t", SESSION, key)
    time.sleep(settle)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tui", default="tui/kolma-tui")
    ap.add_argument("--engine", default="build/kolma")
    ap.add_argument("--out", default="tui-gallery")
    ap.add_argument("--sizes", default="120x40,80x24,60x20")
    args = ap.parse_args()

    tui = os.path.abspath(args.tui)
    engine = os.path.abspath(args.engine)
    for path in (tui, engine):
        if not os.path.exists(path):
            print(f"FAIL: {path} does not exist (build it first)", file=sys.stderr)
            return 1
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    # Fixtures: a compressible text file, a directory, a hidden file, and an
    # encrypted archive so the password-error state can be captured too.
    fixture = os.path.join(out, "fixture")
    shutil.rmtree(fixture, ignore_errors=True)
    os.makedirs(os.path.join(fixture, "subdir"))
    with open(os.path.join(fixture, "sample.txt"), "w") as fh:
        fh.write("the quick brown fox jumps over the lazy dog. " * 400)
    with open(os.path.join(fixture, ".hidden"), "w") as fh:
        fh.write("hidden\n")
    env = dict(os.environ, KOLMA_PASSWORD="gallery-password")
    subprocess.run([engine, "compress", os.path.join(fixture, "sample.txt"),
                    "--out", os.path.join(fixture, "secret.kolma"), "-q"],
                   check=True, env=env, capture_output=True)

    tmux("kill-server", check=False)
    tmux("set", "-as", "terminal-features", ",xterm-256color:RGB", check=False)

    manifest = []
    for size in args.sizes.split(","):
        cols, rows = (int(v) for v in size.split("x"))
        size_dir = os.path.join(out, size)
        os.makedirs(size_dir, exist_ok=True)
        env_cmd = (f"cd {fixture} && TERM=xterm-256color COLORTERM=truecolor "
                   f"{tui} --bin {engine}")
        tmux("new-session", "-d", "-s", SESSION, "-x", str(cols), "-y", str(rows),
             "bash", "-c", env_cmd)
        time.sleep(1.2)

        def shot(name: str) -> None:
            path = capture(cols, rows, os.path.join(size_dir, name))
            manifest.append(path)
            print(f"  captured {size}/{name}")

        try:
            shot("01-browse")
            special("Down", 0.3)
            shot("02-browse-cursor")
            send("/", 0.4)                      # open the fuzzy filter
            shot("03-filter-input")
            send("sam", 0.6)                    # type: matches sample.txt
            shot("04-filter-typing")
            special("Enter", 0.4)               # accept the filter
            shot("05-filter-applied")
            special("Escape", 0.4)              # clear it
            special("Enter", 0.4)               # open the empty subdir
            shot("06-empty-dir")
            special("BSpace", 0.4)
            special("Down", 0.3)
            special("Enter", 1.6)               # select notes.txt, analyse it
            shot("07-inspect")
            send("o", 0.4)                      # edit the output path
            shot("08-inspect-editing")
            special("Escape", 0.3)
            send("c", 1.8)                      # compress -> result screen
            shot("09-run-result")
            special("Escape", 0.3)
            special("Escape", 0.3)              # back to browse
            special("G", 0.3)                   # last entry: secret.kolma
            special("Enter", 1.4)
            shot("10-inspect-archive")
            send("d", 1.2)                      # decompress without password -> error
            shot("11-run-error")
            special("Escape", 0.3)
            send("i", 1.4)                      # info on the ENCRYPTED archive
            shot("12-info-encrypted")
            special("Escape", 0.3)
            special("Escape", 0.3)
            special("Up", 0.3)                  # notes.txt.kolma
            special("Enter", 1.2)
            send("i", 1.4)                      # archive info screen
            shot("13-info")
            special("Escape", 0.3)
            send("b", 3.5)                      # benchmark all eight methods
            shot("14-bench")
            send("Down", 0.4)                   # move the table selection
            shot("15-bench-selected")
            special("Escape", 0.3)
            send("?", 0.5)
            shot("16-help")
        finally:
            tmux("kill-session", "-t", SESSION, check=False)

    tmux("kill-server", check=False)
    print(f"\n{len(manifest)} frames rendered under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
