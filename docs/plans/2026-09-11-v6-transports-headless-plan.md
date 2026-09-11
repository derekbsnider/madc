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
1. **MCP seat**: an adapter translating MCP tool calls ↔ `api` commands (the
   seat pays for itself first — design §2.7 far-slice ordering).
2. **LSP endpoint**: `semanticTokens ← spans`, `publishDiagnostics ← diags`,
   `documentSymbol ← outline`, `hover ← parse_enclosing` — VS Code as the
   first-class `api` client (the acceptance test the api is client-general).

## Invariants to hold (design §2.8)
General client record (done); symmetric capability negotiation (consume AND
offer); a routing slot on the command envelope (`target?`); the change-event
log the ONE replication substrate; node-routable services with node-attributed
results; the renderer decoupled from the local host. A `client == local window`
assumption is the corner that costs the mesh.

## Seam
The battery runs ONCE at the V6 release boundary (owner law), not per sub-slice;
V6a/b/c bank on the arc branch. Name the seam when it is due.
