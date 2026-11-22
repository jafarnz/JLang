#!/usr/bin/env bash
# Checks JLang's RISC-V assembler against the GNU assembler: every program
# that compiles to RISC-V must produce byte-identical machine code.
# Skipped when the riscv64-unknown-elf binutils aren't installed.
set -u
BIN="${1:-build/jlang}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v riscv64-unknown-elf-as >/dev/null; then
    echo "gnu: skipped (install binutils-riscv64-unknown-elf to run it)"
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
printf 'SECTIONS {\n  . = 0;\n  .text : { *(.text) }\n  . = ALIGN(4);\n  .rodata : { *(.rodata) }\n  . = ALIGN(4);\n  .data : { *(.data) }\n}\n' > "$TMP/layout.ld"
pass=0
fail=0
for t in "$DIR"/tests/*.jlang "$DIR"/examples/*.jlang "$DIR"/tests/native/*.jlang; do
    "$BIN" emu "$t" --asm > "$TMP/p.s" 2>/dev/null || continue
    "$BIN" emu "$t" --bin "$TMP/ours.bin" 2>/dev/null || continue
    riscv64-unknown-elf-as -march=rv32im_zicsr -mabi=ilp32 -mno-relax -o "$TMP/p.o" "$TMP/p.s" &&
        riscv64-unknown-elf-ld -m elf32lriscv -T "$TMP/layout.ld" -o "$TMP/p.elf" "$TMP/p.o" 2>/dev/null &&
        riscv64-unknown-elf-objcopy -O binary -R .bss "$TMP/p.elf" "$TMP/gnu.bin" || { fail=$((fail + 1)); continue; }
    size=$(stat -c %s "$TMP/ours.bin" 2>/dev/null || stat -f %z "$TMP/ours.bin")
    if cmp -s <(head -c "$size" "$TMP/gnu.bin") "$TMP/ours.bin"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "DIFFERENT from GNU as: ${t#"$DIR"/}"
    fi
done
echo "gnu: $pass programs assemble identically to GNU as, $fail differ"
[[ $fail -eq 0 ]]
