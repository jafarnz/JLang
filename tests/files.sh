#!/usr/bin/env bash
# Golden-file tests for commands that write files: each tests/vcd/*.jlang
# is run with --vcd (and on the emulator with jlang emu --vcd when
# NAME.emu.expected.vcd exists) and compared with NAME.expected.vcd, and
# each tests/pipeline/*.jlang is run with jlang emu --pipeline.
set -u
BIN="${1:-build/jlang}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
check() { # name, expected file, actual file
    if cmp -s "$2" "$3"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $1"
        diff "$2" "$3" | head -20
    fi
}
for t in "$DIR"/tests/vcd/*.jlang; do
    name="$(basename "$t" .jlang)"
    "$BIN" run "$t" --vcd "$TMP/$name.vcd" >/dev/null 2>&1
    check "vcd $name" "$DIR/tests/vcd/$name.expected.vcd" "$TMP/$name.vcd"
    if [[ -f "$DIR/tests/vcd/$name.emu.expected.vcd" ]]; then
        "$BIN" emu "$t" --vcd "$TMP/$name.emu.vcd" >/dev/null 2>&1
        check "vcd emu $name" "$DIR/tests/vcd/$name.emu.expected.vcd" "$TMP/$name.emu.vcd"
    fi
done
for t in "$DIR"/tests/pipeline/*.jlang; do
    name="$(basename "$t" .jlang)"
    "$BIN" emu "$t" --pipeline 12 > "$TMP/$name.txt" 2>&1
    check "pipeline $name" "$DIR/tests/pipeline/$name.expected.txt" "$TMP/$name.txt"
done
for t in "$DIR"/tests/wcet/*.jlang; do
    name="$(basename "$t" .jlang)"
    "$BIN" wcet "$t" > "$TMP/$name.txt" 2>&1
    check "wcet $name" "$DIR/tests/wcet/$name.expected.txt" "$TMP/$name.txt"
done
for t in "$DIR"/tests/svd/*.svd; do
    name="$(basename "$t" .svd)"
    "$BIN" svd "$t" > "$TMP/$name.jlang" 2>&1
    check "svd $name" "$DIR/tests/svd/$name.expected.jlang" "$TMP/$name.jlang"
    if ! "$BIN" check "$TMP/$name.jlang" >/dev/null 2>&1; then
        fail=$((fail + 1))
        echo "FAIL: svd $name: the generated consts don't compile"
    fi
done
echo "files: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
