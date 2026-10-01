// kolma-tui -- a terminal interface for the kolma compression engine.
//
// It is a client, not a reimplementation: the C++ engine does the work and this
// binary renders it. See the README for the full walkthrough.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/LangerSword/kolma/tui/internal/core"
	"github.com/LangerSword/kolma/tui/internal/ui"
)

func main() {
	bin := flag.String("bin", "", "path to the kolma engine binary (overrides KOLMA_BIN and PATH)")
	showVersion := flag.Bool("version", false, "print the client and engine versions and exit")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "kolma-tui -- terminal interface for the kolma compression engine\n\n")
		fmt.Fprintf(os.Stderr, "usage: kolma-tui [--bin /path/to/kolma]\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	resolved, err := core.FindBinary(*bin)
	if err != nil {
		fmt.Fprintf(os.Stderr, "kolma-tui: %v\n", err)
		os.Exit(1)
	}
	client := core.Client{Bin: resolved}
	version, err := client.Version()
	if err != nil {
		fmt.Fprintf(os.Stderr, "kolma-tui: %v\n", err)
		os.Exit(1)
	}

	if *showVersion {
		fmt.Printf("kolma-tui (client)\nengine: %s (%s)\n", version, resolved)
		return
	}
	if err := ui.Run(client, version); err != nil {
		fmt.Fprintf(os.Stderr, "kolma-tui: %v\n", err)
		os.Exit(1)
	}
}
