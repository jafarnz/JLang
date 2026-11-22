// args: --emit go
// exit: 70
on go {
    print("going")
    emit("crash")
}
on crash {
    print(1 / 0)
}
// expect: going
// expect: runtime error (line 8): division by zero
// expect:   in handler 'on crash', line 8
// expect:   in handler 'on go', line 5
