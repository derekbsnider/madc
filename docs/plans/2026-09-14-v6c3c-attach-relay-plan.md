# V6c-3c — the attach relay: one session, every client

> Slice of the client-server arc (design `2026-09-09-nexus-client-server-design.md`,
> V6 plan `2026-09-11-v6-transports-headless-plan.md` §V6c). Executes inline.
> The battery rides the V6 RELEASE seam, never this slice.

**Goal:** VS Code, an agent's MCP client and a browser window all drive **one
already-running madcide session** — the gatekeeper shape.

**Spec:** V6 plan §V6c; the banked V6c-3c shape — `madcide --lsp --attach
<addr>` / `--mcp --attach <addr>`, plus LSP routing on the socket seat.

**Architecture:** V6c-3b made one *process* wear two faces. What is still
missing is joining a session that is **already running**: an editor and an
agent each spawn their own stdio process, so each needs a way to become a
client of someone else's session rather than the owner of a private one. Two
pieces: the socket seat must be able to speak LSP (it only speaks api and MCP
today), and a tiny stdio↔socket **relay** must carry one editor's or agent's
traffic to it.

## The two decisions this slice makes

**1. A connection declares its protocol; it is never sniffed.** `initialize`
exists in *both* LSP and MCP, so routing JSON-RPC by method name alone is
ambiguous at exactly the first message. The relay is madcide's own code on both
ends, so it says what it speaks: a third envelope, `{"madc":"lsp"}`, read once
per connection by `envelope_of`. **A connection that sends no hello stays MCP**
— today's behaviour, so no existing client moves.

**2. Lifecycle belongs to the connection, not the session.** The relay answers
`shutdown` and `exit` itself and never forwards them: one editor quitting must
not tear down a session other clients are still on. The server learns the client
left the way it always does — the socket closes and that connection's task ends.

## Global Constraints

- The battery runs ONCE at the **V6 release seam**, never at this slice.
- Every discriminator is an **enum**; wire text converts once, at the boundary.
- The seat owns only the envelope. Command semantics stay in `api_run`, MCP in
  `mcp_handle`, LSP in `lsp_handle`.
- The relay holds **no protocol knowledge** beyond its own lifecycle: it is
  transport, and it opens no session at all.
- Dialect: `var` never `value`; object **literals**; ring-lifetime
  `const char *` owned into a `var` immediately; no declarations inside a
  `switch` case.
- **No auth, no TLS.** Loopback only, tunnelled over ssh. Never expose a port.
- Gates run in `fulltest` with **no network and no node**.

## The bug this slice must fix first

`penc` — the negotiated position encoding — lives on the **session** bag. One
session with two attached editors means the second one's `initialize`
overwrites the first's encoding, and every position on every line with a
non-ASCII character before it is then silently mis-placed for one of them. That
is the exact failure V6c-2 introduced the negotiation to prevent, and "many
clients on one session" is what this slice is *for*. It is a prerequisite, not
a follow-up.

## File structure

| File | Responsibility |
|------|----------------|
| `tools/madcide/madcide_enums.inc` | `envHELLO`; `seat_protocol` (`spMCP`/`spLSP`) + converters |
| `tools/madcide/madcide_lsp.inc` | `lsp_encoding` / `lsp_set_encoding` (the one per-connection encoding owner); `run_attach` |
| `tools/madcide/madcide_seat.inc` | the hello, the per-connection protocol, LSP routing + its notes |
| `tools/madcide/madcide.mad` | `--attach <addr>` for `--lsp` and `--mcp` |
| `tools/vscode-madcide/` | `madcide.attach` setting; README |
| `tests/testmadcide_attach.mad` + fixtures | an in-process session, a spawned relay, LSP through it |
| `tests/testmadcide_lsp.mad` | the per-connection encoding, pinned |

## Tasks

### Task 1 — one owner for the position encoding, per connection

`lsp_encoding(w, es, self_id)` and `lsp_set_encoding(w, es, self_id, enc)`,
keyed `penc<self_id>` on the session bag (the stdio seat's `self_id` is -1, so
its slot is `penc-1` — the same rule, no special case). Every site that reads
`es_int(w, es, "penc", peUTF16)` reads the owner instead; `lsp_initialize`
writes through it. `lsp_handle` therefore needs `self_id` on the read path,
which it already carries.

Gate: `testmadcide_lsp` negotiates utf-8 on one id and asserts a *different* id
still reads utf-16 — the collision, made visible.

### Task 2 — the socket seat speaks LSP

- `envHELLO`: `{"madc":"<protocol>"}`. `envelope_of` returns it; the seat
  converts the word once (`seat_protocol_of`), stores it for the connection,
  and answers `{"madc":"<protocol>","ok":true}` so the relay knows it was heard.
- The connection's protocol is a local in `serve_line_client` — it is per
  connection by construction, which is the point.
- `envRPC` then routes: `spLSP` → `lsp_handle` (writing its reply AND draining
  its `notes`, the same way `run_lsp` does), otherwise `mcp_handle`, unchanged.
- An unknown protocol word is refused by name, never silently defaulted.

Gate: `testmadcide_attach` drives LSP over a socket; `testmadcide_serve_mcp`
still passes untouched (no hello ⇒ MCP).

### Task 3 — the relay

`madcide <file> --lsp --attach <addr>` and `--mcp --attach <addr>`.

`run_attach(addr, framed)` opens **no session**: it is pure transport.
`stdio://` (framed for LSP, line-oriented for MCP) on the editor side,
`tcp://<addr>` on the session side, the hello, then two cooperative pump tasks
so a server push and a client request never wait on each other.

Lifecycle, per decision 2: the up-pump parses each message's `method` only far
enough to see `shutdown` (answer `{"result":null}` locally) and `exit` (stop) —
neither is forwarded. Everything else is relayed verbatim. On stdio EOF the
relay closes the socket and stops; on socket EOF it stops too, because the
session it was carrying is gone. Exit status follows the LSP rule: 0 after a
shutdown, 1 without one.

`<file>` is still accepted on the command line (the CLI's shape) and is unused
in attach mode — the session already has its documents, and the editor adds any
others with `didOpen`. The usage line says so.

Gate: `testmadcide_attach` — an in-process session with a real listener, a
spawned `--lsp --attach` child, LSP driven through it; the answers must come
from the in-process session's buffer, and the child must exit 0.

### Task 4 — the extension

`madcide.attach` (default empty): when set, spawn `--lsp --attach <addr>`
instead of a private session, and skip `--serve` (the session already has a
window face). `madcide.window` and `madcide.attach` are mutually exclusive by
construction, and the output channel says which mode it chose.

README: the three-client picture, the ssh tunnel, and the **tier** fact — V6a
grants the first connection OWNER and everyone after it OBSERVER, so a second
client is read-only until an owner runs `clienttier <id> editor`. That is the
gatekeeper behaving as designed; it is documented, not widened.

### Task 5 — docs, status, memory

V6 plan §V6c "as landed"; the design doc's V6 row; status UPDATE 20 naming the
V6 RELEASE seam as the next thing; the memory topic.

## Self-review

- **Spec coverage.** Both banked parts are tasks (relay, routing). The third
  thing the goal implies — that many clients can coexist — is Task 1, promoted
  to a prerequisite because it is a live silent-wrong-answer bug.
- **Placeholders.** None; every method, slot, envelope and helper is named.
- **Type consistency.** `lsp_encoding` has one signature and replaces every
  `es_int(..., "penc", ...)` read; `run_attach`'s `framed` flag is the only
  difference between the LSP and MCP relays.
- **Risk.** The relay's two pumps are the new concurrency; the failure mode is
  a hang, which the gate catches as a timeout and an exit status.
- **Deferred, named:** promoting an attached client automatically (tiers stay
  explicit); `workspace/symbol`; completion; formatting; rename as an L4c
  proposal; `--attach` over anything but loopback (there is no auth).
