/*
 * rv_codegen.c - compiles JLang to RISC-V (RV32IM) assembly.
 *
 * The front end is shared with the VM: the source is parsed and checked
 * by compiler.c, which produces bytecode. This file then translates each
 * function's bytecode into RISC-V instructions.
 *
 * Runtime model
 *   s1  value stack pointer (next free slot, in memory)
 *   s0  frame pointer: address of slot 0 of the current call
 *   sp  machine stack, used for return addresses and saved registers
 *
 * Every JLang value is one 32-bit word. Types are tracked at compile
 * time ("kinds"), which is how print() knows whether a word is a number,
 * a boolean or the address of a string.
 *
 * Hardware access goes through a few C-callable functions (the HAL):
 *   jl_hal_putc(c)  jl_hal_halt(code)  jl_hal_pin_mode(pin, mode)
 *   jl_hal_pin_write(pin, v)  jl_hal_pin_read(pin)  jl_hal_delay(ms)
 *   jl_hal_millis()
 * For the emulator they are generated here; on a real board they are a
 * small C file (see boards/).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "rv.h"
#include "rv_codegen.h"

enum { K_INT, K_BOOL, K_NIL, K_STR, K_FN, K_NATIVE };

typedef struct {
    uint8_t kind;
    uint16_t index; /* string, function or built-in index */
} Kind;

#define MAX_DEPTH 600
#define DYNAMIC 0xFFFF /* kind index when the exact string or function isn't known */

typedef struct {
    JlVM *vm;
    const RvGenOptions *opt;
    char *buf;
    size_t len;
    size_t cap;
    bool failed;
    int label;
    /* per global */
    Kind gkind[JL_GLOBALS_MAX];
    bool gkind_set[JL_GLOBALS_MAX];
    bool reassigned[JL_GLOBALS_MAX];
    int gbind[JL_GLOBALS_MAX]; /* function a global is bound to, or -1 */
    /* string constants used */
    bool *str_used;
    bool *returns_nil; /* per function: every return gives nil */
    bool local_set[256]; /* current function: slots assigned after their let */
    /* extra string literals made by the code generator */
    char **lits;
    uint32_t *lit_lens;
    int nlits;
    /* event handlers: name -> function */
    struct {
        char name[32];
        int fn;
    } handlers[JL_EVENTS_MAX];
    int nhandlers;
    int first_fn;
    /* current function */
    int fn;
    int line;
    Kind kinds[MAX_DEPTH];
    int depth;
} Gen;

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static void out(Gen *g, const char *fmt, ...) JL_PRINTF(2, 3);
static void out(Gen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if (g->len + (size_t)n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 16384;
        while (cap < g->len + (size_t)n + 1) cap *= 2;
        g->buf = jl_xrealloc(g->buf, cap);
        g->cap = cap;
    }
    vsnprintf(g->buf + g->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    g->len += (size_t)n;
}

static void error(Gen *g, const char *fmt, ...) JL_PRINTF(2, 3);
static void error(Gen *g, const char *fmt, ...) {
    if (g->failed) return;
    g->failed = true;
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    vm_errf(g->vm, "error (line %d): %s\n", g->line, msg);
}

static int new_label(Gen *g) { return g->label++; }

/* A length-prefixed string the generator needs (messages, printf text). */
static int literal(Gen *g, const char *s, uint32_t n) {
    for (int i = 0; i < g->nlits; i++) {
        if (g->lit_lens[i] == n && memcmp(g->lits[i], s, n) == 0) return i;
    }
    g->lits = jl_xrealloc(g->lits, (size_t)(g->nlits + 1) * sizeof(char *));
    g->lit_lens = jl_xrealloc(g->lit_lens, (size_t)(g->nlits + 1) * sizeof(uint32_t));
    g->lits[g->nlits] = jl_xrealloc(NULL, n + 1);
    memcpy(g->lits[g->nlits], s, n);
    g->lits[g->nlits][n] = '\0';
    g->lit_lens[g->nlits] = n;
    return g->nlits++;
}

static int msg(Gen *g, const char *s) { return literal(g, s, (uint32_t)strlen(s)); }

/* if the branch is NOT taken, stop with: prefix <number in reg> suffix */
static void trap_num_unless(Gen *g, const char *branch_to_ok, const char *prefix, const char *reg, const char *suffix) {
    int ok = new_label(g);
    out(g, "    %s, .Lok%d\n", branch_to_ok, ok);
    out(g, "    mv a2, %s\n    li a0, %d\n    la a1, jl_lit_%d\n    la a3, jl_lit_%d\n    j jl_rt_trap_num\n",
        reg, g->line, msg(g, prefix), msg(g, suffix));
    out(g, ".Lok%d:\n", ok);
}

/* if the condition branch is NOT taken, stop with `message` */
static void trap_unless(Gen *g, const char *branch_to_ok, const char *message) {
    int ok = new_label(g);
    out(g, "    %s, .Lok%d\n", branch_to_ok, ok);
    out(g, "    li a0, %d\n    la a1, jl_lit_%d\n    j jl_rt_trap\n", g->line, msg(g, message));
    out(g, ".Lok%d:\n", ok);
}

/* ------------------------------------------------------------------ */
/* value stack helpers                                                 */
/* ------------------------------------------------------------------ */

static void push_kind(Gen *g, Kind k) {
    if (g->depth >= MAX_DEPTH) {
        error(g, "expression is too deep for native code");
        return;
    }
    g->kinds[g->depth++] = k;
}

static Kind kind_of(uint8_t kind) {
    Kind k = {kind, 0};
    return k;
}

static Kind top(Gen *g, int n) {
    if (g->depth - 1 - n < 0) return kind_of(K_INT);
    return g->kinds[g->depth - 1 - n];
}

static void pop_kinds(Gen *g, int n) {
    g->depth -= n;
    if (g->depth < 0) g->depth = 0;
}

static void push_reg(Gen *g, const char *reg) {
    out(g, "    sw %s, 0(s1)\n    addi s1, s1, 4\n", reg);
}

/* Can a variable hold both? Strings and functions must stay strings and
 * functions, because print() and calls depend on knowing which they are. */
static bool same_storage(Kind a, Kind b) {
    bool sa = a.kind == K_STR || a.kind == K_FN || a.kind == K_NATIVE;
    bool sb = b.kind == K_STR || b.kind == K_FN || b.kind == K_NATIVE;
    if (!sa && !sb) return true;
    return a.kind == b.kind && a.kind != K_NATIVE;
}

static Kind merge(Kind a, Kind b) {
    if (a.kind == b.kind && a.index == b.index) return a;
    if (a.kind == b.kind && (a.kind == K_STR || a.kind == K_FN)) return (Kind){a.kind, DYNAMIC};
    return kind_of(K_INT);
}

/* The kind a variable read gives: exact only if it is never reassigned. */
static Kind read_kind(Kind k, bool reassigned) {
    if (reassigned && (k.kind == K_STR || k.kind == K_FN)) k.index = DYNAMIC;
    if (reassigned && (k.kind == K_BOOL || k.kind == K_NIL)) k.kind = K_INT;
    return k;
}

/* ------------------------------------------------------------------ */
/* built-in functions                                                  */
/* ------------------------------------------------------------------ */

/* Offset (from s1) of argument i of a call with argc arguments. */
static int arg_off(int argc, int i) { return -4 * (argc - i); }

static void print_value(Gen *g, Kind k, int off) {
    switch (k.kind) {
    case K_STR:
        out(g, "    lw a0, %d(s1)\n    call jl_rt_puts\n", off);
        break;
    case K_BOOL:
        out(g, "    lw a0, %d(s1)\n    call jl_rt_putbool\n", off);
        break;
    case K_NIL:
        out(g, "    la a0, jl_lit_%d\n    call jl_rt_puts\n", msg(g, "nil"));
        break;
    case K_FN: {
        char s[80];
        if (k.index == DYNAMIC) snprintf(s, sizeof s, "<fn>");
        else snprintf(s, sizeof s, "<fn %s>", g->vm->prog.fns[k.index].name);
        out(g, "    la a0, jl_lit_%d\n    call jl_rt_puts\n", msg(g, s));
        break;
    }
    case K_NATIVE: {
        char s[80];
        snprintf(s, sizeof s, "<built-in %s>", g->vm->natives[k.index].name);
        out(g, "    la a0, jl_lit_%d\n    call jl_rt_puts\n", msg(g, s));
        break;
    }
    default:
        out(g, "    lw a0, %d(s1)\n    call jl_rt_putint\n", off);
    }
}

static bool literal_arg(Gen *g, int argc, int i, const char *fn, const char **s, uint32_t *n) {
    Kind k = top(g, argc - 1 - i);
    if (k.kind != K_STR || k.index == DYNAMIC) {
        error(g, "in native code, argument %d of %s() must be written directly as a string literal", i + 1, fn);
        return false;
    }
    *s = g->vm->prog.strs[k.index].chars;
    *n = g->vm->prog.strs[k.index].len;
    return true;
}

static void gen_printf(Gen *g, int argc) {
    const char *f;
    uint32_t n;
    if (argc < 1 || !literal_arg(g, argc, 0, "printf", &f, &n)) return;
    int next = 1;
    uint32_t i = 0;
    char piece[512];
    size_t plen = 0;
#define FLUSH()                                                                             \
    do {                                                                                    \
        if (plen) {                                                                         \
            out(g, "    la a0, jl_lit_%d\n    call jl_rt_puts\n", literal(g, piece, (uint32_t)plen)); \
            plen = 0;                                                                       \
        }                                                                                   \
    } while (0)
    while (i < n && !g->failed) {
        char c = f[i];
        if ((c == '{' || c == '}') && i + 1 < n && f[i + 1] == c) {
            if (plen < sizeof piece) piece[plen++] = c;
            i += 2;
            continue;
        }
        if (c != '{') {
            if (plen == sizeof piece) FLUSH();
            piece[plen++] = c;
            i++;
            continue;
        }
        FLUSH();
        uint32_t j = i + 1;
        while (j < n && f[j] != '}') j++;
        if (j >= n) {
            error(g, "printf(): a '{' has no matching '}'");
            return;
        }
        if (next >= argc) {
            error(g, "printf(): the format has more {} than values passed");
            return;
        }
        char kind = j > i + 1 ? f[i + 1] : 0;
        int width = 0;
        for (uint32_t k = i + 2; k < j; k++) {
            if (f[k] < '0' || f[k] > '9') {
                error(g, "printf(): bad format {%.*s}", (int)(j - i - 1), f + i + 1);
                return;
            }
            width = width * 10 + (f[k] - '0');
        }
        if (width > 32) {
            error(g, "printf(): width can be at most 32");
            return;
        }
        Kind ak = top(g, argc - 1 - next);
        int off = arg_off(argc, next);
        if (kind == 0) {
            print_value(g, ak, off);
        } else if (kind == 'd' || kind == 'x' || kind == 'b' || kind == 'c') {
            if (ak.kind == K_STR) {
                error(g, "printf(): {%c} needs an int, got a string", kind);
                return;
            }
            out(g, "    lw a0, %d(s1)\n", off);
            if (kind == 'c') {
                trap_unless(g, "sltiu t0, a0, 256\n    bnez t0", "printf(): {c} needs a byte (0..255)");
                out(g, "    lw a0, %d(s1)\n    call jl_hal_putc\n", off);
            } else {
                out(g, "    li a1, %d\n    call %s\n", width,
                    kind == 'd' ? "jl_rt_putint_w" : kind == 'x' ? "jl_rt_puthex" : "jl_rt_putbin");
            }
        } else {
            error(g, "printf(): unknown format {%.*s} (use {}, {d}, {x}, {b} or {c})", (int)(j - i - 1), f + i + 1);
            return;
        }
        next++;
        i = j + 1;
    }
    FLUSH();
#undef FLUSH
    if (!g->failed && next < argc) {
        error(g, "printf(): %d value(s) were passed but the format doesn't use them", argc - next);
    }
}

static void check_pin(Gen *g) {
    trap_unless(g, "sltiu t0, a0, 32\n    bnez t0", "pin number is out of range (native code supports pins 0..31)");
}

/* Calls a built-in. Arguments are on the value stack above the callee's
 * placeholder slot; the result replaces the placeholder. */
static void gen_native_call(Gen *g, int native, int argc) {
    const char *name = g->vm->natives[native].name;
    int arity = g->vm->natives[native].arity;
    if (arity >= 0 && argc != arity) {
        error(g, "%s() takes %d argument%s but got %d", name, arity, arity == 1 ? "" : "s", argc);
        return;
    }
    Kind result = kind_of(K_INT);
    const char *s;
    uint32_t n;
    bool result_in_a0 = false;
    bool nil_result = false;

    if (strcmp(name, "print") == 0) {
        for (int i = 0; i < argc; i++) {
            if (i > 0) out(g, "    li a0, 32\n    call jl_hal_putc\n");
            print_value(g, top(g, argc - 1 - i), arg_off(argc, i));
        }
        out(g, "    li a0, 10\n    call jl_hal_putc\n");
        nil_result = true;
    } else if (strcmp(name, "printf") == 0) {
        gen_printf(g, argc);
        nil_result = true;
    } else if (strcmp(name, "pin_mode") == 0) {
        if (!literal_arg(g, argc, 1, name, &s, &n)) return;
        int mode = n == 3 && memcmp(s, "out", 3) == 0 ? 1
                 : n == 2 && memcmp(s, "in", 2) == 0 ? 0
                 : n == 9 && memcmp(s, "in_pullup", 9) == 0 ? 2 : -1;
        if (mode < 0) {
            error(g, "pin_mode(): mode must be \"out\", \"in\" or \"in_pullup\"");
            return;
        }
        out(g, "    lw a0, %d(s1)\n", arg_off(argc, 0));
        check_pin(g);
        out(g, "    li a1, %d\n    call jl_hal_pin_mode\n", mode);
        nil_result = true;
    } else if (strcmp(name, "pin_write") == 0) {
        out(g, "    lw a0, %d(s1)\n", arg_off(argc, 0));
        check_pin(g);
        out(g, "    lw a1, %d(s1)\n", arg_off(argc, 1));
        trap_unless(g, "sltiu t0, a1, 2\n    bnez t0", "pin_write(): a pin can only be 0 (LOW) or 1 (HIGH)");
        out(g, "    call jl_hal_pin_write\n");
        nil_result = true;
    } else if (strcmp(name, "pin_read") == 0) {
        out(g, "    lw a0, %d(s1)\n", arg_off(argc, 0));
        check_pin(g);
        out(g, "    call jl_hal_pin_read\n");
        result_in_a0 = true;
    } else if (strcmp(name, "delay") == 0) {
        out(g, "    lw a0, %d(s1)\n", arg_off(argc, 0));
        trap_unless(g, "bgez a0", "delay(): can't wait a negative time");
        out(g, "    call jl_hal_delay\n");
        nil_result = true;
    } else if (strcmp(name, "millis") == 0) {
        out(g, "    call jl_hal_millis\n");
        result_in_a0 = true;
    } else if (strcmp(name, "u8") == 0 || strcmp(name, "i8") == 0 || strcmp(name, "u16") == 0 ||
               strcmp(name, "i16") == 0) {
        int bits = name[1] == '8' ? 24 : 16;
        out(g, "    lw a0, %d(s1)\n    slli a0, a0, %d\n    %s a0, a0, %d\n", arg_off(argc, 0), bits,
            name[0] == 'u' ? "srli" : "srai", bits);
        result_in_a0 = true;
    } else if (strcmp(name, "assert") == 0) {
        if (argc < 1 || argc > 2) {
            error(g, "assert() takes 1 or 2 arguments");
            return;
        }
        char text[300] = "assertion failed";
        if (argc == 2) {
            if (!literal_arg(g, argc, 1, name, &s, &n)) return;
            snprintf(text, sizeof text, "assertion failed: %.*s", (int)n, s);
        }
        out(g, "    lw t1, %d(s1)\n", arg_off(argc, 0));
        trap_unless(g, "bnez t1", text);
        nil_result = true;
    } else if (strcmp(name, "cycles") == 0) {
        out(g, "    rdinstret a0\n");
        result_in_a0 = true;
    } else if (strcmp(name, "mmio_read32") == 0 || strcmp(name, "mmio_read8") == 0) {
        out(g, "    lw t0, %d(s1)\n    %s a0, 0(t0)\n", arg_off(argc, 0), name[9] == '3' ? "lw" : "lbu");
        result_in_a0 = true;
    } else if (strcmp(name, "mmio_write32") == 0 || strcmp(name, "mmio_write8") == 0) {
        out(g, "    lw t0, %d(s1)\n    lw t1, %d(s1)\n    %s t1, 0(t0)\n", arg_off(argc, 0), arg_off(argc, 1),
            name[10] == '3' ? "sw" : "sb");
        nil_result = true;
    } else if (strcmp(name, "asm") == 0) {
        if (!literal_arg(g, argc, 0, name, &s, &n)) return;
        out(g, "    # @line %d\n    # inline assembly\n", g->line);
        /* one instruction per line; ';' or a newline separates them */
        const char *p = s, *end = s + n;
        while (p < end) {
            const char *q = p;
            while (q < end && *q != '\n' && *q != ';') q++;
            out(g, "    %.*s\n", (int)(q - p), p);
            p = q + 1;
        }
        nil_result = true;
    } else if (strcmp(name, "emit") == 0) {
        if (!literal_arg(g, argc, 0, name, &s, &n)) return;
        int fn = -1;
        for (int i = 0; i < g->nhandlers; i++) {
            if (strlen(g->handlers[i].name) == n && memcmp(g->handlers[i].name, s, n) == 0) fn = g->handlers[i].fn;
        }
        if (fn < 0) {
            error(g, "emit(): nothing handles '%.*s' (add: on %.*s { ... })", (int)n, s, (int)n, s);
            return;
        }
        /* drop the argument, reuse the placeholder slot as the handler's slot 0 */
        out(g, "    addi s1, s1, -4\n    call jl_fn_%d\n", fn);
        pop_kinds(g, argc + 1);
        push_kind(g, kind_of(K_NIL));
        return;
    } else {
        error(g, "%s() isn't available in native code yet (native programs have no heap or "
                 "camera); run it with `jlang run` instead", name);
        return;
    }

    /* the result replaces the placeholder slot */
    out(g, "    addi s1, s1, %d\n", -4 * argc);
    if (result_in_a0) out(g, "    sw a0, -4(s1)\n");
    else if (nil_result) out(g, "    sw zero, -4(s1)\n");
    pop_kinds(g, argc + 1);
    push_kind(g, nil_result ? kind_of(K_NIL) : result);
}

/* ------------------------------------------------------------------ */
/* one function                                                        */
/* ------------------------------------------------------------------ */

static uint16_t u16_at(const uint8_t *code, int off) {
    return (uint16_t)((code[off] << 8) | code[off + 1]);
}

static int op_size(uint8_t op) {
    switch (op) {
    case OP_CONST: case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_LOOP: return 3;
    case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL:
    case OP_DEF_GLOBAL: case OP_CALL: return 2;
    default: return 1;
    }
}

static int jump_target(const Chunk *ch, int off) {
    uint8_t op = ch->code[off];
    int d = u16_at(ch->code, off + 1);
    return op == OP_LOOP ? off + 3 - d : off + 3 + d;
}

static void binary(Gen *g, const char *insn) {
    out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    %s t0, t0, t1\n    sw t0, -8(s1)\n    addi s1, s1, -4\n", insn);
}

static void gen_function(Gen *g, int fi) {
    Function *fn = &g->vm->prog.fns[fi];
    Chunk *ch = &fn->chunk;
    g->fn = fi;
    g->depth = 0;
    push_kind(g, fi == g->first_fn ? kind_of(K_INT) : (Kind){K_FN, (uint16_t)fi}); /* slot 0 */
    for (int i = 0; i < fn->arity; i++) push_kind(g, kind_of(K_INT));
    g->line = ch->count > 0 ? ch->lines[0] : 0;

    memset(g->local_set, 0, sizeof g->local_set);
    for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
        if (ch->code[off] == OP_SET_LOCAL) g->local_set[ch->code[off + 1]] = true;
    }

    /* jump targets and the stack depth/kind expected there */
    bool *target = calloc((size_t)ch->count + 1, 1);
    int *tdepth = malloc(((size_t)ch->count + 1) * sizeof(int));
    Kind *tkind = calloc((size_t)ch->count + 1, sizeof(Kind));
    for (int i = 0; i <= ch->count; i++) tdepth[i] = -1;
    for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
        uint8_t op = ch->code[off];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_LOOP) target[jump_target(ch, off)] = true;
    }

    out(g, "\n    .align 2\n    .word %d\njl_fn_%d:   # %s\n", fn->arity, fi, fn->name);
    out(g, "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s0, 8(sp)\n    addi s0, s1, %d\n", -4 * (fn->arity + 1));
    out(g, "    la t0, jl_depth\n    lw t1, 0(t0)\n    addi t1, t1, 1\n    sw t1, 0(t0)\n    li t2, %d\n", JL_FRAMES_MAX + 1);
    out(g, "    # @line %d\n", g->line);
    {
        char text[120];
        snprintf(text, sizeof text, "stack overflow: more than %d nested calls. Is there a recursion that never stops?", JL_FRAMES_MAX);
        trap_unless(g, "blt t1, t2", text);
    }
    out(g, "    la t0, jl_vstack_end\n    addi t0, t0, -1024\n");
    trap_unless(g, "bltu s1, t0", "value stack overflow (an expression or recursion is too deep)");

    bool reachable = true;
    for (int off = 0; off < ch->count && !g->failed;) {
        if (target[off]) {
            if (!reachable && tdepth[off] >= 0) {
                g->depth = tdepth[off];
                if (g->depth > 0) g->kinds[g->depth - 1] = tkind[off];
            } else if (tdepth[off] >= 0 && g->depth > 0) {
                g->kinds[g->depth - 1] = merge(g->kinds[g->depth - 1], tkind[off]);
            }
            if (tdepth[off] < 0) {
                tdepth[off] = g->depth;
                if (g->depth > 0) tkind[off] = g->kinds[g->depth - 1];
            }
            out(g, ".L%d_%d:\n", fi, off);
        }
        reachable = true;
        if (ch->lines[off] != g->line) {
            g->line = ch->lines[off];
            out(g, "    # @line %d\n", g->line);
        }

        uint8_t op = ch->code[off];
        switch (op) {
        case OP_CONST: {
            Value v = ch->consts[u16_at(ch->code, off + 1)];
            if (v.type == VAL_INT) {
                out(g, "    li t0, %d\n", (int)v.as.i);
                push_reg(g, "t0");
                push_kind(g, kind_of(K_INT));
            } else if (v.type == VAL_STR) {
                g->str_used[v.as.index] = true;
                out(g, "    la t0, jl_str_%d\n", v.as.index);
                push_reg(g, "t0");
                push_kind(g, (Kind){K_STR, v.as.index});
            } else if (v.type == VAL_FN) {
                out(g, "    la t0, jl_fn_%d\n", v.as.index);
                push_reg(g, "t0");
                push_kind(g, (Kind){K_FN, v.as.index});
            } else {
                error(g, "this constant isn't supported in native code");
            }
            break;
        }
        case OP_NIL:
            push_reg(g, "zero");
            push_kind(g, kind_of(K_NIL));
            break;
        case OP_FALSE:
            push_reg(g, "zero");
            push_kind(g, kind_of(K_BOOL));
            break;
        case OP_TRUE:
            out(g, "    li t0, 1\n");
            push_reg(g, "t0");
            push_kind(g, kind_of(K_BOOL));
            break;
        case OP_POP:
            out(g, "    addi s1, s1, -4\n");
            pop_kinds(g, 1);
            break;
        case OP_DUP2:
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    sw t0, 0(s1)\n    sw t1, 4(s1)\n    addi s1, s1, 8\n");
            push_kind(g, top(g, 1));
            push_kind(g, top(g, 1));
            break;
        case OP_GET_LOCAL: {
            int slot = ch->code[off + 1];
            out(g, "    lw t0, %d(s0)\n", 4 * slot);
            push_reg(g, "t0");
            push_kind(g, slot < g->depth ? read_kind(g->kinds[slot], g->local_set[slot]) : kind_of(K_INT));
            break;
        }
        case OP_SET_LOCAL: {
            int slot = ch->code[off + 1];
            if (slot < g->depth) {
                if (!same_storage(g->kinds[slot], top(g, 0))) {
                    error(g, "in native code a variable can't hold a string or function in one place "
                             "and another kind of value in another");
                    break;
                }
                g->kinds[slot] = merge(g->kinds[slot], top(g, 0));
            }
            out(g, "    lw t0, -4(s1)\n    sw t0, %d(s0)\n", 4 * slot);
            break;
        }
        case OP_GET_GLOBAL: {
            int gi = ch->code[off + 1];
            Global *gl = &g->vm->globals[gi];
            if (gl->value.type == VAL_NATIVE && gl->defined) {
                push_reg(g, "zero");
                push_kind(g, (Kind){K_NATIVE, gl->value.as.index});
            } else {
                out(g, "    la t0, jl_g_%d\n    lw t0, 0(t0)\n", gi);
                push_reg(g, "t0");
                if (g->gbind[gi] >= 0) push_kind(g, (Kind){K_FN, (uint16_t)g->gbind[gi]});
                else push_kind(g, g->gkind_set[gi] ? read_kind(g->gkind[gi], g->reassigned[gi]) : kind_of(K_INT));
            }
            break;
        }
        case OP_SET_GLOBAL:
        case OP_DEF_GLOBAL: {
            int gi = ch->code[off + 1];
            Kind k = top(g, 0);
            if (k.kind == K_NATIVE) {
                error(g, "in native code, built-in functions can't be stored in variables");
                break;
            }
            if (g->gkind_set[gi] && !same_storage(g->gkind[gi], k)) {
                error(g, "in native code a variable can't hold a string or function in one place "
                         "and another kind of value in another");
                break;
            }
            g->gkind[gi] = g->gkind_set[gi] ? merge(g->gkind[gi], k) : k;
            g->gkind_set[gi] = true;
            if (op == OP_DEF_GLOBAL && k.kind == K_FN && !g->reassigned[gi]) g->gbind[gi] = k.index;
            out(g, "    lw t0, -4(s1)\n    la t1, jl_g_%d\n    sw t0, 0(t1)\n", gi);
            if (op == OP_DEF_GLOBAL) {
                out(g, "    addi s1, s1, -4\n");
                pop_kinds(g, 1);
            }
            break;
        }
        case OP_ADD: binary(g, "add"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_SUB: binary(g, "sub"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_MUL: binary(g, "mul"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_DIV:
        case OP_MOD:
            out(g, "    lw t1, -4(s1)\n");
            trap_unless(g, "bnez t1", op == OP_DIV ? "division by zero" : "modulo by zero");
            binary(g, op == OP_DIV ? "div" : "rem");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_BAND: binary(g, "and"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_BOR: binary(g, "or"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_BXOR: binary(g, "xor"); pop_kinds(g, 2); push_kind(g, kind_of(K_INT)); break;
        case OP_SHL:
        case OP_SHR:
            out(g, "    lw t1, -4(s1)\n");
            trap_num_unless(g, "sltiu t2, t1, 32\n    bnez t2", "shift amount ", "t1", " is out of range (it must be 0..31)");
            binary(g, op == OP_SHL ? "sll" : "sra");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_NEG:
            out(g, "    lw t0, -4(s1)\n    neg t0, t0\n    sw t0, -4(s1)\n");
            pop_kinds(g, 1);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_BNOT:
            out(g, "    lw t0, -4(s1)\n    not t0, t0\n    sw t0, -4(s1)\n");
            pop_kinds(g, 1);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_NOT:
        case OP_BOOL:
            out(g, "    lw t0, -4(s1)\n    %s t0, t0\n    sw t0, -4(s1)\n", op == OP_NOT ? "seqz" : "snez");
            pop_kinds(g, 1);
            push_kind(g, kind_of(K_BOOL));
            break;
        case OP_EQ:
        case OP_NE: {
            Kind ka = top(g, 1), kb = top(g, 0);
            bool known_a = ka.kind == K_NIL || ka.kind == K_BOOL || ka.kind == K_STR || ka.kind == K_FN;
            bool known_b = kb.kind == K_NIL || kb.kind == K_BOOL || kb.kind == K_STR || kb.kind == K_FN;
            if (known_a && known_b && ka.kind != kb.kind) {
                /* values of different types are never equal */
                out(g, "    li t0, %d\n    sw t0, -8(s1)\n    addi s1, s1, -4\n", op == OP_NE);
                pop_kinds(g, 2);
                push_kind(g, kind_of(K_BOOL));
                break;
            }
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    xor t0, t0, t1\n    %s t0, t0\n    sw t0, -8(s1)\n    addi s1, s1, -4\n",
                op == OP_EQ ? "seqz" : "snez");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_BOOL));
            break;
        }
        case OP_LT: binary(g, "slt"); pop_kinds(g, 2); push_kind(g, kind_of(K_BOOL)); break;
        case OP_GT:
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    slt t0, t1, t0\n    sw t0, -8(s1)\n    addi s1, s1, -4\n");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_BOOL));
            break;
        case OP_LE:
        case OP_GE:
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    slt t0, %s\n    xori t0, t0, 1\n    sw t0, -8(s1)\n    addi s1, s1, -4\n",
                op == OP_LE ? "t1, t0" : "t0, t1");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_BOOL));
            break;
        case OP_JUMP:
        case OP_LOOP:
        case OP_JUMP_IF_FALSE: {
            int t = jump_target(ch, off);
            if (op == OP_JUMP_IF_FALSE) {
                int skip = new_label(g);
                out(g, "    addi s1, s1, -4\n    lw t0, 0(s1)\n    bnez t0, .Lok%d\n    j .L%d_%d\n.Lok%d:\n", skip, fi, t, skip);
                pop_kinds(g, 1);
            } else {
                out(g, "    j .L%d_%d\n", fi, t);
                reachable = false;
            }
            if (tdepth[t] < 0) {
                tdepth[t] = g->depth;
                if (g->depth > 0) tkind[t] = g->kinds[g->depth - 1];
            } else if (t > off && g->depth > 0) {
                tkind[t] = merge(tkind[t], g->kinds[g->depth - 1]);
            }
            break;
        }
        case OP_CALL: {
            int argc = ch->code[off + 1];
            Kind callee = top(g, argc);
            if (callee.kind == K_NATIVE) {
                gen_native_call(g, callee.index, argc);
            } else if (callee.kind == K_FN && callee.index != DYNAMIC) {
                Function *f = &g->vm->prog.fns[callee.index];
                if (f->arity != argc) {
                    error(g, "%s() takes %d argument%s but got %d", f->name, f->arity, f->arity == 1 ? "" : "s", argc);
                    break;
                }
                out(g, "    call jl_fn_%d\n", callee.index);
                pop_kinds(g, argc + 1);
                push_kind(g, kind_of(g->returns_nil[callee.index] ? K_NIL : K_INT));
            } else if (callee.kind == K_STR || callee.kind == K_BOOL || callee.kind == K_NIL) {
                error(g, "this value can't be called (only functions can be called)");
            } else {
                /* a function stored in a variable: check it at run time */
                out(g, "    lw t2, %d(s1)\n", -4 * (argc + 1));
                trap_unless(g, "bnez t2", "can't call this value (only functions can be called)");
                out(g, "    lw t3, -4(t2)\n    li t4, %d\n", argc);
                trap_unless(g, "beq t3, t4", "wrong number of arguments for this function");
                out(g, "    jalr ra, 0(t2)\n");
                pop_kinds(g, argc + 1);
                push_kind(g, kind_of(K_INT));
            }
            break;
        }
        case OP_RETURN:
            out(g, "    lw t0, -4(s1)\n    sw t0, 0(s0)\n    addi s1, s0, 4\n");
            out(g, "    la t1, jl_depth\n    lw t2, 0(t1)\n    addi t2, t2, -1\n    sw t2, 0(t1)\n");
            out(g, "    lw ra, 12(sp)\n    lw s0, 8(sp)\n    addi sp, sp, 16\n    ret\n");
            reachable = false;
            break;
        case OP_INDEX_GET:
            if (top(g, 1).kind == K_STR) {
                error(g, "indexing strings isn't available in native code yet");
                break;
            }
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n");
            trap_num_unless(g, "sltiu t2, t1, 32\n    bnez t2", "bit ", "t1", " doesn't exist: an int has bits 0..31");
            out(g, "    srl t0, t0, t1\n    andi t0, t0, 1\n    sw t0, -8(s1)\n    addi s1, s1, -4\n");
            pop_kinds(g, 2);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_INDEX_UPDATE:
            if (top(g, 2).kind == K_STR) {
                error(g, "strings are read-only");
                break;
            }
            out(g, "    lw t0, -12(s1)\n    lw t1, -8(s1)\n    lw t3, -4(s1)\n");
            trap_num_unless(g, "sltiu t2, t1, 32\n    bnez t2", "bit ", "t1", " doesn't exist: an int has bits 0..31");
            trap_unless(g, "sltiu t2, t3, 2\n    bnez t2", "a bit can only be set to 0 or 1");
            out(g, "    li t4, 1\n    sll t4, t4, t1\n    not t4, t4\n    and t0, t0, t4\n    sll t3, t3, t1\n    or t0, t0, t3\n");
            out(g, "    sw t0, -12(s1)\n    addi s1, s1, -8\n");
            pop_kinds(g, 3);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_INDEX_SET:
            error(g, "can't change a bit of a temporary value; store it in a variable first");
            break;
        case OP_SLICE_GET:
            out(g, "    lw a0, -12(s1)\n    lw a1, -8(s1)\n    lw a2, -4(s1)\n    li a3, %d\n    call jl_rt_slice_get\n", g->line);
            out(g, "    sw a0, -12(s1)\n    addi s1, s1, -8\n");
            pop_kinds(g, 3);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_SLICE_UPDATE:
            out(g, "    lw a0, -16(s1)\n    lw a1, -12(s1)\n    lw a2, -8(s1)\n    lw a3, -4(s1)\n    li a4, %d\n    call jl_rt_slice_set\n", g->line);
            out(g, "    sw a0, -16(s1)\n    addi s1, s1, -12\n");
            pop_kinds(g, 4);
            push_kind(g, kind_of(K_INT));
            break;
        case OP_ON:
            out(g, "    addi s1, s1, -8\n");
            pop_kinds(g, 2);
            break;
        default:
            error(g, "instruction %u isn't supported in native code", op);
        }
        off += op_size(op);
    }
    free(target);
    free(tdepth);
    free(tkind);
}

/* ------------------------------------------------------------------ */
/* runtime library (written in assembly)                               */
/* ------------------------------------------------------------------ */

static const char *RUNTIME =
    "\n# ---- JLang runtime -------------------------------------------\n"
    "\n# jl_rt_puts(a0 = address of a length-prefixed string)\n"
    "jl_rt_puts:\n"
    "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s2, 8(sp)\n    sw s3, 4(sp)\n"
    "    lw s3, 0(a0)\n    addi s2, a0, 4\n"
    "jl_rt_puts_loop:\n"
    "    beqz s3, jl_rt_puts_done\n    lbu a0, 0(s2)\n    call jl_hal_putc\n"
    "    addi s2, s2, 1\n    addi s3, s3, -1\n    j jl_rt_puts_loop\n"
    "jl_rt_puts_done:\n"
    "    lw ra, 12(sp)\n    lw s2, 8(sp)\n    lw s3, 4(sp)\n    addi sp, sp, 16\n    ret\n"
    "\n# jl_rt_putbool(a0)\n"
    "jl_rt_putbool:\n"
    "    la t0, jl_rt_true\n    bnez a0, jl_rt_putbool_go\n    la t0, jl_rt_false\n"
    "jl_rt_putbool_go:\n    mv a0, t0\n    j jl_rt_puts\n"
    "\n# jl_rt_putint(a0): signed decimal\n"
    "jl_rt_putint:\n"
    "    addi sp, sp, -32\n    sw ra, 28(sp)\n    sw s2, 24(sp)\n    sw s3, 20(sp)\n"
    "    mv s2, a0\n    bgez s2, jl_rt_putint_pos\n"
    "    li a0, 45\n    call jl_hal_putc\n    neg s2, s2\n"
    "jl_rt_putint_pos:\n    li s3, 0\n"
    "jl_rt_putint_digit:\n"
    "    li t0, 10\n    remu t1, s2, t0\n    divu s2, s2, t0\n    addi t1, t1, 48\n"
    "    add t2, sp, s3\n    sb t1, 0(t2)\n    addi s3, s3, 1\n    bnez s2, jl_rt_putint_digit\n"
    "jl_rt_putint_out:\n"
    "    addi s3, s3, -1\n    add t2, sp, s3\n    lbu a0, 0(t2)\n    call jl_hal_putc\n"
    "    bnez s3, jl_rt_putint_out\n"
    "    lw ra, 28(sp)\n    lw s2, 24(sp)\n    lw s3, 20(sp)\n    addi sp, sp, 32\n    ret\n"
    "\n# jl_rt_putint_w(a0 = value, a1 = width): decimal padded with spaces\n"
    "jl_rt_putint_w:\n"
    "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s2, 8(sp)\n    sw s3, 4(sp)\n    sw s4, 0(sp)\n"
    "    mv s2, a0\n    mv s3, a1\n    li s4, 0\n    mv t1, s2\n"
    "    bgez t1, jl_rt_putint_w_count\n    li s4, 1\n    neg t1, t1\n"
    "jl_rt_putint_w_count:\n"
    "    li t2, 10\n    addi s4, s4, 1\n    divu t1, t1, t2\n    bnez t1, jl_rt_putint_w_count\n"
    "jl_rt_putint_w_pad:\n"
    "    bge s4, s3, jl_rt_putint_w_go\n    li a0, 32\n    call jl_hal_putc\n    addi s4, s4, 1\n    j jl_rt_putint_w_pad\n"
    "jl_rt_putint_w_go:\n    mv a0, s2\n    call jl_rt_putint\n"
    "    lw ra, 12(sp)\n    lw s2, 8(sp)\n    lw s3, 4(sp)\n    lw s4, 0(sp)\n    addi sp, sp, 16\n    ret\n"
    "\n# jl_rt_puthex(a0 = value, a1 = minimum digits): 0x... uppercase\n"
    "jl_rt_puthex:\n"
    "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s2, 8(sp)\n    sw s3, 4(sp)\n"
    "    mv s2, a0\n    mv s3, a1\n"
    "    li a0, 48\n    call jl_hal_putc\n    li a0, 120\n    call jl_hal_putc\n"
    "    li t0, 1\n"
    "jl_rt_puthex_count:\n"
    "    li t1, 8\n    bge t0, t1, jl_rt_puthex_counted\n    slli t2, t0, 2\n    srl t2, s2, t2\n"
    "    beqz t2, jl_rt_puthex_counted\n    addi t0, t0, 1\n    j jl_rt_puthex_count\n"
    "jl_rt_puthex_counted:\n    bge s3, t0, jl_rt_puthex_loop\n    mv s3, t0\n"
    "jl_rt_puthex_loop:\n"
    "    addi s3, s3, -1\n    li a0, 48\n    li t1, 8\n    bge s3, t1, jl_rt_puthex_emit\n"
    "    slli t2, s3, 2\n    srl t2, s2, t2\n    andi t2, t2, 15\n    li t1, 10\n"
    "    addi a0, t2, 48\n    blt t2, t1, jl_rt_puthex_emit\n    addi a0, t2, 55\n"
    "jl_rt_puthex_emit:\n    call jl_hal_putc\n    bnez s3, jl_rt_puthex_loop\n"
    "    lw ra, 12(sp)\n    lw s2, 8(sp)\n    lw s3, 4(sp)\n    addi sp, sp, 16\n    ret\n"
    ;

static const char *RUNTIME2 =
    "\n# jl_rt_putbin(a0 = value, a1 = minimum digits): 0b...\n"
    "jl_rt_putbin:\n"
    "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s2, 8(sp)\n    sw s3, 4(sp)\n"
    "    mv s2, a0\n    mv s3, a1\n"
    "    li a0, 48\n    call jl_hal_putc\n    li a0, 98\n    call jl_hal_putc\n"
    "    li t0, 32\n"
    "jl_rt_putbin_count:\n"
    "    li t1, 1\n    beq t0, t1, jl_rt_putbin_counted\n    addi t2, t0, -1\n    srl t2, s2, t2\n"
    "    bnez t2, jl_rt_putbin_counted\n    addi t0, t0, -1\n    j jl_rt_putbin_count\n"
    "jl_rt_putbin_counted:\n    bge s3, t0, jl_rt_putbin_loop\n    mv s3, t0\n"
    "jl_rt_putbin_loop:\n"
    "    addi s3, s3, -1\n    li a0, 48\n    li t1, 32\n    bge s3, t1, jl_rt_putbin_emit\n"
    "    srl t2, s2, s3\n    andi t2, t2, 1\n    add a0, a0, t2\n"
    "jl_rt_putbin_emit:\n    call jl_hal_putc\n    bnez s3, jl_rt_putbin_loop\n"
    "    lw ra, 12(sp)\n    lw s2, 8(sp)\n    lw s3, 4(sp)\n    addi sp, sp, 16\n    ret\n"
    "\n# jl_rt_slice_check(a1 = hi, a2 = lo, a3 = line) -> t5 = mask, or a trap\n"
    "jl_rt_slice_check:\n"
    "    li t0, 31\n    bltu t0, a1, jl_rt_slice_bad\n    bltu t0, a2, jl_rt_slice_bad\n"
    "    blt a1, a2, jl_rt_slice_bad\n"
    "    sub t1, a1, a2\n    addi t1, t1, 1\n    li t5, -1\n    li t0, 32\n    beq t1, t0, jl_rt_slice_ok\n"
    "    li t5, 1\n    sll t5, t5, t1\n    addi t5, t5, -1\n"
    "jl_rt_slice_ok:\n    ret\n"
    "jl_rt_slice_bad:\n    mv a0, a3\n    la a1, jl_rt_msg_range\n    j jl_rt_trap\n"
    "\n# jl_rt_slice_get(a0 = value, a1 = hi, a2 = lo, a3 = line) -> a0\n"
    "jl_rt_slice_get:\n"
    "    mv t6, ra\n    mv t4, a0\n    call jl_rt_slice_check\n    mv ra, t6\n"
    "    srl a0, t4, a2\n    and a0, a0, t5\n    ret\n"
    "\n# jl_rt_slice_set(a0 = value, a1 = hi, a2 = lo, a3 = new bits, a4 = line) -> a0\n"
    "jl_rt_slice_set:\n"
    "    mv t6, ra\n    mv t4, a0\n    mv t3, a3\n    mv a3, a4\n    call jl_rt_slice_check\n    mv ra, t6\n"
    "    li t0, -1\n    beq t5, t0, jl_rt_slice_set_go\n"
    "    bltu t5, t3, jl_rt_slice_big\n"
    "jl_rt_slice_set_go:\n"
    "    sll t0, t5, a2\n    not t0, t0\n    and t4, t4, t0\n    and t3, t3, t5\n    sll t3, t3, a2\n"
    "    or a0, t4, t3\n    ret\n"
    "jl_rt_slice_big:          # t3 = value, t1 = width, t5 = max\n"
    "    mv s2, t3\n    mv s3, t1\n    mv s4, t5\n"
    "    la a0, jl_rt_msg_prefix\n    call jl_rt_puts\n    mv a0, a4\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_colon\n    call jl_rt_puts\n    mv a0, s2\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_fit1\n    call jl_rt_puts\n    mv a0, s3\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_fit2\n    li t0, 1\n    bne s3, t0, jl_rt_slice_big_s\n    la a0, jl_rt_msg_fit2s\n"
    "jl_rt_slice_big_s:\n    call jl_rt_puts\n    mv a0, s4\n    call jl_rt_putint\n"
    "    li a0, 41\n    call jl_hal_putc\n    li a0, 10\n    call jl_hal_putc\n    li a0, 70\n    call jl_hal_halt\n"
    "    j jl_rt_hang\n"
    "\n# jl_rt_trap_num(a0 = line, a1 = prefix, a2 = number, a3 = suffix)\n"
    "jl_rt_trap_num:\n"
    "    mv s2, a0\n    mv s3, a1\n    mv s4, a2\n    mv s5, a3\n"
    "    la a0, jl_rt_msg_prefix\n    call jl_rt_puts\n    mv a0, s2\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_colon\n    call jl_rt_puts\n    mv a0, s3\n    call jl_rt_puts\n"
    "    mv a0, s4\n    call jl_rt_putint\n    mv a0, s5\n    call jl_rt_puts\n"
    "    li a0, 10\n    call jl_hal_putc\n    li a0, 70\n    call jl_hal_halt\n    j jl_rt_hang\n"
    "\n# jl_rt_trap(a0 = JLang line, a1 = message): print the error and stop\n"
    "jl_rt_trap:\n"
    "    mv s2, a0\n    mv s3, a1\n"
    "    la a0, jl_rt_msg_prefix\n    call jl_rt_puts\n    mv a0, s2\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_colon\n    call jl_rt_puts\n    mv a0, s3\n    call jl_rt_puts\n"
    "    li a0, 10\n    call jl_hal_putc\n    li a0, 70\n    call jl_hal_halt\n"
    "jl_rt_hang:\n    j jl_rt_hang\n";

static const char *SIM_HAL =
    "\n# ---- HAL for the simulated board (jlang emu) -----------------\n"
    "jl_hal_putc:\n    li t0, 0x10000000\n    sw a0, 0(t0)\n    ret\n"
    "jl_hal_halt:\n    li t0, 0x10003000\n    sw a0, 0(t0)\n    j jl_hal_halt\n"
    "jl_hal_pin_mode:          # a0 = pin, a1 = 0 in, 1 out, 2 in with pull-up\n"
    "    li t0, 1\n    sll t0, t0, a0\n    not t1, t0\n"
    "    li t2, 0x10001000\n    lw t3, 0(t2)\n    and t3, t3, t1\n"
    "    li t4, 1\n    bne a1, t4, jl_hal_pin_mode_dir\n    or t3, t3, t0\n"
    "jl_hal_pin_mode_dir:\n    sw t3, 0(t2)\n"
    "    lw t3, 4(t2)\n    and t3, t3, t1\n    li t4, 2\n    bne a1, t4, jl_hal_pin_mode_pull\n    or t3, t3, t0\n"
    "jl_hal_pin_mode_pull:\n    sw t3, 4(t2)\n    ret\n"
    "jl_hal_pin_write:         # a0 = pin, a1 = level\n"
    "    li t0, 1\n    sll t0, t0, a0\n    li t2, 0x10001000\n    lw t3, 8(t2)\n"
    "    not t1, t0\n    and t3, t3, t1\n    beqz a1, jl_hal_pin_write_go\n    or t3, t3, t0\n"
    "jl_hal_pin_write_go:\n    sw t3, 8(t2)\n    ret\n"
    "jl_hal_pin_read:          # a0 = pin -> a0 = level\n"
    "    li t2, 0x10001000\n    lw t3, 12(t2)\n    srl a0, t3, a0\n    andi a0, a0, 1\n    ret\n"
    "jl_hal_delay:\n    li t0, 0x10002004\n    sw a0, 0(t0)\n    ret\n"
    "jl_hal_millis:\n    li t0, 0x10002000\n    lw a0, 0(t0)\n    ret\n";

static void emit_string(Gen *g, const char *label, const char *s, uint32_t n) {
    out(g, "    .align 2\n%s:\n    .word %u\n", label, (unsigned)n);
    for (uint32_t i = 0; i < n; i += 16) {
        out(g, "    .byte ");
        for (uint32_t j = i; j < n && j < i + 16; j++) out(g, "%s%u", j > i ? ", " : "", (unsigned)(uint8_t)s[j]);
        out(g, "\n");
    }
}

static void prescan(Gen *g, int first, int last) {
    for (int i = 0; i < JL_GLOBALS_MAX; i++) g->gbind[i] = -1;
    for (int fi = first; fi < last; fi++) {
        Chunk *ch = &g->vm->prog.fns[fi].chunk;
        uint8_t prev = 0xFF;
        g->returns_nil[fi] = true;
        for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
            if (ch->code[off] == OP_RETURN && prev != OP_NIL) g->returns_nil[fi] = false;
            prev = ch->code[off];
        }
        for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
            uint8_t op = ch->code[off];
            if (op == OP_SET_GLOBAL) g->reassigned[ch->code[off + 1]] = true;
            /* CONST name; CONST fn; ON  registers a handler */
            if (op == OP_ON && off >= 6 && ch->code[off - 6] == OP_CONST && ch->code[off - 3] == OP_CONST) {
                Value name = ch->consts[u16_at(ch->code, off - 5)];
                Value fn = ch->consts[u16_at(ch->code, off - 2)];
                if (name.type == VAL_STR && fn.type == VAL_FN && g->nhandlers < JL_EVENTS_MAX) {
                    snprintf(g->handlers[g->nhandlers].name, sizeof g->handlers[0].name, "%s",
                             g->vm->prog.strs[name.as.index].chars);
                    g->handlers[g->nhandlers].fn = fn.as.index;
                    g->nhandlers++;
                }
            }
        }
    }
}

char *rv_generate(JlVM *vm, const char *source, const RvGenOptions *opt) {
    int first = jl_compile(vm, source);
    if (first < 0) return NULL;

    Gen *g = calloc(1, sizeof *g);
    if (g == NULL) return NULL;
    g->vm = vm;
    g->opt = opt;
    g->first_fn = first;
    g->str_used = calloc((size_t)vm->prog.str_count + 1, 1);
    g->returns_nil = calloc((size_t)vm->prog.fn_count + 1, 1);
    int last = vm->prog.fn_count;
    prescan(g, first, last);

    out(g, "# Generated by JLang %s from a .jlang program.\n", JL_VERSION);
    out(g, "# Target: RV32IM, GNU assembler syntax.\n");
    if (!opt->sim) {
        out(g, "#\n# Link with a board file that defines the HAL functions:\n");
        out(g, "#   jl_hal_putc, jl_hal_halt, jl_hal_pin_mode, jl_hal_pin_write,\n");
        out(g, "#   jl_hal_pin_read, jl_hal_delay, jl_hal_millis\n");
        out(g, "# and call jl_main() once, then jl_on_<event>() for each event.\n");
    }
    out(g, "\n    .text\n");

    /* C-callable entry points */
    out(g, "\n    .globl jl_main\njl_main:\n");
    out(g, "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s0, 8(sp)\n    sw s1, 4(sp)\n");
    out(g, "    la t0, jl_vsp\n    lw s1, 0(t0)\n    sw zero, 0(s1)\n    addi s1, s1, 4\n");
    out(g, "    call jl_fn_%d\n    addi s1, s1, -4\n    la t0, jl_vsp\n    sw s1, 0(t0)\n", first);
    out(g, "    lw ra, 12(sp)\n    lw s0, 8(sp)\n    lw s1, 4(sp)\n    addi sp, sp, 16\n    ret\n");
    for (int i = 0; i < g->nhandlers; i++) {
        out(g, "\n    .globl jl_on_%s\njl_on_%s:\n", g->handlers[i].name, g->handlers[i].name);
        out(g, "    addi sp, sp, -16\n    sw ra, 12(sp)\n    sw s0, 8(sp)\n    sw s1, 4(sp)\n");
        out(g, "    la t0, jl_vsp\n    lw s1, 0(t0)\n    sw zero, 0(s1)\n    addi s1, s1, 4\n");
        out(g, "    call jl_fn_%d\n    addi s1, s1, -4\n    la t0, jl_vsp\n    sw s1, 0(t0)\n", g->handlers[i].fn);
        out(g, "    lw ra, 12(sp)\n    lw s0, 8(sp)\n    lw s1, 4(sp)\n    addi sp, sp, 16\n    ret\n");
    }

    if (opt->sim) {
        out(g, "\n    .globl _start\n_start:\n    call jl_main\n");
        for (int i = 0; i < opt->nemits && !g->failed; i++) {
            bool found = false;
            for (int h = 0; h < g->nhandlers; h++) found |= strcmp(g->handlers[h].name, opt->emits[i]) == 0;
            if (!found) {
                g->line = 0;
                g->failed = true;
                vm_errf(vm, "error: --emit %s: the program has no 'on %s { ... }' handler\n", opt->emits[i], opt->emits[i]);
                break;
            }
            out(g, "    call jl_on_%s\n", opt->emits[i]);
        }
        out(g, "    li a0, 0\n    call jl_hal_halt\n");
    }

    /* the script first, so globals know what they hold before functions use them */
    gen_function(g, first);
    for (int fi = first + 1; fi < last && !g->failed; fi++) gen_function(g, fi);

    out(g, "%s%s", RUNTIME, RUNTIME2);
    if (opt->sim) out(g, "%s", SIM_HAL);

    out(g, "\n    .section .rodata\n");
    emit_string(g, "jl_rt_true", "true", 4);
    emit_string(g, "jl_rt_false", "false", 5);
    emit_string(g, "jl_rt_msg_prefix", "runtime error (line ", 20);
    emit_string(g, "jl_rt_msg_colon", "): ", 3);
    {
        const char *r = "bit range is invalid (write high..low within 0..31)";
        emit_string(g, "jl_rt_msg_range", r, (uint32_t)strlen(r));
        emit_string(g, "jl_rt_msg_fit1", " doesn't fit in ", 16);
        emit_string(g, "jl_rt_msg_fit2", " bits (the max is ", 18);
        emit_string(g, "jl_rt_msg_fit2s", " bit (the max is ", 17);
    }
    for (int i = 0; i < vm->prog.str_count; i++) {
        if (!g->str_used[i]) continue;
        char label[32];
        snprintf(label, sizeof label, "jl_str_%d", i);
        emit_string(g, label, vm->prog.strs[i].chars, vm->prog.strs[i].len);
    }
    for (int i = 0; i < g->nlits; i++) {
        char label[32];
        snprintf(label, sizeof label, "jl_lit_%d", i);
        emit_string(g, label, g->lits[i], g->lit_lens[i]);
    }

    out(g, "\n    .data\n    .align 2\njl_vsp:\n    .word jl_vstack\njl_depth:\n    .word 0\n");
    for (int i = 0; i < vm->global_count; i++) out(g, "jl_g_%d:\n    .word 0\n", i);
    out(g, "\n    .bss\n    .align 2\njl_vstack:\n    .zero 16384\njl_vstack_end:\n");

    char *result = g->failed ? NULL : g->buf;
    if (g->failed) free(g->buf);
    for (int i = 0; i < g->nlits; i++) free(g->lits[i]);
    free(g->lits);
    free(g->lit_lens);
    free(g->str_used);
    free(g->returns_nil);
    free(g);
    return result;
}
