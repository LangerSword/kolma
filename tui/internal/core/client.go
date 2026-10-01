// Package core is the TUI's client for the kolma engine.
//
// It never links the C++ code: it drives the `kolma` binary and reads the
// stable JSON contract that the CLI emits (one progress object per tick on
// stderr, one final object on stdout). That keeps the engine authoritative and
// the interface replaceable.
package core

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

// ---------------------------------------------------------------- types ----

type Stats struct {
	Algorithm      string  `json:"algorithm"`
	AlgorithmName  string  `json:"algorithm_name"`
	Level          int     `json:"level"`
	OrigSize       uint64  `json:"orig_size"`
	CompSize       uint64  `json:"comp_size"`
	Ratio          float64 `json:"ratio"`
	ElapsedMS      float64 `json:"elapsed_ms"`
	ThroughputMBps float64 `json:"throughput_mbps"`
	Chunks         uint32  `json:"chunks"`
	Threads        uint64  `json:"threads"`
	ChunkSize      uint64  `json:"chunk_size"`
	OrigCRC        string  `json:"orig_crc"`
	RestoredCRC    string  `json:"restored_crc"`
	Verified       bool    `json:"verified"`
	Encrypted      bool    `json:"encrypted"`
	Output         string  `json:"output"`
}

type Trial struct {
	Algo            string  `json:"algo"`
	Name            string  `json:"name"`
	Ratio           float64 `json:"ratio"`
	ElapsedMS       float64 `json:"elapsed_ms"`
	CompressedBytes uint64  `json:"compressed_bytes"`
}

type Analysis struct {
	Path             string  `json:"path"`
	Size             uint64  `json:"size"`
	Type             string  `json:"type"`
	TypeDetail       string  `json:"type_detail"`
	Entropy          float64 `json:"entropy"`
	Compressibility  float64 `json:"compressibility"`
	RecommendedAlgo  string  `json:"recommended_algo"`
	RecommendedLevel int     `json:"recommended_level"`
	StoreRaw         bool    `json:"store_raw"`
	Rationale        string  `json:"rationale"`
	Trials           []Trial `json:"trials"`
}

type BenchmarkRow struct {
	Algo           string  `json:"algo"`
	Name           string  `json:"name"`
	Ratio          float64 `json:"ratio"`
	InputBytes     uint64  `json:"input_bytes"`
	OutputBytes    uint64  `json:"output_bytes"`
	ElapsedMS      float64 `json:"elapsed_ms"`
	ThroughputMBps float64 `json:"throughput_mbps"`
	MemoryBytes    uint64  `json:"memory_bytes"`
	BlockSize      uint64  `json:"block_size"`
	BestRatio      bool    `json:"best_ratio"`
	BestSpeed      bool    `json:"best_speed"`
}

type ArchiveInfo struct {
	OK               bool   `json:"ok"`
	Error            string `json:"error"`
	Version          int    `json:"version"`
	Source           string `json:"source"`
	Type             string `json:"type"`
	TypeDetail       string `json:"type_detail"`
	Algorithm        string `json:"algorithm"`
	AlgorithmName    string `json:"algorithm_name"`
	Level            int    `json:"level"`
	OrigSize         uint64 `json:"orig_size"`
	CompSize         uint64 `json:"comp_size"`
	ChunkCount       uint32 `json:"chunk_count"`
	ChunkSize        uint64 `json:"chunk_size"`
	Created          uint64 `json:"created"`
	OrigCRC          string `json:"orig_crc"`
	Encrypted        bool   `json:"encrypted"`
	KDFIterations    uint32 `json:"kdf_iterations"`
	HeaderConsistent bool   `json:"header_consistent"`
	PayloadCRCOK     bool   `json:"payload_crc_ok"`
	PayloadRead      uint64 `json:"payload_read"`
}

type AlgoParams struct {
	ID               string  `json:"id"`
	Name             string  `json:"name"`
	TypicalRatio     float64 `json:"typical_ratio"`
	TypicalSpeedMbps float64 `json:"typical_speed_mbps"`
	MemoryBytes      uint64  `json:"memory_bytes"`
	BlockSize        uint64  `json:"block_size"`
	DictionarySize   uint64  `json:"dictionary_size"`
	Notes            string  `json:"notes"`
}

type Progress struct {
	BytesIn        uint64  `json:"bytes_in"`
	BytesOut       uint64  `json:"bytes_out"`
	ChunksDone     uint32  `json:"chunks_done"`
	ChunksTotal    uint32  `json:"chunks_total"`
	ElapsedMS      float64 `json:"elapsed_ms"`
	ThroughputMBps float64 `json:"throughput_mbps"`
	Ratio          float64 `json:"ratio"`
}

type Result struct {
	OK        bool
	Error     string
	Stats     Stats
	Analysis  Analysis
	Benchmark []BenchmarkRow
}

// ------------------------------------------------------------------ client --

type Client struct {
	Bin string
}

// FindBinary locates the engine, preferring an explicit override.
func FindBinary(explicit string) (string, error) {
	var candidates []string
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	if env := os.Getenv("KOLMA_BIN"); env != "" {
		candidates = append(candidates, env)
	}
	if path, err := exec.LookPath("kolma"); err == nil {
		candidates = append(candidates, path)
	}
	candidates = append(candidates,
		"./build/kolma", "../build/kolma", "./kolma", "../../build/kolma")
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		candidates = append(candidates, filepath.Join(dir, "kolma"), filepath.Join(dir, "build", "kolma"))
	}
	for _, c := range candidates {
		if c == "" {
			continue
		}
		info, err := os.Stat(c)
		if err != nil || info.IsDir() {
			continue
		}
		if info.Mode()&0o111 == 0 {
			continue
		}
		abs, err := filepath.Abs(c)
		if err != nil {
			abs = c
		}
		return abs, nil
	}
	return "", errors.New("kolma engine not found: build it (cmake --build build) or set KOLMA_BIN")
}

func (c Client) Version() (string, error) {
	out, err := exec.Command(c.Bin, "version").Output()
	if err != nil {
		return "", fmt.Errorf("running %s version: %w", c.Bin, err)
	}
	return strings.TrimSpace(string(out)), nil
}

// run executes the engine and streams progress. `password` travels in the
// environment, never on the command line, so it stays out of `ps`.
func (c Client) run(args []string, password string, onProgress func(Progress)) (Result, error) {
	full := append([]string{}, args...)
	full = append(full, "--json")

	cmd := exec.Command(c.Bin, full...)
	cmd.Env = os.Environ()
	if password != "" {
		cmd.Env = append(cmd.Env, "KOLMA_PASSWORD="+password)
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return Result{}, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return Result{}, err
	}
	if err := cmd.Start(); err != nil {
		return Result{}, fmt.Errorf("starting %s: %w", c.Bin, err)
	}

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		scanner := bufio.NewScanner(stderr)
		scanner.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if !strings.HasPrefix(line, "{") {
				continue
			}
			var event struct {
				Event string `json:"event"`
				Progress
			}
			if err := json.Unmarshal([]byte(line), &event); err != nil {
				continue
			}
			if event.Event == "progress" && onProgress != nil {
				onProgress(event.Progress)
			}
		}
	}()

	raw, readErr := io.ReadAll(stdout)
	wg.Wait()
	waitErr := cmd.Wait()

	payload := strings.TrimSpace(string(raw))
	if payload == "" {
		if waitErr != nil {
			return Result{}, fmt.Errorf("%s failed: %w", strings.Join(full, " "), waitErr)
		}
		if readErr != nil {
			return Result{}, readErr
		}
		return Result{}, errors.New("the engine produced no result")
	}
	line := payload
	if idx := strings.LastIndex(payload, "\n"); idx >= 0 {
		line = payload[idx+1:]
	}
	var done struct {
		Event     string         `json:"event"`
		OK        bool           `json:"ok"`
		Error     string         `json:"error"`
		Stats     Stats          `json:"stats"`
		Analysis  Analysis       `json:"analysis"`
		Benchmark []BenchmarkRow `json:"benchmark"`
	}
	if err := json.Unmarshal([]byte(line), &done); err != nil {
		return Result{}, fmt.Errorf("unreadable engine result %q: %w", line, err)
	}
	return Result{OK: done.OK, Error: done.Error, Stats: done.Stats, Analysis: done.Analysis, Benchmark: done.Benchmark}, nil
}

// ------------------------------------------------------------------ calls ---

type RunRequest struct {
	Input     string
	Output    string
	Algorithm string
	Level     int
	Priority  string
	Password  string
	Threads   int
	ChunkSize int64
	NoVerify  bool
}

func (r RunRequest) args(mode string) []string {
	args := []string{mode, r.Input}
	if r.Output != "" {
		args = append(args, "--out", r.Output)
	}
	if r.Algorithm != "" {
		args = append(args, "--algo", r.Algorithm)
	}
	if r.Level > 0 {
		args = append(args, "--level", fmt.Sprint(r.Level))
	}
	if r.Priority != "" {
		args = append(args, "--priority", r.Priority)
	}
	if r.Threads > 0 {
		args = append(args, "--threads", fmt.Sprint(r.Threads))
	}
	if r.ChunkSize > 0 {
		args = append(args, "--chunk-size", fmt.Sprint(r.ChunkSize))
	}
	if r.NoVerify {
		args = append(args, "--no-verify")
	}
	return args
}

func (c Client) Compress(req RunRequest, onProgress func(Progress)) (Result, error) {
	return c.run(req.args("compress"), req.Password, onProgress)
}

func (c Client) Decompress(req RunRequest, onProgress func(Progress)) (Result, error) {
	return c.run(req.args("decompress"), req.Password, onProgress)
}

func (c Client) Analyze(path, priority string) (Analysis, error) {
	res, err := c.run([]string{"analyze", path, "--priority", priority}, "", nil)
	if err != nil {
		return Analysis{}, err
	}
	if !res.OK {
		if res.Error != "" {
			return Analysis{}, errors.New(res.Error)
		}
		return Analysis{}, fmt.Errorf("could not analyse %s", path)
	}
	return res.Analysis, nil
}

func (c Client) Info(path string) (ArchiveInfo, error) {
	cmd := exec.Command(c.Bin, "info", path, "--json")
	out, err := cmd.Output()
	if err != nil {
		return ArchiveInfo{}, fmt.Errorf("reading %s: %w", path, err)
	}
	var envelope struct {
		OK      bool        `json:"ok"`
		Archive ArchiveInfo `json:"archive"`
		Error   string      `json:"error"`
	}
	if err := json.Unmarshal(out, &envelope); err != nil {
		return ArchiveInfo{}, err
	}
	if !envelope.OK {
		msg := envelope.Archive.Error
		if msg == "" {
			msg = "not a kolma archive"
		}
		return ArchiveInfo{}, errors.New(msg)
	}
	return envelope.Archive, nil
}

func (c Client) Benchmark(path string, level int, limit int64, onProgress func(Progress)) (Result, error) {
	args := []string{"bench", path}
	if level > 0 {
		args = append(args, "--level", fmt.Sprint(level))
	}
	if limit > 0 {
		args = append(args, "--limit", fmt.Sprint(limit))
	}
	return c.run(args, "", onProgress)
}

func (c Client) Algorithms() ([]AlgoParams, error) {
	out, err := exec.Command(c.Bin, "algos", "--json").Output()
	if err != nil {
		return nil, fmt.Errorf("listing algorithms: %w", err)
	}
	var params []AlgoParams
	if err := json.Unmarshal(out, &params); err != nil {
		return nil, err
	}
	return params, nil
}

// ------------------------------------------------------------ filesystem ----

type Entry struct {
	Name      string
	Path      string
	IsDir     bool
	Size      int64
	IsArchive bool
}

// ListDir returns the directory contents: directories first, then files, each
// group sorted by name. Hidden entries are included only when showHidden.
func ListDir(dir string, showHidden bool) ([]Entry, error) {
	items, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var entries []Entry
	for _, item := range items {
		name := item.Name()
		if !showHidden && strings.HasPrefix(name, ".") {
			continue
		}
		info, err := item.Info()
		if err != nil {
			continue
		}
		entries = append(entries, Entry{
			Name:      name,
			Path:      filepath.Join(dir, name),
			IsDir:     item.IsDir(),
			Size:      info.Size(),
			IsArchive: strings.HasSuffix(name, ".kolma"),
		})
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].IsDir != entries[j].IsDir {
			return entries[i].IsDir
		}
		return strings.ToLower(entries[i].Name) < strings.ToLower(entries[j].Name)
	})
	return entries, nil
}

// HumanSize renders a byte count the same way the engine does.
func HumanSize(bytes uint64) string {
	const unit = 1024
	if bytes < unit {
		return fmt.Sprintf("%d B", bytes)
	}
	units := []string{"KiB", "MiB", "GiB", "TiB", "PiB"}
	value := float64(bytes)
	idx := -1
	for value >= unit && idx < len(units)-1 {
		value /= unit
		idx++
	}
	return fmt.Sprintf("%.2f %s", value, units[idx])
}

func HumanRate(mbps float64) string {
	if mbps >= 1000 {
		return fmt.Sprintf("%.2f GB/s", mbps/1000)
	}
	return fmt.Sprintf("%.2f MB/s", mbps)
}
