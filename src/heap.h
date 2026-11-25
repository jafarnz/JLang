/*
 * heap.h - JLang's own memory allocator.
 *
 * JLang does not call the host's malloc() for program data. It gets
 * ONE fixed block of bytes at startup (64 KB by default) and manages
 * it itself, the same way an allocator inside an operating system or
 * a microcontroller SDK does.
 *
 * Layout: the heap is a row of blocks, each with a 12-byte header:
 *
 *   +--------+----------------+--------+----------------+-----
 *   | header | payload        | header | payload        | ...
 *   +--------+----------------+--------+----------------+-----
 *
 * Allocation uses "first fit": walk the blocks from the start and take
 * the first free one that is big enough, splitting off the leftover.
 * Neighbouring free blocks are merged ("coalesced") so the heap does
 * not break into tiny useless pieces (fragmentation).
 *
 * Programs never see raw offsets. They get a HeapRef (handle + gen),
 * see value.h, which lets JLang detect use-after-free and double free.
 */
#ifndef JL_HEAP_H
#define JL_HEAP_H

#include "value.h"

enum { BLOCK_BUF = 0, BLOCK_STR = 1, BLOCK_ARR = 2, BLOCK_STRUCT = 3, BLOCK_RING = 4 };

/* 16-byte header at the start of a BLOCK_RING payload, followed by cap Values. */
typedef struct {
    uint32_t cap;
    uint32_t head;
    uint32_t count;
    uint32_t pad;
} RingHeader;

/* "buf", "str", "array", "struct" or "ring" */
const char *heap_kind_name(uint8_t kind);

typedef struct {
    uint32_t size;   /* payload capacity in bytes (multiple of 4) */
    uint32_t len;    /* bytes the program asked for */
    uint16_t handle; /* which handle owns this block (if used) */
    uint8_t used;    /* 1 = allocated, 0 = free */
    uint8_t kind;    /* BLOCK_BUF, BLOCK_STR or BLOCK_ARR */
} BlockHeader;

#define HEAP_HDR ((uint32_t)sizeof(BlockHeader))

/* One entry of the handle table: where a block lives and its history. */
typedef struct {
    uint32_t off;        /* offset of the block header in the heap */
    uint32_t alloc_line; /* source line that allocated it */
    uint32_t free_line;  /* source line that freed it (if freed) */
    uint16_t gen;        /* bumped on every free */
    uint8_t live;        /* 1 while the block is allocated */
    uint8_t kind;
    uint16_t tag;        /* BLOCK_STRUCT: the struct type, for memmap() */
} HandleEntry;

typedef struct {
    uint8_t *mem;
    uint32_t cap;
    HandleEntry *handles;
    uint16_t handle_cap;
    uint16_t next_handle; /* where the next handle search starts */

    /* statistics, shown by mem() */
    uint32_t used_bytes;   /* sum of requested lengths of live blocks */
    uint32_t live_blocks;
    uint32_t peak_bytes;
    uint32_t total_allocs;
    uint32_t total_frees;
} Heap;

typedef enum {
    HEAP_OK,
    HEAP_NO_MEMORY,  /* no free block is big enough */
    HEAP_NO_HANDLES, /* too many live blocks */
    HEAP_FREED,      /* reference points to a block that was freed */
    HEAP_INVALID     /* reference was never valid */
} HeapStatus;

typedef struct {
    uint32_t off;
    uint32_t size;
    uint32_t len;
    uint16_t handle;
    uint8_t used;
    uint8_t kind;
} BlockInfo;

bool heap_init(Heap *h, uint32_t cap, uint16_t handle_cap);
void heap_destroy(Heap *h);

HeapStatus heap_alloc(Heap *h, uint32_t len, uint8_t kind, uint32_t line,
                      HeapRef *out);
HeapStatus heap_release(Heap *h, HeapRef ref, uint32_t line);
HeapStatus heap_get(Heap *h, HeapRef ref, uint8_t **data, uint32_t *len);

const HandleEntry *heap_entry(const Heap *h, uint16_t handle);
uint32_t heap_free_bytes(const Heap *h);
uint32_t heap_largest_free(const Heap *h);

/* Slide all live blocks together at offset 0 and merge free space into one block.
 * Returns the size in bytes of the resulting contiguous free block. */
uint32_t heap_compact(Heap *h);

/* Walk every block. Start with *off = 0; returns false at the end. */
bool heap_next_block(const Heap *h, uint32_t *off, BlockInfo *info);

#endif
