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
#include "hw.h"
#include "rv_codegen.h"

enum { K_INT, K_BOOL, K_NIL, K_STR, K_FN, K_NATIVE, K_ARR, K_STRUCT };

typedef struct {
    uint8_t kind;
    uint16_t index; /* string, function, built-in, or struct type index (K_ARR: 1 = nested arrays) */
} Kind;

#define MAX_DEPTH 600
#define DYNAMIC 0xFFFF /* kind index when the exact string or function isn't known */

enum { V_MEM = 0, V_REG = 1, V_IMM = 2, V_SREG = 3 };

typedef struct {
    uint8_t loc;
    uint8_t reg;
    int32_t imm;
} VSlot;

#define PROMO_COUNT 6

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
    bool *returns_bool; /* per function: every return gives a bool */
    bool local_set[256]; /* current function: slots assigned after their let */
    /* extra string literals made by the code generator */
    char **lits;
    uint32_t *lit_lens;
    int nlits;
    /* static .bss buffers for top-level arrays and structs */
    uint32_t *static_words;
    int nstatics;
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
    int n_promo; /* promoted local slots 1..n_promo kept in s2..s7 */
    Kind kinds[MAX_DEPTH];
    VSlot vstack[MAX_DEPTH];
    int depth;
    int mem_depth; /* [0..mem_depth) live in memory at s0..s1; [mem_depth..depth) cached */
    uint8_t reg_used; /* bitmask over CACHE_REGS[0..6] */
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
    g->kinds[g->depth] = k;
    g->vstack[g->depth].loc = V_MEM;
    g->depth++;
    g->mem_depth = g->depth;
}

static Kind kind_of(uint8_t kind) {
    Kind k = {kind, 0};
    return k;
}

/* An int known for certain: a literal or the result of arithmetic. Plain
 * K_INT can also be a value whose type isn't tracked, such as a variable
 * declared without a value, so only these are compared by type. */
#define CERTAIN 1
static Kind top(Gen *g, int n);
static Kind certain_int(void) { return (Kind){K_INT, CERTAIN}; }

static int add_static(Gen *g, uint32_t words) {
    if (words == 0) words = 1;
    g->static_words = jl_xrealloc(g->static_words, (size_t)(g->nstatics + 1) * sizeof(uint32_t));
    g->static_words[g->nstatics] = words;
    return g->nstatics++;
}

static bool not_a_number(Kind k) {
    return k.kind == K_BOOL || k.kind == K_NIL || k.kind == K_STR || k.kind == K_FN ||
           k.kind == K_ARR || k.kind == K_STRUCT;
}

static const char *kind_name(Kind k) {
    switch (k.kind) {
    case K_BOOL: return "bool";
    case K_NIL: return "nil";
    case K_STR: return "str";
    case K_FN: return "fn";
    case K_NATIVE: return "built-in";
    case K_ARR: return "array";
    case K_STRUCT: return "struct";
    default: return "int";
    }
}

/* Arithmetic only works on numbers. The VM checks this as the program
 * runs; native code has no type at run time, so it must be known here. */
static bool numbers_only(Gen *g, const char *op, int operands) {
    Kind a = top(g, operands - 1), b = top(g, 0);
    if (not_a_number(a) || (operands == 2 && not_a_number(b))) {
        if (operands == 2) error(g, "can't use '%s' on %s and %s (it only works on ints)", op, kind_name(a), kind_name(b));
        else error(g, "can't use '%s' on %s (it only works on ints)", op, kind_name(a));
        return false;
    }
    return true;
}

static Kind top(Gen *g, int n) {
    if (g->depth - 1 - n < 0) return kind_of(K_INT);
    return g->kinds[g->depth - 1 - n];
}

static void pop_kinds(Gen *g, int n) {
    for (int i = 0; i < n && g->depth > 0; i++) {
        g->depth--;
        if (g->vstack[g->depth].loc == V_REG) g->reg_used &= (uint8_t)~(1u << g->vstack[g->depth].reg);
    }
    if (g->mem_depth > g->depth) g->mem_depth = g->depth;
}

static const char *const CACHE_REGS[7] = {"t3", "t4", "t5", "t6", "a5", "a6", "a7"};
#define CACHE_COUNT 7
static const char *const PROMO_REGS[PROMO_COUNT] = {"s2", "s3", "s4", "s5", "s6", "s7"};

static void free_reg(Gen *g, int r) {
    if (r >= 0 && r < CACHE_COUNT) g->reg_used &= (uint8_t)~(1u << r);
}

/* Flush cached slots [g->mem_depth .. target) to the memory value stack (s1).
 * Any slot in [1 .. g->n_promo] is also copied into its promoted s2..s7 register. */
static void spill_to(Gen *g, int target) {
    if (target > g->depth) target = g->depth;
    if (target <= g->mem_depth) return;
    int n = target - g->mem_depth;
    for (int i = 0; i < n; i++) {
        int slot_idx = g->mem_depth + i;
        VSlot *v = &g->vstack[slot_idx];
        int off = 4 * i;
        bool promo = slot_idx >= 1 && slot_idx <= g->n_promo;
        const char *preg = promo ? PROMO_REGS[slot_idx - 1] : NULL;
        if (v->loc == V_REG) {
            out(g, "    sw %s, %d(s1)\n", CACHE_REGS[v->reg], off);
            if (promo) out(g, "    mv %s, %s\n", preg, CACHE_REGS[v->reg]);
            free_reg(g, v->reg);
        } else if (v->loc == V_SREG) {
            out(g, "    sw %s, %d(s1)\n", PROMO_REGS[v->reg], off);
            if (promo && slot_idx - 1 != (int)v->reg) out(g, "    mv %s, %s\n", preg, PROMO_REGS[v->reg]);
        } else if (v->loc == V_IMM) {
            if (v->imm == 0) {
                out(g, "    sw zero, %d(s1)\n", off);
                if (promo) out(g, "    li %s, 0\n", preg);
            } else if (promo) {
                out(g, "    li %s, %d\n    sw %s, %d(s1)\n", preg, (int)v->imm, preg, off);
            } else {
                out(g, "    li t0, %d\n    sw t0, %d(s1)\n", (int)v->imm, off);
            }
        }
        v->loc = V_MEM;
    }
    out(g, "    addi s1, s1, %d\n", 4 * n);
    g->mem_depth = target;
}

static void spill_all(Gen *g) { spill_to(g, g->depth); }

static int alloc_reg(Gen *g) {
    for (int i = 0; i < CACHE_COUNT; i++) {
        if (!(g->reg_used & (1u << i))) {
            g->reg_used |= (uint8_t)(1u << i);
            return i;
        }
    }
    /* All 7 cache registers are live: spill the oldest cached slot(s) until one frees up. */
    while (g->mem_depth < g->depth) {
        bool was_reg = g->vstack[g->mem_depth].loc == V_REG;
        spill_to(g, g->mem_depth + 1);
        if (was_reg) break;
    }
    for (int i = 0; i < CACHE_COUNT; i++) {
        if (!(g->reg_used & (1u << i))) {
            g->reg_used |= (uint8_t)(1u << i);
            return i;
        }
    }
    return 0;
}

static void vpush_reg(Gen *g, int reg, Kind k) {
    if (g->depth >= MAX_DEPTH) {
        error(g, "expression is too deep for native code");
        return;
    }
    g->kinds[g->depth] = k;
    g->vstack[g->depth].loc = V_REG;
    g->vstack[g->depth].reg = (uint8_t)reg;
    g->vstack[g->depth].imm = 0;
    g->depth++;
}

static void vpush_sreg(Gen *g, int sreg, Kind k) {
    if (g->depth >= MAX_DEPTH) {
        error(g, "expression is too deep for native code");
        return;
    }
    g->kinds[g->depth] = k;
    g->vstack[g->depth].loc = V_SREG;
    g->vstack[g->depth].reg = (uint8_t)sreg;
    g->vstack[g->depth].imm = 0;
    g->depth++;
}

static void vpush_imm(Gen *g, int32_t imm, Kind k) {
    if (g->depth >= MAX_DEPTH) {
        error(g, "expression is too deep for native code");
        return;
    }
    g->kinds[g->depth] = k;
    g->vstack[g->depth].loc = V_IMM;
    g->vstack[g->depth].reg = 0;
    g->vstack[g->depth].imm = imm;
    g->depth++;
}

static void vpush_mem(Gen *g, Kind k) {
    if (g->depth >= MAX_DEPTH) {
        error(g, "expression is too deep for native code");
        return;
    }
    g->kinds[g->depth] = k;
    g->vstack[g->depth].loc = V_MEM;
    g->depth++;
    g->mem_depth = g->depth;
}

/* Keep promoted register s2..s7 synced when an op writes a result onto s1. */
static void sync_promo_at(Gen *g, int slot_idx, int s1_off) {
    if (slot_idx >= 1 && slot_idx <= g->n_promo) {
        out(g, "    lw %s, %d(s1)\n", PROMO_REGS[slot_idx - 1], s1_off);
    }
}

static void sync_promo_top(Gen *g) { sync_promo_at(g, g->depth - 1, -4); }

/* Before mutating promoted register `sreg`, detach any cached stack entries
 * referencing `sreg` so earlier pushed operands keep their old value. */
static void materialize_sreg(Gen *g, int sreg, int limit) {
    for (int i = g->mem_depth; i < limit; i++) {
        if (g->vstack[i].loc == V_SREG && g->vstack[i].reg == (uint8_t)sreg) {
            int r = alloc_reg(g);
            if (i < g->mem_depth) continue; /* alloc_reg spilled i already */
            out(g, "    mv %s, %s\n", CACHE_REGS[r], PROMO_REGS[sreg]);
            g->vstack[i].loc = V_REG;
            g->vstack[i].reg = (uint8_t)r;
        }
    }
}

/* Pop top slot into a register without freeing its cache reg yet (caller frees *used_reg). */
static const char *vpop_reg(Gen *g, const char *scratch, int *used_reg) {
    *used_reg = -1;
    if (g->depth <= 0) return "zero";
    VSlot v = g->vstack[--g->depth];
    if (v.loc == V_REG) {
        *used_reg = v.reg;
        return CACHE_REGS[v.reg];
    }
    if (v.loc == V_SREG) {
        return PROMO_REGS[v.reg];
    }
    if (v.loc == V_IMM) {
        if (v.imm == 0) return "zero";
        out(g, "    li %s, %d\n", scratch, (int)v.imm);
        return scratch;
    }
    g->mem_depth = g->depth;
    out(g, "    lw %s, -4(s1)\n    addi s1, s1, -4\n", scratch);
    return scratch;
}

/* Pop top two slots (second into *ra, top into *rb). Coalesces double memory pop. */
static void vpop_two(Gen *g, const char **ra, const char **rb, int *fa, int *fb) {
    *fa = -1;
    *fb = -1;
    if (g->depth >= 2 && g->vstack[g->depth - 1].loc == V_MEM && g->vstack[g->depth - 2].loc == V_MEM) {
        g->depth -= 2;
        g->mem_depth = g->depth;
        out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    addi s1, s1, -8\n");
        *ra = "t0";
        *rb = "t1";
        return;
    }
    *rb = vpop_reg(g, "t1", fb);
    *ra = vpop_reg(g, "t0", fa);
}

/* Peek top slot into a register (does not pop). */
static const char *vpeek_reg(Gen *g, const char *scratch) {
    if (g->depth <= 0) return "zero";
    VSlot v = g->vstack[g->depth - 1];
    if (v.loc == V_REG) return CACHE_REGS[v.reg];
    if (v.loc == V_SREG) return PROMO_REGS[v.reg];
    if (v.loc == V_IMM) {
        if (v.imm == 0) return "zero";
        out(g, "    li %s, %d\n", scratch, (int)v.imm);
        return scratch;
    }
    out(g, "    lw %s, -4(s1)\n", scratch);
    return scratch;
}

static int reuse_or_alloc(Gen *g, int fa, int fb) {
    if (fa >= 0) {
        free_reg(g, fb);
        return fa;
    }
    if (fb >= 0) {
        return fb;
    }
    return alloc_reg(g);
}

static bool fits_imm12(int32_t v) { return v >= -2048 && v <= 2047; }

static int pow2_shift(int32_t v) {
    if (v <= 0 || (v & (v - 1)) != 0) return -1;
    int s = 0;
    while ((v >> s) != 1) s++;
    return s;
}

/* Emit branch to .L<fi>_<target_off> when condition (bop r1, r2) holds. */
static void emit_cond_branch(Gen *g, const char *bop, const char *inv_bop,
                             const char *r1, const char *r2, int fi, int target_off, int cur_off) {
    int dist = target_off > cur_off ? target_off - cur_off : cur_off - target_off;
    if (dist < 200) {
        out(g, "    %s %s, %s, .L%d_%d\n", bop, r1, r2, fi, target_off);
    } else {
        int skip = new_label(g);
        out(g, "    %s %s, %s, .Lok%d\n    j .L%d_%d\n.Lok%d:\n", inv_bop, r1, r2, skip, fi, target_off, skip);
    }
}

/* Can a variable hold both? Strings, functions, arrays and structs must keep
 * their kind, because print(), indexing, field access and calls depend on it. */
static bool same_storage(Kind a, Kind b) {
    /* bools are 0 and 1 in native code, so print() needs to know them too */
    if ((a.kind == K_BOOL) != (b.kind == K_BOOL)) return false;
    if ((a.kind == K_STRUCT) || (b.kind == K_STRUCT)) return a.kind == b.kind && a.index == b.index;
    if ((a.kind == K_ARR) || (b.kind == K_ARR)) return a.kind == b.kind;
    bool sa = a.kind == K_STR || a.kind == K_FN || a.kind == K_NATIVE;
    bool sb = b.kind == K_STR || b.kind == K_FN || b.kind == K_NATIVE;
    if (!sa && !sb) return true;
    return a.kind == b.kind && a.kind != K_NATIVE;
}

static Kind merge(Kind a, Kind b) {
    if (a.kind == b.kind && a.index == b.index) return a;
    if (a.kind == b.kind && (a.kind == K_STR || a.kind == K_FN)) return (Kind){a.kind, DYNAMIC};
    if (a.kind == K_ARR && b.kind == K_ARR) return a;
    return kind_of(K_INT);
}

/* The kind a variable read gives: exact only if it is never reassigned. */
static Kind read_kind(Kind k, bool reassigned) {
    if (reassigned && (k.kind == K_STR || k.kind == K_FN)) k.index = DYNAMIC;
    if (reassigned && k.kind == K_NIL) k.kind = K_INT;
    if (reassigned && k.kind == K_INT) k.index = 0;
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
    case K_ARR:
        error(g, "printing whole arrays isn't available in native code yet; print their elements instead");
        break;
    case K_STRUCT:
        error(g, "printing whole structs isn't available in native code yet; print their fields instead");
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
    } else if (strcmp(name, "led") == 0) {
        Kind ck = top(g, 0);
        if (ck.kind == K_STR) {
            /* a colour name is looked up now, so the chip only sees a number */
            uint32_t rgb;
            if (!literal_arg(g, argc, 2, name, &s, &n)) return;
            if (!jl_hw_color_by_name(s, n, &rgb)) {
                error(g, "led(): '%.*s' isn't a colour I know. Use red, green, blue, yellow, cyan, magenta, "
                         "orange, purple, pink, white, warm or off, or a number like 0xFF8000", (int)n, s);
                return;
            }
            out(g, "    li a2, %u\n", (unsigned)rgb);
        } else {
            out(g, "    lw a2, %d(s1)\n", arg_off(argc, 2));
            out(g, "    li t0, 0x1000000\n");
            trap_unless(g, "bltu a2, t0", "led(): a colour is 0xRRGGBB, from 0x000000 to 0xFFFFFF");
        }
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n    call jl_hal_led_set\n", arg_off(argc, 0), arg_off(argc, 1));
        nil_result = true;
    } else if (strcmp(name, "rgb") == 0) {
        static const char *part[3] = {"red", "green", "blue"};
        out(g, "    li a0, 0\n");
        for (int i = 0; i < 3; i++) {
            char m[64];
            snprintf(m, sizeof m, "rgb(): %s must be 0..255", part[i]);
            out(g, "    lw t1, %d(s1)\n", arg_off(argc, i));
            trap_unless(g, "sltiu t0, t1, 256\n    bnez t0", m);
            out(g, "    slli a0, a0, 8\n    or a0, a0, t1\n");
        }
        result_in_a0 = true;
    } else if (strcmp(name, "led_show") == 0 || strcmp(name, "led_clear") == 0) {
        out(g, "    call jl_hal_%s\n", name);
        nil_result = true;
    } else if (strcmp(name, "led_width") == 0 || strcmp(name, "led_height") == 0) {
        out(g, "    call jl_hal_%s\n", name);
        result_in_a0 = true;
    } else if (strcmp(name, "u8") == 0 || strcmp(name, "i8") == 0 || strcmp(name, "u16") == 0 ||
               strcmp(name, "i16") == 0) {
        int bits = name[1] == '8' ? 24 : 16;
        out(g, "    lw a0, %d(s1)\n    slli a0, a0, %d\n    %s a0, a0, %d\n", arg_off(argc, 0), bits,
            name[0] == 'u' ? "srli" : "srai", bits);
        result_in_a0 = true;
    } else if (strcmp(name, "abs") == 0) {
        int notmin = new_label(g), done = new_label(g);
        out(g, "    lw a0, %d(s1)\n    li t0, -2147483648\n    bne a0, t0, .Lok%d\n    li a0, 2147483647\n    j .Lok%d\n"
               ".Lok%d:\n    bgez a0, .Lok%d\n    neg a0, a0\n.Lok%d:\n",
            arg_off(argc, 0), notmin, done, notmin, done, done);
        result_in_a0 = true;
    } else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        int done = new_label(g);
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n    %s, .Lok%d\n    mv a0, a1\n.Lok%d:\n",
            arg_off(argc, 0), arg_off(argc, 1), name[1] == 'i' ? "bge a1, a0" : "bge a0, a1", done, done);
        result_in_a0 = true;
    } else if (strcmp(name, "clamp") == 0) {
        int oklo = new_label(g), okhi = new_label(g);
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n    lw a2, %d(s1)\n",
            arg_off(argc, 0), arg_off(argc, 1), arg_off(argc, 2));
        trap_unless(g, "bge a2, a1", "clamp(): the low limit is bigger than the high limit");
        out(g, "    bge a0, a1, .Lok%d\n    mv a0, a1\n.Lok%d:\n    bge a2, a0, .Lok%d\n    mv a0, a2\n.Lok%d:\n",
            oklo, oklo, okhi, okhi);
        result_in_a0 = true;
    } else if (strcmp(name, "sat_add") == 0 || strcmp(name, "sat_sub") == 0) {
        int done = new_label(g);
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n", arg_off(argc, 0), arg_off(argc, 1));
        if (name[4] == 'a') {
            out(g, "    add t0, a0, a1\n    xor t1, a0, t0\n    xor t2, a1, t0\n    and t1, t1, t2\n");
        } else {
            out(g, "    sub t0, a0, a1\n    xor t1, a0, a1\n    xor t2, a0, t0\n    and t1, t1, t2\n");
        }
        out(g, "    bgez t1, .Lok%d\n    li t0, 2147483647\n    bgez a0, .Lok%d\n    li t0, -2147483648\n"
               ".Lok%d:\n    mv a0, t0\n",
            done, done, done);
        result_in_a0 = true;
    } else if (strcmp(name, "fix_mul") == 0) {
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n    lw a2, %d(s1)\n",
            arg_off(argc, 0), arg_off(argc, 1), arg_off(argc, 2));
        trap_num_unless(g, "sltiu t0, a2, 32\n    bnez t0", "fix_mul(): fractional bits must be 0..31, got ", "a2", "");
        out(g, "    call jl_rt_fix_mul\n");
        result_in_a0 = true;
    } else if (strcmp(name, "fix_div") == 0) {
        out(g, "    lw a0, %d(s1)\n    lw a1, %d(s1)\n    lw a2, %d(s1)\n",
            arg_off(argc, 0), arg_off(argc, 1), arg_off(argc, 2));
        trap_num_unless(g, "sltiu t0, a2, 32\n    bnez t0", "fix_div(): fractional bits must be 0..31, got ", "a2", "");
        trap_unless(g, "bnez a1", "fix_div(): division by zero");
        out(g, "    call jl_rt_fix_div\n");
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
        sync_promo_top(g);
        return;
    } else if (strcmp(name, "len") == 0 && top(g, 0).kind == K_ARR) {
        out(g, "    lw a0, %d(s1)\n    lw a0, 0(a0)\n", arg_off(argc, 0));
        result_in_a0 = true;
    } else if (strcmp(name, "free") == 0 && (top(g, 0).kind == K_ARR || top(g, 0).kind == K_STRUCT)) {
        /* Static .bss arrays and structs need no runtime free; allow free() for VM parity. */
        nil_result = true;
    } else {
        static const char *heap_fns[] = {"alloc", "free", "len", "read16", "read32", "write16", "write32",
                                         "mem", "memmap", "compact", "ring", "pack", "unpack",
                                         "capture", "record", "record_start", "record_stop", "ask", "speak",
                                         "slice", "trim", "kv_get", "kv_set", "kv_del", NULL};
        bool heap = false;
        for (int i = 0; heap_fns[i]; i++) heap |= strcmp(name, heap_fns[i]) == 0;
        if (heap)
            error(g, "%s() isn't available in native code yet (native programs have no heap or "
                     "camera); run it with `jlang run` instead", name);
        else
            error(g, "%s() isn't available in native code yet; run it on the VM with `jlang run` instead", name);
        return;
    }

    /* the result replaces the placeholder slot */
    out(g, "    addi s1, s1, %d\n", -4 * argc);
    if (result_in_a0) out(g, "    sw a0, -4(s1)\n");
    else if (nil_result) out(g, "    sw zero, -4(s1)\n");
    pop_kinds(g, argc + 1);
    push_kind(g, nil_result ? kind_of(K_NIL) : result);
    sync_promo_top(g);
}

/* ------------------------------------------------------------------ */
/* one function                                                        */
/* ------------------------------------------------------------------ */

static uint16_t u16_at(const uint8_t *code, int off) {
    return (uint16_t)((code[off] << 8) | code[off + 1]);
}

static int op_size(uint8_t op) {
    switch (op) {
    case OP_CONST: case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_LOOP: case OP_ARRAY:
    case OP_STRUCT: case OP_GET_FIELD: case OP_SET_FIELD: return 3;
    case OP_INIT_FIELD: return 2;
    case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL:
    case OP_DEF_GLOBAL: case OP_CALL: case OP_TIMER: return 2;
    default: return 1;
    }
}

static int jump_target(const Chunk *ch, int off) {
    uint8_t op = ch->code[off];
    int d = u16_at(ch->code, off + 1);
    return op == OP_LOOP ? off + 3 - d : off + 3 + d;
}

/* Note: vpop_two + ALU helper for non-trapping register/immediate ops. */
static void gen_binop(Gen *g, uint8_t op, const char *insn, const char *imm_insn) {
    VSlot *vb = &g->vstack[g->depth - 1];
    VSlot *va = &g->vstack[g->depth - 2];

    /* 1. Compile-time constant folding when both operands are known immediates. */
    if (va->loc == V_IMM && vb->loc == V_IMM) {
        uint32_t a = (uint32_t)va->imm, b = (uint32_t)vb->imm, r = 0;
        switch (op) {
        case OP_ADD: case OP_ADD_TEMP: r = a + b; break;
        case OP_SUB: r = a - b; break;
        case OP_MUL: r = a * b; break;
        case OP_BAND: r = a & b; break;
        case OP_BOR: r = a | b; break;
        case OP_BXOR: r = a ^ b; break;
        default: break;
        }
        pop_kinds(g, 2);
        vpush_imm(g, (int32_t)r, certain_int());
        return;
    }

    /* 2. Immediate folding on RHS (or commutative LHS for ADD/BAND/BOR/BXOR/MUL). */
    bool comm = (op == OP_ADD || op == OP_ADD_TEMP || op == OP_BAND || op == OP_BOR || op == OP_BXOR || op == OP_MUL);
    if (comm && va->loc == V_IMM && vb->loc != V_IMM) {
        VSlot tmp = *va;
        *va = *vb;
        *vb = tmp;
    }
    if (vb->loc == V_IMM) {
        int32_t imm = vb->imm;
        if (op == OP_MUL) {
            if (imm == 0) {
                pop_kinds(g, 1);
                int fa;
                (void)vpop_reg(g, "t0", &fa);
                free_reg(g, fa);
                vpush_imm(g, 0, certain_int());
                return;
            }
            if (imm == 1) {
                pop_kinds(g, 1);
                g->kinds[g->depth - 1] = certain_int();
                return;
            }
            int sh = pow2_shift(imm);
            if (sh > 0) {
                pop_kinds(g, 1);
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                out(g, "    slli %s, %s, %d\n", CACHE_REGS[dst], ra, sh);
                vpush_reg(g, dst, certain_int());
                return;
            }
        } else if (imm_insn != NULL) {
            int32_t eff = (op == OP_SUB) ? (int32_t)(0u - (uint32_t)imm) : imm;
            if ((op != OP_SUB || imm != INT32_MIN) && fits_imm12(eff)) {
                if ((op == OP_ADD || op == OP_ADD_TEMP || op == OP_SUB || op == OP_BOR || op == OP_BXOR) && eff == 0) {
                    pop_kinds(g, 1);
                    g->kinds[g->depth - 1] = certain_int();
                    return;
                }
                pop_kinds(g, 1);
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                out(g, "    %s %s, %s, %d\n", imm_insn, CACHE_REGS[dst], ra, (int)eff);
                vpush_reg(g, dst, certain_int());
                return;
            }
        }
    }

    /* 3. General register-register op. */
    const char *ra, *rb;
    int fa, fb;
    vpop_two(g, &ra, &rb, &fa, &fb);
    int dst = reuse_or_alloc(g, fa, fb);
    out(g, "    %s %s, %s, %s\n", insn, CACHE_REGS[dst], ra, rb);
    vpush_reg(g, dst, certain_int());
}

static void record_target(Gen *g, int *tdepth, Kind *tkind, int off, int t) {
    if (tdepth[t] < 0) {
        tdepth[t] = g->depth;
        if (g->depth > 0) tkind[t] = g->kinds[g->depth - 1];
    } else if (t > off && g->depth > 0) {
        tkind[t] = merge(tkind[t], g->kinds[g->depth - 1]);
    }
}

static void gen_function(Gen *g, int fi) {
    Function *fn = &g->vm->prog.fns[fi];
    Chunk *ch = &fn->chunk;
    g->fn = fi;
    g->depth = 0;
    g->mem_depth = 0;
    g->reg_used = 0;
    vpush_mem(g, fi == g->first_fn ? kind_of(K_INT) : (Kind){K_FN, (uint16_t)fi}); /* slot 0 */
    for (int i = 0; i < fn->arity; i++) vpush_mem(g, kind_of(K_INT));
    g->line = ch->count > 0 ? ch->lines[0] : 0;

    memset(g->local_set, 0, sizeof g->local_set);
    int max_local = 0;
    bool uses_asm = false;
    for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
        uint8_t op = ch->code[off];
        if (op == OP_SET_LOCAL) {
            int sl = ch->code[off + 1];
            g->local_set[sl] = true;
            if (sl > max_local) max_local = sl;
        } else if (op == OP_GET_LOCAL) {
            int sl = ch->code[off + 1];
            if (sl > max_local) max_local = sl;
        } else if (op == OP_GET_GLOBAL) {
            Global *gl = &g->vm->globals[ch->code[off + 1]];
            if (gl->value.type == VAL_NATIVE && gl->defined && strcmp(gl->name, "asm") == 0) uses_asm = true;
        }
    }
    if (fn->arity > max_local) max_local = fn->arity;
    g->n_promo = uses_asm ? 0 : (max_local > PROMO_COUNT ? PROMO_COUNT : max_local);

    /* jump targets, loop ranges, and the stack depth/kind expected there */
    bool *target = calloc((size_t)ch->count + 1, 1);
    bool *in_loop = calloc((size_t)ch->count + 1, 1);
    int *tdepth = malloc(((size_t)ch->count + 1) * sizeof(int));
    Kind *tkind = calloc((size_t)ch->count + 1, sizeof(Kind));
    for (int i = 0; i <= ch->count; i++) tdepth[i] = -1;
    for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
        uint8_t op = ch->code[off];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_LOOP) {
            int tgt = jump_target(ch, off);
            target[tgt] = true;
            if (op == OP_LOOP) {
                for (int k = tgt; k <= off; k++) in_loop[k] = true;
            }
        }
    }

    int frame_bytes = g->n_promo > 0 ? 32 : 16;
    out(g, "\n    .align 2\n    .word %d\njl_fn_%d:   # %s\n", fn->arity, fi, fn->name);
    out(g, "    addi sp, sp, -%d\n    sw ra, %d(sp)\n    sw s0, %d(sp)\n",
        frame_bytes, frame_bytes - 4, frame_bytes - 8);
    for (int p = 0; p < g->n_promo; p++) {
        out(g, "    sw %s, %d(sp)\n", PROMO_REGS[p], frame_bytes - 12 - 4 * p);
    }
    out(g, "    addi s0, s1, %d\n", -4 * (fn->arity + 1));
    for (int p = 0; p < fn->arity && p < g->n_promo; p++) {
        out(g, "    lw %s, %d(s0)\n", PROMO_REGS[p], 4 * (p + 1));
    }
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
            if (reachable) spill_all(g);
            if (!reachable && tdepth[off] >= 0) {
                g->depth = tdepth[off];
                g->mem_depth = g->depth;
                g->reg_used = 0;
                for (int i = 0; i < g->depth; i++) g->vstack[i].loc = V_MEM;
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
                vpush_imm(g, v.as.i, certain_int());
            } else if (v.type == VAL_STR) {
                g->str_used[v.as.index] = true;
                int r = alloc_reg(g);
                out(g, "    la %s, jl_str_%d\n", CACHE_REGS[r], v.as.index);
                vpush_reg(g, r, (Kind){K_STR, v.as.index});
            } else if (v.type == VAL_FN) {
                int r = alloc_reg(g);
                out(g, "    la %s, jl_fn_%d\n", CACHE_REGS[r], v.as.index);
                vpush_reg(g, r, (Kind){K_FN, v.as.index});
            } else {
                error(g, "this constant isn't supported in native code");
            }
            break;
        }
        case OP_NIL:
            vpush_imm(g, 0, kind_of(K_NIL));
            break;
        case OP_FALSE:
            vpush_imm(g, 0, kind_of(K_BOOL));
            break;
        case OP_TRUE:
            vpush_imm(g, 1, kind_of(K_BOOL));
            break;
        case OP_POP:
            if (g->depth > 0) {
                if (g->vstack[g->depth - 1].loc == V_MEM) {
                    out(g, "    addi s1, s1, -4\n");
                }
                pop_kinds(g, 1);
            }
            break;
        case OP_DUP: {
            Kind k = top(g, 0);
            VSlot v = g->vstack[g->depth - 1];
            if (v.loc == V_IMM) {
                vpush_imm(g, v.imm, k);
            } else if (v.loc == V_SREG) {
                vpush_sreg(g, v.reg, k);
            } else if (v.loc == V_REG) {
                int src = v.reg;
                int r = alloc_reg(g);
                if (g->depth - 1 < g->mem_depth) out(g, "    lw %s, -4(s1)\n", CACHE_REGS[r]);
                else out(g, "    mv %s, %s\n", CACHE_REGS[r], CACHE_REGS[src]);
                vpush_reg(g, r, k);
            } else {
                int r = alloc_reg(g);
                out(g, "    lw %s, -4(s1)\n", CACHE_REGS[r]);
                vpush_reg(g, r, k);
            }
            break;
        }
        case OP_DUP2:
            spill_all(g);
            out(g, "    lw t0, -8(s1)\n    lw t1, -4(s1)\n    sw t0, 0(s1)\n    sw t1, 4(s1)\n    addi s1, s1, 8\n");
            vpush_mem(g, top(g, 1));
            vpush_mem(g, top(g, 1));
            sync_promo_at(g, g->depth - 2, -8);
            sync_promo_at(g, g->depth - 1, -4);
            break;
        case OP_GET_LOCAL: {
            int slot = ch->code[off + 1];
            Kind k = slot < g->depth ? read_kind(g->kinds[slot], g->local_set[slot]) : kind_of(K_INT);
            if (slot < g->mem_depth) {
                if (slot >= 1 && slot <= g->n_promo) {
                    vpush_sreg(g, slot - 1, k);
                } else {
                    int r = alloc_reg(g);
                    out(g, "    lw %s, %d(s0)\n", CACHE_REGS[r], 4 * slot);
                    vpush_reg(g, r, k);
                }
            } else if (slot < g->depth && g->vstack[slot].loc == V_IMM) {
                vpush_imm(g, g->vstack[slot].imm, k);
            } else if (slot < g->depth && g->vstack[slot].loc == V_SREG) {
                vpush_sreg(g, g->vstack[slot].reg, k);
            } else if (slot < g->depth && g->vstack[slot].loc == V_REG) {
                int src = g->vstack[slot].reg;
                int r = alloc_reg(g);
                /* alloc_reg may have spilled `slot` to memory; check again. */
                if (slot < g->mem_depth) {
                    if (slot >= 1 && slot <= g->n_promo) out(g, "    mv %s, %s\n", CACHE_REGS[r], PROMO_REGS[slot - 1]);
                    else out(g, "    lw %s, %d(s0)\n", CACHE_REGS[r], 4 * slot);
                } else {
                    out(g, "    mv %s, %s\n", CACHE_REGS[r], CACHE_REGS[src]);
                }
                vpush_reg(g, r, k);
            } else {
                int r = alloc_reg(g);
                out(g, "    lw %s, %d(s0)\n", CACHE_REGS[r], 4 * slot);
                vpush_reg(g, r, k);
            }
            break;
        }
        case OP_SET_LOCAL: {
            int slot = ch->code[off + 1];
            if (slot < g->depth) {
                if (!same_storage(g->kinds[slot], top(g, 0))) {
                    error(g, "in native code a variable keeps its type: this one holds a %s here and a %s elsewhere "
                             "(run it with `jlang run` to mix them)", kind_name(top(g, 0)), kind_name(g->kinds[slot]));
                    break;
                }
                g->kinds[slot] = merge(g->kinds[slot], top(g, 0));
            }
            if (slot >= g->mem_depth) spill_to(g, slot + 1);
            bool drop = off + 2 < ch->count && !target[off + 2] && ch->code[off + 2] == OP_POP;
            if (slot >= 1 && slot <= g->n_promo) {
                int sreg = slot - 1;
                materialize_sreg(g, sreg, drop ? g->depth - 1 : g->depth);
                const char *preg = PROMO_REGS[sreg];
                if (drop) {
                    if (g->vstack[g->depth - 1].loc == V_IMM) {
                        int32_t imm = g->vstack[g->depth - 1].imm;
                        pop_kinds(g, 1);
                        out(g, "    li %s, %d\n", preg, (int)imm);
                    } else {
                        int fr;
                        const char *r = vpop_reg(g, preg, &fr);
                        if (strcmp(r, preg) != 0) out(g, "    mv %s, %s\n", preg, r);
                        free_reg(g, fr);
                    }
                    off += 1; /* consume fused OP_POP */
                } else {
                    const char *r = vpeek_reg(g, "t0");
                    if (strcmp(r, preg) != 0) out(g, "    mv %s, %s\n", preg, r);
                }
            } else if (drop) {
                int fr;
                const char *r = vpop_reg(g, "t0", &fr);
                out(g, "    sw %s, %d(s0)\n", r, 4 * slot);
                free_reg(g, fr);
                off += 1; /* consume fused OP_POP */
            } else {
                const char *r = vpeek_reg(g, "t0");
                out(g, "    sw %s, %d(s0)\n", r, 4 * slot);
            }
            break;
        }
        case OP_GET_GLOBAL: {
            int gi = ch->code[off + 1];
            Global *gl = &g->vm->globals[gi];
            if (gl->value.type == VAL_NATIVE && gl->defined) {
                vpush_imm(g, 0, (Kind){K_NATIVE, gl->value.as.index});
            } else {
                int r = alloc_reg(g);
                out(g, "    la %s, jl_g_%d\n    lw %s, 0(%s)\n", CACHE_REGS[r], gi, CACHE_REGS[r], CACHE_REGS[r]);
                if (g->gbind[gi] >= 0) vpush_reg(g, r, (Kind){K_FN, (uint16_t)g->gbind[gi]});
                else vpush_reg(g, r, g->gkind_set[gi] ? read_kind(g->gkind[gi], g->reassigned[gi]) : kind_of(K_INT));
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
                error(g, "in native code a variable keeps its type: this one holds a %s here and a %s elsewhere "
                         "(run it with `jlang run` to mix them)", kind_name(k), kind_name(g->gkind[gi]));
                break;
            }
            g->gkind[gi] = g->gkind_set[gi] ? merge(g->gkind[gi], k) : k;
            g->gkind_set[gi] = true;
            if (op == OP_DEF_GLOBAL && k.kind == K_FN && !g->reassigned[gi]) g->gbind[gi] = k.index;
            bool drop = (op == OP_DEF_GLOBAL) || (off + 2 < ch->count && !target[off + 2] && ch->code[off + 2] == OP_POP);
            if (drop) {
                int fr;
                const char *r = vpop_reg(g, "t0", &fr);
                out(g, "    la t1, jl_g_%d\n    sw %s, 0(t1)\n", gi, r);
                free_reg(g, fr);
                if (op == OP_SET_GLOBAL) off += 1; /* consume fused OP_POP */
            } else {
                const char *r = vpeek_reg(g, "t0");
                out(g, "    la t1, jl_g_%d\n    sw %s, 0(t1)\n", gi, r);
            }
            break;
        }
        case OP_ADD:
        case OP_ADD_TEMP:
            if (top(g, 0).kind == K_STR || top(g, 1).kind == K_STR) {
                error(g, "joining strings with + isn't available in native code yet (native programs have no "
                         "heap); run it with `jlang run` instead");
                break;
            }
            if (!numbers_only(g, "+", 2)) break;
            gen_binop(g, op, "add", "addi");
            break;
        case OP_SUB:
            if (!numbers_only(g, "-", 2)) break;
            gen_binop(g, op, "sub", "addi");
            break;
        case OP_MUL:
            if (!numbers_only(g, "*", 2)) break;
            gen_binop(g, op, "mul", NULL);
            break;
        case OP_DIV:
        case OP_MOD: {
            if (!numbers_only(g, op == OP_DIV ? "/" : "%", 2)) break;
            VSlot *va = &g->vstack[g->depth - 2];
            VSlot *vb = &g->vstack[g->depth - 1];
            if (va->loc == V_IMM && vb->loc == V_IMM && vb->imm != 0) {
                int32_t res;
                if (va->imm == INT32_MIN && vb->imm == -1) res = op == OP_DIV ? INT32_MIN : 0;
                else res = op == OP_DIV ? va->imm / vb->imm : va->imm % vb->imm;
                pop_kinds(g, 2);
                vpush_imm(g, res, kind_of(K_INT));
                break;
            }
            bool known_nz = vb->loc == V_IMM && vb->imm != 0;
            const char *ra, *rb;
            int fa, fb;
            vpop_two(g, &ra, &rb, &fa, &fb);
            if (!known_nz) {
                char cond[32];
                snprintf(cond, sizeof cond, "bnez %s", rb);
                trap_unless(g, cond, op == OP_DIV ? "division by zero" : "modulo by zero");
            }
            int dst = reuse_or_alloc(g, fa, fb);
            out(g, "    %s %s, %s, %s\n", op == OP_DIV ? "div" : "rem", CACHE_REGS[dst], ra, rb);
            vpush_reg(g, dst, kind_of(K_INT));
            break;
        }
        case OP_BAND:
            if (!numbers_only(g, "&", 2)) break;
            gen_binop(g, op, "and", "andi");
            break;
        case OP_BOR:
            if (!numbers_only(g, "|", 2)) break;
            gen_binop(g, op, "or", "ori");
            break;
        case OP_BXOR:
            if (!numbers_only(g, "^", 2)) break;
            gen_binop(g, op, "xor", "xori");
            break;
        case OP_SHL:
        case OP_SHR: {
            if (!numbers_only(g, op == OP_SHL ? "<<" : ">>", 2)) break;
            VSlot *va = &g->vstack[g->depth - 2];
            VSlot *vb = &g->vstack[g->depth - 1];
            if (vb->loc == V_IMM && vb->imm >= 0 && vb->imm < 32) {
                int32_t sh = vb->imm;
                if (va->loc == V_IMM) {
                    int32_t res = op == OP_SHL ? (int32_t)((uint32_t)va->imm << sh)
                                               : (sh == 0 ? va->imm : (va->imm >= 0 ? (int32_t)((uint32_t)va->imm >> sh)
                                                                                    : (int32_t)(((uint32_t)va->imm >> sh) | (~0u << (32 - sh)))));
                    pop_kinds(g, 2);
                    vpush_imm(g, res, kind_of(K_INT));
                    break;
                }
                pop_kinds(g, 1);
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                if (sh == 0) {
                    if (strcmp(CACHE_REGS[dst], ra) != 0) out(g, "    mv %s, %s\n", CACHE_REGS[dst], ra);
                } else {
                    out(g, "    %s %s, %s, %d\n", op == OP_SHL ? "slli" : "srai", CACHE_REGS[dst], ra, (int)sh);
                }
                vpush_reg(g, dst, kind_of(K_INT));
                break;
            }
            const char *ra, *rb;
            int fa, fb;
            vpop_two(g, &ra, &rb, &fa, &fb);
            char cond[48];
            snprintf(cond, sizeof cond, "sltiu t2, %s, 32\n    bnez t2", rb);
            trap_num_unless(g, cond, "shift amount ", rb, " is out of range (it must be 0..31)");
            int dst = reuse_or_alloc(g, fa, fb);
            out(g, "    %s %s, %s, %s\n", op == OP_SHL ? "sll" : "sra", CACHE_REGS[dst], ra, rb);
            vpush_reg(g, dst, kind_of(K_INT));
            break;
        }
        case OP_NEG:
            if (!numbers_only(g, "-", 1)) break;
            if (g->vstack[g->depth - 1].loc == V_IMM) {
                g->vstack[g->depth - 1].imm = (int32_t)(0u - (uint32_t)g->vstack[g->depth - 1].imm);
                g->kinds[g->depth - 1] = kind_of(K_INT);
                break;
            }
            {
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                out(g, "    neg %s, %s\n", CACHE_REGS[dst], ra);
                vpush_reg(g, dst, kind_of(K_INT));
            }
            break;
        case OP_BNOT:
            if (!numbers_only(g, "~", 1)) break;
            if (g->vstack[g->depth - 1].loc == V_IMM) {
                g->vstack[g->depth - 1].imm = ~g->vstack[g->depth - 1].imm;
                g->kinds[g->depth - 1] = kind_of(K_INT);
                break;
            }
            {
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                out(g, "    not %s, %s\n", CACHE_REGS[dst], ra);
                vpush_reg(g, dst, kind_of(K_INT));
            }
            break;
        case OP_NOT:
        case OP_BOOL:
            if (g->vstack[g->depth - 1].loc == V_IMM) {
                int32_t v = g->vstack[g->depth - 1].imm;
                g->vstack[g->depth - 1].imm = op == OP_NOT ? (v == 0) : (v != 0);
                g->kinds[g->depth - 1] = kind_of(K_BOOL);
                break;
            }
            {
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                out(g, "    %s %s, %s\n", op == OP_NOT ? "seqz" : "snez", CACHE_REGS[dst], ra);
                vpush_reg(g, dst, kind_of(K_BOOL));
            }
            break;
        case OP_EQ:
        case OP_NE:
        case OP_LT:
        case OP_GT:
        case OP_LE:
        case OP_GE: {
            bool is_eq = (op == OP_EQ || op == OP_NE);
            if (!is_eq) {
                const char *sym = op == OP_LT ? "<" : op == OP_GT ? ">" : op == OP_LE ? "<=" : ">=";
                if (!numbers_only(g, sym, 2)) break;
            }
            bool fused = off + 1 < ch->count && !target[off + 1] && ch->code[off + 1] == OP_JUMP_IF_FALSE;
            int jt = fused ? jump_target(ch, off + 1) : 0;

            /* Different known types are never equal. */
            if (is_eq) {
                Kind ka = top(g, 1), kb = top(g, 0);
                bool known_a = not_a_number(ka) || (ka.kind == K_INT && ka.index == CERTAIN);
                bool known_b = not_a_number(kb) || (kb.kind == K_INT && kb.index == CERTAIN);
                if (known_a && known_b && ka.kind != kb.kind) {
                    int val = (op == OP_NE);
                    if (fused) {
                        spill_to(g, g->depth - 2);
                        const char *ra, *rb;
                        int fa, fb;
                        vpop_two(g, &ra, &rb, &fa, &fb);
                        free_reg(g, fa);
                        free_reg(g, fb);
                        if (!val) {
                            out(g, "    j .L%d_%d\n", fi, jt);
                            reachable = false;
                        }
                        record_target(g, tdepth, tkind, off + 1, jt);
                        off += 3;
                    } else {
                        const char *ra, *rb;
                        int fa, fb;
                        vpop_two(g, &ra, &rb, &fa, &fb);
                        free_reg(g, fa);
                        free_reg(g, fb);
                        vpush_imm(g, val, kind_of(K_BOOL));
                    }
                    break;
                }
            }

            /* Both immediate constants: fold at compile time! */
            if (g->vstack[g->depth - 2].loc == V_IMM && g->vstack[g->depth - 1].loc == V_IMM) {
                int32_t a = g->vstack[g->depth - 2].imm, b = g->vstack[g->depth - 1].imm;
                int val = op == OP_EQ ? (a == b) : op == OP_NE ? (a != b)
                        : op == OP_LT ? (a < b)  : op == OP_GT ? (a > b)
                        : op == OP_LE ? (a <= b) : (a >= b);
                pop_kinds(g, 2);
                if (fused) {
                    spill_all(g);
                    if (!val) {
                        out(g, "    j .L%d_%d\n", fi, jt);
                        reachable = false;
                    }
                    record_target(g, tdepth, tkind, off + 1, jt);
                    off += 3;
                } else {
                    vpush_imm(g, val, kind_of(K_BOOL));
                }
                break;
            }

            if (fused) {
                /* Spill slots below the two operands first so s1 is clean at branch target. */
                spill_to(g, g->depth - 2);
                const char *ra, *rb;
                int fa, fb;
                vpop_two(g, &ra, &rb, &fa, &fb);
                /* Branch to jt when comparison is FALSE. */
                if (op == OP_EQ) emit_cond_branch(g, "bne", "beq", ra, rb, fi, jt, off);
                else if (op == OP_NE) emit_cond_branch(g, "beq", "bne", ra, rb, fi, jt, off);
                else if (op == OP_LT) emit_cond_branch(g, "bge", "blt", ra, rb, fi, jt, off);
                else if (op == OP_GT) emit_cond_branch(g, "bge", "blt", rb, ra, fi, jt, off);
                else if (op == OP_LE) emit_cond_branch(g, "blt", "bge", rb, ra, fi, jt, off);
                else emit_cond_branch(g, "blt", "bge", ra, rb, fi, jt, off);
                free_reg(g, fa);
                free_reg(g, fb);
                record_target(g, tdepth, tkind, off + 1, jt);
                off += 3;
                break;
            }

            const char *ra, *rb;
            int fa, fb;
            vpop_two(g, &ra, &rb, &fa, &fb);
            int dst = reuse_or_alloc(g, fa, fb);
            const char *rd = CACHE_REGS[dst];
            if (op == OP_EQ || op == OP_NE) {
                out(g, "    xor %s, %s, %s\n    %s %s, %s\n", rd, ra, rb, op == OP_EQ ? "seqz" : "snez", rd, rd);
            } else if (op == OP_LT) {
                out(g, "    slt %s, %s, %s\n", rd, ra, rb);
            } else if (op == OP_GT) {
                out(g, "    slt %s, %s, %s\n", rd, rb, ra);
            } else if (op == OP_LE) {
                out(g, "    slt %s, %s, %s\n    xori %s, %s, 1\n", rd, rb, ra, rd, rd);
            } else {
                out(g, "    slt %s, %s, %s\n    xori %s, %s, 1\n", rd, ra, rb, rd, rd);
            }
            vpush_reg(g, dst, kind_of(K_BOOL));
            break;
        }
        case OP_JUMP:
        case OP_LOOP:
        case OP_JUMP_IF_FALSE: {
            int t = jump_target(ch, off);
            if (op == OP_JUMP_IF_FALSE) {
                if (g->vstack[g->depth - 1].loc == V_IMM) {
                    int32_t cond = g->vstack[g->depth - 1].imm;
                    pop_kinds(g, 1);
                    if (!cond) {
                        spill_all(g);
                        out(g, "    j .L%d_%d\n", fi, t);
                        reachable = false;
                    }
                    record_target(g, tdepth, tkind, off, t);
                    break;
                }
                spill_to(g, g->depth - 1);
                int fr;
                const char *r = vpop_reg(g, "t0", &fr);
                emit_cond_branch(g, "beq", "bne", r, "zero", fi, t, off);
                free_reg(g, fr);
            } else {
                spill_all(g);
                for (int b = 0; b < ch->bound_count; b++)
                    if (ch->bounds[b].off == off) out(g, "    # @bound %d\n", ch->bounds[b].count);
                out(g, "    j .L%d_%d\n", fi, t);
                reachable = false;
            }
            record_target(g, tdepth, tkind, off, t);
            break;
        }
        case OP_CALL: {
            int argc = ch->code[off + 1];
            Kind callee = top(g, argc);
            /* Static .bss array(n) / array(n, init) at top level outside loops. */
            if (callee.kind == K_NATIVE && strcmp(g->vm->natives[callee.index].name, "array") == 0) {
                if (argc < 1 || argc > 2) {
                    error(g, "array() takes 1 or 2 arguments (size, and optional initial value)");
                    break;
                }
                if (fi != g->first_fn || in_loop[off]) {
                    error(g, "arrays aren't available inside functions or loops in native code yet (native programs have no heap); run it with `jlang run` instead");
                    break;
                }
                VSlot nslot = g->vstack[g->depth - argc];
                if (nslot.loc != V_IMM || nslot.imm < 0 || nslot.imm > 4096) {
                    error(g, "in native code, array(n) requires a constant size 0..4096 at top level (run it with `jlang run` for dynamic sizes)");
                    break;
                }
                int32_t count = nslot.imm;
                Kind fill_k = argc == 2 ? top(g, 0) : kind_of(K_INT);
                if (argc == 2 && (fill_k.kind == K_STR || fill_k.kind == K_ARR || fill_k.kind == K_STRUCT)) {
                    error(g, "in native code, array(n, value) only supports integer or bool initial values");
                    break;
                }
                int id = add_static(g, (uint32_t)count + 1u);
                int ffill = -1;
                const char *rfill = "zero";
                bool has_nonzero_fill = false;
                if (argc == 2) {
                    if (g->vstack[g->depth - 1].loc == V_IMM && g->vstack[g->depth - 1].imm == 0) {
                        pop_kinds(g, 1);
                    } else {
                        rfill = vpop_reg(g, "t1", &ffill);
                        has_nonzero_fill = true;
                    }
                }
                pop_kinds(g, 2); /* count + callee */
                int dst = reuse_or_alloc(g, -1, -1);
                out(g, "    la %s, jl_static_%d\n    li t0, %d\n    sw t0, 0(%s)\n",
                    CACHE_REGS[dst], id, (int)count, CACHE_REGS[dst]);
                if (has_nonzero_fill && count > 0) {
                    int lp = new_label(g);
                    out(g, "    addi t2, %s, 4\n    li t0, %d\n.Lok%d:\n    sw %s, 0(t2)\n    addi t2, t2, 4\n    addi t0, t0, -1\n    bnez t0, .Lok%d\n",
                        CACHE_REGS[dst], (int)count, lp, rfill, lp);
                }
                free_reg(g, ffill);
                vpush_reg(g, dst, kind_of(K_ARR));
                break;
            }
            spill_all(g);
            if (callee.kind != K_NATIVE) {
                bool ok = true;
                for (int a = 0; a < argc && ok; a++) {
                    Kind k = top(g, argc - 1 - a);
                    if (k.kind == K_STR || k.kind == K_BOOL || k.kind == K_NIL || k.kind == K_ARR || k.kind == K_STRUCT) {
                        error(g, "in native code, functions can only be given numbers and functions, but argument %d "
                                 "is a %s (run it with `jlang run` to pass any value)",
                              a + 1, kind_name(k));
                        ok = false;
                    }
                }
                if (!ok) break;
            }
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
                vpush_mem(g, kind_of(g->returns_nil[callee.index] ? K_NIL : g->returns_bool[callee.index] ? K_BOOL : K_INT));
                sync_promo_top(g);
            } else if (callee.kind == K_STR || callee.kind == K_BOOL || callee.kind == K_NIL ||
                       callee.kind == K_ARR || callee.kind == K_STRUCT) {
                error(g, "this value can't be called (only functions can be called)");
            } else {
                out(g, "    lw t2, %d(s1)\n", -4 * (argc + 1));
                trap_unless(g, "bnez t2", "can't call this value (only functions can be called)");
                out(g, "    lw t3, -4(t2)\n    li t4, %d\n", argc);
                trap_unless(g, "beq t3, t4", "wrong number of arguments for this function");
                out(g, "    jalr ra, 0(t2)\n");
                pop_kinds(g, argc + 1);
                vpush_mem(g, kind_of(K_INT));
                sync_promo_top(g);
            }
            break;
        }
        case OP_RETURN: {
            if (fi != g->first_fn && !g->returns_nil[fi] &&
                (top(g, 0).kind == K_STR || top(g, 0).kind == K_ARR || top(g, 0).kind == K_STRUCT ||
                 (top(g, 0).kind == K_BOOL && !g->returns_bool[fi]))) {
                error(g, "in native code, functions can only return numbers, but this returns a %s "
                         "(run it with `jlang run` to return any value)",
                      kind_name(top(g, 0)));
                break;
            }
            int fr;
            const char *r = vpop_reg(g, "t0", &fr);
            out(g, "    sw %s, 0(s0)\n    addi s1, s0, 4\n", r);
            free_reg(g, fr);
            out(g, "    la t1, jl_depth\n    lw t2, 0(t1)\n    addi t2, t2, -1\n    sw t2, 0(t1)\n");
            for (int p = 0; p < g->n_promo; p++) {
                out(g, "    lw %s, %d(sp)\n", PROMO_REGS[p], frame_bytes - 12 - 4 * p);
            }
            out(g, "    lw ra, %d(sp)\n    lw s0, %d(sp)\n    addi sp, sp, %d\n    ret\n",
                frame_bytes - 4, frame_bytes - 8, frame_bytes);
            g->depth = 0;
            g->mem_depth = 0;
            g->reg_used = 0;
            reachable = false;
            break;
        }
        case OP_INDEX_GET:
            if (top(g, 1).kind == K_STR) {
                error(g, "indexing strings isn't available in native code yet");
                break;
            }
            if (top(g, 1).kind == K_ARR) {
                Kind arr_k = top(g, 1);
                const char *ra, *rb;
                int fa, fb;
                vpop_two(g, &ra, &rb, &fa, &fb);
                out(g, "    lw t2, 0(%s)\n", ra);
                int ok = new_label(g);
                out(g, "    bltu %s, t2, .Lok%d\n    li a0, %d\n    mv a1, %s\n    mv a2, t2\n    j jl_rt_trap_bounds\n.Lok%d:\n",
                    rb, ok, g->line, rb, ok);
                int dst = reuse_or_alloc(g, fa, fb);
                out(g, "    slli t2, %s, 2\n    add t2, %s, t2\n    lw %s, 4(t2)\n", rb, ra, CACHE_REGS[dst]);
                vpush_reg(g, dst, arr_k.index == 1 ? kind_of(K_ARR) : kind_of(K_INT));
                break;
            }
            if (g->vstack[g->depth - 1].loc == V_IMM && g->vstack[g->depth - 1].imm >= 0 && g->vstack[g->depth - 1].imm < 32) {
                int32_t bit = g->vstack[g->depth - 1].imm;
                pop_kinds(g, 1);
                int fa;
                const char *ra = vpop_reg(g, "t0", &fa);
                int dst = reuse_or_alloc(g, fa, -1);
                if (bit == 0) out(g, "    andi %s, %s, 1\n", CACHE_REGS[dst], ra);
                else out(g, "    srli %s, %s, %d\n    andi %s, %s, 1\n", CACHE_REGS[dst], ra, (int)bit, CACHE_REGS[dst], CACHE_REGS[dst]);
                vpush_reg(g, dst, kind_of(K_INT));
                break;
            }
            {
                const char *ra, *rb;
                int fa, fb;
                vpop_two(g, &ra, &rb, &fa, &fb);
                char cond[48];
                snprintf(cond, sizeof cond, "sltiu t2, %s, 32\n    bnez t2", rb);
                trap_num_unless(g, cond, "bit ", rb, " doesn't exist: an int has bits 0..31");
                int dst = reuse_or_alloc(g, fa, fb);
                out(g, "    srl %s, %s, %s\n    andi %s, %s, 1\n", CACHE_REGS[dst], ra, rb, CACHE_REGS[dst], CACHE_REGS[dst]);
                vpush_reg(g, dst, kind_of(K_INT));
            }
            break;
        case OP_INDEX_UPDATE:
            if (top(g, 2).kind == K_STR) {
                error(g, "strings are read-only");
                break;
            }
            if (top(g, 2).kind == K_ARR) {
                Kind arr_k = top(g, 2);
                spill_all(g);
                int ok = new_label(g);
                out(g, "    lw t0, -12(s1)\n    lw t1, -8(s1)\n    lw t3, -4(s1)\n    lw t2, 0(t0)\n");
                out(g, "    bltu t1, t2, .Lok%d\n    li a0, %d\n    mv a1, t1\n    mv a2, t2\n    j jl_rt_trap_bounds\n.Lok%d:\n",
                    ok, g->line, ok);
                out(g, "    slli t2, t1, 2\n    add t2, t0, t2\n    sw t3, 4(t2)\n    addi s1, s1, -8\n");
                pop_kinds(g, 3);
                vpush_mem(g, arr_k);
                sync_promo_top(g);
                break;
            }
            spill_all(g);
            out(g, "    lw t0, -12(s1)\n    lw t1, -8(s1)\n    lw t3, -4(s1)\n");
            trap_num_unless(g, "sltiu t2, t1, 32\n    bnez t2", "bit ", "t1", " doesn't exist: an int has bits 0..31");
            trap_unless(g, "sltiu t2, t3, 2\n    bnez t2", "a bit can only be set to 0 or 1");
            out(g, "    li t4, 1\n    sll t4, t4, t1\n    not t4, t4\n    and t0, t0, t4\n    sll t3, t3, t1\n    or t0, t0, t3\n");
            out(g, "    sw t0, -12(s1)\n    addi s1, s1, -8\n");
            pop_kinds(g, 3);
            vpush_mem(g, kind_of(K_INT));
            sync_promo_top(g);
            break;
        case OP_INDEX_SET:
            if (top(g, 2).kind == K_ARR) {
                Kind val_k = top(g, 0);
                spill_all(g);
                int ok = new_label(g);
                out(g, "    lw t0, -12(s1)\n    lw t1, -8(s1)\n    lw t3, -4(s1)\n    lw t2, 0(t0)\n");
                out(g, "    bltu t1, t2, .Lok%d\n    li a0, %d\n    mv a1, t1\n    mv a2, t2\n    j jl_rt_trap_bounds\n.Lok%d:\n",
                    ok, g->line, ok);
                out(g, "    slli t2, t1, 2\n    add t2, t0, t2\n    sw t3, 4(t2)\n    sw t3, -12(s1)\n    addi s1, s1, -8\n");
                pop_kinds(g, 3);
                vpush_mem(g, val_k);
                sync_promo_top(g);
                break;
            }
            error(g, "can't change a bit of a temporary value; store it in a variable first");
            break;
        case OP_SLICE_GET:
            spill_all(g);
            out(g, "    lw a0, -12(s1)\n    lw a1, -8(s1)\n    lw a2, -4(s1)\n    li a3, %d\n    call jl_rt_slice_get\n", g->line);
            out(g, "    sw a0, -12(s1)\n    addi s1, s1, -8\n");
            pop_kinds(g, 3);
            vpush_mem(g, kind_of(K_INT));
            sync_promo_top(g);
            break;
        case OP_SLICE_UPDATE:
            spill_all(g);
            out(g, "    lw a0, -16(s1)\n    lw a1, -12(s1)\n    lw a2, -8(s1)\n    lw a3, -4(s1)\n    li a4, %d\n    call jl_rt_slice_set\n", g->line);
            out(g, "    sw a0, -16(s1)\n    addi s1, s1, -12\n");
            pop_kinds(g, 4);
            vpush_mem(g, kind_of(K_INT));
            sync_promo_top(g);
            break;
        case OP_ON:
            spill_all(g);
            out(g, "    addi s1, s1, -8\n");
            pop_kinds(g, 2);
            break;
        case OP_STRUCT: {
            if (fi != g->first_fn || in_loop[off]) {
                error(g, "structs aren't available inside functions or loops in native code yet (native programs have no heap); "
                         "run it with `jlang run` instead");
                break;
            }
            uint16_t type = u16_at(ch->code, off + 1);
            const StructDef *d = &g->vm->prog.structs[type];
            int id = add_static(g, (uint32_t)d->field_count);
            int dst = alloc_reg(g);
            out(g, "    la %s, jl_static_%d\n", CACHE_REGS[dst], id);
            vpush_reg(g, dst, (Kind){K_STRUCT, type});
            break;
        }
        case OP_INIT_FIELD: {
            uint8_t slot = ch->code[off + 1];
            int fv;
            const char *rv = vpop_reg(g, "t0", &fv);
            const char *rb = vpeek_reg(g, "t1");
            out(g, "    sw %s, %d(%s)\n", rv, 4 * (int)slot, rb);
            free_reg(g, fv);
            break;
        }
        case OP_GET_FIELD: {
            uint16_t name_si = u16_at(ch->code, off + 1);
            Kind sk = top(g, 0);
            if (sk.kind != K_STRUCT) {
                error(g, "can't read field '%s' of a %s", g->vm->prog.strs[name_si].chars, kind_name(sk));
                break;
            }
            const StructDef *d = &g->vm->prog.structs[sk.index];
            int slot = -1;
            for (int f = 0; f < d->field_count; f++) {
                if (d->fields[f] == name_si) {
                    slot = f;
                    break;
                }
            }
            if (slot < 0) {
                error(g, "%s has no field '%s'", g->vm->prog.strs[d->name].chars, g->vm->prog.strs[name_si].chars);
                break;
            }
            int fb;
            const char *rb = vpop_reg(g, "t0", &fb);
            int dst = reuse_or_alloc(g, fb, -1);
            out(g, "    lw %s, %d(%s)\n", CACHE_REGS[dst], 4 * slot, rb);
            vpush_reg(g, dst, kind_of(K_INT));
            break;
        }
        case OP_SET_FIELD: {
            uint16_t name_si = u16_at(ch->code, off + 1);
            Kind sk = top(g, 1);
            if (sk.kind != K_STRUCT) {
                error(g, "can't set field '%s' of a %s", g->vm->prog.strs[name_si].chars, kind_name(sk));
                break;
            }
            const StructDef *d = &g->vm->prog.structs[sk.index];
            int slot = -1;
            for (int f = 0; f < d->field_count; f++) {
                if (d->fields[f] == name_si) {
                    slot = f;
                    break;
                }
            }
            if (slot < 0) {
                error(g, "%s has no field '%s'", g->vm->prog.strs[d->name].chars, g->vm->prog.strs[name_si].chars);
                break;
            }
            Kind vk = top(g, 0);
            const char *rb, *rv;
            int fb, fv;
            vpop_two(g, &rb, &rv, &fb, &fv);
            out(g, "    sw %s, %d(%s)\n", rv, 4 * slot, rb);
            if (fv >= 0) {
                free_reg(g, fb);
                vpush_reg(g, fv, vk);
            } else {
                int dst = reuse_or_alloc(g, fb, -1);
                if (strcmp(CACHE_REGS[dst], rv) != 0) out(g, "    mv %s, %s\n", CACHE_REGS[dst], rv);
                vpush_reg(g, dst, vk);
            }
            break;
        }
        case OP_TIMER:
            error(g, "every and after blocks aren't available in native code yet; "
                     "run it with `jlang run` instead");
            break;
        case OP_ARRAY: {
            if (fi != g->first_fn || in_loop[off]) {
                error(g, "arrays aren't available inside functions or loops in native code yet (native programs have no heap); "
                         "run it with `jlang run` instead");
                break;
            }
            uint16_t count = u16_at(ch->code, off + 1);
            bool all_arr = count > 0;
            for (int i = 0; i < (int)count; i++) {
                Kind ek = top(g, (int)count - 1 - i);
                if (ek.kind != K_ARR) all_arr = false;
                if (ek.kind == K_STR || ek.kind == K_STRUCT) {
                    error(g, "in native code, static arrays only hold ints, bools or nested arrays");
                    break;
                }
            }
            if (g->failed) break;
            spill_all(g);
            int id = add_static(g, (uint32_t)count + 1u);
            out(g, "    la t0, jl_static_%d\n    li t1, %u\n    sw t1, 0(t0)\n", id, (unsigned)count);
            for (int i = 0; i < (int)count; i++) {
                out(g, "    lw t1, %d(s1)\n    sw t1, %d(t0)\n", -4 * ((int)count - i), 4 * (i + 1));
            }
            if (count > 0) out(g, "    addi s1, s1, %d\n", -4 * (int)count);
            pop_kinds(g, (int)count);
            int dst = alloc_reg(g);
            out(g, "    mv %s, t0\n", CACHE_REGS[dst]);
            vpush_reg(g, dst, (Kind){K_ARR, all_arr ? 1 : 0});
            break;
        }
        default:
            error(g, "instruction %u isn't supported in native code", op);
        }
        off += op_size(op);
    }
    free(target);
    free(in_loop);
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

static const char *RUNTIME3 =
    "\n# jl_rt_fix_mul(a0=a, a1=b, a2=frac 0..31) -> a0\n"
    "jl_rt_fix_mul:\n"
    "    mul t0, a0, a1\n    mulh t1, a0, a1\n"
    "    beqz a2, jl_rt_fm_sat\n"
    "    li t3, 32\n    sub t3, t3, a2\n"
    "    srl t0, t0, a2\n    sll t2, t1, t3\n    or t0, t0, t2\n    sra t1, t1, a2\n"
    "jl_rt_fm_sat:\n"
    "    srai t2, t0, 31\n    beq t1, t2, jl_rt_fm_ok\n"
    "    li a0, 2147483647\n    bgez t1, jl_rt_fm_ret\n    li a0, -2147483648\n"
    "jl_rt_fm_ret:\n    ret\n"
    "jl_rt_fm_ok:\n    mv a0, t0\n    ret\n"
    "\n# jl_rt_fix_div(a0=a, a1=b!=0, a2=frac 0..31) -> a0\n"
    "jl_rt_fix_div:\n"
    "    xor t6, a0, a1\n"
    "    bgez a0, jl_rt_fd_pa\n    neg a0, a0\n"
    "jl_rt_fd_pa:\n    bgez a1, jl_rt_fd_pb\n    neg a1, a1\n"
    "jl_rt_fd_pb:\n    li t1, 0\n    mv t0, a0\n    beqz a2, jl_rt_fd_go\n"
    "    li t2, 32\n    sub t2, t2, a2\n    srl t1, a0, t2\n    sll t0, a0, a2\n"
    "jl_rt_fd_go:\n"
    "    li t2, 0\n    li a5, 0\n    li t3, 0\n    li t4, 0\n    li t5, 64\n"
    "jl_rt_fd_loop:\n"
    "    srli a3, t1, 31\n    srli a4, t0, 31\n    slli t1, t1, 1\n    or t1, t1, a4\n    slli t0, t0, 1\n"
    "    srli a5, t2, 31\n    slli t2, t2, 1\n    or t2, t2, a3\n"
    "    srli a4, t3, 31\n    slli t4, t4, 1\n    or t4, t4, a4\n    slli t3, t3, 1\n"
    "    bnez a5, jl_rt_fd_sub\n    bltu t2, a1, jl_rt_fd_next\n"
    "jl_rt_fd_sub:\n    sub t2, t2, a1\n    ori t3, t3, 1\n"
    "jl_rt_fd_next:\n    addi t5, t5, -1\n    bnez t5, jl_rt_fd_loop\n"
    "    bltz t6, jl_rt_fd_neg\n"
    "    bnez t4, jl_rt_fd_psat\n    li t0, 2147483647\n    bltu t0, t3, jl_rt_fd_psat\n"
    "    mv a0, t3\n    ret\n"
    "jl_rt_fd_psat:\n    li a0, 2147483647\n    ret\n"
    "jl_rt_fd_neg:\n"
    "    bnez t4, jl_rt_fd_nsat\n    li t0, -2147483648\n    bgeu t3, t0, jl_rt_fd_nsat\n"
    "    neg a0, t3\n    ret\n"
    "jl_rt_fd_nsat:\n    li a0, -2147483648\n    ret\n"
    "\n# jl_rt_trap_bounds(a0 = line, a1 = idx, a2 = len)\n"
    "jl_rt_trap_bounds:\n"
    "    mv s2, a0\n    mv s3, a1\n    mv s4, a2\n"
    "    la a0, jl_rt_msg_prefix\n    call jl_rt_puts\n"
    "    mv a0, s2\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_colon\n    call jl_rt_puts\n"
    "    la a0, jl_rt_msg_idx1\n    call jl_rt_puts\n"
    "    mv a0, s3\n    call jl_rt_putint\n"
    "    bnez s4, jl_rt_tb_nonempty\n"
    "    la a0, jl_rt_msg_idx_empty\n    call jl_rt_puts\n"
    "    j jl_rt_tb_done\n"
    "jl_rt_tb_nonempty:\n"
    "    la a0, jl_rt_msg_idx2\n    call jl_rt_puts\n"
    "    mv a0, s4\n    call jl_rt_putint\n"
    "    la a0, jl_rt_msg_idx3\n"
    "    li t0, 1\n    bne s4, t0, jl_rt_tb_pl\n"
    "    la a0, jl_rt_msg_idx3s\n"
    "jl_rt_tb_pl:\n"
    "    call jl_rt_puts\n"
    "    addi a0, s4, -1\n    call jl_rt_putint\n"
    "jl_rt_tb_done:\n"
    "    li a0, 10\n    call jl_hal_putc\n"
    "    li a0, 70\n    call jl_hal_halt\n"
    "    j jl_rt_hang\n";

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
    "jl_hal_millis:\n    li t0, 0x10002000\n    lw a0, 0(t0)\n    ret\n"
    "jl_hal_led_set:           # a0 = x, a1 = y, a2 = 0xRRGGBB\n"
    "    slli t0, a0, 16\n    slli t1, a1, 16\n    srli t1, t1, 16\n    or t0, t0, t1\n"
    "    li t2, 0x10004000\n    sw t0, 0(t2)\n    sw a2, 4(t2)\n    ret\n"
    "jl_hal_led_show:\n    li t2, 0x10004000\n    sw zero, 8(t2)\n    ret\n"
    "jl_hal_led_clear:\n    li t2, 0x10004000\n    sw zero, 12(t2)\n    ret\n"
    "jl_hal_led_width:\n    li t2, 0x10004000\n    lw a0, 16(t2)\n    ret\n"
    "jl_hal_led_height:\n    li t2, 0x10004000\n    lw a0, 20(t2)\n    ret\n";

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
        if (g->vm->prog.fns[fi].is_test) continue;
        Chunk *ch = &g->vm->prog.fns[fi].chunk;
        uint8_t prev = 0xFF;
        g->returns_nil[fi] = true;
        /* A function returns bools if every return gives true, false or a
           comparison; the caller then knows its result is a bool. */
        bool any = false, all_bool = true;
        for (int off = 0; off < ch->count; off += op_size(ch->code[off])) {
            if (ch->code[off] == OP_RETURN && prev != OP_NIL) {
                g->returns_nil[fi] = false;
                any = true;
                if (!(prev == OP_TRUE || prev == OP_FALSE || prev == OP_EQ || prev == OP_NE || prev == OP_LT ||
                      prev == OP_LE || prev == OP_GT || prev == OP_GE || prev == OP_NOT || prev == OP_BOOL))
                    all_bool = false;
            }
            prev = ch->code[off];
        }
        g->returns_bool[fi] = any && all_bool;
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
    g->returns_bool = calloc((size_t)vm->prog.fn_count + 1, 1);
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
    for (int fi = first + 1; fi < last && !g->failed; fi++) {
        if (vm->prog.fns[fi].is_test) continue;
        gen_function(g, fi);
    }

    out(g, "%s%s%s", RUNTIME, RUNTIME2, RUNTIME3);
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
        emit_string(g, "jl_rt_msg_idx1", "index ", 6);
        emit_string(g, "jl_rt_msg_idx_empty", " is out of bounds: this array is empty", 38);
        emit_string(g, "jl_rt_msg_idx2", " is out of bounds: this array has ", 34);
        emit_string(g, "jl_rt_msg_idx3", " items, so valid indexes are 0..", 32);
        emit_string(g, "jl_rt_msg_idx3s", " item, so valid indexes are 0..", 31);
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
    out(g, "\n    .bss\n    .align 2\n");
    for (int i = 0; i < g->nstatics; i++) {
        out(g, "jl_static_%d:\n    .zero %u\n", i, (unsigned)(g->static_words[i] * 4u));
    }
    out(g, "jl_vstack:\n    .zero 16384\njl_vstack_end:\n");

    char *result = g->failed ? NULL : g->buf;
    if (g->failed) free(g->buf);
    for (int i = 0; i < g->nlits; i++) free(g->lits[i]);
    free(g->lits);
    free(g->lit_lens);
    free(g->static_words);
    free(g->str_used);
    free(g->returns_nil);
    free(g->returns_bool);
    free(g);
    return result;
}
