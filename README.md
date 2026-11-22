# JLang

JLang is a small programming language for learning how computers work at the level of bits, bytes and pins, and for writing the behaviour of microcontroller projects.

```jlang
let status = 0
status[7] = 1            // set bit 7
status[3..0] = 0b1010    // set bits 3 down to 0
printf("{b8} = {x2}\n", status, status)

let buf = alloc(4)
write32(buf, 0, 123456)
free(buf)
print(buf[0])            // reported as a use after free
```

Integers are 32 bits wide and overflow the way a CPU register does, and individual bits can be read and written with `reg[3]` and `reg[7..4]`. Memory is allocated and freed by hand on a fixed-size heap that reports use-after-free, double frees, out-of-bounds access and leaks, together with the lines where the memory was allocated and freed. Programs that use pins and timers run on a simulated board with virtual time, and the same scripts run on real hardware through a small C API.

The documentation is at [jafarniaz.com/jlang](https://jafarniaz.com/jlang), and the [playground](https://jafarniaz.com/jlang/playground) runs JLang in the browser without installing anything.

## Installing

JLang needs only a C compiler and `make`.

```bash
git clone https://github.com/jafarnz/JLang
cd JLang
make
sudo make install
```

Then create a project and run it on the simulated board:

```bash
jlang new blinky
cd blinky
jlang run main.jlang --emit button
```

`jlang check` compiles a file without running it, `jlang xray` runs it while printing each bytecode instruction and the value stack, `jlang dis` prints the bytecode, and `jlang repl` starts an interactive prompt. `jlang help` lists every option.

## Using JLang in firmware

The core is portable C99 with no operating system calls, so it can be built into microcontroller firmware. The firmware registers its hardware as built-in functions and fires events when buttons are pressed, while a JLang script decides what the device does. [Iris](examples/iris.jlang), a wearable camera that tells visually impaired commuters which bus is arriving, uses JLang in this way. The [embedding guide](https://jafarniaz.com/jlang/docs/guides/embedding) describes the API.

## Repository layout

`src/` contains the lexer, the one-pass compiler, the bytecode virtual machine, the heap allocator, the built-in functions, the board simulator and the `jlang` command. `wasm/` contains the WebAssembly build used by the playground, `editors/vscode/` contains syntax highlighting for VS Code, `examples/` contains example programs and `tests/` contains the test suite. [docs/DESIGN.md](docs/DESIGN.md) explains the implementation file by file.

```bash
make test        # run the tests
make sanitize    # run the tests with AddressSanitizer and UBSan
make wasm        # build build/jlang.wasm (needs clang and wasi-libc)
```

## License

JLang is released under the MIT license.
