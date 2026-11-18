#!/usr/bin/env python3
"""Render an ANSI screen dump (tmux capture-pane -e) to a PNG.

Parses the SGR subset a bubbletea/lipgloss program emits (truecolor, 256-color,
bold, faint, reverse), lays the cells on a fixed monospace grid over a dark
canvas, writes an SVG and rasterises it with rsvg-convert. Stdlib only.

usage: render_ansi.py FRAME.ansi OUT.png [--cols N] [--rows N] [--font FAMILY]
"""

from __future__ import annotations

import argparse
import html
import re
import shutil
import subprocess
import sys
import tempfile

CANVAS = "#0B0F14"  # the dark editorial canvas the TUI is designed against
DEFAULT_FG = "#D8DEE9"
FONT_SIZE = 16
CELL_W = 9.6  # JetBrains Mono advance is 0.6em
LINE_H = 21
PAD = 14

SGR = re.compile(r"\x1b\[([0-9;]*)m")
OTHER_ESC = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*(?:\x07|\x1b\\)|\x1b[()][0-9A-B]|\x1b[=>]|\x1b\][^\x1b]*\x1b\\")


def xterm256(n: int) -> str:
    if n < 16:
        base = [
            "#000000", "#800000", "#008000", "#808000", "#000080", "#800080", "#008080", "#c0c0c0",
            "#808080", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#ffffff",
        ]
        return base[n]
    if n < 232:
        n -= 16
        steps = [0, 95, 135, 175, 215, 255]
        r, g, b = steps[n // 36], steps[(n // 6) % 6], steps[n % 6]
        return f"#{r:02x}{g:02x}{b:02x}"
    v = 8 + (n - 232) * 10
    return f"#{v:02x}{v:02x}{v:02x}"


class Style:
    __slots__ = ("fg", "bg", "bold", "faint")
    fg: str | None
    bg: str | None
    bold: bool
    faint: bool

    def __init__(self):
        self.fg = None
        self.bg = None
        self.bold = False
        self.faint = False

    def copy(self):
        s = Style()
        s.fg, s.bg, s.bold, s.faint = self.fg, self.bg, self.bold, self.faint
        return s

    def key(self):
        return (self.fg, self.bg, self.bold, self.faint)


def apply_sgr(style: Style, params: str) -> None:
    codes = [int(p) if p else 0 for p in params.split(";")]
    i = 0
    while i < len(codes):
        c = codes[i]
        if c == 0:
            style.__init__()
        elif c == 1:
            style.bold = True
        elif c == 2:
            style.faint = True
        elif c == 22:
            style.bold = False
            style.faint = False
        elif c == 7:
            style.fg, style.bg = style.bg, style.fg
        elif c == 27:
            style.fg, style.bg = style.bg, style.fg
        elif 30 <= c <= 37:
            style.fg = xterm256(c - 30)
        elif c == 38:
            if i + 1 < len(codes) and codes[i + 1] == 2 and i + 4 < len(codes):
                style.fg = f"#{codes[i+2]:02x}{codes[i+3]:02x}{codes[i+4]:02x}"
                i += 4
            elif i + 1 < len(codes) and codes[i + 1] == 5 and i + 2 < len(codes):
                style.fg = xterm256(codes[i + 2])
                i += 2
        elif c == 39:
            style.fg = None
        elif 40 <= c <= 47:
            style.bg = xterm256(c - 40)
        elif c == 48:
            if i + 1 < len(codes) and codes[i + 1] == 2 and i + 4 < len(codes):
                style.bg = f"#{codes[i+2]:02x}{codes[i+3]:02x}{codes[i+4]:02x}"
                i += 4
            elif i + 1 < len(codes) and codes[i + 1] == 5 and i + 2 < len(codes):
                style.bg = xterm256(codes[i + 2])
                i += 2
        elif c == 49:
            style.bg = None
        elif 90 <= c <= 97:
            style.fg = xterm256(c - 90 + 8)
        elif 100 <= c <= 107:
            style.bg = xterm256(c - 100 + 8)
        i += 1


def parse(text: str):
    """Return a list of lines, each a list of (char, Style)."""
    lines = [[]]
    style = Style()
    pos = 0
    while pos < len(text):
        m = SGR.match(text, pos)
        if m:
            apply_sgr(style, m.group(1))
            pos = m.end()
            continue
        m = OTHER_ESC.match(text, pos)
        if m:
            pos = m.end()
            continue
        ch = text[pos]
        pos += 1
        if ch == "\n":
            lines.append([])
        elif ch == "\r":
            continue
        elif ch == "\t":
            col = len(lines[-1])
            for _ in range(8 - col % 8):
                lines[-1].append((" ", style.copy()))
        elif ch >= " ":
            lines[-1].append((ch, style.copy()))
    return lines


def render_svg(lines, cols: int, rows: int, font: str) -> str:
    width = int(cols * CELL_W + PAD * 2)
    height = int(rows * LINE_H + PAD * 2)
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        f'<rect width="{width}" height="{height}" fill="{CANVAS}"/>',
        f'<g font-family="{html.escape(font)}" font-size="{FONT_SIZE}">',
    ]
    for r, line in enumerate(lines[:rows]):
        y = PAD + r * LINE_H + FONT_SIZE
        # Background runs first, so text sits on top.
        c = 0
        while c < min(len(line), cols):
            bg = line[c][1].bg
            if bg:
                start = c
                while c < min(len(line), cols) and line[c][1].bg == bg:
                    c += 1
                x = PAD + start * CELL_W
                w = (c - start) * CELL_W
                out.append(f'<rect x="{x:.1f}" y="{PAD + r * LINE_H}" width="{w:.1f}" height="{LINE_H}" fill="{bg}"/>')
            else:
                c += 1
        # Text runs grouped by style.
        c = 0
        text_el = f'<text x="{PAD}" y="{y}" xml:space="preserve" fill="{DEFAULT_FG}">'
        spans = []
        while c < min(len(line), cols):
            st = line[c][1]
            start = c
            while c < min(len(line), cols) and line[c][1].key() == st.key():
                c += 1
            chunk = "".join(ch for ch, _ in line[start:c])
            attrs = ""
            if st.fg:
                attrs += f' fill="{st.fg}"'
            if st.bold:
                attrs += ' font-weight="bold"'
            if st.faint:
                attrs += ' fill-opacity="0.55"'
            if attrs:
                spans.append(f"<tspan{attrs}>{html.escape(chunk)}</tspan>")
            else:
                spans.append(html.escape(chunk))
        if spans:
            out.append(text_el + "".join(spans) + "</text>")
    out.append("</g></svg>")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("frame")
    ap.add_argument("out")
    ap.add_argument("--cols", type=int, default=120)
    ap.add_argument("--rows", type=int, default=40)
    ap.add_argument("--font", default="JetBrainsMono Nerd Font Mono, DejaVu Sans Mono, monospace")
    args = ap.parse_args()

    with open(args.frame, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    lines = parse(text)
    svg = render_svg(lines, args.cols, args.rows, args.font)
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as fh:
        fh.write(svg)
        svg_path = fh.name
    if not shutil.which("rsvg-convert"):
        print("rsvg-convert not found", file=sys.stderr)
        return 1
    subprocess.run(["rsvg-convert", "-o", args.out, svg_path], check=True)
    print(f"rendered {args.out} ({args.cols}x{args.rows}, {len(lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
