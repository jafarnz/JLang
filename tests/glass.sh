#!/usr/bin/env bash
# Runs Glass in its simulator and compares what the LEDs and the screen
# show with saved pictures in tests/glass/. Catches changes to the JLang
# drivers in glass.jlang and to the step API. After an intended change,
# check the new output and save it with: tests/glass.sh build/glass_sim --update
set -u
SIM="${1:-build/glass_sim}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
check() {
    local name="$1"
    shift
    "$SIM" "$@" --out "$TMP" "$DIR/glass/glass.jlang" "$DIR/glass/program.jlang" > "$TMP/$name.txt" 2>/dev/null
    if [[ "${2:-}" == "--update" || " ${UPDATE:-} " == " 1 " ]]; then
        cp "$TMP/$name.txt" "$DIR/tests/glass/$name.txt"
    fi
    if diff -q "$DIR/tests/glass/$name.txt" "$TMP/$name.txt" >/dev/null 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "glass: $name differs"
        diff "$DIR/tests/glass/$name.txt" "$TMP/$name.txt" | head -20
    fi
}
[[ "${2:-}" == "--update" ]] && export UPDATE=1
check startup --knob 0 --snap 1000 --until 1100
check paused --knob 0 --press 300 --snap 6000 --until 6100
check running --knob 2600 --snap 9000 --until 9100
check finished --knob 4095 --snap 2300 --until 2400

for app in raycaster pong calc radar dashboard; do
    if "$SIM" --knob 1200 --press 400 --snap 500 --until 600 --out "$TMP" "$DIR/glass/apps/$app.jlang" > "$TMP/app-$app.txt" 2>/dev/null \
       && grep -q '#' "$TMP/app-$app.txt" && grep -q '█' "$TMP/app-$app.txt"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "glass: app $app failed to render both displays"
    fi
done

echo "glass: $pass simulator pictures match, $fail differ"
[[ $fail -eq 0 ]]
