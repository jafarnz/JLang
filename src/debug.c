/*
 * debug.c - prints bytecode in a human-readable form.
 * Used by `jlang dis` and by x-ray mode (`jlang xray`).
 */
#include <string.h>

#include "debug.h"

static const char *const names[OP_COUNT] = {
    [OP_CONST] = "CONST",
    [OP_NIL] = "NIL",
    [OP_TRUE] = "TRUE",
    [OP_FALSE] = "FALSE",
    [OP_POP] = "POP",
    [OP_DUP2] = "DUP2",
    [OP_GET_LOCAL] = "GET_LOCAL",
    [OP_SET_LOCAL] = "SET_LOCAL",
    [OP_GET_GLOBAL] = "GET_GLOBAL",
    [OP_SET_GLOBAL] = "SET_GLOBAL",
    [OP_DEF_GLOBAL] = "DEF_GLOBAL",
    [OP_ADD] = "ADD",
    [OP_SUB] = "SUB",
    [OP_MUL] = "MUL",
    [OP_DIV] = "DIV",
    [OP_MOD] = "MOD",
    [OP_NEG] = "NEG",
    [OP_BAND] = "BAND",
    [OP_BOR] = "BOR",
    [OP_BXOR] = "BXOR",
    [OP_BNOT] = "BNOT",
    [OP_SHL] = "SHL",
    [OP_SHR] = "SHR",
    [OP_EQ] = "EQ",
    [OP_NE] = "NE",
    [OP_LT] = "LT",
    [OP_LE] = "LE",
    [OP_GT] = "GT",
    [OP_GE] = "GE",
    [OP_NOT] = "NOT",
    [OP_BOOL] = "BOOL",
    [OP_JUMP] = "JUMP",
    [OP_JUMP_IF_FALSE] = "JUMP_IF_FALSE",
    [OP_LOOP] = "LOOP",
    [OP_CALL] = "CALL",
    [OP_RETURN] = "RETURN",
    [OP_INDEX_GET] = "INDEX_GET",
    [OP_INDEX_SET] = "INDEX_SET",
    [OP_INDEX_UPDATE] = "INDEX_UPDATE",
    [OP_SLICE_GET] = "SLICE_GET",
    [OP_SLICE_UPDATE] = "SLICE_UPDATE",
    [OP_ON] = "ON",
    [OP_ARRAY] = "ARRAY",
    [OP_ADD_TEMP] = "ADD_TEMP",
};

const char *opcode_name(uint8_t op) {
    if (op < OP_COUNT && names[op] != NULL) return names[op];
    return "???";
}

static void print_constant(JlVM *vm, Value v) {
    if (v.type == VAL_STR) {
        jl_outf(vm, "\"");
        vm_print_value(vm, v, false);
        jl_outf(vm, "\"");
    } else {
        vm_print_value(vm, v, false);
    }
}

int disassemble_instruction(JlVM *vm, int fn, int offset) {
    Chunk *ch = &vm->prog.fns[fn].chunk;
    jl_outf(vm, "%04d ", offset);
    if (offset > 0 && ch->lines[offset] == ch->lines[offset - 1]) {
        jl_outf(vm, "   | ");
    } else {
        jl_outf(vm, "%4d ", ch->lines[offset]);
    }

    uint8_t op = ch->code[offset];
    const char *name = opcode_name(op);
    switch (op) {
    case OP_CONST: {
        uint16_t idx = (uint16_t)((ch->code[offset + 1] << 8) | ch->code[offset + 2]);
        jl_outf(vm, "%-14s %5u   ", name, idx);
        print_constant(vm, ch->consts[idx]);
        jl_outf(vm, "\n");
        return offset + 3;
    }
    case OP_ARRAY: {
        uint16_t n = (uint16_t)((ch->code[offset + 1] << 8) | ch->code[offset + 2]);
        jl_outf(vm, "%-14s %5u   (values)\n", name, n);
        return offset + 3;
    }
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
        jl_outf(vm, "%-14s %5u   (slot)\n", name, ch->code[offset + 1]);
        return offset + 2;
    case OP_CALL:
        jl_outf(vm, "%-14s %5u   (args)\n", name, ch->code[offset + 1]);
        return offset + 2;
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_DEF_GLOBAL: {
        uint8_t g = ch->code[offset + 1];
        jl_outf(vm, "%-14s %5u   %s\n", name, g, vm->globals[g].name);
        return offset + 2;
    }
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_LOOP: {
        int jump = (ch->code[offset + 1] << 8) | ch->code[offset + 2];
        int target = op == OP_LOOP ? offset + 3 - jump : offset + 3 + jump;
        jl_outf(vm, "%-14s       -> %04d\n", name, target);
        return offset + 3;
    }
    default:
        jl_outf(vm, "%s\n", name);
        return offset + 1;
    }
}

void disassemble_function(JlVM *vm, int fn) {
    Function *f = &vm->prog.fns[fn];
    if (strcmp(f->name, "<script>") == 0) {
        jl_outf(vm, "== top-level code ==\n");
    } else {
        jl_outf(vm, "== %s (%d param%s) ==\n", f->name, f->arity, f->arity == 1 ? "" : "s");
    }
    for (int off = 0; off < f->chunk.count;) {
        off = disassemble_instruction(vm, fn, off);
    }
    jl_outf(vm, "\n");
}
