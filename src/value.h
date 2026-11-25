/*
 * value.h - what a JLang value looks like in memory.
 *
 * Every value is exactly 8 bytes:
 *
 *   byte 0     type      (nil, bool, int, str, buf, fn, built-in)
 *   byte 1     on_heap   (strings only: 1 = lives on the heap)
 *   bytes 2-3  padding
 *   bytes 4-7  payload   (the int, the bool, or a heap reference)
 *
 * Small, fixed-size values are what real hardware likes: they fit in
 * registers and copy fast. Big things (buffers, strings) live on the
 * JLang heap and the value only holds a *reference* to them.
 */
#ifndef JL_VALUE_H
#define JL_VALUE_H

#include "common.h"

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_INT,    /* 32-bit signed integer, wraps around like hardware */
    VAL_STR,    /* text: a string literal or a heap string */
    VAL_BUF,    /* raw bytes on the JLang heap, from alloc() */
    VAL_FN,     /* a function written in JLang */
    VAL_NATIVE, /* a built-in function written in C */
    VAL_ARR,    /* a list of values on the JLang heap */
    VAL_STRUCT, /* named fields on the JLang heap; tag is the struct type */
    VAL_RING    /* fixed-capacity ring buffer on the JLang heap */
} ValueType;

/*
 * A reference to a heap block. It is not a raw address: it is a slot
 * number in the heap's handle table plus a "generation" counter.
 * When a block is freed the generation changes, so any old reference
 * stops matching. That is how JLang catches use-after-free bugs that
 * would silently corrupt memory in C.
 */
typedef struct {
    uint16_t handle;
    uint16_t gen;
} HeapRef;

typedef struct {
    uint8_t type;
    uint8_t on_heap;
    uint16_t tag; /* VAL_STRUCT: which struct type (Program.structs) */
    union {
        bool b;
        int32_t i;
        uint16_t index; /* constant string, function or built-in index */
        HeapRef ref;
    } as;
} Value;

static inline Value nil_val(void) {
    Value v;
    v.type = VAL_NIL;
    v.on_heap = 0;
    v.tag = 0;
    v.as.i = 0;
    return v;
}

static inline Value bool_val(bool b) {
    Value v = nil_val();
    v.type = VAL_BOOL;
    v.as.b = b;
    return v;
}

static inline Value int_val(int32_t i) {
    Value v = nil_val();
    v.type = VAL_INT;
    v.as.i = i;
    return v;
}

static inline Value const_str_val(uint16_t index) {
    Value v = nil_val();
    v.type = VAL_STR;
    v.as.index = index;
    return v;
}

static inline Value heap_str_val(HeapRef ref) {
    Value v = nil_val();
    v.type = VAL_STR;
    v.on_heap = 1;
    v.as.ref = ref;
    return v;
}

static inline Value buf_val(HeapRef ref) {
    Value v = nil_val();
    v.type = VAL_BUF;
    v.on_heap = 1;
    v.as.ref = ref;
    return v;
}

static inline Value struct_val(HeapRef ref, uint16_t type) {
    Value v = nil_val();
    v.type = VAL_STRUCT;
    v.on_heap = 1;
    v.tag = type;
    v.as.ref = ref;
    return v;
}

static inline Value arr_val(HeapRef ref) {
    Value v = nil_val();
    v.type = VAL_ARR;
    v.on_heap = 1;
    v.as.ref = ref;
    return v;
}

static inline Value ring_val(HeapRef ref) {
    Value v = nil_val();
    v.type = VAL_RING;
    v.on_heap = 1;
    v.as.ref = ref;
    return v;
}

static inline Value fn_val(uint16_t index) {
    Value v = nil_val();
    v.type = VAL_FN;
    v.as.index = index;
    return v;
}

static inline Value native_val(uint16_t index) {
    Value v = nil_val();
    v.type = VAL_NATIVE;
    v.as.index = index;
    return v;
}

const char *value_type_name(Value v);

#endif
