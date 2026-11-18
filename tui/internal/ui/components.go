package ui

import (
	"fmt"
	"io"

	"github.com/charmbracelet/bubbles/help"
	"github.com/charmbracelet/bubbles/key"
	"github.com/charmbracelet/bubbles/list"
	"github.com/charmbracelet/bubbles/paginator"
	"github.com/charmbracelet/bubbles/table"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/LangerSword/kolma/tui/internal/core"
)

// entryItem adapts a directory entry to bubbles/list. FilterValue is what the
// fuzzy filter matches against.
type entryItem struct{ entry core.Entry }

func (i entryItem) FilterValue() string { return i.entry.Name }

// entryDelegate renders one file row: kind chip, name, right-aligned size.
// Selection is a full-width mint bar (the background channel); kind keeps its
// semantic color in the foreground channel. One line per item, no spacing,
// so the browser reads like yazi rather than a menu.
type entryDelegate struct{}

func (d entryDelegate) Height() int                             { return 1 }
func (d entryDelegate) Spacing() int                            { return 0 }
func (d entryDelegate) Update(_ tea.Msg, _ *list.Model) tea.Cmd { return nil }

func (d entryDelegate) Render(w io.Writer, m list.Model, index int, item list.Item) {
	e, ok := item.(entryItem)
	if !ok {
		return
	}
	inner := m.Width()
	name := sanitize(e.entry.Name)
	kind := "file"
	if e.entry.IsDir {
		kind = "dir "
		name += "/"
	} else if e.entry.IsArchive {
		kind = "arc "
	}
	size := core.HumanSize(uint64(max64(e.entry.Size)))
	nameW := max(8, inner-2-4-1-10-1)
	nameClipped := padRight(clip(name, nameW), nameW)
	if index == m.Index() {
		raw := fmt.Sprintf("▸ %s %s %s", kind, nameClipped, padLeft(size, 10))
		fmt.Fprint(w, selectedStyle.Render(padRight(raw, inner)))
		return
	}
	kindStyled := dimStyle.Render(kind)
	nameStyled := textStyle.Render(nameClipped)
	if e.entry.IsDir {
		kindStyled = accentStyle.Render(kind)
		nameStyled = accentStyle.Render(nameClipped)
	} else if e.entry.IsArchive {
		kindStyled = warnStyle.Render(kind)
	}
	fmt.Fprintf(w, "  %s %s %s", kindStyled, nameStyled, dimStyle.Render(padLeft(size, 10)))
}

// newBrowser builds the bubbles/list browser with the brand palette and a
// keymap that leaves enter/h/l free for directory navigation.
func newBrowser(width, height int) list.Model {
	ls := list.New(nil, entryDelegate{}, width, height)
	ls.SetShowTitle(false) // the app header carries the title
	ls.SetShowStatusBar(true)
	ls.SetShowPagination(true)
	ls.SetShowHelp(false) // the app key bar carries the hints
	ls.SetFilteringEnabled(true)
	ls.SetShowFilter(true)
	ls.Paginator.Type = paginator.Arabic
	ls.KeyMap = list.KeyMap{
		CursorUp:   key.NewBinding(key.WithKeys("up", "k")),
		CursorDown: key.NewBinding(key.WithKeys("down", "j")),
		// h/l/left/right belong to directory navigation, not paging.
		PrevPage:             key.NewBinding(key.WithKeys("pgup", "b", "u")),
		NextPage:             key.NewBinding(key.WithKeys("pgdown", "f")),
		GoToStart:            key.NewBinding(key.WithKeys("home", "g")),
		GoToEnd:              key.NewBinding(key.WithKeys("end", "G")),
		Filter:               key.NewBinding(key.WithKeys("/"), key.WithHelp("/", "filter")),
		ClearFilter:          key.NewBinding(key.WithKeys("esc")),
		CancelWhileFiltering: key.NewBinding(key.WithKeys("esc")),
		AcceptWhileFiltering: key.NewBinding(key.WithKeys("enter", "tab", "up", "down", "ctrl+k", "ctrl+j")),
		ShowFullHelp:         key.NewBinding(key.WithDisabled()),
		CloseFullHelp:        key.NewBinding(key.WithDisabled()),
		Quit:                 key.NewBinding(key.WithDisabled()), // the app owns q/esc
		ForceQuit:            key.NewBinding(key.WithKeys("ctrl+c")),
	}
	ls.Styles = brandListStyles()
	ls.FilterInput.Cursor.Style = lipgloss.NewStyle().Foreground(accent)
	ls.FilterInput.Prompt = "filter: "
	return ls
}

func brandListStyles() list.Styles {
	s := list.DefaultStyles()
	s.Title = lipgloss.NewStyle().Foreground(text).Bold(true)
	s.StatusBar = lipgloss.NewStyle().Foreground(dim).Padding(0, 1)
	s.StatusEmpty = lipgloss.NewStyle().Foreground(dim)
	s.StatusBarActiveFilter = lipgloss.NewStyle().Foreground(warn)
	// The list concatenates "N items" + "M filtered" with no separator; the
	// left padding is what keeps them from running together.
	s.StatusBarFilterCount = lipgloss.NewStyle().Foreground(dim).Padding(0, 1)
	s.PaginationStyle = lipgloss.NewStyle().Foreground(dim).Padding(0, 1)
	s.ActivePaginationDot = lipgloss.NewStyle().Foreground(accent)
	s.InactivePaginationDot = lipgloss.NewStyle().Foreground(dim)
	s.DividerDot = lipgloss.NewStyle().Foreground(dim)
	s.FilterPrompt = lipgloss.NewStyle().Foreground(accent)
	s.FilterCursor = lipgloss.NewStyle().Foreground(accent)
	s.DefaultFilterCharacterMatch = lipgloss.NewStyle().Foreground(warn).Underline(true)
	s.HelpStyle = lipgloss.NewStyle().Foreground(dim).Padding(0, 1)
	return s
}

// newBenchTable builds the bubbles/table used on the benchmark screen.
// The Cell style is deliberately bare: any style on a cell emits an ANSI
// reset that would cancel the Selected row's background after the first
// cell. Padding is baked into the cell values instead, so the selection
// paints the whole row cleanly.
func newBenchTable(width int) table.Model {
	t := table.New(
		table.WithColumns(benchColumns(false)),
		table.WithFocused(true),
		table.WithHeight(8),
		table.WithWidth(width),
	)
	s := table.DefaultStyles()
	s.Header = lipgloss.NewStyle().Foreground(dim).Bold(true)
	s.Cell = lipgloss.NewStyle()
	s.Selected = lipgloss.NewStyle().Foreground(ink).Background(accent).Bold(true)
	t.SetStyles(s)
	return t
}

func benchColumns(wide bool) []table.Column {
	if wide {
		return []table.Column{
			{Title: " algorithm", Width: 14},
			{Title: " ratio", Width: 26},
			{Title: " output", Width: 11},
			{Title: " throughput", Width: 13},
			{Title: " note", Width: 18},
		}
	}
	return []table.Column{
		{Title: " algorithm", Width: 14},
		{Title: " ratio", Width: 8},
		{Title: " output", Width: 11},
		{Title: " throughput", Width: 13},
	}
}

func benchRow(r core.BenchmarkRow, maxRatio float64, wide bool) table.Row {
	note := ""
	if r.BestRatio {
		note = "best ratio"
	}
	if r.BestSpeed {
		if note != "" {
			note += ", "
		}
		note += "fastest"
	}
	algo := " " + padRight(r.Algo, 12) + " "
	out := " " + padLeft(core.HumanSize(r.OutputBytes), 9) + " "
	thr := " " + padLeft(core.HumanRate(r.ThroughputMBps), 11) + " "
	if wide {
		return table.Row{algo, " " + ratioBar(r.Ratio, maxRatio) + " ", out, thr, " " + padRight(note, 16) + " "}
	}
	return table.Row{algo, " " + padRight(fmt.Sprintf("%.4f", r.Ratio), 6) + " ", out, thr}
}

func brandHelp() help.Model {
	h := help.New()
	h.Styles.ShortKey = lipgloss.NewStyle().Foreground(accent).Bold(true)
	h.Styles.ShortDesc = lipgloss.NewStyle().Foreground(subtle)
	h.Styles.ShortSeparator = lipgloss.NewStyle().Foreground(dim)
	h.Styles.Ellipsis = lipgloss.NewStyle().Foreground(dim)
	h.Styles.FullKey = lipgloss.NewStyle().Foreground(accent).Bold(true)
	h.Styles.FullDesc = lipgloss.NewStyle().Foreground(subtle)
	h.Styles.FullSeparator = lipgloss.NewStyle().Foreground(dim)
	return h
}
