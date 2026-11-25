# Roadmap

Where JLang is going, and work anyone can pick up. Comment on the matching issue, or open one, before starting something large, so two people don't build the same thing.

## Making it easier to start

- **More examples** (good first issue). A traffic light, a reaction-time game, a plant watering timer, a theremin with the distance sensor, Snake on the LED panel. One file each in `examples/`, with a comment at the top saying what it does and how to wire it.
- **Better error messages** (good first issue). Find an error that confused you, and make it say how to fix the problem. `tests/err_*.jlang` shows the format.
- **Publish the VS Code extension** (good first issue). `editors/vscode/` includes syntax highlighting and a zero-dependency language server (`server.mjs` + `extension.js`); it needs packaging and publishing to the marketplace.
- **Translations of the docs.** The first part, "Learn the basics", is the place to start.

## The language

- **Maps**, a fixed-capacity dictionary on the heap. Medium.
- **Ring buffers and bit-packing in native code**, lowering `ring()` and `pack()`/`unpack()` to static `.bss` storage and inline RV32IM shifts. Medium.

## Hardware

- **Tested board support.** The ESP32 firmware is built in CI and tested in an emulator; reports from running it on real boards, and fixes for what doesn't work, are the most valuable hardware help there is.
- **More simulated parts**: a humidity sensor, a rotary encoder, an OLED that draws in the playground, a stepper motor. Each is a few functions in `src/board_sim.c` and a picture in the playground. Medium.
- **Raspberry Pi Pico 2** firmware for the VM, like `boards/esp32-vm`. Medium.
- **Wi-Fi and HTTP** built-ins for the ESP32 firmware, so a device can post readings. Large.

## Native code

Compiling to RISC-V covers a growing subset of the language (including `s2`–`s7` local register promotion and top-level static `.bss` arrays and structs); see [tests/native-skips.txt](tests/native-skips.txt).

- **Timers in native code**, using the chip's timer interrupts. Medium.
- **Passing static arrays and structs to functions** in native code. Medium.

## AI coding assistants

JLang is small, fully documented in [docs/llms.txt](docs/llms.txt), and ships a zero-dependency MCP server in [tools/jlang-mcp](tools/jlang-mcp/README.md).

- **An evaluation set** of hardware tasks with expected output, to measure how well assistants write JLang. Medium.
