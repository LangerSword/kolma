package ui

import "github.com/charmbracelet/bubbles/key"

// keyMap is the single source of truth for what each key does and how it is
// described. bubbles/help renders the footer and the full help screen from
// these bindings, so the documentation can never drift from the keys.
type keyMap struct {
	move, open, upDir, filter, hidden, refresh, mark,
	algo, level, priority, threads, chunk, output, password,
	compress, extract, bench, info, test,
	back, scroll, help, quit key.Binding
}

func newKeyMap() keyMap {
	return keyMap{
		move:       key.NewBinding(key.WithKeys("up", "down", "j", "k"), key.WithHelp("↑/↓", "move")),
		open:       key.NewBinding(key.WithKeys("enter", "l", "right"), key.WithHelp("enter", "open")),
		upDir:      key.NewBinding(key.WithKeys("backspace", "h", "left"), key.WithHelp("bksp", "up")),
		filter:     key.NewBinding(key.WithKeys("/"), key.WithHelp("/", "filter")),
		hidden:     key.NewBinding(key.WithKeys("."), key.WithHelp(".", "hidden")),
		refresh:    key.NewBinding(key.WithKeys("r"), key.WithHelp("r", "refresh")),
		mark:       key.NewBinding(key.WithKeys(" "), key.WithHelp("space", "mark")),
		algo:       key.NewBinding(key.WithKeys("a", "A"), key.WithHelp("a/A", "algorithm")),
		level:      key.NewBinding(key.WithKeys("[", "]"), key.WithHelp("[ ]", "level")),
		priority:   key.NewBinding(key.WithKeys("p"), key.WithHelp("p", "priority")),
		threads:    key.NewBinding(key.WithKeys("t", "T"), key.WithHelp("t/T", "threads")),
		chunk:      key.NewBinding(key.WithKeys("k"), key.WithHelp("k", "chunk")),
		output:     key.NewBinding(key.WithKeys("o"), key.WithHelp("o", "output")),
		password:   key.NewBinding(key.WithKeys("w"), key.WithHelp("w", "password")),
		compress:   key.NewBinding(key.WithKeys("c"), key.WithHelp("c", "compress")),
		extract:    key.NewBinding(key.WithKeys("d", "x"), key.WithHelp("d/x", "extract")),
		bench:      key.NewBinding(key.WithKeys("b"), key.WithHelp("b", "bench")),
		info:       key.NewBinding(key.WithKeys("i"), key.WithHelp("i", "info")),
		test:       key.NewBinding(key.WithKeys("v"), key.WithHelp("v", "test")),
		back:       key.NewBinding(key.WithKeys("esc"), key.WithHelp("esc", "back")),
		scroll:     key.NewBinding(key.WithKeys("up", "down", "pgup", "pgdown"), key.WithHelp("↑/↓", "scroll")),
		help:       key.NewBinding(key.WithKeys("?"), key.WithHelp("?", "help")),
		quit:       key.NewBinding(key.WithKeys("q"), key.WithHelp("q", "quit")),
	}
}

// shortFor feeds the footer. Order is priority order: bubbles/help ellipsizes
// from the right when the terminal is narrow, so the essentials survive.
func (k keyMap) shortFor(s screen, overflow bool) []key.Binding {
	switch s {
	case screenBrowse:
		return []key.Binding{k.move, k.open, k.mark, k.compress, k.extract, k.test, k.filter, k.help, k.quit}
	case screenInspect:
		b := []key.Binding{k.compress, k.extract, k.back, k.help, k.bench, k.info, k.output, k.password}
		if overflow {
			b = append(b, k.scroll)
		}
		return b
	case screenRun, screenInfo, screenBench:
		return []key.Binding{k.back, k.scroll, k.quit}
	case screenHelp:
		return []key.Binding{k.help, k.quit}
	}
	return nil
}

// fullGroups feeds the ? screen: one column per context.
func (k keyMap) fullGroups() [][]key.Binding {
	return [][]key.Binding{
		{k.move, k.open, k.upDir, k.mark, k.filter, k.hidden, k.refresh},
		{k.algo, k.level, k.priority, k.threads, k.chunk, k.output, k.password},
		{k.compress, k.extract, k.bench, k.info},
		{k.back, k.scroll, k.help, k.quit},
	}
}
