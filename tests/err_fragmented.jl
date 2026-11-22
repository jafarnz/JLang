// args: --heap 400
// exit: 70
let a = alloc(100)
let b = alloc(100)
let c = alloc(100)
free(b)
let d = alloc(150)
// expect: runtime error (line 7): out of memory: alloc(150) failed. 152 bytes are free in total, but the largest free block is only 100 bytes. The heap is fragmented
// expect:   in top-level code, line 7
