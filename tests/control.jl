fn sign(n) {
    if n < 0 { return -1 } else if n == 0 { return 0 } else { return 1 }
}
print(sign(-5), sign(0), sign(9))          // expect: -1 0 1
let total = 0
for i in 0..10 {
    if i % 2 == 0 { continue }
    if i > 7 { break }
    total += i
}
print(total)                               // expect: 16
let n = 0
while n < 100 {
    n += 7
    if n > 20 { break }
}
print(n)                                   // expect: 21
// nested loops with locals, break only leaves the inner loop
let hits = 0
for a in 0..3 {
    let inner = 0
    for b in 0..10 {
        let tmp = b
        if tmp == 2 { break }
        inner += 1
    }
    hits += inner
}
print(hits)                                // expect: 6
// short circuit: the right side never runs
fn boom() { print("should not run") return true }
print(false && boom(), true || boom())     // expect: false true
print(0 || 0, 1 && 2, !0, !5)              // expect: false true true false
for i in 5..5 { print("empty range runs nothing") }
for i in 3..0 { print("backwards range runs nothing") }
print("done")                              // expect: done
