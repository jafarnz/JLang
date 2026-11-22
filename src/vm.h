/*
 * vm.h - the inside of the JLang virtual machine (not part of the
 * public API; hosts use jlang.h).
 */
#ifndef JL_VM_H
#define JL_VM_H

#include "chunk.h"
#include "heap.h"
#include "jlang.h"

/* One active function call. */
typedef struct {
    int fn;         /* index into prog.fns */
    uint8_t *ip;    /* instruction pointer: next byte to run */
    Value *slots;   /* this call's window on the value stack */
} CallFrame;

typedef struct {
    char *name;
    int len;
    Value value;
    bool defined;  /* has been given a value at runtime */
    bool declared; /* has a let/fn/built-in declaration */
    int use_line;  /* first use, to report undefined names */
    int use_col;
} Global;

typedef struct {
    const char *name;
    JlNativeFn fn;
    int arity;
} Native;

typedef struct {
    char name[32];
    Value fn;
} EventHandler;

struct JlVM {
    Program prog;

    Value *stack;
    Value *sp; /* points one past the top value */
    CallFrame frames[JL_FRAMES_MAX];
    int frame_count;

    Global globals[JL_GLOBALS_MAX];
    int global_count;
    Native natives[JL_NATIVES_MAX];
    int native_count;
    EventHandler events[JL_EVENTS_MAX];
    int event_count;

    Heap heap;

    JlWriteFn out;
    JlWriteFn err;
    void *user_data;

    bool trace;
    JlStepFn step_hook;
    void *step_ud;
    bool error_reported;
    char errmsg[512];

    uint64_t cycles;               /* instructions executed */
    uint64_t max_cycles;           /* 0 = no limit */
    uint64_t op_counts[OP_COUNT];  /* per-instruction counts */
};

int vm_global_find(JlVM *vm, const char *name, int len);
int vm_global_add(JlVM *vm, const char *name, int len);

void vm_errf(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void vm_write(JlVM *vm, bool to_err, const char *data, size_t len);
void vm_print_value(JlVM *vm, Value v, bool to_err);
int vm_current_line(JlVM *vm);
bool vm_freed_error(JlVM *vm, HeapRef ref, const char *what);

#endif
