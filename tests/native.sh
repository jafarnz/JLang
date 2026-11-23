#!/usr/bin/env bash
# Differential test: every program that compiles to RISC-V must print
# exactly what the bytecode VM prints. Programs that use features native
# code doesn't have (the heap, the camera, ...) are listed with the reason
# in tests/native-skips.txt; any other native compile error is a failure.
set -u
BIN="${1:-build/jlang}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
pass=0
fail=0
skip=0
for t in "$DIR"/tests/*.jlang "$DIR"/examples/*.jlang; do
    [[ -f "$t" ]] || continue
    name="${t#"$DIR"/}"
    args="$(sed -n 's|^// args: ||p' "$t")"
    # shellcheck disable=SC2086
    native="$("$BIN" emu "$t" $args --max-cycles 50000000 2>&1)"
    ncode=$?
    reason="$(awk -F'\t' -v n="$name" '$1 == n { print $2 }' "$DIR/tests/native-skips.txt")"
    if [[ -n "$reason" ]]; then
        if [[ $ncode -eq 65 ]] && grep -qF -- "$reason" <<<"$native"; then
            skip=$((skip + 1))
            continue
        fi
        fail=$((fail + 1))
        echo "UNEXPECTED: $name is listed in tests/native-skips.txt ($reason), but native code gave:"
        head -3 <<<"$native"
        continue
    fi
    # shellcheck disable=SC2086
    vm="$("$BIN" run "$t" $args --max-cycles 50000000 2>&1)"
    vcode=$?
    if [[ $vcode -eq 0 ]]; then
        ok=$([[ $ncode -eq 0 && "$native" == "$vm" ]] && echo 1 || echo 0)
    else
        # Errors: same exit code and the same first error line.
        vfirst="$(grep -m1 'error' <<<"$vm")"
        nfirst="$(grep -m1 'error' <<<"$native")"
        ok=$([[ $ncode -eq $vcode && "$vfirst" == "$nfirst" ]] && echo 1 || echo 0)
    fi
    if [[ $ok == 1 ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "DIFFERENT: $name (vm exit $vcode, native exit $ncode)"
        diff <(echo "$vm") <(echo "$native") | head -10
    fi
done
echo "native: $pass match the VM, $fail differ, $skip skipped for the reasons in tests/native-skips.txt"

# Native-only tests (inline assembly, registers, ...) with expected output.
npass=0
for t in "$DIR"/tests/native/*.jlang; do
    name="${t#"$DIR"/}"
    expected="$(sed -n 's|^.*// expect: \{0,1\}||p' "$t")"
    args="$(sed -n 's|^// args: ||p' "$t")"
    want_exit="$(sed -n 's|^// exit: ||p' "$t")"
    # shellcheck disable=SC2086
    actual="$("$BIN" emu "$t" $args 2>&1)"
    code=$?
    if [[ "$actual" == "$expected" && "$code" == "${want_exit:-0}" ]]; then
        npass=$((npass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $name (exit $code)"
        diff <(echo "$expected") <(echo "$actual") | head -10
    fi
done
echo "native-only: $npass passed"
[[ $fail -eq 0 ]]
