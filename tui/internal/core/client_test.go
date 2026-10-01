// Integration tests: these drive the real engine binary, so they fail if the
// JSON contract between the CLI and this client ever drifts.
package core

import (
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func engineBinary(t *testing.T) string {
	t.Helper()
	if p := os.Getenv("KOLMA_BIN"); p != "" {
		return p
	}
	for _, candidate := range []string{"../../../build/kolma", "../../build/kolma", "../../../build/kolma"} {
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
			abs, err := filepath.Abs(candidate)
			if err == nil {
				return abs
			}
			return candidate
		}
	}
	t.Skip("engine not built; run `cmake --build build` first")
	return ""
}

func newClient(t *testing.T) Client {
	t.Helper()
	return Client{Bin: engineBinary(t)}
}

func writeFixture(t *testing.T, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("writing fixture: %v", err)
	}
	return path
}

func repetitive(n int) []byte {
	unit := []byte("the quick brown fox jumps over the lazy dog. kolma chunks the input. ")
	out := make([]byte, 0, n)
	for len(out) < n {
		out = append(out, unit...)
	}
	return out[:n]
}

func TestVersionAndAlgorithms(t *testing.T) {
	client := newClient(t)
	version, err := client.Version()
	if err != nil {
		t.Fatalf("version: %v", err)
	}
	if !strings.Contains(version, "kolma") {
		t.Fatalf("unexpected version string %q", version)
	}
	algos, err := client.Algorithms()
	if err != nil {
		t.Fatalf("algorithms: %v", err)
	}
	if len(algos) != 7 {
		t.Fatalf("expected 7 algorithms, got %d", len(algos))
	}
	seen := map[string]bool{}
	for _, a := range algos {
		if a.ID == "" || a.Name == "" {
			t.Fatalf("algorithm entry is missing an id or name: %+v", a)
		}
		seen[a.ID] = true
	}
	for _, want := range []string{"store", "rle", "huffman", "lz77", "lzw", "bwt-mtf-ari", "delta-rle"} {
		if !seen[want] {
			t.Errorf("algorithm %q missing from the registry", want)
		}
	}
}

func TestCompressDecompressRoundTrip(t *testing.T) {
	client := newClient(t)
	data := repetitive(300000)
	input := writeFixture(t, "input.txt", data)
	archive := filepath.Join(t.TempDir(), "input.kolma")
	output := filepath.Join(t.TempDir(), "restored.txt")

	var progress []Progress
	res, err := client.Compress(RunRequest{
		Input: input, Output: archive, Algorithm: "lz77", Level: 6, Priority: "balanced", ChunkSize: 16384,
	}, func(p Progress) { progress = append(progress, p) })
	if err != nil {
		t.Fatalf("compress: %v", err)
	}
	if !res.OK {
		t.Fatalf("compress reported failure: %s", res.Error)
	}
	if res.Stats.Ratio >= 0.5 {
		t.Fatalf("repetitive text should compress well, got ratio %v", res.Stats.Ratio)
	}
	if res.Stats.OrigSize != uint64(len(data)) {
		t.Fatalf("orig size %d, want %d", res.Stats.OrigSize, len(data))
	}
	if res.Stats.Algorithm != "lz77" {
		t.Fatalf("algorithm %q, want lz77", res.Stats.Algorithm)
	}
	if len(progress) == 0 {
		t.Fatal("no progress events were delivered")
	}
	last := uint32(0)
	for i, p := range progress {
		if p.ChunksDone < last {
			t.Fatalf("progress went backwards at event %d: %d then %d", i, last, p.ChunksDone)
		}
		last = p.ChunksDone
	}
	if progress[len(progress)-1].ChunksDone != progress[len(progress)-1].ChunksTotal {
		t.Fatalf("final progress %d/%d did not reach the end",
			progress[len(progress)-1].ChunksDone, progress[len(progress)-1].ChunksTotal)
	}

	back, err := client.Decompress(RunRequest{Input: archive, Output: output}, nil)
	if err != nil {
		t.Fatalf("decompress: %v", err)
	}
	if !back.OK {
		t.Fatalf("decompress reported failure: %s", back.Error)
	}
	if !back.Stats.Verified {
		t.Fatal("the checksum was not verified")
	}
	restored, err := os.ReadFile(output)
	if err != nil {
		t.Fatalf("reading restored file: %v", err)
	}
	if string(restored) != string(data) {
		t.Fatalf("restored %d bytes, want %d (content differs)", len(restored), len(data))
	}
}

func TestAutoModePicksAndVetoes(t *testing.T) {
	client := newClient(t)
	textPath := writeFixture(t, "text.txt", repetitive(200000))
	res, err := client.Compress(RunRequest{Input: textPath, Output: textPath + ".kolma", Algorithm: "auto"}, nil)
	if err != nil {
		t.Fatalf("compress: %v", err)
	}
	if !res.OK {
		t.Fatalf("auto compress failed: %s", res.Error)
	}
	if res.Stats.Algorithm == "store" {
		t.Fatal("the Analyzer stored compressible text raw")
	}
	if len(res.Analysis.Trials) == 0 {
		t.Fatal("auto mode reported no sample trials")
	}
	if res.Analysis.Rationale == "" {
		t.Fatal("auto mode reported no rationale")
	}

	noise := make([]byte, 262144)
	if _, err := rand.New(rand.NewSource(7)).Read(noise); err != nil {
		t.Fatalf("generating noise: %v", err)
	}
	noisePath := writeFixture(t, "noise.bin", noise)
	noisy, err := client.Compress(RunRequest{Input: noisePath, Output: noisePath + ".kolma", Algorithm: "auto"}, nil)
	if err != nil {
		t.Fatalf("compress noise: %v", err)
	}
	if !noisy.OK {
		t.Fatalf("noise compress failed: %s", noisy.Error)
	}
	if !noisy.Analysis.StoreRaw {
		t.Errorf("high-entropy data was not flagged incompressible (algorithm %q)", noisy.Stats.Algorithm)
	}
}

func TestAnalyzeReportsTypeAndEntropy(t *testing.T) {
	client := newClient(t)
	path := writeFixture(t, "notes.md", repetitive(50000))
	a, err := client.Analyze(path, "balanced")
	if err != nil {
		t.Fatalf("analyze: %v", err)
	}
	if a.Type != "Text" {
		t.Errorf("type %q, want Text", a.Type)
	}
	if a.Size != 50000 {
		t.Errorf("size %d, want 50000", a.Size)
	}
	if a.Entropy <= 0 || a.Entropy > 8 {
		t.Errorf("entropy %v is out of range", a.Entropy)
	}
	if a.RecommendedAlgo == "" {
		t.Error("no recommendation")
	}
	if a.Compressibility <= 0 {
		t.Errorf("compressibility %v should be positive for text", a.Compressibility)
	}

	// A JPEG magic number must trip the veto.
	jpeg := append([]byte{0xFF, 0xD8, 0xFF, 0xE0}, make([]byte, 50000)...)
	jpegPath := writeFixture(t, "photo.jpg", jpeg)
	ja, err := client.Analyze(jpegPath, "maxratio")
	if err != nil {
		t.Fatalf("analyze jpeg: %v", err)
	}
	if !ja.StoreRaw {
		t.Errorf("an already-compressed format was not vetoed: %+v", ja)
	}
}

func TestEncryptedRoundTripAndWrongPassword(t *testing.T) {
	client := newClient(t)
	data := repetitive(120000)
	input := writeFixture(t, "secret.txt", data)
	archive := filepath.Join(t.TempDir(), "secret.kolma")
	output := filepath.Join(t.TempDir(), "secret.out")

	res, err := client.Compress(RunRequest{
		Input: input, Output: archive, Algorithm: "lz77", Password: "correct horse battery staple",
	}, nil)
	if err != nil {
		t.Fatalf("compress: %v", err)
	}
	if !res.OK || !res.Stats.Encrypted {
		t.Fatalf("expected an encrypted archive, got ok=%v encrypted=%v", res.OK, res.Stats.Encrypted)
	}
	info, err := client.Info(archive)
	if err != nil {
		t.Fatalf("info: %v", err)
	}
	if !info.Encrypted || info.KDFIterations == 0 {
		t.Fatalf("archive metadata does not show encryption: %+v", info)
	}
	if !info.HeaderConsistent || !info.PayloadCRCOK {
		t.Fatalf("archive failed its own consistency checks: %+v", info)
	}

	good, err := client.Decompress(RunRequest{Input: archive, Output: output, Password: "correct horse battery staple"}, nil)
	if err != nil {
		t.Fatalf("decompress: %v", err)
	}
	if !good.OK {
		t.Fatalf("decompress with the right password failed: %s", good.Error)
	}
	restored, err := os.ReadFile(output)
	if err != nil {
		t.Fatalf("reading restored file: %v", err)
	}
	if string(restored) != string(data) {
		t.Fatal("restored content differs from the original")
	}

	// The plaintext must not be sitting in the archive.
	raw, err := os.ReadFile(archive)
	if err != nil {
		t.Fatalf("reading archive: %v", err)
	}
	if strings.Contains(string(raw), "quick brown fox") {
		t.Fatal("plaintext is visible in the encrypted archive")
	}

	bad, err := client.Decompress(RunRequest{Input: archive, Output: output + ".bad", Password: "wrong"}, nil)
	if err != nil {
		t.Fatalf("decompress with a wrong password returned a transport error: %v", err)
	}
	if bad.OK {
		t.Fatal("decompression succeeded with the wrong password")
	}

	missing, err := client.Decompress(RunRequest{Input: archive, Output: output + ".none"}, nil)
	if err != nil {
		t.Fatalf("decompress without a password returned a transport error: %v", err)
	}
	if missing.OK || !strings.Contains(missing.Error, "password") {
		t.Fatalf("expected a password error, got ok=%v error=%q", missing.OK, missing.Error)
	}
}

func TestCorruptedArchiveIsReported(t *testing.T) {
	client := newClient(t)
	input := writeFixture(t, "data.txt", repetitive(150000))
	archive := filepath.Join(t.TempDir(), "data.kolma")
	res, err := client.Compress(RunRequest{Input: input, Output: archive, Algorithm: "lz77"}, nil)
	if err != nil || !res.OK {
		t.Fatalf("compress: err=%v ok=%v %s", err, res.OK, res.Error)
	}
	raw, err := os.ReadFile(archive)
	if err != nil {
		t.Fatalf("reading archive: %v", err)
	}
	raw[len(raw)-1] ^= 0x01
	damaged := filepath.Join(t.TempDir(), "damaged.kolma")
	if err := os.WriteFile(damaged, raw, 0o644); err != nil {
		t.Fatalf("writing damaged archive: %v", err)
	}

	info, err := client.Info(damaged)
	if err != nil {
		t.Fatalf("info on a damaged archive should still read the header: %v", err)
	}
	if info.PayloadCRCOK {
		t.Fatal("the payload checksum passed on a damaged archive")
	}

	out, err := client.Decompress(RunRequest{Input: damaged, Output: filepath.Join(t.TempDir(), "out")}, nil)
	if err != nil {
		t.Fatalf("decompress returned a transport error: %v", err)
	}
	if out.OK {
		t.Fatal("a damaged archive decompressed without complaint")
	}
}

func TestBenchmarkCoversEveryAlgorithm(t *testing.T) {
	client := newClient(t)
	path := writeFixture(t, "bench.txt", repetitive(200000))
	res, err := client.Benchmark(path, 6, 65536, nil)
	if err != nil {
		t.Fatalf("benchmark: %v", err)
	}
	if !res.OK {
		t.Fatalf("benchmark failed: %s", res.Error)
	}
	if len(res.Benchmark) != 7 {
		t.Fatalf("expected 7 benchmark rows, got %d", len(res.Benchmark))
	}
	for i := 1; i < len(res.Benchmark); i++ {
		if res.Benchmark[i].Ratio < res.Benchmark[i-1].Ratio {
			t.Fatalf("benchmark rows are not sorted by ratio")
		}
	}
	if !res.Benchmark[0].BestRatio {
		t.Fatal("the best ratio row is not flagged")
	}
}

func TestErrorsSurfaceInsteadOfPanicking(t *testing.T) {
	client := newClient(t)
	res, err := client.Compress(RunRequest{Input: filepath.Join(t.TempDir(), "missing.bin")}, nil)
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if res.OK || res.Error == "" {
		t.Fatalf("expected a reported error for a missing file, got %+v", res)
	}

	unknown, err := client.Compress(RunRequest{
		Input: writeFixture(t, "x.txt", []byte("hello")), Algorithm: "quantum",
	}, nil)
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if unknown.OK || !strings.Contains(unknown.Error, "unknown algorithm") {
		t.Fatalf("expected an unknown-algorithm error, got %+v", unknown)
	}
}

func TestListDirSortsDirectoriesFirst(t *testing.T) {
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "zdir"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "afile.txt"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, ".hidden"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	entries, err := ListDir(dir, false)
	if err != nil {
		t.Fatalf("ListDir: %v", err)
	}
	if len(entries) != 2 {
		t.Fatalf("expected 2 visible entries, got %d", len(entries))
	}
	if !entries[0].IsDir || entries[0].Name != "zdir" {
		t.Fatalf("directories should come first, got %+v", entries[0])
	}
	all, err := ListDir(dir, true)
	if err != nil {
		t.Fatalf("ListDir with hidden: %v", err)
	}
	if len(all) != 3 {
		t.Fatalf("expected 3 entries with hidden files, got %d", len(all))
	}
}

func TestFindBinaryPrefersTheExplicitPath(t *testing.T) {
	bin := engineBinary(t)
	got, err := FindBinary(bin)
	if err != nil {
		t.Fatalf("FindBinary(%q): %v", bin, err)
	}
	wantAbs, err := filepath.Abs(bin)
	if err != nil {
		t.Fatal(err)
	}
	if got != wantAbs {
		t.Fatalf("FindBinary returned %q, want %q", got, wantAbs)
	}
}

func TestHumanSize(t *testing.T) {
	cases := map[uint64]string{
		0:       "0 B",
		999:     "999 B",
		1024:    "1.00 KiB",
		1048576: "1.00 MiB",
	}
	for input, want := range cases {
		if got := HumanSize(input); got != want {
			t.Errorf("HumanSize(%d) = %q, want %q", input, got, want)
		}
	}
}
