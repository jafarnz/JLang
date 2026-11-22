<div align="center">

# JLang

**A small, open language for software people who want to understand hardware.**

Write code on your laptop against a simulated board, see exactly what the
machine does with it, then run the same script on a real microcontroller.

```jl
let config = 0
config[7] = 1            // turn the sensor on
config[6..4] = 0b101     // sample rate 5
printf("CONFIG = {b8}\n", config)     // CONFIG = 0b11010000
```

[Quick start](#quick-start) · [Why JLang](#why-jlang) · [Language tour](#language-tour) ·
[Docs](docs/LANGUAGE.md) · [Embedding](docs/EMBEDDING.md) · [For AI agents](docs/llms.txt)

</div>

---

## Why JLang

Most languages hide the machine. Python hides memory. JavaScript hides integer
sizes. C shows you everything but punishes mistakes silently. JLang sits in
between: it is as low-level as the hardware you are learning, and it explains
every mistake.

| | What it means for you |
|---|---|
| **Bits are first-class** | `reg[3]` reads bit 3 of an int, `reg[7..4] = 5` writes a bit field, the way hardware registers really work. Binary and hex literals, `{b8}` / `{x2}` formatting. |
| **Real memory, with a safety net** | `alloc()` and `free()` like C, on a fixed-size heap like a microcontroller. Use-after-free, double free, out-of-bounds and leaks are caught and reported with the line that caused them. |
| **Ints behave like registers** | 32-bit, wrap around on overflow, no hidden floats. `u8()`, `i8()`, `u16()`, `i16()` make widths explicit. |
| **Board simulator built in** | `pin_mode`, `pin_write`, `pin_read`, `delay`, `millis`, plus camera / AI / speaker built-ins, simulated on your laptop with **virtual time**: `delay(1000)` is instant and every run is reproducible. |
| **See inside the machine** | `jlang xray` shows each bytecode instruction and the stack as it runs. `jlang dis` prints the compiled bytecode. `--stats` counts instructions like CPU cycles. `memmap()` draws the heap. |
| **Interrupt-style events** | `on button { ... }` runs whenever the host fires `button`. |
| **Errors that teach** | `300 doesn't fit in a byte (0..255). Hint: use u8(v) or v & 0xFF`, `'prnt' is not defined - did you mean 'print'?` |
| **Tiny and embeddable** | ~4,000 lines of portable C99 (with comments), no dependencies. One fixed heap, bounded stack, no garbage collector pauses. Built to run inside ESP32 firmware. |
| **Agent-friendly** | `jlang check` gives precise errors and exit codes, `jlang new` writes an `AGENTS.md`, and [`docs/llms.txt`](docs/llms.txt) is the whole language on one page. |

## Real use: scripting hardware

JLang is the behaviour layer of real devices. The firmware (C) exposes the
hardware as built-ins; **what the device does** lives in a `.jl` script:

```jl
// iris.jl - an AI vision assistant for visually impaired commuters
on bus_button {
    let photo = capture()
    let answer = ask(photo, "Read the number of the bus arriving.")
    free(photo)
    speak(answer, "en")
    free(answer)
}
```

- **Change behaviour without reflashing.** Edit the script on the SD card, not the firmware.
- **A script bug can't crash the device.** Errors are reported; the firmware keeps running.
- **Bounded memory.** The script's heap is fixed, so it can never starve the camera or Wi-Fi.
- **Test without hardware.** The simulator mocks the camera, AI and speaker, so the logic runs in CI.

See [`examples/iris.jl`](examples/iris.jl).

## Quick start

```bash
git clone https://github.com/jafarnz/JLang && cd JLang
make                    # builds ./build/jlang (needs only a C compiler)
sudo make install       # optional: puts `jlang` on your PATH

jlang new blinky        # create a starter project
cd blinky
jlang run main.jl --emit button
```

```text
[sim      0ms] pin 2 -> HIGH
[sim    250ms] pin 2 -> LOW
...
blinked 3 times in 1500 ms
status = 0b10100001 (0xA1)
button pressed!
```

### Commands

| Command | What it does |
|---|---|
| `jlang new <name>` | Create a project with `main.jl`, `README.md` and `AGENTS.md` |
| `jlang run <file>` | Run on the simulated board |
| `jlang check <file>` | Compile only, report errors (exit 0 / 65) |
| `jlang xray <file>` | Run while showing every instruction and the stack |
| `jlang dis <file>` | Print the bytecode |
| `jlang repl` | Interactive prompt |

Options for `run` / `xray`: `--emit <event>` (repeatable), `--pin <n>=<0|1>`,
`--answer <text>`, `--heap <bytes>`, `--realtime`, `--stats`, `--no-leak-check`.

## Language tour

```jl
// Variables and numbers (32-bit ints, wrap like hardware)
let year = 2026
let mask = 0b1111_0000
let big = 0xFFFF_FFFF            // = -1 as a signed 32-bit int

// Functions, loops, conditions (no semicolons, no parentheses needed)
fn clamp(v, lo, hi) {
    if v < lo { return lo } else if v > hi { return hi }
    return v
}
for i in 0..10 { if i % 2 == 0 { continue } print(i) }
while pin_read(4) == 1 { delay(10) }

// Bits
let r = 0
r[0] = 1                          // set bit 0
r[7..4] = 0b1010                  // write bits 7..4
print(r[7..4])                    // 10

// Memory you manage yourself
let buf = alloc(16)               // 16 zeroed bytes on the JLang heap
buf[0] = 0xAA
write32(buf, 4, 123456)           // little-endian, like the ESP32
print(read32(buf, 4))
free(buf)

// Formatted output
printf("{} = {x4} = {b16}\n", 42, 42, 42)

// Events
on timer { print("tick") }
```

The full reference is in [docs/LANGUAGE.md](docs/LANGUAGE.md).

## How it works

```
source ──► lexer ──► one-pass compiler ──► bytecode ──► stack VM ──► host built-ins
 .jl        tokens      (Pratt parser)      chunks       run loop     pins, camera, ...
```

| File | Role |
|---|---|
| `src/lexer.c` | Source text → tokens |
| `src/compiler.c` | Tokens → bytecode in a single pass, no syntax tree |
| `src/vm.c` | Fetch-decode-execute loop over a value stack |
| `src/heap.c` | First-fit allocator with splitting, coalescing and generation-checked handles |
| `src/builtins.c` | Core built-ins (`print`, `alloc`, `read32`, ...) |
| `src/board_sim.c` | Desktop board simulator with virtual time |
| `src/main.c` | The `jlang` command |

Details in [docs/DESIGN.md](docs/DESIGN.md).

## Development

```bash
make test        # run the test suite
make sanitize    # run the tests under AddressSanitizer + UBSan
```

## License

MIT. See [LICENSE](LICENSE).
