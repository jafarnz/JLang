// exit: 70
let b = alloc(4)
b[4] = 1
// expect: runtime error (line 3): index 4 is out of bounds: this buf has 4 bytes, so valid indexes are 0..3
// expect:   in top-level code, line 3
