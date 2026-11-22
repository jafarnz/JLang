#!/usr/bin/env bash
# Runs every tests/*.jl and compares the output with its "// expect:" lines.
#
# Directives inside a test file:
#   // expect: <line>     one expected line of output (stdout + stderr)
#   // args: <options>    extra options for `jlang run`
#   // exit: <code>       expected exit code (default 0)
set -u
BIN="${1:-build/jlang}"
DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0
for t in "$DIR"/*.jl; do
    name="$(basename "$t")"
    expected="$(sed -n 's|^.*// expect: \{0,1\}||p' "$t")"
    args="$(sed -n 's|^// args: ||p' "$t")"
    want_exit="$(sed -n 's|^// exit: ||p' "$t")"
    want_exit="${want_exit:-0}"
    # shellcheck disable=SC2086
    actual="$("$BIN" run "$t" $args 2>&1)"
    code=$?
    if [[ "$actual" == "$expected" && "$code" == "$want_exit" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $name (exit $code, expected $want_exit)"
        diff <(echo "$expected") <(echo "$actual") | head -20
    fi
done
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
