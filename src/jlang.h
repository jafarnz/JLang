/*
 * jlang.h - the public API for embedding JLang in a C program.
 *
 * The same API is used by the desktop command-line tool (main.c) and
 * will be used by the ESP32 firmware for Iris. A host program:
 *
 *   1. creates a VM          jl_new(&config)
 *   2. adds its own built-ins jl_register_native(vm, "capture", fn, 0)
 *   3. runs a script          jl_run_source(vm, source)
 *   4. fires events           jl_emit(vm, "button1")
 */
#ifndef JLANG_H
#define JLANG_H

#include "value.h"

typedef struct JlVM JlVM;

typedef enum {
    JL_OK,
    JL_COMPILE_ERROR,
    JL_RUNTIME_ERROR,
    JL_NO_HANDLER
} JlResult;

typedef void (*JlWriteFn)(void *user_data, const char *data, size_t len);

/*
 * A built-in function written in C. Read arguments from args[0..argc-1],
 * store the result in *out (it starts as nil). Return true on success.
 * On failure return jl_error(vm, "...") which records the message.
 */
typedef bool (*JlNativeFn)(JlVM *vm, int argc, Value *args, Value *out);

typedef struct {
    uint32_t heap_size;   /* bytes for the JLang heap */
    uint16_t max_handles; /* max live heap blocks */
    uint64_t max_cycles;  /* stop after this many instructions (0 = no limit).
                             Works like a watchdog: an endless loop becomes
                             an error instead of a frozen device. */
    JlWriteFn out;        /* program output (NULL = stdout) */
    JlWriteFn err;        /* errors and warnings (NULL = stderr) */
    void *user_data;      /* passed to out/err and available to natives */
} JlConfig;

void jl_default_config(JlConfig *config);
JlVM *jl_new(const JlConfig *config);
void jl_free(JlVM *vm);

/* arity -1 means "any number of arguments". */
bool jl_register_native(JlVM *vm, const char *name, JlNativeFn fn, int arity);

JlResult jl_run_source(JlVM *vm, const char *source);
JlResult jl_emit(JlVM *vm, const char *event);
bool jl_has_handler(const JlVM *vm, const char *event);

/* Optional file reader for `use "path.jlang"` imports. The callback must
   return a malloc'd NUL-terminated string, or NULL if the file can't be read. */
typedef char *(*JlFileReader)(void *user_data, const char *path);
void jl_set_file_reader(JlVM *vm, const char *source_path, JlFileReader reader, void *user_data);

/* Compile without running. With print = true, also show the bytecode. */
JlResult jl_disassemble_or_check(JlVM *vm, const char *source, bool print);

/* Built-in unit tests declared with `test "name" { ... }`. */
int jl_test_count(const JlVM *vm);
const char *jl_test_name(const JlVM *vm, int index);
JlResult jl_run_test(JlVM *vm, int index);

void jl_set_trace(JlVM *vm, bool on);

/* Seeds random(). It starts with a fixed seed, so simulator runs repeat
   exactly; device firmware seeds it from the hardware. */
void jl_seed_random(JlVM *vm, uint32_t seed);

/*
 * Stepping: the VM calls the hook before every instruction, with a view
 * of the machine at that moment. The hook can wait (to run the program
 * slowly), draw the state somewhere, or return false to stop the program,
 * which then ends with a "stopped by the host" error. This is what x-ray
 * mode prints, made available to a host program.
 */
typedef struct {
    int line;           /* source line of the instruction */
    int offset;         /* its byte offset in the function's bytecode */
    const char *op;     /* its name, such as "ADD" or "CALL" */
    const Value *stack; /* the value stack, bottom first */
    int depth;          /* how many values are on it */
    int frames;         /* how many function calls are active */
    uint64_t cycles;    /* instructions executed before this one */
    uint32_t heap_used; /* bytes of heap the program has allocated */
    uint32_t heap_size;
} JlStep;

typedef bool (*JlStepFn)(JlVM *vm, const JlStep *step, void *user_data);

/*
 * Timers (`every 500ms { ... }` and `after 2s { ... }`). The host owns
 * time: it gives the VM a clock, and after the script has run it waits
 * until jl_next_timer() says a timer is due, then calls jl_run_timers().
 */
/* Asks a running program to stop at its next instruction; safe to call
   from another task or an interrupt. The program ends with an error. */
void jl_request_stop(JlVM *vm);

typedef uint32_t (*JlClockFn)(void *user_data);
void jl_set_clock(JlVM *vm, JlClockFn clock, void *user_data);
/* True if a timer is waiting; *due is when the next one should run. */
bool jl_next_timer(JlVM *vm, uint32_t *due);
/* Runs every timer that is due at `now`. */
JlResult jl_run_timers(JlVM *vm, uint32_t now);
void jl_set_step_hook(JlVM *vm, JlStepFn fn, void *user_data);
typedef struct {
    uint32_t heap_used;
    uint32_t heap_cap;
    uint32_t heap_peak;
    uint32_t live_blocks;
    uint64_t cycles;
} JlStats;
void jl_get_stats(const JlVM *vm, JlStats *out);
void jl_print_stats(JlVM *vm);
int jl_report_leaks(JlVM *vm);
void *jl_user_data(JlVM *vm);

/* Helpers for writing natives. */
bool jl_error(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void jl_outf(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void jl_print_value(JlVM *vm, Value v);
bool jl_new_string(JlVM *vm, const char *chars, uint32_t len, Value *out);
bool jl_new_buf(JlVM *vm, uint32_t len, Value *out, uint8_t **data);
/* A new array of n items, all 0. Set them through *items. */
bool jl_new_array(JlVM *vm, uint32_t n, Value *out, Value **items);
/* The items of an array. Records an error for anything else. */
bool jl_get_array(JlVM *vm, const char *fn_name, Value v, Value **items, uint32_t *count);
bool jl_get_string(JlVM *vm, Value v, const char **chars, uint32_t *len);
/* The bytes of a buf (writable) or str (read-only). Records an error and
   returns false for any other value or for freed memory. */
bool jl_get_bytes(JlVM *vm, const char *fn_name, Value v, uint8_t **data, uint32_t *len);
bool jl_get_int(JlVM *vm, const char *fn_name, int arg_no, Value v,
                int32_t *out);
uint64_t jl_cycles(JlVM *vm);

#endif
