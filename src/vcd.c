/*
 * vcd.c - Value Change Dump writer. See vcd.h.
 */
#include "vcd.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define VCD_MAX_SIGNALS 256

typedef struct {
    char name[40];
    int width;
    uint32_t last;
} Signal;

typedef struct {
    uint32_t ms;
    uint16_t sig;
    uint32_t value;
} Change;

struct Vcd {
    Signal sig[VCD_MAX_SIGNALS];
    int nsig;
    Change *changes;
    size_t len, cap;
};

Vcd *vcd_new(void) { return calloc(1, sizeof(Vcd)); }

void vcd_free(Vcd *v) {
    if (!v) return;
    free(v->changes);
    free(v);
}

static int find_signal(Vcd *v, const char *name, int width) {
    for (int i = 0; i < v->nsig; i++)
        if (strcmp(v->sig[i].name, name) == 0) return i;
    if (v->nsig == VCD_MAX_SIGNALS) return -1;
    Signal *s = &v->sig[v->nsig];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->width = width < 1 ? 1 : width > 32 ? 32 : width;
    s->last = 0;
    return v->nsig++;
}

void vcd_change(Vcd *v, uint32_t ms, const char *name, int width, uint32_t value) {
    if (!v) return;
    int i = find_signal(v, name, width);
    if (i < 0) return;
    if (v->sig[i].width < 32) value &= (1u << v->sig[i].width) - 1u;
    if (v->sig[i].last == value) return;
    v->sig[i].last = value;
    /* Several changes at the same moment: only the last one is visible. */
    for (size_t c = v->len; c > 0 && v->changes[c - 1].ms == ms; c--) {
        if (v->changes[c - 1].sig == i) {
            v->changes[c - 1].value = value;
            return;
        }
    }
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        Change *grown = realloc(v->changes, cap * sizeof *grown);
        if (!grown) return;
        v->changes = grown;
        v->cap = cap;
    }
    v->changes[v->len++] = (Change){ms, (uint16_t)i, value};
}

/* Where the dump goes: a file, or a buffer in the browser build. */
typedef struct {
    VcdWriteFn fn;
    void *ud;
} Out;

static void out_printf(Out *o, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) o->fn(o->ud, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

/* Short identifiers made of the printable characters ! to ~. */
static void ident(int i, char out[4]) {
    int n = 0;
    do {
        out[n++] = (char)('!' + i % 94);
        i /= 94;
    } while (i > 0 && n < 3);
    out[n] = '\0';
}

static void value(Out *f, const Signal *s, int i, uint32_t v) {
    char id[4];
    ident(i, id);
    if (s->width == 1) {
        out_printf(f, "%u%s\n", (unsigned)(v & 1u), id);
        return;
    }
    char bits[33];
    int n = 0;
    for (int b = s->width - 1; b >= 0; b--)
        if (n || (v >> b) & 1u || b == 0) bits[n++] = (v >> b) & 1u ? '1' : '0';
    bits[n] = '\0';
    out_printf(f, "b%s %s\n", bits, id);
}

static void emit(const Vcd *v, Out *f) {
    out_printf(f, "$comment Written by jlang run --vcd. Open it with GTKWave. $end\n");
    out_printf(f, "$timescale 1ms $end\n$scope module board $end\n");
    for (int i = 0; i < v->nsig; i++) {
        char id[4];
        ident(i, id);
        out_printf(f, "$var %s %d %s %s $end\n", v->sig[i].width == 1 ? "wire" : "reg", v->sig[i].width, id,
                v->sig[i].name);
    }
    out_printf(f, "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n");
    /* Everything starts at 0 unless it changed at time 0. */
    for (int i = 0; i < v->nsig; i++) {
        uint32_t start = 0;
        for (size_t c = 0; c < v->len && v->changes[c].ms == 0; c++)
            if (v->changes[c].sig == i) start = v->changes[c].value;
        value(f, &v->sig[i], i, start);
    }
    out_printf(f, "$end\n");
    uint32_t now = 0;
    uint32_t end = 0;
    for (size_t c = 0; c < v->len; c++) {
        const Change *ch = &v->changes[c];
        if (ch->ms == 0) continue;
        if (ch->ms != now) {
            now = ch->ms;
            out_printf(f, "#%u\n", (unsigned)now);
        }
        value(f, &v->sig[ch->sig], ch->sig, ch->value);
        end = now;
    }
    /* One more timestamp so the last change has a visible width. */
    out_printf(f, "#%u\n", (unsigned)(end + 1));
}

static void to_file(void *ud, const char *data, size_t len) { fwrite(data, 1, len, (FILE *)ud); }

bool vcd_write(const Vcd *v, FILE *f) {
    Out o = {to_file, f};
    emit(v, &o);
    return !ferror(f);
}

void vcd_emit(const Vcd *v, VcdWriteFn out, void *ud) {
    Out o = {out, ud};
    emit(v, &o);
}
