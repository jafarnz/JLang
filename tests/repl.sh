#!/usr/bin/env bash
# The REPL keeps working after a line that doesn't compile: the failed
# line's declarations are undone, so correcting it works.
set -u
BIN="${1:-build/jlang}"
out="$(printf 'let x = \nlet x = 41\nprint(x + 1)\nconst K = \nconst K = 2\nprint(K)\n' | "$BIN" repl 2>&1)"
if grep -q "already declared\|already taken" <<<"$out" || ! grep -q "jl> 42$" <<<"$out" || ! grep -q "jl> 2$" <<<"$out"; then
    echo "repl: FAILED"
    echo "$out"
    exit 1
fi
echo "repl: a failed line leaves no declarations behind"
