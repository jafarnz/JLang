// jlang.mjs - load jlang.wasm and run JLang code (browser or Node).
//
//   const jl = await loadJLang(wasmBytesOrResponse)
//   const { code, output } = jl.run(source, { mode: "run", emits: ["button"] })
//
// `output` is a list of { text, stream } pieces where stream is "out" or "err".
// `frames` is null, or an ArrayBuffer with every led_show() frame; parseFrames()
// splits it into { ms, w, h, rgb } objects.

const MODES = { run: 0, xray: 1, dis: 2, check: 3, riscv: 4, emu: 5 }

export async function loadJLang(source) {
  const stub = () => 0
  const imports = {
    wasi_snapshot_preview1: { fd_close: stub, fd_fdstat_get: stub, fd_seek: stub, fd_write: stub },
  }
  let instance
  if (typeof Response !== "undefined" && source instanceof Promise) source = await source
  if (typeof Response !== "undefined" && source instanceof Response) {
    const bytes = await source.arrayBuffer() // works even without an application/wasm header
    ;({ instance } = await WebAssembly.instantiate(bytes, imports))
  } else {
    ;({ instance } = await WebAssembly.instantiate(source, imports))
  }
  const ex = instance.exports
  ex._initialize()
  const enc = new TextEncoder()
  const dec = new TextDecoder()

  const cstr = (s) => {
    const bytes = enc.encode(String(s ?? ""))
    const ptr = ex.jlw_alloc(bytes.length + 1)
    const mem = new Uint8Array(ex.memory.buffer, ptr, bytes.length + 1)
    mem.set(bytes)
    mem[bytes.length] = 0
    return ptr
  }

  function run(src, opts = {}) {
    const args = [
      cstr(src),
      MODES[opts.mode ?? "run"] ?? 0,
      cstr((opts.emits ?? []).join("\n")),
      cstr(Object.entries(opts.pins ?? {}).map(([p, v]) => `${p}=${v ? 1 : 0}`).join(",")),
      cstr(opts.answer ?? ""),
    ]
    const code = ex.jlw_run(...args, opts.heap ?? 0, opts.stats ? 1 : 0)
    for (const p of [args[0], args[2], args[3], args[4]]) ex.jlw_free(p)

    const ptr = ex.jlw_output()
    const len = ex.jlw_output_len()
    const text = len ? dec.decode(new Uint8Array(ex.memory.buffer, ptr, len)) : ""
    const output = []
    let i = 0
    while (i < text.length) {
      const s = text.indexOf("\x02", i)
      if (s === -1) { output.push({ text: text.slice(i), stream: "out" }); break }
      if (s > i) output.push({ text: text.slice(i, s), stream: "out" })
      const e = text.indexOf("\x03", s)
      output.push({ text: text.slice(s + 1, e === -1 ? text.length : e), stream: "err" })
      i = e === -1 ? text.length : e + 1
    }
    // merge neighbours of the same stream
    const merged = []
    for (const o of output) {
      const last = merged[merged.length - 1]
      if (last && last.stream === o.stream) last.text += o.text
      else merged.push({ ...o })
    }
    // LED panel frames: a copy of the raw frame buffer (see parseFrames)
    const flen = ex.jlw_frames_len()
    const frames = flen ? new Uint8Array(ex.memory.buffer, ex.jlw_frames(), flen).slice().buffer : null
    return { code, output: merged, frames }
  }

  return { run, version: "0.1.0" }
}

// Splits the frame buffer from run() into { ms, w, h, rgb } frames, where rgb
// holds w * h pixels as R, G, B bytes, row by row from the top left.
export function parseFrames(buffer) {
  const out = []
  if (!buffer) return out
  const view = new DataView(buffer)
  let at = 0
  while (at + 8 <= buffer.byteLength) {
    const ms = view.getUint32(at, true)
    const w = view.getUint16(at + 4, true)
    const h = view.getUint16(at + 6, true)
    const size = w * h * 3
    out.push({ ms, w, h, rgb: new Uint8Array(buffer, at + 8, size) })
    at += 8 + size
  }
  return out
}
