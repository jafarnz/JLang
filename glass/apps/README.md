# Glass Dual-Display Apps

Standalone apps built for Glass (Seeed Studio XIAO ESP32-C6 + 128×64 SH1106/SSD1306 OLED + 32×8 MAX7219 LED matrix + knob + button). Every app imports the shared hardware driver [display.jlang](display.jlang) via `use "display.jlang"` and runs directly on `./build/glass_sim` (emitting real `.ppm` and ASCII snapshots of both displays).

## Apps

| App | OLED (128×64) | LED Matrix (32×8) | Controls |
|-----|---------------|-------------------|----------|
| [`raycaster.jlang`](raycaster.jlang) | Fixed-point (`fix_mul`/`fix_div`) 3D corridor raycaster with shaded side walls | Live 8×8 top-down mini-map, compass heading, and wall proximity gauge | Knob turns view; button walks forward |
| [`pong.jlang`](pong.jlang) | Full court, center net, player paddle, AI paddle, and ball physics | Live big-digit scoreboard (`00-00`) and rally streak meter | Knob moves paddle; button resets match |
| [`calc.jlang`](calc.jlang) | Programmer's calculator showing bitwise, saturating, and fixed-point expressions in hex, decimal, and binary | Live 32-bit bit-register visualizer (Operand A, Operand B, and Result) | Knob changes operand A; button cycles operator |
| [`radar.jlang`](radar.jlang) | Wi-Fi 6 / BLE 5 scanner readout with SSID, channel, dBm bar, and `ring()` RSSI sparkline (encoded via `pack()`/`unpack()`) | 8-channel signal strength bar graph with sweeping radar cursor | Knob selects channel; button toggles Wi-Fi 6 / BLE 5 |
| [`dashboard.jlang`](dashboard.jlang) | Desk dashboard with Pomodoro progress bar, live clock, and bus arrival times | Live `MM:SS` countdown or scrolling bus status ticker | Knob switches page; button pauses/resumes Pomodoro |

## Run in the simulator

```bash
make build/glass_sim
./build/glass_sim --snap 800 --until 900 glass/apps/raycaster.jlang
./build/glass_sim --knob 2000 --snap 1200 --until 1300 glass/apps/pong.jlang
./build/glass_sim --knob 1600 --press 300 --snap 900 --until 1000 glass/apps/calc.jlang
./build/glass_sim --press 500 --snap 1200 --until 1300 glass/apps/radar.jlang
./build/glass_sim --knob 500 --snap 1200 --until 1300 glass/apps/dashboard.jlang
```
