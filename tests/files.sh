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

# Multi-file `use` modules: VM + Native RISC-V + diamond include-once
out="$("$BIN" run "$DIR/tests/modules/main.jlang" 2>&1)"
if [[ "$out" == "18 9" ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: modules run ($out)"; fi

out="$("$BIN" emu "$DIR/tests/modules/main.jlang" 2>&1)"
if [[ "$out" == "18 9" ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: modules emu ($out)"; fi

# Circular `use` detection
out="$("$BIN" check "$DIR/tests/modules/cycle_a.jlang" 2>&1)"
rc=$?
if [[ $rc -eq 65 && "$out" == *"circular 'use'"* ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: modules cycle (rc=$rc: $out)"; fi

# Error in imported file reports imported filename and line
out="$("$BIN" check "$DIR/tests/modules/bad_main.jlang" 2>&1)"
rc=$?
if [[ $rc -eq 65 && "$out" == *"bad_child.jlang, line 3"* && "$out" == *"nope_var"* ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: modules child error (rc=$rc: $out)"; fi

# Built-in `jlang test`: passes isolated tests across multiple files (and skips imported tests)
out="$("$BIN" test "$DIR/tests/unit_tests.jlang" "$DIR/tests/modules/main.jlang" 2>&1)"
rc=$?
if [[ $rc -eq 0 && "$out" == *"4 passed, 0 failed"* ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: jlang test pass (rc=$rc: $out)"; fi

# Built-in `jlang test`: catches both assertion failure and memory leak failure
out="$("$BIN" test "$DIR/tests/modules/unit_fail.jlang" 2>&1)"
rc=$?
if [[ $rc -eq 70 && "$out" == *"PASS passes cleanly"* && "$out" == *"FAIL fails assertion"* && "$out" == *"FAIL fails memory leak"* && "$out" == *"1 passed, 2 failed"* ]]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL: jlang test fail-cases (rc=$rc: $out)"
fi

# Built-in `jlang fmt`: formats code, --check, -w, idempotence, and stdin
cat > "$TMP/messy.jlang" <<'EOF'


enum Mode{Idle,Run}
fn add(a,b){
let x=a+-1   // keep comment
if x>0{
return x+b
}else{
return 0
}
}
EOF
cat > "$TMP/messy.expected.jlang" <<'EOF'
enum Mode { Idle, Run }
fn add(a, b) {
    let x = a + -1  // keep comment
    if x > 0 {
        return x + b
    } else {
        return 0
    }
}
EOF
"$BIN" fmt "$TMP/messy.jlang" > "$TMP/messy.out.jlang" 2>&1
check "fmt stdout" "$TMP/messy.expected.jlang" "$TMP/messy.out.jlang"
"$BIN" fmt - < "$TMP/messy.jlang" > "$TMP/messy.stdin.jlang" 2>&1
check "fmt stdin" "$TMP/messy.expected.jlang" "$TMP/messy.stdin.jlang"
"$BIN" fmt --check "$TMP/messy.jlang" >/dev/null 2>&1
rc=$?
if [[ $rc -eq 1 ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: fmt --check unformatted (rc=$rc)"; fi
"$BIN" fmt -w "$TMP/messy.jlang" >/dev/null 2>&1
"$BIN" fmt --check "$TMP/messy.jlang" >/dev/null 2>&1
rc=$?
if [[ $rc -eq 0 ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: fmt -w then --check (rc=$rc)"; fi

echo "files: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
