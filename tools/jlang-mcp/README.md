# JLang MCP Server

A zero-dependency Model Context Protocol (MCP) server that gives AI coding assistants direct access to the JLang compiler, simulator, test runner, RISC-V pipeline emulator, worst-case execution time (WCET) analyzer, and formatter.

## Tools exposed

- `jlang_check`: compile-check a `.jlang` file or inline snippet (syntax, exhaustive enum `match`, and `use` module resolution).
- `jlang_run`: execute a program on the simulated board with optional `--emit`, `--pin`, `--analog`, `--kv`, `--distance`, `--temperature`, `--vcd`, and `--stats`.
- `jlang_test`: run built-in `test "name" { ... }` blocks with strict heap leak checking.
- `jlang_wcet`: compute exact worst-case RISC-V cycle and microsecond bounds for the script and every `on` handler.
- `jlang_emu`: compile to native RV32IM machine code and run on the 5-stage pipeline simulator (with optional `--pipeline` or `--asm`).
- `jlang_fmt`: format JLang source code with zero-config layout.

## Requirements

- Node.js 18 or newer.
- The `jlang` binary in `build/jlang`, on `PATH`, or pointed to by the `JLANG_BIN` environment variable.

## Client configuration

Add this entry to your MCP client configuration (Claude Desktop, Claude Code, Cursor, or VS Code):

```json
{
  "mcpServers": {
    "jlang": {
      "command": "node",
      "args": ["/path/to/JLang/tools/jlang-mcp/index.mjs"],
      "env": {
        "JLANG_BIN": "/path/to/JLang/build/jlang"
      }
    }
  }
}
```
