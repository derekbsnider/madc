# V6c-3a — the VS Code extension (the LSP face's first real client)

> Slice of the client-server arc (design `2026-09-09-nexus-client-server-design.md`,
> V6 plan `2026-09-11-v6-transports-headless-plan.md` §V6c). Executes inline,
> like L4a–L4e. The battery rides the V6 RELEASE seam, never this slice.

**Goal:** ship `tools/vscode-madcide/` — the extension that points VS Code at
`madcide <file> --lsp` — and fix the LSP seat everywhere a real client
disagrees with `tests/testmadcide_lsp`.

**Spec:** V6 plan §V6c ("VS Code as the first-class api client" is the arc's
stated acceptance criterion); V6c-2 plan `2026-09-14-v6c2-lsp-adapter-plan.md`
for the seat this slice exercises.

**Architecture:** VS Code has no built-in way to point at a language server —
the client lives in extension code — so the extension is on the critical path
whatever transport is chosen later. v1 needs no socket work: the extension
spawns the server on stdio, exactly the deployment `testmadcide_lsp_stdio`
already covers. The extension contributes the `madc` language and NO TextMate
grammar: madc's own `parse_spans` is the one classifier, reaching the editor as
semantic tokens (a grammar would be a second, diverging classification of the
same text — `no-parallel-implementations.md`).

**Tech stack:** node 22 / npm 10 (in the container), `vscode-languageclient` 9
(LSP 3.17, the version that honours `positionEncoding`),
`vscode-languageserver-protocol` 3.17 for the headless probe.

## Global Constraints

- The battery runs ONCE at the **V6 release seam**, never at this slice; slices
  bank on `feature/client-server-views-claude` and push freely.
- Every discriminator is an **enum** in dialect code; wire text converts once at
  the boundary (`lsp_method_of`) — a new method is a new enumerator, never a
  string compare at a call site.
- The seat holds **no analysis**: every answer stays an existing projection or
  graph verb. Range arithmetic over data a projection already returned is
  presentation, not analysis (`lsp_diag_range` is the precedent).
- Dialect code: `var` never `value`; object **literals** not field-by-field;
  zero includes / `using` / `std::`; a `const char *` from a ring helper is
  owned into a `var` immediately.
- A **notification never gets a reply**, even when refused.
- Gates must run in `fulltest` with **no network and no node**: the node probe
  is a developer harness, the `.mad` tests are the gate.
- madcide has no auth or TLS. Nothing in this slice opens a port.

## What the real client already told us

A headless client built on Microsoft's own protocol machinery
(`vscode-jsonrpc` + `vscode-languageserver-protocol` — the same reader, writer
and types `vscode-languageclient` uses inside VS Code) drove the deployed
server end to end. Three disagreements, all real:

| # | Finding | Evidence |
|---|---------|----------|
| F1 | **`references` reports the ARGUMENT, not the callee.** `twice(21)` came back as the range covering `21`. A Call node's `column` is 0-based and anchors at the **first argument** (measured: `kind=Call name=twice line=8 col=16` on `\tlong n = twice(21);`, where `twice` spans bytes 10..15). `lsp_references`' one-byte retry cannot reach back over `(`. The existing gate pinned only the reference's LINE, so the wrong column froze in. | probe + `tmp/refprobe.mad` |
| F2 | **Three "unknown notification" lines on stderr per real session** — `$/setTrace`, `workspace/didChangeConfiguration`, `textDocument/willSave`. VS Code surfaces server stderr in its output channel, so a clean startup reads as three errors. These are notifications a conforming client legitimately sends and this server deliberately ignores; "unknown" is the wrong word for them. | probe stderr |
| F3 | **`documentSymbol.selectionRange` is the whole declaration line**, not the symbol's name. VS Code puts the cursor on `selectionRange` for Go-to-Symbol and paints it in the breadcrumb. An outline row carries `name`, `line`, `column`, `end_line`, and `column` is the start of the **declaration** (measured: `col=0` for `long twice(long a)`). | probe `documentSymbol` |

Not defects, confirmed working: `positionEncoding` negotiation (utf-16
offered, utf-16 answered), diagnostics on open and on an incremental change
(`use of undeclared identifier 'xlong'` with the right range), `hover`,
`definition`, 15 semantic tokens, `-32601` for an unimplemented method, and
`shutdown` → `exit` → **exit status 0** (the first probe's "did not exit" was
its own late listener; re-measured at 3.1 s including the ~2.5 s compile).

## File structure

| File | Responsibility |
|------|----------------|
| `tools/vscode-madcide/package.json` | Extension manifest: the `madc` language, settings, the `vscode-languageclient` dependency |
| `tools/vscode-madcide/extension.js` | The client: resolve the madc binary + the file to bind, spawn `madcide <file> --lsp`, wire the LanguageClient |
| `tools/vscode-madcide/language-configuration.json` | Comments, brackets, auto-closing — editor mechanics only, no highlighting |
| `tools/vscode-madcide/README.md` | Install / Remote-SSH / packaging, and what the server answers today |
| `tools/vscode-madcide/.gitignore` | `node_modules/`, `*.vsix` |
| `tools/vscode-madcide/test/protocol_probe.js` | The headless real-protocol harness that found F1–F3 (needs `npm install`; not a fulltest gate) |
| `tools/madcide/madcide_enums.inc` | +`lsp_method` enumerators for the ignored-notification family (F2) |
| `tools/madcide/madcide_lsp.inc` | +`lsp_name_span` (the one name-range owner), consumed by references (F1) and documentSymbol (F3) |
| `tests/testmadcide_lsp.mad` / `.expect` | The gate: pin the reference COLUMN, the selectionRange, and the ignored notifications |

## Tasks

### Task 1 — F2: the notifications this server deliberately ignores

`lsp_method` grows a named family — `$/setTrace`,
`workspace/didChangeConfiguration`, `workspace/didChangeWatchedFiles`,
`workspace/didChangeWorkspaceFolders`, `textDocument/willSave` — each a
notification the dispatcher answers with "nothing to do, nothing to say",
beside `lmINITIALIZED` / `lmCANCEL`. The stderr line stays for a genuinely
unknown method: it is a useful diagnostic, and the point of the change is that
a conforming client no longer trips it. `lsp_is_notification` lists them, so a
client that puts an id on one is told.

Gate: `testmadcide_lsp` sends `$/setTrace` and
`workspace/didChangeConfiguration` and asserts no reply and no refusal.

### Task 2 — F1/F3: one owner for the span of a NAME on a line

`lsp_name_span(from, to, w, doc, line, limit, name, last)` — the occurrence of
`name` on `line` that starts at or before `limit`, validated as one whole word
by the existing word owner (`lsp_word_span`), else false and the caller keeps
its line-range fallback.

- **references** pass the call's column as `limit` and `last = true`: the
  callee name is the last name before its argument list (`twice(twice(1))`
  resolves the inner call to the inner name).
- **documentSymbol** passes the whole line and `last = false`: a definition's
  name precedes its parameter list.

This is presentation arithmetic over `name` + `line` + `column`, all of which
the projection already returned — not analysis, and no new classification.

Gate: `testmadcide_lsp` pins the reference's start AND end character (10..15 on
the call line, not 16..18) and the selectionRange's characters.

### Task 3 — the extension

`package.json` contributing language `madc` (`.mad`), settings
`madcide.madcPath` / `madcide.madcidePath` / `madcide.serverFile` /
`madcide.trace.server`; `extension.js` resolving the binary (setting →
`bin/madc` under the workspace root → `madc` on PATH), choosing the document to
bind (the active `.mad` editor, else the first `.mad` in the workspace), and
starting the client with `documentSelector: [{scheme:'file', language:'madc'}]`.
No TextMate grammar. `language-configuration.json` for brackets and comments.

### Task 4 — the probe, promoted

`tools/vscode-madcide/test/protocol_probe.js`: the harness above, committed with
the npm commands to run it, so the next disagreement is found the same way.

### Task 5 — docs, status, memory

V6 plan §V6c gains a V6c-3a "as landed"; the design doc's V6 row and
`claude_status.json` carry the slice and the next one (V6c-3b); the memory topic
gains the findings.

## Self-review

- **Spec coverage.** "VS Code as the first-class api client" — Task 3 ships the
  client; Tasks 1–2 fix what a real client's traffic exposed; Task 4 keeps the
  discovery method. The ~60 madcide commands (`executeCommand`) and the webview
  are explicitly V6c-3b, named in the V6 plan, not silently dropped.
- **Placeholders.** None: F1–F3 carry measured evidence and named call sites.
- **Type consistency.** `lsp_name_span` has one signature, two call sites, and
  its `last` flag is the only thing that differs between them.
- **Deferred, named:** `workspace/symbol` (Ctrl+T), completion, signature help,
  formatting, code actions, rename (an L4c proposal), `semanticTokens/full/delta`
  (the server declares `full: true`, so a conforming client never asks), LSP over
  the `--serve` port, and the attach relay — V6c-3b/c.
