// blink.jl - the "hello world" of hardware.
// Run: jlang run examples/blink.jl
// The simulator uses virtual time: delay(500) is instant, but millis()
// still advances, so you can see the timing your code would have.

let LED = 2
pin_mode(LED, "out")

for i in 0..5 {
    pin_write(LED, 1)
    delay(500)
    pin_write(LED, 0)
    delay(500)
}
printf("done after {} ms\n", millis())
