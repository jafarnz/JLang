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
#ifndef JL_NO_REALTIME
#include "port.h"
#endif
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
    Oled oled;
    uint32_t led_xy;
    char kv_keys[SIM_KV_MAX][SIM_KV_KEY_MAX];
    char kv_vals[SIM_KV_MAX][SIM_KV_VAL_MAX];
    int kv_count;
    bool recording;
    uint32_t rec_start_ms;
    bool wifi_connected;
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
    o->voice_text = "What is this?";
    for (int i = 0; i < SIM_PINS; i++) o->analog_inputs[i] = -1;
    o->distance_cm = 100;
    o->temperature_c = 24;
    o->panel_w = PANEL_DEFAULT_W;
    o->panel_h = PANEL_DEFAULT_H;
    o->print_frames = true;
}

static void advance_time(uint32_t ms) {
    sim.now_ms += ms;
#ifndef JL_NO_REALTIME
    if (sim.opt.realtime && ms > 0) jl_sleep_ms(ms);
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

/* record(ms) -> buf holding a simulated microphone recording. Free it when done. */
static bool make_voice_buf(JlVM *vm, Value *out, uint32_t *out_total) {
    const char *v = sim.opt.voice_text ? sim.opt.voice_text : "What is this?";
    size_t vlen = strlen(v);
    uint32_t total = (uint32_t)(4 + vlen);
    uint8_t *data;
    if (!jl_new_buf(vm, total, out, &data)) return false;
    memcpy(data, "JLAV", 4);
    memcpy(data + 4, v, vlen);
    if (out_total) *out_total = total;
    return true;
}

static bool sim_record(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t ms;
    if (!jl_get_int(vm, "record", 1, args[0], &ms)) return false;
    if (ms <= 0 || ms > 30000) {
        return jl_error(vm, "record(): duration must be 1 to 30000 ms, got %d", (int)ms);
    }
    uint32_t total = 0;
    if (!make_voice_buf(vm, out, &total)) return false;
    advance_time((uint32_t)ms);
    jl_outf(vm, "[sim %6ums] mic: recorded %d ms (%u bytes)\n", (unsigned)sim.now_ms, (int)ms, (unsigned)total);
    return time_left(vm);
}

/* record_start() / record_stop(): push-to-talk microphone capture. */
static bool sim_record_start(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args, (void)out;
    if (sim.recording) return jl_error(vm, "record_start(): microphone is already recording");
    sim.recording = true;
    sim.rec_start_ms = sim.now_ms;
    jl_outf(vm, "[sim %6ums] mic: recording started\n", (unsigned)sim.now_ms);
    return true;
}

static bool sim_record_stop(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    if (!sim.recording) return jl_error(vm, "record_stop(): microphone is not recording (call record_start() first)");
    sim.recording = false;
    uint32_t elapsed = sim.now_ms - sim.rec_start_ms;
    if (elapsed == 0) {
        /* Simulated press-and-release with no delay() in between. */
        elapsed = 1200;
        advance_time(elapsed);
    }
    uint32_t total = 0;
    if (!make_voice_buf(vm, out, &total)) return false;
    jl_outf(vm, "[sim %6ums] mic: recording stopped (%u ms, %u bytes)\n",
            (unsigned)sim.now_ms, (unsigned)elapsed, (unsigned)total);
    return time_left(vm);
}

/* ask(photo_or_nil, prompt_or_audio [, extra_prompt]) -> heap str. Free it when done. */
static bool sim_ask(JlVM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || argc > 3) {
        return jl_error(vm, "ask() takes 2 or 3 arguments: (photo_or_nil, prompt_or_audio[, instruction])");
    }
    bool has_photo = args[0].type != VAL_NIL;
    if (has_photo) {
        uint8_t *pdata;
        uint32_t plen;
        if (args[0].type != VAL_BUF) {
            return jl_error(vm, "ask(): argument 1 must be a photo buf from capture() or nil, got %s",
                            value_type_name(args[0]));
        }
        if (!jl_get_bytes(vm, "ask", args[0], &pdata, &plen)) return false;
    }
    const char *extra = NULL;
    uint32_t extra_len = 0;
    if (argc == 3) {
        if (args[2].type != VAL_STR || !jl_get_string(vm, args[2], &extra, &extra_len)) {
            return jl_error(vm, "ask(): argument 3 must be an instruction string");
        }
    }
    if (args[1].type == VAL_STR) {
        const char *prompt;
        uint32_t n;
        if (!jl_get_string(vm, args[1], &prompt, &n)) {
            return jl_error(vm, "ask(): argument 2 must be a live string or audio buf");
        }
        jl_outf(vm, "[sim %6ums] AI: sending %s\"%.*s\"\n", (unsigned)sim.now_ms,
                has_photo ? "photo + " : "", (int)n, prompt);
    } else if (args[1].type == VAL_BUF) {
        uint8_t *adata;
        uint32_t alen;
        if (!jl_get_bytes(vm, "ask", args[1], &adata, &alen)) return false;
        if (alen >= 4 && memcmp(adata, "JLAV", 4) == 0) {
            jl_outf(vm, "[sim %6ums] AI: sending %saudio (\"%.*s\")%s%.*s%s\n", (unsigned)sim.now_ms,
                    has_photo ? "photo + " : "", (int)(alen - 4), (const char *)(adata + 4),
                    extra ? " + \"" : "", (int)extra_len, extra ? extra : "", extra ? "\"" : "");
        } else {
            jl_outf(vm, "[sim %6ums] AI: sending %saudio (%u bytes)%s%.*s%s\n", (unsigned)sim.now_ms,
                    has_photo ? "photo + " : "", (unsigned)alen,
                    extra ? " + \"" : "", (int)extra_len, extra ? extra : "", extra ? "\"" : "");
        }
    } else {
        return jl_error(vm, "ask(): argument 2 must be a question string or an audio buf from record()");
    }
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

/* chime("ready"|"working"|"error") */
static bool sim_chime(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    const char *kind;
    uint32_t n;
    if (args[0].type != VAL_STR || !jl_get_string(vm, args[0], &kind, &n)) {
        return jl_error(vm, "chime(): argument 1 must be \"ready\", \"working\" or \"error\"");
    }
    bool ok = (n == 5 && memcmp(kind, "ready", 5) == 0) ||
              (n == 7 && memcmp(kind, "working", 7) == 0) ||
              (n == 5 && memcmp(kind, "error", 5) == 0);
    if (!ok) return jl_error(vm, "chime(): expected \"ready\", \"working\" or \"error\", got \"%.*s\"", (int)n, kind);
    jl_outf(vm, "[sim %6ums] chime: %.*s\n", (unsigned)sim.now_ms, (int)n, kind);
    advance_time(120);
    return true;
}

/* ---- persistent key-value store (NVS on ESP32, in-memory here) ------- */

static bool kv_check_key(JlVM *vm, const char *fn, Value v, const char **k, uint32_t *n) {
    if (v.type != VAL_STR || !jl_get_string(vm, v, k, n)) {
        return jl_error(vm, "%s(): key must be a string", fn);
    }
    if (*n == 0 || *n >= SIM_KV_KEY_MAX) {
        return jl_error(vm, "%s(): key length must be 1 to %d characters", fn, SIM_KV_KEY_MAX - 1);
    }
    return true;
}

static int kv_find(const char *k, uint32_t n) {
    for (int i = 0; i < sim.kv_count; i++) {
        if (strlen(sim.kv_keys[i]) == n && memcmp(sim.kv_keys[i], k, n) == 0) return i;
    }
    return -1;
}

static bool sim_kv_set(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    const char *k, *v;
    uint32_t kn, vn;
    if (!kv_check_key(vm, "kv_set", args[0], &k, &kn)) return false;
    if (args[1].type != VAL_STR || !jl_get_string(vm, args[1], &v, &vn)) {
        return jl_error(vm, "kv_set(): value must be a string");
    }
    if (vn >= SIM_KV_VAL_MAX) {
        return jl_error(vm, "kv_set(): value can be at most %d bytes", SIM_KV_VAL_MAX - 1);
    }
    int idx = kv_find(k, kn);
    if (idx < 0) {
        if (sim.kv_count == SIM_KV_MAX) return jl_error(vm, "kv_set(): storage is full (max %d keys)", SIM_KV_MAX);
        idx = sim.kv_count++;
        memcpy(sim.kv_keys[idx], k, kn);
        sim.kv_keys[idx][kn] = '\0';
    }
    memcpy(sim.kv_vals[idx], v, vn);
    sim.kv_vals[idx][vn] = '\0';
    return true;
}

/* kv_get(key) -> heap str or nil; kv_get(key, default) -> heap str (always free it). */
static bool sim_kv_get(JlVM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || argc > 2) return jl_error(vm, "kv_get() takes 1 or 2 arguments: (key[, default])");
    const char *k;
    uint32_t kn;
    if (!kv_check_key(vm, "kv_get", args[0], &k, &kn)) return false;
    int idx = kv_find(k, kn);
    if (idx >= 0) {
        return jl_new_string(vm, sim.kv_vals[idx], (uint32_t)strlen(sim.kv_vals[idx]), out);
    }
    if (argc == 2) {
        const char *d;
        uint32_t dn;
        if (args[1].type != VAL_STR || !jl_get_string(vm, args[1], &d, &dn)) {
            return jl_error(vm, "kv_get(): default value must be a string");
        }
        return jl_new_string(vm, d, dn, out);
    }
    *out = nil_val();
    return true;
}

static bool sim_kv_del(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *k;
    uint32_t kn;
    if (!kv_check_key(vm, "kv_del", args[0], &k, &kn)) return false;
    int idx = kv_find(k, kn);
    if (idx < 0) {
        *out = bool_val(false);
        return true;
    }
    sim.kv_count--;
    if (idx < sim.kv_count) {
        memcpy(sim.kv_keys[idx], sim.kv_keys[sim.kv_count], SIM_KV_KEY_MAX);
        memcpy(sim.kv_vals[idx], sim.kv_vals[sim.kv_count], SIM_KV_VAL_MAX);
    }
    *out = bool_val(true);
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

/* ---- I2C and SPI ------------------------------------------------------------
 *
 * The simulated I2C bus has three devices on it, so programs can talk to
 * real chips' registers:
 *   0x48  a TMP102 temperature sensor: register 0 holds the temperature
 *         as a 12-bit number of 1/16 degrees, in the top bits of 2 bytes
 *   0x68  an MPU-6050 motion sensor: WHO_AM_I (0x75) is 0x68; it starts
 *         asleep, and writing 0 to PWR_MGMT_1 (0x6B) wakes it, after which
 *         the accelerometer (0x3B..0x40) reads 1 g (16384) on the z axis
 *   0x3C  an SH1106 OLED display, which accepts whatever is written
 */

static struct {
    uint8_t mpu[128]; /* MPU-6050 registers */
    uint8_t ptr[128]; /* register pointer of each device */
    bool ready;
} bus;

static bool i2c_present(int addr) { return addr == 0x48 || addr == 0x68 || addr == 0x3C; }

static void bus_setup(void) {
    if (bus.ready) return;
    memset(&bus, 0, sizeof bus);
    bus.mpu[0x75] = 0x68; /* WHO_AM_I */
    bus.mpu[0x6B] = 0x40; /* PWR_MGMT_1: asleep */
    bus.ready = true;
}

static uint8_t i2c_reg(int addr, int reg) {
    if (addr == 0x48) {
        int raw = sim.opt.temperature_c * 16; /* 1/16 degree steps */
        if (reg == 0x00) return (uint8_t)(raw >> 4);
        if (reg == 0x01) return (uint8_t)(raw << 4);
        if (reg == 0x02) return 0x60; /* configuration register */
        if (reg == 0x03) return 0xA0;
        return 0;
    }
    if (addr == 0x68) {
        bool awake = !(bus.mpu[0x6B] & 0x40);
        if (reg >= 0x3B && reg <= 0x40) {
            if (!awake) return 0;
            return reg == 0x3F ? 0x40 : 0; /* z = 0x4000 = 16384 = 1 g */
        }
        return bus.mpu[reg & 0x7F];
    }
    return 0;
}

static bool get_byte(JlVM *vm, const char *fn, int arg, Value v, int32_t *out) {
    if (!jl_get_int(vm, fn, arg, v, out)) return false;
    if (*out < 0 || *out > 255) return jl_error(vm, "%s(): argument %d must be a byte (0 to 255), got %d", fn, arg, (int)*out);
    return true;
}

static bool get_i2c_addr(JlVM *vm, const char *fn, Value v, int32_t *addr) {
    if (!jl_get_int(vm, fn, 1, v, addr)) return false;
    if (*addr < 0 || *addr > 127)
        return jl_error(vm, "%s(): I2C addresses are 7 bits (0 to 127, or 0x00 to 0x7F), got %d", fn, (int)*addr);
    if (!i2c_present(*addr))
        return jl_error(vm, "%s(): no device answered at address 0x%02X (i2c_scan() lists the ones that do)", fn,
                        (unsigned)*addr);
    return true;
}

/* i2c_scan(): the addresses of the devices on the bus, as an array you free. */
static bool sim_i2c_scan(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    static const int found[] = {0x3C, 0x48, 0x68};
    Value *items;
    if (!jl_new_array(vm, 3, out, &items)) return false;
    for (int i = 0; i < 3; i++) items[i] = int_val(found[i]);
    SIM_LOG("i2c scan found 0x3C, 0x48, 0x68\n");
    return true;
}

/* i2c_write(addr, byte, ...): the first byte is usually a register number. */
static bool sim_i2c_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    int32_t addr;
    if (argc < 2) return jl_error(vm, "i2c_write(): give an address and at least one byte, like i2c_write(0x68, 0x6B, 0)");
    if (!get_i2c_addr(vm, "i2c_write", args[0], &addr)) return false;
    bus_setup();
    char hex[3 * 16 + 8] = "";
    size_t n = 0;
    for (int i = 1; i < argc; i++) {
        int32_t b;
        if (!get_byte(vm, "i2c_write", i + 1, args[i], &b)) return false;
        if (i == 1) bus.ptr[addr] = (uint8_t)b;
        else if (addr == 0x68) bus.mpu[(bus.ptr[addr] + i - 2) & 0x7F] = (uint8_t)b;
        if (i <= 16) n += (size_t)snprintf(hex + n, sizeof hex - n, " %02X", (unsigned)b);
    }
    SIM_LOG("i2c 0x%02X write%s%s\n", (unsigned)addr, hex, argc > 17 ? " ..." : "");
    return true;
}

/* i2c_write_buf(addr, prefix, buf, off, count): bulk transfer from a buf or str. */
static bool sim_i2c_write_buf(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)out;
    int32_t addr, prefix, off, count;
    uint8_t *bytes = NULL;
    uint32_t len = 0;
    if (!get_i2c_addr(vm, "i2c_write_buf", args[0], &addr) ||
        !get_byte(vm, "i2c_write_buf", 2, args[1], &prefix) ||
        !jl_get_bytes(vm, "i2c_write_buf", args[2], &bytes, &len) ||
        !jl_get_int(vm, "i2c_write_buf", 4, args[3], &off) ||
        !jl_get_int(vm, "i2c_write_buf", 5, args[4], &count)) return false;
    if (off < 0 || count < 1 || count > 256 || (uint32_t)off + (uint32_t)count > len) {
        return jl_error(vm, "i2c_write_buf(): slice off=%d count=%d is out of bounds (buffer has %u bytes, max count 256)",
                        (int)off, (int)count, (unsigned)len);
    }
    bus_setup();
    bus.ptr[addr] = (uint8_t)prefix;
    if (addr == 0x68) {
        for (int32_t i = 0; i < count; i++) bus.mpu[(prefix + i) & 0x7F] = bytes[off + i];
    }
    SIM_LOG("i2c 0x%02X write_buf 0x%02X (%d bytes)\n", (unsigned)addr, (unsigned)prefix, (int)count);
    return true;
}

static void sim_oled_write(void *ud, const char *data, size_t len) {
    jl_outf((JlVM *)ud, "%.*s", (int)len, data);
}

static bool sim_oled_show_hook(JlVM *vm, Oled *o, void *ud) {
    (void)ud;
    oled_show(o, sim.now_ms, sim_oled_write, vm);
    return true;
}

/* i2c_read(addr, reg): one byte from a register. */
static bool sim_i2c_read(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t addr, reg;
    if (!get_i2c_addr(vm, "i2c_read", args[0], &addr) || !get_byte(vm, "i2c_read", 2, args[1], &reg)) return false;
    bus_setup();
    uint8_t v = i2c_reg(addr, reg);
    SIM_LOG("i2c 0x%02X read register 0x%02X -> 0x%02X\n", (unsigned)addr, (unsigned)reg, (unsigned)v);
    *out = int_val(v);
    return true;
}

/* i2c_read16(addr, reg): two bytes, high byte first, as most sensors send them. */
static bool sim_i2c_read16(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t addr, reg;
    if (!get_i2c_addr(vm, "i2c_read16", args[0], &addr) || !get_byte(vm, "i2c_read16", 2, args[1], &reg)) return false;
    bus_setup();
    uint16_t v = (uint16_t)(i2c_reg(addr, reg) << 8 | i2c_reg(addr, (reg + 1) & 0xFF));
    SIM_LOG("i2c 0x%02X read register 0x%02X -> 0x%04X\n", (unsigned)addr, (unsigned)reg, (unsigned)v);
    *out = int_val(v);
    return true;
}

/* spi_write(cs, byte, ...): select the chip on pin cs, send the bytes. */
static bool sim_spi_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    int32_t cs;
    if (argc < 2) return jl_error(vm, "spi_write(): give the chip-select pin and at least one byte");
    if (!get_pin(vm, "spi_write", args[0], &cs)) return false;
    char hex[3 * 16 + 8] = "";
    size_t n = 0;
    for (int i = 1; i < argc; i++) {
        int32_t b;
        if (!get_byte(vm, "spi_write", i + 1, args[i], &b)) return false;
        if (i <= 16) n += (size_t)snprintf(hex + n, sizeof hex - n, " %02X", (unsigned)b);
    }
    SIM_LOG("spi cs %d write%s%s\n", (int)cs, hex, argc > 17 ? " ..." : "");
    return true;
}

/* spi_transfer(cs, byte): send one byte and return the one received at the
   same time. Nothing is connected in the simulator, so it reads 0xFF. */
static bool sim_spi_transfer(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t cs, b;
    if (!get_pin(vm, "spi_transfer", args[0], &cs) || !get_byte(vm, "spi_transfer", 2, args[1], &b)) return false;
    SIM_LOG("spi cs %d sent %02X, received FF\n", (int)cs, (unsigned)b);
    *out = int_val(0xFF);
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

static bool sim_wifi_connect(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *ssid, *pass;
    uint32_t sn, pn;
    if (!jl_get_string(vm, args[0], &ssid, &sn) || !jl_get_string(vm, args[1], &pass, &pn))
        return jl_error(vm, "wifi_connect(): expected (ssid, password) strings");
    if (sn == 0 || sn > 32)
        return jl_error(vm, "wifi_connect(): ssid must be 1 to 32 bytes");
    if (pn > 64)
        return jl_error(vm, "wifi_connect(): password must be at most 64 bytes");
    advance_time(250);
    sim.wifi_connected = true;
    jl_outf(vm, "[sim %6ums] wifi connect \"%.*s\" -> 192.168.1.42\n",
            (unsigned)sim.now_ms, (int)sn, ssid);
    *out = bool_val(true);
    return true;
}

static bool sim_wifi_ip(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    if (!sim.wifi_connected) {
        *out = nil_val();
        return true;
    }
    return jl_new_string(vm, "192.168.1.42", 12, out);
}

static bool sim_http_get(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *url;
    uint32_t un;
    if (!jl_get_string(vm, args[0], &url, &un))
        return jl_error(vm, "http_get(): url must be a string");
    if (un == 0 || un > 512)
        return jl_error(vm, "http_get(): url must be 1 to 512 bytes");
    if (!sim.wifi_connected) {
        *out = nil_val();
        return true;
    }
    advance_time(120);
    jl_outf(vm, "[sim %6ums] http_get %.*s -> 200 OK\n", (unsigned)sim.now_ms, (int)un, url);
    /* Return a realistic MUIS Singapore sample when testing the prayer API. */
    bool is_prayer = false;
    for (uint32_t i = 0; i + 6 <= un; i++) {
        if (memcmp(url + i, "prayer", 6) == 0) {
            is_prayer = true;
            break;
        }
    }
    const char *body = is_prayer
                           ? "18:54:55|MAGHRIB|18:55|5|05:44|12:55|16:18|18:55|20:09"
                           : "OK";
    return jl_new_string(vm, body, (uint32_t)strlen(body), out);
}

void board_sim_register(JlVM *vm, const BoardSimOptions *o) {
    panel_free(&sim.panel);
    memset(&sim, 0, sizeof sim);
    sim.opt = *o;
    sim.kv_count = o->kv_count < SIM_KV_MAX ? o->kv_count : SIM_KV_MAX;
    for (int i = 0; i < sim.kv_count; i++) {
        memcpy(sim.kv_keys[i], o->kv_keys[i], SIM_KV_KEY_MAX);
        memcpy(sim.kv_vals[i], o->kv_vals[i], SIM_KV_VAL_MAX);
    }
    panel_init(&sim.panel, o->panel_w, o->panel_h);
    sim.panel.print = o->print_frames;
    sim.panel.ansi = o->ansi_frames;
    sim.panel.sink = o->frame_sink;
    sim.panel.sink_data = o->frame_data;
    oled_init(&sim.oled);
    sim.oled.print = o->print_frames;
    sim.oled.sink = o->oled_sink;
    sim.oled.sink_data = o->oled_data;
    jl_set_clock(vm, sim_clock, NULL);
    jl_register_native(vm, "pin_mode", sim_pin_mode, 2);
    jl_register_native(vm, "pin_write", sim_pin_write, 2);
    jl_register_native(vm, "pin_read", sim_pin_read, 1);
    jl_register_native(vm, "delay", sim_delay, 1);
    jl_register_native(vm, "millis", sim_millis, 0);
    jl_register_native(vm, "capture", sim_capture, 0);
    jl_register_native(vm, "record", sim_record, 1);
    jl_register_native(vm, "record_start", sim_record_start, 0);
    jl_register_native(vm, "record_stop", sim_record_stop, 0);
    jl_register_native(vm, "ask", sim_ask, -1);
    jl_register_native(vm, "speak", sim_speak, -1);
    jl_register_native(vm, "chime", sim_chime, 1);
    jl_register_native(vm, "kv_set", sim_kv_set, 2);
    jl_register_native(vm, "kv_get", sim_kv_get, -1);
    jl_register_native(vm, "kv_del", sim_kv_del, 1);
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
    oled_register_natives(vm, &sim.oled, sim_oled_show_hook, NULL);
    jl_register_native(vm, "i2c_scan", sim_i2c_scan, 0);
    jl_register_native(vm, "i2c_write", sim_i2c_write, -1);
    jl_register_native(vm, "i2c_write_buf", sim_i2c_write_buf, 5);
    jl_register_native(vm, "i2c_read", sim_i2c_read, 2);
    jl_register_native(vm, "i2c_read16", sim_i2c_read16, 2);
    jl_register_native(vm, "spi_write", sim_spi_write, -1);
    jl_register_native(vm, "spi_transfer", sim_spi_transfer, 2);
    jl_register_native(vm, "wifi_connect", sim_wifi_connect, 2);
    jl_register_native(vm, "wifi_ip", sim_wifi_ip, 0);
    jl_register_native(vm, "http_get", sim_http_get, 1);
    bus.ready = false;
}
