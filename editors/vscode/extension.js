// Zero-dependency VS Code extension client for the JLang Language Server.
const { spawn } = require("node:child_process");
const path = require("node:path");
const vscode = require("vscode");

let serverProc = null;
let nextId = 1;
const pending = new Map();
let diagnostics = null;
let outBuf = Buffer.alloc(0);

function sendLsp(msg) {
  if (!serverProc || serverProc.killed) return;
  const json = JSON.stringify(msg);
  const bytes = Buffer.byteLength(json, "utf8");
  serverProc.stdin.write(`Content-Length: ${bytes}\r\n\r\n${json}`);
}

function requestLsp(method, params) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    sendLsp({ jsonrpc: "2.0", id, method, params });
  });
}

function handleServerMessage(msg) {
  if (msg.id !== undefined && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) reject(new Error(msg.error.message));
    else resolve(msg.result);
    return;
  }
  if (msg.method === "textDocument/publishDiagnostics" && diagnostics) {
    const uri = vscode.Uri.parse(msg.params.uri);
    const items = (msg.params.diagnostics || []).map((d) => {
      const range = new vscode.Range(
        d.range.start.line,
        d.range.start.character,
        d.range.end.line,
        d.range.end.character,
      );
      return new vscode.Diagnostic(range, d.message, vscode.DiagnosticSeverity.Error);
    });
    diagnostics.set(uri, items);
  }
}

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection("jlang");
  context.subscriptions.push(diagnostics);

  const cfg = vscode.workspace.getConfiguration("jlang");
  const jlangPath = cfg.get("binaryPath") || "";
  const serverPath = path.join(context.extensionPath, "server.mjs");

  serverProc = spawn(process.execPath, [serverPath], {
    stdio: ["pipe", "pipe", "pipe"],
    env: { ...process.env, ...(jlangPath ? { JLANG_BIN: jlangPath } : {}) },
  });

  serverProc.stdout.on("data", (chunk) => {
    outBuf = Buffer.concat([outBuf, chunk]);
    while (true) {
      const headerEnd = outBuf.indexOf("\r\n\r\n");
      if (headerEnd === -1) break;
      const header = outBuf.slice(0, headerEnd).toString("ascii");
      const m = header.match(/Content-Length:\s*(\d+)/i);
      if (!m) {
        outBuf = outBuf.slice(headerEnd + 4);
        continue;
      }
      const len = parseInt(m[1], 10);
      const bodyStart = headerEnd + 4;
      if (outBuf.length < bodyStart + len) break;
      const body = outBuf.slice(bodyStart, bodyStart + len).toString("utf8");
      outBuf = outBuf.slice(bodyStart + len);
      try {
        handleServerMessage(JSON.parse(body));
      } catch {
        // Ignore malformed server output.
      }
    }
  });

  requestLsp("initialize", {
    processId: process.pid,
    rootUri: null,
    capabilities: {},
    initializationOptions: jlangPath ? { jlangPath } : {},
  }).then(() => {
    sendLsp({ jsonrpc: "2.0", method: "initialized", params: {} });
    for (const doc of vscode.workspace.textDocuments) {
      if (doc.languageId === "jlang") {
        sendLsp({
          jsonrpc: "2.0",
          method: "textDocument/didOpen",
          params: {
            textDocument: {
              uri: doc.uri.toString(),
              languageId: "jlang",
              version: doc.version,
              text: doc.getText(),
            },
          },
        });
      }
    }
  });

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId !== "jlang") return;
      sendLsp({
        jsonrpc: "2.0",
        method: "textDocument/didOpen",
        params: {
          textDocument: {
            uri: doc.uri.toString(),
            languageId: "jlang",
            version: doc.version,
            text: doc.getText(),
          },
        },
      });
    }),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId !== "jlang") return;
      sendLsp({
        jsonrpc: "2.0",
        method: "textDocument/didChange",
        params: {
          textDocument: { uri: e.document.uri.toString(), version: e.document.version },
          contentChanges: [{ text: e.document.getText() }],
        },
      });
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId !== "jlang") return;
      sendLsp({
        jsonrpc: "2.0",
        method: "textDocument/didClose",
        params: { textDocument: { uri: doc.uri.toString() } },
      });
    }),
    vscode.languages.registerHoverProvider("jlang", {
      async provideHover(doc, position) {
        const res = await requestLsp("textDocument/hover", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: position.line, character: position.character },
        });
        if (!res || !res.contents) return null;
        return new vscode.Hover(new vscode.MarkdownString(res.contents.value));
      },
    }),
    vscode.languages.registerDocumentFormattingEditProvider("jlang", {
      async provideDocumentFormattingEdits(doc) {
        const edits = await requestLsp("textDocument/formatting", {
          textDocument: { uri: doc.uri.toString() },
          options: { tabSize: 4, insertSpaces: true },
        });
        return (edits || []).map(
          (e) =>
            new vscode.TextEdit(
              new vscode.Range(
                e.range.start.line,
                e.range.start.character,
                e.range.end.line,
                e.range.end.character,
              ),
              e.newText,
            ),
        );
      },
    }),
    vscode.languages.registerCompletionItemProvider("jlang", {
      async provideCompletionItems(doc, position) {
        const items = await requestLsp("textDocument/completion", {
          textDocument: { uri: doc.uri.toString() },
          position: { line: position.line, character: position.character },
        });
        return (items || []).map((it) => {
          const ci = new vscode.CompletionItem(
            it.label,
            it.kind === 3 ? vscode.CompletionItemKind.Function : vscode.CompletionItemKind.Keyword,
          );
          if (it.documentation?.value) {
            ci.documentation = new vscode.MarkdownString(it.documentation.value);
          }
          return ci;
        });
      },
    }),
  );
}

function deactivate() {
  if (serverProc && !serverProc.killed) {
    sendLsp({ jsonrpc: "2.0", method: "exit", params: null });
    serverProc.kill();
  }
}

module.exports = { activate, deactivate };
