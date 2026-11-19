// Headless tests for the interface model: no terminal required. They exercise
// the same Update path the real program uses, which is where the state bugs
// would live.
package ui

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/charmbracelet/bubbles/cursor"
	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/ansi"

	"github.com/LangerSword/kolma/tui/internal/core"
)

func newTestModel(t *testing.T) (Model, string) {
	t.Helper()
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "subdir"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "a.txt"), []byte("hello world hello world"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "b.kolma"), []byte("not really an archive"), 0o644); err != nil {
		t.Fatal(err)
	}
	m := New(core.Client{Bin: "/nonexistent/kolma"}, "kolma 0.1.0")
	m.width, m.height = 120, 40
	m.list.SetSize(120, 37)
	m.help.Width = 120
	m.cwd = dir
	m.refresh()
	return m, dir
}

func keyMsg(s string) tea.KeyMsg {
	switch s {
	case "down":
		return tea.KeyMsg{Type: tea.KeyDown}
	case "up":
		return tea.KeyMsg{Type: tea.KeyUp}
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}
	case "esc":
		return tea.KeyMsg{Type: tea.KeyEscape}
	case "backspace":
		return tea.KeyMsg{Type: tea.KeyBackspace}
	case "tab":
		return tea.KeyMsg{Type: tea.KeyTab}
	case "right":
		return tea.KeyMsg{Type: tea.KeyRight}
	case "left":
		return tea.KeyMsg{Type: tea.KeyLeft}
	default:
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
	}
}

func send(t *testing.T, m Model, keys ...string) Model {
	t.Helper()
	var model tea.Model = m
	for _, k := range keys {
		next, _ := model.Update(keyMsg(k))
		model = next
	}
	out, ok := model.(Model)
	if !ok {
		t.Fatalf("Update returned %T, want ui.Model", model)
	}
	return out
}

// drainCmds executes the commands an Update returned and feeds their messages
// back, the way bubbletea's runtime does. Needed for async flows like the
// list's fuzzy filter, whose matches arrive as a FilterMatchesMsg command.
// The spinner's infinite tick chain is deliberately not chased.
func drainCmds(m Model, cmd tea.Cmd) Model {
	for cmd != nil {
		msg := cmd()
		if msg == nil {
			return m
		}
		if batch, ok := msg.(tea.BatchMsg); ok {
			for _, c := range batch {
				m = drainCmds(m, c)
			}
			return m
		}
		if _, ok := msg.(spinner.TickMsg); ok {
			return m
		}
		if _, ok := msg.(cursor.BlinkMsg); ok {
			return m // don't chase the text cursor's blink chain either
		}
		next, c := m.Update(msg)
		m = next.(Model)
		cmd = c
	}
	return m
}

func sendDrain(t *testing.T, m Model, keys ...string) Model {
	t.Helper()
	for _, k := range keys {
		next, cmd := m.Update(keyMsg(k))
		m = drainCmds(next.(Model), cmd)
	}
	return m
}

func TestBrowseListsDirectoriesFirst(t *testing.T) {
	m, _ := newTestModel(t)
	if len(m.entries) != 3 {
		t.Fatalf("expected 3 entries (a subdirectory and two files), got %d", len(m.entries))
	}
	if !m.entries[0].IsDir {
		t.Fatalf("directories should sort first, got %q", m.entries[0].Name)
	}
	if !m.entries[2].IsArchive {
		t.Fatalf("the .kolma file should be marked as an archive, got %+v", m.entries[2])
	}
	view := m.View()
	if !strings.Contains(view, "subdir") || !strings.Contains(view, "a.txt") {
		t.Fatalf("the browser view is missing entries:\n%s", view)
	}
}

func TestNavigationAndHiddenToggle(t *testing.T) {
	m, dir := newTestModel(t)
	if err := os.WriteFile(filepath.Join(dir, ".dotfile"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	m = send(t, m, ".")
	if len(m.entries) != 4 {
		t.Fatalf("hidden files should be visible after toggling, got %d entries", len(m.entries))
	}
	m = send(t, m, ".")
	if len(m.entries) != 3 {
		t.Fatalf("hidden files should be hidden again, got %d entries", len(m.entries))
	}

	m = send(t, m, "down")
	if m.list.Index() != 1 {
		t.Fatalf("cursor should be 1 after one down, got %d", m.list.Index())
	}
	m = send(t, m, "G")
	if m.list.Index() != len(m.entries)-1 {
		t.Fatalf("G should jump to the last entry, got %d", m.list.Index())
	}
	m = send(t, m, "g")
	if m.list.Index() != 0 {
		t.Fatalf("g should jump to the first entry, got %d", m.list.Index())
	}
}

func TestEnteringADirectoryChangesTheWorkingDirectory(t *testing.T) {
	m, dir := newTestModel(t)
	m = send(t, m, "enter")
	if m.cwd != filepath.Join(dir, "subdir") {
		t.Fatalf("cwd is %q, want the subdirectory", m.cwd)
	}
	m = send(t, m, "backspace")
	if m.cwd != dir {
		t.Fatalf("backspace did not return to the parent, cwd is %q", m.cwd)
	}
}

func TestSelectingAFileOpensTheInspectScreen(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter") // a.txt is the second entry
	if m.screen != screenInspect {
		t.Fatalf("screen is %v, want inspect", m.screen)
	}
	if m.target.Name != "a.txt" {
		t.Fatalf("target is %q, want a.txt", m.target.Name)
	}
	if !m.analyzing {
		t.Fatal("selecting a file should start an analysis")
	}
	view := m.View()
	if !strings.Contains(view, "analyzer") || !strings.Contains(view, "settings") {
		t.Fatalf("the inspect view is missing sections:\n%s", view)
	}
}

func TestSettingsCycleWithinBounds(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")

	// Level: clamped to 0..9, where 0 means auto.
	for i := 0; i < 12; i++ {
		m = send(t, m, "]")
	}
	if m.level != 9 {
		t.Fatalf("level should clamp at 9, got %d", m.level)
	}
	for i := 0; i < 12; i++ {
		m = send(t, m, "[")
	}
	if m.level != 0 {
		t.Fatalf("level should clamp at 0 (auto), got %d", m.level)
	}

	// Priority cycles through exactly three values.
	start := m.priority
	seen := map[int]bool{}
	for i := 0; i < 3; i++ {
		m = send(t, m, "p")
		seen[m.priority] = true
	}
	if m.priority != start {
		t.Fatalf("priority did not return to %d after three cycles, got %d", start, m.priority)
	}
	if len(seen) != 3 {
		t.Fatalf("priority should visit 3 distinct values, visited %d", len(seen))
	}

	// Threads: never negative.
	for i := 0; i < 5; i++ {
		m = send(t, m, "T")
	}
	if m.threads != 0 {
		t.Fatalf("threads should clamp at 0 (auto), got %d", m.threads)
	}
	m = send(t, m, "t", "t")
	if m.threads != 2 {
		t.Fatalf("threads should be 2, got %d", m.threads)
	}

	// Algorithm cycling without a registry must not panic or overflow.
	m = send(t, m, "a", "a")
	if m.algoIdx < 0 {
		t.Fatalf("algorithm index went negative: %d", m.algoIdx)
	}
	m = send(t, m, "A", "A", "A")
	if m.algoIdx != 0 {
		t.Fatalf("algorithm index should clamp at 0 (auto), got %d", m.algoIdx)
	}

	// Chunk size cycles through the fixed list.
	for i := 0; i < len(chunkChoices); i++ {
		m = send(t, m, "k")
	}
	if m.chunkIdx != 0 {
		t.Fatalf("chunk index should wrap to 0, got %d", m.chunkIdx)
	}
}

func TestTextEditingTakesOverTheKeyboard(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter", "o")
	if m.editing != 0 {
		t.Fatalf("editing should be the output field, got %d", m.editing)
	}
	if !strings.HasSuffix(m.inputs[0].Value(), ".kolma") {
		t.Fatalf("the output field should be prefilled, got %q", m.inputs[0].Value())
	}
	// While editing, "a" is a character, not an algorithm change.
	before := m.algoIdx
	m = send(t, m, "a")
	if m.algoIdx != before {
		t.Fatal("a key press while editing changed the algorithm")
	}
	m = send(t, m, "esc")
	if m.editing != -1 {
		t.Fatalf("esc should leave editing mode, got %d", m.editing)
	}

	m = send(t, m, "w")
	if m.editing != 1 {
		t.Fatalf("w should edit the password, got %d", m.editing)
	}
	m = send(t, m, "tab")
	if m.editing != 0 {
		t.Fatalf("tab should move to the output field, got %d", m.editing)
	}
	m = send(t, m, "enter")
	if m.editing != -1 {
		t.Fatal("enter should commit the edit")
	}
}

func TestOutputAndRequestDerivation(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")
	if !strings.HasSuffix(m.outputText(), "a.txt.kolma") {
		t.Fatalf("derived output path is %q", m.outputText())
	}
	req := m.runRequest()
	if req.Algorithm != "auto" {
		t.Fatalf("default algorithm should be auto, got %q", req.Algorithm)
	}
	if req.Priority != "balanced" {
		t.Fatalf("default priority should be balanced, got %q", req.Priority)
	}
	if req.Input != m.target.Path {
		t.Fatalf("request input is %q, want %q", req.Input, m.target.Path)
	}
}

func TestRunningWithoutASelectionIsRefused(t *testing.T) {
	m := New(core.Client{Bin: "/nonexistent/kolma"}, "kolma 0.1.0")
	m.screen = screenInspect
	next, _ := m.Update(keyMsg("c"))
	model := next.(Model)
	if model.running {
		t.Fatal("a run started with no file selected")
	}
	if !strings.Contains(model.status, "select a file") {
		t.Fatalf("expected a status message about selecting a file, got %q", model.status)
	}
}

func TestProgressAndResultMessagesUpdateTheModel(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")
	m.running = true
	m.screen = screenRun
	m.runID = 7

	next, _ := m.Update(progressMsg{runID: 7, p: core.Progress{BytesIn: 1024, ChunksDone: 1, ChunksTotal: 4}})
	m = next.(Model)
	if m.progress.ChunksDone != 1 {
		t.Fatalf("progress was not applied: %+v", m.progress)
	}

	// A stale run id must be ignored.
	next, _ = m.Update(progressMsg{runID: 6, p: core.Progress{ChunksDone: 4, ChunksTotal: 4}})
	m = next.(Model)
	if m.progress.ChunksDone != 1 {
		t.Fatalf("a stale progress message was applied: %+v", m.progress)
	}

	next, _ = m.Update(resultMsg{runID: 7, kind: "compression", res: core.Result{
		OK: true,
		Stats: core.Stats{Algorithm: "lz77", AlgorithmName: "LZ77 (LZSS)", Level: 6,
			OrigSize: 2048, CompSize: 512, Ratio: 0.25, Chunks: 4, ChunkSize: 512,
			Threads: 2, OrigCRC: "deadbeef"},
	}})
	m = next.(Model)
	if m.running {
		t.Fatal("the model still reports running after a result")
	}
	view := m.View()
	for _, want := range []string{"lz77", "0.2500", "deadbeef"} {
		if !strings.Contains(view, want) {
			t.Fatalf("the result view is missing %q:\n%s", want, view)
		}
	}
}

func TestErrorResultIsSurfaced(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")
	m.running = true
	m.runID = 3
	next, _ := m.Update(resultMsg{runID: 3, kind: "compression", res: core.Result{
		OK: false, Error: "archive is encrypted; a password is required",
	}})
	m = next.(Model)
	if !m.statusErr || !strings.Contains(m.status, "password") {
		t.Fatalf("the error was not surfaced: %q (err=%v)", m.status, m.statusErr)
	}
}

func TestEveryScreenRenders(t *testing.T) {
	m, _ := newTestModel(t)
	m.analysis = core.Analysis{
		Path: "a.txt", Size: 11, Type: "Text", TypeDetail: "text", Entropy: 3.2,
		Compressibility: 0.7, RecommendedAlgo: "lz77", RecommendedLevel: 6,
		Rationale: "text-like input", Trials: []core.Trial{{Algo: "lz77", Ratio: 0.4, ElapsedMS: 1.2}},
	}
	m.archive = &core.ArchiveInfo{Version: 1, Source: "a.txt", Algorithm: "lz77", OrigSize: 11,
		CompSize: 5, ChunkCount: 1, ChunkSize: 11, OrigCRC: "deadbeef", HeaderConsistent: true,
		PayloadCRCOK: true, PayloadRead: 5}
	m.bench = []core.BenchmarkRow{{Algo: "lz77", Ratio: 0.4, OutputBytes: 4, ThroughputMBps: 20, BestRatio: true}}
	m.result = core.Result{OK: true, Stats: core.Stats{Algorithm: "lz77", Ratio: 0.4, OrigSize: 11, CompSize: 4}}

	for _, screen := range []screen{screenBrowse, screenInspect, screenRun, screenInfo, screenBench, screenHelp} {
		m.screen = screen
		view := m.View()
		if strings.TrimSpace(view) == "" {
			t.Fatalf("screen %v rendered nothing", screen)
		}
		if strings.Contains(view, "%!") {
			t.Fatalf("screen %v has a formatting error:\n%s", screen, view)
		}
	}
}

func TestHelpTogglesAndQuitKeys(t *testing.T) {
	m, _ := newTestModel(t)

	// From the browser, help must return to the browser.
	m = send(t, m, "?")
	if m.screen != screenHelp {
		t.Fatalf("? should open help, got %v", m.screen)
	}
	m = send(t, m, "?")
	if m.screen != screenBrowse {
		t.Fatalf("closing help should return where it was opened, got %v", m.screen)
	}

	// From the inspect screen, esc must also return there.
	m = send(t, m, "down", "enter")
	if m.screen != screenInspect {
		t.Fatalf("expected the inspect screen, got %v", m.screen)
	}
	m = send(t, m, "?")
	if m.screen != screenHelp {
		t.Fatalf("? should open help, got %v", m.screen)
	}
	m = send(t, m, "esc")
	if m.screen != screenInspect {
		t.Fatalf("esc should close help back to inspect, got %v", m.screen)
	}
	if m = send(t, m, "esc"); m.screen != screenBrowse {
		t.Fatalf("esc should return to the browser, got %v", m.screen)
	}

	// q quits from every screen, including the middle of the workflow.
	for _, start := range []screen{screenBrowse, screenInspect, screenRun, screenInfo, screenBench} {
		model := m
		model.screen = start
		next, cmd := model.Update(keyMsg("q"))
		if cmd == nil {
			t.Fatalf("q on screen %v did not produce a command", start)
		}
		if _, ok := cmd().(tea.QuitMsg); !ok {
			t.Fatalf("q on screen %v produced %T, want a quit message", start, cmd())
		}
		_ = next
	}
}

func TestWindowResizeIsHonoured(t *testing.T) {
	m, _ := newTestModel(t)
	next, _ := m.Update(tea.WindowSizeMsg{Width: 60, Height: 20})
	m = next.(Model)
	if m.width != 60 || m.height != 20 {
		t.Fatalf("size is %dx%d, want 60x20", m.width, m.height)
	}
	view := m.View()
	if strings.TrimSpace(view) == "" {
		t.Fatal("nothing rendered after a resize")
	}
}

// ------------------------------------------------------------- regressions --

func TestBenchmarkResultsAreSurfaced(t *testing.T) {
	// Regression: resultMsg used to drop the benchmark rows on the floor, so
	// the benchmark screen showed "no results" forever after a real run.
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")
	m.running = true
	m.screen = screenBench
	m.runID = 5
	next, _ := m.Update(resultMsg{runID: 5, kind: "benchmark", res: core.Result{
		OK: true,
		Benchmark: []core.BenchmarkRow{
			{Algo: "lz77", Ratio: 0.25, OutputBytes: 100, ThroughputMBps: 20, BestRatio: true},
			{Algo: "store", Ratio: 1.0, OutputBytes: 400, ThroughputMBps: 900, BestSpeed: true},
		},
	}})
	m = next.(Model)
	if len(m.bench) != 2 {
		t.Fatalf("benchmark rows were not stored: %d", len(m.bench))
	}
	view := m.View()
	if !strings.Contains(view, "lz77") || !strings.Contains(view, "best ratio") {
		t.Fatalf("the benchmark view is missing its rows:\n%s", view)
	}
	if strings.Contains(view, "no results") {
		t.Fatalf("the benchmark view still says no results:\n%s", view)
	}
}

func TestOverwriteNeedsASecondPress(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "enter")
	m.running = true
	m.runID = 9
	next, _ := m.Update(resultMsg{runID: 9, kind: "compression", res: core.Result{
		OK: false, Error: "output already exists: /tmp/a.txt.kolma (use --force to overwrite)",
	}})
	m = next.(Model)
	if !m.pendingForce {
		t.Fatal("an 'already exists' failure should arm the overwrite confirmation")
	}
	if !strings.Contains(m.status, "overwrite") {
		t.Fatalf("the status should explain the second press, got %q", m.status)
	}
	// The second c press consumes the confirmation and starts the run.
	m = send(t, m, "c")
	if m.pendingForce {
		t.Fatal("the confirmation should be consumed by the second press")
	}
	if !m.running {
		t.Fatal("the second press should start the run")
	}
}

func TestArchiveOutputDefaultStripsTheSuffix(t *testing.T) {
	m, dir := newTestModel(t)
	// b.kolma is the third entry; select it.
	m = send(t, m, "G", "enter")
	if !m.target.IsArchive {
		t.Fatalf("expected the archive entry, got %q", m.target.Name)
	}
	want := filepath.Join(dir, "b")
	if m.outputText() != want {
		t.Fatalf("derived output is %q, want %q", m.outputText(), want)
	}
	m = send(t, m, "o")
	if m.inputs[0].Value() != want {
		t.Fatalf("prefilled output is %q, want %q", m.inputs[0].Value(), want)
	}
}

func TestSanitizeNeutralizesControlCharacters(t *testing.T) {
	got := sanitize("a\nb\x1b[31m\x00c")
	if got != "a?b?[31m?c" {
		t.Fatalf("sanitize produced %q", got)
	}
	if sanitize("plain.txt") != "plain.txt" {
		t.Fatal("sanitize should leave clean names alone")
	}
}

func TestMouseWheelMovesTheBrowserCursor(t *testing.T) {
	m, _ := newTestModel(t)
	next, _ := m.Update(tea.MouseMsg{Button: tea.MouseButtonWheelDown, Action: tea.MouseActionPress})
	m = next.(Model)
	if m.list.Index() != 2 {
		t.Fatalf("wheel down should move the cursor by 3 (clamped), got %d", m.list.Index())
	}
	next, _ = m.Update(tea.MouseMsg{Button: tea.MouseButtonWheelUp, Action: tea.MouseActionPress})
	m = next.(Model)
	if m.list.Index() != 0 {
		t.Fatalf("wheel up should return to the top, got %d", m.list.Index())
	}
}

// TestPanelBordersAreExact checks the panel primitive directly, without the
// viewport: every line of a panel must be exactly the requested width. The
// viewport silently clips overflow, so only a direct check catches a border
// that overhangs its own body.
func TestPanelBordersAreExact(t *testing.T) {
	for _, w := range []int{20, 40, 59, 60, 80, 120} {
		for _, right := range []string{"", "2/3", "sample.txt"} {
			p := panel("title", right, "line one\nline two", w, 5, false)
			for i, ln := range strings.Split(p, "\n") {
				if got := ansi.StringWidth(ln); got != w {
					t.Fatalf("panel(width=%d, right=%q) line %d has width %d, want %d:\n%q",
						w, right, i, got, w, ln)
				}
			}
		}
	}
}

func TestFilterNarrowsAndOpensTheMatch(t *testing.T) {
	m, _ := newTestModel(t)
	m = sendDrain(t, m, "/")
	if !m.list.SettingFilter() {
		t.Fatal("/ should open the filter input")
	}
	m = sendDrain(t, m, "a", ".", "t")
	if len(m.list.VisibleItems()) != 1 {
		t.Fatalf("filter 'a.t' should leave 1 visible item, got %d", len(m.list.VisibleItems()))
	}
	m = sendDrain(t, m, "enter") // accept the filter
	if m.list.SettingFilter() {
		t.Fatal("enter should accept the filter and close the input")
	}
	m = send(t, m, "enter") // open the filtered selection
	if m.screen != screenInspect || m.target.Name != "a.txt" {
		t.Fatalf("enter on the filtered item should inspect a.txt, got screen=%v target=%q", m.screen, m.target.Name)
	}
}

func TestFilterEscapeRestoresAllItems(t *testing.T) {
	m, _ := newTestModel(t)
	m = sendDrain(t, m, "/", "z", "z", "z")
	if len(m.list.VisibleItems()) != 0 {
		t.Fatalf("filter 'zzz' should match nothing, got %d", len(m.list.VisibleItems()))
	}
	m = sendDrain(t, m, "esc")
	if m.list.SettingFilter() {
		t.Fatal("esc should cancel the filter input")
	}
	if len(m.list.VisibleItems()) != 3 {
		t.Fatalf("cancel should restore all items, got %d", len(m.list.VisibleItems()))
	}
}

func TestBenchTableMovesWithKeys(t *testing.T) {
	m, _ := newTestModel(t)
	m.bench = []core.BenchmarkRow{
		{Algo: "lz77", Ratio: 0.25, BestRatio: true},
		{Algo: "lzw", Ratio: 0.4},
		{Algo: "store", Ratio: 1.0, BestSpeed: true},
	}
	m.syncBenchTable()
	m.screen = screenBench
	m = send(t, m, "down")
	if m.benchTable.Cursor() != 1 {
		t.Fatalf("down should move the table cursor to 1, got %d", m.benchTable.Cursor())
	}
	view := m.View()
	if !strings.Contains(view, "dictionary") && !strings.Contains(view, "lzw") {
		t.Fatalf("the bench view should show the table and the selected row's notes:\n%s", view)
	}
}

func TestEmptyDirectoryShowsAHint(t *testing.T) {
	m, dir := newTestModel(t)
	m.cwd = filepath.Join(dir, "subdir")
	m.refresh()
	view := m.viewBrowse()
	if !strings.Contains(view, "empty directory") {
		t.Fatalf("an empty directory should say so:\n%s", view)
	}
}

// ------------------------------------------------------- bundles / extract --

func TestSpaceMarksAndSelectionGathers(t *testing.T) {
	m, _ := newTestModel(t)
	// Mark the first two entries (subdir, a.txt); space steps down each time.
	m = send(t, m, " ", " ")
	if len(m.marked) != 2 {
		t.Fatalf("expected 2 marked entries, got %d", len(m.marked))
	}
	sel := m.selection()
	if len(sel) != 2 {
		t.Fatalf("selection should gather the 2 marked entries, got %d", len(sel))
	}
	if !strings.Contains(m.breadcrumb(), "2 selected") {
		t.Fatalf("the header should show the selection count, got %q", m.breadcrumb())
	}
	if !strings.Contains(m.View(), "●") {
		t.Fatalf("marked rows should render a mark indicator:\n%s", m.View())
	}
	// esc clears the selection.
	m = send(t, m, "esc")
	if len(m.marked) != 0 {
		t.Fatalf("esc should clear the selection, got %d", len(m.marked))
	}
}

func TestCompressingAFolderBundlesIt(t *testing.T) {
	m, dir := newTestModel(t)
	// The first entry is the subdirectory; c on it bundles straight away.
	m = send(t, m, "c")
	if m.screen != screenRun {
		t.Fatalf("compressing a folder should go to the run screen, got %v", m.screen)
	}
	if len(m.bundleInputs) != 1 || m.bundleInputs[0] != filepath.Join(dir, "subdir") {
		t.Fatalf("bundleInputs should hold the folder, got %v", m.bundleInputs)
	}
	if m.runKind != "compress" {
		t.Fatalf("runKind is %q, want compress", m.runKind)
	}
	if !strings.HasSuffix(m.inputs[0].Value(), "subdir.kolma") {
		t.Fatalf("bundle output should be named after the folder, got %q", m.inputs[0].Value())
	}
}

func TestCompressingSeveralMarkedItemsBundlesThem(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, " ", " ") // mark subdir + a.txt
	m = send(t, m, "c")
	if len(m.bundleInputs) != 2 {
		t.Fatalf("expected a 2-item bundle, got %v", m.bundleInputs)
	}
	if m.screen != screenRun {
		t.Fatalf("expected the run screen, got %v", m.screen)
	}
}

func TestCompressingALoneFileOpensInspect(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "c") // a.txt
	if m.screen != screenInspect {
		t.Fatalf("a lone file should open inspect (analyzer evidence), got %v", m.screen)
	}
	if len(m.bundleInputs) != 0 {
		t.Fatalf("a lone file must not be treated as a bundle, got %v", m.bundleInputs)
	}
}

func TestBundleSubtitleSurvivesResult(t *testing.T) {
	// Regression: the success handler clears bundleInputs, which used to wipe
	// the "N items -> one archive" subtitle before the result view rendered.
	m, _ := newTestModel(t)
	m = send(t, m, " ", " ") // mark subdir + a.txt
	m = send(t, m, "c")      // start the bundle run
	if m.runItems != 2 {
		t.Fatalf("runItems should be 2, got %d", m.runItems)
	}
	next, _ := m.Update(resultMsg{runID: m.runID, kind: "compression", res: core.Result{
		OK: true, Stats: core.Stats{Algorithm: "lz77", Ratio: 0.4, OrigSize: 100, CompSize: 40},
	}})
	m = next.(Model)
	if len(m.bundleInputs) != 0 {
		t.Fatal("the selection should be cleared after a successful run")
	}
	if !strings.Contains(m.View(), "2 items → one archive") {
		t.Fatalf("the bundle subtitle should survive the result:\n%s", m.View())
	}
}

func TestExtractingAnArchiveTargetsItsDirectory(t *testing.T) {
	m, dir := newTestModel(t)
	m = send(t, m, "G", "x") // b.kolma is last
	if m.runKind != "extract" {
		t.Fatalf("runKind is %q, want extract", m.runKind)
	}
	if m.screen != screenRun {
		t.Fatalf("expected the run screen, got %v", m.screen)
	}
	if m.inputs[0].Value() != dir {
		t.Fatalf("extract destination should default to the archive's dir %q, got %q", dir, m.inputs[0].Value())
	}
}

func TestExtractRefusesNonArchives(t *testing.T) {
	m, _ := newTestModel(t)
	m = send(t, m, "down", "x") // a.txt is not an archive
	if m.running {
		t.Fatal("extract should not start on a non-archive")
	}
	if !strings.Contains(m.status, "archive") {
		t.Fatalf("expected a hint about selecting an archive, got %q", m.status)
	}
}

func TestInfoShowsBundleContents(t *testing.T) {
	m, _ := newTestModel(t)
	m.archive = &core.ArchiveInfo{Version: 2, Source: "proj", Algorithm: "lz77", OrigSize: 100,
		CompSize: 40, ChunkCount: 1, ChunkSize: 100, OrigCRC: "deadbeef", HeaderConsistent: true,
		PayloadCRCOK: true, PayloadRead: 40}
	m.contents = []core.BundleEntry{
		{Path: "proj/", IsDir: true},
		{Path: "proj/a.txt", Size: 60},
		{Path: "proj/sub/b.txt", Size: 40},
	}
	m.screen = screenInfo
	view := m.View()
	if !strings.Contains(view, "contents (3)") || !strings.Contains(view, "proj/a.txt") {
		t.Fatalf("the info screen should list the bundle contents:\n%s", view)
	}
}

// TestEveryScreenFitsTheTerminal is the permanent "not broken" guard: at every
// supported size, no rendered line may be wider than the terminal and no frame
// may be taller than it. Overflow is what makes a TUI feel broken.
func TestEveryScreenFitsTheTerminal(t *testing.T) {
	sizes := []struct{ w, h int }{{120, 40}, {100, 30}, {80, 24}, {60, 20}}
	for _, sz := range sizes {
		m, _ := newTestModel(t)
		next, _ := m.Update(tea.WindowSizeMsg{Width: sz.w, Height: sz.h})
		m = next.(Model)
		m.analysis = core.Analysis{
			Path: "a.txt", Size: 11, Type: "Text", TypeDetail: "text", Entropy: 3.2,
			Compressibility: 0.7, RecommendedAlgo: "lz77", RecommendedLevel: 6,
			Rationale: "text-like input with a fairly long rationale that should wrap instead of overflowing the panel",
			Trials: []core.Trial{
				{Algo: "lz77", Ratio: 0.4, ElapsedMS: 1.2},
				{Algo: "bwt-mtf-ari", Ratio: 0.3, ElapsedMS: 9.9},
			},
		}
		m.archive = &core.ArchiveInfo{Version: 2, Source: "a.txt", Algorithm: "lz77", OrigSize: 11,
			CompSize: 5, ChunkCount: 1, ChunkSize: 11, OrigCRC: "deadbeef", HeaderConsistent: true,
			PayloadCRCOK: true, PayloadRead: 5, Encrypted: true, Authenticated: true, KDFIterations: 200000}
		m.bench = []core.BenchmarkRow{
			{Algo: "bwt-mtf-ari", Ratio: 0.3, OutputBytes: 3, ThroughputMBps: 7, BestRatio: true},
			{Algo: "lz77", Ratio: 0.4, OutputBytes: 4, ThroughputMBps: 20},
			{Algo: "store", Ratio: 1.0, OutputBytes: 11, ThroughputMBps: 3000, BestSpeed: true},
		}
		m.syncBenchTable()
		m.result = core.Result{OK: true, Stats: core.Stats{Algorithm: "lz77", AlgorithmName: "LZ77 (LZSS)",
			Ratio: 0.4, OrigSize: 11, CompSize: 4, Level: 6, Chunks: 1, ChunkSize: 11, Threads: 16,
			OrigCRC: "deadbeef", Encrypted: true, Authenticated: true, Output: "/tmp/a.txt.kolma"}}
		m.progress = core.Progress{BytesIn: 11, BytesOut: 4, ChunksDone: 1, ChunksTotal: 1, Ratio: 0.4, ThroughputMBps: 20}
		m.status = "compression finished: ratio 0.4000"

		for _, screen := range []screen{screenBrowse, screenInspect, screenRun, screenInfo, screenBench, screenHelp} {
			m.screen = screen
			// Raw content first: the viewport clips overflow, which would hide
			// a too-wide panel from the final-frame check below.
			raw := m.viewBrowse()
			if screen != screenBrowse {
				raw = m.screenContent()
			}
			for i, ln := range strings.Split(raw, "\n") {
				if w := ansi.StringWidth(ln); w > sz.w {
					t.Fatalf("%dx%d screen %v raw line %d: width %d overflows:\n%q",
						sz.w, sz.h, screen, i, w, ln)
				}
			}
			view := m.View()
			lines := strings.Split(view, "\n")
			if len(lines) > sz.h {
				t.Fatalf("%dx%d screen %v: frame is %d lines tall, overflows the terminal",
					sz.w, sz.h, screen, len(lines))
			}
			for i, ln := range lines {
				if w := ansi.StringWidth(ln); w > sz.w {
					t.Fatalf("%dx%d screen %v line %d: width %d overflows the terminal:\n%q",
						sz.w, sz.h, screen, i, w, ln)
				}
			}
		}
	}
}
