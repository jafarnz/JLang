#!/usr/bin/env node
/**
 * JLang Language Server (LSP over stdio, zero external dependencies).
 *
 * Works with VS Code (via extension.js) and any LSP-capable editor
 * (Neovim, Helix, Zed, Emacs eglot). Requires Node.js 18+ and `jlang`.
 *
 * Features:
 *   - Real-time compile diagnostics via `jlang check` (with sibling temp file
 *     so relative `use "..."` imports resolve while editing unsaved buffers)
 *   - Hover documentation for keywords and built-in functions
 *   - Autocompletion for keywords and built-in functions
 *   - Document formatting via `jlang fmt -`
 */

import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));

const DOCS = {
  // Keywords
  let: "Declare a mutable variable: `let x = 0`.",
  const: "Declare a compile-time integer constant: `const LED = 2`.",
  fn: "Define a function: `fn add(a, b) { return a + b }`.",
  return: "Return a value from the current function (runs pending `defer` statements first).",
  if: "Conditional branch without required parentheses: `if x > 0 { ... }`.",
  else: "Alternative branch for `if`.",
  while: "Loop while a condition is true: `while n > 0 { ... }`.",
  for: "Counted loop over a half-open range: `for i in 0..10 { ... }`.",
  in: "Range separator in `for i in start..end`.",
  break: "Exit the innermost `while` or `for` loop (runs pending `defer` statements in exited scopes).",
  continue: "Skip to the next iteration of the innermost loop.",
  match: "Value or enum pattern match: `match mode { Mode.Idle => 0, _ => 1 }`. Exhaustive on enum values.",
  enum: "Define compile-time scoped integer constants: `enum Mode { Idle, Run, Sleep }`.",
  struct: "Define a fixed-field record type: `struct Reading { temp, ok }`.",
  defer: "Schedule a cleanup statement to run when leaving the enclosing block (LIFO order): `defer free(buf)`.",
  use: "Import another `.jlang` module once (relative to current file): `use \"driver.jlang\"`.",
  test: "Define an isolated unit test run by `jlang test`: `test \"name\" { assert(1 == 1) }`.",
  on: "Register an event handler: `on button { ... }`.",
  every: "Register a periodic timer: `every 500 ms { ... }`.",
  after: "Register a one-shot timer: `after 2 s { ... }`.",
  true: "Boolean true literal.",
  false: "Boolean false literal.",
  nil: "Absence-of-value literal.",

  // Core, memory, collections, wire packing
  print: "`print(a, b, ...)` — Print values separated by spaces followed by a newline.",
  printf: "`printf(fmt, ...)` — Formatted print supporting `{d}`, `{x}`, `{b}`, `{c}` and width specifiers like `{x4}`.",
  fmt: "`fmt(fmt, ...)` — Format into a newly allocated heap string (must be freed with `free()`).",
  alloc: "`alloc(n)` — Allocate a zeroed byte buffer of `n` bytes on the heap (must be freed with `free()`).",
  free: "`free(x)` — Free a heap buffer, array, struct, ring buffer, or heap string.",
  compact: "`compact()` — Slide live heap blocks together to eliminate fragmentation without invalidating handles.",
  len: "`len(x)` — Byte length of a string/buffer, element count of an array, or current item count of a ring buffer.",
  array: "`array(n[, val])` — Create an array of `n` items initialized to `0` (or `val`). Static `.bss` in native RISC-V.",
  ring: "`ring(cap)` — Allocate a fixed-capacity circular FIFO ring buffer on the heap (`free()` when done).",
  ring_push: "`ring_push(r, val)` — Push a value onto ring buffer `r`. Returns `false` if full.",
  ring_pop: "`ring_pop(r)` — Pop and return the oldest value from `r`, or `nil` if empty.",
  ring_peek: "`ring_peek(r)` — Return the oldest value in `r` without removing it, or `nil` if empty.",
  ring_full: "`ring_full(r)` — Return `true` when `r` has reached its capacity.",
  ring_empty: "`ring_empty(r)` — Return `true` when `r` has no items.",
  ring_cap: "`ring_cap(r)` — Return the fixed capacity of ring buffer `r`.",
  ring_clear: "`ring_clear(r)` — Empty ring buffer `r` in O(1) time without freeing its backing memory.",
  pack: "`pack(buf, off, fmt, ...)` — Pack integer fields into `buf` using a wire-format spec (`u1..u32`, `i1..i32`, `u16be`, `u16le`, `u24be`, `u32be`, `u32le`, `padN`). Returns bytes written.",
  unpack: "`unpack(buf, off, fmt)` — Unpack fields from `buf` according to a wire-format spec into an array (must be freed with `free()`).",
  read16: "`read16(buf, off)` — Read a big-endian 16-bit unsigned integer from `buf` at `off`.",
  read32: "`read32(buf, off)` — Read a big-endian 32-bit integer from `buf` at `off`.",
  write16: "`write16(buf, off, v)` — Write a big-endian 16-bit integer into `buf` at `off`.",
  write32: "`write32(buf, off, v)` — Write a big-endian 32-bit integer into `buf` at `off`.",
  mem: "`mem()` — Return bytes currently allocated on the heap.",
  memmap: "`memmap()` — Print a visual map of live blocks and free gaps in the heap.",

  // Math & fixed-point
  abs: "`abs(x)` — Absolute value of a 32-bit integer.",
  min: "`min(a, b)` — Smaller of two integers.",
  max: "`max(a, b)` — Larger of two integers.",
  clamp: "`clamp(x, lo, hi)` — Clamp `x` into `[lo, hi]`.",
  map: "`map(x, in_lo, in_hi, out_lo, out_hi)` — Linearly scale `x` from one integer range to another.",
  sat_add: "`sat_add(a, b)` — Saturating 32-bit signed addition (clamps instead of wrapping).",
  sat_sub: "`sat_sub(a, b)` — Saturating 32-bit signed subtraction (clamps instead of wrapping).",
  fix_mul: "`fix_mul(a, b, frac)` — Exact 64-bit intermediate fixed-point multiply `(a * b) >> frac` with saturation.",
  fix_div: "`fix_div(a, b, frac)` — Exact 64-bit intermediate fixed-point divide `(a << frac) / b` with saturation.",
  u8: "`u8(x)` — Mask `x` to 8-bit unsigned (`0..255`).",
  i8: "`i8(x)` — Sign-extend low 8 bits (`-128..127`).",
  u16: "`u16(x)` — Mask `x` to 16-bit unsigned (`0..65535`).",
  i16: "`i16(x)` — Sign-extend low 16 bits (`-32768..32767`).",
  assert: "`assert(cond[, msg])` — Halt with a runtime error if `cond` is false.",
  cycles: "`cycles()` — Bytecode instructions executed so far.",

  // Hardware & board I/O
  pin_mode: "`pin_mode(pin, mode)` — Configure GPIO `pin` as `\"in\"`, `\"out\"`, or `\"in_pullup\"`.",
  pin_write: "`pin_write(pin, level)` — Drive output GPIO `pin` low (`0`) or high (`1`).",
  pin_read: "`pin_read(pin)` — Read input GPIO `pin` (`0` or `1`).",
  delay: "`delay(ms)` — Pause for `ms` milliseconds.",
  millis: "`millis()` — Milliseconds since startup.",
  i2c_write: "`i2c_write(addr, reg, buf, off, n)` — Write `n` bytes from `buf[off..]` to I2C device `addr`.",
  i2c_read: "`i2c_read(addr, reg)` — Read one byte from I2C device `addr`, register `reg`.",
  i2c_read16: "`i2c_read16(addr, reg)` — Read a 16-bit big-endian value from I2C device `addr`.",
  i2c_scan: "`i2c_scan()` — Return an array of responding 7-bit I2C addresses.",
  spi_write: "`spi_write(buf, off, n)` — Write `n` bytes over SPI.",
  spi_transfer: "`spi_transfer(byte)` — Exchange one byte over SPI and return the received byte.",
  mmio_read32: "`mmio_read32(addr)` — Read a 32-bit hardware register at `addr` (native RISC-V).",
  mmio_write32: "`mmio_write32(addr, val)` — Write a 32-bit hardware register at `addr` (native RISC-V).",
  asm: "`asm(line)` — Emit inline RV32IM assembly instruction (native RISC-V).",
  kv_get: "`kv_get(key)` — Read a persistent key-value string from flash (or `nil`). Free returned string.",
  kv_set: "`kv_set(key, val)` — Store a persistent key-value string (up to 255 bytes) in flash.",
  kv_del: "`kv_del(key)` — Delete a persistent key from flash.",
  record_start: "`record_start()` — Start recording microphone audio (push-to-talk).",
  record_stop: "`record_stop()` — Stop recording and return transcribed heap string (must be freed).",
};

const documents = new Map(); // uri -> { text, version }
let jlangBin = null;

function findJlangBin() {
  if (jlangBin) return jlangBin;
  if (process.env.JLANG_BIN) return (jlangBin = process.env.JLANG_BIN);
  const repoLocal = resolve(__dirname, "../../build/jlang");
  if (existsSync(repoLocal)) return (jlangBin = repoLocal);
  return (jlangBin = "jlang");
}

function runJlang(args, stdinText = null, timeoutMs = 10000) {
  return new Promise((resolvePromise) => {
    const child = execFile(
      findJlangBin(),
      args,
      { timeout: timeoutMs, maxBuffer: 2 * 1024 * 1024 },
      (err, stdout, stderr) => {
        const code = err ? (typeof err.code === "number" ? err.code : 1) : 0;
        resolvePromise({ code, stdout: stdout ?? "", stderr: stderr ?? "" });
      },
    );
    if (stdinText !== null && child.stdin) {
      child.stdin.end(stdinText);
    }
  });
}

function sendMessage(obj) {
  const json = JSON.stringify(obj);
  const bytes = Buffer.byteLength(json, "utf8");
  process.stdout.write(`Content-Length: ${bytes}\r\n\r\n${json}`);
}

function parseDiagnostics(stderr, docText) {
  const diagnostics = [];
  const lines = docText.split("\n");
  for (const raw of stderr.split("\n")) {
    const m = raw.match(
      /^(?:(?:compile|runtime)\s+)?error \((?:[^,]+,\s*)?line (\d+)(?:,\s*col (\d+))?\):\s*(.+)$/,
    );
    if (!m) continue;
    const lineNum = Math.max(0, parseInt(m[1], 10) - 1);
    const colNum = m[2] ? Math.max(0, parseInt(m[2], 10) - 1) : 0;
    const lineText = lines[lineNum] ?? "";
    diagnostics.push({
      range: {
        start: { line: lineNum, character: colNum },
        end: { line: lineNum, character: Math.max(colNum + 1, lineText.length) },
      },
      severity: 1,
      source: "jlang",
      message: m[3].trim(),
    });
  }
  return diagnostics;
}

async function validateDocument(uri, text) {
  let checkPath = null;
  let cleanupDir = null;
  let cleanupFile = null;
  try {
    if (uri.startsWith("file://")) {
      const realPath = fileURLToPath(uri);
      const dir = dirname(realPath);
      if (existsSync(dir)) {
        // Sibling temp file preserves relative `use "..."` module resolution.
        checkPath = join(dir, `.jlang-lsp-${process.pid}-${Date.now()}.jlang`);
        cleanupFile = checkPath;
        await writeFile(checkPath, text, "utf8");
      }
    }
    if (!checkPath) {
      cleanupDir = await mkdtemp(join(tmpdir(), "jlang-lsp-"));
      checkPath = join(cleanupDir, "buffer.jlang");
      await writeFile(checkPath, text, "utf8");
    }
    const res = await runJlang(["check", checkPath]);
    const diagnostics = res.code === 0 ? [] : parseDiagnostics(res.stderr, text);
    sendMessage({
      jsonrpc: "2.0",
      method: "textDocument/publishDiagnostics",
      params: { uri, diagnostics },
    });
  } finally {
    if (cleanupFile) await rm(cleanupFile, { force: true }).catch(() => {});
    if (cleanupDir) await rm(cleanupDir, { recursive: true, force: true }).catch(() => {});
  }
}

function wordAt(text, line, character) {
  const lines = text.split("\n");
  const row = lines[line] ?? "";
  let start = character;
  let end = character;
  while (start > 0 && /[A-Za-z0-9_]/.test(row[start - 1])) start--;
  while (end < row.length && /[A-Za-z0-9_]/.test(row[end])) end++;
  return row.slice(start, end);
}

async function handleRequest(msg) {
  const { id, method, params } = msg;

  if (method === "initialize") {
    if (params?.initializationOptions?.jlangPath) {
      jlangBin = params.initializationOptions.jlangPath;
    }
    sendMessage({
      jsonrpc: "2.0",
      id,
      result: {
        capabilities: {
          textDocumentSync: 1, // Full document sync
          hoverProvider: true,
          documentFormattingProvider: true,
          completionProvider: { triggerCharacters: [".", "_"] },
        },
        serverInfo: { name: "jlang-lsp", version: "0.2.0" },
      },
    });
    return;
  }

  if (method === "shutdown") {
    sendMessage({ jsonrpc: "2.0", id, result: null });
    return;
  }

  if (method === "exit") {
    process.exit(0);
  }

  if (method === "textDocument/didOpen") {
    const doc = params.textDocument;
    documents.set(doc.uri, { text: doc.text, version: doc.version });
    await validateDocument(doc.uri, doc.text);
    return;
  }

  if (method === "textDocument/didChange") {
    const uri = params.textDocument.uri;
    const changes = params.contentChanges ?? [];
    if (changes.length > 0) {
      const text = changes[changes.length - 1].text;
      documents.set(uri, { text, version: params.textDocument.version });
      await validateDocument(uri, text);
    }
    return;
  }

  if (method === "textDocument/didSave") {
    const uri = params.textDocument.uri;
    const entry = documents.get(uri);
    if (entry) await validateDocument(uri, entry.text);
    return;
  }

  if (method === "textDocument/didClose") {
    const uri = params.textDocument.uri;
    documents.delete(uri);
    sendMessage({
      jsonrpc: "2.0",
      method: "textDocument/publishDiagnostics",
      params: { uri, diagnostics: [] },
    });
    return;
  }

  if (method === "textDocument/hover") {
    const doc = documents.get(params.textDocument.uri);
    const word = doc ? wordAt(doc.text, params.position.line, params.position.character) : "";
    const info = DOCS[word];
    sendMessage({
      jsonrpc: "2.0",
      id,
      result: info ? { contents: { kind: "markdown", value: info } } : null,
    });
    return;
  }

  if (method === "textDocument/completion") {
    const items = Object.entries(DOCS).map(([label, detail]) => ({
      label,
      kind: detail.startsWith("`") ? 3 : 14, // Function vs Keyword
      documentation: { kind: "markdown", value: detail },
    }));
    sendMessage({ jsonrpc: "2.0", id, result: items });
    return;
  }

  if (method === "textDocument/formatting") {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) {
      sendMessage({ jsonrpc: "2.0", id, result: [] });
      return;
    }
    const res = await runJlang(["fmt", "-"], doc.text);
    if (res.code !== 0 || res.stdout === doc.text) {
      sendMessage({ jsonrpc: "2.0", id, result: [] });
      return;
    }
    const lines = doc.text.split("\n");
    sendMessage({
      jsonrpc: "2.0",
      id,
      result: [
        {
          range: {
            start: { line: 0, character: 0 },
            end: { line: lines.length, character: 0 },
          },
          newText: res.stdout,
        },
      ],
    });
    return;
  }

  if (id !== undefined && id !== null) {
    sendMessage({
      jsonrpc: "2.0",
      id,
      error: { code: -32601, message: `Method not found: ${method}` },
    });
  }
}

// Standard LSP Content-Length framing parser over stdin.
let buf = Buffer.alloc(0);
process.stdin.on("data", (chunk) => {
  buf = Buffer.concat([buf, chunk]);
  while (true) {
    const headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd === -1) break;
    const header = buf.slice(0, headerEnd).toString("ascii");
    const m = header.match(/Content-Length:\s*(\d+)/i);
    if (!m) {
      buf = buf.slice(headerEnd + 4);
      continue;
    }
    const len = parseInt(m[1], 10);
    const bodyStart = headerEnd + 4;
    if (buf.length < bodyStart + len) break;
    const body = buf.slice(bodyStart, bodyStart + len).toString("utf8");
    buf = buf.slice(bodyStart + len);
    try {
      handleRequest(JSON.parse(body));
    } catch {
      // Ignore malformed JSON frames.
    }
  }
});
