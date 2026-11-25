# Multi-Driver Weather Station

Demonstrates combining two independent drivers (`tmp102.jlang` and `display.jlang`) that both declare `const ADDRESS`, `let ready`, and `fn init()` without symbol collisions using `use "..." as <alias>`.

## Run on desktop

```bash
jlang run examples/weather_station/main.jlang --temperature 29
```

## Bundle for browser playground or ESP32 firmware

Because the browser WASM runtime and stock ESP32 VM firmware run a single script with no filesystem, bundle the project into one standalone `.jlang` file:

```bash
jlang bundle examples/weather_station/main.jlang -o weather.jlang
jlang run weather.jlang --temperature 29
```

Paste `weather.jlang` into the playground or send it to a connected ESP32 over Web Serial.
