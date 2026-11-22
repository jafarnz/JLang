// iris.jl - behaviour script for Iris, an AI vision assistant for
// visually impaired commuters.
//
// On the device, this file lives on the SD card. The firmware (C)
// provides the hardware built-ins: capture(), ask(), speak(), pins.
// Everything Iris DOES is decided here, so it can be changed without
// recompiling or reflashing the firmware. A bug in this script is
// reported as an error; it can't crash the device.
//
// Test it on a laptop (camera, AI and speaker are simulated):
//   jlang run examples/iris.jl --emit bus_button
//   jlang run examples/iris.jl --emit read_button --answer "Exit B, Bugis MRT"

let LANG = "en"          // "en", "zh", "ms" or "ta"
let STATUS_LED = 21
let requests = 0

pin_mode(STATUS_LED, "out")

// Take a photo, ask the AI about it, say the answer, free the memory.
fn look_and_say(question) {
    pin_write(STATUS_LED, 1)           // LED on while working
    let start = millis()

    let photo = capture()              // buf on the JLang heap
    let answer = ask(photo, question)  // str on the JLang heap
    free(photo)                        // free as early as possible
    speak(answer, LANG)
    free(answer)

    pin_write(STATUS_LED, 0)
    requests += 1
    printf("request #{} took {} ms\n", requests, millis() - start)
}

on bus_button {
    look_and_say("Read the service number of the bus arriving. Reply only: Bus <number>, or: No bus.")
}

on read_button {
    look_and_say("Read the most important text in this photo aloud, briefly.")
}

on describe_button {
    look_and_say("Describe what is in front of me in one short sentence.")
}

print("Iris ready")
