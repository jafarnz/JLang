/*
 * chunk.h - bytecode: the instructions the JLang virtual machine runs.
 *
 * The compiler turns source code into a list of bytes. Each instruction
 * is a 1-byte opcode, sometimes followed by operand bytes:
 *
 *   let x = 2 + 3        CONST  0     push constant #0 (2)
 *                        CONST  1     push constant #1 (3)
 *                        ADD          pop two, push the sum
 *                        DEF_GLOBAL 0 pop into global #0 (x)
 *
 * This is the same idea as machine code for a real CPU, but for a
 * "stack machine": operands live on a stack instead of in registers.
 */
#ifndef JL_CHUNK_H
#define JL_CHUNK_H

#include "value.h"

typedef enum {
    OP_CONST,         /* u16 index: push a constant */
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_DUP2,          /* duplicate the top two values */

    OP_GET_LOCAL,     /* u8 slot */
    OP_SET_LOCAL,     /* u8 slot */
    OP_GET_GLOBAL,    /* u8 index */
    OP_SET_GLOBAL,    /* u8 index */
    OP_DEF_GLOBAL,    /* u8 index */

    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_NOT,           /* logical not -> bool */
    OP_BOOL,          /* convert to bool */

    OP_JUMP,          /* u16 forward offset */
    OP_JUMP_IF_FALSE, /* u16 forward offset, pops the condition */
    OP_LOOP,          /* u16 backward offset */

    OP_CALL,          /* u8 argument count */
    OP_RETURN,

    OP_INDEX_GET,     /* target[index]  (byte of a buf/str, bit of an int) */
    OP_INDEX_SET,     /* temp[index] = value, pushes value */
    OP_INDEX_UPDATE,  /* var[index] = value, pushes the updated container */
    OP_SLICE_GET,     /* int[hi..lo]  -> bits hi down to lo */
    OP_SLICE_UPDATE,  /* var[hi..lo] = value, pushes the updated int */
    OP_ON,            /* register an event handler */
    OP_ARRAY,         /* u16 count: make an array of the top count values */
    OP_ADD_TEMP,      /* + where the left value is the result of the + just before
                         it: a joined string nothing else can refer to, so it is
                         freed once it has been joined again */
    OP_TIMER,         /* u8 repeat: start a timer; pops the interval and the handler */

    OP_COUNT
} OpCode;

typedef struct {
    uint8_t *code;
    int *lines; /* source line of every byte, for error messages */
    int count;
    int cap;
    Value *consts;
    int const_count;
    int const_cap;
} Chunk;

typedef struct {
    char *name;
    int arity;
    Chunk chunk;
} Function;

typedef struct {
    char *chars;
    uint32_t len;
} StrConst;

/* Everything the compiler produced: functions and string constants. */
typedef struct {
    Function *fns;
    int fn_count;
    int fn_cap;
    StrConst *strs;
    int str_count;
    int str_cap;
} Program;

void *jl_xrealloc(void *ptr, size_t size);

void chunk_write(Chunk *c, uint8_t byte, int line);
int chunk_add_const(Chunk *c, Value v);

void program_init(Program *p);
void program_free(Program *p);
int program_add_fn(Program *p, const char *name, int len);
int program_intern(Program *p, const char *chars, uint32_t len);

#endif
