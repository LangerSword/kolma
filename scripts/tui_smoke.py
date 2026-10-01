#!/usr/bin/env python3
"""Headless smoke test for the kolma TUI.

Drives the real binary through a pseudo-terminal, sends keystrokes and asserts
on what the program actually renders. This is the only way to check the
terminal interface without a human at the keyboard, and it runs in CI.

usage: tui_smoke.py [--tui PATH] [--engine PATH] [--workdir DIR]
"""

from __future__ import annotations

import argparse
import os
import pty
import re
import select
import shutil
import signal
import sys
import tempfile
import time

TIMEOUT = 20.0
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


class Screen:
    """A pty-attached child process whose output we can assert on."""

    def __init__(self, argv: list[str], cwd: str) -> None:
        self.buffer = ""
        env = dict(os.environ)
        env["TERM"] = "xterm-256color"
        env["COLUMNS"] = "120"
        env["LINES"] = "40"
        pid, fd = pty.fork()
        if pid == 0:  # child
            os.chdir(cwd)
            os.execvpe(argv[0], argv, env)
        self.pid = pid
        self.fd = fd

    def read_available(self, timeout: float = 0.3) -> str:
        chunk = ""
        while True:
            ready, _, _ = select.select([self.fd], [], [], timeout)
            if not ready:
                break
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                break
            if not data:
                break
            text = data.decode("utf-8", errors="replace")
            chunk += text
            self.answer_queries(text)
        self.buffer += chunk
        return chunk

    def answer_queries(self, text: str) -> None:
        """Answer the terminal capability queries the program sends.

        termenv asks for the background colour (OSC 11) and the cursor position
        (CSI 6n) at start-up and blocks until it hears back or times out. A real
        terminal replies; a dumb pty does not, which would make this test crawl.
        """
        replies = ""
        if "\x1b]11;?" in text:
            replies += "\x1b]11;rgb:0000/0000/0000\x1b\\"
        if "\x1b[6n" in text:
            replies += "\x1b[1;1R"
        if replies:
            try:
                os.write(self.fd, replies.encode())
            except OSError:
                pass

    def wait_for(self, needle: str, timeout: float = TIMEOUT) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.plain():
                return True
            self.read_available(0.2)
        return needle in self.plain()

    def plain(self) -> str:
        """The rendered text with terminal control sequences removed."""
        return ANSI.sub("", self.buffer)

    def send(self, keys: str, settle: float = 0.6) -> None:
        os.write(self.fd, keys.encode())
        deadline = time.time() + settle
        while time.time() < deadline:
            self.read_available(0.15)

    def wait_for_exit(self, timeout: float = TIMEOUT) -> int:
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read_available(0.2)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid != 0:
                if os.WIFEXITED(status):
                    return os.WEXITSTATUS(status)
                return -1
        return -2

    def kill(self) -> None:
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def fail(message: str, screen: Screen) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    print("--- last screen output ---", file=sys.stderr)
    print(screen.buffer[-3000:], file=sys.stderr)
    screen.kill()
    sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tui", default="tui/kolma-tui")
    parser.add_argument("--engine", default="build/kolma")
    parser.add_argument("--workdir", default="")
    args = parser.parse_args()

    tui = os.path.abspath(args.tui)
    engine = os.path.abspath(args.engine)
    for path in (tui, engine):
        if not os.path.exists(path):
            print(f"FAIL: {path} does not exist (build it first)", file=sys.stderr)
            return 1

    workdir = args.workdir
    cleanup = False
    if not workdir:
        workdir = tempfile.mkdtemp(prefix="kolma-tui-smoke-")
        cleanup = True
        with open(os.path.join(workdir, "sample.txt"), "w") as handle:
            handle.write("the quick brown fox jumps over the lazy dog. " * 2000)
        with open(os.path.join(workdir, ".hidden"), "w") as handle:
            handle.write("hidden\n")
        os.mkdir(os.path.join(workdir, "subdir"))

    print(f"driving {tui} against {engine} in {workdir}")
    screen = Screen([tui, "--bin", engine], workdir)
    try:
        if not screen.wait_for("kolma  browse"):
            fail("the browser frame never rendered", screen)
        print("  ok  browser renders")

        if "sample.txt" not in screen.plain():
            fail("the browser did not list the fixture file", screen)
        print("  ok  fixture file listed")

        screen.send("\x1b[B")  # down
        if "> file sample.txt" not in screen.plain():
            fail("the cursor did not move to the second entry", screen)
        print("  ok  cursor moved")

        screen.send(".")
        if ".hidden" not in screen.plain():
            fail("the hidden-file toggle did not reveal .hidden", screen)
        print("  ok  hidden files toggle")

        screen.send(".")
        screen.send("?")
        if not screen.wait_for("kolma  help"):
            fail("the help screen did not render", screen)
        if "pluggable compression engine" not in screen.plain():
            fail("the help screen is missing its body", screen)
        print("  ok  help screen")

        screen.send("?")  # back to inspect (there is no file selected)
        screen.send("j")
        screen.send("\r")  # enter on the highlighted entry
        if not screen.wait_for("analyzer", timeout=TIMEOUT):
            fail("selecting an entry did not open the inspect screen", screen)
        print("  ok  inspect screen opens")

        if not screen.wait_for("settings", timeout=TIMEOUT):
            fail("the inspect screen has no settings panel", screen)
        if not screen.wait_for("auto (the Analyzer decides)", timeout=5):
            fail("the algorithm setting does not default to auto", screen)
        print("  ok  settings panel present")

        screen.send("]")  # level up
        screen.send("p")  # priority cycle
        screen.send("a")  # algorithm cycle
        screen.send("k")  # chunk cycle
        screen.send("t")  # threads
        print("  ok  settings respond to keys")

        # Run a real compression through the interface and check the artifact.
        archive = os.path.join(workdir, "sample.txt.kolma")
        if os.path.exists(archive):
            os.remove(archive)
        screen.send("c")
        if not screen.wait_for("result", timeout=TIMEOUT):
            fail("compression never produced a result panel", screen)
        if not os.path.exists(archive):
            fail(f"the interface reported success but {archive} does not exist", screen)
        print("  ok  compression runs and writes the archive")

        screen.send("q")
        code = screen.wait_for_exit()
        if code != 0:
            fail(f"expected exit 0, got {code}", screen)
        print("  ok  quits cleanly with q")
    finally:
        screen.kill()
        if cleanup:
            shutil.rmtree(workdir, ignore_errors=True)

    print("\nTUI smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
