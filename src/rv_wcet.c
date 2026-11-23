/*
 * rv_wcet.c - the worst-case time of each event handler, worked out from
 * the machine code without running it (jlang wcet).
 *
 * A handler that must answer a button within a millisecond has to finish
 * within a millisecond however its inputs turn out, which testing alone
 * can't show. This reads the handler's code, follows every branch and
 * call, and adds up the longest way through:
 *
 *   - every instruction costs what it can cost at most in the pipeline of
 *     jlang emu --pipeline: 1 cycle, 2 for a load (it may stall the next
 *     instruction), 3 for a branch or jump (it may flush 2 instructions);
 *   - a call costs the worst case of the routine it calls;
 *   - a loop costs its body's worst case times the number of times it can
 *     run, which is only known for loops like `for i in 0..8`. A `while`
 *     loop, or a `for` loop over a variable range, could run any number
 *     of times, so the handler has no limit and the report says which
 *     loop is to blame.
 *
 * The result is an upper bound for this pipeline model. A real chip adds
 * caches, interrupts and the board's own functions (jl_hal_*), which are
 * listed separately.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"

#define MAX_BLOCKS 2048
#define MAX_DEPTH 32

typedef struct {
    uint32_t start, end; /* [start, end): its instructions */
    int succ[2];
    int nsucc;
    uint64_t cost;       /* its own instructions, plus the calls they make */
    uint32_t loop_back;  /* if it ends with a jump back to a loop start: its address */
} Block;

typedef struct {
    const RvImage *img;
    char why[200];       /* the reason there is no limit, if there isn't */
    char hal[8][64];     /* board functions called */
    int nhal;
    int depth;
} Wcet;

static uint32_t word(const RvImage *img, uint32_t pc) {
    if (pc + 4 > img->text_size) return 0;
    const uint8_t *p = img->image + pc;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static uint32_t jal_target(uint32_t in, uint32_t pc) {
    int32_t imm = sext(((in >> 31) << 20) | (((in >> 12) & 255) << 12) | (((in >> 20) & 1) << 11) | (((in >> 21) & 1023) << 1), 21);
    return pc + (uint32_t)imm;
}

static uint32_t branch_target(uint32_t in, uint32_t pc) {
    int32_t imm = sext(((in >> 31) << 12) | (((in >> 7) & 1) << 11) | (((in >> 25) & 63) << 5) | (((in >> 8) & 15) << 1), 13);
    return pc + (uint32_t)imm;
}

static const char *name_at(const RvImage *img, uint32_t addr) {
    for (int i = 0; i < img->nsyms; i++)
        if (img->syms[i].addr == addr && img->syms[i].name[0] != '.') return img->syms[i].name;
    return NULL;
}

static int bound_at(const RvImage *img, uint32_t addr) {
    for (int i = 0; i < img->nbounds; i++)
        if (img->bounds[i].addr == addr) return img->bounds[i].count;
    return -1;
}

static bool routine(Wcet *w, uint32_t entry, uint64_t *out);

/* Board functions and printing take as long as the board and its serial
   port take, so they are listed instead of counted. */
static bool external(Wcet *w, const char *name) {
    if (!name) return false;
    const char *label = NULL;
    if (strncmp(name, "jl_hal_", 7) == 0) label = name;
    else if (strncmp(name, "jl_rt_put", 9) == 0) label = "printing";
    if (!label) return false;
    for (int i = 0; i < w->nhal; i++)
        if (strcmp(w->hal[i], label) == 0) return true;
    if (w->nhal < 8) snprintf(w->hal[w->nhal++], sizeof w->hal[0], "%s", label);
    return true;
}

/* An instruction's worst cost, and where control can go after it. */
typedef enum { NEXT, BRANCH, JUMP, RETURN, STOP, FAIL } Flow;

static Flow flow(Wcet *w, uint32_t pc, uint32_t *target, uint64_t *cost) {
    uint32_t in = word(w->img, pc);
    uint32_t op = in & 0x7F, rd = (in >> 7) & 31, rs1 = (in >> 15) & 31;
    *cost = 1;
    switch (op) {
    case 0x03: *cost = 2; return NEXT;
    case 0x63: *cost = 3; *target = branch_target(in, pc); return BRANCH;
    case 0x6F: {
        *cost = 3;
        *target = jal_target(in, pc);
        if (rd == 0) return JUMP;
        /* a call */
        const char *name = name_at(w->img, *target);
        if (name && (strcmp(name, "jl_rt_trap") == 0 || strcmp(name, "jl_hal_halt") == 0))
            return STOP; /* an error stops the program; it doesn't come back */
        if (external(w, name)) return NEXT;
        uint64_t callee;
        if (!routine(w, *target, &callee)) return FAIL;
        *cost += callee;
        return NEXT;
    }
    case 0x67:
        *cost = 3;
        if (rd == 0 && rs1 == 1) return RETURN;
        /* `call` is auipc + jalr: the auipc before it gives the address */
        if (rd == 1 && pc >= 4 && (word(w->img, pc - 4) & 0x7F) == 0x17 && ((word(w->img, pc - 4) >> 7) & 31) == rs1) {
            uint32_t au = word(w->img, pc - 4);
            *target = (pc - 4) + (au & 0xFFFFF000u) + (uint32_t)sext(in >> 20, 12);
            const char *name = name_at(w->img, *target);
            if (name && (strcmp(name, "jl_rt_trap") == 0 || strcmp(name, "jl_hal_halt") == 0)) return STOP;
            if (external(w, name)) return NEXT;
            uint64_t callee;
            if (!routine(w, *target, &callee)) return FAIL;
            *cost += callee;
            return NEXT;
        }
        snprintf(w->why, sizeof w->why, "it jumps to an address worked out while running (at 0x%x)", (unsigned)pc);
        return FAIL;
    }
    return NEXT;
}

/* The worst case of the routine starting at `entry`. */
static bool routine(Wcet *w, uint32_t entry, uint64_t *out) {
    if (++w->depth > MAX_DEPTH) {
        snprintf(w->why, sizeof w->why, "its function calls go more than %d deep, or call themselves", MAX_DEPTH);
        w->depth--;
        return false;
    }
    Block *b = calloc(MAX_BLOCKS, sizeof *b);
    uint32_t *todo = calloc(MAX_BLOCKS, sizeof *todo);
    int nb = 0, ntodo = 0;
    bool ok = b && todo;

    /* Find the blocks: each starts at the entry or at a jump target. */
    uint32_t *starts = calloc(MAX_BLOCKS, sizeof *starts);
    int nstarts = 0;
    ok = ok && starts;
    if (ok) {
        starts[nstarts++] = entry;
        todo[ntodo++] = entry;
    }
    while (ok && ntodo > 0) {
        uint32_t pc = todo[--ntodo];
        for (;;) {
            uint32_t t = 0;
            uint64_t c;
            uint32_t in = word(w->img, pc);
            uint32_t op = in & 0x7F;
            /* only look at control flow here; costs come later */
            Flow f = NEXT;
            if (op == 0x63) f = BRANCH, t = branch_target(in, pc);
            else if (op == 0x6F && ((in >> 7) & 31) == 0) f = JUMP, t = jal_target(in, pc);
            else if (op == 0x67 && ((in >> 7) & 31) == 0) f = RETURN;
            else if ((op == 0x6F || op == 0x67) && ((in >> 7) & 31) == 1) {
                uint32_t tt = 0;
                if (op == 0x6F) tt = jal_target(in, pc);
                else if (pc >= 4) tt = (pc - 4) + (word(w->img, pc - 4) & 0xFFFFF000u) + (uint32_t)sext(in >> 20, 12);
                const char *name = name_at(w->img, tt);
                if (name && (strcmp(name, "jl_rt_trap") == 0 || strcmp(name, "jl_hal_halt") == 0)) f = STOP;
            }
            (void)c;
            if (f == BRANCH || f == JUMP) {
                bool seen = false;
                for (int i = 0; i < nstarts; i++) seen = seen || starts[i] == t;
                if (!seen) {
                    if (nstarts == MAX_BLOCKS) {
                        ok = false;
                        snprintf(w->why, sizeof w->why, "it is too big to analyse");
                        break;
                    }
                    starts[nstarts++] = t;
                    todo[ntodo++] = t;
                }
            }
            if (f == BRANCH) {
                bool seen = false;
                for (int i = 0; i < nstarts; i++) seen = seen || starts[i] == pc + 4;
                if (!seen && nstarts < MAX_BLOCKS) {
                    starts[nstarts++] = pc + 4;
                    todo[ntodo++] = pc + 4;
                }
            }
            if (f != NEXT) break;
            pc += 4;
            if (pc >= w->img->text_size) break;
        }
    }

    /* Cut the code into blocks at those starts, and cost them. */
    for (int i = 0; ok && i < nstarts; i++) {
        Block *k = &b[nb++];
        k->start = starts[i];
        k->nsucc = 0;
        uint32_t pc = k->start;
        for (;;) {
            uint32_t t = 0;
            uint64_t c;
            Flow f = flow(w, pc, &t, &c);
            if (f == FAIL) {
                ok = false;
                break;
            }
            k->cost += c;
            pc += 4;
            bool next_is_start = false;
            for (int j = 0; j < nstarts; j++) next_is_start = next_is_start || starts[j] == pc;
            if (f == BRANCH) {
                k->succ[k->nsucc++] = (int)t; /* addresses for now */
                k->succ[k->nsucc++] = (int)pc;
                break;
            }
            if (f == JUMP) {
                k->succ[k->nsucc++] = (int)t;
                if (bound_at(w->img, pc - 4) >= 0 || t < pc) k->loop_back = pc - 4;
                break;
            }
            if (f == RETURN || f == STOP) break;
            if (next_is_start) {
                k->succ[k->nsucc++] = (int)pc;
                break;
            }
        }
        k->end = pc;
    }
    /* addresses -> block numbers */
    for (int i = 0; ok && i < nb; i++) {
        for (int s = 0; s < b[i].nsucc; s++) {
            int found = -1;
            for (int j = 0; j < nb; j++)
                if ((int)b[j].start == b[i].succ[s]) found = j;
            b[i].succ[s] = found;
        }
    }

    /* Loops: a jump back to an earlier block. Each block's cost is
       multiplied by how many times the loops around it can run, and the
       jump back is replaced by the loop's way out, which leaves an acyclic
       graph whose longest path is an upper bound. */
    uint64_t *mult = calloc((size_t)nb, sizeof *mult);
    ok = ok && mult;
    for (int i = 0; ok && i < nb; i++) mult[i] = 1;
    for (int i = 0; ok && i < nb; i++) {
        for (int s = 0; s < b[i].nsucc; s++) {
            int h = b[i].succ[s];
            if (h < 0 || b[h].start > b[i].start) continue; /* not backwards */
            int count = b[i].loop_back ? bound_at(w->img, b[i].loop_back) : -1;
            if (count < 0) {
                const char *rname = name_at(w->img, entry);
                if (rname && strncmp(rname, "jl_fn_", 6) != 0)
                    snprintf(w->why, sizeof w->why, "a loop in %s can run any number of times", rname);
                else
                    snprintf(w->why, sizeof w->why,
                             "the loop at line %d can run any number of times (only loops like for i in 0..8 have a known count)",
                             rv_line_for(w->img, b[h].end - 4));
                ok = false;
                break;
            }
            /* The loop is every block between the header and the jump back. */
            for (int j = 0; j < nb; j++)
                if (b[j].start >= b[h].start && b[j].start <= b[i].start) mult[j] *= (uint64_t)count + 1;
            /* Instead of jumping back, continue where the loop is left: the
               nearest block after it that a block inside it jumps to. */
            int exit = -1;
            for (int j = 0; j < nb; j++) {
                if (b[j].start < b[h].start || b[j].start > b[i].start) continue;
                for (int t = 0; t < b[j].nsucc; t++) {
                    int x = b[j].succ[t];
                    if (x >= 0 && b[x].start > b[i].start && (exit < 0 || b[x].start < b[exit].start)) exit = x;
                }
            }
            b[i].succ[s] = exit;
        }
    }

    /* Longest path from the entry, over blocks in address order (the graph
       is now acyclic and every edge goes forward). */
    uint64_t best = 0;
    if (ok) {
        uint64_t *dist = calloc((size_t)nb, sizeof *dist);
        bool *reached = calloc((size_t)nb, sizeof *reached);
        int *order = calloc((size_t)nb, sizeof *order);
        ok = dist && reached && order;
        for (int i = 0; ok && i < nb; i++) order[i] = i;
        for (int i = 1; ok && i < nb; i++) /* sort by address */
            for (int j = i; j > 0 && b[order[j]].start < b[order[j - 1]].start; j--) {
                int t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        if (ok) reached[0] = true; /* block 0 is the entry */
        for (int k = 0; ok && k < nb; k++) {
            int i = order[k];
            if (!reached[i]) continue;
            uint64_t here = dist[i] + b[i].cost * mult[i];
            if (here > best) best = here;
            for (int s = 0; s < b[i].nsucc; s++) {
                int j = b[i].succ[s];
                if (j < 0) continue;
                if (b[j].start <= b[i].start) {
                    ok = false;
                    snprintf(w->why, sizeof w->why, "its loops are too tangled to analyse");
                    break;
                }
                if (!reached[j] || here > dist[j]) dist[j] = here;
                reached[j] = true;
            }
        }
        free(dist);
        free(reached);
        free(order);
    }
    free(mult);
    free(b);
    free(todo);
    free(starts);
    w->depth--;
    *out = best;
    return ok;
}

void rv_wcet_report(const RvImage *img, uint32_t mhz, RvWriteFn out, void *ud) {
    char line[512];
    int n;
#define EMIT(...) (n = snprintf(line, sizeof line, __VA_ARGS__), out(ud, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1)))
    int handlers = 0;
    for (int i = 0; i < img->nsyms; i++) {
        const char *name = img->syms[i].name;
        if (strncmp(name, "jl_on_", 6) != 0 && strncmp(name, "jl_main", 7) != 0) continue;
        if (handlers++ == 0) {
            EMIT("Worst case of each part of the program, at %u MHz, in the pipeline model of\n", (unsigned)mhz);
            EMIT("jlang emu --pipeline. A real chip adds caches, interrupts and its board functions.\n\n");
        }
        Wcet w;
        memset(&w, 0, sizeof w);
        w.img = img;
        uint64_t cycles = 0;
        char label[48];
        if (strcmp(name, "jl_main") == 0) snprintf(label, sizeof label, "the script");
        else snprintf(label, sizeof label, "on %s", name + 6);
        if (!routine(&w, img->syms[i].addr, &cycles)) {
            EMIT("%-16s no limit: %s\n", label, w.why);
            continue;
        }
        cycles += 4; /* filling the pipeline */
        double us = (double)cycles / (double)mhz;
        EMIT("%-16s at most %llu cycles = %.2f %s", label, (unsigned long long)cycles, us >= 1000 ? us / 1000 : us,
             us >= 1000 ? "ms" : "us");
        if (w.nhal > 0) {
            EMIT(", plus");
            for (int h = 0; h < w.nhal; h++) EMIT("%s %s", h ? (h == w.nhal - 1 ? " and" : ",") : "", w.hal[h]);
        }
        EMIT("\n");
    }
    if (handlers == 0) EMIT("The program has no code to analyse.\n");
#undef EMIT
}
