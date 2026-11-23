/*
 * board_sim.c - desktop simulator for the hardware built-ins.
 *
 * Time is VIRTUAL by default: delay(1000) moves the simulated clock
 * forward by 1000 ms instantly. Runs are fast and give the same output
 * every time, so hardware logic can be tested automatically.
 *
 * The Iris built-ins (capture, ask, speak) are mocked here with
 * realistic delays. On the real device the firmware implements the
 * same functions with the camera, an AI API call and the speaker.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_sim.h"
#include "hw.h"
#include "rv.h"

enum { MODE_UNSET, MODE_OUT, MODE_IN, MODE_IN_PULLUP };

/* Simulated costs, in milliseconds, roughly like the real hardware. */
#define CAPTURE_MS 150
#define ASK_MS 1800

static struct {
    BoardSimOptions opt;
    int mode[SIM_PINS];
    int level[SIM_PINS];
    uint32_t now_ms;
    int photos;
    int last_pressed[SIM_PINS]; /* pressed(): the level at the previous call, 0 = unknown */
    Panel panel;
    uint32_t led_xy;
} sim;

/* Records a signal for --vcd. Names look like "pin2" or "pin7_tone_hz". */
static void trace(int pin, const char *what, int width, uint32_t v) {
    if (!sim.opt.vcd) return;
    char name[40];
    if (pin < 0) snprintf(name, sizeof name, "%s", what);
    else if (what[0]) snprintf(name, sizeof name, "pin%d_%s", pin, what);
    else snprintf(name, sizeof name, "pin%d", pin);
    vcd_change(sim.opt.vcd, sim.now_ms, name, width, v);
}

void board_sim_default_options(BoardSimOptions *o) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < SIM_PINS; i++) o->pin_inputs[i] = -1;
    o->ai_answer = "Bus 179 is arriving.";
    for (int i = 0; i < SIM_PINS; i++) o->analog_inputs[i] = -1;
    o->distance_cm = 100;
    o->panel_w = PANEL_DEFAULT_W;
    o->panel_h = PANEL_DEFAULT_H;
    o->print_frames = true;
}

static void advance_time(uint32_t ms) {
    sim.now_ms += ms;
#ifndef JL_NO_REALTIME
    if (sim.opt.realtime && ms > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
#endif
}

static bool get_pin(JlVM *vm, const char *fn, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, 1, v, pin)) return false;
    if (*pin < 0 || *pin >= SIM_PINS) {
        return jl_error(vm, "%s(): pin %d doesn't exist (this board has pins 0..%d)",
                        fn, (int)(*pin), SIM_PINS - 1);
    }
    return true;
}

static bool sim_pin_mode(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t pin;
    const char *m;
    uint32_t n;
    if (!get_pin(vm, "pin_mode", args[0], &pin)) return false;
    if (args[1].type != VAL_STR || !jl_get_string(vm, args[1], &m, &n)) {
        return jl_error(vm, "pin_mode(): mode must be \"out\", \"in\" or \"in_pullup\"");
    }
    if (n == 3 && memcmp(m, "out", 3) == 0) sim.mode[pin] = MODE_OUT;
    else if (n == 2 && memcmp(m, "in", 2) == 0) sim.mode[pin] = MODE_IN;
    else if (n == 9 && memcmp(m, "in_pullup", 9) == 0) sim.mode[pin] = MODE_IN_PULLUP;
    else return jl_error(vm, "pin_mode(): mode must be \"out\", \"in\" or \"in_pullup\"");
    return true;
}

static bool sim_pin_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t pin;
    if (!get_pin(vm, "pin_write", args[0], &pin)) return false;
    if (sim.mode[pin] != MODE_OUT) {
        return jl_error(vm, "pin_write(): pin %d is not an output. Call "
                            "pin_mode(%d, \"out\") first", (int)pin, (int)pin);
    }
    int level;
    if (args[1].type == VAL_BOOL) level = args[1].as.b;
    else if (args[1].type == VAL_INT && (args[1].as.i == 0 || args[1].as.i == 1)) level = args[1].as.i;
    else return jl_error(vm, "pin_write(): a pin can only be 0 (LOW) or 1 (HIGH)");

    if (sim.level[pin] != level) {
        sim.level[pin] = level;
        jl_outf(vm, "[sim %6ums] pin %d -> %s\n", (unsigned)sim.now_ms, (int)pin, level ? "HIGH" : "LOW");
    }
    trace(pin, "", 1, (uint32_t)level);
    return true;
}

static bool sim_pin_read(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pin_read", args[0], &pin)) return false;
    if (sim.mode[pin] != MODE_IN && sim.mode[pin] != MODE_IN_PULLUP) {
        return jl_error(vm, "pin_read(): pin %d is not an input. Call "
                            "pin_mode(%d, \"in\") first", (int)pin, (int)pin);
    }
    int v = sim.opt.pin_inputs[pin];
    if (v < 0) v = sim.mode[pin] == MODE_IN_PULLUP ? 1 : 0; /* pull-up reads HIGH */
    trace(pin, "in", 1, (uint32_t)v);
    *out = int_val(v);
    return true;
}

/* In the browser a program that animates forever is stopped after a while. */
static bool time_left(JlVM *vm) {
    if (sim.opt.max_ms == 0 || sim.now_ms < sim.opt.max_ms) return true;
    return jl_error(vm, "stopped after %u seconds of simulated time. On a board the program would keep running",
                    (unsigned)(sim.opt.max_ms / 1000));
}

static bool sim_delay(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t ms;
    if (!jl_get_int(vm, "delay", 1, args[0], &ms)) return false;
    if (ms < 0) return jl_error(vm, "delay(): can't wait a negative time (%d ms)", (int)ms);
    advance_time((uint32_t)ms);
    return time_left(vm);
}

static bool sim_millis(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm;
    (void)argc;
    (void)args;
    *out = int_val((int32_t)sim.now_ms);
    return true;
}

/* ---- Iris built-ins (mocked) ---------------------------------------- */

/* capture() -> buf holding a (fake) JPEG photo. Free it when done. */
static bool sim_capture(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    static const uint8_t fake_jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9,
    };
    advance_time(CAPTURE_MS);
    sim.photos++;
    /* The photo lives on the JLang heap, so leaks are tracked. */
    uint8_t *data;
    if (!jl_new_buf(vm, (uint32_t)sizeof fake_jpeg, out, &data)) return false;
    memcpy(data, fake_jpeg, sizeof fake_jpeg);
    jl_outf(vm, "[sim %6ums] camera: photo #%d captured (%u bytes)\n", (unsigned)sim.now_ms,
            sim.photos, (unsigned)sizeof fake_jpeg);
    return true;
}

/* ask(photo, prompt) -> heap str with the AI's answer. Free it when done. */
static bool sim_ask(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *prompt;
    uint32_t n;
    if (args[0].type != VAL_BUF) {
        return jl_error(vm, "ask(): argument 1 must be a photo buf from capture(), got %s",
                        value_type_name(args[0]));
    }
    if (args[1].type != VAL_STR || !jl_get_string(vm, args[1], &prompt, &n)) {
        return jl_error(vm, "ask(): argument 2 must be the question string");
    }
    jl_outf(vm, "[sim %6ums] AI: sending photo + \"%.*s\"\n", (unsigned)sim.now_ms, (int)n, prompt);
    advance_time(ASK_MS);
    const char *a = sim.opt.ai_answer;
    if (!jl_new_string(vm, a, (uint32_t)strlen(a), out)) return false;
    jl_outf(vm, "[sim %6ums] AI: answered\n", (unsigned)sim.now_ms);
    return true;
}

/* speak(text) or speak(text, lang) */
static bool sim_speak(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    const char *text, *lang = "en";
    uint32_t n, ln = 2;
    if (argc < 1 || argc > 2) return jl_error(vm, "speak() takes 1 or 2 arguments");
    if (args[0].type != VAL_STR || !jl_get_string(vm, args[0], &text, &n)) {
        return jl_error(vm, "speak(): argument 1 must be a string");
    }
    if (argc == 2 && (args[1].type != VAL_STR || !jl_get_string(vm, args[1], &lang, &ln))) {
        return jl_error(vm, "speak(): argument 2 must be a language code like \"en\" or \"zh\"");
    }
    jl_outf(vm, "[sim %6ums] speaker (%.*s): \"%.*s\"\n", (unsigned)sim.now_ms, (int)ln, lang,
            (int)n, text);
    advance_time(60u * n); /* about 60 ms per character of speech */
    return true;
}

/* ---- beginner built-ins ------------------------------------------------ */

#define SIM_LOG(...) jl_outf(vm, "[sim %6ums] ", (unsigned)sim.now_ms), jl_outf(vm, __VA_ARGS__)

static void set_level(JlVM *vm, int pin, int level) {
    sim.mode[pin] = MODE_OUT;
    if (sim.level[pin] != level) {
        sim.level[pin] = level;
        SIM_LOG("pin %d -> %s\n", pin, level ? "HIGH" : "LOW");
    }
    trace(pin, "", 1, (uint32_t)level);
}

static bool sim_turn_on(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    if (!get_pin(vm, "turn_on", args[0], &pin)) return false;
    set_level(vm, pin, 1);
    return true;
}

static bool sim_turn_off(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    if (!get_pin(vm, "turn_off", args[0], &pin)) return false;
    set_level(vm, pin, 0);
    return true;
}

static bool sim_toggle(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    if (!get_pin(vm, "toggle", args[0], &pin)) return false;
    set_level(vm, pin, sim.mode[pin] == MODE_OUT ? !sim.level[pin] : 1);
    return true;
}

static bool sim_brightness(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, pct;
    if (!get_pin(vm, "brightness", args[0], &pin) || !jl_get_int(vm, "brightness", 2, args[1], &pct)) return false;
    if (pct < 0 || pct > 100) return jl_error(vm, "brightness(): use a percentage from 0 to 100, got %d", (int)pct);
    sim.mode[pin] = MODE_OUT;
    SIM_LOG("pin %d brightness %d%%\n", (int)pin, (int)pct);
    trace(pin, "duty_percent", 7, (uint32_t)pct);
    return true;
}

static void log_color(JlVM *vm, int pin, int index, uint32_t rgb) {
    const char *name = jl_hw_color_name(rgb);
    char where[32] = "";
    if (index >= 0) snprintf(where, sizeof where, " LED %d", index);
    char what[24] = "rgb";
    if (index >= 0) snprintf(what, sizeof what, "led%d_rgb", index);
    trace(pin, what, 24, rgb);
    if (name) SIM_LOG("pin %d%s colour -> %s (#%06X)\n", pin, where, name, (unsigned)rgb);
    else SIM_LOG("pin %d%s colour -> #%06X\n", pin, where, (unsigned)rgb);
}

static bool sim_color(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    uint32_t rgb;
    if (!get_pin(vm, "color", args[0], &pin) || !jl_hw_color(vm, "color", 2, args[1], &rgb)) return false;
    log_color(vm, pin, -1, rgb);
    return true;
}

static bool sim_color_at(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, index;
    uint32_t rgb;
    if (!get_pin(vm, "color_at", args[0], &pin) || !jl_get_int(vm, "color_at", 2, args[1], &index) ||
        !jl_hw_color(vm, "color_at", 3, args[2], &rgb))
        return false;
    if (index < 0 || index > 63) return jl_error(vm, "color_at(): LED number must be 0..63, got %d", (int)index);
    log_color(vm, pin, index, rgb);
    return true;
}

static bool play(JlVM *vm, const char *fn, int32_t pin, Value note, int32_t ms) {
    int32_t hz;
    if (!jl_hw_note(vm, fn, 2, note, &hz)) return false;
    if (ms < 0) return jl_error(vm, "%s(): can't play for a negative time (%d ms)", fn, (int)ms);
    const char *s;
    uint32_t n;
    if (hz == 0) SIM_LOG("pin %d silent for %d ms\n", (int)pin, (int)ms);
    else if (note.type == VAL_STR && jl_get_string(vm, note, &s, &n))
        SIM_LOG("pin %d tone %d Hz (%.*s) for %d ms\n", (int)pin, (int)hz, (int)n, s, (int)ms);
    else SIM_LOG("pin %d tone %d Hz for %d ms\n", (int)pin, (int)hz, (int)ms);
    trace(pin, "tone_hz", 16, (uint32_t)hz);
    advance_time((uint32_t)ms);
    trace(pin, "tone_hz", 16, 0);
    return time_left(vm);
}

static bool sim_tone(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, ms;
    if (!get_pin(vm, "tone", args[0], &pin) || !jl_get_int(vm, "tone", 3, args[2], &ms)) return false;
    return play(vm, "tone", pin, args[1], ms);
}

static bool sim_beep(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    if (!get_pin(vm, "beep", args[0], &pin)) return false;
    return play(vm, "beep", pin, int_val(1000), 100);
}

static bool sim_servo(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, angle;
    if (!get_pin(vm, "servo", args[0], &pin) || !jl_get_int(vm, "servo", 2, args[1], &angle)) return false;
    if (angle < 0 || angle > 180) return jl_error(vm, "servo(): the angle must be 0 to 180 degrees, got %d", (int)angle);
    SIM_LOG("pin %d servo -> %d degrees\n", (int)pin, (int)angle);
    trace(pin, "servo_degrees", 8, (uint32_t)angle);
    return true;
}

/* pressed(pin): true once each time a button between the pin and ground
   is pressed. Simulate the button with --pin N=0. */
static bool sim_pressed(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pressed", args[0], &pin)) return false;
    sim.mode[pin] = MODE_IN_PULLUP;
    int level = sim.opt.pin_inputs[pin] < 0 ? 1 : sim.opt.pin_inputs[pin] != 0;
    int was = sim.last_pressed[pin] ? sim.last_pressed[pin] - 1 : 1;
    sim.last_pressed[pin] = level + 1;
    trace(pin, "in", 1, (uint32_t)level);
    *out = bool_val(was == 1 && level == 0);
    return true;
}

static bool sim_analog(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "analog", args[0], &pin)) return false;
    int v = sim.opt.analog_inputs[pin];
    trace(pin, "analog", 12, (uint32_t)(v < 0 ? 2048 : v));
    *out = int_val(v < 0 ? 2048 : v);
    return true;
}

static bool sim_distance(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t trig, echo;
    if (!get_pin(vm, "distance", args[0], &trig) || !get_pin(vm, "distance", args[1], &echo)) return false;
    trace(trig, "", 1, 1); /* a trigger pulse, then the echo comes back */
    trace(echo, "", 1, 0);
    advance_time(1 + (uint32_t)sim.opt.distance_cm * 58 / 1000); /* the echo takes 58 us per cm */
    trace(trig, "", 1, 0);
    trace(-1, "distance_cm", 16, (uint32_t)sim.opt.distance_cm);
    *out = int_val(sim.opt.distance_cm);
    return true;
}

/* ---- the LED panel ------------------------------------------------------ */

static bool sim_led(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t x, y;
    uint32_t rgb;
    if (!jl_get_int(vm, "led", 1, args[0], &x) || !jl_get_int(vm, "led", 2, args[1], &y) ||
        !jl_hw_color(vm, "led", 3, args[2], &rgb))
        return false;
    panel_set(&sim.panel, x, y, rgb); /* pixels off the panel are ignored */
    return true;
}

static bool sim_led_clear(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args, (void)out;
    panel_clear(&sim.panel);
    return true;
}

static void panel_write(void *ud, const char *data, size_t len) { jl_outf((JlVM *)ud, "%.*s", (int)len, data); }

static bool sim_led_show(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args, (void)out;
    panel_show(&sim.panel, sim.now_ms, panel_write, vm);
    trace(-1, "panel_frames", 16, (uint32_t)sim.panel.frames);
    return true;
}

static bool sim_led_width(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(sim.panel.w);
    return true;
}

static bool sim_led_height(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(sim.panel.h);
    return true;
}

/* led_size() -> [width, height], an array you free */
static bool sim_led_size(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    Value *items;
    if (!jl_new_array(vm, 2, out, &items)) return false;
    items[0] = int_val(sim.panel.w);
    items[1] = int_val(sim.panel.h);
    return true;
}

static uint32_t text_rgb;
static void plot(void *ud, int x, int y) { panel_set((Panel *)ud, x, y, text_rgb); }

/* led_text(msg, x, y, colour) draws msg (a string or a number) in the 5x7
   font and returns the x just after it, so text can be measured or scrolled. */
static bool sim_led_text(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t x, y;
    uint32_t rgb;
    const char *s;
    uint32_t n;
    char num[16];
    if (args[0].type == VAL_INT) {
        n = (uint32_t)snprintf(num, sizeof num, "%d", (int)args[0].as.i);
        s = num;
    } else if (args[0].type != VAL_STR || !jl_get_string(vm, args[0], &s, &n)) {
        return jl_error(vm, "led_text(): the text must be a string or a number, got %s", value_type_name(args[0]));
    }
    if (!jl_get_int(vm, "led_text", 2, args[1], &x) || !jl_get_int(vm, "led_text", 3, args[2], &y) ||
        !jl_hw_color(vm, "led_text", 4, args[3], &rgb))
        return false;
    text_rgb = rgb;
    *out = int_val(jl_hw_text(s, n, x, y, plot, &sim.panel));
    return true;
}

/* ---- memory-mapped I/O --------------------------------------------- */
/* The same registers as the RISC-V emulator (see rv.h), so code that
 * drives hardware through registers runs the same way in both. */

static bool get_addr(JlVM *vm, const char *fn, Value v, uint32_t *addr) {
    int32_t a;
    if (!jl_get_int(vm, fn, 1, v, &a)) return false;
    *addr = (uint32_t)a;
    return true;
}

static uint32_t sim_gpio_bits(int want_mode) {
    uint32_t v = 0;
    for (int pin = 0; pin < 32; pin++) {
        bool on;
        if (want_mode == MODE_OUT) on = sim.mode[pin] == MODE_OUT;
        else if (want_mode == MODE_IN_PULLUP) on = sim.mode[pin] == MODE_IN_PULLUP;
        else on = sim.level[pin] != 0;
        if (on) v |= 1u << pin;
    }
    return v;
}

static bool mmio_read(JlVM *vm, uint32_t addr, uint32_t *v) {
    switch (addr) {
    case RV_GPIO_DIR: *v = sim_gpio_bits(MODE_OUT); return true;
    case RV_GPIO_PULL: *v = sim_gpio_bits(MODE_IN_PULLUP); return true;
    case RV_GPIO_OUT: *v = sim_gpio_bits(-1); return true;
    case RV_GPIO_IN: {
        uint32_t r = 0;
        for (int pin = 0; pin < 32; pin++) {
            int level;
            if (sim.mode[pin] == MODE_OUT) level = sim.level[pin];
            else if (sim.opt.pin_inputs[pin] >= 0) level = sim.opt.pin_inputs[pin];
            else level = sim.mode[pin] == MODE_IN_PULLUP;
            if (level) r |= 1u << pin;
        }
        *v = r;
        return true;
    }
    case RV_TIMER_MS: *v = sim.now_ms; return true;
    case RV_UART_TX: case RV_TIMER_WAIT: *v = 0; return true;
    case RV_LED_XY: *v = sim.led_xy; return true;
    case RV_LED_W: *v = (uint32_t)sim.panel.w; return true;
    case RV_LED_H: *v = (uint32_t)sim.panel.h; return true;
    case RV_LED_RGB: case RV_LED_SHOW: case RV_LED_CLEAR: *v = 0; return true;
    }
    return jl_error(vm, "mmio_read32(): 0x%08X is not a register of the simulated board", (unsigned)addr);
}

static bool mmio_write(JlVM *vm, uint32_t addr, uint32_t v) {
    switch (addr) {
    case RV_UART_TX: {
        char c = (char)(v & 0xFF);
        jl_outf(vm, "%c", c);
        return true;
    }
    case RV_GPIO_DIR:
    case RV_GPIO_PULL:
        for (int pin = 0; pin < 32; pin++) {
            bool bit = (v >> pin) & 1u;
            if (addr == RV_GPIO_DIR) {
                if (bit) sim.mode[pin] = MODE_OUT;
                else if (sim.mode[pin] == MODE_OUT) sim.mode[pin] = MODE_IN;
            } else if (sim.mode[pin] != MODE_OUT) {
                sim.mode[pin] = bit ? MODE_IN_PULLUP : MODE_IN;
            }
        }
        return true;
    case RV_GPIO_OUT:
        for (int pin = 0; pin < 32; pin++) {
            int level = (v >> pin) & 1u;
            if (sim.mode[pin] == MODE_OUT && sim.level[pin] != level) {
                jl_outf(vm, "[sim %6ums] pin %d -> %s\n", (unsigned)sim.now_ms, pin, level ? "HIGH" : "LOW");
                trace(pin, "", 1, (uint32_t)level);
            }
            sim.level[pin] = level;
        }
        return true;
    case RV_TIMER_WAIT: advance_time(v); return true;
    case RV_LED_XY: sim.led_xy = v; return true;
    case RV_LED_RGB: panel_set(&sim.panel, (int16_t)(sim.led_xy >> 16), (int16_t)(sim.led_xy & 0xFFFF), v); return true;
    case RV_LED_SHOW: panel_show(&sim.panel, sim.now_ms, panel_write, vm); return true;
    case RV_LED_CLEAR: panel_clear(&sim.panel); return true;
    case RV_EXIT: return jl_error(vm, "the program wrote %u to the exit register", (unsigned)v);
    }
    return jl_error(vm, "mmio_write32(): 0x%08X is not a register of the simulated board", (unsigned)addr);
}

static bool sim_mmio_read32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    uint32_t addr, v;
    if (!get_addr(vm, "mmio_read32", args[0], &addr) || !mmio_read(vm, addr, &v)) return false;
    *out = int_val((int32_t)v);
    return true;
}

static bool sim_mmio_write32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    uint32_t addr;
    int32_t v;
    if (!get_addr(vm, "mmio_write32", args[0], &addr) || !jl_get_int(vm, "mmio_write32", 2, args[1], &v)) return false;
    return mmio_write(vm, addr, (uint32_t)v);
}

static bool sim_mmio_read8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    uint32_t addr, v;
    if (!get_addr(vm, "mmio_read8", args[0], &addr) || !mmio_read(vm, addr & ~3u, &v)) return false;
    *out = int_val((int32_t)((v >> (8 * (addr & 3u))) & 0xFFu));
    return true;
}

static bool sim_mmio_write8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    uint32_t addr;
    int32_t v;
    if (!get_addr(vm, "mmio_write8", args[0], &addr) || !jl_get_int(vm, "mmio_write8", 2, args[1], &v)) return false;
    return mmio_write(vm, addr, (uint32_t)v & 0xFFu);
}

static uint32_t sim_clock(void *ud) {
    (void)ud;
    return sim.now_ms;
}

JlResult board_sim_run_timers(JlVM *vm, uint32_t until_ms) {
    if (sim.opt.max_ms && (until_ms == 0 || until_ms > sim.opt.max_ms)) until_ms = sim.opt.max_ms;
    uint32_t due;
    while (jl_next_timer(vm, &due)) {
        if (until_ms && (int32_t)(due - until_ms) > 0) {
            jl_outf(vm, "[sim %6ums] timers stopped after %u seconds of simulated time. On a board they would keep running\n",
                    (unsigned)until_ms, (unsigned)(until_ms / 1000));
            if (sim.now_ms < until_ms) advance_time(until_ms - sim.now_ms);
            return JL_OK;
        }
        if ((int32_t)(due - sim.now_ms) > 0) advance_time(due - sim.now_ms);
        JlResult r = jl_run_timers(vm, sim.now_ms);
        if (r != JL_OK) return r;
    }
    return JL_OK;
}

void board_sim_register(JlVM *vm, const BoardSimOptions *o) {
    panel_free(&sim.panel);
    memset(&sim, 0, sizeof sim);
    sim.opt = *o;
    panel_init(&sim.panel, o->panel_w, o->panel_h);
    sim.panel.print = o->print_frames;
    sim.panel.ansi = o->ansi_frames;
    sim.panel.sink = o->frame_sink;
    sim.panel.sink_data = o->frame_data;
    jl_set_clock(vm, sim_clock, NULL);
    jl_register_native(vm, "pin_mode", sim_pin_mode, 2);
    jl_register_native(vm, "pin_write", sim_pin_write, 2);
    jl_register_native(vm, "pin_read", sim_pin_read, 1);
    jl_register_native(vm, "delay", sim_delay, 1);
    jl_register_native(vm, "millis", sim_millis, 0);
    jl_register_native(vm, "capture", sim_capture, 0);
    jl_register_native(vm, "ask", sim_ask, 2);
    jl_register_native(vm, "speak", sim_speak, -1);
    jl_register_native(vm, "mmio_read32", sim_mmio_read32, 1);
    jl_register_native(vm, "mmio_write32", sim_mmio_write32, 2);
    jl_register_native(vm, "mmio_read8", sim_mmio_read8, 1);
    jl_register_native(vm, "mmio_write8", sim_mmio_write8, 2);
    jl_register_native(vm, "turn_on", sim_turn_on, 1);
    jl_register_native(vm, "turn_off", sim_turn_off, 1);
    jl_register_native(vm, "toggle", sim_toggle, 1);
    jl_register_native(vm, "brightness", sim_brightness, 2);
    jl_register_native(vm, "color", sim_color, 2);
    jl_register_native(vm, "color_at", sim_color_at, 3);
    jl_register_native(vm, "tone", sim_tone, 3);
    jl_register_native(vm, "beep", sim_beep, 1);
    jl_register_native(vm, "servo", sim_servo, 2);
    jl_register_native(vm, "pressed", sim_pressed, 1);
    jl_register_native(vm, "analog", sim_analog, 1);
    jl_register_native(vm, "distance", sim_distance, 2);
    jl_register_native(vm, "led", sim_led, 3);
    jl_register_native(vm, "led_clear", sim_led_clear, 0);
    jl_register_native(vm, "led_show", sim_led_show, 0);
    jl_register_native(vm, "led_width", sim_led_width, 0);
    jl_register_native(vm, "led_height", sim_led_height, 0);
    jl_register_native(vm, "led_size", sim_led_size, 0);
    jl_register_native(vm, "led_text", sim_led_text, 4);
}
