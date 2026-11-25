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
#include "../src/vm.h"

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

/* Frames from oled_show(): ms (u32 LE), flags (u8: bit 0 = inverted),
   contrast (u8), followed by the 1,024-byte 128x64 page framebuffer. */
#define OLED_FRAMES_MAX (8u * 1024u * 1024u)
static uint8_t *oled_frames;
static size_t oled_frames_len, oled_frames_cap;

static void on_oled_frame(void *ud, uint32_t ms, const uint8_t *fb, bool inverted, uint8_t contrast) {
    (void)ud;
    size_t need = 6 + OLED_FB_SIZE;
    if (oled_frames_len + need > OLED_FRAMES_MAX) return;
    if (oled_frames_len + need > oled_frames_cap) {
        size_t cap = oled_frames_cap ? oled_frames_cap : 16384;
        while (cap < oled_frames_len + need) cap *= 2;
        uint8_t *f = realloc(oled_frames, cap);
        if (f == NULL) return;
        oled_frames = f;
        oled_frames_cap = cap;
    }
    uint8_t *p = oled_frames + oled_frames_len;
    p[0] = (uint8_t)ms;
    p[1] = (uint8_t)(ms >> 8);
    p[2] = (uint8_t)(ms >> 16);
    p[3] = (uint8_t)(ms >> 24);
    p[4] = inverted ? 1u : 0u;
    p[5] = contrast;
    memcpy(p + 6, fb, OLED_FB_SIZE);
    oled_frames_len += need;
}

EXPORT(jlw_oled_frames) const uint8_t *jlw_oled_frames(void) { return oled_frames; }
EXPORT(jlw_oled_frames_len) size_t jlw_oled_frames_len(void) { return oled_frames_len; }

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

static size_t printed; /* bytes of program output (not errors) so far */

static void write_out(void *ud, const char *d, size_t n) {
    (void)ud;
    printed += n;
    append(d, n);
}

/* ---- the step debugger ------------------------------------------------------
 *
 * In mode 6 the program runs as usual, and every time it reaches a new line
 * a snapshot is written as JSON: the line, the call stack with each call's
 * variables, the globals, the heap blocks and how much has been printed.
 * The playground steps through the snapshots forwards and backwards.
 */
#define TRACE_MAX (6u * 1024u * 1024u)
#define TRACE_STEPS 3000
static char *trace;
static size_t trace_len, trace_cap;
static int trace_steps, trace_last_line, trace_last_depth;
static bool trace_full;

static void tput(const char *d, size_t n) {
    if (trace_len + n > TRACE_MAX) {
        trace_full = true;
        return;
    }
    if (trace_len + n > trace_cap) {
        size_t cap = trace_cap ? trace_cap : 65536;
        while (cap < trace_len + n) cap *= 2;
        char *t = realloc(trace, cap);
        if (t == NULL) {
            trace_full = true;
            return;
        }
        trace = t;
        trace_cap = cap;
    }
    memcpy(trace + trace_len, d, n);
    trace_len += n;
}

static void tputs(const char *s) { tput(s, strlen(s)); }

/* A JSON string. */
static void tstr(const char *s, size_t n) {
    tput("\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char esc[8];
        if (ch == '"' || ch == '\\') {
            esc[0] = '\\';
            esc[1] = (char)ch;
            tput(esc, 2);
        } else if (ch < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", ch);
            tput(esc, 6);
        } else {
            tput((const char *)&ch, 1);
        }
    }
    tput("\"", 1);
}

/* A value as the debugger shows it, like print() but with strings quoted. */
static size_t show(JlVM *vm, Value v, char *out, size_t size, int depth) {
    size_t n = 0;
#define ADD(...) (n += (size_t)snprintf(out + n, n < size ? size - n : 0, __VA_ARGS__))
    switch ((ValueType)v.type) {
    case VAL_NIL: ADD("nil"); break;
    case VAL_BOOL: ADD("%s", v.as.b ? "true" : "false"); break;
    case VAL_INT: ADD("%d", (int)v.as.i); break;
    case VAL_FN: ADD("fn %s", vm->prog.fns[v.as.index].name); break;
    case VAL_NATIVE: ADD("built-in"); break;
    case VAL_STR: {
        const char *s;
        uint32_t len;
        if (jl_get_string(vm, v, &s, &len)) ADD("\"%.*s%s\"", (int)(len > 40 ? 40 : len), s, len > 40 ? "..." : "");
        else ADD("<freed str>");
        break;
    }
    case VAL_BUF: {
        uint8_t *d;
        uint32_t len;
        if (heap_get(&vm->heap, v.as.ref, &d, &len) == HEAP_OK) ADD("buf #%u, %u bytes", (unsigned)v.as.ref.handle, (unsigned)len);
        else ADD("<freed buf>");
        break;
    }
    case VAL_RING: {
        uint8_t *d;
        uint32_t len;
        if (heap_get(&vm->heap, v.as.ref, &d, &len) == HEAP_OK) {
            const RingHeader *rh = (const RingHeader *)(const void *)d;
            ADD("ring #%u (%u/%u)", (unsigned)v.as.ref.handle, (unsigned)rh->count, (unsigned)rh->cap);
        } else {
            ADD("<freed ring>");
        }
        break;
    }
    case VAL_ARR:
    case VAL_STRUCT: {
        Value *items;
        uint32_t len;
        if (heap_get(&vm->heap, v.as.ref, (uint8_t **)(void *)&items, &len) != HEAP_OK) {
            ADD("<freed %s>", v.type == VAL_ARR ? "array" : "struct");
            break;
        }
        len /= (uint32_t)sizeof(Value);
        const StructDef *d = v.type == VAL_STRUCT ? &vm->prog.structs[v.tag] : NULL;
        if (d) ADD("%.*s { ", (int)vm->prog.strs[d->name].len, vm->prog.strs[d->name].chars);
        else ADD("[");
        if (depth > 1) {
            ADD("...");
        } else {
            for (uint32_t i = 0; i < len && i < 8; i++) {
                if (i) ADD(", ");
                if (d) ADD("%.*s: ", (int)vm->prog.strs[d->fields[i]].len, vm->prog.strs[d->fields[i]].chars);
                if (n < size) n += show(vm, items[i], out + n, size - n, depth + 1);
            }
            if (len > 8) ADD(", ... %u more", (unsigned)(len - 8));
        }
        ADD(d ? " }" : "]");
        break;
    }
    }
#undef ADD
    return n < size ? n : size - 1;
}

static void tvar(JlVM *vm, const char *name, size_t nlen, Value v, bool first) {
    char text[256];
    size_t n = show(vm, v, text, sizeof text, 0);
    if (!first) tput(",", 1);
    tput("[", 1);
    tstr(name, nlen);
    tput(",", 1);
    tstr(text, n);
    tput("]", 1);
}

static bool on_step(JlVM *vm, const JlStep *st, void *ud) {
    (void)ud;
    if (trace_full) return true;
    if (st->line == trace_last_line && vm->frame_count == trace_last_depth) return true;
    if (trace_steps == TRACE_STEPS) {
        trace_full = true;
        return true;
    }
    trace_last_line = st->line;
    trace_last_depth = vm->frame_count;
    char buf[96];
    snprintf(buf, sizeof buf, "%s{\"line\":%d,\"out\":%u,\"heap\":%u,\"frames\":[", trace_steps ? "," : "",
             st->line, (unsigned)printed, (unsigned)vm->heap.used_bytes);
    tputs(buf);
    trace_steps++;
    /* the call stack, innermost call first */
    for (int f = vm->frame_count - 1; f >= 0; f--) {
        const CallFrame *fr = &vm->frames[f];
        const Chunk *ch = &vm->prog.fns[fr->fn].chunk;
        int off = (int)(fr->ip - ch->code);
        Value *top = f == vm->frame_count - 1 ? vm->sp : vm->frames[f + 1].slots;
        int line = ch->lines[off > 0 && f != vm->frame_count - 1 ? off - 1 : off];
        snprintf(buf, sizeof buf, "%s{\"line\":%d,\"name\":", f == vm->frame_count - 1 ? "" : ",", line);
        tputs(buf);
        const char *fname = vm->prog.fns[fr->fn].name;
        tstr(fname, strlen(fname));
        tputs(",\"vars\":[");
        bool first = true;
        for (int i = 0; i < ch->var_count; i++) {
            if (ch->vars[i].from > off || (ch->vars[i].to >= 0 && ch->vars[i].to <= off)) continue;
            if (fr->slots + ch->vars[i].slot >= top) continue;
            const StrConst *nm = &vm->prog.strs[ch->vars[i].name];
            tvar(vm, nm->chars, nm->len, fr->slots[ch->vars[i].slot], first);
            first = false;
        }
        tputs("]}");
    }
    tputs("],\"globals\":[");
    bool first = true;
    for (int g = 0; g < vm->global_count; g++) {
        const Global *gl = &vm->globals[g];
        if (!gl->defined || gl->value.type == VAL_NATIVE || gl->value.type == VAL_FN) continue;
        tvar(vm, gl->name, (size_t)gl->len, gl->value, first);
        first = false;
    }
    tputs("],\"blocks\":[");
    uint32_t boff = 0;
    BlockInfo bi;
    first = true;
    while (heap_next_block(&vm->heap, &boff, &bi)) {
        if (!bi.used) continue;
        snprintf(buf, sizeof buf, "%s[\"%s\",%u,%u,%u]", first ? "" : ",", heap_kind_name(bi.kind), (unsigned)bi.handle,
                 (unsigned)bi.len, (unsigned)vm->heap.handles[bi.handle].alloc_line);
        tputs(buf);
        first = false;
    }
    tputs("]}");
    return true;
}

EXPORT(jlw_trace) const char *jlw_trace(void) { return trace; }
EXPORT(jlw_trace_len) size_t jlw_trace_len(void) { return trace_len; }

static void write_err(void *ud, const char *d, size_t n) {
    (void)ud;
    append("\x02", 1);
    append(d, n);
    append("\x03", 1);
}

static char voice_buf[256];

EXPORT(jlw_set_voice) void jlw_set_voice(const char *s) {
    if (s == NULL) voice_buf[0] = '\0';
    else snprintf(voice_buf, sizeof voice_buf, "%s", s);
}

EXPORT(jlw_alloc) void *jlw_alloc(size_t n) { return malloc(n); }
EXPORT(jlw_free) void jlw_free(void *p) { free(p); }
EXPORT(jlw_output) const char *jlw_output(void) { return out_buf; }
EXPORT(jlw_output_len) size_t jlw_output_len(void) { return out_len; }

/*
 * mode:    0 = run, 1 = xray, 2 = dis, 3 = check,
 *          4 = RISC-V assembly, 5 = run on the RISC-V emulator,
 *          6 = run, recording each line for the step debugger (jlw_trace),
 *          7 = run on the emulator with the pipeline view
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
    printed = 0;
    trace_len = 0;
    truncated = false;
    frames_len = 0;
    oled_frames_len = 0;
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
    if (voice_buf[0] != '\0') opt.voice_text = voice_buf;
    opt.print_frames = false; /* the page draws the frames */
    opt.frame_sink = on_frame;
    opt.oled_sink = on_oled_frame;
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
    if (mode == 4 || mode == 5 || mode == 7) {
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
        RvPipe *pipe = NULL;
        if (mode == 7) {
            pipe = rv_pipe_new(&img, 24);
            run.trace = rv_pipe_step;
            run.trace_ud = pipe;
        }
        uint64_t cycles = 0;
        code = rv_run(&img, &run, &cycles);
        finish_vcd();
        if (pipe) {
            rv_pipe_report(pipe, &img, write_out, NULL);
            rv_pipe_free(pipe);
        }
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
    if (mode == 6) {
        trace_len = 0;
        trace_steps = 0;
        trace_last_line = -1;
        trace_last_depth = -1;
        trace_full = false;
        tputs("{\"steps\":[");
        jl_set_step_hook(vm, on_step, NULL);
    }
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
    if (mode == 6) {
        char end[48];
        snprintf(end, sizeof end, "],\"complete\":%s}", trace_full ? "false" : "true");
        trace_full = false;
        tputs(end);
    }
    jl_free(vm);
    finish_vcd();
    return code;
}
