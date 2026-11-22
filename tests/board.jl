// args: --pin 5=1
pin_mode(2, "out")
pin_mode(5, "in")
pin_mode(6, "in_pullup")
pin_mode(7, "in")
print(pin_read(5), pin_read(6), pin_read(7))  // expect: 1 1 0
pin_write(2, 1)                               // expect: [sim      0ms] pin 2 -> HIGH
pin_write(2, 1)
delay(1500)
pin_write(2, false)                           // expect: [sim   1500ms] pin 2 -> LOW
print(millis())                               // expect: 1500
let photo = capture()                         // expect: [sim   1650ms] camera: photo #1 captured (22 bytes)
printf("{x2} {x2}\n", photo[0], photo[1])     // expect: 0xFF 0xD8
let a = ask(photo, "what bus?")               // expect: [sim   1650ms] AI: sending photo + "what bus?"
// expect: [sim   3450ms] AI: answered
speak(a, "zh")                                // expect: [sim   3450ms] speaker (zh): "Bus 179 is arriving."
free(a)
free(photo)
