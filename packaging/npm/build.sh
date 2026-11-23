#!/usr/bin/env bash
# Assembles the npm package in build/npm from the WebAssembly build:
#   make wasm && packaging/npm/build.sh 0.2.0
set -eu
VERSION="${1:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/npm"
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib"
cp "$ROOT/packaging/npm/bin/jlang.js" "$OUT/bin/jlang.js"
chmod +x "$OUT/bin/jlang.js"
cp "$ROOT/build/jlang.wasm" "$ROOT/wasm/jlang.mjs" "$OUT/lib/"
cp "$ROOT/LICENSE" "$OUT/"
cp -r "$ROOT/examples" "$OUT/"
cat > "$OUT/package.json" <<JSON
{
  "name": "jlang-cli",
  "version": "$VERSION",
  "description": "JLang, a small language for learning how hardware works: a simulated board, LED panel, sensors and a RISC-V compiler, running anywhere Node does.",
  "type": "module",
  "bin": { "jlang": "bin/jlang.js" },
  "files": ["bin", "lib", "examples", "LICENSE", "README.md"],
  "engines": { "node": ">=18" },
  "keywords": ["jlang", "language", "hardware", "embedded", "microcontroller", "risc-v", "education", "esp32"],
  "homepage": "https://jafarniaz.com/jlang",
  "repository": { "type": "git", "url": "git+https://github.com/jafarnz/JLang.git" },
  "bugs": "https://github.com/jafarnz/JLang/issues",
  "license": "MIT"
}
JSON
cat > "$OUT/README.md" <<'MD'
# jlang-cli

[JLang](https://jafarniaz.com/jlang) is a small programming language for people who know Python or JavaScript and want to learn how hardware works. This package is the real JLang compiler and virtual machine, compiled to WebAssembly, so it runs on any computer with Node.js 18 or later.

```bash
npx jlang-cli new blinky
cd blinky
npx jlang-cli run main.jlang --emit button
```

Or install it once with `npm install -g jlang-cli` and run `jlang`. The documentation, a playground and the native build are at [jafarniaz.com/jlang](https://jafarniaz.com/jlang), and the source is at [github.com/jafarnz/JLang](https://github.com/jafarnz/JLang).
MD
echo "built $OUT (jlang-cli $VERSION)"
