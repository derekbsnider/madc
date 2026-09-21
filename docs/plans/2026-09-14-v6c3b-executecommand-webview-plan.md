# V6c-3b — madcide's real controls in VS Code

> Slice of the client-server arc (design `2026-09-09-nexus-client-server-design.md`,
> V6 plan `2026-09-11-v6-transports-headless-plan.md` §V6c). Executes inline,
> like L4a–L4e and V6c-3a. The battery rides the V6 RELEASE seam, never this
> slice.

**Goal:** VS Code drives madcide's ~60 registry commands, hears about what the
session does, and can open the real madcide window — all against **one**
session.

**Spec:** V6 plan §V6c; the banked V6c-3b shape — `workspace/executeCommand`
onto the command registry, `$/madc/*` notifications from the change-event feed,
and a webview on the existing `--serve` page.

**Architecture:** V6c-3a gave VS Code language intelligence. Everything madcide
*does* — build, check, save, terminal, project, views, themes, outline,
problems — is in the command registry, which `api_run` already drives for every
transport. So the adapter is thin: one LSP method maps onto `api_run`, and the
change events `api_run` already broadcasts to api clients become LSP
notifications. The window is the existing `--serve` page in a webview.

## The one design decision this slice makes

The webview needs a served port, and madcide's `--serve` is a *session*. Two
processes would mean **two sessions on one file** — two carets, two undo
histories, last save wins: exactly the defect `edit_file`'s canonical
comparison was fixed for in V6c-2. So `--lsp` and `--serve` become combinable:

> **`madcide <file> --lsp --serve <addr>` is ONE process with ONE session
> wearing two faces** — LSP on stdio, api/ws/http on a loopback port.

This is what V6c-2 built `stdio://` as a *pollable channel* for ("so a serve
task parks like any socket"). The accept loop is `run_serve`'s, spawned with
`go` inside a `scope`; the stdio reader parks cooperatively the moment a task
is live, so the two faces interleave on one thread with no locking — the same
model `run_serve` already uses for many connections on one session.

It delivers the VS Code + browser half of V6c-3c's "one session" goal from the
easy direction (one process). V6c-3c still owns the **relay** — attaching to an
*already running* session, and routing MCP as well as LSP over the socket seat.

## Global Constraints

- The battery runs ONCE at the **V6 release seam**, never at this slice.
- Every discriminator is an **enum**; wire text converts once at the boundary.
  A new LSP method is a new `lsp_method` enumerator.
- The seat holds **no analysis** and **no command grammar**: `api_run` is the
  one command core, `cmd_of` the one name→code table, `cmd_min_tier` the one
  permission rule. The adapter must not learn what any command means.
- Dialect: `var` never `value`; object **literals**; zero includes / `using` /
  `std::`; ring-lifetime `const char *` owned into a `var` immediately.
- A **notification never gets a reply**.
- **No auth, no TLS.** The served port binds loopback only; the extension
  reaches it through `vscode.env.asExternalUri` (VS Code's own forwarding),
  never by exposing it.
- Gates run in `fulltest` with **no network and no node**.

## File structure

| File | Responsibility |
|------|----------------|
| `tools/madcide/madcide_enums.inc` | +`lmEXECUTECOMMAND`; the `$/madc/*` notification method names |
| `tools/madcide/madcide_lsp.inc` | +`lsp_execute_command` (→ `api_run`), +`lsp_session_notes` (events + message → `$/madc/*`), +`executeCommandProvider` in the capabilities, + the serve face in `run_lsp` |
| `tools/madcide/madcide.mad` | `--lsp` and `--serve` combinable |
| `tools/vscode-madcide/extension.js` | the command palette over the advertised list, the webview, the `$/madc/*` listeners |
| `tools/vscode-madcide/package.json` | `contributes.commands`, the `madcide.window` setting |
| `tests/testmadcide_lsp.mad` / `.expect` | executeCommand, the tier refusal, the unknown-command refusal, the `$/madc/*` notes |
| `tests/testmadcide_lsp_serve.mad` / fixtures | the two faces on one session, over the real wire |

## Tasks

### Task 1 — `workspace/executeCommand`

`lmEXECUTECOMMAND` (`workspace/executeCommand`, a REQUEST). Params are
`{command, arguments?}`. The command id on the wire is **`madcide.<name>`** —
prefixed because a VS Code client merges every server's command list into one
namespace, and stripped ONCE at this boundary before `cmd_of` ever sees it.
`arguments[0]`, when present, is the command's argument text (`IdeSession::command`
takes one argument string, which is why the MCP seat's tool schema has exactly
one `args` property).

The body is `api_run`'s, verbatim: `{ok, errors, text}` or `{ok:false, error}`.
The tier gate and the unknown-command refusal are already `api_run`'s — the
adapter adds neither.

`initialize` advertises `executeCommandProvider: {commands: [...]}` built from
`cmd_table`, prefixed the same way. One list, one source: what is advertised is
exactly what dispatch accepts.

Gate: `testmadcide_lsp` runs `madcide.check`, asserts `ok`; runs
`madcide.nosuchcommand`, asserts the refusal; asserts the advertised list
contains `madcide.check` and has `cmd_table`'s length.

### Task 2 — `$/madc/*` notifications

After a command, the session may have (a) advanced the change log and (b) set a
status message. Both already exist and both already reach api clients —
`broadcast_events` fans the first, the `msg` slot carries the second. The LSP
face pushes them as notifications instead of a poll:

- `$/madc/event` — one per new change-log record, the record verbatim (the same
  payload `broadcast_events` sends, so there is one event shape, not two).
- `$/madc/message` — the session's status line, when the command set one.
- plus `textDocument/publishDiagnostics` for the active document, because a
  command like `check` or `save` is exactly when diagnostics change.

`lsp_session_notes(notes, S, doc, adoc, head_before, msg_before)` is the one
producer; `lsp_execute_command` is its only caller today.

Gate: `testmadcide_lsp` asserts that an editing command produces a
`$/madc/event` note and a `publishDiagnostics` note.

### Task 3 — one session, two faces

`madcide.mad` stops treating `--serve` and `--lsp` as alternatives. `run_lsp`
gains an optional `serve` address:

- open the listener BEFORE the scope (so its endpoint is known), `detach()` it,
  and `go lsp_serve_task(&S, doc, handle, w, es)` — which `adopt()`s it and
  runs `run_serve`'s accept loop, spawning the existing `serve_conn_task` per
  connection. Nothing about the serve seat changes.
- the stdio message loop moves into `lsp_stdio_loop` so both the plain and the
  serving path run the SAME loop, not two copies.
- **shutdown:** the serve task is parked in `accept`, and `scope` joins before
  `run_lsp` returns. After the LSP loop ends, the serving path connects once to
  its own endpoint — the standard self-connect poke — so `accept` returns, the
  task sees `lspexit` and stops. Closing the fd under a parked poller is not a
  reliable wake; a connection is.
- the endpoint reaches the client in-protocol as `$/madc/serve` in the notes of
  `initialized` (the client's own ready signal), and stays on stderr for a human.

Gate: a new `testmadcide_lsp_serve` — one process spawned with BOTH faces;
drive LSP over the framed exec:// channel AND connect an api client to the
reported port; assert the api client sees the buffer the LSP client edited (one
session), then shutdown/exit and assert exit status 0 (the poke works).

### Task 4 — the extension

- `madcide.runCommand` — a QuickPick over the server's advertised list, an
  input box for the argument, then `workspace/executeCommand`. No hard-coded
  command list: the server's capability IS the list.
- `madcide.openWindow` — a webview panel hosting the served page through
  `vscode.env.asExternalUri` (so Remote-SSH forwards the loopback port).
- `$/madc/message` → the status bar; `$/madc/serve` → remember the endpoint;
  `$/madc/event` → the output channel when tracing.
- `madcide.window` (default `true`) adds `--serve 127.0.0.1:0` to the spawn.

### Task 5 — docs, status, memory

V6 plan §V6c gains a V6c-3b "as landed"; the design doc's V6 row; the status
UPDATE 19 with V6c-3c as NEXT; the memory topic.

## Self-review

- **Spec coverage.** All three banked parts are tasks. The fourth thing the
  spec implies — that they act on one session — is Task 3, and it is called out
  as this slice's one design decision rather than smuggled in.
- **Placeholders.** None; every method, slot and helper is named.
- **Type consistency.** `lsp_session_notes` has one signature and one caller;
  `api_run`'s body shape is reused verbatim rather than re-modelled.
- **Risk.** Task 3 is the only new concurrency. Its failure mode is a hang at
  exit, which the gate catches directly (exit status, under the runner's cap).
  If the poke proves unreliable the fallback is stated: keep the two faces but
  let the serving path end the process without joining.
- **Deferred, named:** the attach relay and MCP-over-socket (V6c-3c);
  `workspace/symbol`; completion; formatting; rename as an L4c proposal.
