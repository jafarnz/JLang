#!/usr/bin/env bash
# Verifies a packaged or downloaded jlang binary against key end-to-end
# workflows: version reporting, bundling and running examples/weather_station
# at positive (+29C) and negative (-5C) temperatures, import-order preservation
# (before -> init -> after), bundler block-scope shadowing, and strict native
# parity rejections.
#
# Usage: bash tests/verify-package.sh <path-to-jlang-binary> [expected-version]
set -euo pipefail

BIN="${1:?usage: verify-package.sh <jlang-bin> [expected-version]}"
EXPECTED_VERSION="${2:-}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ver="$("$BIN" version)"
echo "Binary version: $ver"
if [[ -n "$EXPECTED_VERSION" && "$ver" != *"$EXPECTED_VERSION"* ]]; then
    echo "FAIL: expected version containing '$EXPECTED_VERSION', got '$ver'" >&2
    exit 1
fi

# 1. Weather station: direct vs bundled at +29C and -5C
"$BIN" bundle "$DIR/examples/weather_station/main.jlang" -o "$TMP/weather_bundled.jlang"
for temp in 29 -5; do
    "$BIN" run "$DIR/examples/weather_station/main.jlang" --temperature "$temp" > "$TMP/w_direct_${temp}.txt"
    "$BIN" run "$TMP/weather_bundled.jlang" --temperature "$temp" > "$TMP/w_bundled_${temp}.txt"
    diff -u "$TMP/w_direct_${temp}.txt" "$TMP/w_bundled_${temp}.txt"
    grep -Fq "sensor=0x48 display=0x3C temp=${temp}C frames=1" "$TMP/w_bundled_${temp}.txt"
done
echo "PASS: weather_station direct == bundled at +29C and -5C"

# 2. Import initialization order (before -> init -> after) + include-once
order_expected=$'before\ninit\nafter 42\nagain 42'
"$BIN" bundle "$DIR/tests/modules/order_main.jlang" -o "$TMP/order_bundled.jlang"
for mode in run emu; do
    out_direct="$("$BIN" "$mode" "$DIR/tests/modules/order_main.jlang")"
    out_bundled="$("$BIN" "$mode" "$TMP/order_bundled.jlang")"
    [[ "$out_direct" == "$order_expected" ]] || { echo "FAIL: order direct $mode: $out_direct" >&2; exit 1; }
    [[ "$out_bundled" == "$order_expected" ]] || { echo "FAIL: order bundled $mode: $out_bundled" >&2; exit 1; }
done
echo "PASS: import-order regression (run + emu, direct + bundled)"

# 3. Bundler block-scope shadowing
shadow_expected=$'1\n7\n3\n7\n4\n7\n2\n10\n21 7\n9\n42\n7'
"$BIN" bundle "$DIR/tests/modules/ns_shadow_main.jlang" -o "$TMP/shadow_bundled.jlang"
for mode in run emu; do
    out_direct="$("$BIN" "$mode" "$DIR/tests/modules/ns_shadow_main.jlang")"
    out_bundled="$("$BIN" "$mode" "$TMP/shadow_bundled.jlang")"
    [[ "$out_direct" == "$shadow_expected" ]] || { echo "FAIL: shadow direct $mode: $out_direct" >&2; exit 1; }
    [[ "$out_bundled" == "$shadow_expected" ]] || { echo "FAIL: shadow bundled $mode: $out_bundled" >&2; exit 1; }
done
echo "PASS: bundler block-scope shadowing (run + emu, direct + bundled)"

# 4. Strict native parity compile-time rejections
for f in tests/bool_array.jlang tests/err_array_uaf.jlang tests/err_bool_index.jlang tests/err_bool_slice.jlang; do
    set +e
    "$BIN" emu "$DIR/$f" >/dev/null 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 65 ]]; then
        echo "FAIL: expected native compile error (65) for $f, got $rc" >&2
        exit 1
    fi
done
echo "PASS: strict native parity rejections"
