# madcide for VS Code

Mad-C language support in VS Code, served by **madcide's own compiler** over the
Language Server Protocol. Every answer — diagnostics, outline, hover,
go-to-definition, find-references, syntax colour — comes from the same parse
handle madcide's own editor uses. Nothing in this extension analyses madc code.

Slice V6c-3a of the client-server arc
(`docs/plans/2026-09-14-v6c3a-vscode-extension-plan.md`).

## What it gives you today

| VS Code | madcide |
|---------|---------|
| Problems / squiggles | `parse_check`, republished on open, change and save |
| Outline, breadcrumbs, Go to Symbol | `parse_outline` |
| Hover | the code graph, falling back to `parse_enclosing` |
| Go to Definition | `graph_definition` |
| Find All References | `graph_references` |
| Syntax colour | `parse_spans`, as **semantic tokens** |
| **madcide: Run Command…** | any of madcide's ~110 registry commands — build, check, save, terminal, project, views, themes, outline, problems — through `workspace/executeCommand` |
| **madcide: Open Window** | the real madcide UI in a webview, on the **same session** |
| Status bar | madcide's own status line, pushed as `$/madc/message` |

The command palette is built from what the **server** advertises
(`executeCommandProvider`, itself built from madcide's registry), so a command
added to madcide appears here without touching this extension.

There is deliberately **no TextMate grammar**. madc's own classification reaches
the editor as semantic tokens, from the same parse as everything else; a grammar
would be a second classification of the same text, free to disagree with the
compiler about what a word is. If your theme shows no colour, check that
`editor.semanticHighlighting.enabled` is `true` or `configuredByTheme`.

### One session, two faces

**madcide: Open Window** needs a served port, and madcide's `--serve` is a
*session*. A second process would be a second session on the same file — two
carets, two undo histories, last save wins. So the extension asks for both
faces from **one** process:

```
madc tools/madcide/madcide.mad <file> --lsp --serve 127.0.0.1:0
```

LSP on stdio for the editor, api/ws/http on a loopback port for the window,
against the same buffers. The server reports the port in-protocol
(`$/madc/serve`), and the webview reaches it through `vscode.env.asExternalUri`,
so VS Code forwards it when the extension host is remote. **The port is
loopback-only and never exposed: madcide has no authentication or TLS.** Set
`madcide.window` to `false` to run without the second face.

Not here yet, and each its own slice: one shared session behind VS Code, an
agent's MCP client and a browser *at once*, by attaching to an already-running
session (V6c-3c). Also deferred: `workspace/symbol` (Ctrl+T), completion,
signature help, formatting, code actions and rename.

## Install

The extension spawns `madc` locally, so the extension host must sit where madc
is built. In a container that means **Remote-SSH**: open the container as a
remote, and VS Code runs the extension host inside it — no tunnel, no relay, no
ports.

```bash
cd tools/vscode-madcide
npm install
```

Then either:

- **Run it from source.** Open `tools/vscode-madcide` in VS Code and press F5 —
  an Extension Development Host opens with the extension loaded. Open any
  `.mad` file in it.
- **Package and install.** `npx @vscode/vsce package` produces `madcide-0.1.0.vsix`;
  install it with `code --install-extension madcide-0.1.0.vsix`, or from the
  Extensions view's `···` → *Install from VSIX…*.

## Settings

| Setting | Default | Meaning |
|---------|---------|---------|
| `madcide.madcPath` | *(empty)* | The `madc` binary. Empty = `bin/madc` under the workspace folder, else `madc` on PATH. |
| `madcide.madcidePath` | `tools/madcide/madcide.mad` | The madcide script madc runs. |
| `madcide.serverFile` | *(empty)* | The document madcide opens with. Empty = the active `.mad` editor, else the first `.mad` in the workspace. Other files join the session on demand. |
| `madcide.window` | `true` | Give the server its second face (`--serve 127.0.0.1:0`) so **Open Window** works against the same session. |
| `madcide.extraArgs` | `[]` | Extra arguments for madcide. |
| `madcide.trace.server` | `off` | Log the LSP traffic in the **madcide** output channel. |

madcide takes a document on its command line because it is a *session* over one,
not a stateless analyser; every other file the editor opens is added to that
same session by `textDocument/didOpen`.

If something does not start, open the **madcide** output channel — the extension
prints the binary it chose, the document it bound, the exact command line and
the working directory before it spawns anything.

## Testing the server without VS Code

`test/protocol_probe.js` is a real LSP client on Microsoft's own protocol
machinery (`vscode-jsonrpc`, the reader and writer `vscode-languageclient` uses
inside VS Code). It needs no VS Code, no display and no network:

```bash
cd tools/vscode-madcide
npm install
node test/protocol_probe.js          # uses ../../bin/madc
```

It sends a full VS Code-shaped `initialize` and drives a complete session —
both faces included: it runs `madcide.check`, watches the `$/madc/*`
notifications, and fetches the served page over HTTP from the same process. On
its first run it found three defects in the server — references highlighting the
call's first argument, `selectionRange` covering the whole declaration line, and
three conforming-client notifications landing on stderr as "unknown" — each now
pinned in `tests/testmadcide_lsp`, which is the gate. This probe is the
instrument; it is not part of `make -C src fulltest`, which runs with no network
and no node.

A conforming session prints **nothing on stderr but the serve banner** and
exits **0**.
