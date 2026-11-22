# Embedding JLang in firmware

JLang is a C library with a small API ([`src/jlang.h`](../src/jlang.h)). The
`jlang` command is one host; device firmware is another. Both register
built-ins with the same names, so scripts run unchanged on both.

## The pattern

```
┌───────────────────────── firmware (C) ─────────────────────────┐
│ camera driver · Wi-Fi · AI API client · audio · GPIO           │
│        │ exposed as built-ins: capture() ask() speak() ...     │
│        ▼                                                       │
│  ┌──────────────── JLang VM ────────────────┐                  │
│  │ behaviour script from SD card (iris.jl)  │  fixed heap      │
│  └──────────────────────────────────────────┘                  │
│        ▲                                                       │
│ button interrupt ──► jl_emit(vm, "bus_button")                 │
└────────────────────────────────────────────────────────────────┘
```

## Minimal host

```c
#include "jlang.h"

static bool my_led(JlVM *vm, int argc, Value *args, Value *out) {
    int32_t on;
    if (!jl_get_int(vm, "led", 1, args[0], &on)) return false;
    gpio_set_level(2, on != 0);          // your hardware call
    return true;
}

void app_main(void) {
    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 32 * 1024;           // the script can never use more
    cfg.out = uart_write;                // where print() goes

    JlVM *vm = jl_new(&cfg);
    jl_register_native(vm, "led", my_led, 1);

    char *script = read_file("/sdcard/main.jl");
    if (jl_run_source(vm, script) != JL_OK) {
        // the error was printed; the firmware keeps running
    }

    for (;;) {
        if (button_pressed()) jl_emit(vm, "button");
        vTaskDelay(10);
    }
}
```

## Writing built-ins

- Read arguments from `args[0..argc-1]`; check types with `jl_get_int` /
  `jl_get_string`.
- Put the result in `*out` (it starts as `nil`).
- On failure, `return jl_error(vm, "message")`; JLang adds the line number and
  call trace.
- Return memory to the script with `jl_new_buf` / `jl_new_string`: it lives on
  the script's heap, so it is counted, leak-checked and freed by the script.

## Building for a device

Copy `src/` except `main.c` and `board_sim.c` into the firmware project. Shrink
the limits for small RAM, e.g. `-DJL_STACK_MAX=256 -DJL_HANDLES_MAX=128`.

## Why a script layer?

| Without JLang | With JLang |
|---|---|
| change behaviour → recompile + reflash firmware | edit the script on the SD card |
| a bug in logic can crash the whole device | errors are reported, firmware keeps running |
| logic can use any amount of RAM | script heap is fixed and checked |
| testing needs the hardware | logic is tested on a laptop with the simulator |
