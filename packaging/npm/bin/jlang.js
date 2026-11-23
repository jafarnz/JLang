#!/usr/bin/env node
// jlang for Node.js: the real JLang compiler and VM, compiled to
// WebAssembly, so it runs anywhere Node does without a C compiler.
//
//   npx jlang-cli run hello.jlang
//   npm install -g jlang-cli && jlang run hello.jlang
//
// It covers run, check, dis, xray, emu, build and new. The native build
// (brew, the install script, or make) adds repl, wcet, svd and --vcd.
import fs from "node:fs"
import path from "node:path"
import { fileURLToPath } from "node:url"
import { loadJLang, parseFrames } from "../lib/jlang.mjs"

const here = path.dirname(fileURLToPath(import.meta.url))
const pkg = JSON.parse(fs.readFileSync(path.join(here, "..", "package.json"), "utf8"))

const USAGE = `JLang ${pkg.version} (WebAssembly build) - a small language for learning how hardware works

usage:
  jlang new <name>          create a starter project in ./<name>
  jlang run <file>          run a program on the simulated board
  jlang check <file>        check for errors without running
  jlang xray <file>         run, showing every instruction and the stack
  jlang dis <file>          print the compiled bytecode
  jlang emu <file>          compile to RISC-V and run it on the emulator
  jlang build <file>        print the RISC-V assembly (-o file.s writes it)

options:
  --emit <event>            fire an event after the program (repeatable)
  --pin <n>=<0|1>           what input pin n reads
  --analog <n>=<0..4095>    what analog(n) reads
  --distance <cm>           what distance() measures
  --temperature <C>         what the I2C temperature sensor reads
  --heap <bytes>            heap size
  --pipeline                with emu: show the 5-stage pipeline view
  --frames                  print each LED panel frame as text
  --stats                   print instruction counts and heap use

Docs: https://jafarniaz.com/jlang/docs`

const STARTER = `// A first JLang program. Run it with: jlang run main.jlang
let LED = 2

for i in 0..3 {
    turn_on(LED)
    delay(250)
    turn_off(LED)
    delay(250)
}
print("blinked 3 times in", millis(), "ms")

on button {
    print("button pressed!")
}
`

function main(argv) {
  const [command, ...rest] = argv
  if (!command || command === "help" || command === "--help" || command === "-h") {
    console.log(USAGE)
    return 0
  }
  if (command === "version" || command === "--version") {
    console.log(`jlang ${pkg.version} (WebAssembly)`)
    return 0
  }
  if (command === "new") return newProject(rest[0])

  const modes = { run: "run", check: "check", xray: "xray", dis: "dis", emu: "emu", build: "riscv" }
  if (!(command in modes)) {
    console.error(`jlang: unknown command '${command}'\n\n${USAGE}`)
    return 64
  }
  const opts = { mode: modes[command], emits: [], pins: {}, analog: {} }
  let file = null
  let out = null
  let frames = false
  for (let i = 0; i < rest.length; i++) {
    const a = rest[i]
    const value = () => {
      if (i + 1 >= rest.length) throw new Error(`${a} needs a value`)
      return rest[++i]
    }
    const pair = (v) => {
      const [k, x] = v.split("=").map(Number)
      if (!Number.isInteger(k) || !Number.isInteger(x)) throw new Error(`${a} takes n=value, like ${a} 4=0`)
      return [k, x]
    }
    if (a === "--emit") opts.emits.push(value())
    else if (a === "--pin") {
      const [k, x] = pair(value())
      opts.pins[k] = x
    } else if (a === "--analog") {
      const [k, x] = pair(value())
      opts.analog[k] = x
    } else if (a === "--distance") opts.distance = Number(value())
    else if (a === "--temperature") opts.temperature = Number(value())
    else if (a === "--heap") opts.heap = Number(value())
    else if (a === "--answer") opts.answer = value()
    else if (a === "--stats") opts.stats = true
    else if (a === "--frames") frames = true
    else if (a === "--pipeline") opts.mode = "pipeline"
    else if (a === "-o") out = value()
    else if (a.startsWith("--")) throw new Error(`unknown option '${a}'`)
    else file = a
  }
  if (!file) {
    console.error(`usage: jlang ${command} <file.jlang>`)
    return 64
  }
  let source
  try {
    source = fs.readFileSync(file, "utf8")
  } catch (err) {
    console.error(`jlang: can't read '${file}': ${err.message}`)
    return 66
  }

  return run(source, opts, { frames, out })
}

async function run(source, opts, { frames, out }) {
  const wasm = fs.readFileSync(path.join(here, "..", "lib", "jlang.wasm"))
  const jl = await loadJLang(wasm)
  const result = jl.run(source, opts)
  let text = ""
  for (const piece of result.output) {
    if (piece.stream === "err") process.stderr.write(piece.text)
    else text += piece.text
  }
  if (out && opts.mode === "riscv" && result.code === 0) {
    fs.writeFileSync(out, text)
    console.log(`wrote ${out}`)
  } else {
    process.stdout.write(text)
  }
  if (frames) {
    for (const f of parseFrames(result.frames)) {
      let s = `frame at ${f.ms} ms\n`
      for (let y = 0; y < f.h; y++) {
        for (let x = 0; x < f.w; x++) {
          const p = (y * f.w + x) * 3
          s += f.rgb[p] || f.rgb[p + 1] || f.rgb[p + 2] ? "#" : "."
        }
        s += "\n"
      }
      process.stdout.write(s)
    }
  }
  return result.code
}

function newProject(name) {
  if (!name) {
    console.error("usage: jlang new <name>")
    return 64
  }
  if (fs.existsSync(name)) {
    console.error(`jlang: '${name}' already exists`)
    return 64
  }
  fs.mkdirSync(name)
  fs.writeFileSync(path.join(name, "main.jlang"), STARTER)
  console.log(`created ${name}/main.jlang\nnext: cd ${name} && jlang run main.jlang --emit button`)
  return 0
}

try {
  const code = await main(process.argv.slice(2))
  process.exitCode = code
} catch (err) {
  console.error(`jlang: ${err.message}`)
  process.exitCode = 64
}
