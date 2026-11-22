/*
 * glass.c - see glass.h.
 */
#include "glass.h"

#include <string.h>

enum { NOT_STARTED, RUNNING, FINISHED, FAILED };

static uint64_t ticks;

uint64_t glass_ticks(void) { return ticks; }

static struct {
    JlVM *display;
    const char *source;
    const JlStep *step; /* valid while a tick runs */
    int state;
    int32_t skip;
    uint64_t cycles; /* instructions executed, kept after the program ends */
    JlWriteFn echo;
    char out[64], err[192]; /* last line printed / first error line */
    char out_line[64];     /* the line being printed */
    size_t out_len;
    bool err_done;
} g;

/* ---- capturing the program's output ----------------------------------- */

static void on_out(void *ud, const char *data, size_t len) {
    (void)ud;
    if (g.echo) g.echo(NULL, data, len);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            memcpy(g.out, g.out_line, g.out_len);
            g.out[g.out_len] = 0;
            g.out_len = 0;
        } else if (g.out_len < sizeof g.out_line - 1) {
            g.out_line[g.out_len++] = data[i];
        }
    }
}

static void on_err(void *ud, const char *data, size_t len) {
    (void)ud;
    if (g.echo) g.echo(NULL, data, len);
    size_t have = strlen(g.err);
    for (size_t i = 0; i < len && !g.err_done; i++) {
        if (data[i] == '\n') g.err_done = true;
        else if (have < sizeof g.err - 1) g.err[have++] = data[i];
    }
    g.err[have] = 0;
}

/* ---- the step hook ------------------------------------------------------ */

static bool on_step(JlVM *vm, const JlStep *step, void *ud) {
    (void)vm;
    (void)ud;
    g.cycles = step->cycles;
    if (g.skip > 0) {
        g.skip--;
        return true;
    }
    g.step = step;
    ticks++;
    JlResult r = jl_emit(g.display, "tick");
    g.step = NULL;
    return r == JL_OK || r == JL_NO_HANDLER;
}

JlResult glass_run(JlVM *display, const char *source, uint32_t heap_size, JlWriteFn echo) {
    memset(&g, 0, sizeof g);
    g.display = display;
    g.source = source;
    g.echo = echo;

    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = heap_size;
    cfg.max_cycles = 0;
    cfg.out = on_out;
    cfg.err = on_err;
    JlVM *prog = jl_new(&cfg);
    if (prog == NULL) return JL_RUNTIME_ERROR;
    jl_set_step_hook(prog, on_step, NULL);

    jl_emit(display, "start");
    g.state = RUNNING;
    JlResult r = jl_run_source(prog, source);
    g.cycles = jl_cycles(prog);
    g.state = r == JL_OK ? FINISHED : FAILED;
    jl_emit(display, "done");
    jl_free(prog);
    return r;
}

/* ---- built-ins for the display ------------------------------------------ */

static const Value *stack_value(int32_t i) {
    if (g.step == NULL || i < 0 || i >= g.step->depth) return NULL;
    return &g.step->stack[g.step->depth - 1 - i];
}

static bool b_line(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(g.step ? g.step->line : 0);
    return true;
}

static bool b_depth(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(g.step ? g.step->depth : 0);
    return true;
}

static bool b_frames(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(g.step ? g.step->frames : 0);
    return true;
}

static bool b_cycles(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val((int32_t)g.cycles);
    return true;
}

static bool b_heap(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(g.step ? (int32_t)g.step->heap_used : 0);
    return true;
}

static bool b_state(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(g.state);
    return true;
}

static bool b_value(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t i;
    if (!jl_get_int(vm, "prog_value", 1, args[0], &i)) return false;
    const Value *v = stack_value(i);
    int32_t n = 0;
    if (v != NULL) {
        switch (v->type) {
        case VAL_INT: n = v->as.i; break;
        case VAL_BOOL: n = v->as.b; break;
        case VAL_STR:
        case VAL_ARR:
        case VAL_BUF: n = v->on_heap || v->type == VAL_BUF ? v->as.ref.handle : v->as.index; break;
        case VAL_FN:
        case VAL_NATIVE: n = v->as.index; break;
        default: break;
        }
    }
    *out = int_val(n);
    return true;
}

static bool b_kind(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t i;
    if (!jl_get_int(vm, "prog_kind", 1, args[0], &i)) return false;
    const Value *v = stack_value(i);
    static const int kinds[] = {[VAL_NIL] = 2, [VAL_BOOL] = 1, [VAL_INT] = 0, [VAL_STR] = 3,
                                [VAL_BUF] = 4, [VAL_FN] = 5, [VAL_NATIVE] = 5, [VAL_ARR] = 6};
    *out = int_val(v ? kinds[v->type] : 2);
    return true;
}

/* The text of source line n, without its indentation or a trailing comment. */
static size_t source_line(int n, char *dst, size_t cap) {
    const char *p = g.source;
    for (int line = 1; line < n && *p; p++)
        if (*p == '\n') line++;
    while (*p == ' ' || *p == '\t') p++;
    size_t len = 0;
    while (p[len] && p[len] != '\n' && !(p[len] == '/' && p[len + 1] == '/')) len++;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) len--;
    if (len > cap) len = cap;
    memcpy(dst, p, len);
    return len;
}

static bool b_text(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *what;
    uint32_t wn;
    uint8_t *buf;
    uint32_t cap;
    if (!jl_get_string(vm, args[0], &what, &wn))
        return jl_error(vm, "prog_text(): the first argument must be \"op\", \"src\", \"out\" or \"err\"");
    if (args[1].type != VAL_BUF) return jl_error(vm, "prog_text(): the second argument must be a buf");
    if (!jl_get_bytes(vm, "prog_text", args[1], &buf, &cap)) return false;

    const char *text = "";
    size_t len = 0;
    if (wn == 2 && memcmp(what, "op", 2) == 0) {
        text = g.step ? g.step->op : "";
        len = strlen(text);
    } else if (wn == 3 && memcmp(what, "src", 3) == 0) {
        len = g.step ? source_line(g.step->line, (char *)buf, cap) : 0;
        *out = int_val((int32_t)len);
        return true;
    } else if (wn == 3 && memcmp(what, "out", 3) == 0) {
        text = g.out;
        len = strlen(text);
    } else if (wn == 3 && memcmp(what, "err", 3) == 0) {
        /* "runtime error (line 3): msg" becomes "line 3: msg" */
        text = g.err;
        const char *open = strchr(text, '(');
        const char *close = open ? strstr(open, "): ") : NULL;
        size_t n = 0;
        char tmp[sizeof g.err];
        if (open && close && strncmp(open + 1, "line ", 5) == 0) {
            memcpy(tmp, open + 1, (size_t)(close - open - 1));
            n = (size_t)(close - open - 1);
            tmp[n++] = ':';
            text = close + 2;
        }
        size_t rest = strlen(text);
        if (n + rest > sizeof tmp - 1) rest = sizeof tmp - 1 - n;
        memcpy(tmp + n, text, rest);
        len = n + rest;
        if (len > cap) len = cap;
        memcpy(buf, tmp, len);
        *out = int_val((int32_t)len);
        return true;
    } else {
        return jl_error(vm, "prog_text(): the first argument must be \"op\", \"src\", \"out\" or \"err\"");
    }
    if (len > cap) len = cap;
    memcpy(buf, text, len);
    *out = int_val((int32_t)len);
    return true;
}

static bool b_skip(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t n;
    if (!jl_get_int(vm, "skip", 1, args[0], &n)) return false;
    g.skip = n > 0 ? n : 0;
    return true;
}

void glass_register(JlVM *display) {
    jl_register_native(display, "prog_line", b_line, 0);
    jl_register_native(display, "prog_depth", b_depth, 0);
    jl_register_native(display, "prog_value", b_value, 1);
    jl_register_native(display, "prog_kind", b_kind, 1);
    jl_register_native(display, "prog_frames", b_frames, 0);
    jl_register_native(display, "prog_cycles", b_cycles, 0);
    jl_register_native(display, "prog_heap", b_heap, 0);
    jl_register_native(display, "prog_state", b_state, 0);
    jl_register_native(display, "prog_text", b_text, 2);
    jl_register_native(display, "skip", b_skip, 1);
}
