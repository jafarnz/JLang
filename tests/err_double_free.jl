// exit: 70
fn release(x) { free(x) }
let b = alloc(4)
release(b)
release(b)
// expect: runtime error (line 2): double free: this buf was already freed at line 2 (it was allocated at line 3)
// expect:   in release(), line 2
// expect:   in top-level code, line 5
