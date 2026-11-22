// packet.jl - building a binary message with manual memory.
// Run: jlang run examples/packet.jl
//
// Devices talk in bytes. This builds a small sensor packet:
//   [0]     start byte 0xAA
//   [1]     sensor id
//   [2..5]  reading, 32-bit little-endian
//   [6]     checksum (XOR of bytes 0..5)

fn checksum(buf, n) {
    let x = 0
    for i in 0..n {
        x ^= buf[i]
    }
    return x
}

let packet = alloc(7)
packet[0] = 0xAA
packet[1] = 3
write32(packet, 2, 123456)
packet[6] = checksum(packet, 6)

printf("packet:")
for i in 0..len(packet) {
    printf(" {x2}", packet[i])
}
printf("\n")

// The receiver side: check it, then decode.
assert(checksum(packet, 6) == packet[6], "checksum mismatch")
printf("sensor {} read {}\n", packet[1], read32(packet, 2))

memmap()
free(packet)      // delete this line and run again: JLang reports the leak
