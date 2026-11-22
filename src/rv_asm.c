/*
 * rv_asm.c - a two-pass assembler for RV32IM (GNU syntax).
 *
 * Pass 1 walks the source to learn how big every line is and where each
 * label lands. Pass 2 walks it again and encodes the machine code, now
 * that every address is known.
 *
 * Supported: all RV32I and M instructions, rdcycle/rdinstret/rdtime and
 * csrr, the usual pseudo-instructions (li, la, mv, not, neg, seqz, snez,
 * j, jr, ret, call, tail, beqz, bnez, bgt, ble, ...), and the directives
 * .text .rodata .data .bss .section .globl .align .balign .word .half
 * .byte .zero .space .string .asciz .ascii .equ.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"

enum { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS, SEC_COUNT, SEC_ABS = -1 };

typedef struct {
    char name[64];
    int sec;
    uint32_t off;
} Label;

typedef struct {
    RvImage *img;
    int pass;
    int sec;
    uint32_t off[SEC_COUNT];
    uint32_t base[SEC_COUNT];
    Label *labels;
    int nlabels;
    int caplabels;
    int asm_line;
    int jl_line;
    bool allow_undefined;
    bool failed;
    /* listing */
    bool want_listing;
    char *lst;
    size_t lst_len;
    size_t lst_cap;
    uint32_t line_words[4];
    int line_nwords;
    /* JLang line map */
    int caplines;
} Asm;

/* ------------------------------------------------------------------ */
/* errors, output                                                      */
/* ------------------------------------------------------------------ */

static void fail(Asm *a, const char *fmt, ...) JL_PRINTF(2, 3);
static void fail(Asm *a, const char *fmt, ...) {
    if (a->failed) return;
    a->failed = true;
    char msg[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (a->jl_line > 0) {
        snprintf(a->img->error, sizeof a->img->error, "error (line %d): in assembly: %s", a->jl_line, msg);
    } else {
        snprintf(a->img->error, sizeof a->img->error, "error: assembly line %d: %s", a->asm_line, msg);
    }
}

static void lst_append(Asm *a, const char *s, size_t n) {
    if (a->lst_len + n + 1 > a->lst_cap) {
        size_t cap = a->lst_cap ? a->lst_cap * 2 : 4096;
        while (cap < a->lst_len + n + 1) cap *= 2;
        char *p = realloc(a->lst, cap);
        if (p == NULL) return;
        a->lst = p;
        a->lst_cap = cap;
    }
    memcpy(a->lst + a->lst_len, s, n);
    a->lst_len += n;
    a->lst[a->lst_len] = '\0';
}

static uint32_t here(const Asm *a) { return a->base[a->sec] + a->off[a->sec]; }

static void emit8(Asm *a, uint8_t b) {
    if (a->pass == 2 && a->sec != SEC_BSS) a->img->image[here(a)] = b;
    if (a->pass == 2 && a->sec == SEC_BSS && b != 0) fail(a, ".bss can only hold zeros");
    a->off[a->sec]++;
}

static void emit32(Asm *a, uint32_t w) {
    if (a->pass == 2 && a->line_nwords < 4) a->line_words[a->line_nwords++] = w;
    for (int i = 0; i < 4; i++) emit8(a, (uint8_t)(w >> (8 * i)));
}

/* ------------------------------------------------------------------ */
/* operands                                                            */
/* ------------------------------------------------------------------ */

static const char *const REG_NAMES[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
    "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
    "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static int parse_reg(Asm *a, const char *s) {
    if (strcmp(s, "fp") == 0) return 8;
    for (int i = 0; i < 32; i++) {
        if (strcmp(s, REG_NAMES[i]) == 0) return i;
    }
    if (s[0] == 'x' && isdigit((unsigned char)s[1])) {
        char *end;
        long n = strtol(s + 1, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32) return (int)n;
    }
    fail(a, "'%s' is not a register", s);
    return 0;
}

static bool parse_number(const char *s, int64_t *out) {
    const char *p = s;
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    if (*p == '\'' && p[1] && p[2] == '\'' && p[3] == '\0') {
        *out = neg ? -(int64_t)(unsigned char)p[1] : (unsigned char)p[1];
        return true;
    }
    if (!isdigit((unsigned char)*p)) return false;
    int64_t v = 0;
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        if (!*p) return false;
        for (; *p; p++) {
            if (*p != '0' && *p != '1') return false;
            v = v * 2 + (*p - '0');
        }
    } else {
        char *end;
        unsigned long long u = strtoull(p, &end, 0);
        if (*end != '\0') return false;
        v = (int64_t)u;
    }
    *out = neg ? -v : v;
    return true;
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$'; }

static bool symbol_value(Asm *a, const char *name, uint32_t *out) {
    for (int i = 0; i < a->nlabels; i++) {
        if (strcmp(a->labels[i].name, name) == 0) {
            const Label *l = &a->labels[i];
            *out = l->sec == SEC_ABS ? l->off : a->base[l->sec] + l->off;
            return true;
        }
    }
    return false;
}

/* number, symbol, or symbol+number / symbol-number */
static int64_t parse_expr(Asm *a, const char *s) {
    int64_t v;
    if (parse_number(s, &v)) return v;
    char name[64];
    size_t n = 0;
    while (s[n] && s[n] != '+' && s[n] != '-' && n < sizeof name - 1) {
        name[n] = s[n];
        n++;
    }
    name[n] = '\0';
    if (n == 0 || !is_ident_start(name[0])) {
        fail(a, "'%s' is not a number or a label", s);
        return 0;
    }
    int64_t add = 0;
    if (s[n]) {
        if (!parse_number(s + n, &add)) {
            fail(a, "bad offset in '%s'", s);
            return 0;
        }
    }
    uint32_t addr = 0;
    if (!symbol_value(a, name, &addr) && a->pass == 2 && !a->allow_undefined) {
        fail(a, "undefined label '%s'", name);
    }
    return (int64_t)addr + add;
}

static int64_t parse_imm(Asm *a, const char *s) {
    int64_t v;
    if (parse_number(s, &v)) return v;
    return parse_expr(a, s);
}

/* "off(reg)" or "(reg)" */
static void parse_mem(Asm *a, const char *s, int32_t *off, int *reg) {
    const char *open = strchr(s, '(');
    const char *close = strrchr(s, ')');
    if (open == NULL || close == NULL || close < open) {
        fail(a, "expected an address like 8(sp), got '%s'", s);
        return;
    }
    char imm[64] = "0";
    size_t n = (size_t)(open - s);
    if (n > 0) {
        if (n >= sizeof imm) n = sizeof imm - 1;
        memcpy(imm, s, n);
        imm[n] = '\0';
    }
    char r[16];
    size_t rn = (size_t)(close - open - 1);
    if (rn >= sizeof r) rn = sizeof r - 1;
    memcpy(r, open + 1, rn);
    r[rn] = '\0';
    *off = (int32_t)parse_imm(a, imm);
    *reg = parse_reg(a, r);
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

static void check_range(Asm *a, int64_t v, int64_t lo, int64_t hi, const char *what) {
    if (v < lo || v > hi) fail(a, "%s %lld is out of range (%lld..%lld)", what, (long long)v, (long long)lo, (long long)hi);
}

static uint32_t enc_r(uint32_t op, uint32_t f3, uint32_t f7, int rd, int rs1, int rs2) {
    return (f7 << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | (f3 << 12) | ((uint32_t)rd << 7) | op;
}
static uint32_t enc_i(uint32_t op, uint32_t f3, int rd, int rs1, int32_t imm) {
    return (((uint32_t)imm & 0xFFFu) << 20) | ((uint32_t)rs1 << 15) | (f3 << 12) | ((uint32_t)rd << 7) | op;
}
static uint32_t enc_s(uint32_t op, uint32_t f3, int rs1, int rs2, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return (((u >> 5) & 0x7Fu) << 25) | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | (f3 << 12) |
           ((u & 0x1Fu) << 7) | op;
}
static uint32_t enc_b(uint32_t f3, int rs1, int rs2, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) | ((uint32_t)rs2 << 20) |
           ((uint32_t)rs1 << 15) | (f3 << 12) | (((u >> 1) & 0xFu) << 8) | (((u >> 11) & 1u) << 7) | 0x63u;
}
static uint32_t enc_u(uint32_t op, int rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFFu) << 12) | ((uint32_t)rd << 7) | op;
}
static uint32_t enc_j(int rd, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) | (((u >> 11) & 1u) << 20) |
           (((u >> 12) & 0xFFu) << 12) | ((uint32_t)rd << 7) | 0x6Fu;
}

/* hi/lo split for auipc/lui + addi/jalr pairs */
static void split_hi_lo(int64_t v, uint32_t *hi, int32_t *lo) {
    uint32_t u = (uint32_t)v;
    *hi = ((u + 0x800u) >> 12) & 0xFFFFFu;
    *lo = (int32_t)(u - (*hi << 12));
}

static void emit_li(Asm *a, int rd, int64_t v) {
    if (v < -2147483648LL || v > 4294967295LL) fail(a, "li value %lld doesn't fit in 32 bits", (long long)v);
    int32_t s = (int32_t)(uint32_t)v;
    if (s >= -2048 && s <= 2047) {
        emit32(a, enc_i(0x13, 0, rd, 0, s));
        return;
    }
    uint32_t hi;
    int32_t lo;
    split_hi_lo(s, &hi, &lo);
    emit32(a, enc_u(0x37, rd, hi));
    if (lo != 0) emit32(a, enc_i(0x13, 0, rd, rd, lo));
}

static int32_t branch_offset(Asm *a, const char *target, int64_t limit) {
    int64_t t = parse_expr(a, target);
    int64_t off = t - (int64_t)here(a);
    if (a->pass == 2) {
        if (off & 1) fail(a, "branch target is not 2-byte aligned");
        check_range(a, off, -limit, limit - 2, "jump distance");
    }
    return (int32_t)off;
}

typedef struct {
    const char *name;
    char kind;
    uint8_t op;
    uint8_t f3;
    uint8_t f7;
} Op;

/* kind: R = rd,rs1,rs2  I = rd,rs1,imm  H = shift-immediate  L = load
 *       S = store  B = branch */
static const Op OPS[] = {
    {"add", 'R', 0x33, 0, 0x00},  {"sub", 'R', 0x33, 0, 0x20},  {"sll", 'R', 0x33, 1, 0x00},
    {"slt", 'R', 0x33, 2, 0x00},  {"sltu", 'R', 0x33, 3, 0x00}, {"xor", 'R', 0x33, 4, 0x00},
    {"srl", 'R', 0x33, 5, 0x00},  {"sra", 'R', 0x33, 5, 0x20},  {"or", 'R', 0x33, 6, 0x00},
    {"and", 'R', 0x33, 7, 0x00},  {"mul", 'R', 0x33, 0, 0x01},  {"mulh", 'R', 0x33, 1, 0x01},
    {"mulhsu", 'R', 0x33, 2, 0x01}, {"mulhu", 'R', 0x33, 3, 0x01}, {"div", 'R', 0x33, 4, 0x01},
    {"divu", 'R', 0x33, 5, 0x01}, {"rem", 'R', 0x33, 6, 0x01},  {"remu", 'R', 0x33, 7, 0x01},
    {"addi", 'I', 0x13, 0, 0},    {"slti", 'I', 0x13, 2, 0},    {"sltiu", 'I', 0x13, 3, 0},
    {"xori", 'I', 0x13, 4, 0},    {"ori", 'I', 0x13, 6, 0},     {"andi", 'I', 0x13, 7, 0},
    {"slli", 'H', 0x13, 1, 0x00}, {"srli", 'H', 0x13, 5, 0x00}, {"srai", 'H', 0x13, 5, 0x20},
    {"lb", 'L', 0x03, 0, 0},      {"lh", 'L', 0x03, 1, 0},      {"lw", 'L', 0x03, 2, 0},
    {"lbu", 'L', 0x03, 4, 0},     {"lhu", 'L', 0x03, 5, 0},     {"sb", 'S', 0x23, 0, 0},
    {"sh", 'S', 0x23, 1, 0},      {"sw", 'S', 0x23, 2, 0},      {"beq", 'B', 0x63, 0, 0},
    {"bne", 'B', 0x63, 1, 0},     {"blt", 'B', 0x63, 4, 0},     {"bge", 'B', 0x63, 5, 0},
    {"bltu", 'B', 0x63, 6, 0},    {"bgeu", 'B', 0x63, 7, 0},
};

static int csr_number(Asm *a, const char *s) {
    if (strcmp(s, "cycle") == 0) return 0xC00;
    if (strcmp(s, "time") == 0) return 0xC01;
    if (strcmp(s, "instret") == 0) return 0xC02;
    if (strcmp(s, "cycleh") == 0) return 0xC80;
    if (strcmp(s, "timeh") == 0) return 0xC81;
    if (strcmp(s, "instreth") == 0) return 0xC82;
    int64_t v;
    if (parse_number(s, &v) && v >= 0 && v < 4096) return (int)v;
    fail(a, "unknown CSR '%s'", s);
    return 0;
}

static void need(Asm *a, int argc, int want, const char *m) {
    if (argc != want) fail(a, "'%s' takes %d operand%s, got %d", m, want, want == 1 ? "" : "s", argc);
}

static void instruction(Asm *a, const char *m, char **ops, int n) {
    if (a->sec != SEC_TEXT) fail(a, "instructions belong in .text");

    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        const Op *o = &OPS[i];
        if (strcmp(m, o->name) != 0) continue;
        switch (o->kind) {
        case 'R':
            need(a, n, 3, m);
            if (a->failed) return;
            emit32(a, enc_r(o->op, o->f3, o->f7, parse_reg(a, ops[0]), parse_reg(a, ops[1]), parse_reg(a, ops[2])));
            return;
        case 'I': {
            need(a, n, 3, m);
            if (a->failed) return;
            int64_t imm = parse_imm(a, ops[2]);
            check_range(a, imm, -2048, 2047, "immediate");
            emit32(a, enc_i(o->op, o->f3, parse_reg(a, ops[0]), parse_reg(a, ops[1]), (int32_t)imm));
            return;
        }
        case 'H': {
            need(a, n, 3, m);
            if (a->failed) return;
            int64_t sh = parse_imm(a, ops[2]);
            check_range(a, sh, 0, 31, "shift amount");
            emit32(a, enc_r(o->op, o->f3, o->f7, parse_reg(a, ops[0]), parse_reg(a, ops[1]), (int)(sh & 31)));
            return;
        }
        case 'L': {
            need(a, n, 2, m);
            if (a->failed) return;
            int32_t off = 0;
            int rs1 = 0;
            parse_mem(a, ops[1], &off, &rs1);
            check_range(a, off, -2048, 2047, "offset");
            emit32(a, enc_i(o->op, o->f3, parse_reg(a, ops[0]), rs1, off));
            return;
        }
        case 'S': {
            need(a, n, 2, m);
            if (a->failed) return;
            int32_t off = 0;
            int rs1 = 0;
            parse_mem(a, ops[1], &off, &rs1);
            check_range(a, off, -2048, 2047, "offset");
            emit32(a, enc_s(o->op, o->f3, rs1, parse_reg(a, ops[0]), off));
            return;
        }
        case 'B': {
            need(a, n, 3, m);
            if (a->failed) return;
            int rs1 = parse_reg(a, ops[0]), rs2 = parse_reg(a, ops[1]);
            emit32(a, enc_b(o->f3, rs1, rs2, branch_offset(a, ops[2], 4096)));
            return;
        }
        }
    }

    /* branch pseudo-instructions */
    static const struct {
        const char *name;
        const char *real;
        int zero_pos; /* -1: two registers, swap if 's'; 0/1: where x0 goes */
        bool swap;
    } BR[] = {
        {"beqz", "beq", 1, false}, {"bnez", "bne", 1, false}, {"blez", "bge", 0, false},
        {"bgez", "bge", 1, false}, {"bltz", "blt", 1, false}, {"bgtz", "blt", 0, false},
        {"bgt", "blt", -1, true},  {"ble", "bge", -1, true},  {"bgtu", "bltu", -1, true},
        {"bleu", "bgeu", -1, true},
    };
    for (size_t i = 0; i < sizeof BR / sizeof BR[0]; i++) {
        if (strcmp(m, BR[i].name) != 0) continue;
        uint32_t f3 = 0;
        for (size_t j = 0; j < sizeof OPS / sizeof OPS[0]; j++) {
            if (strcmp(OPS[j].name, BR[i].real) == 0) f3 = OPS[j].f3;
        }
        int r1, r2;
        const char *target;
        if (BR[i].zero_pos >= 0) {
            need(a, n, 2, m);
            if (a->failed) return;
            int r = parse_reg(a, ops[0]);
            r1 = BR[i].zero_pos == 0 ? 0 : r;
            r2 = BR[i].zero_pos == 0 ? r : 0;
            target = ops[1];
        } else {
            need(a, n, 3, m);
            if (a->failed) return;
            r1 = parse_reg(a, ops[1]);
            r2 = parse_reg(a, ops[0]);
            target = ops[2];
        }
        emit32(a, enc_b(f3, r1, r2, branch_offset(a, target, 4096)));
        return;
    }

    if (strcmp(m, "lui") == 0 || strcmp(m, "auipc") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        int64_t v = parse_imm(a, ops[1]);
        check_range(a, v, 0, 0xFFFFF, "upper immediate");
        emit32(a, enc_u(m[0] == 'l' ? 0x37 : 0x17, parse_reg(a, ops[0]), (uint32_t)v));
    } else if (strcmp(m, "jal") == 0) {
        if (n == 1) emit32(a, enc_j(1, branch_offset(a, ops[0], 1 << 20)));
        else {
            need(a, n, 2, m);
            if (a->failed) return;
            int rd = parse_reg(a, ops[0]);
            emit32(a, enc_j(rd, branch_offset(a, ops[1], 1 << 20)));
        }
    } else if (strcmp(m, "j") == 0) {
        need(a, n, 1, m);
        if (a->failed) return;
        emit32(a, enc_j(0, branch_offset(a, ops[0], 1 << 20)));
    } else if (strcmp(m, "jalr") == 0) {
        int rd = 1, rs1 = 0;
        int32_t off = 0;
        if (n == 1) {
            rs1 = parse_reg(a, ops[0]);
        } else if (n == 2) {
            rd = parse_reg(a, ops[0]);
            if (strchr(ops[1], '(')) parse_mem(a, ops[1], &off, &rs1);
            else rs1 = parse_reg(a, ops[1]);
        } else {
            need(a, n, 3, m);
            if (a->failed) return;
            rd = parse_reg(a, ops[0]);
            rs1 = parse_reg(a, ops[1]);
            off = (int32_t)parse_imm(a, ops[2]);
        }
        check_range(a, off, -2048, 2047, "offset");
        emit32(a, enc_i(0x67, 0, rd, rs1, off));
    } else if (strcmp(m, "jr") == 0) {
        need(a, n, 1, m);
        if (a->failed) return;
        emit32(a, enc_i(0x67, 0, 0, parse_reg(a, ops[0]), 0));
    } else if (strcmp(m, "ret") == 0) {
        need(a, n, 0, m);
        emit32(a, enc_i(0x67, 0, 0, 1, 0));
    } else if (strcmp(m, "call") == 0 || strcmp(m, "tail") == 0) {
        need(a, n, 1, m);
        if (a->failed) return;
        bool call = m[0] == 'c';
        int64_t t = parse_expr(a, ops[0]);
        uint32_t hi;
        int32_t lo;
        split_hi_lo(t - (int64_t)here(a), &hi, &lo);
        int r = call ? 1 : 6;
        emit32(a, enc_u(0x17, r, hi));
        emit32(a, enc_i(0x67, 0, call ? 1 : 0, r, lo));
    } else if (strcmp(m, "la") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        int rd = parse_reg(a, ops[0]);
        int64_t t = parse_expr(a, ops[1]);
        uint32_t hi;
        int32_t lo;
        split_hi_lo(t - (int64_t)here(a), &hi, &lo);
        emit32(a, enc_u(0x17, rd, hi));
        emit32(a, enc_i(0x13, 0, rd, rd, lo));
    } else if (strcmp(m, "li") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        int rd = parse_reg(a, ops[0]);
        int64_t v;
        if (parse_number(ops[1], &v)) {
            emit_li(a, rd, v);
        } else {
            /* li with a symbol: always lui + addi so both passes agree */
            uint32_t hi;
            int32_t lo;
            split_hi_lo(parse_expr(a, ops[1]), &hi, &lo);
            emit32(a, enc_u(0x37, rd, hi));
            emit32(a, enc_i(0x13, 0, rd, rd, lo));
        }
    } else if (strcmp(m, "nop") == 0) {
        emit32(a, enc_i(0x13, 0, 0, 0, 0));
    } else if (strcmp(m, "mv") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_i(0x13, 0, parse_reg(a, ops[0]), parse_reg(a, ops[1]), 0));
    } else if (strcmp(m, "not") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_i(0x13, 4, parse_reg(a, ops[0]), parse_reg(a, ops[1]), -1));
    } else if (strcmp(m, "neg") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_r(0x33, 0, 0x20, parse_reg(a, ops[0]), 0, parse_reg(a, ops[1])));
    } else if (strcmp(m, "seqz") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_i(0x13, 3, parse_reg(a, ops[0]), parse_reg(a, ops[1]), 1));
    } else if (strcmp(m, "snez") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_r(0x33, 3, 0, parse_reg(a, ops[0]), 0, parse_reg(a, ops[1])));
    } else if (strcmp(m, "sltz") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_r(0x33, 2, 0, parse_reg(a, ops[0]), parse_reg(a, ops[1]), 0));
    } else if (strcmp(m, "sgtz") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        emit32(a, enc_r(0x33, 2, 0, parse_reg(a, ops[0]), 0, parse_reg(a, ops[1])));
    } else if (strcmp(m, "ecall") == 0) {
        emit32(a, 0x00000073u);
    } else if (strcmp(m, "ebreak") == 0) {
        emit32(a, 0x00100073u);
    } else if (strcmp(m, "fence") == 0) {
        emit32(a, 0x0FF0000Fu);
    } else if (strcmp(m, "rdcycle") == 0 || strcmp(m, "rdinstret") == 0 || strcmp(m, "rdtime") == 0) {
        need(a, n, 1, m);
        if (a->failed) return;
        int csr = m[2] == 'c' ? 0xC00 : m[2] == 'i' ? 0xC02 : 0xC01;
        emit32(a, ((uint32_t)csr << 20) | (2u << 12) | ((uint32_t)parse_reg(a, ops[0]) << 7) | 0x73u);
    } else if (strcmp(m, "csrr") == 0) {
        need(a, n, 2, m);
        if (a->failed) return;
        int rd = parse_reg(a, ops[0]);
        emit32(a, ((uint32_t)csr_number(a, ops[1]) << 20) | (2u << 12) | ((uint32_t)rd << 7) | 0x73u);
    } else if (strcmp(m, "csrrs") == 0 || strcmp(m, "csrrw") == 0 || strcmp(m, "csrrc") == 0) {
        need(a, n, 3, m);
        if (a->failed) return;
        uint32_t f3 = m[4] == 'w' ? 1 : m[4] == 's' ? 2 : 3;
        int rd = parse_reg(a, ops[0]);
        int csr = csr_number(a, ops[1]);
        int rs1 = parse_reg(a, ops[2]);
        emit32(a, ((uint32_t)csr << 20) | ((uint32_t)rs1 << 15) | (f3 << 12) | ((uint32_t)rd << 7) | 0x73u);
    } else {
        fail(a, "unknown instruction '%s'", m);
    }
}

/* ------------------------------------------------------------------ */
/* directives                                                          */
/* ------------------------------------------------------------------ */

static void align_to(Asm *a, uint32_t n) {
    if (n == 0 || (n & (n - 1)) != 0) {
        fail(a, "alignment must be a power of two");
        return;
    }
    while (a->off[a->sec] % n) emit8(a, 0);
}

static bool set_section(Asm *a, const char *name) {
    if (strncmp(name, ".text", 5) == 0) a->sec = SEC_TEXT;
    else if (strncmp(name, ".rodata", 7) == 0 || strncmp(name, ".srodata", 8) == 0) a->sec = SEC_RODATA;
    else if (strncmp(name, ".data", 5) == 0 || strncmp(name, ".sdata", 6) == 0) a->sec = SEC_DATA;
    else if (strncmp(name, ".bss", 4) == 0 || strncmp(name, ".sbss", 5) == 0) a->sec = SEC_BSS;
    else return false;
    return true;
}

static void string_directive(Asm *a, const char *rest, bool zero) {
    const char *p = strchr(rest, '"');
    if (p == NULL) {
        fail(a, "expected a quoted string");
        return;
    }
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            case 'x': {
                char hex[3] = {p[1], p[2], 0};
                c = (char)strtol(hex, NULL, 16);
                p += 2;
                break;
            }
            default: c = *p; break;
            }
        }
        emit8(a, (uint8_t)c);
    }
    if (zero) emit8(a, 0);
}

static void directive(Asm *a, const char *d, char **ops, int n, const char *rest) {
    if (set_section(a, d)) return;
    if (strcmp(d, ".section") == 0) {
        if (n < 1 || !set_section(a, ops[0])) fail(a, "unknown section");
    } else if (strcmp(d, ".globl") == 0 || strcmp(d, ".global") == 0 || strcmp(d, ".type") == 0 ||
               strcmp(d, ".size") == 0 || strcmp(d, ".option") == 0 || strcmp(d, ".file") == 0 ||
               strcmp(d, ".ident") == 0 || strcmp(d, ".attribute") == 0 || strcmp(d, ".local") == 0) {
        /* no effect on a flat image */
    } else if (strcmp(d, ".align") == 0 || strcmp(d, ".p2align") == 0) {
        if (n >= 1) align_to(a, 1u << (unsigned)parse_imm(a, ops[0]));
    } else if (strcmp(d, ".balign") == 0) {
        if (n >= 1) align_to(a, (uint32_t)parse_imm(a, ops[0]));
    } else if (strcmp(d, ".word") == 0 || strcmp(d, ".4byte") == 0) {
        for (int i = 0; i < n; i++) emit32(a, (uint32_t)parse_expr(a, ops[i]));
    } else if (strcmp(d, ".half") == 0 || strcmp(d, ".2byte") == 0) {
        for (int i = 0; i < n; i++) {
            uint32_t v = (uint32_t)parse_imm(a, ops[i]);
            emit8(a, (uint8_t)v);
            emit8(a, (uint8_t)(v >> 8));
        }
    } else if (strcmp(d, ".byte") == 0) {
        for (int i = 0; i < n; i++) emit8(a, (uint8_t)parse_imm(a, ops[i]));
    } else if (strcmp(d, ".zero") == 0 || strcmp(d, ".space") == 0 || strcmp(d, ".skip") == 0) {
        int64_t count = n >= 1 ? parse_imm(a, ops[0]) : 0;
        check_range(a, count, 0, RV_RAM_SIZE, "size");
        if (a->sec == SEC_BSS) a->off[a->sec] += (uint32_t)count;
        else for (int64_t i = 0; i < count; i++) emit8(a, 0);
    } else if (strcmp(d, ".string") == 0 || strcmp(d, ".asciz") == 0) {
        string_directive(a, rest, true);
    } else if (strcmp(d, ".ascii") == 0) {
        string_directive(a, rest, false);
    } else if (strcmp(d, ".equ") == 0 || strcmp(d, ".set") == 0) {
        if (n != 2) {
            fail(a, ".equ needs a name and a value");
            return;
        }
        if (a->pass == 1) {
            if (a->nlabels == a->caplabels) {
                a->caplabels = a->caplabels ? a->caplabels * 2 : 64;
                a->labels = realloc(a->labels, (size_t)a->caplabels * sizeof(Label));
            }
            Label *l = &a->labels[a->nlabels++];
            snprintf(l->name, sizeof l->name, "%s", ops[0]);
            l->sec = SEC_ABS;
            l->off = (uint32_t)parse_imm(a, ops[1]);
        }
    } else {
        fail(a, "unknown directive '%s'", d);
    }
}

/* ------------------------------------------------------------------ */
/* lines                                                               */
/* ------------------------------------------------------------------ */

static void define_label(Asm *a, const char *name) {
    if (a->pass != 1) return;
    for (int i = 0; i < a->nlabels; i++) {
        if (strcmp(a->labels[i].name, name) == 0) {
            fail(a, "label '%s' is defined twice", name);
            return;
        }
    }
    if (a->nlabels == a->caplabels) {
        a->caplabels = a->caplabels ? a->caplabels * 2 : 64;
        a->labels = realloc(a->labels, (size_t)a->caplabels * sizeof(Label));
    }
    Label *l = &a->labels[a->nlabels++];
    snprintf(l->name, sizeof l->name, "%s", name);
    l->sec = a->sec;
    l->off = a->off[a->sec];
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void process_line(Asm *a, char *line) {
    /* "# @line N" maps the following code to JLang line N */
    char *at = strstr(line, "# @line ");
    if (at) a->jl_line = atoi(at + 8);

    /* strip comments, but not inside a string */
    bool in_str = false;
    for (char *p = line; *p; p++) {
        if (*p == '"' && (p == line || p[-1] != '\\')) in_str = !in_str;
        if (!in_str && *p == '#') {
            *p = '\0';
            break;
        }
    }
    char *s = trim(line);

    /* labels */
    for (;;) {
        char *colon = strchr(s, ':');
        if (colon == NULL) break;
        bool ident = is_ident_start(*s);
        for (char *p = s; p < colon && ident; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '$')) ident = false;
        }
        if (!ident) break;
        *colon = '\0';
        define_label(a, s);
        s = trim(colon + 1);
    }
    if (*s == '\0') return;

    char mnemonic[32];
    size_t mn = 0;
    while (s[mn] && !isspace((unsigned char)s[mn]) && mn < sizeof mnemonic - 1) {
        mnemonic[mn] = (char)tolower((unsigned char)s[mn]);
        mn++;
    }
    mnemonic[mn] = '\0';
    char *rest = trim(s + mn);
    char rest_copy[512];
    snprintf(rest_copy, sizeof rest_copy, "%s", rest);

    char *ops[64];
    int n = 0;
    if (*rest && mnemonic[0] != '.') {
        for (char *tok = strtok(rest, ","); tok && n < 64; tok = strtok(NULL, ",")) ops[n++] = trim(tok);
    } else if (*rest) {
        bool quoted = false;
        char *start = rest;
        for (char *p = rest;; p++) {
            if (*p == '"') quoted = !quoted;
            if ((*p == ',' && !quoted) || *p == '\0') {
                bool end = *p == '\0';
                *p = '\0';
                if (n < 64) {
                    ops[n++] = trim(start);
                } else {
                    fail(a, "too many operands (at most 64 per line)");
                    return;
                }
                if (end) break;
                start = p + 1;
            }
        }
    }

    uint32_t start_addr = here(a);
    int start_sec = a->sec;
    a->line_nwords = 0;
    if (mnemonic[0] == '.') directive(a, mnemonic, ops, n, rest_copy);
    else instruction(a, mnemonic, ops, n);

    if (a->pass == 2 && start_sec == SEC_TEXT && a->sec == SEC_TEXT && here(a) > start_addr) {
        if (a->jl_line > 0) {
            RvImage *img = a->img;
            if (img->nlines == 0 || img->lines[img->nlines - 1].line != a->jl_line) {
                if (img->nlines == a->caplines) {
                    a->caplines = a->caplines ? a->caplines * 2 : 256;
                    img->lines = realloc(img->lines, (size_t)a->caplines * sizeof(RvLine));
                }
                img->lines[img->nlines].addr = start_addr;
                img->lines[img->nlines].line = a->jl_line;
                img->nlines++;
            }
        }
        if (a->want_listing) {
            char buf[256];
            int len = snprintf(buf, sizeof buf, "%08x  ", start_addr);
            for (int i = 0; i < 2; i++) {
                if (i < a->line_nwords) len += snprintf(buf + len, sizeof buf - (size_t)len, "%08x ", a->line_words[i]);
                else len += snprintf(buf + len, sizeof buf - (size_t)len, "         ");
            }
            len += snprintf(buf + len, sizeof buf - (size_t)len, " %s %s\n", mnemonic, rest_copy);
            lst_append(a, buf, (size_t)len);
        }
    }
}

static void run_pass(Asm *a, const char *src, int pass) {
    a->pass = pass;
    a->sec = SEC_TEXT;
    a->jl_line = 0;
    memset(a->off, 0, sizeof a->off);
    a->asm_line = 0;
    const char *p = src;
    char line[1024];
    while (*p && !a->failed) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) {
            a->asm_line++;
            fail(a, "line is too long");
            return;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        a->asm_line++;
        if (pass == 2 && a->want_listing) {
            char *t = trim(line);
            if (t[0] && t[strlen(t) - 1] == ':' && !strchr(t, ' ')) {
                lst_append(a, t, strlen(t));
                lst_append(a, "\n", 1);
            }
            memcpy(line, p, n);
            line[n] = '\0';
        }
        process_line(a, line);
        p = e ? e + 1 : p + n;
    }
}

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

bool rv_assemble(const char *src, RvImage *out, bool want_listing, bool allow_undefined) {
    memset(out, 0, sizeof *out);
    Asm a;
    memset(&a, 0, sizeof a);
    a.img = out;
    a.allow_undefined = allow_undefined;

    run_pass(&a, src, 1);
    if (!a.failed) {
        a.base[SEC_TEXT] = 0;
        a.base[SEC_RODATA] = align4(a.off[SEC_TEXT]);
        a.base[SEC_DATA] = align4(a.base[SEC_RODATA] + a.off[SEC_RODATA]);
        a.base[SEC_BSS] = align4(a.base[SEC_DATA] + a.off[SEC_DATA]);
        out->size = a.base[SEC_BSS] + a.off[SEC_BSS];
        out->text_size = a.off[SEC_TEXT];
        if (out->size > RV_RAM_SIZE - 64 * 1024) {
            snprintf(out->error, sizeof out->error, "program is too big (%u bytes)", out->size);
            a.failed = true;
        }
    }
    if (!a.failed) {
        out->image = calloc(1, out->size ? out->size : 1);
        a.want_listing = want_listing;
        run_pass(&a, src, 2);
    }

    if (!a.failed) {
        out->syms = calloc((size_t)(a.nlabels ? a.nlabels : 1), sizeof(RvSymbol));
        for (int i = 0; i < a.nlabels; i++) {
            snprintf(out->syms[i].name, sizeof out->syms[i].name, "%s", a.labels[i].name);
            symbol_value(&a, a.labels[i].name, &out->syms[i].addr);
        }
        out->nsyms = a.nlabels;
        out->listing = a.lst;
        a.lst = NULL;
    }
    free(a.labels);
    free(a.lst);
    if (a.failed) {
        free(out->image);
        free(out->lines);
        out->image = NULL;
        out->lines = NULL;
        out->nlines = 0;
        return false;
    }
    return true;
}

void rv_image_free(RvImage *img) {
    free(img->image);
    free(img->syms);
    free(img->lines);
    free(img->listing);
    memset(img, 0, sizeof *img);
}

bool rv_symbol(const RvImage *img, const char *name, uint32_t *addr) {
    for (int i = 0; i < img->nsyms; i++) {
        if (strcmp(img->syms[i].name, name) == 0) {
            *addr = img->syms[i].addr;
            return true;
        }
    }
    return false;
}

int rv_line_for(const RvImage *img, uint32_t addr) {
    int line = 0;
    for (int i = 0; i < img->nlines; i++) {
        if (img->lines[i].addr > addr) break;
        line = img->lines[i].line;
    }
    return line;
}
