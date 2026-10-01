# kolma -- build, test and package the engine and the terminal interface.
#
#   make            build the engine and the TUI
#   make test       run the C++ unit and integration suite
#   make tui-test   run the Go suite (needs the engine, it is an integration test)
#   make smoke      drive the TUI through a pty and assert on what it renders
#   make check      everything above, in order
#   make bench FILE=somefile.bin

BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
CMAKE       ?= cmake
GENERATOR   ?= Ninja
ENGINE      := $(CURDIR)/$(BUILD_DIR)/kolma
TUI         := $(CURDIR)/tui/kolma-tui

.PHONY: all configure engine tui test tui-test smoke check bench clean distclean install help

all: engine tui

help:
	@grep -E '^#   ' Makefile | sed 's/^#   //'

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

engine: configure
	$(CMAKE) --build $(BUILD_DIR)

tui:
	cd tui && go build -o kolma-tui .

test: engine
	./$(BUILD_DIR)/kolma_tests

tui-test: engine tui
	cd tui && KOLMA_BIN=$(ENGINE) go test ./...

smoke: engine tui
	python3 scripts/tui_smoke.py --tui tui/kolma-tui --engine $(BUILD_DIR)/kolma

check: test tui-test smoke

bench: engine
	@test -n "$(FILE)" || { echo "usage: make bench FILE=path"; exit 2; }
	./$(BUILD_DIR)/kolma bench $(FILE) --limit 4M

install: engine tui
	install -Dm755 $(BUILD_DIR)/kolma $(DESTDIR)/usr/local/bin/kolma
	install -Dm755 tui/kolma-tui $(DESTDIR)/usr/local/bin/kolma-tui

clean:
	rm -rf $(BUILD_DIR) tui/kolma-tui

distclean: clean
	rm -rf build-asan