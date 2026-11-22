/*
 * heap.c - first-fit allocator with block splitting and coalescing.
 * Read heap.h first for the big picture.
 */
#include <stdlib.h>
#include <string.h>

#include "heap.h"

static BlockHeader *hdr_at(const Heap *h, uint32_t off) {
    return (BlockHeader *)(void *)(h->mem + off);
}

/* Round up to a multiple of 4 so every header stays 4-byte aligned. */
static uint32_t align4(uint32_t n) {
    return (n + 3u) & ~3u;
}

bool heap_init(Heap *h, uint32_t cap, uint16_t handle_cap) {
    memset(h, 0, sizeof *h);
    cap &= ~3u;
    if (cap < HEAP_HDR + 4u || handle_cap == 0) return false;

    h->mem = malloc(cap);
    h->handles = calloc(handle_cap, sizeof(HandleEntry));
    if (h->mem == NULL || h->handles == NULL) {
        free(h->mem);
        free(h->handles);
        h->mem = NULL;
        h->handles = NULL;
        return false;
    }
    h->cap = cap;
    h->handle_cap = handle_cap;

    /* At the start the whole heap is one big free block. */
    BlockHeader *first = hdr_at(h, 0);
    first->size = cap - HEAP_HDR;
    first->len = 0;
    first->handle = 0;
    first->used = 0;
    first->kind = 0;
    return true;
}

void heap_destroy(Heap *h) {
    free(h->mem);
    free(h->handles);
    memset(h, 0, sizeof *h);
}

/* Merge every free block that directly follows the free block at off. */
static void coalesce_forward(Heap *h, uint32_t off) {
    BlockHeader *b = hdr_at(h, off);
    uint32_t next = off + HEAP_HDR + b->size;
    while (next < h->cap && !hdr_at(h, next)->used) {
        b->size += HEAP_HDR + hdr_at(h, next)->size;
        next = off + HEAP_HDR + b->size;
    }
}

HeapStatus heap_alloc(Heap *h, uint32_t len, uint8_t kind, uint32_t line,
                      HeapRef *out) {
    if (len > h->cap) return HEAP_NO_MEMORY;
    uint32_t need = align4(len == 0 ? 1u : len);

    /* 1. Find a free slot in the handle table. */
    uint16_t slot;
    for (slot = 0; slot < h->handle_cap; slot++) {
        if (!h->handles[slot].live) break;
    }
    if (slot == h->handle_cap) return HEAP_NO_HANDLES;

    /* 2. First fit: walk the blocks, take the first free one that fits. */
    uint32_t off = 0;
    while (off < h->cap) {
        BlockHeader *b = hdr_at(h, off);
        if (!b->used) {
            coalesce_forward(h, off);
            if (b->size >= need) {
                /* 3. Split: if the leftover can hold a header plus a few
                 *    bytes, turn it into a new free block. */
                uint32_t rest = b->size - need;
                if (rest >= HEAP_HDR + 4u) {
                    BlockHeader *tail = hdr_at(h, off + HEAP_HDR + need);
                    tail->size = rest - HEAP_HDR;
                    tail->len = 0;
                    tail->handle = 0;
                    tail->used = 0;
                    tail->kind = 0;
                    b->size = need;
                }
                b->used = 1;
                b->len = len;
                b->handle = slot;
                b->kind = kind;
                /* New memory is zeroed. C's malloc() does not do this,
                 * which is a classic source of bugs. */
                memset(h->mem + off + HEAP_HDR, 0, b->size);

                HandleEntry *e = &h->handles[slot];
                e->off = off;
                e->alloc_line = line;
                e->free_line = 0;
                e->live = 1;
                e->kind = kind;

                h->used_bytes += len;
                h->live_blocks++;
                h->total_allocs++;
                if (h->used_bytes > h->peak_bytes) h->peak_bytes = h->used_bytes;

                out->handle = slot;
                out->gen = e->gen;
                return HEAP_OK;
            }
        }
        off += HEAP_HDR + b->size;
    }
    return HEAP_NO_MEMORY;
}

static HeapStatus check_ref(const Heap *h, HeapRef ref) {
    if (ref.handle >= h->handle_cap) return HEAP_INVALID;
    const HandleEntry *e = &h->handles[ref.handle];
    if (!e->live || e->gen != ref.gen) return HEAP_FREED;
    return HEAP_OK;
}

HeapStatus heap_release(Heap *h, HeapRef ref, uint32_t line) {
    HeapStatus st = check_ref(h, ref);
    if (st != HEAP_OK) return st;

    HandleEntry *e = &h->handles[ref.handle];
    BlockHeader *b = hdr_at(h, e->off);
    h->used_bytes -= b->len;
    h->live_blocks--;
    h->total_frees++;

    b->used = 0;
    b->len = 0;
    coalesce_forward(h, e->off);

    /* Bump the generation so every old reference becomes invalid. */
    e->live = 0;
    e->gen++;
    e->free_line = line;
    return HEAP_OK;
}

HeapStatus heap_get(Heap *h, HeapRef ref, uint8_t **data, uint32_t *len) {
    HeapStatus st = check_ref(h, ref);
    if (st != HEAP_OK) return st;
    const HandleEntry *e = &h->handles[ref.handle];
    BlockHeader *b = hdr_at(h, e->off);
    *data = h->mem + e->off + HEAP_HDR;
    *len = b->len;
    return HEAP_OK;
}

const HandleEntry *heap_entry(const Heap *h, uint16_t handle) {
    if (handle >= h->handle_cap) return NULL;
    return &h->handles[handle];
}

bool heap_next_block(const Heap *h, uint32_t *off, BlockInfo *info) {
    if (*off >= h->cap) return false;
    const BlockHeader *b = hdr_at(h, *off);
    info->off = *off;
    info->size = b->size;
    info->len = b->len;
    info->handle = b->handle;
    info->used = b->used;
    info->kind = b->kind;
    *off += HEAP_HDR + b->size;
    return true;
}

uint32_t heap_free_bytes(const Heap *h) {
    uint32_t total = 0, off = 0;
    BlockInfo bi;
    while (heap_next_block(h, &off, &bi)) {
        if (!bi.used) total += bi.size;
    }
    return total;
}

uint32_t heap_largest_free(const Heap *h) {
    /* Neighbouring free blocks count as one, because alloc() would
     * merge them before using them. */
    uint32_t best = 0, run = 0, off = 0;
    bool in_run = false;
    BlockInfo bi;
    while (heap_next_block(h, &off, &bi)) {
        if (bi.used) {
            in_run = false;
            run = 0;
            continue;
        }
        run = in_run ? run + HEAP_HDR + bi.size : bi.size;
        in_run = true;
        if (run > best) best = run;
    }
    return best;
}
