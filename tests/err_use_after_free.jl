// exit: 70
let b = alloc(4)
free(b)
print(b[0])
// expect: runtime error (line 4): use after free: this buf was freed at line 3 (it was allocated at line 2)
// expect:   in top-level code, line 4
