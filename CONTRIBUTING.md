# Contributing to JLang

Thanks for being here. JLang is meant to be the language people reach for when they move from software to hardware, and that only happens if people who are learning help shape it. You don't need to have studied computer engineering or written a compiler before: plenty of useful work is examples, docs, error messages and simulated parts, and the code is small enough to read in an afternoon.

## Ways to help

- **Try it and tell us what confused you.** Open an [issue](https://github.com/jafarnz/JLang/issues) or start a [discussion](https://github.com/jafarnz/JLang/discussions). Beginners notice what experts don't.
- **Write an example** in `examples/`: a sensor, a game on the LED panel, a project you built. Good examples are the fastest way for others to learn.
- **Improve an error message.** If one didn't tell you how to fix the problem, that's a bug.
- **Pick something from [ROADMAP.md](ROADMAP.md).** Items marked *good first issue* are small and self-contained.
- **Fix the docs.** They live in the website's repository, at [jafarnz.github.io/content/jlang/docs](https://github.com/jafarnz/jafarnz.github.io/tree/main/content/jlang/docs); every page has an "Edit this page on GitHub" link.

## Building and testing

You need a C compiler and `make`. Nothing else.

```bash
git clone https://github.com/jafarnz/JLang
cd JLang
make            # builds build/jlang
make test       # the test suite
make sanitize   # the tests again under AddressSanitizer and UBSan
```

Both `make test` and `make sanitize` must pass before a pull request is merged; GitHub runs them on Linux and macOS for every pull request. Some RISC-V checks need `binutils-riscv64-unknown-elf` and `gcc-riscv64-unknown-elf`, and skip themselves if those aren't installed. `make wasm` builds the WebAssembly version the playground uses, and needs clang and wasi-libc.

## How the code is laid out

[docs/DESIGN.md](docs/DESIGN.md) walks through the implementation file by file, in the order a program flows through it. In short:

| File | What it does |
|---|---|
| `src/lexer.c` | turns source text into tokens |
| `src/compiler.c` | one pass from tokens to bytecode, no syntax tree |
| `src/vm.c` | runs the bytecode on a stack machine |
| `src/heap.c` | the fixed-size heap, with handles that catch use after free |
| `src/builtins.c` | built-in functions that every host has |
| `src/board_sim.c` | the simulated board: pins, time, parts, LED panel, I2C |
| `src/rv_codegen.c`, `rv_asm.c`, `rv_emu.c` | compiling to RISC-V, assembling, emulating |
| `src/main.c` | the `jlang` command |
| `boards/` | firmware for real boards |
| `wasm/` | the WebAssembly build for the browser and npm |

## Your first change: a new built-in

Most features are a new built-in function. Say you want `sign(x)`, which gives -1, 0 or 1:

1. Write it in `src/builtins.c`, next to `clamp`. A built-in gets its arguments as `Value`s and returns `true`, or returns `jl_error(...)` with a message that says how to fix the problem.
2. Register it at the bottom of the same file: `jl_register_native(vm, "sign", bi_sign, 1);`
3. Add a test: a file `tests/sign.jlang` whose `// expect:` comments are the output. `make test` runs it.
4. Document it in `docs/LANGUAGE.md` and `docs/llms.txt`, the reference that AI coding assistants read.

Hardware built-ins go in `src/board_sim.c`, which simulates them, and in `boards/esp32-vm/main/hardware.c`, which runs them on a real ESP32.

## Rules the code follows

- C99 with no dependencies, warning-free with `-Wall -Wextra -Wpedantic -Wshadow`.
- The core must run on a microcontroller: no operating system calls outside `main.c` and `board_sim.c`, no unbounded recursion, and fixed limits from `src/common.h`.
- Every language feature has a test and an entry in the language reference.
- Error messages say what went wrong, where, and how to fix it, in plain words.
- Native code either runs a program exactly as the VM does or refuses to compile it with a reason. Never silently differently.

## Pull requests

Keep each pull request to one idea, describe what it changes and why, and add a test. If you're planning something large, open an issue first so we can agree on the approach before you spend time on it. AI coding assistants are welcome: [AGENTS.md](AGENTS.md) has the rules they should follow, and you are responsible for what they write.

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
