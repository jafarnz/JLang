# Running JLang on hardware

There are two ways to run a JLang program on a microcontroller.

**Native code** compiles the program to RISC-V machine code with `jlang build`. It runs on any RISC-V microcontroller, with no interpreter on the chip. The board only has to provide the seven small functions in [`jl_hal.h`](jl_hal.h), which print a character, drive and read pins and measure time. This folder has them for the RISC-V ESP32 chips ([`esp32-riscv`](esp32-riscv): ESP32-C3, C6 and H2) and the [Raspberry Pi Pico 2](pico2). Native code supports the language without the heap and strings created at run time, and adds `mmio_read32`, `mmio_write32` and `asm()` for direct register access and inline assembly.

**The bytecode VM** runs the full language, including memory, inside the firmware of any 32-bit microcontroller with about 64 KB of RAM. The firmware registers its hardware as built-in functions, as in the [ESP32 example](esp32-vm), which works on every ESP32 including the ESP32-S3 used by Iris.

Without any hardware, `jlang emu app.jlang` runs the native code on a RISC-V emulator, and `jlang run app.jlang` runs the VM on a simulated board.

## First flash on an ESP32

Both ESP32 projects build with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html) 5.x. Install it once, following Espressif's guide for your operating system, then open a terminal where `idf.py` works (on Windows, the "ESP-IDF Terminal" shortcut; on macOS and Linux, run `. $HOME/esp/esp-idf/export.sh`).

Build the JLang compiler first, because the native project uses it:

```bash
make
```

Plug the board in with a USB-C cable that carries data. Then, for the VM on a XIAO ESP32-S3 (or any ESP32):

```bash
cd boards/esp32-vm
idf.py set-target esp32s3
idf.py build flash monitor
```

Or for native RISC-V code on a XIAO ESP32-C6 (or a C3 or H2):

```bash
cd boards/esp32-riscv
idf.py set-target esp32c6
idf.py build flash monitor
```

`set-target` is only needed the first time. The first build takes a few minutes; later ones take seconds. When it finishes, the board's LED blinks three times and the monitor prints a line saying JLang is running. Press the BOOT button and the program's `on button` handler runs. Press `Ctrl+]` to leave the monitor.

The program is `main/app.jlang` in each project. Edit it and run `idf.py build flash monitor` again. The example pins are for Seeed Studio XIAO boards, whose user LED is GPIO 21 on the ESP32-S3 and GPIO 15 on the ESP32-C6, and lights when the pin is 0. On another board, change `LED` to its LED's GPIO number.

If flashing fails with "No serial data received", hold the BOOT button, press and release RESET, release BOOT, and flash again. This puts the chip into download mode by hand, which a board running a program that keeps its USB port busy sometimes needs. If `idf.py` can't find the port, pass it with `-p`, for example `-p /dev/ttyACM0` on Linux, `-p /dev/cu.usbmodem101` on macOS or `-p COM5` on Windows.

On the RISC-V chips, `cycles()` uses the `rdcycle` instruction, which the ESP32 cores don't implement, so a program that calls it stops with an illegal-instruction error. Use `millis()` to measure time instead.

## Native code on a Raspberry Pi Pico 2

```bash
jlang build app.jlang -o boards/pico2/program.s
cd boards/pico2 && mkdir build && cd build
cmake .. && make
```

Copy `jlang_app.uf2` to the board while holding BOOTSEL.

## Adding a board

Implement the functions in `jl_hal.h` with the board's SDK, add the generated `program.s` to the build, and call `jl_main()` from `main`. To react to events, call `jl_on_<event>()` (for example `jl_on_button()`) from the main loop rather than from an interrupt handler, because the handler runs JLang code.

The ESP32 projects are compiled with ESP-IDF for the ESP32-S3 and ESP32-C6 as part of development. The Pico 2 board file follows the Pico SDK's documentation but hasn't been built against it yet. Reports and fixes are welcome.
