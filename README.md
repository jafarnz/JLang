<img src="docs/logo.svg" alt="" width="72">

# JLang

JLang is a small programming language for people who know Python or JavaScript and want to learn how hardware works, from blinking an LED to the bits in a register and the instructions in a processor's pipeline. It is written in C99 with no dependencies: a one-pass compiler to bytecode, a stack-based virtual machine with a checked heap, a simulated board, and a RISC-V assembler, emulator and code generator.

The clearest way to see it is [Glass](glass/README.md), a pocket computer built from an ESP32-C6, an LED matrix and a small OLED. One JLang VM runs a program, such as Fibonacci until the sign bit flips, and an instruction hook hands its state after every step to a second VM, whose JLang program draws the value stack, the heap and the instruction on the displays. The same display code runs in a desktop simulator that decodes what it sends to the LED and OLED driver chips, which is how its tests check the pictures.

```jlang
let status = 0
status[7] = 1            // set bit 7
status[3..0] = 0b1010    // set bits 3 down to 0
printf("{b8} = {x2}\n", status, status)

let buf = alloc(4)
write32(buf, 0, 123456)
free(buf)
print(buf[0])            // reported as a use after free, with both lines
```

Integers are 32 bits and overflow like a CPU register, and bits are read and written with `reg[3]` and `reg[7..4]`. Memory is allocated and freed by hand on a fixed-size heap whose handles carry a generation count, so use after free, double frees, out-of-bounds access and leaks are reported with the lines involved. Programs that use pins, timers, sensors and an LED panel run on a simulated board in virtual time, and the same programs run on an ESP32 inside the firmware in [boards/esp32-vm](boards/esp32-vm).

The [documentation and playground](https://jafarniaz.com/jlang) run JLang in the browser, compiled to WebAssembly, with a step debugger, a waveform view and running programs on a board over USB.

## Installing

On macOS or Linux:

```bash
curl -fsSL https://jafarniaz.com/jlang/install.sh | sh
```

The script downloads the prebuilt `jlang` archive for your architecture from the latest [GitHub release](https://github.com/jafarnz/JLang/releases) into `~/.local/bin` without `sudo` (falling back to building from source if needed). Other ways:

| | |
|---|---|
| Prebuilt archives (Linux, macOS, Windows) | Download `jlang-<os>-<arch>.tar.gz` or `jlang-windows-x86_64.zip` with `SHA256SUMS` from [GitHub Releases](https://github.com/jafarnz/JLang/releases) |
| From source | `git clone https://github.com/jafarnz/JLang && cd JLang && make`, then `./build/jlang` (`make install` puts it on your `PATH`) |
| No install | The [playground](https://jafarniaz.com/jlang/playground) runs JLang in WebAssembly in the browser and flashes ESP32 boards over Web Serial |

Packaging recipes for Homebrew (`packaging/homebrew/jlang.rb`) and npm (`packaging/npm/`) live in this repository; public Homebrew tap and npm registry publishing are not enabled yet.

Then:

```bash
jlang new blinky
cd blinky
jlang run main.jlang --emit button
```

Key commands: `jlang check` (compile only), `jlang test` (isolated unit tests with leak checking), `jlang fmt` (zero-config formatter), `jlang bundle` (combine `use` modules into a single file for browser WASM or ESP32), `jlang xray`, `jlang dis`, `jlang repl`, `jlang emu`, `jlang build`, `jlang wcet`, and `jlang svd`.

## Compiling to RISC-V

JLang also compiles programs to RV32IM machine code, covering a strictly checked subset of the language. The same program can run on both:

```bash
./build/jlang run examples/registers.jlang    # on the bytecode VM
./build/jlang emu examples/registers.jlang    # compiled to RISC-V, on the built-in emulator
```

`--asm` prints the generated assembly, `--listing` each instruction's address and encoding, and `--pipeline` how a 5-stage pipeline runs it, with its stalls and flushes. `jlang build` writes assembly for a real board, `jlang wcet` works out the worst-case time of each event handler from the machine code, and `jlang svd` turns a chip's SVD register description into consts. Native code reaches hardware registers with `mmio_read32` and `mmio_write32` and can include instructions with `asm("...")`.

Native programs have no dynamic heap and no run-time types, so a program either behaves identically to the VM or is rejected at compile time with the reason (including rejecting `free()`, non-int arrays/structs, and dynamic heap built-ins). See [docs/LANGUAGE.md#native-code](docs/LANGUAGE.md#native-code) for the complete native subset table. The parity suite (`tests/native.sh`) runs every example and test program both ways and verifies every expected rejection in [tests/native-skips.txt](tests/native-skips.txt); `tests/gnu.sh` verifies that the assembler output is byte-identical to GNU `as`.

## Using JLang in firmware

There are two ways to run JLang on a microcontroller, and [boards/](boards) has the full board support matrix, memory footprints, wiring, and recovery steps.

Native code from `jlang build` runs on RISC-V microcontrollers (CI-built with ESP-IDF for ESP32-C3 and ESP32-C6; an untested Raspberry Pi Pico 2 HAL template is also included in [boards/pico2](boards/pico2)). A board provides seven small C functions for printing, pins and time, described in [boards/jl_hal.h](boards/jl_hal.h).

The bytecode VM runs the full language inside the firmware of 32-bit microcontrollers, including Xtensa chips such as the ESP32-S3 (CI-built merged binaries for ESP32-S3, ESP32-C6, and ESP32-C3 are served in the playground's Web Serial installer). Multi-file projects using `use "..." as alias` can be deployed to the browser or stock ESP32 firmware with `jlang bundle entry.jlang -o app.jlang`; see [examples/weather_station](examples/weather_station) for a combined sensor and display project. The [embedding guide](https://jafarniaz.com/jlang/docs/guides/embedding) describes the C API.

## Repository layout

`src/` contains the lexer, the one-pass compiler, the bytecode virtual machine, the heap allocator, the built-in functions, the board simulator and the `jlang` command, and `src/rv_*.c` contains the RISC-V code generator, assembler and emulator. `boards/` contains hardware support. `wasm/` contains the WebAssembly build used by the playground, `editors/vscode/` contains syntax highlighting for VS Code, `examples/` contains example programs and `tests/` contains the test suite. [docs/DESIGN.md](docs/DESIGN.md) explains the implementation file by file.

```bash
make test        # run the tests (RISC-V checks need binutils/gcc-riscv64-unknown-elf)
make sanitize    # run the tests with AddressSanitizer and UBSan
make wasm        # build build/jlang.wasm (needs clang and wasi-libc)
```

## Contributing

JLang is built to be the language people reach for when they move from software to hardware, and it gets there with help from the people learning it. You don't need a computer engineering background: examples, clearer error messages, simulated parts and docs are all valuable, and the whole implementation is small enough to read. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to build, test and make a first change, and [ROADMAP.md](ROADMAP.md) lists work to pick up, with the small items marked.

Questions, ideas and projects go in [Discussions](https://github.com/jafarnz/JLang/discussions), and bugs in [Issues](https://github.com/jafarnz/JLang/issues). Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## License

JLang is released under the MIT license.
