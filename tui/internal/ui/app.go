package ui

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/charmbracelet/bubbles/progress"
	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"

	"github.com/LangerSword/kolma/tui/internal/core"
)

type screen int

const (
	screenBrowse screen = iota
	screenInspect
	screenRun
	screenInfo
	screenBench
	screenHelp
)

var priorities = []string{"maxratio", "balanced", "maxspeed"}

var chunkChoices = []int64{0, 4096, 65536, 262144, 1048576, 4194304}

// ------------------------------------------------------------------ messages

type progressMsg struct {
	runID int
	p     core.Progress
}

type resultMsg struct {
	runID int
	kind  string
	res   core.Result
	err   error
}

type analysisMsg struct {
	runID int
	a     core.Analysis
	err   error
}

type infoMsg struct {
	runID int
	info  core.ArchiveInfo
	err   error
}

type algosMsg struct {
	algos []core.AlgoParams
	err   error
}

type errMsg struct{ err error }

// --------------------------------------------------------------------- model

type Model struct {
	client core.Client
	width  int
	height int

	screen screen
	// Where ? was pressed, so closing help returns there instead of guessing.
	helpReturn screen
	status     string
	statusErr  bool

	cwd        string
	entries    []core.Entry
	cursor     int
	showHidden bool

	target     core.Entry
	analysis   core.Analysis
	analyzing  bool
	analyzeErr string

	algos    []core.AlgoParams
	algoIdx  int // 0 = auto, 1..len(algos)
	level    int // 0 = auto
	priority int
	threads  int
	chunkIdx int

	inputs  [2]textinput.Model // 0 = output path, 1 = password
	editing int                // -1 none, 0 output, 1 password

	runID      int
	running    bool
	runKind    string
	progress   core.Progress
	result     core.Result
	progressCh chan core.Progress

	// pendingForce implements the overwrite confirmation: the first attempt
	// refuses an existing output, the next c/d press confirms with --force.
	pendingForce bool

	archive    *core.ArchiveInfo
	archiveErr string

	bench []core.BenchmarkRow

	spinner spinner.Model
	bar     progress.Model
	vp      viewport.Model

	version string
}

func New(client core.Client, version string) Model {
	output := textinput.New()
	output.Placeholder = "(derived from the input name)"
	output.CharLimit = 512
	output.Width = 48
	output.Prompt = ""

	password := textinput.New()
	password.Placeholder = "(none: the archive will not be encrypted)"
	password.CharLimit = 256
	password.Width = 48
	password.Prompt = ""
	password.EchoMode = textinput.EchoPassword

	sp := spinner.New(spinner.WithSpinner(spinner.MiniDot),
		spinner.WithStyle(lipgloss.NewStyle().Foreground(accent)))
	bar := progress.New(
		progress.WithSolidFill(string(accent)),
		progress.WithFillCharacters('█', '─'),
		progress.WithoutPercentage(),
		progress.WithWidth(40),
	)

	cwd, err := os.Getwd()
	if err != nil {
		cwd = "."
	}
	m := Model{
		client:   client,
		width:    100,
		height:   30,
		cwd:      cwd,
		editing:  -1,
		level:    0,
		priority: 1,
		threads:  0,
		chunkIdx: 0,
		version:  version,
		inputs:   [2]textinput.Model{output, password},
		spinner:  sp,
		bar:      bar,
		vp:       viewport.New(100, 27),
	}
	m.refresh()
	return m
}

func (m *Model) refresh() {
	entries, err := core.ListDir(m.cwd, m.showHidden)
	if err != nil {
		m.status = fmt.Sprintf("cannot read %s: %v", m.cwd, err)
		m.statusErr = true
		m.entries = nil
		return
	}
	m.entries = entries
	if m.cursor >= len(m.entries) {
		m.cursor = max(0, len(m.entries)-1)
	}
}

func (m Model) Init() tea.Cmd {
	return func() tea.Msg {
		algos, err := m.client.Algorithms()
		return algosMsg{algos: algos, err: err}
	}
}

// ------------------------------------------------------------------ helpers

func (m Model) algoName() string {
	if m.algoIdx == 0 {
		return "auto (the Analyzer decides)"
	}
	if m.algoIdx-1 < len(m.algos) {
		return m.algos[m.algoIdx-1].ID
	}
	return "auto"
}

func (m Model) algoID() string {
	if m.algoIdx == 0 || m.algoIdx-1 >= len(m.algos) {
		return "auto"
	}
	return m.algos[m.algoIdx-1].ID
}

func (m Model) levelText() string {
	if m.level == 0 {
		return "auto (from the priority)"
	}
	return fmt.Sprintf("%d", m.level)
}

func (m Model) threadsText() string {
	if m.threads == 0 {
		return "auto (hardware concurrency)"
	}
	return fmt.Sprintf("%d", m.threads)
}

func (m Model) chunkText() string {
	c := chunkChoices[m.chunkIdx]
	if c == 0 {
		return "auto (from level and memory limit)"
	}
	return core.HumanSize(uint64(c))
}

// defaultOutput is what the engine will derive when no explicit path is set:
// <input>.kolma for compression, the stripped name for an archive.
func (m Model) defaultOutput() string {
	if m.target.Name == "" {
		return "(no file selected)"
	}
	if m.target.IsArchive {
		if strings.HasSuffix(m.target.Path, ".kolma") {
			return strings.TrimSuffix(m.target.Path, ".kolma")
		}
		return m.target.Path + ".out"
	}
	return m.target.Path + ".kolma"
}

func (m Model) outputText() string {
	if v := strings.TrimSpace(m.inputs[0].Value()); v != "" {
		return v
	}
	return m.defaultOutput()
}

func (m Model) passwordSet() bool { return m.inputs[1].Value() != "" }

func (m *Model) setStatus(msg string, isErr bool) {
	m.status = msg
	m.statusErr = isErr
}

// contentHeight is the space between the header line and the status/key bars.
func (m Model) contentHeight() int { return max(3, m.height-3) }

func (m Model) visibleRows() int { return max(3, m.contentHeight()-2) }

func (m Model) windowOffset() int {
	rows := m.visibleRows()
	if m.cursor < rows {
		return 0
	}
	return m.cursor - rows + 1
}

func (m Model) shortVersion() string {
	fields := strings.Fields(m.version)
	if len(fields) >= 2 {
		return fields[0] + " " + fields[1]
	}
	return m.version
}

// --------------------------------------------------------------------- update

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		m.vp.Width = msg.Width
		m.vp.Height = m.contentHeight()
		for i := range m.inputs {
			m.inputs[i].Width = min(48, max(10, msg.Width-22))
		}
		m.bar.Width = min(60, max(10, msg.Width-12))
		return m, nil

	case spinner.TickMsg:
		var cmd tea.Cmd
		m.spinner, cmd = m.spinner.Update(msg)
		if !m.analyzing && !m.running {
			cmd = nil // stop the chain when there is nothing to wait for
		}
		return m, cmd

	case algosMsg:
		if msg.err == nil {
			m.algos = msg.algos
		}
		return m, nil

	case errMsg:
		m.setStatus(msg.err.Error(), true)
		m.running = false
		return m, nil

	case progressMsg:
		if msg.runID != m.runID {
			return m, nil
		}
		m.progress = msg.p
		return m, m.waitProgress(msg.runID)

	case analysisMsg:
		if msg.runID != m.runID {
			return m, nil
		}
		m.analyzing = false
		if msg.err != nil {
			m.analyzeErr = msg.err.Error()
			return m, nil
		}
		m.analyzeErr = ""
		m.analysis = msg.a
		return m, nil

	case infoMsg:
		if msg.runID != m.runID {
			return m, nil
		}
		m.running = false
		if msg.err != nil {
			m.archive = nil
			m.archiveErr = msg.err.Error()
			return m, nil
		}
		m.archiveErr = ""
		m.archive = &msg.info
		m.vp.GotoTop()
		return m, nil

	case resultMsg:
		if msg.runID != m.runID {
			return m, nil
		}
		m.running = false
		m.result = msg.res
		if msg.kind == "benchmark" {
			m.bench = msg.res.Benchmark
		}
		if msg.err != nil {
			m.setStatus(msg.err.Error(), true)
		} else if !msg.res.OK {
			if strings.Contains(msg.res.Error, "already exists") {
				m.pendingForce = true
				m.setStatus("output exists — press c or d again to overwrite", true)
			} else {
				m.setStatus(msg.res.Error, true)
			}
		} else {
			if msg.kind == "benchmark" && len(msg.res.Benchmark) > 0 {
				best := msg.res.Benchmark[0]
				m.setStatus(fmt.Sprintf("benchmark finished: best ratio %.4f (%s)", best.Ratio, best.Algo), false)
			} else {
				m.setStatus(fmt.Sprintf("%s finished: ratio %.4f", msg.kind, msg.res.Stats.Ratio), false)
			}
			m.refresh()
		}
		m.vp.GotoTop()
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)

	case tea.MouseMsg:
		return m.handleMouse(msg)
	}
	return m, nil
}

func (m Model) handleMouse(msg tea.MouseMsg) (tea.Model, tea.Cmd) {
	if m.screen == screenBrowse {
		switch msg.Button {
		case tea.MouseButtonWheelUp:
			m.cursor = max(0, m.cursor-3)
		case tea.MouseButtonWheelDown:
			m.cursor = min(len(m.entries)-1, m.cursor+3)
		case tea.MouseButtonLeft:
			if msg.Action == tea.MouseActionPress {
				// Row 0 is the header, row 1 the panel's top border.
				idx := m.windowOffset() + msg.Y - 2
				if idx >= 0 && idx < len(m.entries) {
					m.cursor = idx
				}
			}
		}
		return m, nil
	}
	var cmd tea.Cmd
	m.vp, cmd = m.vp.Update(msg)
	return m, cmd
}

func (m Model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	key := msg.String()

	// A text field owns the keyboard while it is being edited.
	if m.editing >= 0 {
		switch key {
		case "esc", "enter":
			m.editing = -1
			m.inputs[0].Blur()
			m.inputs[1].Blur()
			return m, nil
		case "tab":
			m.startEditing(1 - m.editing)
			return m, nil
		}
		var cmd tea.Cmd
		m.inputs[m.editing], cmd = m.inputs[m.editing].Update(msg)
		return m, cmd
	}

	switch key {
	case "ctrl+c":
		return m, tea.Quit
	case "?":
		if m.screen == screenHelp {
			m.screen = m.helpReturn
		} else {
			m.helpReturn = m.screen
			m.screen = screenHelp
			m.status = ""
			m.statusErr = false
		}
		m.vp.GotoTop()
		return m, nil
	case "q":
		// Quitting is always available; esc is the key that walks back.
		return m, tea.Quit
	case "esc":
		if m.screen == screenHelp {
			m.screen = m.helpReturn
			m.vp.GotoTop()
			return m, nil
		}
	}

	switch m.screen {
	case screenBrowse:
		return m.browseKey(key)
	case screenInspect:
		// Settings keys own the letters; the arrows and page keys scroll.
		switch key {
		case "up", "down", "pgup", "pgdown", "home", "end", "ctrl+u", "ctrl+d":
			var cmd tea.Cmd
			m.vp, cmd = m.vp.Update(msg)
			return m, cmd
		}
		return m.inspectKey(key)
	case screenRun, screenInfo, screenBench:
		if key == "esc" {
			m.screen = screenInspect
			m.vp.GotoTop()
			return m, nil
		}
		var cmd tea.Cmd
		m.vp, cmd = m.vp.Update(msg)
		return m, cmd
	case screenHelp:
		var cmd tea.Cmd
		m.vp, cmd = m.vp.Update(msg)
		return m, cmd
	}
	return m, nil
}

func (m Model) browseKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "up", "k":
		if m.cursor > 0 {
			m.cursor--
		}
	case "down", "j":
		if m.cursor < len(m.entries)-1 {
			m.cursor++
		}
	case "g":
		m.cursor = 0
	case "G":
		m.cursor = max(0, len(m.entries)-1)
	case "backspace", "left", "h":
		parent := filepath.Dir(m.cwd)
		if parent != m.cwd {
			m.cwd = parent
			m.cursor = 0
			m.refresh()
		}
	case "r":
		m.refresh()
	case ".":
		m.showHidden = !m.showHidden
		m.refresh()
	case "enter", "l", "right":
		if len(m.entries) == 0 {
			return m, nil
		}
		entry := m.entries[m.cursor]
		if entry.IsDir {
			m.cwd = entry.Path
			m.cursor = 0
			m.refresh()
			return m, nil
		}
		m.selectTarget(entry)
		return m, tea.Batch(m.analyzeCmd(entry.Path), m.spinner.Tick)
	}
	return m, nil
}

func (m *Model) selectTarget(entry core.Entry) {
	m.target = entry
	m.screen = screenInspect
	m.analyzing = true
	m.analyzeErr = ""
	m.analysis = core.Analysis{}
	m.result = core.Result{}
	m.archive = nil
	m.archiveErr = ""
	m.bench = nil
	m.pendingForce = false
	m.status = ""
	m.statusErr = false
	m.runID++
	m.vp.GotoTop()
}

func (m Model) analyzeCmd(path string) tea.Cmd {
	client := m.client
	priority := priorities[m.priority]
	runID := m.runID
	return func() tea.Msg {
		a, err := client.Analyze(path, priority)
		return analysisMsg{runID: runID, a: a, err: err}
	}
}

func (m Model) inspectKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc":
		m.screen = screenBrowse
		m.status = ""
		m.statusErr = false
		return m, nil
	case "a", "right":
		if m.algoIdx < len(m.algos) {
			m.algoIdx++
		}
		return m, nil
	case "A", "left":
		if m.algoIdx > 0 {
			m.algoIdx--
		}
		return m, nil
	case "]":
		if m.level < 9 {
			m.level++
		}
		return m, nil
	case "[":
		if m.level > 0 {
			m.level--
		}
		return m, nil
	case "p":
		m.priority = (m.priority + 1) % len(priorities)
		m.analyzing = true
		return m, tea.Batch(m.analyzeCmd(m.target.Path), m.spinner.Tick)
	case "t":
		m.threads++
		return m, nil
	case "T":
		if m.threads > 0 {
			m.threads--
		}
		return m, nil
	case "k":
		m.chunkIdx = (m.chunkIdx + 1) % len(chunkChoices)
		return m, nil
	case "o":
		m.startEditing(0)
		return m, nil
	case "w":
		m.startEditing(1)
		return m, nil
	case "c":
		return m.startRun("compress")
	case "d":
		return m.startRun("decompress")
	case "b":
		return m.startBench()
	case "i":
		return m.startInfo()
	}
	return m, nil
}

func (m *Model) startEditing(which int) {
	m.editing = which
	m.inputs[which].Focus()
	if which == 0 && m.inputs[0].Value() == "" {
		m.inputs[0].SetValue(m.defaultOutput())
	}
}

func (m Model) runRequest() core.RunRequest {
	return core.RunRequest{
		Input:     m.target.Path,
		Output:    strings.TrimSpace(m.inputs[0].Value()),
		Algorithm: m.algoID(),
		Level:     m.level,
		Priority:  priorities[m.priority],
		Password:  m.inputs[1].Value(),
		Threads:   m.threads,
		ChunkSize: chunkChoices[m.chunkIdx],
	}
}

func (m Model) startRun(kind string) (tea.Model, tea.Cmd) {
	if m.target.Path == "" {
		m.setStatus("select a file first", true)
		return m, nil
	}
	m.runID++
	m.running = true
	m.runKind = kind
	m.progress = core.Progress{}
	m.result = core.Result{}
	m.status = ""
	m.statusErr = false
	m.screen = screenRun
	m.vp.GotoTop()
	m.progressCh = make(chan core.Progress, 64)

	client := m.client
	req := m.runRequest()
	if m.pendingForce {
		req.Force = true // the user pressed c/d a second time: confirmed
		m.pendingForce = false
	}
	ch := m.progressCh
	runID := m.runID
	report := func(p core.Progress) {
		select {
		case ch <- p:
		default:
		}
	}

	var work tea.Cmd
	if kind == "compress" {
		work = func() tea.Msg {
			res, err := client.Compress(req, report)
			close(ch)
			return resultMsg{runID: runID, kind: "compression", res: res, err: err}
		}
	} else {
		work = func() tea.Msg {
			res, err := client.Decompress(req, report)
			close(ch)
			return resultMsg{runID: runID, kind: "decompression", res: res, err: err}
		}
	}
	return m, tea.Batch(work, m.waitProgress(runID), m.spinner.Tick)
}

func (m Model) waitProgress(runID int) tea.Cmd {
	ch := m.progressCh
	if ch == nil {
		return nil
	}
	return func() tea.Msg {
		p, ok := <-ch
		if !ok {
			return nil
		}
		return progressMsg{runID: runID, p: p}
	}
}

func (m Model) startBench() (tea.Model, tea.Cmd) {
	m.runID++
	m.running = true
	m.runKind = "benchmark"
	m.bench = nil
	m.status = ""
	m.statusErr = false
	m.screen = screenBench
	m.vp.GotoTop()
	m.progressCh = make(chan core.Progress, 64)

	client := m.client
	path := m.target.Path
	level := m.level
	ch := m.progressCh
	runID := m.runID
	report := func(p core.Progress) {
		select {
		case ch <- p:
		default:
		}
	}
	work := func() tea.Msg {
		res, err := client.Benchmark(path, level, 8<<20, report)
		close(ch)
		return resultMsg{runID: runID, kind: "benchmark", res: res, err: err}
	}
	return m, tea.Batch(work, m.waitProgress(runID), m.spinner.Tick)
}

func (m Model) startInfo() (tea.Model, tea.Cmd) {
	m.runID++
	m.running = true
	m.status = ""
	m.statusErr = false
	m.screen = screenInfo
	m.vp.GotoTop()
	client := m.client
	path := m.target.Path
	runID := m.runID
	return m, tea.Batch(func() tea.Msg {
		info, err := client.Info(path)
		return infoMsg{runID: runID, info: info, err: err}
	}, m.spinner.Tick)
}

// ----------------------------------------------------------------------- view

func (m Model) View() string {
	width := max(20, m.width)
	head := header(m.screenName(), m.breadcrumb(), m.shortVersion(), width)

	var body string
	overflow := false
	if m.screen == screenBrowse {
		body = m.viewBrowse()
	} else {
		content := m.screenContent()
		vp := m.vp
		vp.Width = width
		vp.Height = m.contentHeight()
		vp.SetContent(content)
		body = vp.View()
		overflow = len(strings.Split(content, "\n")) > m.contentHeight()
	}

	status := ""
	if m.status != "" {
		badge := okBadge.Render("✓")
		if m.statusErr {
			badge = errBadge.Render("✗")
		}
		status = badge + " " + clip(sanitize(m.status), width-3)
	}
	hints := m.keyHintsFor()
	if overflow && m.screen == screenInspect {
		// Scrolling is essential when content overflows: give the hint a
		// priority slot so narrow terminals keep it.
		hints = append(hints[:2], append([]string{keyHint("↑↓", "scroll")}, hints[2:]...)...)
	}
	keys := fitKeyBar(hints, width)
	return head + "\n" + body + "\n" + status + "\n" + keys
}

func (m Model) screenName() string {
	switch m.screen {
	case screenBrowse:
		return "browse"
	case screenInspect:
		return "inspect"
	case screenRun:
		return m.runKind
	case screenInfo:
		return "archive"
	case screenBench:
		return "benchmark"
	case screenHelp:
		return "help"
	}
	return ""
}

func (m Model) breadcrumb() string {
	if m.screen == screenBrowse {
		return m.cwd
	}
	return m.target.Path
}

// keyHintsFor lists the screen's hints in priority order: fitKeyBar keeps as
// many as fit, so narrow terminals lose the least important hints first.
func (m Model) keyHintsFor() []string {
	switch m.screen {
	case screenBrowse:
		return []string{keyHint("↑↓", "move"), keyHint("enter", "open"), keyHint("?", "help"),
			keyHint("q", "quit"), keyHint("bksp", "up"), keyHint(".", "hidden"), keyHint("r", "refresh")}
	case screenInspect:
		return []string{keyHint("c", "compress"), keyHint("d", "decompress"), keyHint("esc", "back"),
			keyHint("?", "keys"), keyHint("b", "bench"), keyHint("i", "info"),
			keyHint("o", "output"), keyHint("w", "password")}
	case screenRun, screenInfo, screenBench:
		return []string{keyHint("esc", "back"), keyHint("↑↓", "scroll"), keyHint("q", "quit")}
	case screenHelp:
		return []string{keyHint("?/esc", "close"), keyHint("q", "quit")}
	}
	return nil
}

func fitKeyBar(hints []string, width int) string {
	sep := dimStyle.Render("  ·  ")
	sepW := ansi.StringWidth(sep)
	var kept []string
	used := 0
	for _, h := range hints {
		add := ansi.StringWidth(h)
		if len(kept) > 0 {
			add += sepW
		}
		if used+add > width {
			break
		}
		kept = append(kept, h)
		used += add
	}
	return strings.Join(kept, sep)
}

func (m Model) screenContent() string {
	switch m.screen {
	case screenInspect:
		return m.viewInspect()
	case screenRun:
		return m.viewRun()
	case screenInfo:
		return m.viewInfo()
	case screenBench:
		return m.viewBench()
	case screenHelp:
		return m.viewHelp()
	}
	return ""
}

// -------------------------------------------------------------------- browse

func (m Model) viewBrowse() string {
	rows := m.visibleRows()
	offset := m.windowOffset()
	inner := max(16, max(20, m.width)-4)
	nameW := max(8, inner-2-4-1-10-1)

	lines := make([]string, 0, rows)
	if len(m.entries) == 0 {
		lines = append(lines, dimStyle.Render("(empty directory)"))
	}
	for i := offset; i < len(m.entries) && i < offset+rows; i++ {
		entry := m.entries[i]
		name := sanitize(entry.Name)
		kind := "file"
		if entry.IsDir {
			kind = "dir "
			name += "/"
		} else if entry.IsArchive {
			kind = "arc "
		}
		size := core.HumanSize(uint64(max64(entry.Size)))
		nameClipped := padRight(clip(name, nameW), nameW)
		if i == m.cursor {
			raw := fmt.Sprintf("▸ %s %s %s", kind, nameClipped, padLeft(size, 10))
			lines = append(lines, selectedStyle.Render(padRight(raw, inner)))
			continue
		}
		kindStyled := dimStyle.Render(kind)
		nameStyled := textStyle.Render(nameClipped)
		if entry.IsDir {
			kindStyled = accentStyle.Render(kind)
			nameStyled = accentStyle.Render(nameClipped)
		} else if entry.IsArchive {
			kindStyled = warnStyle.Render(kind)
		}
		lines = append(lines, fmt.Sprintf("  %s %s %s", kindStyled, nameStyled, dimStyle.Render(padLeft(size, 10))))
	}
	right := ""
	if len(m.entries) > 0 {
		right = fmt.Sprintf("%d/%d", m.cursor+1, len(m.entries))
	}
	return panel("browse", right, strings.Join(lines, "\n"), max(20, m.width), m.contentHeight(), true)
}

func max64(v int64) int64 {
	if v < 0 {
		return 0
	}
	return v
}

// ratioBar renders "0.3671 ████▌···········": the number plus a 16-cell bar
// with sub-cell precision (fractional block glyphs), scaled so the worst
// ratio fills the track. Lower ratio = shorter bar = better compression.
func ratioBar(ratio, maxRatio float64) string {
	const cells = 16
	frac := []rune{'▏', '▎', '▍', '▌', '▋', '▊', '▉'}
	used := 0.0
	if maxRatio > 0 {
		used = ratio / maxRatio * cells
	}
	if used > cells {
		used = cells
	}
	full := int(used)
	rest := int((used - float64(full)) * 8)
	bar := strings.Repeat("█", full)
	if rest > 0 && full < cells {
		bar += string(frac[rest-1])
		full++
	}
	bar += strings.Repeat("·", cells-full)
	return padRight(fmt.Sprintf("%7.4f ", ratio)+bar, 24)
}

// ------------------------------------------------------------------- inspect

func (m Model) viewInspect() string {
	width := max(20, m.width)
	wide := width >= 100
	leftW, rightW := width, width
	if wide {
		leftW = width/2 - 1
		rightW = width - leftW - 2
	}
	aInner := leftW - 4  // the analyzer panel's real inner width
	sInner := rightW - 4 // the settings panel's real inner width

	var a strings.Builder
	if m.analyzing {
		a.WriteString("  " + m.spinner.View() + " " + dimStyle.Render("sampling and trialling…"))
	} else if m.analyzeErr != "" {
		a.WriteString("  " + badStyle.Render(clip(sanitize(m.analyzeErr), aInner-2)))
	} else {
		an := m.analysis
		a.WriteString(row("size", core.HumanSize(an.Size), "", aInner) + "\n")
		a.WriteString(row("type", fmt.Sprintf("%s (%s)", an.Type, sanitize(an.TypeDetail)), "", aInner) + "\n")
		a.WriteString(row("entropy", fmt.Sprintf("%.4f bits/byte", an.Entropy), "", aInner) + "\n")
		a.WriteString(row("compressible", fmt.Sprintf("%.1f%%", an.Compressibility*100), "", aInner) + "\n")
		verdict := an.RecommendedAlgo
		if an.StoreRaw {
			verdict += "  (stored raw)"
		}
		a.WriteString(row("recommends", accentStyle.Render(verdict), "", aInner) + "\n")
		if len(an.Trials) > 0 {
			a.WriteString("  " + dimStyle.Render("sample trials") + "\n")
			best := an.Trials[0].Ratio
			for _, t := range an.Trials {
				if t.Ratio < best {
					best = t.Ratio
				}
			}
			for _, t := range an.Trials {
				line := fmt.Sprintf("    %-12s %7.4f %8.2f ms", t.Algo, t.Ratio, t.ElapsedMS)
				if t.Ratio == best {
					line = accentStyle.Render(line)
				} else {
					line = subtleStyle.Render(line)
				}
				a.WriteString(clip(line, aInner) + "\n")
			}
		}
		if an.Rationale != "" {
			// Wrap to the panel's own width, at word boundaries.
			wrapped := ansi.Wordwrap(sanitize(an.Rationale), max(10, aInner-4), "")
			for _, ln := range strings.Split(wrapped, "\n") {
				a.WriteString("  " + dimStyle.Render(ln) + "\n")
			}
		}
	}

	var s strings.Builder
	s.WriteString(row("algorithm", m.algoName(), "a/A", sInner) + "\n")
	s.WriteString(row("level", m.levelText(), "[ ]", sInner) + "\n")
	s.WriteString(row("priority", priorities[m.priority], "p", sInner) + "\n")
	s.WriteString(row("threads", m.threadsText(), "t/T", sInner) + "\n")
	s.WriteString(row("chunk size", m.chunkText(), "k", sInner) + "\n")
	if m.editing == 0 {
		s.WriteString(clip("  "+labelStyle.Render("output")+m.inputs[0].View(), sInner) + "\n")
	} else {
		s.WriteString(row("output", clipLeft(sanitize(m.outputText()), sInner-20), "o", sInner) + "\n")
	}
	if m.editing == 1 {
		s.WriteString(clip("  "+labelStyle.Render("password")+m.inputs[1].View(), sInner) + "\n")
	} else {
		state := dimStyle.Render("(none — not encrypted)")
		if m.passwordSet() {
			state = accentStyle.Render("set") + dimStyle.Render(" · ChaCha20 + HMAC")
		}
		s.WriteString(row("password", state, "w", sInner) + "\n")
	}

	aContent := strings.TrimSuffix(a.String(), "\n")
	sContent := strings.TrimSuffix(s.String(), "\n")

	// Equalize the CONTENT heights so the two panels' bottom borders land on
	// the same row (padding after the panel would just add dead space below).
	ah := len(strings.Split(aContent, "\n"))
	sh := len(strings.Split(sContent, "\n"))
	if ah < sh {
		aContent += strings.Repeat("\n", sh-ah)
	} else if sh < ah {
		sContent += strings.Repeat("\n", ah-sh)
	}

	if wide {
		h := max(ah, sh) + 2
		if h < m.contentHeight() {
			h = m.contentHeight()
		}
		left := panel("analyzer", "", aContent, leftW, h, false)
		right := panel("settings", "", sContent, rightW, h, m.editing >= 0)
		return lipgloss.JoinHorizontal(lipgloss.Top, left, "  ", right)
	}
	return panelAuto("analyzer", "", aContent, width, false) + "\n" +
		panelAuto("settings", "", sContent, width, m.editing >= 0)
}

// ----------------------------------------------------------------------- run

func (m Model) viewRun() string {
	width := max(20, m.width)
	inner := width - 4
	p := m.progress
	pct := 0.0
	if p.ChunksTotal > 0 {
		pct = float64(p.ChunksDone) / float64(p.ChunksTotal)
	}

	var b strings.Builder
	if m.running {
		b.WriteString("  " + m.spinner.View() + " " + dimStyle.Render(m.runKind+"ing…") + "\n")
	}
	b.WriteString("  " + m.bar.ViewAs(pct) + fmt.Sprintf(" %5.1f%%", pct*100) + "\n")
	if m.running {
		b.WriteString("\n")
		b.WriteString(row("progress", fmt.Sprintf("%d/%d chunks", p.ChunksDone, p.ChunksTotal), "", inner) + "\n")
		b.WriteString(row("input", core.HumanSize(p.BytesIn), "", inner) + "\n")
		b.WriteString(row("output", core.HumanSize(p.BytesOut), "", inner) + "\n")
		b.WriteString(row("ratio", fmt.Sprintf("%.4f", p.Ratio), "", inner) + "\n")
		b.WriteString(row("throughput", core.HumanRate(p.ThroughputMBps), "", inner) + "\n")
		b.WriteString(row("elapsed", fmt.Sprintf("%.1f ms", p.ElapsedMS), "", inner) + "\n")
	}

	if !m.running && m.result.Stats.Algorithm != "" {
		s := m.result.Stats
		b.WriteString("\n  " + accentStyle.Render("result") + "\n")
		b.WriteString(row("algorithm", fmt.Sprintf("%s (%s) level %d", s.Algorithm, s.AlgorithmName, s.Level), "", inner) + "\n")
		b.WriteString(row("original", core.HumanSize(s.OrigSize), "", inner) + "\n")
		b.WriteString(row("compressed", core.HumanSize(s.CompSize), "", inner) + "\n")
		saved := ""
		if s.OrigSize > 0 {
			saved = fmt.Sprintf("  (%.1f%% saved)", (1-s.Ratio)*100)
		}
		b.WriteString(row("ratio", accentStyle.Render(fmt.Sprintf("%.4f", s.Ratio))+dimStyle.Render(saved), "", inner) + "\n")
		b.WriteString(row("chunks", fmt.Sprintf("%d x %s on %d threads", s.Chunks, core.HumanSize(s.ChunkSize), s.Threads), "", inner) + "\n")
		b.WriteString(row("throughput", core.HumanRate(s.ThroughputMBps), "", inner) + "\n")
		b.WriteString(row("elapsed", fmt.Sprintf("%.1f ms", s.ElapsedMS), "", inner) + "\n")
		if s.Restoring && s.RestoredCRC != "" {
			state := okBadge.Render("✓ match")
			if !s.Verified {
				state = errBadge.Render("✗ MISMATCH")
			}
			b.WriteString(row("checksum", fmt.Sprintf("%s recorded → %s restored  %s", s.OrigCRC, s.RestoredCRC, state), "", inner) + "\n")
		} else {
			b.WriteString(row("checksum", s.OrigCRC+" recorded in the header", "", inner) + "\n")
		}
		b.WriteString(row("encrypted", encryptionText(s.Encrypted, s.Authenticated), "", inner) + "\n")
		b.WriteString(row("output", clipLeft(sanitize(s.Output), inner-18), "", inner) + "\n")
	}
	if !m.running && !m.result.OK && m.result.Error != "" {
		b.WriteString("\n  " + badStyle.Render(clip(sanitize(m.result.Error), inner-2)) + "\n")
		if m.pendingForce {
			b.WriteString("  " + warnStyle.Render("press c or d again to overwrite") + "\n")
		}
	}
	title := m.runKind
	if title == "" {
		title = "run"
	}
	return panelFill(title, sanitize(m.target.Name), strings.TrimSuffix(b.String(), "\n"), width, m.contentHeight(), m.running)
}

func encryptionText(encrypted, authenticated bool) string {
	if !encrypted {
		return dimStyle.Render("no")
	}
	if authenticated {
		return accentStyle.Render("yes") + dimStyle.Render(" · ChaCha20 + PBKDF2 · HMAC-SHA256 authenticated")
	}
	return warnStyle.Render("yes · legacy v1, NOT authenticated")
}

// ---------------------------------------------------------------------- info

func (m Model) viewInfo() string {
	width := max(20, m.width)
	inner := width - 4
	var b strings.Builder
	if m.running {
		b.WriteString("  " + m.spinner.View() + " " + dimStyle.Render("reading the header and checksumming the payload…"))
		return panelFill("archive", sanitize(m.target.Name), b.String(), width, m.contentHeight(), true)
	}
	if m.archiveErr != "" {
		b.WriteString("  " + badStyle.Render(clip(sanitize(m.archiveErr), inner-2)))
		return panelFill("archive", sanitize(m.target.Name), b.String(), width, m.contentHeight(), false)
	}
	if m.archive == nil {
		return panelFill("archive", sanitize(m.target.Name), dimStyle.Render("  no archive read yet"), width, m.contentHeight(), false)
	}
	a := *m.archive
	b.WriteString(row("version", fmt.Sprintf("%d", a.Version), "", inner) + "\n")
	b.WriteString(row("source", fmt.Sprintf("%s (%s / %s)", sanitize(a.Source), a.Type, sanitize(a.TypeDetail)), "", inner) + "\n")
	b.WriteString(row("algorithm", fmt.Sprintf("%s (%s) level %d", a.Algorithm, a.AlgorithmName, a.Level), "", inner) + "\n")
	b.WriteString(row("original", core.HumanSize(a.OrigSize), "", inner) + "\n")
	b.WriteString(row("payload", core.HumanSize(a.CompSize), "", inner) + "\n")
	ratio := 1.0
	if a.OrigSize > 0 {
		ratio = float64(a.CompSize) / float64(a.OrigSize)
	}
	b.WriteString(row("ratio", fmt.Sprintf("%.4f", ratio), "", inner) + "\n")
	b.WriteString(row("chunks", fmt.Sprintf("%d x %s", a.ChunkCount, core.HumanSize(a.ChunkSize)), "", inner) + "\n")
	b.WriteString(row("crc32", a.OrigCRC, "", inner) + "\n")
	if a.Encrypted {
		enc := fmt.Sprintf("yes · PBKDF2-HMAC-SHA256, %d iterations", a.KDFIterations)
		if a.Authenticated {
			b.WriteString(row("encrypted", accentStyle.Render(enc+" · HMAC authenticated"), "", inner) + "\n")
		} else {
			b.WriteString(row("encrypted", warnStyle.Render(enc+" · legacy v1, NOT authenticated"), "", inner) + "\n")
		}
	} else {
		b.WriteString(row("encrypted", dimStyle.Render("no"), "", inner) + "\n")
	}
	headerState := okBadge.Render("✓ consistent")
	if !a.HeaderConsistent {
		headerState = errBadge.Render("✗ INCONSISTENT")
	}
	b.WriteString(row("header", headerState, "", inner) + "\n")
	payloadState := okBadge.Render("✓ checksum ok")
	if !a.PayloadCRCOK {
		payloadState = errBadge.Render("✗ CHECKSUM MISMATCH")
	}
	b.WriteString(row("payload", payloadState+" "+dimStyle.Render("("+core.HumanSize(a.PayloadRead)+" read back)"), "", inner))
	return panelFill("archive", sanitize(m.target.Name), b.String(), width, m.contentHeight(), false)
}

// --------------------------------------------------------------------- bench

func (m Model) viewBench() string {
	width := max(20, m.width)
	inner := width - 4
	var b strings.Builder
	if m.running {
		pct := 0.0
		if m.progress.ChunksTotal > 0 {
			pct = float64(m.progress.ChunksDone) / float64(m.progress.ChunksTotal)
		}
		b.WriteString("  " + m.spinner.View() + " " + dimStyle.Render(fmt.Sprintf("running %d/%d algorithms…", m.progress.ChunksDone, m.progress.ChunksTotal)) + "\n")
		b.WriteString("  " + m.bar.ViewAs(pct))
		return panelFill("benchmark", sanitize(m.target.Name), b.String(), width, m.contentHeight(), true)
	}
	if len(m.bench) == 0 {
		b.WriteString(dimStyle.Render("  no results"))
		return panelFill("benchmark", sanitize(m.target.Name), b.String(), width, m.contentHeight(), false)
	}
	maxRatio := 0.0
	for _, r := range m.bench {
		if r.Ratio > maxRatio {
			maxRatio = r.Ratio
		}
	}
	showBar := inner >= 72
	if showBar {
		b.WriteString(dimStyle.Render(fmt.Sprintf("  %-12s  %-24s  %10s  %12s  %s",
			"algorithm", "ratio", "output", "throughput", "note")) + "\n")
	} else {
		b.WriteString(dimStyle.Render(fmt.Sprintf("  %-12s  %7s  %10s  %12s  %s",
			"algorithm", "ratio", "output", "throughput", "note")) + "\n")
	}
	for _, r := range m.bench {
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
		out := padLeft(core.HumanSize(r.OutputBytes), 10)
		thr := padLeft(core.HumanRate(r.ThroughputMBps), 12)
		var line string
		if showBar {
			line = fmt.Sprintf("  %-12s  %-24s  %s  %s  %s", r.Algo, ratioBar(r.Ratio, maxRatio), out, thr, note)
		} else {
			line = fmt.Sprintf("  %-12s  %7.4f  %s  %s  %s", r.Algo, r.Ratio, out, thr, note)
		}
		switch {
		case r.BestRatio:
			line = accentStyle.Render(line)
		case r.BestSpeed:
			line = infoStyle.Render(line)
		default:
			line = subtleStyle.Render(line)
		}
		b.WriteString(clip(line, inner) + "\n")
	}
	b.WriteString("\n  " + dimStyle.Render("ratio = compressed / original; lower is better"))
	return panelFill("benchmark", sanitize(m.target.Name), strings.TrimSuffix(b.String(), "\n"), width, m.contentHeight(), false)
}

// ---------------------------------------------------------------------- help

func (m Model) viewHelp() string {
	width := max(20, m.width)
	inner := width - 4
	k := func(key, action string) string {
		return clip("    "+keyStyle.Render(padRight(key, 14))+actionStyle.Render(action), inner)
	}
	section := func(name string) string { return "  " + titleStyle.Render(name) }
	var b strings.Builder
	intro := "kolma drives a pluggable compression engine. This interface is a client of the kolma binary: every screen is a real command, and every number comes from the engine's JSON output."
	for _, ln := range strings.Split(ansi.Wordwrap(intro, max(10, inner-2), ""), "\n") {
		b.WriteString("  " + dimStyle.Render(ln) + "\n")
	}
	b.WriteString("\n" + section("browse") + "\n")
	b.WriteString(k("↑/↓ · j/k", "move") + "\n")
	b.WriteString(k("enter · l", "open a directory, or select a file and analyse it") + "\n")
	b.WriteString(k("backspace", "go up a directory") + "\n")
	b.WriteString(k(". · r · g/G", "hidden files · refresh · top/bottom") + "\n")
	b.WriteString("\n" + section("inspect") + "\n")
	b.WriteString(k("a / A", "cycle the algorithm (auto lets the Analyzer choose)") + "\n")
	b.WriteString(k("[ / ]", "level, 1 fastest .. 9 best ratio (0 = auto)") + "\n")
	b.WriteString(k("p · t/T · k", "priority · threads · chunk size") + "\n")
	b.WriteString(k("o · w", "edit the output path · set a password") + "\n")
	b.WriteString(k("c · d", "compress · decompress") + "\n")
	b.WriteString(k("b · i", "benchmark every method · read an archive's header") + "\n")
	b.WriteString("\n" + section("run · archive · benchmark") + "\n")
	b.WriteString(k("esc", "back to the file") + "\n")
	b.WriteString(k("↑/↓ · wheel", "scroll") + "\n")
	b.WriteString("\n" + section("everywhere") + "\n")
	b.WriteString(k("? · q", "help · quit") + "\n")
	b.WriteString("\n  " + dimStyle.Render(clip(sanitize(m.version), inner-2)) + "\n")
	b.WriteString("  " + dimStyle.Render(clip("archives: format v2, CRC-32 verified; encrypted archives carry an HMAC-SHA256 tag", inner-2)))
	return b.String()
}

// Run starts the program.
func Run(client core.Client, version string) error {
	model := New(client, version)
	program := tea.NewProgram(model, tea.WithAltScreen(), tea.WithMouseCellMotion())
	_, err := program.Run()
	return err
}
