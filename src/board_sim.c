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

#include <string.h>
#include <time.h>

#include "board_sim.h"

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
} sim;

void board_sim_default_options(BoardSimOptions *o) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < SIM_PINS; i++) o->pin_inputs[i] = -1;
    o->ai_answer = "Bus 179 is arriving.";
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
                        fn, *pin, SIM_PINS - 1);
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
                            "pin_mode(%d, \"out\") first", pin, pin);
    }
    int level;
    if (args[1].type == VAL_BOOL) level = args[1].as.b;
    else if (args[1].type == VAL_INT && (args[1].as.i == 0 || args[1].as.i == 1)) level = args[1].as.i;
    else return jl_error(vm, "pin_write(): a pin can only be 0 (LOW) or 1 (HIGH)");

    if (sim.level[pin] != level) {
        sim.level[pin] = level;
        jl_outf(vm, "[sim %6ums] pin %d -> %s\n", sim.now_ms, pin, level ? "HIGH" : "LOW");
    }
    return true;
}

static bool sim_pin_read(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pin_read", args[0], &pin)) return false;
    if (sim.mode[pin] != MODE_IN && sim.mode[pin] != MODE_IN_PULLUP) {
        return jl_error(vm, "pin_read(): pin %d is not an input. Call "
                            "pin_mode(%d, \"in\") first", pin, pin);
    }
    int v = sim.opt.pin_inputs[pin];
    if (v < 0) v = sim.mode[pin] == MODE_IN_PULLUP ? 1 : 0; /* pull-up reads HIGH */
    *out = int_val(v);
    return true;
}

static bool sim_delay(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t ms;
    if (!jl_get_int(vm, "delay", 1, args[0], &ms)) return false;
    if (ms < 0) return jl_error(vm, "delay(): can't wait a negative time (%d ms)", ms);
    advance_time((uint32_t)ms);
    return true;
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
    jl_outf(vm, "[sim %6ums] camera: photo #%d captured (%u bytes)\n", sim.now_ms,
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
    jl_outf(vm, "[sim %6ums] AI: sending photo + \"%.*s\"\n", sim.now_ms, (int)n, prompt);
    advance_time(ASK_MS);
    const char *a = sim.opt.ai_answer;
    if (!jl_new_string(vm, a, (uint32_t)strlen(a), out)) return false;
    jl_outf(vm, "[sim %6ums] AI: answered\n", sim.now_ms);
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
    jl_outf(vm, "[sim %6ums] speaker (%.*s): \"%.*s\"\n", sim.now_ms, (int)ln, lang,
            (int)n, text);
    advance_time(60u * n); /* about 60 ms per character of speech */
    return true;
}

void board_sim_register(JlVM *vm, const BoardSimOptions *o) {
    memset(&sim, 0, sizeof sim);
    sim.opt = *o;
    jl_register_native(vm, "pin_mode", sim_pin_mode, 2);
    jl_register_native(vm, "pin_write", sim_pin_write, 2);
    jl_register_native(vm, "pin_read", sim_pin_read, 1);
    jl_register_native(vm, "delay", sim_delay, 1);
    jl_register_native(vm, "millis", sim_millis, 0);
    jl_register_native(vm, "capture", sim_capture, 0);
    jl_register_native(vm, "ask", sim_ask, 2);
    jl_register_native(vm, "speak", sim_speak, -1);
}
