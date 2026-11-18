package ui

import (
	"strings"

	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// The design system, distilled from lazygit and btop:
//
//   - One dark canvas (the terminal's own background). Color only where it
//     carries meaning: mint = focus/success/selection, amber = archives and
//     in-progress states, red = errors, cyan = informational badges. Five
//     hues, each with a fixed job; everything else is neutral gray.
//   - Panels with rounded borders, titles embedded in the top border, one
//     cell of inner padding. The focused panel's border takes the accent.
//   - Two-channel highlighting: background marks the selection, foreground
//     marks data semantics. The two never fight.
//   - Two-tone rows: bright labels, dim metadata. Numbers right-aligned.
//   - A contextual key bar: bold accent keys, dim labels, always present.

var (
	accent = lipgloss.Color("#7DD3A0") // mint: focus, success, selection
	text   = lipgloss.Color("#E5E7EB")
	subtle = lipgloss.Color("#9CA3AF")
	dim    = lipgloss.Color("#6B7280")
	warn   = lipgloss.Color("#F0A868") // amber: archives, in-progress
	bad    = lipgloss.Color("#F87171") // red: errors
	info   = lipgloss.Color("#7DC4E4") // cyan: informational badges
	ink    = lipgloss.Color("#0B0F14") // text on the accent background

	brandStyle  = lipgloss.NewStyle().Foreground(accent).Bold(true)
	titleStyle  = lipgloss.NewStyle().Foreground(text).Bold(true)
	dimStyle    = lipgloss.NewStyle().Foreground(dim)
	subtleStyle = lipgloss.NewStyle().Foreground(subtle)
	textStyle   = lipgloss.NewStyle().Foreground(text)
	accentStyle = lipgloss.NewStyle().Foreground(accent).Bold(true)
	warnStyle   = lipgloss.NewStyle().Foreground(warn)
	badStyle    = lipgloss.NewStyle().Foreground(bad)
	infoStyle   = lipgloss.NewStyle().Foreground(info)

	labelStyle = lipgloss.NewStyle().Foreground(dim).Width(14)
	valueStyle = lipgloss.NewStyle().Foreground(text)

	selectedStyle = lipgloss.NewStyle().
			Foreground(ink).
			Background(accent).
			Bold(true)

	keyStyle    = lipgloss.NewStyle().Foreground(accent).Bold(true)
	actionStyle = lipgloss.NewStyle().Foreground(subtle)

	okBadge  = lipgloss.NewStyle().Foreground(accent).Bold(true)
	errBadge = lipgloss.NewStyle().Foreground(bad).Bold(true)
)

// sanitize renders untrusted text (file names, archive source names, engine
// errors) for the terminal: control characters become '?'. A file literally
// named with an escape sequence must not be able to forge the display.
func sanitize(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for _, r := range s {
		if r < 0x20 || r == 0x7F {
			b.WriteByte('?')
		} else {
			b.WriteRune(r)
		}
	}
	return b.String()
}

// clip truncates to a display width (rune- and ANSI-aware), with an ellipsis.
func clip(s string, width int) string {
	if width <= 0 {
		return ""
	}
	if ansi.StringWidth(s) <= width {
		return s
	}
	return ansi.Truncate(s, width, "…")
}

// clipLeft keeps the tail of a path, prefixing an ellipsis: /…/parent/name.
func clipLeft(s string, width int) string {
	if width <= 0 {
		return ""
	}
	w := ansi.StringWidth(s)
	if w <= width {
		return s
	}
	// TruncateLeft REMOVES n cells from the left; remove just enough that the
	// remainder plus the ellipsis fits the requested width.
	return ansi.TruncateLeft(s, w-width+1, "…")
}

func padRight(s string, width int) string {
	gap := width - ansi.StringWidth(s)
	if gap <= 0 {
		return s
	}
	return s + strings.Repeat(" ", gap)
}

func padLeft(s string, width int) string {
	gap := width - ansi.StringWidth(s)
	if gap <= 0 {
		return s
	}
	return strings.Repeat(" ", gap) + s
}

// panel draws a rounded box with the title embedded in the top border and an
// optional right-aligned annotation (a counter, a key hint). Content lines are
// clipped and padded to the inner width; the box is exactly width x height.
func panel(title, right, content string, width, height int, focused bool) string {
	if width < 8 {
		width = 8
	}
	if height < 3 {
		height = 3
	}
	borderColor := dim
	if focused {
		borderColor = accent
	}
	border := lipgloss.NewStyle().Foreground(borderColor)
	inner := width - 4 // two border cells, one padding cell each side

	titleRendered := titleStyle.Render(title)
	if focused {
		titleRendered = accentStyle.Render(title)
	}
	left := "╭─ " + titleRendered + " "
	if ansi.StringWidth(left) > width-2 {
		left = ansi.Truncate(left, width-2, "")
	}
	leftW := ansi.StringWidth(left)
	rightPart := ""
	endWidth := 2 // the plain "─╮" cap
	if right != "" {
		// The annotation shrinks to whatever the title leaves room for.
		maxRight := width - leftW - 4 // " " + text + " ─╮"
		if maxRight >= 1 {
			rightText := clip(right, min(inner/2, maxRight))
			rightPart = " " + dimStyle.Render(rightText) + " ─╮"
			endWidth = ansi.StringWidth(rightPart)
		}
	}
	fill := width - leftW - endWidth
	if fill < 0 {
		fill = 0
	}
	top := left + border.Render(strings.Repeat("─", fill))
	if rightPart == "" {
		top += border.Render("─╮")
	} else {
		top += rightPart
	}

	lines := strings.Split(content, "\n")
	body := make([]string, 0, height-2)
	for i := 0; i < height-2; i++ {
		line := ""
		if i < len(lines) {
			line = clip(lines[i], inner)
		}
		body = append(body, border.Render("│")+" "+padRight(line, inner)+" "+border.Render("│"))
	}
	bottom := border.Render("╰" + strings.Repeat("─", width-2) + "╯")
	return top + "\n" + strings.Join(body, "\n") + "\n" + bottom
}

// panelAuto sizes the box to its content (for stacking inside the viewport).
func panelAuto(title, right, content string, width int, focused bool) string {
	n := len(strings.Split(content, "\n"))
	return panel(title, right, content, width, n+2, focused)
}

// panelFill stretches the box to at least minHeight so short content doesn't
// float in dead space; taller content keeps its natural height and scrolls.
func panelFill(title, right, content string, width, minHeight int, focused bool) string {
	n := len(strings.Split(content, "\n")) + 2
	if n < minHeight {
		n = minHeight
	}
	return panel(title, right, content, width, n, focused)
}

// keyHint renders one "key action" pair for the bottom bar.
func keyHint(key, action string) string {
	return keyStyle.Render(key) + " " + actionStyle.Render(action)
}

// header is the top line: brand, screen title, breadcrumb, engine version.
func header(screen, breadcrumb, version string, width int) string {
	left := brandStyle.Render("kolma") + "  " + titleStyle.Render(screen)
	if breadcrumb != "" {
		left += "  " + dimStyle.Render(clipLeft(sanitize(breadcrumb), max(10, width/2)))
	}
	right := ""
	if version != "" {
		right = dimStyle.Render(version)
	}
	gap := width - ansi.StringWidth(left) - ansi.StringWidth(right)
	if gap < 1 {
		// Not enough room for both: keep the left, drop the version.
		return clip(left, width)
	}
	return left + strings.Repeat(" ", gap) + right
}

// row renders a label/value line with an optional right-aligned key hint,
// clipped to width so it can never overflow its panel.
func row(label, value, hint string, width int) string {
	line := "  " + labelStyle.Render(label) + valueStyle.Render(value)
	if hint != "" {
		h := dimStyle.Render(hint)
		gap := width - ansi.StringWidth(line) - ansi.StringWidth(h) - 2
		if gap >= 2 {
			return clip(line+strings.Repeat(" ", gap)+h, width)
		}
	}
	return clip(line, width)
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
