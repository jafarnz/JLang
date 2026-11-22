# How JLang works

A guided tour of the implementation. Read the files in this order: it is the
same order the code flows.

## 1. The pipeline

```
"let x = 2 + 3"
      │
      ▼  lexer.c
LET  IDENT(x)  EQ  INT(2)  PLUS  INT(3)
      │
      ▼  compiler.c (one pass, no syntax tree)
CONST 2 · CONST 3 · ADD · DEF_GLOBAL x
      │
      ▼  vm.c (fetch, decode, execute)
globals: x = 5
```

Try it yourself: `jlang dis file.jl` prints the bytecode and `jlang xray file.jl`
runs it one instruction at a time, showing the stack.

## 2. Values (`value.h`)

Every value is 8 bytes: a type tag and a 4-byte payload. A fixed size means the
stack is just an array, values copy with one move, and there is no hidden
allocation when you do maths. Large data (buffers, runtime strings) lives on the
heap and the value holds a **reference**.

## 3. The lexer (`lexer.c`)

A hand-written scanner. Tokens point into the source instead of copying text.
Number values are decoded later by the compiler, which is where range errors
like "too big for 32 bits" are reported with a position.

## 4. The compiler (`compiler.c`)

**One pass.** As soon as a construct is recognised, its bytecode is emitted.
There is no syntax tree, which keeps memory use tiny: important when compiling
on a microcontroller.

**Pratt parsing** handles expressions: each token type has a prefix rule, an
infix rule and a precedence (the `rules[]` table). Adding an operator means
adding one row.

**Locals** live in stack slots. The compiler tracks which slot each name uses,
so at runtime a local read is just `GET_LOCAL 3`: no name lookup.

**Globals** are resolved to an index at compile time. After compiling, any name
that was used but never declared is reported, with a "did you mean" suggestion
from an edit-distance search.

**Bit assignment** (`r[7..4] = 5`): ints are values, not objects, so writing
bits must produce a *new* int and store it back into the variable. The compiler
remembers the last plain variable read; when `[...]` follows it directly and is
assigned, it emits `SLICE_UPDATE` followed by `SET_LOCAL`/`SET_GLOBAL`.

## 5. Bytecode (`chunk.h`)

Each function has a chunk: a byte array of instructions, a parallel array of
source line numbers (for error messages), and a constant table. Operands are 1
byte (slots, globals, argument counts) or 2 bytes big-endian (constants, jump
offsets).

## 6. The VM (`vm.c`)

`run()` is a `for (;;) switch (opcode)` loop: the software version of a CPU's
fetch-decode-execute cycle. Each call gets a **frame**: which function, its
instruction pointer, and where its locals start on the shared value stack.

Integer maths is done in `uint32_t` and cast back, which makes wrap-around
well-defined in C (signed overflow is undefined behaviour in C; JLang never
relies on it).

Every instruction increments `vm->cycles` and a per-opcode counter; that is what
`cycles()` and `--stats` report.

## 7. The heap (`heap.c`)

JLang never calls `malloc` for program data. It gets one fixed block at startup
and manages it:

```
┌────────┬───────────┬────────┬──────────────┬────────┬──────────────────┐
│ header │ buf #0    │ header │ (free)       │ header │ str #2           │
└────────┴───────────┴────────┴──────────────┴────────┴──────────────────┘
  12 B     payload
```

- **First fit:** walk the blocks, take the first free one that is big enough.
- **Split:** if the free block is much bigger, the rest becomes a new free block.
- **Coalesce:** neighbouring free blocks are merged, on free and while searching.
- **Fragmentation is visible:** `memmap()` shows the gaps, and "out of memory"
  errors say when there is enough free memory in total but no single block is
  big enough.

### Handles and generations

Programs never hold raw offsets. A `buf` value holds `(handle, generation)`.
The handle table maps a handle to the block's offset. On `free`, the entry is
marked dead and its generation is incremented, so:

- **use after free:** the value's generation no longer matches, so the access is refused.
- **double free:** the entry is already dead, so the error names both lines.
- the table also remembers the allocating line, which is how leak reports work.

Handles also make **compaction** possible in the future: blocks could be moved
to fix fragmentation, because only the table holds real offsets.

## 8. Built-ins and hosts

A built-in is a C function `bool fn(JlVM *vm, int argc, Value *args, Value *out)`.
The core ones are in `builtins.c`. Hardware ones belong to the **host**:
`board_sim.c` is the desktop host; device firmware registers its own versions of
the same functions. See [EMBEDDING.md](EMBEDDING.md).

## 9. Design choices

| Choice | Why |
|---|---|
| No floats | Exact results, matches integer-only MCUs, simpler VM |
| Manual memory, checked | Teaches how memory really works without silent corruption |
| No GC | Predictable timing, no pauses on a device |
| Assignment is a statement | Removes the `if (x = 1)` bug |
| Bitwise above comparison | Removes the `x & 1 == 0` bug |
| Fixed limits everywhere | Predictable memory use on small devices |
| Virtual time in the simulator | Fast, reproducible tests of timing logic |
