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
    const char *use_src;  /* source text valid during jl_compile */
    const char *use_file; /* filename valid during jl_compile, or NULL */
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

/* An `every` or `after` block waiting to run. */
typedef struct {
    Value fn;
    uint32_t interval; /* ms */
    uint32_t due;      /* time it runs next, from the host's clock */
    bool repeat;       /* every (true) or after (false) */
    bool active;
} Timer;

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
    Timer timers[JL_TIMERS_MAX];
    int timer_count;
    int current_timer;  /* the timer whose code is running, or -1 */
    JlClockFn clock;    /* the host's time in ms, for timers */
    void *clock_ud;

    Heap heap;

    JlWriteFn out;
    JlWriteFn err;
    void *user_data;

    JlFileReader file_reader;
    void *file_reader_ud;
    const char *source_path;

    bool trace;

    /* While capturing, program output goes into this buffer instead,
       which is how fmt() and + on strings build their result. */
    bool capturing;
    char *cap;
    size_t cap_len, cap_size;
    JlStepFn step_hook;
    void *step_ud;
    bool error_reported;
    char errmsg[512];

    volatile bool stop_requested;  /* set by jl_request_stop() */
    uint64_t cycles;               /* instructions executed */
    uint32_t rng;                  /* random() state, xorshift32 */
    uint64_t max_cycles;           /* 0 = no limit */
    uint64_t op_counts[OP_COUNT];  /* per-instruction counts */
};

int vm_global_find(JlVM *vm, const char *name, int len);
int vm_global_add(JlVM *vm, const char *name, int len);

void vm_errf(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void vm_write(JlVM *vm, bool to_err, const char *data, size_t len);
void vm_capture_begin(JlVM *vm);
/* Ends capturing and turns what was captured into a heap str. */
bool vm_capture_end(JlVM *vm, Value *out);
void vm_print_value(JlVM *vm, Value v, bool to_err);
int vm_current_line(JlVM *vm);
bool vm_freed_error(JlVM *vm, HeapRef ref, const char *what);
bool vm_stop_current_timer(JlVM *vm);

#endif
