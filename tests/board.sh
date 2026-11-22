#!/usr/bin/env bash
# Links JLang native code with a board file written in C and compiled by
# GCC, exactly as on a real board, and checks it behaves like the VM.
# Skipped when riscv64-unknown-elf-gcc isn't installed.
set -u
BIN="${1:-build/jlang}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v riscv64-unknown-elf-gcc >/dev/null; then
    echo "board: skipped (install gcc-riscv64-unknown-elf to run it)"
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
riscv64-unknown-elf-gcc -S -O2 -march=rv32im -mabi=ilp32 -fno-pic -o "$TMP/hal.s" "$DIR/tests/hal_c_board.c" || exit 1
grep -v -E '^\s*\.(ident|attribute|file|section\s+\.note)' "$TMP/hal.s" > "$TMP/hal_clean.s"
"$BIN" build "$DIR/examples/button.jlang" -o "$TMP/prog.s" >/dev/null || exit 1
cat "$TMP/prog.s" "$TMP/hal_clean.s" > "$TMP/all.s"
native="$("$BIN" asm "$TMP/all.s" 2>&1)"
code=$?
vm="$("$BIN" run "$DIR/examples/button.jlang" --emit press --emit press 2>&1)"
if [[ $code -eq 0 && "$native" == "$vm" ]]; then
    echo "board: native code linked with a GCC-compiled C board file behaves like the VM"
else
    echo "board: FAILED (exit $code)"
    diff <(echo "$vm") <(echo "$native")
    exit 1
fi
