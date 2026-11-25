# Glass

Glass is a pocket computer that shows a program running, one instruction at a time. A 32×8 LED matrix shows the JLang VM's value stack as bits, a small screen shows the instruction about to run and its source line, and a knob sets the clock. Turned fully left, the program pauses and a button steps through it one instruction per press. Turned right, the clock doubles with each step of the knob until the program runs as fast as the chip allows.

The program it shows by default computes Fibonacci numbers until they no longer fit in 32 bits. On the LEDs, the numbers grow from right to left until bit 31 turns on and the value becomes negative, which is two's-complement overflow happening in front of you.

Almost all of Glass is written in JLang. [`glass.jlang`](glass.jlang) contains the LED matrix driver, which clocks bits into four MAX7219 chips one pin at a time, the screen driver, which sends SH1106 or SSD1306 commands over I2C, the text rendering, and the clock logic. The C firmware only provides pins, time, the knob's ADC reading and raw I2C writes. The firmware runs two JLang VMs: one runs `glass.jlang`, and the other runs the program being shown. Before each of the program's instructions, the second VM pauses and calls `on tick` in the first, which reads the program's state with the `prog_*` built-ins described in [`core/glass.h`](core/glass.h).

## Parts

- Seeed Studio XIAO ESP32-C6
- MAX7219 4-in-1 LED dot matrix module (32×8)
- 1.3" SH1106 or 0.96" SSD1306 OLED with I2C (4 pins: GND, VCC, SCL, SDA)
- a potentiometer, for example 10 kΩ
- a push button
- a breadboard and jumper wires

## Wiring

| From | To on the XIAO ESP32-C6 |
|---|---|
| LED matrix VCC | 5V |
| LED matrix GND | GND |
| LED matrix DIN | D10 |
| LED matrix CS | D2 |
| LED matrix CLK | D8 |
| OLED VCC | 3V3 |
| OLED GND | GND |
| OLED SDA | D4 |
| OLED SCL | D5 |
| potentiometer, outer pins | 3V3 and GND |
| potentiometer, middle pin | D0 |
| button | D1 and GND |

The LED matrix runs on 5V and reads the XIAO's 3.3V signals, which works with most modules. If the matrix shows random dots, its inputs need a level shifter.

Never connect the potentiometer to 5V. The XIAO's pins accept at most 3.3V.

## Flashing

Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html) 5.x, plug in the XIAO, then:

```bash
cd glass/firmware
idf.py build flash monitor
```

At startup the LED modules show the numbers 1 to 4 for two seconds. If they are mirrored, upside down, sideways or in the wrong order, change `MIRROR`, `FLIP`, `ROTATE` or `REVERSE` at the top of `glass.jlang` and flash again, because different module makers wire the matrices differently. If you have the 0.96" screen, set `SH1106` to `false`.

To show a different program, edit [`program.jlang`](program.jlang). Glass runs it again a few seconds after it finishes or stops with an error.

## Without the hardware

The simulator runs the same `glass.jlang` and decodes what it sends to the LED chips and the screen controller, so its pictures show what the real hardware would display:

```bash
make build/glass_sim
./build/glass_sim --knob 0 --press 300 --snap 6000 glass/glass.jlang glass/program.jlang
```

`--knob` sets the knob from 0 to 4095, `--press` presses the button every so many milliseconds, and `--snap` prints the LEDs and the screen at a moment of virtual time and saves a picture as `snap-<ms>.ppm`. `--stats` prints how many VM instructions and I2C bytes each frame took.

## Standalone dual-display apps

`glass_sim` also runs standalone dual-display apps with a single file argument. See [`apps/README.md`](apps/README.md) for five ready-to-run apps built on the shared [`apps/display.jlang`](apps/display.jlang) driver module: a fixed-point 3D corridor raycaster, Pong, a bitwise programmer's calculator, a Wi-Fi 6 / BLE 5 signal radar, and a Pomodoro desk dashboard.

```bash
./build/glass_sim --knob 400 --snap 500 --until 600 glass/apps/raycaster.jlang
./build/glass_sim --knob 2000 --snap 800 --until 900 glass/apps/pong.jlang
./build/glass_sim --knob 1600 --press 300 --snap 800 --until 900 glass/apps/calc.jlang
./build/glass_sim --knob 1200 --press 400 --snap 800 --until 900 glass/apps/radar.jlang
./build/glass_sim --knob 500 --snap 800 --until 900 glass/apps/dashboard.jlang
```
