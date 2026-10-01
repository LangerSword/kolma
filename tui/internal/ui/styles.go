package ui

import "github.com/charmbracelet/lipgloss"

// A dark editorial palette: one accent, one warning, everything else neutral.
var (
	accent = lipgloss.Color("#7DD3A0")
	dim    = lipgloss.Color("#6B7280")
	text   = lipgloss.Color("#E5E7EB")
	warn   = lipgloss.Color("#F0A868")
	bad    = lipgloss.Color("#F87171")
	subtle = lipgloss.Color("#9CA3AF")

	titleStyle = lipgloss.NewStyle().
			Foreground(accent).
			Bold(true)

	headerStyle = lipgloss.NewStyle().
			Foreground(text).
			Bold(true)

	breadcrumbStyle = lipgloss.NewStyle().
			Foreground(subtle)

	labelStyle = lipgloss.NewStyle().
			Foreground(dim).
			Width(14)

	valueStyle = lipgloss.NewStyle().
			Foreground(text)

	accentValueStyle = lipgloss.NewStyle().
				Foreground(accent).
				Bold(true)

	dimStyle = lipgloss.NewStyle().Foreground(dim)

	warnStyle = lipgloss.NewStyle().Foreground(warn)

	badStyle = lipgloss.NewStyle().Foreground(bad)

	selectedStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#0B0F14")).
			Background(accent).
			Bold(true)

	cursorStyle = lipgloss.NewStyle().Foreground(accent)

	footerStyle = lipgloss.NewStyle().
			Foreground(dim)

	boxStyle = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(dim).
			Padding(0, 1)

	activeBoxStyle = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(accent).
			Padding(0, 1)
)

func header(title, breadcrumb string) string {
	return titleStyle.Render("kolma") + "  " + headerStyle.Render(title) + "   " +
		breadcrumbStyle.Render(breadcrumb)
}

func footer(hints string) string { return footerStyle.Render(hints) }
