# JLang language reference

Version 0.1. Everything here is tested in [`tests/`](../tests).

## Contents

1. [Source files](#source-files)
2. [Values and types](#values-and-types)
3. [Numbers](#numbers)
4. [Operators](#operators)
5. [Variables](#variables)
6. [Control flow](#control-flow)
7. [Functions](#functions)
8. [Bits](#bits)
9. [Memory](#memory)
10. [Strings](#strings)
11. [Events](#events)
12. [Built-in functions](#built-in-functions)
13. [Board built-ins](#board-built-ins)
14. [Errors](#errors)
15. [Limits](#limits)

## Source files

- Files use the `.jlang` extension and UTF-8 text.
- Comments: `// to end of line` and `/* block */`.
- There are no semicolons. Statements end where the grammar says they end, so
  one statement per line is the convention.
- Blocks use `{ }`. Conditions do not need parentheses: `if x > 3 { ... }`.

## Values and types

| Type | Example | Notes |
|---|---|---|
| `nil` | `nil` | "no value" |
| `bool` | `true`, `false` | |
| `int` | `42`, `0xFF`, `0b1010`, `'A'` | 32-bit signed, wraps on overflow |
| `str` | `"hello"` | read-only bytes |
| `buf` | `alloc(16)` | mutable bytes on the JLang heap |
| `fn` | `fn add(a, b) { ... }` | functions are values |
| built-in | `print` | functions written in C by the host |

Every value is 8 bytes. `str` and `buf` values are references to data on the heap
(or, for string literals, in the program's constant table).

**Truthiness** follows C: `nil`, `false` and `0` are false; everything else is true.

## Numbers

```jlang
let a = 1_000_000      // underscores are ignored
let b = 0x7F           // hex
let c = 0b1010_0001    // binary
let d = 'A'            // character literal = its byte value (65)
let e = 0xFFFFFFFF     // = -1: literals above 2147483647 wrap to negative
```

Ints are **32-bit two's complement** and arithmetic **wraps**, exactly like a CPU
register: `2147483647 + 1 == -2147483648`. There are no floats; this is
deliberate, since many microcontrollers do integer maths only, and it keeps
results exact.

Division truncates toward zero (`-7 / 2 == -3`, `-7 % 2 == -1`). Dividing by zero
is an error.

## Operators

From lowest to highest precedence:

| Operators | Meaning |
|---|---|
| `\|\|` | logical or (short-circuit, gives a bool) |
| `&&` | logical and (short-circuit, gives a bool) |
| `==` `!=` | equality (any types; different types are never equal) |
| `<` `<=` `>` `>=` | comparison (ints) |
| `\|` | bitwise or |
| `^` | bitwise xor |
| `&` | bitwise and |
| `<<` `>>` | shifts (amount 0..31; `>>` keeps the sign) |
| `+` `-` | add, subtract |
| `*` `/` `%` | multiply, divide, remainder |
| `-` `!` `~` | negate, logical not, bitwise not (prefix) |
| `f(x)` `x[i]` `x[hi..lo]` | call, index, bit range |

> **Different from C:** bitwise operators bind *tighter* than comparisons, so
> `flags & 1 == 0` means `(flags & 1) == 0`. In C it means `flags & (1 == 0)`,
> a famous source of bugs.

Compound assignment: `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`.

## Variables

```jlang
let x = 5          // declare and assign
let y              // declare; starts as nil
x = x + 1
x += 1
```

- `let` at the top level of a file declares a **global**; anywhere else it declares
  a **local** that lives until the end of its `{ }` block.
- Redeclaring a name in the same block is an error.
- **Assignment is a statement, never an expression.** `if x = 3 { }` is an error
  ("did you mean == ?"), which removes a whole class of bugs.

## Control flow

```jlang
if temp > 30 {
    fan_on()
} else if temp < 20 {
    fan_off()
} else {
    print("ok")
}

while pin_read(BUTTON) == 1 {
    delay(10)
}

for i in 0..8 {        // 0, 1, ..., 7 (the end is exclusive)
    if i == 2 { continue }
    if i == 6 { break }
    print(i)
}
```

`for i in a..b` counts up by one from `a` while `i < b`. An empty or backwards
range runs zero times.

## Functions

```jlang
fn add(a, b) {
    return a + b
}

fn log_value(v) {
    printf("value = {}\n", v)   // no return: gives nil
}
```

- Functions are declared at the top level of a file and can call each other in
  any order.
- Calling with the wrong number of arguments is an error.
- Recursion works; the call depth is limited (64 by default) so a runaway
  recursion is reported instead of crashing.
- Functions are values: `let f = add` then `f(1, 2)`.

## Bits

Ints can be read and written **bit by bit**, using the same notation as
datasheets and Verilog.

```jlang
let r = 0
r[3] = 1             // set bit 3        -> 0b1000
r[3] = 0             // clear bit 3
r[0] ^= 1            // toggle bit 0
print(r[0])          // read bit 0 -> 0 or 1

r[7..4] = 0b1010     // write bits 7 down to 4
print(r[7..4])       // read them back -> 10
```

- Bit numbers are 0 (least significant) to 31.
- Ranges are written **high..low**, like `reg[7..4]`.
- Writing a value that doesn't fit in the range is an error (`16` doesn't fit in
  4 bits) instead of silently being cut off.
- Writing bits works on **variables**: `r[3] = 1` updates `r`.

Explicit-width conversions keep the low bits and re-interpret them:

| Call | Result | Example |
|---|---|---|
| `u8(v)` | low 8 bits, unsigned (0..255) | `u8(300) == 44` |
| `i8(v)` | low 8 bits, signed (-128..127) | `i8(200) == -56` |
| `u16(v)` | low 16 bits, unsigned | `u16(-1) == 65535` |
| `i16(v)` | low 16 bits, signed | `i16(40000) == -25536` |

## Memory

JLang gives every program a **fixed-size heap** (64 KB by default, set with
`--heap`), like the RAM of a microcontroller. You manage it yourself:

```jlang
let buf = alloc(8)      // 8 bytes, zeroed
buf[0] = 0xAA           // bytes are 0..255
print(len(buf))         // 8
free(buf)               // give it back
```

JLang checks every access:

| Mistake | What you get |
|---|---|
| reading after `free` | `use after free: this buf was freed at line 3 (it was allocated at line 2)` |
| freeing twice | `double free: this buf was already freed at line 3 ...` |
| index outside the buffer | `index 8 is out of bounds: this buf has 8 bytes, so valid indexes are 0..7` |
| storing 300 in a byte | `300 doesn't fit in a byte (0..255). Hint: use u8(v) or v & 0xFF ...` |
| forgetting to free | at exit: `leak: buf #0 (8 bytes) allocated at line 1 was never freed` |
| heap full / fragmented | `out of memory: alloc(150) failed. 152 bytes are free in total, but the largest free block is only 100 bytes. The heap is fragmented` |

How this works: a `buf` value isn't a raw address but a **handle** plus a
**generation number**. Freeing a block bumps its generation, so every old
reference stops matching. See [DESIGN.md](DESIGN.md#the-heap).

Multi-byte values are stored **little-endian** (lowest byte first), like the
ESP32 and x86:

```jlang
write32(buf, 0, 0x11223344)   // buf[0] == 0x44, buf[3] == 0x11
read32(buf, 0)                // 0x11223344
write16(buf, 4, 0xBEEF)
read16(buf, 4)
```

`mem()` prints heap usage; `memmap()` prints every block and free gap.

## Strings

```jlang
let s = "héllo\n"
print(len(s))          // 7: length in bytes (é is 2 bytes in UTF-8)
print(s[0])            // 104: strings index as bytes
```

- Escapes: `\n \t \r \0 \\ \" \' \xHH`.
- Strings are read-only. To build bytes, use a `buf`.
- There is no `+` for strings; print several values instead: `print("x =", x)`.
- String literals live in the program, not on the heap. Strings created at
  runtime by built-ins (like `ask()`) live on the heap and **must be freed**.

## Events

```jlang
on button {
    print("pressed")
}
```

An `on` handler runs every time the host fires that event: a button interrupt,
a timer, a message. On the desktop, fire events with `--emit button`, or from
code with `emit("button")`. Handlers are declared at the top level, one per
event name.

## Built-in functions

| Function | Description |
|---|---|
| `print(a, b, ...)` | print values separated by spaces, then a newline |
| `printf(fmt, ...)` | formatted print, **no** automatic newline (see below) |
| `alloc(n)` | new `buf` of `n` zeroed bytes |
| `free(x)` | free a `buf` or a heap `str` |
| `len(x)` | size of a `buf` or `str` in bytes |
| `read16(buf, off)` / `read32(buf, off)` | read a little-endian 16/32-bit value |
| `write16(buf, off, v)` / `write32(buf, off, v)` | write one |
| `u8(v)` `i8(v)` `u16(v)` `i16(v)` | explicit-width conversions |
| `mem()` | print heap statistics |
| `memmap()` | print the heap layout |
| `cycles()` | number of VM instructions executed so far |
| `assert(cond, msg)` | stop with an error if `cond` is false |
| `emit(name)` | fire an event from code |

### printf placeholders

| Placeholder | Output for 42 |
|---|---|
| `{}` | any value, as `print` shows it: `42` |
| `{d}` / `{d5}` | decimal / padded to width 5: `   42` |
| `{x}` / `{x4}` | hex / at least 4 digits: `0x2A` / `0x002A` |
| `{b}` / `{b8}` | binary / at least 8 digits: `0b101010` / `0b00101010` |
| `{c}` | the byte as a character: `*` |
| `{{` `}}` | literal braces |

## Board built-ins

These come from the **host**. The desktop `jlang` provides a simulated board;
device firmware provides the real ones with the same names.

| Function | Description |
|---|---|
| `pin_mode(pin, mode)` | mode is `"out"`, `"in"` or `"in_pullup"` |
| `pin_write(pin, v)` | set an output pin to 0/1 (or false/true) |
| `pin_read(pin)` | read an input pin (simulate with `--pin 4=0`) |
| `delay(ms)` | wait (virtual time in the simulator unless `--realtime`) |
| `millis()` | milliseconds since start |
| `capture()` | take a photo, returns a `buf` (JPEG) you must free |
| `ask(photo, question)` | ask the vision AI, returns a heap `str` you must free |
| `speak(text, lang)` | say text aloud; `lang` defaults to `"en"` |

The simulated `ask()` replies with the text from `--answer` and takes 1.8 s of
virtual time; `capture()` takes 150 ms; `speak()` about 60 ms per character.

## Errors

Compile errors show the line, the column and a pointer:

```text
error (line 3, col 1): 'prnt' is not defined - did you mean 'print'?
     3 | prnt(cuont)
       | ^
```

Runtime errors show the message and the chain of calls:

```text
runtime error (line 2): division by zero
  in average(), line 2
  in top-level code, line 5
```

Exit codes of `jlang`: `0` ok, `64` bad usage, `65` compile error, `66` file not
found, `70` runtime error.

## Limits

Defaults, all changeable when building JLang (see `src/common.h`):

| Limit | Default |
|---|---|
| heap | 64 KB (`--heap` at runtime) |
| live heap blocks | 1024 |
| value stack | 1024 values |
| call depth | 64 |
| globals (including built-ins) | 256 |
| locals per function | 256 |
| constants per function | 65536 |
| event handlers | 16 |
| instructions per run | unlimited (`--max-cycles <n>` to cap; the browser playground caps at 50 million) |
