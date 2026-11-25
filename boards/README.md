# Running JLang on hardware

There are two ways to run a JLang program on a microcontroller:

- **The bytecode VM** (`boards/esp32-vm/`) runs the full language (including checked heap, strings, `ring()`, `pack()`/`unpack()`, I2C, SPI, PWM, ADC, RMT WS2812, and NVS `kv_*`) inside firmware with a Web Serial USB protocol.
- **Native RISC-V code** (`boards/esp32-riscv/`, `boards/pico2/`) compiles the heap-free subset via `jlang build` directly to RV32IM machine code linked with the seven HAL functions in [`jl_hal.h`](jl_hal.h).

Without hardware, `jlang run app.jlang` runs on the simulated board and `jlang emu app.jlang` runs compiled RV32IM machine code on the built-in emulator.

## Board support matrix

| Board / Target | Firmware | Verification status | Merged image size (ESP-IDF v5.5.1 CI) | VM core RAM + default heap | Working peripherals in firmware |
|---|---|---|---|---|---|
| **ESP32-S3** (e.g. XIAO ESP32-S3) | `boards/esp32-vm` | CI-built merged `.bin` (not yet bench-tested on physical board) | 492 KB (`jlang-esp32s3.bin`) | ~44 KB VM + 64 KB heap | GPIO, LEDC PWM, RMT WS2812, ADC, I2C, SPI, NVS `kv_*`, BOOT press/hold/release, USB Serial/JTAG `@jlang` |
| **ESP32-C6** (e.g. XIAO ESP32-C6) | `boards/esp32-vm` | CI-built merged `.bin` (not yet bench-tested on physical board) | 421 KB (`jlang-esp32c6.bin`) | ~44 KB VM + 64 KB heap | GPIO, LEDC PWM, RMT WS2812, ADC, I2C, SPI, NVS `kv_*`, BOOT press/hold/release, USB Serial/JTAG `@jlang` |
| **ESP32-C3** (e.g. XIAO ESP32-C3) | `boards/esp32-vm` | CI-built merged `.bin` (not yet bench-tested on physical board) | 475 KB (`jlang-esp32c3.bin`) | ~44 KB VM + 64 KB heap | GPIO, LEDC PWM, RMT WS2812, ADC, I2C, SPI, NVS `kv_*`, BOOT press/hold/release, USB Serial/JTAG `@jlang` |
| **ESP32-C6 / C3** (native) | `boards/esp32-riscv` | CI-built against ESP-IDF v5.5.1 (not yet bench-tested on physical board) | Built in CI | Static `.bss` + 16 KB stack | GPIO, UART/USB stdout, `delay`/`millis`, BOOT `on button`, MMIO |
| **Glass (XIAO ESP32-C6)** | `glass/firmware` | CI-built against ESP-IDF v5.5.1; drivers verified in `glass_sim` | Built in CI | 2 × VM (16 KB heap each) | Bit-banged MAX7219 32×8 LED matrix, I2C SH1106/SSD1306 OLED, ADC knob, GPIO button |
| **Raspberry Pi Pico 2 (RP2350)** | `boards/pico2` | **Template only** (not yet compiled against Pico SDK in CI or tested on hardware) | — | Static `.bss` + 16 KB stack | `jl_hal.h` template only |

## Complete beginner path: Playground to Desktop to XIAO ESP32-C6

This walkthrough takes one multi-driver project (or a starter blink+button script) all the way from the browser playground to a Seeed Studio XIAO ESP32-C6.

### Step 1: Test in the playground

Open [jafarniaz.com/jlang/playground](https://jafarniaz.com/jlang/playground) and run:

```jlang
const LED = 15    // XIAO ESP32-C6 user LED (active-low); use 21 on XIAO ESP32-S3
let presses = 0

pin_mode(LED, "out")
pin_write(LED, 1) // LED off on active-low XIAO boards
print("ready on XIAO ESP32-C6")

on button {
    presses += 1
    pin_write(LED, presses % 2)
    printf("button press #{}\n", presses)
}
```

### Step 2: Run and test on desktop

```bash
jlang check main.jlang
jlang run main.jlang --emit button --emit button
```

Expected desktop output:

```text
ready on XIAO ESP32-C6
button press #1
button press #2
```

If your project uses multiple files with `use "driver.jlang" as alias` (such as [`examples/weather_station`](../examples/weather_station)), bundle it into one file before uploading to the browser or board:

```bash
jlang bundle examples/weather_station/main.jlang -o weather.jlang
```

### Step 3: Wiring on XIAO ESP32-C6

For the starter blink + button script above, **no external wiring is needed**: the XIAO ESP32-C6 has an onboard user LED on GPIO 15 and a BOOT button on GPIO 9.

If you connect an external TMP102 I2C sensor or OLED (as in `examples/weather_station`):

| Peripheral pin | XIAO ESP32-C6 pin | Notes |
|---|---|---|
| VCC | `3V3` | Never connect 5 V signals to ESP32 GPIO pins |
| GND | `GND` | Shared ground |
| SDA | `D4` (GPIO 22) | Default I2C data pin in `boards/esp32-vm` |
| SCL | `D5` (GPIO 23) | Default I2C clock pin in `boards/esp32-vm` |

### Step 4: Flash firmware and run on the board

Option A — **Browser Web Serial (no toolchain needed)**:

1. Connect the XIAO ESP32-C6 with a USB-C **data** cable (not a charge-only cable).
2. In Chrome or Edge, open [jafarniaz.com/jlang/playground](https://jafarniaz.com/jlang/playground), open the **Board** menu, select **Install JLang firmware**, and choose **ESP32-C6**.
3. Once flashing finishes, click **Connect**, then **Run on device**.

Option B — **Build and flash from source with ESP-IDF 5.x**:

```bash
make
cd boards/esp32-vm
idf.py set-target esp32c6
idf.py build flash monitor
```

Expected serial output at 115200 baud after reset and pressing BOOT twice:

```text
ready on XIAO ESP32-C6
button press #1
button press #2
```

### Step 5: Recovering from a failed flash or unresponsive board

- **Chip does not enter bootloader / "No serial data received"**: hold the **BOOT** button on the XIAO, tap and release **RESET**, then release **BOOT**. The chip stays in ROM download mode until you flash or reset again.
- **No serial port appears**: swap the USB-C cable (many phone charging cables lack D+/D− data wires). On Linux, ensure your user is in the `dialout` group (`sudo usermod -aG dialout $USER` and re-login).
- **Web Serial unavailable**: use Chrome, Edge, or Brave on desktop (Safari and Firefox do not implement Web Serial).
- **Corrupted NVS or boot loop**: erase flash cleanly before reflashing:
  ```bash
  esptool.py --chip esp32c6 erase_flash
  ```
  Then re-install the firmware from the playground or `idf.py flash`.
- **Native RISC-V note**: `cycles()` uses `rdcycle`, which ESP32 RISC-V cores do not implement. Use `millis()` on hardware.

## Adding a board

Implement the seven functions in [`jl_hal.h`](jl_hal.h) using your board's SDK, compile your `.jlang` file with `jlang build app.jlang -o program.s`, link `program.s`, call `jl_main()` once, and invoke `jl_on_<event>()` from the main loop (never directly from an ISR).
