/*
 * rv_pipe.c - what the instructions a program ran would do in a classic
 * 5-stage RISC-V pipeline, the one in most computer architecture courses:
 *
 *   F fetch    read the instruction from memory
 *   D decode   work out what it is and read its registers
 *   X execute  the ALU does the arithmetic; branches are decided here
 *   M memory   loads and stores use the data memory
 *   W write    the result is written to its register
 *
 * Each cycle a new instruction can start, so in the best case one
 * instruction finishes every cycle. Two things break that rhythm:
 *
 *   - A load followed straight away by an instruction that uses what it
 *     loaded: the value only arrives after M, so the second instruction
 *     waits one cycle in D (a "load-use stall"). Every other result is
 *     forwarded from X or M to the next instruction, so it never waits.
 *   - A taken branch or a jump: the pipeline has already fetched the next
 *     two instructions in order before the branch is decided in X, so
 *     those two are thrown away ("flushed").
 *
 * The emulator runs the program as usual and feeds every instruction it
 * executed into this model through RvRunOptions.trace.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"

static const char *const REG[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

void rv_disasm(uint32_t in, uint32_t pc, char *out, size_t size) {
    uint32_t op = in & 0x7F, rd = (in >> 7) & 31, f3 = (in >> 12) & 7;
    uint32_t rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
    int32_t imm_i = sext(in >> 20, 12);
    int32_t imm_s = sext(((in >> 25) << 5) | ((in >> 7) & 31), 12);
    int32_t imm_b = sext(((in >> 31) << 12) | (((in >> 7) & 1) << 11) | (((in >> 25) & 63) << 5) | (((in >> 8) & 15) << 1), 13);
    int32_t imm_j = sext(((in >> 31) << 20) | (((in >> 12) & 255) << 12) | (((in >> 20) & 1) << 11) | (((in >> 21) & 1023) << 1), 21);
    static const char *const BR[8] = {"beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu"};
    static const char *const LD[8] = {"lb", "lh", "lw", "?", "lbu", "lhu", "?", "?"};
    static const char *const ST[8] = {"sb", "sh", "sw", "?", "?", "?", "?", "?"};
    static const char *const ALUI[8] = {"addi", "slli", "slti", "sltiu", "xori", "srli", "ori", "andi"};
    static const char *const ALU[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
    static const char *const MUL[8] = {"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
    switch (op) {
    case 0x37: snprintf(out, size, "lui %s, 0x%x", REG[rd], in >> 12); return;
    case 0x17: snprintf(out, size, "auipc %s, 0x%x", REG[rd], in >> 12); return;
    case 0x6F:
        if (rd == 0) snprintf(out, size, "j 0x%x", pc + (uint32_t)imm_j);
        else snprintf(out, size, "jal %s, 0x%x", REG[rd], pc + (uint32_t)imm_j);
        return;
    case 0x67:
        if (rd == 0 && rs1 == 1 && imm_i == 0) snprintf(out, size, "ret");
        else snprintf(out, size, "jalr %s, %d(%s)", REG[rd], imm_i, REG[rs1]);
        return;
    case 0x63: snprintf(out, size, "%s %s, %s, 0x%x", BR[f3], REG[rs1], REG[rs2], pc + (uint32_t)imm_b); return;
    case 0x03: snprintf(out, size, "%s %s, %d(%s)", LD[f3], REG[rd], imm_i, REG[rs1]); return;
    case 0x23: snprintf(out, size, "%s %s, %d(%s)", ST[f3], REG[rs2], imm_s, REG[rs1]); return;
    case 0x13:
        if (in == 0x00000013) snprintf(out, size, "nop");
        else if (f3 == 0 && rs1 == 0) snprintf(out, size, "li %s, %d", REG[rd], imm_i);
        else if (f3 == 0 && imm_i == 0) snprintf(out, size, "mv %s, %s", REG[rd], REG[rs1]);
        else if (f3 == 1 || f3 == 5)
            snprintf(out, size, "%s %s, %s, %u", f3 == 5 && (f7 & 0x20) ? "srai" : ALUI[f3], REG[rd], REG[rs1], rs2);
        else snprintf(out, size, "%s %s, %s, %d", ALUI[f3], REG[rd], REG[rs1], imm_i);
        return;
    case 0x33:
        if (f7 == 1) snprintf(out, size, "%s %s, %s, %s", MUL[f3], REG[rd], REG[rs1], REG[rs2]);
        else if (f7 == 0x20) snprintf(out, size, "%s %s, %s, %s", f3 == 0 ? "sub" : "sra", REG[rd], REG[rs1], REG[rs2]);
        else snprintf(out, size, "%s %s, %s, %s", ALU[f3], REG[rd], REG[rs1], REG[rs2]);
        return;
    case 0x73:
        if (in == 0x00000073) snprintf(out, size, "ecall");
        else if (in == 0x00100073) snprintf(out, size, "ebreak");
        else snprintf(out, size, "csrr %s, 0x%x", REG[rd], in >> 20);
        return;
    case 0x0F: snprintf(out, size, "fence"); return;
    }
    snprintf(out, size, ".word 0x%08x", in);
}

/* Which registers an instruction reads and writes, and whether it loads. */
typedef struct {
    int rd;  /* -1 if it writes none */
    int rs1; /* -1 if not read */
    int rs2;
    bool load;
} Uses;

static Uses uses(uint32_t in) {
    uint32_t op = in & 0x7F;
    int rd = (int)((in >> 7) & 31), rs1 = (int)((in >> 15) & 31), rs2 = (int)((in >> 20) & 31);
    Uses u = {-1, -1, -1, false};
    switch (op) {
    case 0x37: case 0x17: case 0x6F: u.rd = rd; break;
    case 0x67: u.rd = rd; u.rs1 = rs1; break;
    case 0x63: u.rs1 = rs1; u.rs2 = rs2; break;
    case 0x03: u.rd = rd; u.rs1 = rs1; u.load = true; break;
    case 0x23: u.rs1 = rs1; u.rs2 = rs2; break;
    case 0x13: u.rd = rd; u.rs1 = rs1; break;
    case 0x33: u.rd = rd; u.rs1 = rs1; u.rs2 = rs2; break;
    case 0x73: if (in >> 20 && ((in >> 12) & 7)) u.rd = rd; break;
    }
    if (u.rd == 0) u.rd = -1; /* x0 never changes */
    return u;
}

/* The routine an address is part of: the closest label before it, leaving
   out local labels such as jl_rt_puts_loop inside jl_rt_puts. */
static const RvSymbol *owner(const RvImage *img, uint32_t pc) {
    const RvSymbol *best = NULL;
    for (int i = 0; i < img->nsyms; i++) {
        const RvSymbol *s = &img->syms[i];
        bool routine = strncmp(s->name, "jl_", 3) == 0 && strlen(s->name) > 6 && strchr(s->name + 6, '_') == NULL;
        if (routine && s->addr <= pc && (!best || s->addr > best->addr)) best = s;
    }
    return best;
}

/* Code compiled from the program itself: jl_fn_0 (the script), jl_fn_1... */
static bool own_code(const RvImage *img, uint32_t pc) {
    const RvSymbol *s = owner(img, pc);
    return s && strncmp(s->name, "jl_fn_", 6) == 0;
}

#define PIPE_MAX_ROWS 64

typedef struct {
    uint32_t pc, insn;
    uint64_t fetch, ex; /* cycle numbers; decode runs from fetch+1 to ex-1 */
    const char *why;    /* why it waited, or NULL */
} Row;

struct RvPipe {
    uint64_t count;          /* instructions */
    uint64_t last_fetch, last_ex;
    Uses last;
    bool last_taken;
    uint64_t load_use;       /* stall cycles */
    uint64_t taken;          /* taken branches and jumps */
    const RvImage *img;
    bool recording;
    int rows_max, rows;
    Row row[PIPE_MAX_ROWS];
};

RvPipe *rv_pipe_new(const RvImage *img, int diagram_rows) {
    RvPipe *p = calloc(1, sizeof *p);
    if (p == NULL) return NULL;
    p->img = img;
    p->rows_max = diagram_rows < 0 ? 0 : diagram_rows > PIPE_MAX_ROWS ? PIPE_MAX_ROWS : diagram_rows;
    return p;
}

void rv_pipe_free(RvPipe *p) { free(p); }

void rv_pipe_step(void *pipe, uint32_t pc, uint32_t insn, uint32_t next_pc) {
    RvPipe *p = pipe;
    Uses u = uses(insn);
    uint64_t fetch, ex;
    const char *why = NULL;
    if (p->count == 0) {
        fetch = 0;
        ex = 2;
    } else {
        /* Fetched the cycle after the previous one, or after a taken
           branch was decided in X. */
        fetch = p->last_taken ? p->last_ex + 1 : p->last_fetch + 1 + (p->last_ex - p->last_fetch - 2);
        ex = fetch + 2;
        if (ex <= p->last_ex) ex = p->last_ex + 1;
        if (p->last.load && p->last.rd >= 0 && (u.rs1 == p->last.rd || u.rs2 == p->last.rd)) {
            if (ex < p->last_ex + 2) {
                p->load_use += p->last_ex + 2 - ex;
                ex = p->last_ex + 2;
            }
            why = "load-use: waits for the loaded value";
        }
        if (p->last_taken) why = why ? why : "after a taken branch: 2 flushed";
    }
    bool taken = next_pc != pc + 4;
    if (taken) p->taken++;
    if (!p->recording && p->rows_max > 0 && rv_line_for(p->img, pc) > 0 && own_code(p->img, pc)) p->recording = true;
    if (p->recording && p->rows < p->rows_max) p->row[p->rows++] = (Row){pc, insn, fetch, ex, why};
    p->count++;
    p->last_fetch = fetch;
    p->last_ex = ex;
    p->last = u;
    p->last_taken = taken;
}

void rv_pipe_report(const RvPipe *p, const RvImage *img, RvWriteFn out, void *ud) {
    char line[512];
    int n;
#define EMIT(...) (n = snprintf(line, sizeof line, __VA_ARGS__), out(ud, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1)))
    if (p->count == 0) return;
    uint64_t cycles = p->last_ex + 3; /* the last instruction's M and W */
    EMIT("\n--- pipeline ---\n");
    EMIT("5 stages: F fetch, D decode, X execute, M memory, W write back. Results are forwarded,\n");
    EMIT("a value loaded from memory reaches the next instruction one cycle late, and branches\n");
    EMIT("are decided in X, so a taken branch throws away the 2 instructions fetched after it.\n\n");
    if (p->rows > 0) {
        uint64_t base = p->row[0].fetch;
        uint64_t last = p->row[p->rows - 1].ex + 2 - base;
        int width = (int)(last + 1 > 60 ? 60 : last + 1);
        EMIT("%-14s %-26s", "line", "instruction    cycle:");
        for (int c = 0; c < width; c++) EMIT("%-2d", (int)(c + 1) % 10);
        EMIT("\n");
        for (int r = 0; r < p->rows; r++) {
            const Row *w = &p->row[r];
            char text[64];
            rv_disasm(w->insn, w->pc, text, sizeof text);
            /* The JLang line, or the runtime routine it's part of. */
            char where[24] = "";
            const RvSymbol *routine = owner(img, w->pc);
            if (own_code(img, w->pc)) {
                int src = rv_line_for(img, w->pc);
                if (src > 0) snprintf(where, sizeof where, "%d", src);
            } else if (routine) {
                snprintf(where, sizeof where, "%.23s", routine->name);
            }
            char cells[160];
            int k = 0;
            for (int c = 0; c < width && k + 2 < (int)sizeof cells; c++) {
                uint64_t cy = base + (uint64_t)c;
                char s = ' ';
                if (cy == w->fetch) s = 'F';
                else if (cy > w->fetch && cy < w->ex) s = cy == w->fetch + 1 ? 'D' : '-';
                else if (cy == w->ex) s = 'X';
                else if (cy == w->ex + 1) s = 'M';
                else if (cy == w->ex + 2) s = 'W';
                cells[k++] = s;
                cells[k++] = ' ';
            }
            while (k > 0 && cells[k - 1] == ' ') k--;
            cells[k] = '\0';
            EMIT("%-14.14s %-26.26s%s%s%s\n", where, text, cells, w->why ? "   " : "", w->why ? w->why : "");
        }
        EMIT("\n");
    }
    EMIT("%llu instructions took %llu cycles: %.2f cycles per instruction (1.00 is the best a\n",
         (unsigned long long)p->count, (unsigned long long)cycles, (double)cycles / (double)p->count);
    EMIT("pipeline like this can do). Of the extra cycles, %llu were load-use stalls and %llu were\n",
         (unsigned long long)p->load_use, (unsigned long long)(p->taken * 2));
    EMIT("flushed after %llu taken branches and jumps; the rest is filling the pipeline at the start.\n",
         (unsigned long long)p->taken);
#undef EMIT
}
