# V6 — transports + headless (plan)

**Arc:** client-server Views (design doc `2026-09-09-nexus-client-server-design.md`,
§2.7 transports + headless, §2.5 tiers, §2.8 groundwork invariants).
**Branch:** `feature/client-server-views-claude` (arc branch; slices bank here,
the battery runs once at the V6 seam).
**Status of V1–V5:** merged to develop (`7b4d881c`) and green. V6 begins here.

## What V6 delivers (design §2.7)

A madcide **session with zero windows** serving remote clients over a channel:
- **`api`** — one JSON line per message (`{"cmd","args","seq"}` in;
  `{"event":…}` / `{"reply":…}` out), the SAME registry commands the profiles
  bind and the SAME events the change-event log records, plus queries (the
  composed tree, or the raw projections a client declares it consumes). The MCP
  seat and the LSP endpoint are adapters over `api`.
- **`ws`** — a remote WINDOW: the same embedded `page.js` over a WebSocket (the
  DOM-op JSON down the socket, the page's event JSON back into `post_event`).
- **Headless** — `madcide --serve <addr> [file|manifest]`: no window, one or
  more `api`/`ws` clients; the display-free host (`register_host`/`post_event`)
  is the precursor already proven by V3b.
- **Permission tiers** — `observe`(0) < `edit`(1) < `admin`(2), a verb's
  requirement declared as data (`%require ide >= N`); local clients born
  `admin`, remote/MCP born `observe`, promoted by `clienttier <id> <tier>`.

## Groundwork already in place (recon 2026-09-11)

- `ide_transport { trLOCAL, trWS, trAPI }` (`madcide_enums.inc`) + the general
  Client record `make_client` (`madcide_client.inc`): `transport`, `level`,
  `tier`, `capabilities` (symmetric consume/offer), `last_seq` (cursor into the
  V4 log), `keysgen`.
- `run_ide` over a client roster via `ui::event_any` (V3b); `spawn_client`
  opens a frontend + joins the roster.
- `IdeSession::command(doc, code, arg, cont)` / `post` / `error_count` (V1.5) —
  a command by code, the api seat's `{"cmd","args"}` posts through this.
- `register_host(target, level, ops)` + `ui::post_event(ctx, json)` (`ns_ui.cpp`,
  `ns_ui_web`) — the host seam; a display-free host is a registered target.
- `madc::channel` + `src/madc_socket_channel.cpp` — a real byte-channel over
  sockets, but **connect-only today** (`NetworkSocketChannelFactory` dials out;
  there is no bind/listen/accept). `channel_stream.h` frames bytes.
- The V4 change-event log (`clog_*`) — the replication substrate a client's
  `last_seq` reads (design invariant #4).

## Sub-slices (execute in order; targeted gates each; ONE battery at the V6 seam)

### V6a — the `api` transport + headless `--serve` + tiers (the foundation)
The substrate ws/MCP/LSP all ride. Engine + dialect.

1. **Engine — `listen://host:port` accept channel** (`madc_socket_channel.cpp`,
   the channel scheme registry). A listening socket that yields ACCEPTED
   channels, each an ordinary `madc::channel` (a `SocketDataChannel` over the
   accepted fd). Non-blocking accept so the cooperative scheduler drives it (no
   thread — design §2.7). Register the `listen` scheme beside the connect
   schemes. Gate: a unit test — listen, connect a client, accept, byte
   round-trip.
2. **Dialect — the `api` seat** (`tools/madcide/`): a client whose `read_events`
   reads one JSON line from its accepted channel and turns `{"cmd",args,seq}`
   into an `IdeSession::command` by NAME (the name resolved against the registry
   at the seat boundary, exit/refusal on unknown — the V1.5 `-c` boundary
   reused); `render` writes `{"reply":…}` / `{"event":…}` JSON lines out (the
   composed tree or a raw projection per the client's declared `consume`). A
   Client record `trAPI`, level `ui::NONE`, tier `observe`.
3. **Dialect — `madcide --serve <addr> [file]`**: open the session headless (no
   `ui::open` window), `listen://<addr>`, accept clients as cooperative tasks
   joined into `run_ide`'s roster; each accepted connection = a `spawn_client`
   with `trAPI`.
4. **Tiers**: `%require ide >= N` on verb rows (data, the existing gate shape);
   `client_tier` bounds dispatch; local=admin, remote=observe; `clienttier <id>
   <tier>` an admin verb; a refused verb answers with its registered refusal
   prose (never silent).
   Gate: a headless integration test — a scripted api client posts commands over
   a channel, receives replies/events; an `observe` client's edit verb is
   refused with prose; `clienttier` promotes it and the edit lands.

### V6b — the `ws` transport (a remote window)
1. **Engine — an RFC6455 WebSocket framer** over a byte channel (beside
   `channel_stream.h`; no extensions). The `listen://` accept + the upgrade
   handshake yield a framed channel.
2. A **ws client** = a Client record `trWS`, level `ui::WEB`: the same `page.js`
   over the socket. DOM-op JSON down, event JSON up into `post_event`.
   Gate: a ws client (a scripted framer) receives the composed tree as DOM ops
   and an edit round-trips; the local window is byte-identical (N=1).

### V6c — the MCP seat + the LSP adapter (adapters over `api`)
1. **MCP seat** — SHIPPED 2026-09-12 (`2be5687da`, structured state
   `9620e32b5`): an adapter translating MCP tool calls ↔ `api` commands (the
   seat pays for itself first — design §2.7 far-slice ordering). It then grew
   its own arc: the code-graph MCP ladder L1 → L4e (design
   `2026-09-12-ast-graph-mcp-for-agents.md`, `2026-09-13-nexus-L4-design.md`).
2. **LSP endpoint** — SHIPPED 2026-09-14 (plan
   `2026-09-14-v6c2-lsp-adapter-plan.md`): `semanticTokens ← spans`,
   `publishDiagnostics ← diags`, `documentSymbol ← outline`, `hover ←
   parse_enclosing`, plus `definition` / `references` ← the L1/L2 graph verbs
   — every editor that speaks LSP drives madcide without adopting madcide's
   frontend, and the api's client-generality gets its acceptance test.

   **As landed.** The FRAMING is an engine channel facet, not seat code:
   `channel::frame_headers()` (`madcdis/header_channel.h`) turns a byte
   channel into a Content-Length message channel, the V6b-1 WebSocket-framer
   shape, named for the framing because DAP and BSP share it. stdio became an
   ordinary channel (`stdio://`) so one seat loop serves stdio today and a
   socket later. The position ENCODING is negotiated at `initialize` and the
   UTF-16 ↔ byte column arithmetic got one owner (`madcdis/text_utf16.h`,
   `ui::text_bytecol` / `ui::text_col16`) shared with the web hit test.
   `madcide_lsp.inc` holds no analysis: every answer is an existing
   projection or graph verb, run after the same L4e layer check, and every
   edit rides the ONE text-mutation owner. Deferred and named: LSP over the
   `--serve` port, `workspace/*`, completion, formatting, rename (an L4c
   proposal), and the background reparse.
   Gates: `testmadcide_lsp` (in process), `testmadcide_lsp_stdio` (the
   deployed process over exec://, framing and exit status included),
   `test_header_channel`, `test_text_utf16`, `testcanonicalpath`.
3. **The VS Code extension** — SHIPPED 2026-09-14 (plan
   `2026-09-14-v6c3a-vscode-extension-plan.md`): `tools/vscode-madcide/`, the
   client that points VS Code at `madc tools/madcide/madcide.mad <file> --lsp`
   on stdio. This is the arc's stated acceptance criterion — VS Code as a
   first-class api client — and it needed no socket work: with Remote-SSH the
   extension host runs beside the binary.

   **As landed.** The extension contributes the `madc` language and NO
   TextMate grammar: `parse_spans` reaches the editor as semantic tokens from
   the same parse handle that produces the diagnostics and the outline, and a
   grammar would be a second classification of the same text. A real client
   built on Microsoft's own protocol machinery
   (`tools/vscode-madcide/test/protocol_probe.js` — `vscode-jsonrpc`, no VS
   Code, no display, no network) drove the deployed server and found three
   defects the repo's gates had not: `references` highlighted the call's first
   ARGUMENT (a graph Call node's column anchors at its argument list and the
   old gate pinned only the line), `documentSymbol`'s `selectionRange` covered
   the whole declaration line, and three conforming-client notifications
   (`$/setTrace`, `workspace/didChangeConfiguration`, `textDocument/willSave`)
   reached stderr as "unknown" — which VS Code shows in its output channel. The
   first two are now one owner, `lsp_name_span`: the projection reports what a
   thing is CALLED, so the range is found from the NAME and validated as a
   whole word, never guessed from a column that anchors elsewhere. A conforming
   session now prints empty stderr and exits 0.
   Gates: `testmadcide_lsp` extended (the reference's COLUMN, the
   selectionRange, the ignored notifications), both range fixes
   negative-controlled. The probe is the instrument, not a battery member —
   the battery runs with no network and no node.
   Next: V6c-3b (`workspace/executeCommand` onto the command registry,
   `$/madc/*` notifications, a webview on the `--serve` page) and V6c-3c (the
   attach relay: one session behind VS Code, an agent's MCP client and a
   browser).

## Invariants to hold (design §2.8)
General client record (done); symmetric capability negotiation (consume AND
offer); a routing slot on the command envelope (`target?`); the change-event
log the ONE replication substrate; node-routable services with node-attributed
results; the renderer decoupled from the local host. A `client == local window`
assumption is the corner that costs the mesh.

## Seam
The battery runs ONCE at the V6 release boundary (owner law), not per sub-slice;
V6a/b/c bank on the arc branch. Name the seam when it is due.
