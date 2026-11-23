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

## Trying it

JLang needs only a C compiler and `make`, and runs from the build folder:

```bash
git clone https://github.com/jafarnz/JLang
cd JLang
make
./build/jlang run examples/scroll.jlang --realtime
```

`make install` copies `build/jlang` to `/usr/local/bin` if you want it on your `PATH`. `./build/jlang new blinky` creates a project, `check` compiles a file without running it, `xray` prints each bytecode instruction and the value stack as it runs, `dis` prints the bytecode, `repl` starts an interactive prompt, and `help` lists everything.

## Compiling to RISC-V

JLang also compiles programs to RV32IM machine code, which is experimental and covers a subset of the language. The same program can run on both:

```bash
./build/jlang run examples/registers.jlang    # on the bytecode VM
./build/jlang emu examples/registers.jlang    # compiled to RISC-V, on the built-in emulator
```

`--asm` prints the generated assembly, `--listing` each instruction's address and encoding, and `--pipeline` how a 5-stage pipeline runs it, with its stalls and flushes. `jlang build` writes assembly for a real board, `jlang wcet` works out the worst-case time of each event handler from the machine code, and `jlang svd` turns a chip's SVD register description into consts. Native code reaches hardware registers with `mmio_read32` and `mmio_write32` and can include instructions with `asm("...")`.

Native programs have no heap and no run-time types, so a program either behaves exactly as it does on the VM or is rejected when compiling, with the reason. The test suite runs every example and test program both ways and compares the output; the ones native code can't run are listed with the reason in [tests/native-skips.txt](tests/native-skips.txt). Another test checks that the machine code is byte-for-byte what the GNU assembler produces.

## Using JLang in firmware

There are two ways to run JLang on a microcontroller, and [boards/](boards) has working setups for both.

Native code from `jlang build` runs on any RISC-V microcontroller, such as the ESP32-C3, ESP32-C6 or Raspberry Pi Pico 2. A board provides seven small C functions for printing, pins and time, described in [boards/jl_hal.h](boards/jl_hal.h).

The bytecode VM runs the full language inside the firmware of any 32-bit microcontroller, including Xtensa chips such as the ESP32-S3. The firmware registers its hardware as built-in functions and fires events when buttons are pressed, while a JLang script decides what the device does. [Iris](examples/iris.jlang), a wearable camera that tells visually impaired commuters which bus is arriving, uses JLang in this way. The [embedding guide](https://jafarniaz.com/jlang/docs/guides/embedding) describes the API.

## Repository layout

`src/` contains the lexer, the one-pass compiler, the bytecode virtual machine, the heap allocator, the built-in functions, the board simulator and the `jlang` command, and `src/rv_*.c` contains the RISC-V code generator, assembler and emulator. `boards/` contains hardware support. `wasm/` contains the WebAssembly build used by the playground, `editors/vscode/` contains syntax highlighting for VS Code, `examples/` contains example programs and `tests/` contains the test suite. [docs/DESIGN.md](docs/DESIGN.md) explains the implementation file by file.

```bash
make test        # run the tests (RISC-V checks need binutils/gcc-riscv64-unknown-elf)
make sanitize    # run the tests with AddressSanitizer and UBSan
make wasm        # build build/jlang.wasm (needs clang and wasi-libc)
```

## License

JLang is released under the MIT license.
