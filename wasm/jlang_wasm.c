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

#define EXPORT(name) __attribute__((export_name(#name)))
#define OUTPUT_MAX (512u * 1024u)

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
 * mode:    0 = run, 1 = xray, 2 = dis, 3 = check
 * emits:   event names separated by '\n' (may be empty)
 * pins:    "4=1,5=0" (may be empty)
 * answer:  reply of the simulated ask() (empty = default)
 * Returns the same exit codes as the jlang command.
 */
EXPORT(jlw_run)
int jlw_run(const char *src, int mode, const char *emits, const char *pins,
            const char *answer, uint32_t heap, int stats) {
    out_len = 0;
    truncated = false;

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
    for (const char *p = pins; p != NULL && *p;) {
        char *end;
        long pin = strtol(p, &end, 10);
        if (*end == '=' && pin >= 0 && pin < SIM_PINS) {
            opt.pin_inputs[pin] = end[1] == '1' ? 1 : 0;
        }
        p = strchr(p, ',');
        if (p) p++;
    }
    board_sim_register(vm, &opt);

    int code = 0;
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

    if (code != 65) {
        if (stats) jl_print_stats(vm);
        if (code == 0) jl_report_leaks(vm);
    }
    jl_free(vm);
    return code;
}
