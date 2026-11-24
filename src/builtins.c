/*
 * builtins.c - the core built-in functions every JLang host has.
 *
 * Hardware-specific built-ins (pins, camera, ...) are NOT here. Each
 * host registers its own: see board_sim.c for the desktop simulator.
 */
#include <stdio.h>
#include <string.h>

#include "vm.h"

static bool get_buf(JlVM *vm, const char *fn, int arg_no, Value v,
                    uint8_t **data, uint32_t *len) {
    if (v.type != VAL_BUF) {
        return jl_error(vm, "%s(): argument %d must be a buf, got %s", fn, arg_no,
                        value_type_name(v));
    }
    if (heap_get(&vm->heap, v.as.ref, data, len) != HEAP_OK) {
        return vm_freed_error(vm, v.as.ref, "buf");
    }
    return true;
}

/* ---- output ------------------------------------------------------- */

/* Printing freed memory is a use-after-free like any other access. */
static bool check_alive(JlVM *vm, Value v) {
    if (v.type != VAL_BUF && v.type != VAL_ARR && v.type != VAL_STRUCT && !(v.type == VAL_STR && v.on_heap)) return true;
    uint8_t *d;
    uint32_t n;
    if (heap_get(&vm->heap, v.as.ref, &d, &n) == HEAP_OK) return true;
    return vm_freed_error(vm, v.as.ref, value_type_name(v));
}

static bool bi_print(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    for (int i = 0; i < argc; i++)
        if (!check_alive(vm, args[i])) return false;
    for (int i = 0; i < argc; i++) {
        if (i > 0) vm_write(vm, false, " ", 1);
        vm_print_value(vm, args[i], false);
    }
    vm_write(vm, false, "\n", 1);
    return true;
}

/* One {placeholder}: {} {d} {x} {b} {c}, optionally with a width: {x2} {b8}.
 * {d02} pads a decimal with zeros instead of spaces. */
static bool format_one(JlVM *vm, const char *spec, uint32_t n, Value v) {
    if (n == 0) {
        vm_print_value(vm, v, false);
        return true;
    }
    char kind = spec[0];
    int width = 0;
    bool zeros = n > 2 && spec[1] == '0';
    for (uint32_t i = 1; i < n; i++) {
        if (spec[i] < '0' || spec[i] > '9' || width > 32) {
            return jl_error(vm, "printf(): bad format {%.*s}", (int)n, spec);
        }
        width = width * 10 + (spec[i] - '0');
    }
    if (width > 32) return jl_error(vm, "printf(): width can be at most 32");
    if (kind != 'd' && kind != 'x' && kind != 'b' && kind != 'c') {
        return jl_error(vm, "printf(): unknown format {%.*s} (use {}, {d}, {x}, {b} or {c})",
                        (int)n, spec);
    }
    if (v.type != VAL_INT) {
        return jl_error(vm, "printf(): {%c} needs an int, got %s", kind, value_type_name(v));
    }
    uint32_t u = (uint32_t)v.as.i;
    char buf[48];
    int len = 0;
    switch (kind) {
    case 'd':
        len = zeros ? snprintf(buf, sizeof buf, "%0*d", width, (int)v.as.i)
                    : snprintf(buf, sizeof buf, "%*d", width, (int)v.as.i);
        break;
    case 'x':
        len = snprintf(buf, sizeof buf, "0x%0*X", width, (unsigned)u);
        break;
    case 'b': {
        int bits = 1;
        for (int i = 31; i > 0; i--) {
            if (u & (1u << i)) {
                bits = i + 1;
                break;
            }
        }
        if (width > bits) bits = width;
        buf[len++] = '0';
        buf[len++] = 'b';
        for (int i = bits - 1; i >= 0; i--) buf[len++] = (u >> i) & 1u ? '1' : '0';
        break;
    }
    case 'c':
        if (v.as.i < 0 || v.as.i > 255) {
            return jl_error(vm, "printf(): {c} needs a byte (0..255), got %d", (int)v.as.i);
        }
        buf[len++] = (char)v.as.i;
        break;
    }
    vm_write(vm, false, buf, (size_t)len);
    return true;
}

/* Writes fmt with its {} filled in, for printf() and fmt(). */
static bool format(JlVM *vm, const char *fn, int argc, Value *args) {
    for (int i = 1; i < argc; i++)
        if (!check_alive(vm, args[i])) return false;
    const char *f;
    uint32_t n;
    if (argc < 1 || args[0].type != VAL_STR) {
        return jl_error(vm, "%s(): the first argument must be a format string", fn);
    }
    if (!jl_get_string(vm, args[0], &f, &n)) return vm_freed_error(vm, args[0].as.ref, "str");

    int next = 1;
    uint32_t i = 0, lit = 0;
    while (i < n) {
        bool open_esc = f[i] == '{' && i + 1 < n && f[i + 1] == '{';
        bool close_esc = f[i] == '}' && i + 1 < n && f[i + 1] == '}';
        if (open_esc || close_esc) {
            vm_write(vm, false, f + lit, i - lit);
            vm_write(vm, false, f + i, 1);
            i += 2;
            lit = i;
            continue;
        }
        if (f[i] == '{') {
            vm_write(vm, false, f + lit, i - lit);
            uint32_t j = i + 1;
            while (j < n && f[j] != '}') j++;
            if (j >= n) return jl_error(vm, "%s(): a '{' has no matching '}'", fn);
            if (next >= argc) {
                return jl_error(vm, "%s(): the format has more {} than values passed", fn);
            }
            if (!format_one(vm, f + i + 1, j - i - 1, args[next++])) return false;
            i = j + 1;
            lit = i;
            continue;
        }
        i++;
    }
    vm_write(vm, false, f + lit, n - lit);
    if (next < argc) {
        return jl_error(vm, "%s(): %d value(s) were passed but the format doesn't use them",
                        fn, argc - next);
    }
    return true;
}

static bool bi_printf(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    return format(vm, "printf", argc, args);
}

/* fmt("score: {}", 42) returns the text printf would print, as a heap str. */
static bool bi_fmt(JlVM *vm, int argc, Value *args, Value *out) {
    vm_capture_begin(vm);
    bool ok = format(vm, "fmt", argc, args);
    if (!ok) {
        vm->capturing = false;
        return false;
    }
    return vm_capture_end(vm, out);
}

/* ---- memory ------------------------------------------------------- */

static bool bi_alloc(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t n;
    if (!jl_get_int(vm, "alloc", 1, args[0], &n)) return false;
    if (n <= 0) return jl_error(vm, "alloc(): size must be at least 1 byte, got %d", (int)n);

    HeapRef ref;
    HeapStatus st = heap_alloc(&vm->heap, (uint32_t)n, BLOCK_BUF,
                               (uint32_t)vm_current_line(vm), &ref);
    if (st == HEAP_NO_HANDLES) {
        return jl_error(vm, "alloc(): too many live blocks (max %u). Are you "
                            "forgetting to free()?", vm->heap.handle_cap);
    }
    if (st != HEAP_OK) {
        uint32_t free_bytes = heap_free_bytes(&vm->heap);
        uint32_t largest = heap_largest_free(&vm->heap);
        if (free_bytes >= (uint32_t)n) {
            return jl_error(vm, "out of memory: alloc(%d) failed. %u bytes are free in "
                                "total, but the largest free block is only %u bytes. "
                                "The heap is fragmented", (int)n, (unsigned)free_bytes, (unsigned)largest);
        }
        return jl_error(vm, "out of memory: alloc(%d) failed, only %u bytes are free",
                        (int)n, (unsigned)free_bytes);
    }
    *out = buf_val(ref);
    return true;
}

static bool bi_free(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    Value v = args[0];
    if (v.type == VAL_STR && !v.on_heap) {
        return jl_error(vm, "free(): string literals are not on the heap and can't be freed");
    }
    if (v.type != VAL_BUF && v.type != VAL_STR && v.type != VAL_ARR && v.type != VAL_STRUCT) {
        return jl_error(vm, "free() expects a buf, an array, a struct or a heap string, got %s",
                        value_type_name(v));
    }
    HeapStatus st = heap_release(&vm->heap, v.as.ref, (uint32_t)vm_current_line(vm));
    if (st == HEAP_OK) return true;
    const HandleEntry *e = heap_entry(&vm->heap, v.as.ref.handle);
    if (e != NULL && !e->live) {
        return jl_error(vm, "double free: this %s was already freed at line %u "
                            "(it was allocated at line %u)",
                        value_type_name(v), (unsigned)e->free_line, (unsigned)e->alloc_line);
    }
    return jl_error(vm, "double free: this %s was already freed", value_type_name(v));
}

static bool bi_len(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    Value v = args[0];
    if (v.type == VAL_BUF) {
        uint8_t *d;
        uint32_t len;
        if (!get_buf(vm, "len", 1, v, &d, &len)) return false;
        *out = int_val((int32_t)len);
        return true;
    }
    if (v.type == VAL_STR) {
        const char *s;
        uint32_t len;
        if (!jl_get_string(vm, v, &s, &len)) return vm_freed_error(vm, v.as.ref, "str");
        *out = int_val((int32_t)len);
        return true;
    }
    if (v.type == VAL_ARR) {
        Value *items;
        uint32_t n;
        if (!jl_get_array(vm, "len", v, &items, &n)) return false;
        *out = int_val((int32_t)n);
        return true;
    }
    return jl_error(vm, "len() expects a buf, str or array, got %s", value_type_name(v));
}

/* read16/read32/write16/write32: multi-byte values, little-endian
 * (lowest byte first), which is how ESP32 and x86 store them. */
static bool span(JlVM *vm, const char *fn, Value b, Value off, uint32_t size,
                 uint8_t **p) {
    uint8_t *data;
    uint32_t len;
    int32_t o;
    if (!get_buf(vm, fn, 1, b, &data, &len)) return false;
    if (!jl_get_int(vm, fn, 2, off, &o)) return false;
    if (o < 0 || (uint32_t)o + size > len) {
        return jl_error(vm, "%s(): bytes %d..%d are outside this buf (it has %u bytes)",
                        fn, (int)o, (int)(o + (int32_t)size - 1), (unsigned)len);
    }
    *p = data + o;
    return true;
}

static bool bi_read16(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    uint8_t *p;
    if (!span(vm, "read16", args[0], args[1], 2, &p)) return false;
    *out = int_val((int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)));
    return true;
}

static bool bi_read32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    uint8_t *p;
    if (!span(vm, "read32", args[0], args[1], 4, &p)) return false;
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[3] << 24);
    *out = int_val((int32_t)u);
    return true;
}

static bool bi_write16(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    uint8_t *p;
    int32_t v;
    if (!span(vm, "write16", args[0], args[1], 2, &p)) return false;
    if (!jl_get_int(vm, "write16", 3, args[2], &v)) return false;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    return true;
}

static bool bi_write32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    uint8_t *p;
    int32_t v;
    if (!span(vm, "write32", args[0], args[1], 4, &p)) return false;
    if (!jl_get_int(vm, "write32", 3, args[2], &v)) return false;
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    return true;
}

static bool bi_mem(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    (void)out;
    Heap *h = &vm->heap;
    uint32_t free_bytes = heap_free_bytes(h);
    jl_outf(vm, "heap: %u bytes\n", (unsigned)h->cap);
    jl_outf(vm, "  in use   : %u bytes in %u block(s)\n", (unsigned)h->used_bytes, (unsigned)h->live_blocks);
    jl_outf(vm, "  free     : %u bytes (largest free block: %u bytes)\n", (unsigned)free_bytes,
            (unsigned)(heap_largest_free(h)));
    jl_outf(vm, "  overhead : %u bytes (block headers + alignment)\n",
            (unsigned)(h->cap - free_bytes - h->used_bytes));
    jl_outf(vm, "  peak use : %u bytes, allocs: %u, frees: %u\n", (unsigned)h->peak_bytes,
            (unsigned)h->total_allocs, (unsigned)h->total_frees);
    return true;
}

static bool bi_memmap(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    (void)out;
    Heap *h = &vm->heap;
    jl_outf(vm, "offset   size   contents\n");
    uint32_t off = 0;
    BlockInfo bi;
    bool in_free = false;
    uint32_t free_start = 0, free_size = 0;
    while (heap_next_block(h, &off, &bi)) {
        if (!bi.used) {
            if (!in_free) {
                free_start = bi.off;
                free_size = bi.size;
                in_free = true;
            } else {
                free_size += HEAP_HDR + bi.size;
            }
            continue;
        }
        if (in_free) {
            jl_outf(vm, "0x%04X %6u   free\n", (unsigned)free_start, (unsigned)free_size);
            in_free = false;
        }
        if (bi.kind == BLOCK_STRUCT) {
            /* struct Reading #3: each field is one 8-byte value. */
            const StructDef *d = &vm->prog.structs[h->handles[bi.handle].tag];
            const StrConst *name = &vm->prog.strs[d->name];
            jl_outf(vm, "0x%04X %6u   struct %.*s #%u, allocated at line %u:", (unsigned)bi.off, (unsigned)bi.size,
                    (int)name->len, name->chars, (unsigned)bi.handle, (unsigned)(h->handles[bi.handle].alloc_line));
            for (int i = 0; i < d->field_count; i++) {
                const StrConst *f = &vm->prog.strs[d->fields[i]];
                jl_outf(vm, "%s %.*s @%d", i ? "," : "", (int)f->len, f->chars, i * (int)sizeof(Value));
            }
            jl_outf(vm, "\n");
            continue;
        }
        jl_outf(vm, "0x%04X %6u   %s #%u, %u bytes used, allocated at line %u\n",
                (unsigned)bi.off, (unsigned)bi.size, heap_kind_name(bi.kind),
                (unsigned)bi.handle, (unsigned)bi.len, (unsigned)(h->handles[bi.handle].alloc_line));
    }
    if (in_free) jl_outf(vm, "0x%04X %6u   free\n", (unsigned)free_start, (unsigned)free_size);
    return true;
}

/* ---- explicit-width conversions ------------------------------------ */

static bool bi_u8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t v;
    if (!jl_get_int(vm, "u8", 1, args[0], &v)) return false;
    *out = int_val((int32_t)(uint8_t)(uint32_t)v);
    return true;
}

static bool bi_i8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t v;
    if (!jl_get_int(vm, "i8", 1, args[0], &v)) return false;
    uint32_t low = (uint32_t)v & 0xFFu;
    *out = int_val(low >= 0x80u ? (int32_t)low - 0x100 : (int32_t)low);
    return true;
}

static bool bi_u16(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t v;
    if (!jl_get_int(vm, "u16", 1, args[0], &v)) return false;
    *out = int_val((int32_t)(uint16_t)(uint32_t)v);
    return true;
}

static bool bi_i16(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t v;
    if (!jl_get_int(vm, "i16", 1, args[0], &v)) return false;
    uint32_t low = (uint32_t)v & 0xFFFFu;
    *out = int_val(low >= 0x8000u ? (int32_t)low - 0x10000 : (int32_t)low);
    return true;
}

/* ---- misc ---------------------------------------------------------- */

static bool bi_assert(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 1 || argc > 2) return jl_error(vm, "assert() takes 1 or 2 arguments");
    bool ok = args[0].type == VAL_BOOL ? args[0].as.b
              : args[0].type == VAL_INT ? args[0].as.i != 0
                                        : args[0].type != VAL_NIL;
    if (ok) return true;
    if (argc == 2 && args[1].type == VAL_STR) {
        const char *s;
        uint32_t n;
        if (jl_get_string(vm, args[1], &s, &n)) {
            return jl_error(vm, "assertion failed: %.*s", (int)n, s);
        }
    }
    return jl_error(vm, "assertion failed");
}

static bool bi_cycles(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    *out = int_val((int32_t)(uint32_t)vm->cycles);
    return true;
}

static bool bi_emit(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    const char *s;
    uint32_t n;
    if (args[0].type != VAL_STR || !jl_get_string(vm, args[0], &s, &n)) {
        return jl_error(vm, "emit() expects an event name string, like emit(\"button1\")");
    }
    char name[32];
    if (n >= sizeof name) return jl_error(vm, "emit(): event name is too long");
    memcpy(name, s, n);
    name[n] = '\0';
    JlResult r = jl_emit(vm, name);
    if (r == JL_NO_HANDLER) {
        return jl_error(vm, "emit(): nothing handles '%s' (add: on %s { ... })", name, name);
    }
    return r == JL_OK; /* errors inside the handler were already reported */
}

/* ---- numbers ------------------------------------------------------ */

/* random(lo, hi): a whole number from lo to hi, both included. */
static bool bi_random(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t lo, hi;
    if (!jl_get_int(vm, "random", 1, args[0], &lo) || !jl_get_int(vm, "random", 2, args[1], &hi)) return false;
    if (lo > hi) return jl_error(vm, "random(): the first number (%d) must not be bigger than the second (%d)",
                                 (int)lo, (int)hi);
    if (vm->rng == 0) vm->rng = 0x9E3779B9u;
    uint32_t x = vm->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng = x;
    uint64_t span = (uint64_t)((int64_t)hi - lo) + 1;
    *out = int_val((int32_t)((int64_t)lo + (int64_t)(x % span)));
    return true;
}

/* map(v, from_lo, from_hi, to_lo, to_hi): rescale v from one range to another. */
static bool bi_map(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a[5];
    for (int i = 0; i < 5; i++)
        if (!jl_get_int(vm, "map", i + 1, args[i], &a[i])) return false;
    if (a[1] == a[2]) return jl_error(vm, "map(): the first range is empty (%d to %d)", (int)a[1], (int)a[2]);
    int64_t r = (int64_t)a[3] + ((int64_t)a[0] - a[1]) * ((int64_t)a[4] - a[3]) / ((int64_t)a[2] - a[1]);
    *out = int_val((int32_t)r);
    return true;
}

/* stop_timer(): stops the every block it is called from. */
static bool bi_stop_timer(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args, (void)out;
    if (!vm_stop_current_timer(vm)) return jl_error(vm, "stop_timer() only works inside an every or after block");
    return true;
}

/* clamp(v, lo, hi): v, kept between lo and hi. */
static bool bi_clamp(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t v, lo, hi;
    if (!jl_get_int(vm, "clamp", 1, args[0], &v) || !jl_get_int(vm, "clamp", 2, args[1], &lo) ||
        !jl_get_int(vm, "clamp", 3, args[2], &hi))
        return false;
    if (lo > hi) return jl_error(vm, "clamp(): the low limit (%d) is bigger than the high limit (%d)", (int)lo, (int)hi);
    *out = int_val(v < lo ? lo : v > hi ? hi : v);
    return true;
}

/* array(n) or array(n, value): a new array of n items, all 0 or all value. */
static bool bi_array(JlVM *vm, int argc, Value *args, Value *out) {
    int32_t n;
    if (argc < 1 || argc > 2) return jl_error(vm, "array() takes 1 or 2 arguments: the size and an optional value");
    if (!jl_get_int(vm, "array", 1, args[0], &n)) return false;
    if (n < 0 || n > 65535) return jl_error(vm, "array(): the size must be 0..65535, got %d", (int)n);
    Value *items;
    if (!jl_new_array(vm, (uint32_t)n, out, &items)) return false;
    if (argc == 2)
        for (int32_t i = 0; i < n; i++) items[i] = args[1];
    return true;
}

/* rgb(r, g, b): a colour as 0xRRGGBB, each part 0..255. */
static bool bi_rgb(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t c[3];
    static const char *names[3] = {"red", "green", "blue"};
    for (int i = 0; i < 3; i++) {
        if (!jl_get_int(vm, "rgb", i + 1, args[i], &c[i])) return false;
        if (c[i] < 0 || c[i] > 255)
            return jl_error(vm, "rgb(): %s must be 0..255, got %d", names[i], (int)c[i]);
    }
    *out = int_val(c[0] << 16 | c[1] << 8 | c[2]);
    return true;
}

/* ---- strings ------------------------------------------------------ */

static bool get_str(JlVM *vm, const char *fn, int arg_no, Value v, const char **s, uint32_t *len) {
    if (v.type != VAL_STR) {
        return jl_error(vm, "%s(): argument %d must be a string, got %s", fn, arg_no, value_type_name(v));
    }
    if (!jl_get_string(vm, v, s, len)) return vm_freed_error(vm, v.as.ref, "str");
    return true;
}

static bool bi_contains(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *s, *sub;
    uint32_t n, m;
    if (!get_str(vm, "contains", 1, args[0], &s, &n) || !get_str(vm, "contains", 2, args[1], &sub, &m)) return false;
    if (m == 0) {
        *out = bool_val(true);
        return true;
    }
    bool found = false;
    for (uint32_t i = 0; i + m <= n; i++) {
        if (memcmp(s + i, sub, m) == 0) {
            found = true;
            break;
        }
    }
    *out = bool_val(found);
    return true;
}

static bool bi_starts_with(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *s, *pre;
    uint32_t n, m;
    if (!get_str(vm, "starts_with", 1, args[0], &s, &n) || !get_str(vm, "starts_with", 2, args[1], &pre, &m))
        return false;
    *out = bool_val(m <= n && (m == 0 || memcmp(s, pre, m) == 0));
    return true;
}

static bool bi_ends_with(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *s, *suf;
    uint32_t n, m;
    if (!get_str(vm, "ends_with", 1, args[0], &s, &n) || !get_str(vm, "ends_with", 2, args[1], &suf, &m))
        return false;
    *out = bool_val(m <= n && (m == 0 || memcmp(s + n - m, suf, m) == 0));
    return true;
}

/* slice(s, start, end): bytes start..end (end exclusive), as a new heap str. */
static bool bi_slice(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *s;
    uint32_t n;
    int32_t start, end;
    if (!get_str(vm, "slice", 1, args[0], &s, &n) || !jl_get_int(vm, "slice", 2, args[1], &start) ||
        !jl_get_int(vm, "slice", 3, args[2], &end))
        return false;
    if (start < 0 || end < start || (uint32_t)end > n) {
        return jl_error(vm, "slice(): range %d..%d is outside this string (length %u)", (int)start, (int)end,
                        (unsigned)n);
    }
    return jl_new_string(vm, s + start, (uint32_t)(end - start), out);
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* trim(s): s without leading or trailing whitespace, as a new heap str. */
static bool bi_trim(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    const char *s;
    uint32_t n;
    if (!get_str(vm, "trim", 1, args[0], &s, &n)) return false;
    uint32_t lo = 0, hi = n;
    while (lo < hi && is_space(s[lo])) lo++;
    while (hi > lo && is_space(s[hi - 1])) hi--;
    return jl_new_string(vm, s + lo, hi - lo, out);
}

/* compact(): defragment the heap by sliding all live blocks together.
 * Returns the size in bytes of the single contiguous free block now available. */
static bool bi_compact(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    *out = int_val((int32_t)heap_compact(&vm->heap));
    return true;
}

static int32_t clamp_i32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static int64_t asr64(int64_t v, int k) {
    if (k == 0) return v;
    uint64_t u = (uint64_t)v;
    uint64_t s = u >> k;
    if (v < 0) s |= (~0ULL) << (64 - k);
    return (int64_t)s;
}

static bool bi_abs(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t x;
    if (!jl_get_int(vm, "abs", 1, args[0], &x)) return false;
    *out = int_val(x == INT32_MIN ? INT32_MAX : (x < 0 ? -x : x));
    return true;
}

static bool bi_min(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b;
    if (!jl_get_int(vm, "min", 1, args[0], &a) || !jl_get_int(vm, "min", 2, args[1], &b)) return false;
    *out = int_val(a < b ? a : b);
    return true;
}

static bool bi_max(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b;
    if (!jl_get_int(vm, "max", 1, args[0], &a) || !jl_get_int(vm, "max", 2, args[1], &b)) return false;
    *out = int_val(a > b ? a : b);
    return true;
}

static bool bi_sat_add(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b;
    if (!jl_get_int(vm, "sat_add", 1, args[0], &a) || !jl_get_int(vm, "sat_add", 2, args[1], &b)) return false;
    *out = int_val(clamp_i32((int64_t)a + (int64_t)b));
    return true;
}

static bool bi_sat_sub(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b;
    if (!jl_get_int(vm, "sat_sub", 1, args[0], &a) || !jl_get_int(vm, "sat_sub", 2, args[1], &b)) return false;
    *out = int_val(clamp_i32((int64_t)a - (int64_t)b));
    return true;
}

/* fix_mul(a, b, frac_bits): saturating (a * b) >> frac_bits using 64-bit intermediate. */
static bool bi_fix_mul(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b, frac;
    if (!jl_get_int(vm, "fix_mul", 1, args[0], &a) || !jl_get_int(vm, "fix_mul", 2, args[1], &b) ||
        !jl_get_int(vm, "fix_mul", 3, args[2], &frac))
        return false;
    if (frac < 0 || frac > 31) {
        return jl_error(vm, "fix_mul(): fractional bits must be 0..31, got %d", (int)frac);
    }
    int64_t prod = (int64_t)a * (int64_t)b;
    *out = int_val(clamp_i32(asr64(prod, frac)));
    return true;
}

/* fix_div(a, b, frac_bits): saturating (a << frac_bits) / b using 64-bit intermediate. */
static bool bi_fix_div(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t a, b, frac;
    if (!jl_get_int(vm, "fix_div", 1, args[0], &a) || !jl_get_int(vm, "fix_div", 2, args[1], &b) ||
        !jl_get_int(vm, "fix_div", 3, args[2], &frac))
        return false;
    if (frac < 0 || frac > 31) {
        return jl_error(vm, "fix_div(): fractional bits must be 0..31, got %d", (int)frac);
    }
    if (b == 0) return jl_error(vm, "fix_div(): division by zero");
    int64_t num = (int64_t)((uint64_t)(int64_t)a << frac);
    *out = int_val(clamp_i32(num / (int64_t)b));
    return true;
}

static bool bi_asm(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)args;
    (void)out;
    return jl_error(vm, "asm() only works in native code. Run the program with "
                        "`jlang emu` or build it with `jlang build`");
}

void jl_register_core(JlVM *vm) {
    jl_register_native(vm, "print", bi_print, -1);
    jl_register_native(vm, "printf", bi_printf, -1);
    jl_register_native(vm, "fmt", bi_fmt, -1);
    jl_register_native(vm, "alloc", bi_alloc, 1);
    jl_register_native(vm, "free", bi_free, 1);
    jl_register_native(vm, "len", bi_len, 1);
    jl_register_native(vm, "read16", bi_read16, 2);
    jl_register_native(vm, "read32", bi_read32, 2);
    jl_register_native(vm, "write16", bi_write16, 3);
    jl_register_native(vm, "write32", bi_write32, 3);
    jl_register_native(vm, "mem", bi_mem, 0);
    jl_register_native(vm, "memmap", bi_memmap, 0);
    jl_register_native(vm, "compact", bi_compact, 0);
    jl_register_native(vm, "u8", bi_u8, 1);
    jl_register_native(vm, "i8", bi_i8, 1);
    jl_register_native(vm, "u16", bi_u16, 1);
    jl_register_native(vm, "i16", bi_i16, 1);
    jl_register_native(vm, "assert", bi_assert, -1);
    jl_register_native(vm, "cycles", bi_cycles, 0);
    jl_register_native(vm, "emit", bi_emit, 1);
    jl_register_native(vm, "asm", bi_asm, 1);
    jl_register_native(vm, "random", bi_random, 2);
    jl_register_native(vm, "map", bi_map, 5);
    jl_register_native(vm, "clamp", bi_clamp, 3);
    jl_register_native(vm, "abs", bi_abs, 1);
    jl_register_native(vm, "min", bi_min, 2);
    jl_register_native(vm, "max", bi_max, 2);
    jl_register_native(vm, "sat_add", bi_sat_add, 2);
    jl_register_native(vm, "sat_sub", bi_sat_sub, 2);
    jl_register_native(vm, "fix_mul", bi_fix_mul, 3);
    jl_register_native(vm, "fix_div", bi_fix_div, 3);
    jl_register_native(vm, "stop_timer", bi_stop_timer, 0);
    jl_register_native(vm, "rgb", bi_rgb, 3);
    jl_register_native(vm, "array", bi_array, -1);
    jl_register_native(vm, "contains", bi_contains, 2);
    jl_register_native(vm, "starts_with", bi_starts_with, 2);
    jl_register_native(vm, "ends_with", bi_ends_with, 2);
    jl_register_native(vm, "slice", bi_slice, 3);
    jl_register_native(vm, "trim", bi_trim, 1);
}
