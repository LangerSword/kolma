// Headless tests for the interface model: no terminal required. They exercise
// the same Update path the real program uses, which is where the state bugs
// would live.
package ui

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"

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
	m.cwd = dir
	m.refresh()
	return m, dir
}

func key(s string) tea.KeyMsg {
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
		next, _ := model.Update(key(k))
		model = next
	}
	out, ok := model.(Model)
	if !ok {
		t.Fatalf("Update returned %T, want ui.Model", model)
	}
	return out
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
	if m.cursor != 1 {
		t.Fatalf("cursor should be 1 after one down, got %d", m.cursor)
	}
	m = send(t, m, "G")
	if m.cursor != len(m.entries)-1 {
		t.Fatalf("G should jump to the last entry, got %d", m.cursor)
	}
	m = send(t, m, "g")
	if m.cursor != 0 {
		t.Fatalf("g should jump to the first entry, got %d", m.cursor)
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
	next, _ := m.Update(key("c"))
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
		next, cmd := model.Update(key("q"))
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
	if m.visibleRows() < 3 {
		t.Fatalf("visible rows should never drop below 3, got %d", m.visibleRows())
	}
	view := m.View()
	if strings.TrimSpace(view) == "" {
		t.Fatal("nothing rendered after a resize")
	}
}
