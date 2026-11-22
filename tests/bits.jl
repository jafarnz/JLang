print(0xF0 | 0x0F, 0xFF & 0x3C, 0b1100 ^ 0b1010, ~0)  // expect: 255 60 6 -1
print(1 << 31, -16 >> 2, 16 >> 2)          // expect: -2147483648 -4 4
// bitwise binds tighter than comparison (unlike C)
print(6 & 1 == 0, 5 & 1 == 1)              // expect: true true
let r = 0
r[0] = 1
r[3] = true
print(r, r[3], r[2])                       // expect: 9 1 0
r[7..4] = 0b1010
printf("{b8} {x2}\n", r, r)                // expect: 0b10101001 0xA9
print(r[7..4], r[7..0], r[31..0])          // expect: 10 169 169
r[3] = 0
r[0] ^= 1
print(r)                                   // expect: 160
let full = 0
full[31..0] = -1
print(full)                                // expect: -1
print(u8(-1), i8(255), u16(-1), i16(65535)) // expect: 255 -1 65535 -1
let flags = 0
flags[31] = 1
print(flags)                               // expect: -2147483648
