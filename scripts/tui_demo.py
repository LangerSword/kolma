#!/usr/bin/env python3
"""Record a short demo of the kolma TUI as an animated GIF.

Drives the real binary inside tmux through a scripted workflow, captures a
frame every 0.5 s, renders each with render_ansi.py, and assembles them with
ffmpeg (palette-optimised). The result is docs/demo.gif for the README.

usage: tui_demo.py [--tui PATH] [--engine PATH] [--out docs/demo.gif]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

SOCKET = "kolma-demo"
SESSION = "demo"
HERE = os.path.dirname(os.path.abspath(__file__))
COLS, ROWS = 100, 30
INTERVAL = 0.5


def tmux(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["tmux", "-L", SOCKET, *args], check=check,
                          capture_output=True, text=True)


class Recorder:
    def __init__(self, workdir: str):
        self.workdir = workdir
        self.frames: list[str] = []
        self.n = 0

    def shot(self) -> None:
        base = os.path.join(self.workdir, f"frame_{self.n:03d}")
        self.n += 1
        frame = tmux("capture-pane", "-t", SESSION, "-e", "-p").stdout
        with open(base + ".ansi", "w", encoding="utf-8") as fh:
            fh.write(frame)
        subprocess.run([sys.executable, os.path.join(HERE, "render_ansi.py"),
                        base + ".ansi", base + ".png",
                        "--cols", str(COLS), "--rows", str(ROWS)],
                       check=True, capture_output=True)
        self.frames.append(base + ".png")

    def hold(self, seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            self.shot()
            time.sleep(INTERVAL)

    def key(self, k: str, settle: float = 0.5) -> None:
        tmux("send-keys", "-t", SESSION, k)
        time.sleep(settle)
        self.shot()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tui", default="tui/kolma-tui")
    ap.add_argument("--engine", default="build/kolma")
    ap.add_argument("--out", default="docs/demo.gif")
    args = ap.parse_args()

    tui, engine = os.path.abspath(args.tui), os.path.abspath(args.engine)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    work = os.path.join(os.path.dirname(out), ".demo-work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    fixture = os.path.join(work, "fixture")
    os.makedirs(fixture)
    with open(os.path.join(fixture, "notes.txt"), "w") as fh:
        fh.write("the quick brown fox jumps over the lazy dog. " * 800)
    with open(os.path.join(fixture, "sensor.bin"), "wb") as fh:
        fh.write(bytes([(i // 200) % 256 for i in range(30000)]))
    os.makedirs(os.path.join(fixture, "assets"))

    tmux("kill-server", check=False)
    tmux("set", "-as", "terminal-features", ",xterm-256color:RGB", check=False)
    tmux("new-session", "-d", "-s", SESSION, "-x", str(COLS), "-y", str(ROWS),
         "bash", "-c",
         f"cd {fixture} && TERM=xterm-256color COLORTERM=truecolor {tui} --bin {engine}")
    time.sleep(1.2)
    rec = Recorder(work)
    try:
        rec.hold(1.5)                       # browse
        rec.key("j", 0.4)                   # cursor onto notes.txt
        rec.hold(0.5)
        rec.key("Enter", 0.4)               # inspect: analysing…
        rec.hold(2.0)                       # …verdict arrives
        rec.key("c", 0.4)                   # compress
        rec.hold(2.0)                       # result
        rec.key("Escape", 0.3)
        rec.key("Escape", 0.3)              # back to browse
        rec.key("j", 0.3)
        rec.key("Enter", 0.4)               # sensor.bin
        rec.hold(1.5)
        rec.key("b", 0.4)                   # benchmark
        rec.hold(3.5)                       # seven methods race
        rec.key("Escape", 0.3)
        rec.key("?", 0.4)                   # help
        rec.hold(1.5)
        rec.key("?", 0.3)
        tmux("send-keys", "-t", SESSION, "q")  # quit: no frame after this
        time.sleep(0.5)
    finally:
        tmux("kill-server", check=False)

    # Assemble: 2 fps, lanczos scale, palette-optimised GIF.
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", "2", "-i", os.path.join(work, "frame_%03d.png"),
        "-vf", "scale=900:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer:bayer_scale=3",
        out,
    ], check=True)
    size = os.path.getsize(out)
    print(f"demo: {rec.n} frames -> {out} ({size/1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
