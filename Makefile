CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow
PREFIX  ?= /usr/local

SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
BIN     := build/jlang

.PHONY: all test sanitize wasm install uninstall clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(BIN) build/glass_sim
	@./tests/run.sh $(BIN)
	@./tests/native.sh $(BIN)
	@./tests/gnu.sh $(BIN)
	@./tests/board.sh $(BIN)
	@./tests/files.sh $(BIN)
	@./tests/repl.sh $(BIN)
	@./tests/glass.sh build/glass_sim

# Build with AddressSanitizer + UndefinedBehaviorSanitizer and run the
# tests: catches memory bugs in JLang itself.
sanitize:
	@mkdir -p build
	$(CC) -std=c99 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o build/jlang-san $(SRC)
	@./tests/run.sh build/jlang-san
	@./tests/files.sh build/jlang-san
	@./tests/repl.sh build/jlang-san
	@./tests/native.sh build/jlang-san

# WebAssembly build for the browser playground (needs clang + wasi-libc).
WASI_SYSROOT ?= /usr
CORE := $(filter-out src/main.c,$(SRC))

wasm: build/jlang.wasm

build/jlang.wasm: $(SRC) $(HDR) wasm/jlang_wasm.c
	@mkdir -p build
	clang --target=wasm32-wasi --sysroot=$(WASI_SYSROOT) -std=c99 -O2 \
		-DJL_NO_REALTIME -mexec-model=reactor -Wl,--strip-all \
		-o $@ $(CORE) wasm/jlang_wasm.c

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/jlang

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jlang

clean:
	rm -rf build

# Glass, the pocket computer that shows a program running: its desktop
# simulator. See glass/README.md.
GLASS_SRC := glass/sim/glass_sim.c glass/core/glass.c $(filter-out src/main.c src/board_sim.c src/rv_%.c,$(SRC))

build/glass_sim: $(GLASS_SRC) $(HDR) glass/core/glass.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -Iglass/core -o $@ $(GLASS_SRC)
