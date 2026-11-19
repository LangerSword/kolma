package ui

import (
	"fmt"
	"strings"

	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// Premium rendering primitives borrowed from btop and yazi (studied from live
// captures): color that carries data, braille sub-cell graphs, smooth meters,
// dotted leaders and compact pills. Every helper is width-exact so the panel
// and frame invariants in app_test.go hold.

// rampStops is btop's magnitude ramp: green (good/low) -> red (bad/high).
// Interpolated in RGB per cell so a meter's color literally encodes its value.
var rampStops = [][3]int{
	{0x3F, 0xB9, 0x50}, // green
	{0xA3, 0xD9, 0x77}, // yellow-green
	{0xE3, 0xC8, 0x4A}, // yellow
	{0xE0, 0x8A, 0x3C}, // orange
	{0xE0, 0x55, 0x61}, // red
}

// rampColor maps t in [0,1] onto the magnitude ramp. 0 = green, 1 = red.
func rampColor(t float64) lipgloss.Color {
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	seg := t * float64(len(rampStops)-1)
	i := int(seg)
	if i >= len(rampStops)-1 {
		i = len(rampStops) - 2
	}
	f := seg - float64(i)
	a, b := rampStops[i], rampStops[i+1]
	r := int(float64(a[0]) + (float64(b[0]-a[0]))*f)
	g := int(float64(a[1]) + (float64(b[1]-a[1]))*f)
	bl := int(float64(a[2]) + (float64(b[2]-a[2]))*f)
	return lipgloss.Color(fmt.Sprintf("#%02X%02X%02X", r, g, bl))
}

// ratioMeter renders a ratio as a color-ramped line meter: "0.3671 ━━━━····".
// Line glyphs (not solid blocks) so it stays legible inside the mint selected
// table row, where blocks would read as black notches. The fill color encodes
// the value: a low (good) ratio is green, a high (bad) ratio trends red.
func ratioMeter(ratio, maxRatio float64, cells int) string {
	used := 0.0
	if maxRatio > 0 {
		used = ratio / maxRatio * float64(cells)
	}
	filled := max(0, min(cells, int(used+0.5)))
	t := 0.0
	if maxRatio > 0 {
		t = ratio / maxRatio
	}
	fill := lipgloss.NewStyle().Foreground(rampColor(t)).Render(strings.Repeat("━", filled))
	track := dimStyle.Render(strings.Repeat("·", cells-filled))
	return padRight(fmt.Sprintf("%7.4f ", ratio)+fill+track, 8+cells)
}

// partialBlocks are the 1/8..7/8 left-block glyphs for sub-cell meter fill.
var partialBlocks = []rune{' ', '▏', '▎', '▍', '▌', '▋', '▊', '▉'}

// gradientMeter is btop's track meter: a smooth color-ramped fill on a dim
// track, with the percentage rendered in the same color as the fill's tip so
// the number and the bar agree. Used for run/bench progress (never inside a
// selected row, so solid blocks are safe here).
func gradientMeter(pct float64, width int) string {
	if width < 4 {
		width = 4
	}
	if pct < 0 {
		pct = 0
	}
	if pct > 1 {
		pct = 1
	}
	trackW := width - 7 // leave room for " 100.0%"
	if trackW < 1 {
		trackW = 1
	}
	exact := pct * float64(trackW)
	full := int(exact)
	eighth := int((exact - float64(full)) * 8)
	var fill strings.Builder
	for i := 0; i < full; i++ {
		t := float64(i+1) / float64(trackW)
		fill.WriteString(lipgloss.NewStyle().Foreground(rampColor(t)).Render("█"))
	}
	if eighth > 0 && full < trackW {
		t := float64(full+1) / float64(trackW)
		fill.WriteString(lipgloss.NewStyle().Foreground(rampColor(t)).Render(string(partialBlocks[eighth])))
		full++
	}
	track := dimStyle.Render(strings.Repeat("░", max(0, trackW-full)))
	pctStr := lipgloss.NewStyle().Foreground(rampColor(pct)).Bold(true).Render(fmt.Sprintf("%5.1f%%", pct*100))
	return fill.String() + track + " " + pctStr
}

// braille dot bitmasks for a column filled from the bottom, heights 0..4.
// Left column = dots 1,2,3,7; right column = dots 4,5,6,8.
var brailleLeft = [5]int{0x00, 0x40, 0x44, 0x46, 0x47}
var brailleRight = [5]int{0x00, 0x80, 0xA0, 0xB0, 0xB8}

// brailleSpark renders values as a 2x4-dot braille sparkline, width chars wide
// (2 samples per char). Color is mapped to height like btop's history graphs:
// baseline green, peaks red. This is the single biggest "expensive" signal a
// TUI can carry — real dot-matrix plots instead of chunky ASCII bars.
func brailleSpark(values []float64, width int) string {
	if width < 1 || len(values) == 0 {
		return strings.Repeat(" ", max(0, width))
	}
	maxV := 0.0
	for _, v := range values {
		if v > maxV {
			maxV = v
		}
	}
	if maxV <= 0 {
		maxV = 1
	}
	// Resample to width*2 columns.
	cols := width * 2
	heights := make([]int, cols)
	for c := 0; c < cols; c++ {
		src := c * len(values) / cols
		h := int(values[src]/maxV*4 + 0.5)
		if h < 0 {
			h = 0
		}
		if h > 4 {
			h = 4
		}
		heights[c] = h
	}
	var b strings.Builder
	for c := 0; c < cols; c += 2 {
		l, r := heights[c], 0
		if c+1 < cols {
			r = heights[c+1]
		}
		code := 0x2800 + brailleLeft[l] + brailleRight[r]
		peak := l
		if r > peak {
			peak = r
		}
		style := lipgloss.NewStyle().Foreground(rampColor(float64(peak) / 4))
		if peak == 0 {
			style = dimStyle
		}
		b.WriteString(style.Render(string(rune(code))))
	}
	return b.String()
}

// pill is yazi's compact status badge: text on a rounded colored background.
func pill(s string, fg, bg lipgloss.Color) string {
	return lipgloss.NewStyle().Foreground(fg).Background(bg).Bold(true).
		Padding(0, 1).Render(s)
}

// leaderRow is btop's dotted-leader row: a left label, a dim dot leader, and a
// right-aligned value, clipped to width. Numbers scan in a clean vertical line.
func leaderRow(label, value string, width int) string {
	l := subtleStyle.Render(label)
	v := textStyle.Render(value)
	lw := ansi.StringWidth(l)
	vw := ansi.StringWidth(v)
	gap := width - lw - vw - 2
	if gap < 1 {
		return clip(l+" "+v, width)
	}
	leader := dimStyle.Render(strings.Repeat("·", gap))
	return clip(l+" "+leader+" "+v, width)
}

// fileType returns a yazi-style icon + semantic color for a directory entry.
// Color does the scanning work: dirs mint, archives amber, code cyan, text
// neutral, media violet. The icon is a single-width glyph so rows stay aligned.
func fileType(name string, isDir, isArchive bool) (string, lipgloss.Color) {
	if isDir {
		return "\uf07b", accent
	}
	if isArchive {
		return "\uf1c6", warn
	}
	lower := strings.ToLower(name)
	switch {
	case strings.HasSuffix(lower, ".cpp"), strings.HasSuffix(lower, ".hpp"),
		strings.HasSuffix(lower, ".c"), strings.HasSuffix(lower, ".h"),
		strings.HasSuffix(lower, ".go"), strings.HasSuffix(lower, ".rs"),
		strings.HasSuffix(lower, ".py"), strings.HasSuffix(lower, ".ts"),
		strings.HasSuffix(lower, ".js"), strings.HasSuffix(lower, ".java"):
		return "\uf121", info
	case strings.HasSuffix(lower, ".md"), strings.HasSuffix(lower, ".txt"),
		strings.HasSuffix(lower, ".rst"), strings.HasSuffix(lower, ".json"),
		strings.HasSuffix(lower, ".yaml"), strings.HasSuffix(lower, ".yml"),
		strings.HasSuffix(lower, ".toml"), strings.HasSuffix(lower, ".csv"):
		return "\uf15c", text
	case strings.HasSuffix(lower, ".png"), strings.HasSuffix(lower, ".jpg"),
		strings.HasSuffix(lower, ".jpeg"), strings.HasSuffix(lower, ".gif"),
		strings.HasSuffix(lower, ".svg"), strings.HasSuffix(lower, ".webp"),
		strings.HasSuffix(lower, ".mp4"), strings.HasSuffix(lower, ".mkv"),
		strings.HasSuffix(lower, ".mov"), strings.HasSuffix(lower, ".mp3"),
		strings.HasSuffix(lower, ".wav"), strings.HasSuffix(lower, ".flac"):
		return "\uf1c5", lipgloss.Color("#C678DD")
	case strings.HasSuffix(lower, ".sh"), strings.HasSuffix(lower, ".bash"),
		strings.HasSuffix(lower, ".nix"), strings.HasSuffix(lower, "makefile"),
		strings.HasSuffix(lower, "cmakelists.txt"):
		return "\uf013", lipgloss.Color("#E3C84A")
	default:
		return "\uf15b", subtle
	}
}
