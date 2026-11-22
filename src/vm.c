/*
 * vm.c - the JLang virtual machine: a stack-based bytecode interpreter.
 *
 * The heart of it is run(): a loop that reads one opcode at a time and
 * executes it, exactly like the fetch-decode-execute cycle of a CPU.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "debug.h"
#include "vm.h"

void jl_register_core(JlVM *vm); /* builtins.c */

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void std_out(void *ud, const char *d, size_t n) {
    (void)ud;
    fwrite(d, 1, n, stdout);
}

static void std_err(void *ud, const char *d, size_t n) {
    (void)ud;
    fflush(stdout); /* keep program output and errors in order */
    fwrite(d, 1, n, stderr);
    fflush(stderr);
}

void vm_write(JlVM *vm, bool to_err, const char *data, size_t len) {
    (to_err ? vm->err : vm->out)(vm->user_data, data, len);
}

static void vwritef(JlVM *vm, bool to_err, const char *fmt, va_list ap) {
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && (size_t)n < sizeof buf) {
        vm_write(vm, to_err, buf, (size_t)n);
    } else if (n >= 0) {
        char *big = malloc((size_t)n + 1);
        if (big != NULL) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            vm_write(vm, to_err, big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

void jl_outf(JlVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwritef(vm, false, fmt, ap);
    va_end(ap);
}

void vm_errf(JlVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwritef(vm, true, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void jl_default_config(JlConfig *c) {
    c->heap_size = JL_DEFAULT_HEAP;
    c->max_cycles = 0;
    c->max_handles = JL_HANDLES_MAX;
    c->out = NULL;
    c->err = NULL;
    c->user_data = NULL;
}

JlVM *jl_new(const JlConfig *config) {
    JlConfig def;
    if (config == NULL) {
        jl_default_config(&def);
        config = &def;
    }
    JlVM *vm = calloc(1, sizeof *vm);
    if (vm == NULL) return NULL;
    vm->stack = malloc(JL_STACK_MAX * sizeof(Value));
    if (vm->stack == NULL ||
        !heap_init(&vm->heap, config->heap_size, config->max_handles)) {
        free(vm->stack);
        free(vm);
        return NULL;
    }
    vm->sp = vm->stack;
    program_init(&vm->prog);
    vm->out = config->out ? config->out : std_out;
    vm->err = config->err ? config->err : std_err;
    vm->user_data = config->user_data;
    vm->max_cycles = config->max_cycles;
    jl_register_core(vm);
    return vm;
}

void jl_free(JlVM *vm) {
    if (vm == NULL) return;
    for (int i = 0; i < vm->global_count; i++) free(vm->globals[i].name);
    program_free(&vm->prog);
    heap_destroy(&vm->heap);
    free(vm->stack);
    free(vm);
}

void *jl_user_data(JlVM *vm) { return vm->user_data; }
void jl_set_trace(JlVM *vm, bool on) { vm->trace = on; }

void jl_set_step_hook(JlVM *vm, JlStepFn fn, void *user_data) {
    vm->step_hook = fn;
    vm->step_ud = user_data;
}
uint64_t jl_cycles(JlVM *vm) { return vm->cycles; }

int vm_global_find(JlVM *vm, const char *name, int len) {
    for (int i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].len == len && memcmp(vm->globals[i].name, name, (size_t)len) == 0) {
            return i;
        }
    }
    return -1;
}

int vm_global_add(JlVM *vm, const char *name, int len) {
    if (vm->global_count == JL_GLOBALS_MAX) return -1;
    Global *g = &vm->globals[vm->global_count];
    memset(g, 0, sizeof *g);
    g->name = malloc((size_t)len + 1);
    if (g->name == NULL) return -1;
    memcpy(g->name, name, (size_t)len);
    g->name[len] = '\0';
    g->len = len;
    g->value = nil_val();
    return vm->global_count++;
}

bool jl_register_native(JlVM *vm, const char *name, JlNativeFn fn, int arity) {
    if (vm->native_count == JL_NATIVES_MAX) return false;
    int len = (int)strlen(name);
    int g = vm_global_find(vm, name, len);
    if (g >= 0 && vm->globals[g].declared) return false;
    if (g < 0) g = vm_global_add(vm, name, len);
    if (g < 0) return false;

    Native *n = &vm->natives[vm->native_count];
    n->name = vm->globals[g].name;
    n->fn = fn;
    n->arity = arity;
    vm->globals[g].value = native_val((uint16_t)vm->native_count);
    vm->globals[g].defined = true;
    vm->globals[g].declared = true;
    vm->native_count++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

bool jl_error(JlVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->errmsg, sizeof vm->errmsg, fmt, ap);
    va_end(ap);
    return false;
}

int vm_current_line(JlVM *vm) {
    if (vm->frame_count == 0) return 0;
    CallFrame *f = &vm->frames[vm->frame_count - 1];
    Chunk *ch = &vm->prog.fns[f->fn].chunk;
    int off = (int)(f->ip - ch->code) - 1;
    if (off < 0) off = 0;
    return ch->lines[off];
}

bool vm_freed_error(JlVM *vm, HeapRef ref, const char *what) {
    const HandleEntry *e = heap_entry(&vm->heap, ref.handle);
    if (e != NULL && !e->live) {
        return jl_error(vm, "use after free: this %s was freed at line %u "
                            "(it was allocated at line %u)",
                        what, (unsigned)e->free_line, (unsigned)e->alloc_line);
    }
    return jl_error(vm, "use after free: this %s was freed earlier and its "
                        "memory has been reused", what);
}

/* Prints the error once, with a trace of the active function calls. */
static void report_error(JlVM *vm) {
    if (vm->error_reported) return;
    vm->error_reported = true;
    vm_errf(vm, "runtime error (line %d): %s\n", vm_current_line(vm), vm->errmsg);
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame *f = &vm->frames[i];
        Function *fn = &vm->prog.fns[f->fn];
        int off = (int)(f->ip - fn->chunk.code) - 1;
        int line = fn->chunk.lines[off < 0 ? 0 : off];

        /* Collapse runs of identical frames (deep recursion). */
        int same = 0;
        while (i - same - 1 >= 0) {
            CallFrame *g = &vm->frames[i - same - 1];
            if (g->fn != f->fn || g->ip != f->ip) break;
            same++;
        }

        if (strcmp(fn->name, "<script>") == 0) {
            vm_errf(vm, "  in top-level code, line %d\n", line);
        } else if (strncmp(fn->name, "on ", 3) == 0) {
            vm_errf(vm, "  in handler '%s', line %d\n", fn->name, line);
        } else {
            vm_errf(vm, "  in %s(), line %d\n", fn->name, line);
        }
        if (same > 0) {
            vm_errf(vm, "  ... the same call repeated %d more time%s\n", same,
                    same == 1 ? "" : "s");
            i -= same;
        }
    }
}

static void reset_stack(JlVM *vm) {
    vm->sp = vm->stack;
    vm->frame_count = 0;
}

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

bool jl_get_string(JlVM *vm, Value v, const char **chars, uint32_t *len) {
    if (v.type != VAL_STR) return false;
    if (!v.on_heap) {
        StrConst *s = &vm->prog.strs[v.as.index];
        *chars = s->chars;
        *len = s->len;
        return true;
    }
    uint8_t *data;
    if (heap_get(&vm->heap, v.as.ref, &data, len) != HEAP_OK) return false;
    *chars = (const char *)data;
    return true;
}

static bool new_block(JlVM *vm, uint32_t len, uint8_t kind, HeapRef *ref, uint8_t **data) {
    HeapStatus st = heap_alloc(&vm->heap, len, kind, (uint32_t)vm_current_line(vm), ref);
    if (st == HEAP_NO_HANDLES) {
        return jl_error(vm, "too many live heap blocks (max %u). Are you forgetting to free()?",
                        vm->heap.handle_cap);
    }
    if (st != HEAP_OK) {
        return jl_error(vm, "out of memory: needed %u bytes, but the largest free block "
                            "is %u bytes", (unsigned)len, (unsigned)(heap_largest_free(&vm->heap)));
    }
    uint32_t n;
    heap_get(&vm->heap, *ref, data, &n);
    return true;
}

bool jl_new_string(JlVM *vm, const char *chars, uint32_t len, Value *out) {
    HeapRef ref;
    uint8_t *data;
    if (!new_block(vm, len, BLOCK_STR, &ref, &data)) return false;
    if (len > 0) memcpy(data, chars, len);
    *out = heap_str_val(ref);
    return true;
}

bool jl_new_buf(JlVM *vm, uint32_t len, Value *out, uint8_t **data) {
    HeapRef ref;
    if (!new_block(vm, len, BLOCK_BUF, &ref, data)) return false;
    *out = buf_val(ref);
    return true;
}

bool jl_get_int(JlVM *vm, const char *fn_name, int arg_no, Value v, int32_t *out) {
    if (v.type != VAL_INT) {
        return jl_error(vm, "%s(): argument %d must be an int, got %s",
                        fn_name, arg_no, value_type_name(v));
    }
    *out = v.as.i;
    return true;
}

void vm_print_value(JlVM *vm, Value v, bool to_err) {
    char buf[64];
    switch ((ValueType)v.type) {
    case VAL_NIL: vm_write(vm, to_err, "nil", 3); return;
    case VAL_BOOL:
        if (v.as.b) vm_write(vm, to_err, "true", 4);
        else vm_write(vm, to_err, "false", 5);
        return;
    case VAL_INT: {
        int n = snprintf(buf, sizeof buf, "%d", (int)v.as.i);
        vm_write(vm, to_err, buf, (size_t)n);
        return;
    }
    case VAL_STR: {
        const char *s;
        uint32_t len;
        if (jl_get_string(vm, v, &s, &len)) vm_write(vm, to_err, s, len);
        else vm_write(vm, to_err, "<freed str>", 11);
        return;
    }
    case VAL_BUF: {
        uint8_t *d;
        uint32_t len;
        int n;
        if (heap_get(&vm->heap, v.as.ref, &d, &len) == HEAP_OK) {
            n = snprintf(buf, sizeof buf, "<buf #%u, %u bytes>",
                         (unsigned)v.as.ref.handle, (unsigned)len);
        } else {
            n = snprintf(buf, sizeof buf, "<freed buf>");
        }
        vm_write(vm, to_err, buf, (size_t)n);
        return;
    }
    case VAL_FN: {
        int n = snprintf(buf, sizeof buf, "<fn %s>", vm->prog.fns[v.as.index].name);
        vm_write(vm, to_err, buf, (size_t)n);
        return;
    }
    case VAL_NATIVE: {
        int n = snprintf(buf, sizeof buf, "<built-in %s>", vm->natives[v.as.index].name);
        vm_write(vm, to_err, buf, (size_t)n);
        return;
    }
    }
}

void jl_print_value(JlVM *vm, Value v) { vm_print_value(vm, v, false); }

/* Like C: nil, false and 0 are false; everything else is true. */
static bool truthy(Value v) {
    switch ((ValueType)v.type) {
    case VAL_NIL: return false;
    case VAL_BOOL: return v.as.b;
    case VAL_INT: return v.as.i != 0;
    default: return true;
    }
}

/* Returns 1 if equal, 0 if not, -1 on error (freed string). */
static int values_equal(JlVM *vm, Value a, Value b) {
    if (a.type != b.type) return 0;
    switch ((ValueType)a.type) {
    case VAL_NIL: return 1;
    case VAL_BOOL: return a.as.b == b.as.b;
    case VAL_INT: return a.as.i == b.as.i;
    case VAL_FN:
    case VAL_NATIVE: return a.as.index == b.as.index;
    case VAL_BUF:
        return a.as.ref.handle == b.as.ref.handle && a.as.ref.gen == b.as.ref.gen;
    case VAL_STR: {
        const char *sa, *sb;
        uint32_t la, lb;
        if (!jl_get_string(vm, a, &sa, &la)) {
            vm_freed_error(vm, a.as.ref, "str");
            return -1;
        }
        if (!jl_get_string(vm, b, &sb, &lb)) {
            vm_freed_error(vm, b.as.ref, "str");
            return -1;
        }
        return la == lb && memcmp(sa, sb, la) == 0;
    }
    }
    return 0;
}

/* The bytes behind a buf or str, for indexing. */
static bool get_bytes(JlVM *vm, Value v, uint8_t **data, uint32_t *len) {
    if (v.type == VAL_BUF) {
        if (heap_get(&vm->heap, v.as.ref, data, len) != HEAP_OK) {
            return vm_freed_error(vm, v.as.ref, "buf");
        }
        return true;
    }
    if (v.type == VAL_STR) {
        const char *s;
        if (!jl_get_string(vm, v, &s, len)) return vm_freed_error(vm, v.as.ref, "str");
        *data = (uint8_t *)(uintptr_t)s;
        return true;
    }
    return jl_error(vm, "can't index a %s (you can index a buf, a str, or the bits of an int)",
                    value_type_name(v));
}

bool jl_get_bytes(JlVM *vm, const char *fn_name, Value v, uint8_t **data, uint32_t *len) {
    if (v.type != VAL_BUF && v.type != VAL_STR)
        return jl_error(vm, "%s(): expected a buf or str, got %s", fn_name, value_type_name(v));
    return get_bytes(vm, v, data, len);
}

static bool check_index(JlVM *vm, Value idx, uint32_t len, const char *what, int32_t *out) {
    if (idx.type != VAL_INT) {
        return jl_error(vm, "an index must be an int, got %s", value_type_name(idx));
    }
    int32_t i = idx.as.i;
    if (i < 0 || (uint32_t)i >= len) {
        if (len == 0) return jl_error(vm, "index %d is out of bounds: this %s is empty", (int)i, what);
        return jl_error(vm, "index %d is out of bounds: this %s has %u bytes, so valid "
                            "indexes are 0..%u", (int)i, what, (unsigned)len, (unsigned)(len - 1));
    }
    *out = i;
    return true;
}

/* buf[i] = v. Only bytes 0..255 fit. */
static bool store_byte(JlVM *vm, Value target, Value idx, Value v) {
    if (target.type == VAL_STR) {
        return jl_error(vm, "strings are read-only; copy the bytes into a buf to change them");
    }
    if (target.type != VAL_BUF) {
        return jl_error(vm, "can't set an element of a %s", value_type_name(target));
    }
    uint8_t *data;
    uint32_t len;
    int32_t i;
    if (!get_bytes(vm, target, &data, &len)) return false;
    if (!check_index(vm, idx, len, "buf", &i)) return false;
    if (v.type != VAL_INT) {
        return jl_error(vm, "a buf holds bytes (ints 0..255), got %s", value_type_name(v));
    }
    if (v.as.i < 0 || v.as.i > 255) {
        return jl_error(vm, "%d doesn't fit in a byte (0..255). Hint: use u8(v) or "
                            "v & 0xFF to keep only the low 8 bits", (int)v.as.i);
    }
    data[i] = (uint8_t)v.as.i;
    return true;
}

static bool check_bit(JlVM *vm, Value idx, int32_t *out) {
    if (idx.type != VAL_INT) {
        return jl_error(vm, "a bit number must be an int, got %s", value_type_name(idx));
    }
    if (idx.as.i < 0 || idx.as.i > 31) {
        return jl_error(vm, "bit %d doesn't exist: an int has bits 0..31", (int)idx.as.i);
    }
    *out = idx.as.i;
    return true;
}

static bool check_range(JlVM *vm, Value target, Value hi, Value lo,
                        int32_t *h, int32_t *l, uint32_t *mask) {
    if (target.type != VAL_INT) {
        return jl_error(vm, "bit ranges like x[7..4] only work on ints, got %s",
                        value_type_name(target));
    }
    if (hi.type != VAL_INT || lo.type != VAL_INT) {
        return jl_error(vm, "bit range limits must be ints");
    }
    *h = hi.as.i;
    *l = lo.as.i;
    if (*l < 0 || *h > 31 || *l > 31 || *h < 0) {
        return jl_error(vm, "bit range %d..%d is outside 0..31", (int)(*h), (int)(*l));
    }
    if (*h < *l) {
        return jl_error(vm, "write bit ranges high..low, like x[7..4] (got %d..%d)", (int)(*h), (int)(*l));
    }
    int width = *h - *l + 1;
    *mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
    return true;
}

/* ------------------------------------------------------------------ */
/* Calls                                                               */
/* ------------------------------------------------------------------ */

static bool call_value(JlVM *vm, Value callee, int argc) {
    if (callee.type == VAL_FN) {
        Function *fn = &vm->prog.fns[callee.as.index];
        if (argc != fn->arity) {
            return jl_error(vm, "%s() takes %d argument%s but got %d", fn->name,
                            fn->arity, fn->arity == 1 ? "" : "s", argc);
        }
        if (vm->frame_count == JL_FRAMES_MAX) {
            return jl_error(vm, "stack overflow: more than %d nested calls. "
                                "Is there a recursion that never stops?", JL_FRAMES_MAX);
        }
        CallFrame *f = &vm->frames[vm->frame_count++];
        f->fn = callee.as.index;
        f->ip = fn->chunk.code;
        f->slots = vm->sp - argc - 1;
        return true;
    }
    if (callee.type == VAL_NATIVE) {
        Native *n = &vm->natives[callee.as.index];
        if (n->arity >= 0 && argc != n->arity) {
            return jl_error(vm, "%s() takes %d argument%s but got %d", n->name,
                            n->arity, n->arity == 1 ? "" : "s", argc);
        }
        Value out = nil_val();
        Value *args = vm->sp - argc;
        if (!n->fn(vm, argc, args, &out)) return false;
        vm->sp -= argc + 1;
        *vm->sp++ = out;
        return true;
    }
    return jl_error(vm, "can't call a %s (only functions can be called)",
                    value_type_name(callee));
}

static const char *op_symbol(uint8_t op) {
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUB: return "-";
    case OP_MUL: return "*";
    case OP_DIV: return "/";
    case OP_MOD: return "%";
    case OP_BAND: return "&";
    case OP_BOR: return "|";
    case OP_BXOR: return "^";
    case OP_SHL: return "<<";
    case OP_SHR: return ">>";
    case OP_LT: return "<";
    case OP_LE: return "<=";
    case OP_GT: return ">";
    case OP_GE: return ">=";
    default: return "?";
    }
}

static void binary_type_error(JlVM *vm, uint8_t op, Value a, Value b) {
    if (op == OP_ADD && (a.type == VAL_STR || b.type == VAL_STR)) {
        jl_error(vm, "can't use + on %s and %s. JLang has no string +; "
                     "print several values instead: print(a, b)",
                 value_type_name(a), value_type_name(b));
        return;
    }
    jl_error(vm, "can't use '%s' on %s and %s (it only works on ints)",
             op_symbol(op), value_type_name(a), value_type_name(b));
}

static void trace_stack(JlVM *vm) {
    jl_outf(vm, "          stack:");
    if (vm->sp == vm->stack) jl_outf(vm, " (empty)");
    for (Value *v = vm->stack; v < vm->sp; v++) {
        jl_outf(vm, " [");
        vm_print_value(vm, *v, false);
        jl_outf(vm, "]");
    }
    jl_outf(vm, "\n");
}

/* ------------------------------------------------------------------ */
/* The interpreter loop                                                */
/* ------------------------------------------------------------------ */

/* Runs until the call at depth `base` returns. */
static JlResult run(JlVM *vm, int base) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];

#define CHUNK() (&vm->prog.fns[frame->fn].chunk)
#define READ_BYTE() (*frame->ip++)
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define PUSH(v)                                                                   \
    do {                                                                          \
        if (vm->sp >= vm->stack + JL_STACK_MAX) {                                 \
            jl_error(vm, "value stack overflow (an expression or recursion is "   \
                         "too deep)");                                            \
            goto error;                                                           \
        }                                                                         \
        *vm->sp++ = (v);                                                          \
    } while (0)
#define POP() (*--vm->sp)
#define PEEK(n) (vm->sp[-1 - (n)])
#define INT_OPERANDS(op)                                     \
    if (PEEK(0).type != VAL_INT || PEEK(1).type != VAL_INT) { \
        binary_type_error(vm, (op), PEEK(1), PEEK(0));        \
        goto error;                                           \
    }                                                         \
    int32_t b = POP().as.i;                                   \
    int32_t a = POP().as.i

    for (;;) {
        if (vm->trace) {
            trace_stack(vm);
            disassemble_instruction(vm, frame->fn, (int)(frame->ip - CHUNK()->code));
        }
        if (vm->step_hook != NULL) {
            int off = (int)(frame->ip - CHUNK()->code);
            JlStep st = {
                .line = CHUNK()->lines[off],
                .offset = off,
                .op = opcode_name(*frame->ip),
                .stack = vm->stack,
                .depth = (int)(vm->sp - vm->stack),
                .frames = vm->frame_count,
                .cycles = vm->cycles,
                .heap_used = vm->heap.used_bytes,
                .heap_size = vm->heap.cap,
            };
            if (!vm->step_hook(vm, &st, vm->step_ud)) {
                jl_error(vm, "stopped by the host");
                goto error;
            }
        }
        uint8_t op = READ_BYTE();
        if (++vm->cycles > vm->max_cycles && vm->max_cycles != 0) {
            jl_error(vm, "stopped after %llu instructions. Is there a loop that "
                         "never ends?", (unsigned long long)vm->max_cycles);
            goto error;
        }
        if (op < OP_COUNT) vm->op_counts[op]++;

        switch (op) {
        case OP_CONST: {
            Value v = CHUNK()->consts[READ_U16()];
            PUSH(v);
            break;
        }
        case OP_NIL: PUSH(nil_val()); break;
        case OP_TRUE: PUSH(bool_val(true)); break;
        case OP_FALSE: PUSH(bool_val(false)); break;
        case OP_POP: vm->sp--; break;
        case OP_DUP2: {
            Value a = PEEK(1), b = PEEK(0);
            PUSH(a);
            PUSH(b);
            break;
        }

        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();
            PUSH(frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL: frame->slots[READ_BYTE()] = PEEK(0); break;
        case OP_GET_GLOBAL: {
            Global *g = &vm->globals[READ_BYTE()];
            if (!g->defined) {
                jl_error(vm, "'%s' is used before it is given a value", g->name);
                goto error;
            }
            PUSH(g->value);
            break;
        }
        case OP_SET_GLOBAL: {
            Global *g = &vm->globals[READ_BYTE()];
            if (!g->defined) {
                jl_error(vm, "'%s' is assigned before its 'let' has run", g->name);
                goto error;
            }
            g->value = PEEK(0);
            break;
        }
        case OP_DEF_GLOBAL: {
            Global *g = &vm->globals[READ_BYTE()];
            g->value = POP();
            g->defined = true;
            break;
        }

        /* Integer maths wraps around at 32 bits, like a real CPU.
         * The casts through uint32_t make that well-defined in C. */
        case OP_ADD: { INT_OPERANDS(OP_ADD); PUSH(int_val((int32_t)((uint32_t)a + (uint32_t)b))); break; }
        case OP_SUB: { INT_OPERANDS(OP_SUB); PUSH(int_val((int32_t)((uint32_t)a - (uint32_t)b))); break; }
        case OP_MUL: { INT_OPERANDS(OP_MUL); PUSH(int_val((int32_t)((uint32_t)a * (uint32_t)b))); break; }
        case OP_DIV: {
            INT_OPERANDS(OP_DIV);
            if (b == 0) {
                jl_error(vm, "division by zero");
                goto error;
            }
            PUSH(int_val((a == INT32_MIN && b == -1) ? INT32_MIN : a / b));
            break;
        }
        case OP_MOD: {
            INT_OPERANDS(OP_MOD);
            if (b == 0) {
                jl_error(vm, "modulo by zero");
                goto error;
            }
            PUSH(int_val((a == INT32_MIN && b == -1) ? 0 : a % b));
            break;
        }
        case OP_NEG: {
            if (PEEK(0).type != VAL_INT) {
                jl_error(vm, "can't negate a %s", value_type_name(PEEK(0)));
                goto error;
            }
            int32_t a = POP().as.i;
            PUSH(int_val((int32_t)(0u - (uint32_t)a)));
            break;
        }
        case OP_BAND: { INT_OPERANDS(OP_BAND); PUSH(int_val(a & b)); break; }
        case OP_BOR: { INT_OPERANDS(OP_BOR); PUSH(int_val(a | b)); break; }
        case OP_BXOR: { INT_OPERANDS(OP_BXOR); PUSH(int_val(a ^ b)); break; }
        case OP_BNOT: {
            if (PEEK(0).type != VAL_INT) {
                jl_error(vm, "can't use '~' on a %s", value_type_name(PEEK(0)));
                goto error;
            }
            int32_t a = POP().as.i;
            PUSH(int_val(~a));
            break;
        }
        case OP_SHL:
        case OP_SHR: {
            INT_OPERANDS(op);
            if (b < 0 || b > 31) {
                jl_error(vm, "shift amount %d is out of range (it must be 0..31)", (int)b);
                goto error;
            }
            int32_t r;
            if (op == OP_SHL) r = (int32_t)((uint32_t)a << b);
            else r = a < 0 ? ~(~a >> b) : a >> b; /* arithmetic shift */
            PUSH(int_val(r));
            break;
        }

        case OP_EQ:
        case OP_NE: {
            Value b = POP(), a = POP();
            int eq = values_equal(vm, a, b);
            if (eq < 0) goto error;
            PUSH(bool_val(op == OP_EQ ? eq == 1 : eq == 0));
            break;
        }
        case OP_LT: { INT_OPERANDS(OP_LT); PUSH(bool_val(a < b)); break; }
        case OP_LE: { INT_OPERANDS(OP_LE); PUSH(bool_val(a <= b)); break; }
        case OP_GT: { INT_OPERANDS(OP_GT); PUSH(bool_val(a > b)); break; }
        case OP_GE: { INT_OPERANDS(OP_GE); PUSH(bool_val(a >= b)); break; }
        case OP_NOT: { Value v = POP(); PUSH(bool_val(!truthy(v))); break; }
        case OP_BOOL: { Value v = POP(); PUSH(bool_val(truthy(v))); break; }

        case OP_JUMP: {
            uint16_t off = READ_U16();
            frame->ip += off;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            uint16_t off = READ_U16();
            Value cond = POP();
            if (!truthy(cond)) frame->ip += off;
            break;
        }
        case OP_LOOP: {
            uint16_t off = READ_U16();
            frame->ip -= off;
            break;
        }

        case OP_CALL: {
            int argc = READ_BYTE();
            if (!call_value(vm, PEEK(argc), argc)) goto error;
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }
        case OP_RETURN: {
            Value result = POP();
            vm->sp = frame->slots;
            vm->frame_count--;
            *vm->sp++ = result;
            if (vm->frame_count == base) return JL_OK;
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }

        case OP_INDEX_GET: {
            Value idx = POP(), target = POP();
            if (target.type == VAL_INT) {
                int32_t bit = 0;
                if (!check_bit(vm, idx, &bit)) goto error;
                PUSH(int_val((int32_t)(((uint32_t)target.as.i >> bit) & 1u)));
                break;
            }
            uint8_t *data;
            uint32_t len;
            int32_t i;
            if (!get_bytes(vm, target, &data, &len)) goto error;
            if (!check_index(vm, idx, len, value_type_name(target), &i)) goto error;
            PUSH(int_val(data[i]));
            break;
        }
        case OP_INDEX_SET: {
            Value v = POP(), idx = POP(), target = POP();
            if (target.type == VAL_INT) {
                jl_error(vm, "can't change a bit of a temporary value; store it in "
                             "a variable first, then write var[bit] = 1");
                goto error;
            }
            if (!store_byte(vm, target, idx, v)) goto error;
            PUSH(v);
            break;
        }
        case OP_INDEX_UPDATE: {
            Value v = POP(), idx = POP(), target = POP();
            if (target.type == VAL_INT) {
                int32_t bit = 0;
                if (!check_bit(vm, idx, &bit)) goto error;
                int32_t on;
                if (v.type == VAL_BOOL) on = v.as.b;
                else if (v.type == VAL_INT && (v.as.i == 0 || v.as.i == 1)) on = v.as.i;
                else {
                    jl_error(vm, "a bit can only be set to 0 or 1");
                    goto error;
                }
                uint32_t u = (uint32_t)target.as.i;
                u = on ? (u | (1u << bit)) : (u & ~(1u << bit));
                PUSH(int_val((int32_t)u));
                break;
            }
            if (!store_byte(vm, target, idx, v)) goto error;
            PUSH(target);
            break;
        }
        case OP_SLICE_GET: {
            Value lo = POP(), hi = POP(), target = POP();
            int32_t h, l;
            uint32_t mask;
            if (!check_range(vm, target, hi, lo, &h, &l, &mask)) goto error;
            PUSH(int_val((int32_t)(((uint32_t)target.as.i >> l) & mask)));
            break;
        }
        case OP_SLICE_UPDATE: {
            Value v = POP(), lo = POP(), hi = POP(), target = POP();
            int32_t h, l;
            uint32_t mask;
            if (!check_range(vm, target, hi, lo, &h, &l, &mask)) goto error;
            if (v.type != VAL_INT) {
                jl_error(vm, "bits can only be set to an int, got %s", value_type_name(v));
                goto error;
            }
            int width = h - l + 1;
            if (width < 32 && (v.as.i < 0 || (uint32_t)v.as.i > mask)) {
                jl_error(vm, "%d doesn't fit in %d bit%s (the max is %u)", (int)v.as.i,
                         width, width == 1 ? "" : "s", (unsigned)mask);
                goto error;
            }
            uint32_t u = (uint32_t)target.as.i;
            u = (u & ~(mask << l)) | (((uint32_t)v.as.i & mask) << l);
            PUSH(int_val((int32_t)u));
            break;
        }

        case OP_ON: {
            Value fn = POP(), name = POP();
            const char *s = "";
            uint32_t len = 0;
            jl_get_string(vm, name, &s, &len);
            for (int i = 0; i < vm->event_count; i++) {
                if (strcmp(vm->events[i].name, s) == 0) {
                    jl_error(vm, "there is already an 'on %s' handler", s);
                    goto error;
                }
            }
            if (vm->event_count == JL_EVENTS_MAX) {
                jl_error(vm, "too many event handlers (max %d)", JL_EVENTS_MAX);
                goto error;
            }
            EventHandler *h = &vm->events[vm->event_count++];
            snprintf(h->name, sizeof h->name, "%s", s);
            h->fn = fn;
            break;
        }

        default:
            jl_error(vm, "corrupt bytecode: unknown instruction %u", op);
            goto error;
        }
    }

error:
    report_error(vm);
    return JL_RUNTIME_ERROR;

#undef CHUNK
#undef READ_BYTE
#undef READ_U16
#undef PUSH
#undef POP
#undef PEEK
#undef INT_OPERANDS
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

JlResult jl_run_source(JlVM *vm, const char *source) {
    if (vm->frame_count != 0) return JL_RUNTIME_ERROR;
    vm->error_reported = false;
    int fi = jl_compile(vm, source);
    if (fi < 0) return JL_COMPILE_ERROR;

    reset_stack(vm);
    *vm->sp++ = fn_val((uint16_t)fi);
    if (!call_value(vm, vm->sp[-1], 0)) {
        report_error(vm);
        reset_stack(vm);
        return JL_RUNTIME_ERROR;
    }
    JlResult r = run(vm, 0);
    reset_stack(vm);
    return r;
}

JlResult jl_emit(JlVM *vm, const char *event) {
    bool outermost = vm->frame_count == 0;
    if (outermost) vm->error_reported = false;

    EventHandler *h = NULL;
    for (int i = 0; i < vm->event_count; i++) {
        if (strcmp(vm->events[i].name, event) == 0) h = &vm->events[i];
    }
    if (h == NULL) return JL_NO_HANDLER;

    Value *saved = vm->sp;
    int base = vm->frame_count;
    if (vm->sp >= vm->stack + JL_STACK_MAX) {
        jl_error(vm, "value stack overflow");
        report_error(vm);
        return JL_RUNTIME_ERROR;
    }
    *vm->sp++ = h->fn;
    if (!call_value(vm, h->fn, 0)) {
        report_error(vm);
        if (outermost) reset_stack(vm);
        return JL_RUNTIME_ERROR;
    }
    JlResult r = run(vm, base);
    if (r != JL_OK) {
        if (outermost) reset_stack(vm);
        return r;
    }
    vm->sp = saved;
    return JL_OK;
}

JlResult jl_disassemble_or_check(JlVM *vm, const char *source, bool print) {
    int first = vm->prog.fn_count;
    int fi = jl_compile(vm, source);
    if (fi < 0) return JL_COMPILE_ERROR;
    if (print) {
        for (int i = first; i < vm->prog.fn_count; i++) disassemble_function(vm, i);
    }
    return JL_OK;
}

int jl_report_leaks(JlVM *vm) {
    int leaks = 0;
    for (uint16_t i = 0; i < vm->heap.handle_cap; i++) {
        const HandleEntry *e = &vm->heap.handles[i];
        if (!e->live) continue;
        HeapRef ref = {i, e->gen};
        uint8_t *d;
        uint32_t len = 0;
        heap_get(&vm->heap, ref, &d, &len);
        vm_errf(vm, "leak: %s #%u (%u bytes) allocated at line %u was never freed\n",
                e->kind == BLOCK_STR ? "str" : "buf", (unsigned)i, (unsigned)len, (unsigned)e->alloc_line);
        leaks++;
    }
    if (leaks > 0) {
        vm_errf(vm, "hint: every alloc() needs a matching free(). On a device that "
                    "runs for days, a leak eventually uses up all memory.\n");
    }
    return leaks;
}

void jl_print_stats(JlVM *vm) {
    vm_errf(vm, "--- stats ---\n");
    vm_errf(vm, "instructions executed: %llu\n", (unsigned long long)vm->cycles);
    /* Show the 8 most used instructions. */
    bool shown[OP_COUNT] = {false};
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int op = 0; op < OP_COUNT; op++) {
            if (!shown[op] && vm->op_counts[op] > 0 &&
                (best < 0 || vm->op_counts[op] > vm->op_counts[best])) {
                best = op;
            }
        }
        if (best < 0) break;
        shown[best] = true;
        vm_errf(vm, "  %-14s %llu\n", opcode_name((uint8_t)best),
                (unsigned long long)vm->op_counts[best]);
    }
    vm_errf(vm, "heap peak: %u bytes, allocs: %u, frees: %u\n", (unsigned)vm->heap.peak_bytes,
            (unsigned)vm->heap.total_allocs, (unsigned)vm->heap.total_frees);
}
