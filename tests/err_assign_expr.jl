// exit: 65
let x = 1
if x = 2 { print(x) }
// expect: error (line 3, col 6): assignment must be a statement of its own, not part of an expression (did you mean == ?)
// expect:      3 | if x = 2 { print(x) }
// expect:        |      ^
