# Running JLang on hardware

There are two ways to run a JLang program on a microcontroller.

**Native code** compiles the program to RISC-V machine code with `jlang build`. It runs on any RISC-V microcontroller, with no interpreter on the chip. The board only has to provide the seven small functions in [`jl_hal.h`](jl_hal.h), which print a character, drive and read pins and measure time. This folder has them for the [ESP32-C3](esp32c3) and the [Raspberry Pi Pico 2](pico2). Native code supports the language without the heap and strings created at run time, and adds `mmio_read32`, `mmio_write32` and `asm()` for direct register access and inline assembly.

**The bytecode VM** runs the full language, including memory, inside the firmware of any 32-bit microcontroller with about 64 KB of RAM. The firmware registers its hardware as built-in functions, as in the [ESP32 example](esp32-vm), which works on every ESP32 including the ESP32-S3 used by Iris.

Without any hardware, `jlang emu app.jlang` runs the native code on a RISC-V emulator, and `jlang run app.jlang` runs the VM on a simulated board.

## Native code on an ESP32-C3

```bash
jlang build app.jlang -o boards/esp32c3/main/program.s
cd boards/esp32c3
idf.py set-target esp32c3
idf.py build flash monitor
```

## Native code on a Raspberry Pi Pico 2

```bash
jlang build app.jlang -o boards/pico2/program.s
cd boards/pico2 && mkdir build && cd build
cmake .. && make
```

Copy `jlang_app.uf2` to the board while holding BOOTSEL. Some RISC-V cores don't allow programs to read the instruction counter, in which case `cycles()` stops the program with an illegal-instruction error.

## Adding a board

Implement the functions in `jl_hal.h` with the board's SDK, add the generated `program.s` to the build, and call `jl_main()` from `main`. To react to events, call `jl_on_<event>()` (for example `jl_on_button()`) from the main loop rather than from an interrupt handler, because the handler runs JLang code.

These board files follow each SDK's documented APIs but haven't been tested on every board yet. Reports and fixes are welcome.
