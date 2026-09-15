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
4. **madcide's real controls in VS Code** — SHIPPED 2026-09-14 (plan
   `2026-09-14-v6c3b-executecommand-webview-plan.md`): `workspace/executeCommand`
   onto the command registry, `$/madc/*` notifications, and the window.

   **As landed.** The adapter holds no command grammar: it strips the wire
   prefix (`madcide.<name>` — a client merges every server's command list into
   one namespace) and hands the name to `api_run`, the one command core every
   transport drives. The tier gate and the unknown-command refusal are already
   `api_run`'s. `executeCommandProvider` is built from `cmd_table`, so what is
   advertised is exactly what dispatch accepts — 110 commands. A command's side
   effects arrive as notifications: `$/madc/event` carries each change-log
   record VERBATIM (the same payload `broadcast_events` fans to api clients —
   one event shape for every transport), `$/madc/message` the status line, and
   `publishDiagnostics` republishes.

   **The slice's one design decision: one process, two faces.** The webview
   needs a served port and `--serve` is a *session*, so two processes would be
   two sessions on one file — two carets, two undo histories, last save wins.
   `--lsp` and `--serve` are therefore combinable: the accept loop is
   `run_serve`'s, spawned with `go` inside a `scope`, and the stdio reader parks
   cooperatively the moment a task is live — which is what V6c-2 built
   `stdio://` as a pollable channel for. The endpoint reaches the client
   in-protocol (`$/madc/serve`, on `initialized`). Shutdown is a self-connect
   poke, because closing an fd under a parked poller does not reliably wake it.
   Found on the way: `lsp_method_of`'s loop bound was the last enumerator, so a
   method added after it converted to `lmNONE`; the bound is now an `lmLAST`
   sentinel.
   Gates: `testmadcide_lsp` extended (and now compiling the same composition
   the program does); NEW `testmadcide_lsp_serve` drives BOTH faces of one
   spawned process, proves the api client sees the line the LSP client deleted,
   and proves the process still exits 0.
5. **The attach relay** — SHIPPED 2026-09-14 (plan
   `2026-09-14-v6c3c-attach-relay-plan.md`): `madcide <file> --lsp --attach
   <addr>` (and `--mcp`) makes an editor or an agent a client of a session that
   is **already running**, instead of the owner of a private one.

   **As landed.** Two rulings. A connection **declares** its JSON-RPC dialect —
   `initialize` exists in both LSP and MCP, so routing by method name is
   ambiguous at exactly the first message and sniffing the params would be a
   guess; the relay is madcide's own code on both ends, so a third envelope
   (`{"madc":"lsp"}`, `envHELLO`) says it. A connection that sends no hello
   stays MCP, so no existing client moved. And **lifecycle belongs to the
   connection**: the relay answers `shutdown`/`exit` itself and never forwards
   them, because a session other clients are on must outlive one editor
   quitting. The relay opens no session — it is pure transport, two cooperative
   pumps.

   **The prerequisite bug.** `penc` — the negotiated position encoding — lived
   on the SESSION bag, so a second attached editor's `initialize` overwrote the
   first's and silently mis-placed every position on a line with a non-ASCII
   character before it. It is now per connection (`lsp_encoding` /
   `lsp_set_encoding`, keyed by share id) and resolved ONCE per message at the
   dispatcher, then passed down — a handler must never re-read it, because
   handlers yield and a shared slot read after a yield can hold another
   client's answer. Negative-controlled.

   Also: `madcide_lsp.inc` is now the dispatcher alone; `run_lsp`,
   `lsp_serve_task` and `run_attach` moved beside `run_serve`, where madcide's
   serving processes live — which is also what breaks the include cycle the
   routing creates (the seat needs `lsp_handle`, the serve face needs
   `serve_conn_task`). The relay's down-pump hit the same parked-poller trap as
   the accept loop; half-closing solves it properly (the seat reads EOF and
   closes its end, which is a real readable event).
   Gates: `testmadcide_attach` holds a live session with a real listener,
   spawns the relay, drives LSP down its stdio, and proves the edit landed in
   the holding process's session, that diagnostics and `$/madc/*` come back,
   that the relay exits 0, and that the session still stands afterwards.
   Confirmed with real clients (`tools/vscode-madcide/test/attach_probe.js`): an
   LSP editor and an api client on one session, the api client seeing the
   editor's edit.

6. **Session discovery** — SHIPPED 2026-09-15 (plan
   `2026-09-15-v6c4-session-discovery-plan.md`): every listening session
   publishes a JSON record under `$XDG_STATE_HOME/madcide/sessions/`, and
   `--attach` with no address finds the one that holds the file. The attach
   story (5) worked only if a human typed the port into three places; this is
   what removes the typing.

   **As landed.** An ADVERTISEMENT, not a lock — madcide refuses no file (there
   is no swapfile mechanism), so it never excludes, and two sessions in one root
   both appear. Staleness is a CONNECT TEST, never a pid test: a record whose
   endpoint refuses connection is removed by the next scan, so the directory
   self-heals after a crash. The enabling change is that the EDITOR listens by
   default (loopback, ephemeral; `--no-serve` opts out) — nothing is
   discoverable otherwise, and the TUI's `read_keys` already parks on {stdin, io
   waiters, timers} through taskio whenever a task is live, so the accept task
   runs while the editor waits for a keystroke. ⚠️ NO AUTH, NO TLS: the existing
   `--serve` caveat, now on by default.

   Three rulings, taken rather than left open: a STATE DIR rather than the
   project root (what neovim — the owner's own analogy — does with its state; a
   `.madcide.lock` would land in every project's git status); the editor
   listens; and a second TUI does NOT silently become a remote client, because
   that needs a TUI-over-api thin client which does not exist and is its own
   slice — it says what is running and how to join it, in one status line.

   **The two banked blockers were not real.** `getenv` is a registered builtin
   with the real C shape and `getpid` resolves through the dlsym fallback —
   probed in the JIT pass AND the `-o` exe pass. The slice is pure dialect code:
   no `src/`, no `include/`, no rule trailers.

   Two consolidations rather than two more copies: `serve_accept_task` is THE
   accept loop (run_serve's own copy had already diverged — no shutdown poke, no
   advertisement refresh), and `advertep` is THE slot holding the endpoint this
   session listens on, so `$/madc/serve` now tells an editor attached to an
   ORDINARY editor session about its window face too.
   Gates: `testmadcide_discover` (hermetic `MADCIDE_SESSION_DIR`) advertises a
   live session and spawns `--lsp --attach` with no address, which discovers it
   and drives it; negative controls on the component-boundary prefix match, the
   bound endpoint, and the reaping of a dead record. Plus
   `check-madcide-one-accept-loop.sh` in fulltest: one spawn site, and one
   `session_advertise()` per `listen://` bind.

**V6 is COMPLETE.** The remaining step is the arc's RELEASE seam — the owner's
call: `/dupaudit` scoped to `madcdis` + `tools/madcide`, the ONE battery
(`make -C src fulltest` + `--exe` + `--obj` + the packed / headerless / lane
runs), `scripts/lane_ledger.sh record`, merge to develop.

## Invariants to hold (design §2.8)
General client record (done); symmetric capability negotiation (consume AND
offer); a routing slot on the command envelope (`target?`); the change-event
log the ONE replication substrate; node-routable services with node-attributed
results; the renderer decoupled from the local host. A `client == local window`
assumption is the corner that costs the mesh.

## Seam
The battery runs ONCE at the V6 release boundary (owner law), not per sub-slice;
V6a/b/c bank on the arc branch. Name the seam when it is due.
