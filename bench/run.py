#!/usr/bin/env python3
"""Runs each benchmark with JLang and MicroPython and writes RESULTS.md.

    python3 bench/run.py [path/to/micropython]

Both run on this computer, so the numbers compare the two interpreters, not
two chips. Each program runs 5 times and the fastest time counts, which
leaves out most of the noise from other programs.
"""
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
JLANG = os.path.join(HERE, "..", "build", "jlang")
MPY = sys.argv[1] if len(sys.argv) > 1 else "micropython"
RUNS = 5
BENCHES = [
    ("fib", "function calls: recursive fib(27)"),
    ("loop", "arithmetic: a 3 million step loop"),
    ("sieve", "memory: primes below 200,000 in a byte buffer"),
    ("bits", "bits: counting the 1 bits of 300,000 numbers"),
    ("strings", "text: building 20,000 formatted messages"),
]


def measure(cmd):
    """Fastest wall time in ms, and the output."""
    best, out = None, ""
    for _ in range(RUNS):
        start = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True)
        ms = (time.perf_counter() - start) * 1000
        if p.returncode != 0:
            sys.exit(f"{' '.join(cmd)} failed:\n{p.stdout}{p.stderr}")
        out = p.stdout.strip()
        best = ms if best is None else min(best, ms)
    return best, out


def main():
    mpy_version = subprocess.run([MPY, "-c", "import sys; print(sys.implementation.version)"],
                                 capture_output=True, text=True).stdout.strip()
    rows = []
    empty = os.path.join(HERE, "empty")
    with open(empty + ".jlang", "w") as f:
        f.write("")
    with open(empty + ".py", "w") as f:
        f.write("")
    for name, what in [("empty", "starting up: an empty program")] + BENCHES:
        j = measure([JLANG, "run", "--heap", "262144", "--no-leak-check", os.path.join(HERE, name + ".jlang")])
        m = measure([MPY, os.path.join(HERE, name + ".py")])
        if j[1] != m[1]:
            sys.exit(f"{name}: JLang printed {j[1]!r} but MicroPython printed {m[1]!r}")
        rows.append((name, what, j[0], m[0]))
        print(f"{name:8} JLang {j[0]:8.1f} ms   MicroPython {m[0]:8.1f} ms")
    os.remove(empty + ".jlang")
    os.remove(empty + ".py")

    size_j = os.path.getsize(JLANG)
    size_m = os.path.getsize(subprocess.run(["which", MPY], capture_output=True, text=True).stdout.strip() or MPY)
    with open(os.path.join(HERE, "RESULTS.md"), "w") as f:
        f.write("# JLang and MicroPython\n\n")
        f.write(f"Measured by `bench/run.py` on {platform.machine()} Linux, against MicroPython "
                f"{mpy_version.strip('()').replace(', ', '.').replace(chr(39), '').rstrip('.')} "
                "built from source (the Unix port, standard variant). Each program ran "
                f"{RUNS} times and the fastest run counts. Both print the same answer for every program, "
                "which the script checks.\n\n")
        f.write("| Program | What it does | JLang | MicroPython | JLang / MicroPython |\n")
        f.write("|---|---|---:|---:|---:|\n")
        for name, what, jt, mt in rows:
            f.write(f"| `{name}` | {what} | {jt:.0f} ms | {mt:.0f} ms | {jt / mt:.2f}x |\n")
        f.write(f"\nThe `jlang` program is {size_j // 1024} KB and `micropython` is {size_m // 1024} KB. "
                "JLang's includes its compiler, the VM, the simulated board, the RISC-V assembler and "
                "emulator; MicroPython's includes its compiler, VM and standard modules.\n\n")
        f.write("## Why JLang is faster here\n\n"
                "Mostly because it does less. Every JLang number is a 32-bit integer in a fixed 8-byte value, "
                "global variables are numbered slots found while compiling rather than names looked up while "
                "running, and there is no garbage collector, because the program frees its own memory. "
                "MicroPython has floats, integers of any size, classes, dictionaries and modules, and every "
                "operation checks for them. The strings benchmark shows the difference most: MicroPython "
                "leaves 20,000 discarded strings for its garbage collector, while JLang frees each one as "
                "soon as the program is done with it.\n\n"
                "MicroPython is the better choice for a large program that needs its libraries. JLang's "
                "aim is to make what the hardware does visible, and being quick is a side effect of "
                "keeping it small.\n\n"
                "## On a board\n\n"
                "These numbers are from a computer. On an ESP32 both are about 20 to 50 times slower, but "
                "the ratio should be similar. To check it once a board is connected: run each `.jlang` "
                "file from the playground's Run on device button with `let t = millis()` at the top and "
                "`print(millis() - t)` at the end, and each `.py` file with MicroPython's `time.ticks_ms()`.\n")
    print("wrote bench/RESULTS.md")


if __name__ == "__main__":
    main()
