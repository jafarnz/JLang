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

/* Compile without running. With print = true, also show the bytecode. */
JlResult jl_disassemble_or_check(JlVM *vm, const char *source, bool print);

void jl_set_trace(JlVM *vm, bool on);
void jl_print_stats(JlVM *vm);
int jl_report_leaks(JlVM *vm);
void *jl_user_data(JlVM *vm);

/* Helpers for writing natives. */
bool jl_error(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void jl_outf(JlVM *vm, const char *fmt, ...) JL_PRINTF(2, 3);
void jl_print_value(JlVM *vm, Value v);
bool jl_new_string(JlVM *vm, const char *chars, uint32_t len, Value *out);
bool jl_new_buf(JlVM *vm, uint32_t len, Value *out, uint8_t **data);
bool jl_get_string(JlVM *vm, Value v, const char **chars, uint32_t *len);
bool jl_get_int(JlVM *vm, const char *fn_name, int arg_no, Value v,
                int32_t *out);
uint64_t jl_cycles(JlVM *vm);

#endif
