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

test: $(BIN)
	@./tests/run.sh $(BIN)

# Build with AddressSanitizer + UndefinedBehaviorSanitizer and run the
# tests: catches memory bugs in JLang itself.
sanitize:
	@mkdir -p build
	$(CC) -std=c99 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o build/jlang-san $(SRC)
	@./tests/run.sh build/jlang-san

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
