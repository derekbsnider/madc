# madcide for VS Code

Mad-C language support in VS Code, served by **madc's own compiler** over the
Language Server Protocol: the extension spawns `madc --lsp <file>`. Every
answer — diagnostics, outline, hover, go-to-definition, find-references,
syntax colour — comes from madc's language services (`madc::lang`, one parse
door per document). Nothing in this extension analyses madc code.

Slice V6c-3a of the client-server arc
(`docs/plans/2026-09-14-v6c3a-vscode-extension-plan.md`); since split arc 1D
(`docs/plans/2026-10-09-madc-madcide-split-arc.md`) the server is madc's
(`madc --lsp`), not madcide's.

## What it gives you today

| VS Code | madc |
|---------|------|
| Problems / squiggles | the document's diagnostics, republished on open, change and save |
| Outline, breadcrumbs, Go to Symbol | the document's outline |
| Hover | the declaration under the cursor |
| Go to Definition / Find All References | the language services' definition / references |
| Syntax colour | the parse's classes, as **semantic tokens** |
| **madcide: Run Command…** | madc's `%` commands, through `workspace/executeCommand` (`madc.NAME`) |

The command palette is built from what the **server** advertises
(`executeCommandProvider`), so a command added to madc appears here without
touching this extension.

There is deliberately **no TextMate grammar**. madc's own classification reaches
the editor as semantic tokens, from the same parse as everything else; a grammar
would be a second classification of the same text, free to disagree with the
compiler about what a word is. If your theme shows no colour, check that
`editor.semanticHighlighting.enabled` is `true` or `configuredByTheme`.

Not here yet: `workspace/symbol` (Ctrl+T), completion, signature help,
formatting, code actions and rename.

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
| `madcide.serverFile` | *(empty)* | The document `madc --lsp` opens with (its project). Empty = the active `.mad` editor, else the first `.mad` in the workspace. Other files join on demand. |
| `madcide.extraArgs` | `[]` | Extra arguments for `madc --lsp`. |
| `madcide.trace.server` | `off` | Log the LSP traffic in the **madcide** output channel. |

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

Every line it prints is an answer from the server; read them against the
protocol. `tests/testlsp_stdio` and the `test_lsp` unit cases are the gates;
this probe is the instrument.
