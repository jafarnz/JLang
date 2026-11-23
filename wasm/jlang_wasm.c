/*
 * jlang_wasm.c - host for running JLang in a web browser.
 *
 * Built with `make wasm`. The page loads build/jlang.wasm in a Web
 * Worker, copies the source into wasm memory and calls jlw_run().
 * Output is collected in a buffer: program output as-is, error output
 * wrapped in \x02 ... \x03 so the page can colour it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/board_sim.h"
#include "../src/jlang.h"
#include "../src/rv.h"
#include "../src/rv_codegen.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define OUTPUT_MAX (512u * 1024u)

/* Frames from led_show(), one after another: the time in ms (u32), the
   width and height (u16 each), then width * height pixels as R, G, B bytes. */
#define FRAMES_MAX (24u * 1024u * 1024u)
#define SIM_SECONDS 60
#define TIMER_SECONDS 20
static uint8_t *frames;
static size_t frames_len, frames_cap;

static void on_frame(void *ud, uint32_t ms, int w, int h, const uint32_t *rgb) {
    (void)ud;
    size_t need = 8 + (size_t)w * (size_t)h * 3;
    if (frames_len + need > FRAMES_MAX) return;
    if (frames_len + need > frames_cap) {
        size_t cap = frames_cap ? frames_cap : 65536;
        while (cap < frames_len + need) cap *= 2;
        uint8_t *f = realloc(frames, cap);
        if (f == NULL) return;
        frames = f;
        frames_cap = cap;
    }
    uint8_t *p = frames + frames_len;
    p[0] = (uint8_t)ms, p[1] = (uint8_t)(ms >> 8), p[2] = (uint8_t)(ms >> 16), p[3] = (uint8_t)(ms >> 24);
    p[4] = (uint8_t)w, p[5] = (uint8_t)(w >> 8), p[6] = (uint8_t)h, p[7] = (uint8_t)(h >> 8);
    p += 8;
    for (int i = 0; i < w * h; i++) {
        *p++ = (uint8_t)(rgb[i] >> 16);
        *p++ = (uint8_t)(rgb[i] >> 8);
        *p++ = (uint8_t)rgb[i];
    }
    frames_len += need;
}

EXPORT(jlw_frames) const uint8_t *jlw_frames(void) { return frames; }
EXPORT(jlw_frames_len) size_t jlw_frames_len(void) { return frames_len; }

/* The pins as a waveform (VCD text), for the waveform view and download. */
#define VCD_MAX (8u * 1024u * 1024u)
static Vcd *vcd;
static char *vcd_buf;
static size_t vcd_len, vcd_cap;

static void vcd_append(void *ud, const char *d, size_t n) {
    (void)ud;
    if (vcd_len + n > VCD_MAX) return;
    if (vcd_len + n > vcd_cap) {
        size_t cap = vcd_cap ? vcd_cap : 8192;
        while (cap < vcd_len + n) cap *= 2;
        char *b = realloc(vcd_buf, cap);
        if (b == NULL) return;
        vcd_buf = b;
        vcd_cap = cap;
    }
    memcpy(vcd_buf + vcd_len, d, n);
    vcd_len += n;
}

/* Turns the recorded changes into text once the run is over. */
static void finish_vcd(void) {
    if (vcd) vcd_emit(vcd, vcd_append, NULL);
    vcd_free(vcd);
    vcd = NULL;
}

EXPORT(jlw_vcd) const char *jlw_vcd(void) { return vcd_buf; }
EXPORT(jlw_vcd_len) size_t jlw_vcd_len(void) { return vcd_len; }

static char *out_buf;
static size_t out_len, out_cap;
static bool truncated;

static void append(const char *d, size_t n) {
    if (truncated) return;
    if (out_len + n + 64 > OUTPUT_MAX) {
        const char *msg = "\x02\n[output truncated]\x03";
        n = 0;
        d = NULL;
        truncated = true;
        size_t m = strlen(msg);
        if (out_len + m + 1 > out_cap) {
            char *b = realloc(out_buf, out_len + m + 1);
            if (b == NULL) return;
            out_buf = b;
            out_cap = out_len + m + 1;
        }
        memcpy(out_buf + out_len, msg, m);
        out_len += m;
        return;
    }
    if (out_len + n + 1 > out_cap) {
        size_t cap = out_cap ? out_cap : 4096;
        while (cap < out_len + n + 1) cap *= 2;
        char *b = realloc(out_buf, cap);
        if (b == NULL) return;
        out_buf = b;
        out_cap = cap;
    }
    memcpy(out_buf + out_len, d, n);
    out_len += n;
}

static void write_out(void *ud, const char *d, size_t n) {
    (void)ud;
    append(d, n);
}

static void write_err(void *ud, const char *d, size_t n) {
    (void)ud;
    append("\x02", 1);
    append(d, n);
    append("\x03", 1);
}

EXPORT(jlw_alloc) void *jlw_alloc(size_t n) { return malloc(n); }
EXPORT(jlw_free) void jlw_free(void *p) { free(p); }
EXPORT(jlw_output) const char *jlw_output(void) { return out_buf; }
EXPORT(jlw_output_len) size_t jlw_output_len(void) { return out_len; }

/*
 * mode:    0 = run, 1 = xray, 2 = dis, 3 = check,
 *          4 = RISC-V assembly, 5 = run on the RISC-V emulator
 * emits:   event names separated by '\n' (may be empty)
 * pins:    "4=1,5=0" (may be empty); "a1=3000" sets analog(1) and
 *          "d=42" sets distance()
 * answer:  reply of the simulated ask() (empty = default)
 * Returns the same exit codes as the jlang command.
 */
EXPORT(jlw_run)
int jlw_run(const char *src, int mode, const char *emits, const char *pins,
            const char *answer, uint32_t heap, int stats) {
    out_len = 0;
    truncated = false;
    frames_len = 0;
    vcd_len = 0;
    vcd_free(vcd);
    vcd = vcd_new();

    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = heap >= 64 ? heap : JL_DEFAULT_HEAP;
    cfg.max_cycles = 50000000; /* keeps the browser tab responsive */
    cfg.out = write_out;
    cfg.err = write_err;
    JlVM *vm = jl_new(&cfg);
    if (vm == NULL) {
        write_err(NULL, "could not create the VM\n", 24);
        return 70;
    }

    BoardSimOptions opt;
    board_sim_default_options(&opt);
    if (answer != NULL && answer[0] != '\0') opt.ai_answer = answer;
    opt.print_frames = false; /* the page draws the frames */
    opt.frame_sink = on_frame;
    opt.max_ms = SIM_SECONDS * 1000;
    opt.vcd = vcd;
    for (const char *p = pins; p != NULL && *p;) {
        char *end;
        if (*p == 'd' && p[1] == '=') { /* d=CM: what distance() measures */
            opt.distance_cm = (int)strtol(p + 2, NULL, 10);
        } else if (*p == 't' && p[1] == '=') { /* t=C: the I2C temperature sensor */
            opt.temperature_c = (int)strtol(p + 2, NULL, 10);
        } else if (*p == 'a') { /* aN=V: what analog(N) reads */
            long pin = strtol(p + 1, &end, 10);
            if (*end == '=' && pin >= 0 && pin < SIM_PINS) {
                long v = strtol(end + 1, NULL, 10);
                opt.analog_inputs[pin] = (int)(v < 0 ? 0 : v > 4095 ? 4095 : v);
            }
        } else {
            long pin = strtol(p, &end, 10);
            if (*end == '=' && pin >= 0 && pin < SIM_PINS) {
                opt.pin_inputs[pin] = end[1] == '1' ? 1 : 0;
            }
        }
        p = strchr(p, ',');
        if (p) p++;
    }
    board_sim_register(vm, &opt);

    int code = 0;
    if (mode == 4 || mode == 5) {
        const char *events[64];
        int nevents = 0;
        static char names[64][32];
        for (const char *p = emits; p != NULL && *p && nevents < 64;) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (n > 0 && n < 32) {
                memcpy(names[nevents], p, n);
                names[nevents][n] = '\0';
                events[nevents] = names[nevents];
                nevents++;
            }
            p = nl ? nl + 1 : NULL;
        }
        RvGenOptions gen = {true, events, nevents};
        char *assembly = rv_generate(vm, src, &gen);
        jl_free(vm);
        if (assembly == NULL) return 65;
        if (mode == 4) {
            write_out(NULL, assembly, strlen(assembly));
            free(assembly);
            return 0;
        }
        RvImage img;
        if (!rv_assemble(assembly, &img, false, false)) {
            write_err(NULL, img.error, strlen(img.error));
            write_err(NULL, "\n", 1);
            free(assembly);
            return 65;
        }
        free(assembly);
        RvRunOptions run;
        memset(&run, 0, sizeof run);
        for (int i = 0; i < 32; i++) run.pin_inputs[i] = opt.pin_inputs[i];
        run.max_cycles = 200000000;
        run.out = write_out;
        run.err = write_err;
        run.frame_sink = on_frame;
        run.max_ms = SIM_SECONDS * 1000;
        run.vcd = vcd;
        uint64_t cycles = 0;
        code = rv_run(&img, &run, &cycles);
        finish_vcd();
        if (stats) {
            char msg[128];
            int m = snprintf(msg, sizeof msg, "RISC-V instructions executed: %llu, code size: %u bytes\n",
                             (unsigned long long)cycles, img.text_size);
            write_err(NULL, msg, (size_t)m);
        }
        rv_image_free(&img);
        return code;
    }
    if (mode == 2 || mode == 3) {
        JlResult r = jl_disassemble_or_check(vm, src, mode == 2);
        if (r != JL_OK) code = 65;
        else if (mode == 3) write_out(NULL, "ok: no errors\n", 14);
        jl_free(vm);
        append("", 0);
        return code;
    }

    jl_set_trace(vm, mode == 1);
    JlResult r = jl_run_source(vm, src);
    if (r == JL_COMPILE_ERROR) code = 65;
    if (r == JL_RUNTIME_ERROR) code = 70;

    char name[64];
    for (const char *p = emits; code == 0 && p != NULL && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 0 && n < sizeof name) {
            memcpy(name, p, n);
            name[n] = '\0';
            r = jl_emit(vm, name);
            if (r == JL_NO_HANDLER) {
                char msg[160];
                int m = snprintf(msg, sizeof msg,
                                 "event '%s' has no handler (add: on %s { ... })\n", name, name);
                write_err(NULL, msg, (size_t)m);
                code = 70;
            } else if (r != JL_OK) {
                code = 70;
            }
        }
        p = nl ? nl + 1 : NULL;
    }

    /* every and after blocks, for up to TIMER_SECONDS of simulated time. */
    if (code == 0 && board_sim_run_timers(vm, TIMER_SECONDS * 1000) != JL_OK) code = 70;

    if (code != 65) {
        if (stats) jl_print_stats(vm);
        if (code == 0) jl_report_leaks(vm);
    }
    jl_free(vm);
    finish_vcd();
    return code;
}
