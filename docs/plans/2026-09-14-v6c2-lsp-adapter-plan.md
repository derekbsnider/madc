# V6c-2 — the LSP adapter (`madcide --lsp`): implementation plan

> **For agentic workers:** this plan is executed INLINE in the session that
> wrote it (the L1–L4e mode). Steps use checkbox (`- [ ]`) syntax; a task is
> banked with its own commit and its own targeted gate. The FULL battery rides
> the V6 arc RELEASE seam, never a task or this slice.

**Goal:** serve the Language Server Protocol over stdio — `madcide <file>
--lsp` — so any LSP-speaking editor (VS Code, neovim, emacs, Helix, JetBrains)
drives the same session, the same registry commands and the same query layer
the api seat and the MCP seat drive.

**Architecture:** the protocol's FRAMING is an engine channel facet (the V6b-1
WebSocket-framer precedent), stdio becomes an ordinary channel so the framer
has something to frame, and the SEAT is a dialect file (`madcide_lsp.inc`) that
converts one JSON-RPC message into session work and back. Every answer is an
existing projection — `parse_check`, `parse_outline`, `parse_spans`,
`parse_enclosing` — or an existing graph verb (`graph.definition`,
`graph.references`); this slice adds a FACE, not an analysis engine.

**Tech Stack:** madc engine C++11 (`src/`, `include/madcdis/`, `include/madc/`),
the madc dialect (`tools/madcide/*.inc`), doctest for the codec unit test,
`tests/*.mad` + fixtures for the integration gates.

**Spec:** `docs/plans/2026-09-11-v6-transports-headless-plan.md` §V6c item 2
(the deliverable: `semanticTokens ← spans`, `publishDiagnostics ← diags`,
`documentSymbol ← outline`, `hover ← parse_enclosing`; VS Code as the
first-class api client — "the acceptance test the api is client-general"), and
`docs/plans/2026-09-09-nexus-client-server-design.md` §2.7 (the rich api offers
BOTH the composed tree and the raw projections; the LSP adapter is named there)
and §2.8 (groundwork invariants: the general client record, symmetric
capability negotiation, a routing slot, the change log as the ONE replication
substrate, "a `client == local window` assumption is the corner that costs the
mesh").

## Global Constraints

- **Enums, not strings** (owner law, `.claude/rules/enum-over-strings.md`):
  every discriminator — the LSP method, the position encoding, the symbol kind,
  the token type, the sync kind — is an enum in `tools/madcide/madcide_enums.inc`
  with its `X_name` / `X_of` converter pair. Protocol text converts ONCE at the
  message boundary; dispatch is a `switch` on the code.
- **Value-first dialect** (`.claude/rules/value-first.md`): zero `#include`,
  zero `using`, zero `std::` in `.inc` / `.mad` code; the carrier is `var`;
  output is bare `print` / `println` / `format`.
- **Dialect literals** (`.claude/rules/dialect-literals.md`): build objects with
  `var x = { "k": v };`; imperative key-assign only for mutation, computed keys
  and indices.
- **One implementation per concern** (`.claude/rules/no-parallel-implementations.md`):
  no second framer, no second word scanner, no second UTF-16 column walk, no
  second path canonicalizer, no second uri→doc map.
- **Rule trailers** (`.claude/rules/rule-trailers.md`): every commit touching
  `src/` or `include/` carries `Hypothesis:` / `Layer:` / `Searched:` /
  `Oracle:`. Dialect, test and doc commits ride without.
- **Zero warnings**, both surfaces, every lane.
- **Targeted gates per task**; `make -C src fulltest` is the V6 SEAM's battery,
  never this slice's.
- **Scratch in `tmp/`** only; the owner's `donut.c` / `test.mad` /
  `testsort.mad` stay untouched.
- Attribution trailers on every commit:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01HgpX35SCeWYzi8eahsMaid`.

---

## 1. Decisions (the rulings this plan argues from)

1. **The framing is an ENGINE CHANNEL FACET, not dialect string handling.**
   LSP frames each message as `Content-Length: N\r\n\r\n` + exactly N bytes.
   `madc::channel::readline` cannot express an exact-byte read, and a framing
   loop written in the dialect would be a second implementation of what the
   WebSocket framer already does one layer down. So: `channel::frame_headers()`
   switches a byte channel into header-framed MESSAGE mode — afterwards
   `readline()` returns one complete message body and `write()` emits the
   header plus the body. Precedent and shape: `upgrade_websocket()` /
   `WebSocketDataChannel` (V6b-1).

2. **The facet is named for the FRAMING, not for LSP.** `frame_headers()` /
   `HeaderFramedDataChannel`. The same framing carries DAP and BSP; naming it
   `frame_lsp` would make the next protocol grow a copy.

3. **stdio is a CHANNEL, not a special case.** A new `stdio://` factory yields
   a channel reading fd 0 and writing fd 1, pollable on fd 0, whose `close()`
   never closes the process's standard descriptors. Why: the framer takes a
   channel; making stdio one keeps ONE seat loop for stdio today and for a
   socket tomorrow, and it parks on the reactor like every other channel
   instead of blocking the thread under live tasks.

4. **The seat loop is the MCP seat's loop.** `run_lsp(path, ro)` opens a
   headless `IdeSession`, opens `stdio://`, calls `frame_headers()`, and loops
   `readline` → `js::parse` → `lsp_handle` → `write`. The lone stdio client is
   the operator: `self_id` -1 (untracked → owner via `eff_tier`), exactly the
   `--mcp` and `-c` rule. A socket-borne LSP client would be born low; that
   rides the deferred transport.

5. **Every LSP method is an ENUM at the boundary.** `lsp_method` +
   `lsp_method_of(word)` / `lsp_method_name(code)` in `madcide_enums.inc`;
   `lsp_handle` converts once and switches. An unknown method is `lmNONE` → a
   `-32601` refusal naming the method. Same for the position encoding
   (`pos_encoding`), the document sync kind and the symbol/token vocabularies.

6. **The position encoding is NEGOTIATED, never assumed.** LSP positions are
   0-based lines and 0-based columns in **UTF-16 code units** by default;
   3.17 lets a server offer others. `initialize` reads
   `params.general.positionEncodings`: if it lists `utf-8` we take it (a column
   IS a byte offset, zero conversion); otherwise we take `utf-16` and convert
   honestly. The chosen code is stored on the session bag and answered as
   `capabilities.positionEncoding`. A server that silently reads utf-16 columns
   as bytes corrupts every line with a non-ASCII character before the caret —
   that is the silent-wrong-answer class, not a rounding error.

7. **The UTF-16 ↔ byte column arithmetic gets ONE owner.** `web_byte_col()`
   (UTF-16 column → byte index) already exists in
   `include/madcdis/web_model.h` and is used once by the web hit test. It moves
   into a new `include/madcdis/text_utf16.h` beside its new inverse
   `web_col16()` (byte index → UTF-16 column); `web_model.h` includes that and
   its one call site is unchanged. `ui::text_bytecol` / `ui::text_col16` are
   the dialect publics over the same pair. A second byte-walking loop in
   `ns_ui.cpp` is precisely the divergence `/dupaudit` hunts.

8. **A document is addressed by URI; the BUFFERS TABLE is the map.** `didOpen`
   turns the URI into a path and drives `edit_file` — the ^K E open-or-switch
   owner — and every later request naming a `textDocument.uri` switches the
   active buffer the same way before it reads anything. No parallel uri→doc
   map: the table already IS path→doc, and a second map would drift the moment
   a buffer closes.

9. **Paths compare CANONICALLY.** A client sends `file:///abs/path`; madcide
   may have been launched with `rel/path`. Without canonical comparison one
   file becomes two buffers and every edit lands in the wrong one. The standing
   owner is `madc::detail::canonical_path_for_compare` (AGENTS.md rule #4); it
   gets a dialect public, `madc::canonical_path(out, path)`. No new
   normalization rule is written.

10. **Text mutation goes through the ONE text-mutation owner.** An incremental
    `didChange` is `ed_text_erase` + `ed_text_insert` (anchors shift, the
    splice is journaled into the L4c project stream and fans out to every other
    connected client — an LSP edit is visible in an open TUI window). A
    full-text change and `didOpen` use `ui::text_load`: a LOAD is not an edit,
    and the client's text is authoritative on open.

11. **Diagnostics publish on didOpen / didChange / didSave, through the ONE
    compiler entry.** `refresh_diags(w, es, doc)` → `parse_check` rows →
    `textDocument/publishDiagnostics`. The reparse is SYNCHRONOUS in v1, the
    same cost the editor pays on `^K ;`. No debounce heuristic, no "cheap
    parses only" test: a rule keyed on how expensive a file looks is the
    hard-coded specific Rule #7 bans. The background-parse path
    (`parse_pending`, already built for the editor loop) is the named
    follow-up.

12. **Hover / definition / references answer from the GRAPH layer.** The
    identifier at a position comes from the ONE word owner
    (`ui::text_word_left` / `ui::text_word_right`, "a word byte is
    `[A-Za-z0-9_]`"), and the answer from `graph_call` — the same
    `graph.definition` / `graph.references` verbs the MCP seat serves, under
    the same tier and layer gates. Hover falls back to `parse_enclosing` (the
    status line's query) when the word resolves to nothing.

13. **`semanticTokens/full` reads `madc::parse_spans`; the THEME is not
    consulted.** LSP carries semantics and the client owns colour, so the
    legend is the class vocabulary (`keyword`, `ident`→`variable`, `number`,
    `string`, `comment`, `type`, `function`) and the rows are delta-encoded
    (deltaLine, deltaStartChar, length, tokenType, tokenModifiers) per the
    spec. `spans_to_hspans`, which DOES consult the theme and drops unstyled
    classes, is the terminal/web renderer's path and is not reused here.

14. **`documentSymbol` reads `madc::parse_outline`** — rows
    `{kind, name, line, column, end_line}`; `kind` "function" → LSP
    `SymbolKind` 12, `range` = line..end_line, `selectionRange` = the name's
    own line. A kind the outline grows later maps through one converter, not a
    ladder at the call site.

15. **A refusal is a JSON-RPC error, never silence** (design §6, the
    registered-prose rule): `-32601` unknown method (naming it), `-32602`
    missing/!bad params (naming the field), `-32803` RequestFailed when a
    document cannot be opened (naming the path). A notification never gets a
    reply, even when it is refused — the refusal goes to stderr, which is not
    the protocol stream.

16. **Scope: stdio only.** Deferred and NAMED: LSP over the `--serve` port (a
    fourth `serve_web` classification), `workspace/*` (symbol, didChangeWatched
    Files), completion, signature help, formatting, code actions, and
    `textDocument/rename` — rename is an L4c PROPOSAL, and wiring it to
    `workspace/applyEdit` is its own design question (whose tier applies, and
    whether the editor sees a proposal or a landed edit).

---

## 2. File structure

| File | Disposition | Responsibility |
|---|---|---|
| `include/madcdis/text_utf16.h` | **create** | THE owner of UTF-16 ↔ UTF-8 column arithmetic over one line of text: `web_byte_col` (moved) + `web_col16` (new). |
| `include/madcdis/web_model.h` | modify | include `text_utf16.h`; the `web_byte_col` definition leaves, the one call site stays. |
| `include/madc/ns_ui` | modify | declare `ui::text_bytecol` / `ui::text_col16` (document-aware wrappers). |
| `src/ns_ui.cpp` | modify | implement the two publics over the one owner. |
| `include/madc/ns_madc` | modify | declare `madc::canonical_path`; document the `stdio://` scheme and the `frame_headers()` facet on `channel`. |
| `src/ns_madc.cpp` | modify | implement `madc::canonical_path` over `detail::canonical_path_for_compare`. |
| `src/madc_datachannel.cpp` | modify | the `stdio://` factory + `StdioDataChannel` (read fd 0 / write fd 1, pollable, close() leaves the std fds alone). |
| `include/madcdis/header_channel.h` | **create** | `HeaderFramedDataChannel`: the Content-Length codec over a byte channel (`feed` / `next_message` / `encode_message`) + raw inner I/O + the pollable passthrough. |
| `src/madc_header_channel.cpp` | **create** | that codec's implementation. |
| `src/Makefile` | modify | `madc_header_channel.o` joins `CORE_OFILES`. |
| `include/madcdis/channel.h` | modify | declare `bool frame_headers();` on `madc::channel`. |
| `src/madc_channel_object.cpp` | modify | `frame_headers()`, the `hfc` typed alias on `ChannelState`, the `hdr_recv` park loop, and the framed write path. |
| `tools/madcide/madcide_enums.inc` | modify | `lsp_method`, `pos_encoding`, `lsp_symbol_kind`, `lsp_token_type`, `lsp_sync` + their converter pairs. |
| `tools/madcide/madcide_lsp.inc` | **create** | THE LSP seat: `lsp_handle`, the lifecycle, document sync, the URI and position converters, the six answering methods, `run_lsp`. |
| `tools/madcide/madcide.mad` | modify | `--lsp` → `run_lsp(argv[1], ro)`; the usage line; the include. |
| `scripts/check-madcide-enums.sh` | modify | `madcide_lsp.inc` joins `SEAT_FILES`; the new converters join the anchor list. |
| `tests/unit/test_header_channel.cpp` | **create** | the codec pinned: split reads, multiple headers, a body with embedded newlines, a missing/!numeric Content-Length, encode round-trip. |
| `tests/testmadcide_lsp.mad` (+ `.expect`, `.expect_quiet`) | **create** | the seat in-process: lifecycle, both encodings, sync, all six methods, the refusals. |
| `tests/testmadcide_lsp_stdio.mad` (+ `.expect`, `.expect_quiet`) | **create** | end-to-end: spawn `{madc} madcide.mad <file> --lsp` as a child, speak real framed LSP down the pipe, read framed replies. |
| `docs/plans/2026-09-11-v6-transports-headless-plan.md` | modify | V6c-2 marked SHIPPED with its gate. |
| `docs/plans/2026-09-09-nexus-client-server-design.md` | modify | §2.7 "as landed" for the LSP face; the V6 row. |
| `claude_status.json` | modify | `live_handoff` UPDATE 17. |

---

## 3. Tasks

### Task 1 — the `stdio://` channel (engine)

**Files:** modify `src/madc_datachannel.cpp`, `include/madc/ns_madc` (scheme
doc). **Test:** `tmp/lsp_probe_stdio.mad` (scratch probe; the shipped gate is
Task 8's end-to-end test, which cannot run without this).

**Interfaces — produces:** the URI scheme `stdio://` opening a read+write byte
channel over fds 0 and 1.

- [ ] **Step 1: probe the current behaviour** — `bin/madc tmp/lsp_probe_stdio.mad`
  with `var c = madc::channel("stdio://"); println("ok={}", c.ok());`
  Expected: `ok=0` (no factory for the scheme).
- [ ] **Step 2: implement `StdioDataChannel`** in the anonymous namespace of
  `src/madc_datachannel.cpp`, beside `FileDataChannel`: two fds (`rfd_` 0,
  `wfd_` 1), `read`/`write` through `detail::read_fd` /
  `detail::write_fd_without_sigpipe` (both already Win32-portable), capabilities
  read+write, seek false, `name()` "stdio". It derives from `DataChannel` AND
  `PollableDataChannel` with `read_poll_handle()` = `rfd_`. `close_write()`
  closes fd 1; `close()` marks the channel spent WITHOUT closing fd 0/1 — the
  process's standard descriptors are not the channel's to reap.
- [ ] **Step 3: register the factory** in `DataChannelRegistry::DataChannelRegistry()`
  beside `file`/`pipe`: `register_factory("stdio", … new StdioChannelFactory())`.
- [ ] **Step 4: document the scheme** in `include/madc/ns_madc`'s channel
  comment (the URI list gains `stdio://`, one line saying it is the process's
  own standard streams and that `close()` leaves them open).
- [ ] **Step 5: rebuild and re-run the probe.** `make -C src` then the probe:
  expected `ok=1`, and a write reaching stdout, and a readline returning a line
  piped into it.
- [ ] **Step 6: commit** (engine → four trailers).

### Task 2 — the header framer (engine)

**Files:** create `include/madcdis/header_channel.h`,
`src/madc_header_channel.cpp`; modify `src/Makefile`,
`include/madcdis/channel.h`, `src/madc_channel_object.cpp`,
`include/madc/ns_madc`. **Test:** create `tests/unit/test_header_channel.cpp`.

**Interfaces — consumes:** Task 1's `stdio://`. **Produces:**
`bool madc::channel::frame_headers()`; after it, `readline(value&)` yields one
complete message body and `write(const char*)` emits
`Content-Length: <n>\r\n\r\n<body>`.

- [ ] **Step 1: write the failing unit test** `tests/unit/test_header_channel.cpp`
  over the codec alone (no sockets): feed a message in three arbitrary splits
  and get exactly one body; two messages in one feed and get both, in order; a
  body containing `\n` bytes (a pretty-printed JSON body) and get it whole; a
  header block with `Content-Type` before `Content-Length` and get the body; a
  missing or non-numeric `Content-Length` and get a refusal, not a hang;
  `encode_message("{}")` == `"Content-Length: 2\r\n\r\n{}"`.
- [ ] **Step 2: run it and watch it fail to compile** (`make -C src test`),
  expected: no such header `madcdis/header_channel.h`.
- [ ] **Step 3: implement the codec.** `HeaderFramedDataChannel` mirrors
  `WebSocketDataChannel`'s layering exactly: it OWNS the inner byte channel, is
  a `DataChannel` whose `read`/`write` are RAW inner bytes, is a
  `PollableDataChannel` passing through the inner poll handle, and carries the
  message layer `feed(data, n)` / `next_message(out, eof, err)` /
  `encode_message(data, n)`. Header parsing reuses the same rule the WebSocket
  handshake uses (case-insensitive `Name: value` lines, CRLF-separated, blank
  line terminates) — lift `header_value` / `trim` / `iequals` into this header
  as shared statics so ONE header-block reader serves both, and have
  `madc_websocket_channel.cpp` include it rather than keep its private copies.
- [ ] **Step 4: wire the facet.** In `src/madc_channel_object.cpp`: a
  `HeaderFramedDataChannel *hfc` alias on `ChannelState` (nulled with `channel`
  in `close()`/`accept()` exactly as `wsc` is); `channel::frame_headers()`
  wrapping the current endpoint (carrying `s->pending` in as leftover);
  `hdr_recv(s, out)` as the park loop (the `ws_recv` shape: pull, park, read
  raw, feed); `readline` and `write` routed through it when set.
- [ ] **Step 5: add `madc_header_channel.o`** to `CORE_OFILES` in `src/Makefile`
  and the `frame_headers()` declaration to both `include/madcdis/channel.h` and
  `include/madc/ns_madc` (the layout contract: the two declarations stay
  signature-identical).
- [ ] **Step 6: run the unit test.** `make -C src test` — expected PASS, zero
  warnings.
- [ ] **Step 7: commit** (engine → four trailers).

### Task 3 — the UTF-16 column owner (engine)

**Files:** create `include/madcdis/text_utf16.h`; modify
`include/madcdis/web_model.h`, `include/madc/ns_ui`, `src/ns_ui.cpp`.
**Test:** extend `tests/unit/test_header_channel.cpp`? No — a new
`tests/unit/test_text_utf16.cpp` (the concerns are unrelated).

**Interfaces — produces:** `size_t madc::web_byte_col(const std::string &line,
long col16)` and `long madc::web_col16(const std::string &line, size_t bytecol)`
in `madcdis/text_utf16.h`; `int64_t ui::text_bytecol(w, entity, line, col16)`
and `int64_t ui::text_col16(w, entity, line, bytecol)` for the dialect (both
1-based `line`, both columns 0-based, both clamping).

- [ ] **Step 1: write the failing unit test** `tests/unit/test_text_utf16.cpp`:
  ASCII round-trips identically; a 2-byte sequence (`é`) is one UTF-16 unit and
  two bytes; a 3-byte sequence (`→`) is one unit and three bytes; a 4-byte
  sequence (an emoji) is TWO units (a surrogate pair) and four bytes; a column
  inside a surrogate pair snaps to the pair's start; a column past the end
  clamps to the line length; `web_col16(web_byte_col(t, c)) == c` for every
  valid c of a mixed line.
- [ ] **Step 2: run it and watch it fail** (`make -C src test`) — no such
  header.
- [ ] **Step 3: create `include/madcdis/text_utf16.h`** holding the MOVED
  `web_byte_col` verbatim plus the new `web_col16` written as its exact
  inverse (walk the same lead-byte table, counting 2 units for a 4-byte
  sequence, stopping at `bytecol`).
- [ ] **Step 4: make `web_model.h` include it** and delete its local copy; its
  one call site (`web_model.h`, the edit-node hit test) is untouched.
- [ ] **Step 5: add the two `ui::` publics** — declaration in
  `include/madc/ns_ui`, implementation in `src/ns_ui.cpp` reading the line's
  text through the same text component the other `text_line_*` publics use and
  delegating to the one owner. No byte walking in `ns_ui.cpp`.
- [ ] **Step 6: run the unit tests** (`make -C src test`) — PASS, zero
  warnings — and a scratch dialect probe in `tmp/` calling both publics on a
  line with a non-ASCII character.
- [ ] **Step 7: commit** (engine → four trailers).

### Task 4 — `madc::canonical_path` (engine)

**Files:** modify `include/madc/ns_madc`, `src/ns_madc.cpp`. **Test:**
`tests/testcanonicalpath.mad` (+ `.expect`, `.expect_quiet`).

**Interfaces — produces:** `value &madc::canonical_path(value &out, const char
*path)` — the comparison spelling of `path` (the standing owner
`detail::canonical_path_for_compare`: `realpath` when it resolves, the input
spelling otherwise).

- [ ] **Step 1: write the failing test** `tests/testcanonicalpath.mad`: a
  relative path and its absolute spelling answer the SAME text; a path with
  `..` collapses; a non-existent path answers its input spelling unchanged
  (never empty); the answer is a `var` the caller owns.
- [ ] **Step 2: run it** — `bin/madc tests/testcanonicalpath.mad` fails: no
  such member `canonical_path`.
- [ ] **Step 3: implement** in `src/ns_madc.cpp` beside the other path-facing
  publics, one line of body over `detail::canonical_path_for_compare`; declare
  it in `include/madc/ns_madc` with the contract comment (WHY it exists: two
  spellings of one file must compare equal).
- [ ] **Step 4: run the test** — PASS; pin `.expect` from the real run.
- [ ] **Step 5: commit** (engine → four trailers).

### Task 5 — the LSP vocabulary (dialect enums + the gate)

**Files:** modify `tools/madcide/madcide_enums.inc`,
`scripts/check-madcide-enums.sh`. **Test:** `bash scripts/check-madcide-enums.sh`.

**Interfaces — produces:**
- `enum lsp_method : unsigned char { lmNONE = 0, lmINITIALIZE, lmINITIALIZED,
  lmSHUTDOWN, lmEXIT, lmDIDOPEN, lmDIDCHANGE, lmDIDSAVE, lmDIDCLOSE,
  lmDOCUMENTSYMBOL, lmHOVER, lmDEFINITION, lmREFERENCES, lmSEMANTICTOKENS,
  lmCANCEL };` with `lsp_method_name(long)` / `lsp_method_of(var &word)`.
- `enum pos_encoding : unsigned char { peNONE = 0, peUTF8, peUTF16 };` with its
  pair (the wire words `utf-8` / `utf-16`).
- `enum lsp_symbol_kind : unsigned char { lskNONE = 0, lskFUNCTION };` +
  `lsp_symbol_number(long)` (the protocol's integer: function = 12).
- `enum lsp_token_type : unsigned char { lttNONE = 0, lttKEYWORD, lttVARIABLE,
  lttNUMBER, lttSTRING, lttCOMMENT, lttTYPE, lttFUNCTION };` with
  `lsp_token_name` (the legend's words) and `lsp_token_of_class(var &cls)` (the
  `parse_spans` class → the token type; `ident` → `lttVARIABLE`).
- `enum lsp_sync : unsigned char { lsNONE = 0, lsFULL, lsINCREMENTAL };`

- [ ] **Step 1: add the five enums with their converter pairs** to
  `madcide_enums.inc`, in the L4-vocabulary style (each with an OUTPUT
  `X_name(long)` and an INPUT `X_of(var &word)` returning the NONE code for an
  unknown word).
- [ ] **Step 2: extend the enum gate**: `madcide_lsp.inc` joins `SEAT_FILES`;
  the awk anchor alternation gains `lsp_method|pos_encoding|lsp_symbol_kind|lsp_token_type|lsp_sync`.
- [ ] **Step 3: run the gate** — `bash scripts/check-madcide-enums.sh`; expected
  GREEN with a higher discriminator-name count than 95, and its negative
  controls still firing.
- [ ] **Step 4: commit** (dialect — no trailers).

### Task 6 — the LSP seat: lifecycle, sync, converters

**Files:** create `tools/madcide/madcide_lsp.inc`; modify
`tools/madcide/madcide.mad`. **Test:** `tests/testmadcide_lsp.mad` (first half).

**Interfaces — consumes:** Task 3's `ui::text_bytecol` / `ui::text_col16`,
Task 4's `madc::canonical_path`, Task 5's enums, and the existing
`edit_file` / `ed_text_insert` / `ed_text_erase` / `refresh_diags` /
`reparse_buffer` / `active_doc`. **Produces:**
- `bool lsp_handle(IdeSession &S, long doc, long self_id, var &req, var &out)` —
  true when `out` holds a reply to write (a notification answers false).
- `void lsp_notify(var &out, const char *method, var &params)` — the
  server→client notification envelope.
- `bool lsp_uri_path(var &out, var &uri)` / `void lsp_path_uri(var &out, var &path)`.
- `long lsp_offset_of(long w, long doc, long enc, var &pos)` and
  `void lsp_position_of(var &out, long w, long doc, long enc, long off)`.
- `long lsp_doc_for(IdeSession &S, long doc, var &tdoc)` — the URI → active
  document resolution (0 = it could not be opened).
- `long run_lsp(const char *path, bool ro)`.

- [ ] **Step 1: write the failing test's first half** — `tests/testmadcide_lsp.mad`
  including the seat, driving `lsp_handle` in-process with `self_id` -1:
  `initialize` with no `general` answers `positionEncoding: "utf-16"` and the
  full capability set; `initialize` offering `["utf-8","utf-16"]` answers
  `"utf-8"`; the `initialized` notification returns false (no reply);
  `didOpen` with text loads the buffer (the session's text equals what was
  sent); an incremental `didChange` splices exactly (text pinned) and a
  full-text change replaces; `didClose` then a request on that URI still
  resolves (the buffer stays, LSP close is not a buffer close); a request with
  a URI outside the session opens it as a new buffer; a `file://` URI with
  `%20` round-trips through both converters; a position on a line with a
  non-ASCII character resolves to the same byte offset under `utf-16` as the
  byte column does under `utf-8`; `shutdown` replies null; an unknown method
  answers `-32601` naming it.
- [ ] **Step 2: run it** — fails: no `madcide_lsp.inc`.
- [ ] **Step 3: write the converters** — `lsp_uri_path` (strip `file://`,
  `js::decodeURIComponent` per path segment, then `madc::canonical_path`),
  `lsp_path_uri` (canonicalize, encode per segment, prefix `file://`),
  `lsp_offset_of` (line start via `ui::text_line_start` + the column through
  `ui::text_bytecol` when the encoding is `peUTF16`, the raw byte column when
  `peUTF8`; clamp to the line's length), `lsp_position_of` (the inverse, via
  `ui::text_line_of` + `ui::text_col16`).
- [ ] **Step 4: write the lifecycle** — `initialize` (read
  `params.general.positionEncodings`, store `penc` on the es bag, answer
  `capabilities` = `textDocumentSync {openClose:true, change:2,
  save:{includeText:false}}`, `hoverProvider`, `definitionProvider`,
  `referencesProvider`, `documentSymbolProvider`, `semanticTokensProvider
  {legend, full:true}`, `positionEncoding`, plus `serverInfo {name:"madcide",
  version}`), `initialized` (no reply), `shutdown` (null result), `exit` (the
  loop ends), `$/cancelRequest` (no reply, no work — v1 runs every request to
  completion).
- [ ] **Step 5: write the document sync** — `lsp_doc_for` (URI → path →
  `edit_file` on the session's world, then `active_doc`), `didOpen`
  (`ui::text_load` with the client's text, then publish diagnostics),
  `didChange` (for each change: a `range` present = `ed_text_erase` over the
  converted span then `ed_text_insert` of the new text; absent = `ui::text_load`;
  then publish), `didSave` (reparse + publish), `didClose` (no buffer close —
  record nothing; the buffer stays open for the other clients).
- [ ] **Step 6: write `run_lsp`** — headless session, `stdio://` +
  `frame_headers()`, the readline loop, `js::parse` refusing with `-32700`,
  `lsp_handle`, `channel.write(js::stringify(resp))`; a parked notification
  queue drained after each message so `publishDiagnostics` reaches the client.
- [ ] **Step 7: wire `--lsp`** into `tools/madcide/madcide.mad` (the flag, the
  usage line, the include after `madcide_mcp.inc`).
- [ ] **Step 8: run the test's first half** — PASS.
- [ ] **Step 9: commit** (dialect — no trailers).

### Task 7 — the six answering methods

**Files:** modify `tools/madcide/madcide_lsp.inc`. **Test:**
`tests/testmadcide_lsp.mad` (second half).

**Interfaces — consumes:** Task 6's converters and `lsp_doc_for`; the existing
`graph_call`, `madc::parse_check` / `parse_outline` / `parse_spans` /
`parse_enclosing`, `ui::text_word_left` / `text_word_right`. **Produces:** the
six method arms plus `void lsp_word_at(var &out, long w, long doc, long off)`
and `void lsp_diagnostics(var &out, IdeSession &S, long adoc)`.

- [ ] **Step 1: write the failing test's second half** — a two-function fixture
  with one deliberate arity error: `publishDiagnostics` carries one error
  diagnostic with `severity` 1 and a range on the right line;
  `documentSymbol` lists both functions with kind 12 and an end line past the
  start; `hover` on the call site names the callee's definition; `definition`
  on the call site answers a Location whose range is the definition's span;
  `references` on the definition answers the call site; `semanticTokens/full`
  answers a `data` array whose length is a multiple of 5, whose first row's
  deltaLine is the first token's 0-based line, and whose token types index the
  legend; `hover` on whitespace answers the enclosing function; `definition` on
  an unknown word answers an empty array, never an error.
- [ ] **Step 2: run it** — fails on the unimplemented methods.
- [ ] **Step 3: implement `lsp_word_at`** — `ui::text_word_right` from the
  offset for the end, `ui::text_word_left` from that end for the start, the
  substring through `perl::substr`, owned immediately into a `var` (ring
  discipline); empty when the offset is not inside a word byte.
- [ ] **Step 4: implement `lsp_diagnostics`** — `refresh_diags`, then each
  `parse_check` row → `{range, severity, source:"madc", message}` with
  `severity` from `severity_code` (`madc::diag_severity::error` → 1, warning →
  2) — the row's ENUMERATOR, never its display word — and the range from the
  row's line/column converted through `lsp_position_of`.
- [ ] **Step 5: implement `documentSymbol`** — `parse_outline` rows through
  `lsp_symbol_number`, `range` = the row's line..end_line, `selectionRange` =
  the name on its own line.
- [ ] **Step 6: implement `hover` / `definition` / `references`** — the word at
  the position, then `graph_call` with `graph.definition` / `graph.references`
  and the node's `span` → a Location (`uri` from the doc's path,
  `range` from the span). Hover's contents is plaintext: the node's
  `kind name` and its line, or the enclosing function from `parse_enclosing`
  when the word resolves to nothing.
- [ ] **Step 7: implement `semanticTokens/full`** — `parse_spans` rows sorted
  by (line, column), delta-encoded; the class → `lsp_token_type` through the
  converter; `tokenModifiers` 0 throughout in v1.
- [ ] **Step 8: run the whole test** — PASS; pin `.expect` from the real run;
  add `.expect_quiet`.
- [ ] **Step 9: run the neighbours** — `testmadcide`, `testmadcide_serve_mcp`,
  `testmcpclient`, `testnexus_layers`, `testgraphpast`, `testmadcide_changelog`
  — all green (the seat's includes and the enums moved).
- [ ] **Step 10: commit** (dialect + test — no trailers).

### Task 8 — the end-to-end stdio gate + the docs

**Files:** create `tests/testmadcide_lsp_stdio.mad` (+ `.expect`,
`.expect_quiet`); modify the two plan docs and `claude_status.json`.

**Interfaces — consumes:** everything above.

- [ ] **Step 1: write the failing test** — it writes a small fixture `.mad`,
  spawns `exec://{madc} --no-config tools/madcide/madcide.mad <fixture> --lsp`
  (the `{madc}` substitution `testmcpclient` uses), calls `frame_headers()` on
  the child channel, and speaks real LSP: `initialize` → the capabilities come
  back framed; `initialized`; `didOpen` → a `publishDiagnostics` notification
  arrives; `textDocument/documentSymbol` → the functions; `shutdown`; `exit`;
  then the child's exit status is 0 after `close()`. It asserts the FRAMING
  itself: a reply's bytes begin with `Content-Length: `.
- [ ] **Step 2: run it** — expect it to fail first on whatever the seat gets
  wrong end-to-end (the in-process test cannot see framing or buffering).
- [ ] **Step 3: fix what it finds**, including `fflush` discipline: the framed
  write must leave libc's buffer before the client waits (the `--mcp` lesson).
- [ ] **Step 4: run it** — PASS; pin `.expect`.
- [ ] **Step 5: run the gates** — `check-madcide-enums.sh`,
  `check-madcide-single-owners.sh`, `check-dialect-literals.sh`,
  `check-dialect-lean.sh`, `check-expect-twins.sh`, `check-rule-trailers.sh`,
  `check-one-delim-tracker.sh`, `check-command-registry.sh` — all green.
- [ ] **Step 6: update the docs** — the V6 plan's V6c-2 row marked SHIPPED with
  its gate; the client-server design §2.7 "as landed" paragraph (what the LSP
  face is, what it reuses, what it defers); `claude_status.json` `live_handoff`
  UPDATE 17.
- [ ] **Step 7: commit** (test + docs), then push to `origin`
  (`derekbsnider/madc`, URL verified before the push).

---

## 4. Self-review

**Spec coverage.** The V6c plan's item 2 names four mappings: `semanticTokens ←
spans` (Task 7 step 7), `publishDiagnostics ← diags` (Task 7 step 4),
`documentSymbol ← outline` (Task 7 step 5), `hover ← parse_enclosing` (Task 7
step 6). Its acceptance framing — "VS Code as the first-class api client" —
is Task 8's end-to-end gate, which drives the real deployed process over the
real framing rather than an in-process shortcut. Design §2.8's invariants:
symmetric capability negotiation is exercised for the first time by decision 5
(the position encoding is the client's offer, answered); the change-event log
stays the ONE replication substrate because decision 10 routes every edit
through `ed_text_insert`/`ed_text_erase`; the general client record is unchanged
(the stdio client is the operator, decision 4). §2.7's "a client declares at
connect what it consumes" is exactly `initialize`. Gap accepted and named:
§2.8's routing slot (`target?`) has no LSP surface — LSP has no node concept —
and that is the deferred socket transport's problem, not this face's.

**Placeholder scan.** No "TBD" / "handle edge cases" / "similar to Task N"
steps; every step names its file, its command or its code shape. The two
deliberately-unspecified values are the `.expect` contents of the three new
tests, which are PINNED FROM THE REAL RUN (the repo's convention — an expect
file invented ahead of the run is a fiction), and the exact `parse_check`
column anchoring, which Task 7 step 4 resolves by reading the row the compiler
actually returns rather than by assuming: the `ns_madc` comment warns that
diagnostics columns are end-anchored where span columns are start-anchored, and
guessing which is the silent-wrong-answer class.

**Type consistency.** `lsp_handle` / `lsp_notify` / `lsp_uri_path` /
`lsp_path_uri` / `lsp_offset_of` / `lsp_position_of` / `lsp_doc_for` /
`lsp_word_at` / `lsp_diagnostics` / `run_lsp` are spelled identically in the
interface blocks of Tasks 6 and 7 and in the file table. The engine names —
`frame_headers`, `HeaderFramedDataChannel`, `web_byte_col`, `web_col16`,
`ui::text_bytecol`, `ui::text_col16`, `madc::canonical_path`, the `stdio://`
scheme — are spelled identically in Tasks 1–4 and in the file table. Every
dialect function returns `void` with an out-param or a scalar: no `var f()`
returning a carrier (the banked dialect trap).

**Ordering.** Tasks 1–4 are independent engine work and could land in any
order; Task 2 consumes Task 1 only in its end-to-end use, not at compile time.
Task 5 must precede 6 (the enums), 6 must precede 7 (the converters), and 8 is
last because it drives the deployed binary.
