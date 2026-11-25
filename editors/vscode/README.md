# JLang for VS Code (and standalone LSP)

Syntax highlighting, bracket matching, real-time compile diagnostics (`jlang check`), hover documentation, autocompletion, and document formatting (`jlang fmt`) for `.jlang` files.

Zero external npm dependencies are required.

## Install in VS Code

```bash
cd editors/vscode
npx @vscode/vsce package        # creates jlang-0.2.0.vsix
code --install-extension jlang-0.2.0.vsix
```

Or copy this folder to `~/.vscode/extensions/jlang` and restart VS Code. Make sure `jlang` is on your `PATH` or set `jlang.binaryPath` in VS Code settings.

## Use the Language Server in Neovim, Helix, or Zed

Run `node editors/vscode/server.mjs` as a standard stdio LSP server. Set `JLANG_BIN` if `jlang` is not on your `PATH`.
