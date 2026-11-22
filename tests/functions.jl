fn fib(n) {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(25))                             // expect: 75025
fn nothing() { }
print(nothing())                           // expect: nil
let g = 1
fn shadow(g) { return g * 10 }
print(shadow(5), g)                        // expect: 50 1
fn later() { return helper() + 1 }
fn helper() { return 41 }
print(later())                             // expect: 42
let f = fib
print(f(10), f)                            // expect: 55 <fn fib>
print(print)                               // expect: <built-in print>
