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

static bool bi_print(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    for (int i = 0; i < argc; i++) {
        if (i > 0) vm_write(vm, false, " ", 1);
        vm_print_value(vm, args[i], false);
    }
    vm_write(vm, false, "\n", 1);
    return true;
}

/* One {placeholder}: {} {d} {x} {b} {c}, optionally with a width: {x2} {b8}. */
static bool format_one(JlVM *vm, const char *spec, uint32_t n, Value v) {
    if (n == 0) {
        vm_print_value(vm, v, false);
        return true;
    }
    char kind = spec[0];
    int width = 0;
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
        len = snprintf(buf, sizeof buf, "%*d", width, (int)v.as.i);
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

static bool bi_printf(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    const char *f;
    uint32_t n;
    if (argc < 1 || args[0].type != VAL_STR) {
        return jl_error(vm, "printf(): the first argument must be a format string");
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
            if (j >= n) return jl_error(vm, "printf(): a '{' has no matching '}'");
            if (next >= argc) {
                return jl_error(vm, "printf(): the format has more {} than values passed");
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
        return jl_error(vm, "printf(): %d value(s) were passed but the format doesn't use them",
                        argc - next);
    }
    return true;
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
    if (v.type != VAL_BUF && v.type != VAL_STR) {
        return jl_error(vm, "free() expects a buf or a heap string, got %s",
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
    return jl_error(vm, "len() expects a buf or str, got %s", value_type_name(v));
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
        jl_outf(vm, "0x%04X %6u   %s #%u, %u bytes used, allocated at line %u\n",
                (unsigned)bi.off, (unsigned)bi.size, bi.kind == BLOCK_STR ? "str" : "buf",
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
    jl_register_native(vm, "alloc", bi_alloc, 1);
    jl_register_native(vm, "free", bi_free, 1);
    jl_register_native(vm, "len", bi_len, 1);
    jl_register_native(vm, "read16", bi_read16, 2);
    jl_register_native(vm, "read32", bi_read32, 2);
    jl_register_native(vm, "write16", bi_write16, 3);
    jl_register_native(vm, "write32", bi_write32, 3);
    jl_register_native(vm, "mem", bi_mem, 0);
    jl_register_native(vm, "memmap", bi_memmap, 0);
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
    jl_register_native(vm, "rgb", bi_rgb, 3);
}
