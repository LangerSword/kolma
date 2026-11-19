#!/usr/bin/env python3
"""Capture the new WinRAR-like TUI features: multi-select marks, bundle
compress, archive contents, and extract-to-directory. Renders PNGs for vision
review. Reuses the gallery's tmux + render_ansi approach."""
from __future__ import annotations
import os, shutil, subprocess, sys, time

SOCKET = "kolma-feat"
SESSION = "feat"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def tmux(*a, check=True):
    return subprocess.run(["tmux", "-L", SOCKET, *a], check=check, capture_output=True, text=True)

def capture(cols, rows, base):
    frame = tmux("capture-pane", "-t", SESSION, "-e", "-p").stdout
    with open(base + ".ansi", "w") as fh:
        fh.write(frame)
    png = base + ".png"
    subprocess.run([sys.executable, os.path.join(HERE, "render_ansi.py"),
                    base + ".ansi", png, "--cols", str(cols), "--rows", str(rows)],
                   check=True, capture_output=True)
    print("  captured", os.path.basename(png))
    return png

def send(k, s=0.5):
    tmux("send-keys", "-t", SESSION, k); time.sleep(s)

def main():
    tui = os.path.join(ROOT, "tui", "kolma-tui")
    engine = os.path.join(ROOT, "build", "kolma")
    out = os.path.join(ROOT, "tui-features")
    os.makedirs(out, exist_ok=True)
    fixture = os.path.join(out, "fixture")
    shutil.rmtree(fixture, ignore_errors=True)
    os.makedirs(os.path.join(fixture, "proj", "sub"))
    with open(os.path.join(fixture, "proj", "a.txt"), "w") as fh:
        fh.write("alpha beta gamma delta epsilon. " * 300)
    with open(os.path.join(fixture, "proj", "sub", "b.txt"), "w") as fh:
        fh.write("nested content here\n" * 50)
    with open(os.path.join(fixture, "notes.txt"), "w") as fh:
        fh.write("loose file in the root\n" * 80)
    with open(os.path.join(fixture, "readme.md"), "w") as fh:
        fh.write("# readme\n\nsome markdown text to bundle\n" * 60)
    # a real bundle archive to inspect/extract
    subprocess.run([engine, "compress", os.path.join(fixture, "proj"),
                    "--out", os.path.join(fixture, "proj.kolma"), "-q"],
                   check=True, capture_output=True)
    # Remove the source folder so extracting the bundle recreates it cleanly
    # (otherwise the overwrite guard correctly refuses the collision).
    shutil.rmtree(os.path.join(fixture, "proj"))

    cols, rows = 120, 40
    tmux("kill-server", check=False)
    tmux("set", "-as", "terminal-features", ",xterm-256color:RGB", check=False)
    env_cmd = (f"cd {fixture} && TERM=xterm-256color COLORTERM=truecolor {tui} --bin {engine}")
    tmux("new-session", "-d", "-s", SESSION, "-x", str(cols), "-y", str(rows), "bash", "-c", env_cmd)
    time.sleep(1.3)
    try:
        capture(cols, rows, os.path.join(out, "f01-browse"))
        # entries (files only now): notes.txt, proj.kolma, readme.md
        send(" ", 0.3)                          # mark notes.txt -> proj.kolma
        send("Down", 0.3)                       # readme.md
        send(" ", 0.3)                          # mark readme.md
        capture(cols, rows, os.path.join(out, "f02-marked"))
        send("c", 1.8)                          # bundle the two marks -> notes.txt.kolma
        capture(cols, rows, os.path.join(out, "f03-bundle-result"))
        send("Escape", 0.4); send("Escape", 0.4)  # back to browse
        send("/", 0.4); send("proj.kolma", 0.7); send("Enter", 0.4)
        send("Enter", 1.2)                      # open the bundle -> inspect
        send("i", 1.6)                          # info + contents
        capture(cols, rows, os.path.join(out, "f04-info-contents"))
        send("Escape", 0.4)                     # back to inspect
        send("d", 1.8)                          # extract to directory
        capture(cols, rows, os.path.join(out, "f05-extract-result"))
    finally:
        tmux("kill-session", "-t", SESSION, check=False)
    tmux("kill-server", check=False)
    print("done")

if __name__ == "__main__":
    main()
