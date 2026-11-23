#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"

void *jl_xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL && size != 0) {
        fprintf(stderr, "jlang: out of host memory\n");
        abort();
    }
    return p;
}

void chunk_write(Chunk *c, uint8_t byte, int line) {
    if (c->count == c->cap) {
        int cap = c->cap < 16 ? 16 : c->cap * 2;
        c->code = jl_xrealloc(c->code, (size_t)cap);
        c->lines = jl_xrealloc(c->lines, (size_t)cap * sizeof(int));
        c->cap = cap;
    }
    c->code[c->count] = byte;
    c->lines[c->count] = line;
    c->count++;
}

int chunk_add_const(Chunk *c, Value v) {
    /* Reuse an identical constant instead of storing it twice. */
    for (int i = 0; i < c->const_count; i++) {
        Value o = c->consts[i];
        if (o.type == v.type && o.on_heap == v.on_heap &&
            ((v.type == VAL_INT && o.as.i == v.as.i) ||
             ((v.type == VAL_STR || v.type == VAL_FN) &&
              o.as.index == v.as.index))) {
            return i;
        }
    }
    if (c->const_count >= 65536) return -1;
    if (c->const_count == c->const_cap) {
        c->const_cap = c->const_cap < 8 ? 8 : c->const_cap * 2;
        c->consts = jl_xrealloc(c->consts, (size_t)c->const_cap * sizeof(Value));
    }
    c->consts[c->const_count] = v;
    return c->const_count++;
}

void program_init(Program *p) {
    memset(p, 0, sizeof *p);
}

void program_free(Program *p) {
    for (int i = 0; i < p->fn_count; i++) {
        free(p->fns[i].name);
        free(p->fns[i].chunk.code);
        free(p->fns[i].chunk.lines);
        free(p->fns[i].chunk.consts);
        free(p->fns[i].chunk.bounds);
    }
    free(p->fns);
    for (int i = 0; i < p->str_count; i++) free(p->strs[i].chars);
    free(p->strs);
    memset(p, 0, sizeof *p);
}

int program_add_fn(Program *p, const char *name, int len) {
    if (p->fn_count >= 65536) return -1;
    if (p->fn_count == p->fn_cap) {
        p->fn_cap = p->fn_cap < 8 ? 8 : p->fn_cap * 2;
        p->fns = jl_xrealloc(p->fns, (size_t)p->fn_cap * sizeof(Function));
    }
    Function *f = &p->fns[p->fn_count];
    memset(f, 0, sizeof *f);
    f->name = jl_xrealloc(NULL, (size_t)len + 1);
    memcpy(f->name, name, (size_t)len);
    f->name[len] = '\0';
    return p->fn_count++;
}

int program_intern(Program *p, const char *chars, uint32_t len) {
    for (int i = 0; i < p->str_count; i++) {
        if (p->strs[i].len == len && memcmp(p->strs[i].chars, chars, len) == 0) {
            return i;
        }
    }
    if (p->str_count >= 65536) return -1;
    if (p->str_count == p->str_cap) {
        p->str_cap = p->str_cap < 8 ? 8 : p->str_cap * 2;
        p->strs = jl_xrealloc(p->strs, (size_t)p->str_cap * sizeof(StrConst));
    }
    StrConst *s = &p->strs[p->str_count];
    s->chars = jl_xrealloc(NULL, (size_t)len + 1);
    memcpy(s->chars, chars, len);
    s->chars[len] = '\0';
    s->len = len;
    return p->str_count++;
}
