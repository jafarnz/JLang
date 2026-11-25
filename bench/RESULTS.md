# JLang and MicroPython

Measured by `bench/run.py` on x86_64 Linux, against MicroPython 1.24.1 built from source (the Unix port, standard variant). Each program ran 5 times and the fastest run counts. Both print the same answer for every program, which the script checks.

| Program | What it does | JLang | MicroPython | JLang / MicroPython |
|---|---|---:|---:|---:|
| `empty` | starting up: an empty program | 1 ms | 1 ms | 0.97x |
| `fib` | function calls: recursive fib(27) | 23 ms | 45 ms | 0.51x |
| `loop` | arithmetic: a 3 million step loop | 136 ms | 269 ms | 0.50x |
| `sieve` | memory: primes below 200,000 in a byte buffer | 35 ms | 74 ms | 0.47x |
| `bits` | bits: counting the 1 bits of 300,000 numbers | 108 ms | 245 ms | 0.44x |
| `strings` | text: building 20,000 formatted messages | 5 ms | 28 ms | 0.19x |

The `jlang` program is 233 KB and `micropython` is 578 KB. JLang's includes its compiler, the VM, the simulated board, the RISC-V assembler and emulator; MicroPython's includes its compiler, VM and standard modules.

## Why JLang is faster here

Mostly because it does less. Every JLang number is a 32-bit integer in a fixed 8-byte value, global variables are numbered slots found while compiling rather than names looked up while running, and there is no garbage collector, because the program frees its own memory. MicroPython has floats, integers of any size, classes, dictionaries and modules, and every operation checks for them. The strings benchmark shows the difference most: MicroPython leaves 20,000 discarded strings for its garbage collector, while JLang frees each one as soon as the program is done with it.

MicroPython is the better choice for a large program that needs its libraries. JLang's aim is to make what the hardware does visible, and being quick is a side effect of keeping it small.

## On a board (not yet measured on physical hardware)

All numbers above are **desktop x86_64 Linux measurements** comparing the JLang bytecode VM against MicroPython's Unix port. Neither runtime has been benchmarked on a physical ESP32 board yet.

For native RV32IM codegen (`jlang emu`), `bench/bits.jlang` executes in 34,841,599 RV32IM instructions (down 4.73x from the initial 164.7M unoptimized backend after adding `s2`–`s7` local register promotion and the 7-register expression cache).

To reproduce an on-board timing comparison once an ESP32 is connected: run each `.jlang` file from the playground's **Run on device** button wrapped with `let t = millis()` and `print(millis() - t)`, and run the matching `.py` file on MicroPython with `time.ticks_ms()`.
