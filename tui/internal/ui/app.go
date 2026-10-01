package ui

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

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

	archive    *core.ArchiveInfo
	archiveErr string

	bench []core.BenchmarkRow

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

func (m Model) outputText() string {
	if v := strings.TrimSpace(m.inputs[0].Value()); v != "" {
		return v
	}
	if m.target.Name == "" {
		return "(no file selected)"
	}
	return m.target.Path + ".kolma"
}

func (m Model) passwordSet() bool { return m.inputs[1].Value() != "" }

func (m *Model) setStatus(msg string, isErr bool) {
	m.status = msg
	m.statusErr = isErr
}

func (m Model) visibleRows() int {
	rows := m.height - 12
	if rows < 3 {
		rows = 3
	}
	return rows
}

func (m Model) windowOffset() int {
	rows := m.visibleRows()
	if m.cursor < rows {
		return 0
	}
	return m.cursor - rows + 1
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

// --------------------------------------------------------------------- update

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil

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
		return m, nil

	case resultMsg:
		if msg.runID != m.runID {
			return m, nil
		}
		m.running = false
		m.result = msg.res
		if msg.err != nil {
			m.setStatus(msg.err.Error(), true)
		} else if !msg.res.OK {
			m.setStatus(msg.res.Error, true)
		} else {
			m.setStatus(fmt.Sprintf("%s finished: ratio %.4f", msg.kind, msg.res.Stats.Ratio), false)
			m.refresh()
		}
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)
	}
	return m, nil
}

func (m Model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	key := msg.String()

	// A text field owns the keyboard while it is being edited.
	if m.editing >= 0 {
		switch key {
		case "esc":
			m.editing = -1
			m.inputs[0].Blur()
			m.inputs[1].Blur()
			return m, nil
		case "enter":
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
		}
		return m, nil
	case "esc":
		if m.screen == screenHelp {
			m.screen = m.helpReturn
			return m, nil
		}
	case "q":
		// Quitting is always available; esc is the key that walks back.
		return m, tea.Quit
	}

	switch m.screen {
	case screenBrowse:
		return m.browseKey(key)
	case screenInspect:
		return m.inspectKey(key)
	case screenRun, screenInfo, screenBench:
		if key == "esc" {
			m.screen = screenInspect
			return m, nil
		}
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
		return m, m.analyzeCmd(entry.Path)
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
	m.runID++
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
		return m, m.analyzeCmd(m.target.Path)
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
		m.inputs[0].SetValue(m.target.Path + ".kolma")
	}
}

func (m Model) runRequest() core.RunRequest {
	chunk := chunkChoices[m.chunkIdx]
	if chunk == 0 {
		chunk = 0
	}
	return core.RunRequest{
		Input:     m.target.Path,
		Output:    strings.TrimSpace(m.inputs[0].Value()),
		Algorithm: m.algoID(),
		Level:     m.level,
		Priority:  priorities[m.priority],
		Password:  m.inputs[1].Value(),
		Threads:   m.threads,
		ChunkSize: chunk,
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
	m.screen = screenRun
	m.progressCh = make(chan core.Progress, 64)

	client := m.client
	req := m.runRequest()
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
	return m, tea.Batch(work, m.waitProgress(runID))
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
	m.screen = screenBench
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
	return m, tea.Batch(work, m.waitProgress(runID))
}

func (m Model) startInfo() (tea.Model, tea.Cmd) {
	m.runID++
	m.running = true
	m.screen = screenInfo
	client := m.client
	path := m.target.Path
	runID := m.runID
	return m, func() tea.Msg {
		info, err := client.Info(path)
		return infoMsg{runID: runID, info: info, err: err}
	}
}

// ----------------------------------------------------------------------- view

func (m Model) View() string {
	var b strings.Builder
	switch m.screen {
	case screenBrowse:
		b.WriteString(m.viewBrowse())
	case screenInspect:
		b.WriteString(m.viewInspect())
	case screenRun:
		b.WriteString(m.viewRun())
	case screenInfo:
		b.WriteString(m.viewInfo())
	case screenBench:
		b.WriteString(m.viewBench())
	case screenHelp:
		b.WriteString(m.viewHelp())
	}
	if m.status != "" {
		style := accentValueStyle
		if m.statusErr {
			style = badStyle
		}
		b.WriteString("\n" + style.Render(truncate(m.status, m.width-2)) + "\n")
	} else {
		b.WriteString("\n")
	}
	b.WriteString(m.footerFor())
	return b.String()
}

func truncate(s string, width int) string {
	if width <= 0 || len(s) <= width {
		return s
	}
	if width <= 1 {
		return s[:width]
	}
	return s[:width-1] + "…"
}

func (m Model) footerFor() string {
	switch m.screen {
	case screenBrowse:
		return footer("↑/↓ move · enter open · backspace up · . hidden · r refresh · q quit")
	case screenInspect:
		return footer("a/A algorithm · [ ] level · p priority · t/T threads · k chunk · o output · w password · c compress · d decompress · b bench · i info · esc back")
	case screenRun:
		return footer("esc back to the file")
	case screenInfo, screenBench:
		return footer("esc back")
	case screenHelp:
		return footer("? or esc to close · q to quit")
	}
	return ""
}

func (m Model) viewBrowse() string {
	var b strings.Builder
	b.WriteString(header("browse", m.cwd) + "\n\n")

	rows := m.visibleRows()
	offset := m.windowOffset()
	if len(m.entries) == 0 {
		b.WriteString(dimStyle.Render("  (empty directory)") + "\n")
	}
	for i := offset; i < len(m.entries) && i < offset+rows; i++ {
		entry := m.entries[i]
		marker := "  "
		if i == m.cursor {
			marker = cursorStyle.Render("> ")
		}
		kind := "file"
		if entry.IsDir {
			kind = "dir "
		} else if entry.IsArchive {
			kind = "arc "
		}
		size := core.HumanSize(uint64(max64(entry.Size)))
		line := fmt.Sprintf("%s%-4s %-44s %10s", marker, kind, truncate(entry.Name, 44), size)
		if i == m.cursor {
			line = selectedStyle.Render(fmt.Sprintf("%s %-4s %-44s %10s", ">", kind, truncate(entry.Name, 44), size))
		}
		b.WriteString(line + "\n")
	}
	if len(m.entries) > rows {
		b.WriteString(dimStyle.Render(fmt.Sprintf("  %d/%d", m.cursor+1, len(m.entries))) + "\n")
	}
	return b.String()
}

func max64(v int64) int64 {
	if v < 0 {
		return 0
	}
	return v
}

func (m Model) viewInspect() string {
	var b strings.Builder
	b.WriteString(header("inspect", m.target.Path) + "\n\n")

	// --- the Analyzer's verdict
	b.WriteString(accentValueStyle.Render("analyzer") + "\n")
	if m.analyzing {
		b.WriteString(dimStyle.Render("  sampling and trialling algorithms…") + "\n")
	} else if m.analyzeErr != "" {
		b.WriteString(badStyle.Render("  "+truncate(m.analyzeErr, m.width-4)) + "\n")
	} else {
		a := m.analysis
		row := func(label, value string) string {
			return "  " + labelStyle.Render(label) + valueStyle.Render(value) + "\n"
		}
		b.WriteString(row("size", core.HumanSize(a.Size)))
		b.WriteString(row("type", fmt.Sprintf("%s (%s)", a.Type, a.TypeDetail)))
		b.WriteString(row("entropy", fmt.Sprintf("%.4f bits/byte", a.Entropy)))
		b.WriteString(row("compressible", fmt.Sprintf("%.1f%%", a.Compressibility*100)))
		verdict := a.RecommendedAlgo
		if a.StoreRaw {
			verdict += "  (flagged incompressible: stored raw)"
		}
		b.WriteString(row("recommends", verdict))
		if len(a.Trials) > 0 {
			b.WriteString("  " + dimStyle.Render("sample trials:") + "\n")
			for _, t := range a.Trials {
				b.WriteString(fmt.Sprintf("    %-14s ratio %-9.4f %8.2f ms\n", t.Algo, t.Ratio, t.ElapsedMS))
			}
		}
		if a.Rationale != "" {
			b.WriteString("  " + dimStyle.Render(truncate(a.Rationale, m.width-4)) + "\n")
		}
	}

	// --- the User's settings
	b.WriteString("\n" + accentValueStyle.Render("settings") + "\n")
	setting := func(label, value string) string {
		return "  " + labelStyle.Render(label) + valueStyle.Render(value) + "\n"
	}
	b.WriteString(setting("algorithm", m.algoName()))
	b.WriteString(setting("level", m.levelText()))
	b.WriteString(setting("priority", priorities[m.priority]))
	b.WriteString(setting("threads", m.threadsText()))
	b.WriteString(setting("chunk size", m.chunkText()))
	if m.editing == 0 {
		b.WriteString("  " + labelStyle.Render("output") + m.inputs[0].View() + "\n")
	} else {
		b.WriteString(setting("output", truncate(m.outputText(), m.width-20)))
	}
	if m.editing == 1 {
		b.WriteString("  " + labelStyle.Render("password") + m.inputs[1].View() + "\n")
	} else {
		state := "(none: the archive will not be encrypted)"
		if m.passwordSet() {
			state = "set (ChaCha20 + PBKDF2-HMAC-SHA256)"
		}
		b.WriteString(setting("password", state))
	}
	return b.String()
}

func (m Model) viewRun() string {
	var b strings.Builder
	b.WriteString(header(m.runKind, m.target.Path) + "\n\n")
	p := m.progress
	bar := progressBar(p.ChunksDone, p.ChunksTotal, min(60, m.width-10))
	b.WriteString("  " + bar + "\n\n")
	row := func(label, value string) string {
		return "  " + labelStyle.Render(label) + valueStyle.Render(value) + "\n"
	}
	b.WriteString(row("progress", fmt.Sprintf("%d/%d chunks", p.ChunksDone, p.ChunksTotal)))
	b.WriteString(row("input", core.HumanSize(p.BytesIn)))
	b.WriteString(row("output", core.HumanSize(p.BytesOut)))
	b.WriteString(row("ratio", fmt.Sprintf("%.4f", p.Ratio)))
	b.WriteString(row("throughput", core.HumanRate(p.ThroughputMBps)))
	b.WriteString(row("elapsed", fmt.Sprintf("%.1f ms", p.ElapsedMS)))

	if !m.running && m.result.Stats.Algorithm != "" {
		b.WriteString("\n" + accentValueStyle.Render("result") + "\n")
		s := m.result.Stats
		b.WriteString(row("algorithm", fmt.Sprintf("%s (%s) level %d", s.Algorithm, s.AlgorithmName, s.Level)))
		b.WriteString(row("original", core.HumanSize(s.OrigSize)))
		b.WriteString(row("compressed", core.HumanSize(s.CompSize)))
		saved := "(1-ratio)*100"
		if s.OrigSize > 0 {
			saved = fmt.Sprintf("%.1f%% of the original saved", (1-s.Ratio)*100)
		}
		b.WriteString(row("ratio", fmt.Sprintf("%.4f  %s", s.Ratio, saved)))
		b.WriteString(row("chunks", fmt.Sprintf("%d x %s on %d threads", s.Chunks, core.HumanSize(s.ChunkSize), s.Threads)))
		if s.RestoredCRC != "" {
			state := "match"
			if !s.Verified {
				state = "MISMATCH"
			}
			b.WriteString(row("checksum", fmt.Sprintf("%s -> %s %s", s.OrigCRC, s.RestoredCRC, state)))
		} else {
			b.WriteString(row("checksum", s.OrigCRC+" recorded"))
		}
		b.WriteString(row("encrypted", fmt.Sprintf("%v", s.Encrypted)))
		b.WriteString(row("output", s.Output))
	}
	return b.String()
}

func progressBar(done, total uint32, width int) string {
	if width < 10 {
		width = 10
	}
	filled := 0
	if total > 0 {
		filled = int(float64(done) / float64(total) * float64(width))
	}
	if filled > width {
		filled = width
	}
	return accentValueStyle.Render(strings.Repeat("█", filled)) +
		dimStyle.Render(strings.Repeat("░", width-filled)) +
		fmt.Sprintf(" %5.1f%%", func() float64 {
			if total == 0 {
				return 0
			}
			return float64(done) / float64(total) * 100
		}())
}

func (m Model) viewInfo() string {
	var b strings.Builder
	b.WriteString(header("archive", m.target.Path) + "\n\n")
	if m.running {
		b.WriteString(dimStyle.Render("  reading the header and checksumming the payload…") + "\n")
		return b.String()
	}
	if m.archiveErr != "" {
		b.WriteString(badStyle.Render("  "+truncate(m.archiveErr, m.width-4)) + "\n")
		return b.String()
	}
	if m.archive == nil {
		return b.String()
	}
	a := *m.archive
	row := func(label, value string) string {
		return "  " + labelStyle.Render(label) + valueStyle.Render(value) + "\n"
	}
	b.WriteString(row("version", fmt.Sprintf("%d", a.Version)))
	b.WriteString(row("source", fmt.Sprintf("%s (%s / %s)", a.Source, a.Type, a.TypeDetail)))
	b.WriteString(row("algorithm", fmt.Sprintf("%s (%s) level %d", a.Algorithm, a.AlgorithmName, a.Level)))
	b.WriteString(row("original", core.HumanSize(a.OrigSize)))
	b.WriteString(row("payload", core.HumanSize(a.CompSize)))
	ratio := 1.0
	if a.OrigSize > 0 {
		ratio = float64(a.CompSize) / float64(a.OrigSize)
	}
	b.WriteString(row("ratio", fmt.Sprintf("%.4f", ratio)))
	b.WriteString(row("chunks", fmt.Sprintf("%d x %s", a.ChunkCount, core.HumanSize(a.ChunkSize))))
	b.WriteString(row("crc32", a.OrigCRC))
	if a.Encrypted {
		b.WriteString(row("encrypted", fmt.Sprintf("yes (PBKDF2-HMAC-SHA256, %d iterations)", a.KDFIterations)))
	} else {
		b.WriteString(row("encrypted", "no"))
	}
	headerState := accentValueStyle.Render("consistent")
	if !a.HeaderConsistent {
		headerState = badStyle.Render("INCONSISTENT")
	}
	b.WriteString(row("header", headerState))
	payloadState := accentValueStyle.Render("checksum ok")
	if !a.PayloadCRCOK {
		payloadState = badStyle.Render("CHECKSUM MISMATCH")
	}
	b.WriteString(row("payload", payloadState+" "+dimStyle.Render("("+core.HumanSize(a.PayloadRead)+" read back)")))
	return b.String()
}

func (m Model) viewBench() string {
	var b strings.Builder
	b.WriteString(header("benchmark", m.target.Path) + "\n\n")
	if m.running {
		b.WriteString(dimStyle.Render(fmt.Sprintf("  running %d/%d algorithms…", m.progress.ChunksDone, m.progress.ChunksTotal)) + "\n")
		return b.String()
	}
	if len(m.bench) == 0 {
		b.WriteString(dimStyle.Render("  no results") + "\n")
		return b.String()
	}
	b.WriteString(fmt.Sprintf("  %-14s %-9s %-11s %-13s %s\n", "algorithm", "ratio", "output", "throughput", "note"))
	for _, row := range m.bench {
		note := ""
		if row.BestRatio {
			note = "best ratio"
		}
		if row.BestSpeed {
			if note != "" {
				note += ", "
			}
			note += "fastest"
		}
		line := fmt.Sprintf("  %-14s %-9.4f %-11s %-13s %s", row.Algo, row.Ratio,
			core.HumanSize(row.OutputBytes), core.HumanRate(row.ThroughputMBps), note)
		if row.BestRatio {
			line = accentValueStyle.Render(line)
		}
		b.WriteString(line + "\n")
	}
	return b.String()
}

func (m Model) viewHelp() string {
	body := `
  kolma drives a pluggable compression engine. This interface is a client of
  the ` + "`kolma`" + ` binary: every screen you see is a real command, and the numbers
  come from the engine's JSON output.

  browse      move with j/k or the arrows, enter opens a directory or selects
              a file, backspace goes up, . toggles hidden entries
  inspect     the Analyzer's verdict for the selected file, then your settings
              a/A cycles the algorithm (auto lets the Analyzer choose)
              [ ] adjusts the level (1 fastest .. 9 best ratio)
              p cycles the priority, t/T the threads, k the chunk size
              o edits the output path, w sets a password
              c compresses, d decompresses, b benchmarks, i reads an archive
  run         live progress, then the final statistics and checksum result

  The engine is C++20 and knows seven methods: store, rle, huffman, lz77, lzw,
  bwt-mtf-ari and delta-rle. Archives are self-describing and carry a CRC-32 of
  the original data, so a corrupted archive fails loudly instead of quietly.
`
	return header("help", "") + "\n" + valueStyle.Render(body)
}

// Run starts the program.
func Run(client core.Client, version string) error {
	model := New(client, version)
	program := tea.NewProgram(model, tea.WithAltScreen())
	_, err := program.Run()
	return err
}

var _ = lipgloss.NewStyle
