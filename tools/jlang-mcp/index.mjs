#!/usr/bin/env node
/**
 * JLang MCP Server (Model Context Protocol, JSON-RPC 2.0 over stdio).
 * Zero external dependencies — requires only Node.js 18+ and the `jlang` binary.
 *
 * Exposes tools for AI assistants:
 *   - jlang_check  : check syntax, exhaustive enum matches, and modules
 *   - jlang_run    : run on the simulated board (with pins, sensors, events, VCD)
 *   - jlang_test   : run built-in test blocks with strict heap leak checking
 *   - jlang_wcet   : compute worst-case cycle and microsecond bounds on RV32IM
 *   - jlang_emu    : compile to native RV32IM and run on the 5-stage pipeline sim
 *   - jlang_fmt    : format JLang source code with zero-config layout
 */

import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const PROTOCOL_VERSION = "2024-11-05";

function findJlangBin() {
  if (process.env.JLANG_BIN) return process.env.JLANG_BIN;
  const repoLocal = resolve(__dirname, "../../build/jlang");
  if (existsSync(repoLocal)) return repoLocal;
  return "jlang";
}

function runJlang(args, stdinText = null, timeoutMs = 15000) {
  return new Promise((resolvePromise) => {
    const bin = findJlangBin();
    const child = execFile(
      bin,
      args,
      { timeout: timeoutMs, maxBuffer: 4 * 1024 * 1024 },
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

async function withSourceOrFile(params, fn) {
  if (params.path) {
    return fn(String(params.path));
  }
  if (typeof params.source !== "string") {
    throw new Error("Provide either 'path' (file on disk) or 'source' (inline JLang code).");
  }
  const dir = await mkdtemp(join(tmpdir(), "jlang-mcp-"));
  const file = join(dir, "snippet.jlang");
  try {
    await writeFile(file, params.source, "utf8");
    return await fn(file);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

const TOOLS = [
  {
    name: "jlang_check",
    description:
      "Compile-check a JLang file or inline source (verifies syntax, exhaustive enum match, and module imports without running).",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Path to a .jlang file." },
        source: { type: "string", description: "Inline JLang source code (alternative to path)." },
      },
    },
  },
  {
    name: "jlang_run",
    description:
      "Run a JLang program on the simulated board with optional simulated inputs, events, heap stats, and VCD waveform output.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Path to a .jlang file." },
        source: { type: "string", description: "Inline JLang source code." },
        emit: { type: "array", items: { type: "string" }, description: "Event names to emit after top-level code." },
        pins: { type: "array", items: { type: "string" }, description: "Pin states like '4=1'." },
        analog: { type: "array", items: { type: "string" }, description: "Analog inputs like '1=2048'." },
        kv: { type: "array", items: { type: "string" }, description: "Pre-populated KV pairs like 'mode=study'." },
        distance: { type: "integer", description: "Simulated ultrasonic distance in cm (2..400)." },
        temperature: { type: "integer", description: "Simulated I2C temperature in C (-55..125)." },
        answer: { type: "string", description: "Simulated ask() reply." },
        voice: { type: "string", description: "Simulated record() transcript." },
        vcd: { type: "string", description: "Path to write a GTKWave .vcd waveform file." },
        stats: { type: "boolean", description: "Include instruction count and heap usage stats." },
        max_cycles: { type: "integer", description: "Maximum bytecode instructions before halting." },
      },
    },
  },
  {
    name: "jlang_test",
    description:
      "Run built-in test \"name\" { ... } blocks in one or more .jlang files (or inline source), failing on assertion errors or heap leaks.",
    inputSchema: {
      type: "object",
      properties: {
        paths: { type: "array", items: { type: "string" }, description: "Paths to .jlang files." },
        source: { type: "string", description: "Inline JLang source containing test blocks." },
      },
    },
  },
  {
    name: "jlang_wcet",
    description:
      "Compute exact worst-case execution time (cycles and microseconds) for the script and every 'on' handler compiled to native RV32IM.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Path to a .jlang file." },
        source: { type: "string", description: "Inline JLang source code." },
        mhz: { type: "integer", description: "CPU clock frequency in MHz (default 160)." },
      },
    },
  },
  {
    name: "jlang_emu",
    description:
      "Compile a JLang program to native RV32IM machine code and run it on the built-in RISC-V emulator, optionally showing 5-stage pipeline stalls or generated assembly.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Path to a .jlang file." },
        source: { type: "string", description: "Inline JLang source code." },
        emit: { type: "array", items: { type: "string" }, description: "Events to fire." },
        pipeline: { type: "integer", description: "Show 5-stage pipeline diagram with N rows (0..64)." },
        asm: { type: "boolean", description: "Return generated RV32IM assembly instead of executing." },
        stats: { type: "boolean", description: "Print executed RISC-V instruction count." },
        max_cycles: { type: "integer", description: "Cycle cap." },
      },
    },
  },
  {
    name: "jlang_fmt",
    description: "Format JLang source code using the built-in zero-config formatter.",
    inputSchema: {
      type: "object",
      properties: {
        source: { type: "string", description: "JLang source code to format." },
        path: { type: "string", description: "Path to a .jlang file to format." },
        write: { type: "boolean", description: "When path is given, write formatted output in place." },
      },
    },
  },
];

function formatResult(res) {
  const parts = [];
  if (res.stdout) parts.push(res.stdout.trimEnd());
  if (res.stderr) parts.push(res.stderr.trimEnd());
  const text = parts.join("\n") || `(exit ${res.code})`;
  return {
    content: [{ type: "text", text }],
    isError: res.code !== 0,
  };
}

async function handleToolCall(name, args = {}) {
  switch (name) {
    case "jlang_check": {
      const res = await withSourceOrFile(args, (f) => runJlang(["check", f]));
      return formatResult(res);
    }
    case "jlang_run": {
      const res = await withSourceOrFile(args, (f) => {
        const cli = ["run", f];
        for (const e of args.emit ?? []) cli.push("--emit", String(e));
        for (const p of args.pins ?? []) cli.push("--pin", String(p));
        for (const a of args.analog ?? []) cli.push("--analog", String(a));
        for (const kv of args.kv ?? []) cli.push("--kv", String(kv));
        if (args.distance !== undefined) cli.push("--distance", String(args.distance));
        if (args.temperature !== undefined) cli.push("--temperature", String(args.temperature));
        if (args.answer !== undefined) cli.push("--answer", String(args.answer));
        if (args.voice !== undefined) cli.push("--voice", String(args.voice));
        if (args.vcd) cli.push("--vcd", String(args.vcd));
        if (args.stats) cli.push("--stats");
        if (args.max_cycles !== undefined) cli.push("--max-cycles", String(args.max_cycles));
        return runJlang(cli);
      });
      return formatResult(res);
    }
    case "jlang_test": {
      if (Array.isArray(args.paths) && args.paths.length > 0) {
        return formatResult(await runJlang(["test", ...args.paths.map(String)]));
      }
      const res = await withSourceOrFile(args, (f) => runJlang(["test", f]));
      return formatResult(res);
    }
    case "jlang_wcet": {
      const res = await withSourceOrFile(args, (f) => {
        const cli = ["wcet", f];
        if (args.mhz !== undefined) cli.push("--mhz", String(args.mhz));
        return runJlang(cli);
      });
      return formatResult(res);
    }
    case "jlang_emu": {
      const res = await withSourceOrFile(args, (f) => {
        const cli = ["emu", f];
        for (const e of args.emit ?? []) cli.push("--emit", String(e));
        if (args.pipeline !== undefined) cli.push("--pipeline", String(args.pipeline));
        if (args.asm) cli.push("--asm");
        if (args.stats) cli.push("--stats");
        if (args.max_cycles !== undefined) cli.push("--max-cycles", String(args.max_cycles));
        return runJlang(cli);
      });
      return formatResult(res);
    }
    case "jlang_fmt": {
      if (args.path) {
        const cli = ["fmt"];
        if (args.write) cli.push("-w");
        cli.push(String(args.path));
        return formatResult(await runJlang(cli));
      }
      if (typeof args.source !== "string") {
        throw new Error("Provide 'source' or 'path' to jlang_fmt.");
      }
      return formatResult(await runJlang(["fmt", "-"], args.source));
    }
    default:
      throw new Error(`Unknown tool: ${name}`);
  }
}

function send(msg) {
  process.stdout.write(`${JSON.stringify(msg)}\n`);
}

async function handleMessage(msg) {
  if (!msg || typeof msg !== "object" || msg.jsonrpc !== "2.0") return;
  const { id, method, params } = msg;

  // Notifications have no id and expect no reply.
  if (id === undefined || id === null) return;

  try {
    if (method === "initialize") {
      send({
        jsonrpc: "2.0",
        id,
        result: {
          protocolVersion: PROTOCOL_VERSION,
          capabilities: { tools: {} },
          serverInfo: { name: "jlang-mcp", version: "0.2.0" },
        },
      });
    } else if (method === "ping") {
      send({ jsonrpc: "2.0", id, result: {} });
    } else if (method === "tools/list") {
      send({ jsonrpc: "2.0", id, result: { tools: TOOLS } });
    } else if (method === "tools/call") {
      const result = await handleToolCall(params?.name, params?.arguments);
      send({ jsonrpc: "2.0", id, result });
    } else {
      send({
        jsonrpc: "2.0",
        id,
        error: { code: -32601, message: `Method not found: ${method}` },
      });
    }
  } catch (err) {
    send({
      jsonrpc: "2.0",
      id,
      error: { code: -32602, message: err instanceof Error ? err.message : String(err) },
    });
  }
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
rl.on("line", (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  let parsed;
  try {
    parsed = JSON.parse(trimmed);
  } catch {
    return;
  }
  handleMessage(parsed);
});
