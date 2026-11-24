# Roadmap

Where JLang is going, and work anyone can pick up. Comment on the matching issue, or open one, before starting something large, so two people don't build the same thing. Each item says roughly how big it is: *good first issue* means a few hours and no deep knowledge of the code.

## Making it easier to start

- **More examples** (*good first issue*). A traffic light, a reaction-time game, a plant watering timer, a theremin with the distance sensor, Snake on the LED panel. One file each in `examples/`, with a comment at the top saying what it does and how to wire it.
- **Better error messages** (*good first issue*). Find an error that confused you, and make it say how to fix the problem. `tests/err_*.jlang` shows the format.
- **A VS Code extension on the marketplace** (*good first issue*). `editors/vscode/` has the syntax highlighting; it needs packaging and publishing, and a "Run on the simulator" command.
- **`jlang fmt`**, a formatter that lays code out one standard way. Medium.
- **A language server** for errors and completion in editors, reusing the compiler's error reporting. Large.
- **Translations of the docs.** The first part, "Learn the basics", is the place to start.

## The language

- **Maps**, a fixed-capacity dictionary on the heap. Medium.
- **Exhaustive `match` checks on `enum` values** so missing a state is a compile error. Medium.

## Hardware

- **Tested board support.** The ESP32 firmware is built in CI and tested in an emulator; reports from running it on real boards, and fixes for what doesn't work, are the most valuable hardware help there is. *Good first issue* for anyone with an ESP32.
- **More simulated parts**: a humidity sensor, a rotary encoder, an OLED that draws in the playground, a stepper motor. Each is a few functions in `src/board_sim.c` and a picture in the playground. Medium.
- **Raspberry Pi Pico 2** firmware for the VM, like `boards/esp32-vm`. Medium.
- **Wi-Fi and HTTP** built-ins for the ESP32 firmware, so a device can post readings. Large.

## Native code

Compiling to RISC-V covers a subset of the language today; see [tests/native-skips.txt](tests/native-skips.txt).

- **Arrays and structs in native code**, with a static heap. Large.
- **Timers in native code**, using the chip's timer interrupts. Medium.
- **Cross-block local register promotion** (extending the current 7-register expression cache to keep loop locals in callee-saved registers across basic blocks). Medium.

## AI coding assistants

JLang is small and fully documented in [docs/llms.txt](docs/llms.txt), which makes it a good language for AI assistants to write and check.

- **An MCP server** that lets an assistant run, check and step through JLang programs and read the simulated board, so it can test hardware code before it reaches a device. Medium.
- **An evaluation set** of hardware tasks with expected output, to measure how well assistants write JLang. Medium.
