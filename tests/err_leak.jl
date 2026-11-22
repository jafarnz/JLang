let keep = alloc(10)
let msg = ask(capture(), "hi")
// expect: [sim    150ms] camera: photo #1 captured (22 bytes)
// expect: [sim    150ms] AI: sending photo + "hi"
// expect: [sim   1950ms] AI: answered
// expect: leak: buf #0 (10 bytes) allocated at line 1 was never freed
// expect: leak: buf #1 (22 bytes) allocated at line 2 was never freed
// expect: leak: str #2 (20 bytes) allocated at line 2 was never freed
// expect: hint: every alloc() needs a matching free(). On a device that runs for days, a leak eventually uses up all memory.
