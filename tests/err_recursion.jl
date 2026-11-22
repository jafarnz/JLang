// exit: 70
fn down(n) { return down(n + 1) }
down(0)
// expect: runtime error (line 2): stack overflow: more than 64 nested calls. Is there a recursion that never stops?
// expect:   in down(), line 2
// expect:   ... the same call repeated 62 more times
// expect:   in top-level code, line 3
