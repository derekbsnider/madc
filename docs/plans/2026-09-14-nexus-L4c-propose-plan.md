# Nexus L4c — the `propose` tier, the ONE project-scoped stream, `parse_would_accept`, proposal + decision records, `graph.proposals / proposal / accept / reject / withdraw` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an agent that may not edit PROPOSE an edit: a mutation verb at the
new `proposer` tier stores a validated, previewed, byte-anchored PROPOSAL record
in the change stream instead of touching the buffer; an editor lands it through
the ordinary edit path after three re-checks, or rejects it; the proposer may
withdraw its own. The stream that holds proposals becomes ONE per session
(project-scoped), every record tagged by document, so a proposal about one
file and a splice in another share one `seq` and one persistence rule.

**Architecture:** Four layers, each doing one thing. (1) **Engine**
(`src/madc_program.cpp` + the five-layer plumbing): the ONE validator
`internal_program_parse_refresh_checked` gains a `commit` flag; the new public
`madc::parse_would_accept(out_diags, handle, source)` is the same body with
`commit = false` — validate, never swap. (2) **Texteditor layer**
(`tools/texteditor/editor_events.inc`): the change log becomes ONE entity per
world found by name (`clog_find`), every record carries `doc` and `path`, the
readers fold ONE document's records, compaction writes one checkpoint PER
document at the cut, and a restored log re-attaches its records to the session's
document ids by PATH (`clog_adopt`). (3) **madcide vocabulary + seat**
(`madcide_enums.inc`, `madcide_mcp.inc`, new `madcide_propose.inc`, new
`madcide_seat.inc`): `tierPROPOSER` as the fourth ladder level, `edit_mode`,
`proposal_status`, `graph_edit_apply` over an ops LIST in `emAPPLY | emPROPOSE`
mode (one candidate, one validation, one checkpoint, all splices or none), the
proposal FOLD and the five verbs, and the JSON-line connection loop dispatching
TWO envelopes (the api shape and JSON-RPC) so an MCP client speaks to `--serve`
over a real socket. (4) **Tests**: an engine test for the validator's two modes,
the changelog gate extended with a two-document case, and the connection-level
wiring test the L3 review deferred — real connections, real tiers.

**Tech Stack:** C++11 engine (`src/madc_program.cpp`), the C bridge
(`src/parser.cpp`, `include/ns_common.h`), the dialect wrappers
(`src/ns_madc.cpp`, `include/madc/ns_madc`), the texteditor core and the
madcide seat (dialect `.inc`), value-first `.mad` tests, `madc::channel`
loopback sockets + cooperative tasks (the `serve_tiers` harness).

**Spec:** `docs/plans/2026-09-13-nexus-L4-design.md` (rev 4) §3.1 (the ONE
stream; RULED shape), §3.2 (actors), §3.4 (proposals: the transaction, the
byte-exact re-check, derived status), §3.6 (tiers: `proposer`), §4.4 (the
propose tier in the seat), §5 flow 3 (propose → land), §6 refusals, §7
contracts, §8 L4c tests, §9 slice table, §10 defaults 1 and 4. Owner approval
of the design: 2026-09-13; L4c is the banked NEXT of live_handoff UPDATE 13.

## Decisions this plan settles

1. **Records carry `actor` = the CONNECTION's share id** (`self_id`; `-1` = the
   local operator) beside `es`. Design §3.2 named `es` the identity, but in a
   headless session every connection acts through the ONE `S.es`; the share id
   is what distinguishes a proposer from the editor who lands its proposal.
   `graph.withdraw` at the proposer tier checks `actor == self_id`.
2. **The byte-exact target text rides the op (`op.text`), not `target`.** The
   nodeop record keeps its L3 `target` shape (`graph_span`'s answer: `{id,
   kind, name, span}`); copying the node's bytes into every nodeop would bloat
   the log for no reader. The proposal's ops carry `text` for the landing
   re-check (design §3.4's `target.text`, one level up).
3. **Every record carries `path`** (the doc's `"path"` bag value) beside `doc`.
   A record's `doc` is an entity id of the session that wrote it; a restored
   log (`clog_restore`) would filter every record OUT under the new per-doc
   fold. `clog_adopt(w, doc)` rewrites the `doc` field of the records whose
   `path` is this document's — called by `clog_restore` for the restoring
   document and by `open_buffer_doc` for a document opened later in a
   project. `path` is the asset's durable identity until L4e's asset records.
4. **`clog_restore` refuses to load over a session that already logged events**
   (a message on the es bag, never a silent clobber): with ONE stream, the
   V4 behaviour "the file replaces the doc's log" would now drop OTHER
   documents' in-session records. A fresh session (project startup — the only
   path today's tests exercise) restores exactly as before.
5. **`clog_replay(doc)` answers the LIVE text when the stream holds no record
   of that document** (the "no log yet" rule generalised: a document with no
   history IS at revision 0). With the shared stream a document can have zero
   records while the log entity exists.
6. **The JSON-line connection loop moves to `tools/madcide/madcide_seat.inc`**
   (included after `madcide_mcp.inc`) and dispatches by ENVELOPE — `enum
   ide_envelope { envNONE, envAPI, envRPC }`, read once by `envelope_of(req)`:
   `{"cmd":…}` → `api_run`, `{"jsonrpc":…}` → `mcp_handle`. `serve_api_on` /
   `serve_client_task` keep their names (no call site moves); the four tests
   that drive the loop gain two include lines. ONE line loop over the channel
   (a second loop for MCP would be the divergence class `/dupaudit` hunts);
   `serve_web` already classifies a JSON-RPC line as `2` (a `{` first byte).
7. **`parse_would_accept` on a REVISION handle is allowed** (validation only —
   the read-only refusal exists because a swap would bump the tagged
   generation; a would-accept swaps nothing). `parse_refresh_checked` keeps
   its refusal. Pinned by the engine test.
8. **`graph_min_tier` for `insert / replace / delete` becomes `tierPROPOSER`**;
   the reroute inside `graph_call` picks the MODE: exactly `tierPROPOSER` →
   `emPROPOSE`; any higher tier → `emAPPLY` unless the call carries
   `propose: true`. An observer's refusal prose now reads "requires proposer
   (you are observer)" (no test pinned the old word for a graph verb;
   `delline`'s "requires editor" in `testmadcide_serve_tiers` is a registry
   command and is unchanged).
9. **The proposal fold recomputes per call** (design §3.5's lazily-cached fold
   is L4d's `nexus_fold`; v1 proposal counts are tens). `proposal_fold`
   walks the stream once per verb; status = the LAST decision naming the
   proposal (a decided proposal refuses further decisions, so there is one).
10. **`graph_status`'s hand-rolled error count is replaced by the existing
    `diag_error_count`** (found while reading the seat: a second reader of
    the severity word — the fix-what-you-find rule; one line).

## Global Constraints

Every task's requirements implicitly include this section.

- **ONE validator.** The seat calls `madc::parse_refresh_checked` in exactly
  one place and `madc::parse_would_accept` in exactly one place, both inside
  `graph_edit_apply` (gated: `check-madcide-single-owners.sh`).
- **ONE stream.** No code reads a `"changelog"` key off a document bag after
  this slice; the log is `clog_find(w)` / `changelog_of(w, doc)`.
- **All-or-nothing.** An invalid candidate touches nothing: not the buffer,
  not the tree, not the undo stack, not the log. A rejected proposal stores
  NOTHING.
- **Every refusal is prose, `isError` on the wire** (design §6); the seat tests
  assert each new refusal.
- **Enums, not strings**: `ide_tier`, `edit_mode`, `proposal_status`,
  `ide_envelope` in `madcide_enums.inc`; wire words only through their
  `*_name` / `*_of` converters; dispatch is a `switch` on the code.
- **Value-first dialect code**; `var`; out-param carriers; literals for
  objects; a map is a null `var` made an object by its first keyed write
  (`keyed_get` reads either kind — now in `editor_events.inc`, promoted);
  optional MCP arguments through `arg_of`; `var == var` compares text; no
  mixed-type ternary; `{}` is an empty ARRAY.
- **Rule trailers** on the engine commit; **zero warnings**; targeted tests
  only (the battery rides the V6 seam).
- **Thread contract**: the stream is appended by the session thread only
  (unchanged); the fold is per-call state; connection tiers live in the
  engine's connection cookies (unchanged).

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `src/madc_program.cpp` | modify | `internal_program_parse_refresh_checked(…, bool commit)`: the tagged refusal only when committing; `!commit` → validate and discard |
| `src/parser.cpp`, `include/ns_common.h`, `src/ns_madc.cpp`, `include/madc/ns_madc` | modify | fwd decl gains `commit`; the existing bridge passes `true`; new bridge `madc_parse_would_accept` + wrapper + public |
| `tests/testparsewould.mad` (+ `.expect`, `.expect_quiet`) | create | the validator's two modes at the engine |
| `tools/texteditor/editor_events.inc` | modify | `keyed_get` (promoted); `clog_find`; `changelog_of` by name; `path` on every record; per-doc `clog_replay` / `events_since(doc 0 = all)` / `clog_head` / `clog_compact` (one checkpoint per doc); `clog_adopt` |
| `tools/madcide/madcide_core.inc` | modify | `clog_persist` reads `clog_find`; `clog_restore` guards + adopts; `open_buffer_doc` adopts |
| `tools/madcide/madcide_past.inc` | modify | `keyed_get` removed (promoted); `graph_history` reads `clog_find`; `graph_status` uses `diag_error_count` |
| `tests/testmadcide_changelog.mad` (+ `.expect`) | modify | byte-identical first nine lines + the two-document case |
| `tools/madcide/madcide_enums.inc` | modify | `tierPROPOSER`; `tier_name` / `tier_of_name`; `graph_verb` += five; `edit_mode`; `proposal_status` + converters; `ide_envelope` + `envelope_of`; `graph_min_tier` |
| `tools/madcide/madcide_seat.inc` | create | the JSON-line connection loop: `serve_line_client` (two envelopes), `serve_api_on`, `serve_client_task` (moved from `madcide_api.inc`) |
| `tools/madcide/madcide_api.inc` | modify | the three loop functions removed; header updated |
| `tools/madcide/madcide.mad`, `tools/madcide/madcide_serve.inc` | modify | include `madcide_seat.inc`; the dependency comment |
| `tests/testmadcide_serve_tiers.mad`, `_push.mad`, `_concurrent.mad`, `tests/testmadcide_serve.mad` | modify | two include lines (`madcide_mcp.inc`, `madcide_seat.inc`) |
| `tools/madcide/madcide_propose.inc` | create | `proposal_fold`, `proposal_of`, `graph_proposals`, `graph_proposal`, `graph_accept`, `proposal_decide` |
| `tools/madcide/madcide_mcp.inc` | modify | `#include "madcide_propose.inc"`; `graph_verb_name`; `edit_preconditions`, `edit_op_plan`, `edit_candidate`, `edit_preview`, `graph_edit_apply(ops, mode, proposal_seq)`; descriptors (`propose` on the three mutations; t25–t29); `graph_call` reroute + five cases |
| `scripts/check-madcide-single-owners.sh` | modify | the ONE-validator marker (+ negative control) |
| `scripts/check-madcide-enums.sh` | modify | `tier_name` + `proposal_status_name` join the converter list; the seat files join the checked set |
| `tests/testmadcide_serve_propose.mad` (+ `.expect`, `.expect_quiet`) | create | the connection-level wiring test |
| `docs/plans/2026-09-13-nexus-L4-design.md` | modify | §3.2 actor amendment; §3.1 `path` + adopt; §9 L4c shipped |

---

### Task 1: Engine — `parse_would_accept` (the ONE validator with a `commit` flag)

**Files:**
- Modify: `src/madc_program.cpp` (`internal_program_parse_refresh_checked` at `:5321-5360`)
- Modify: `src/parser.cpp` (fwd decl `:382`; bridge after `madc_parse_refresh_checked` `:1362`),
  `include/ns_common.h` (after `:257`), `src/ns_madc.cpp` (after `:302`),
  `include/madc/ns_madc` (the comment block `:228-236` + the declaration)
- Create: `tests/testparsewould.mad`, `.expect`, `.expect_quiet`

**Interfaces (produces):**
```cpp
namespace madc {
    // Parse `source` into a fresh tree and answer whether it would be ACCEPTED
    // by parse_refresh_checked's rule (no more errors than the live tree) —
    // WITHOUT swapping it in: the live tree, its generation and every id are
    // untouched either way. out_diags = the candidate's diagnostics rows.
    bool parse_would_accept(value &out_diags, int64_t handle, const char *source);
}
// engine internal (the one body): commit=true is parse_refresh_checked, false is parse_would_accept
bool internal_program_parse_refresh_checked(::Program &self, int64_t handle,
                                            const std::string &source_text,
                                            madc::value &out_diags, bool commit);
```

- [ ] **Step 1: The failing engine test** `tests/testparsewould.mad`:

```c
// Nexus L4c: ONE validator, two modes. parse_would_accept answers the
// parse_refresh_checked verdict without the swap — the tree, its generation
// and its ids are untouched; parse_refresh_checked still swaps. A revision
// handle accepts a would-accept (nothing to bump) and refuses the swap.
int main()
{
    var src = "long g = 1;\nlong f(long x) { return x + g; }\n";
    long h = madc::parse_open(src.c_str(), "would.mad");
    long gen0 = madc::parse_generation(h);
    var sy;
    madc::graph_symbols(sy, h);
    var n0 = sy["nodes"][0];
    long id = n0["id"].as_integer();
    var diags;
    var good = "long g = 1;\nlong f(long x) { return x - g; }\n";
    println("would-accept-good: {}", madc::parse_would_accept(diags, h, good.c_str()) ? 1 : 0);
    println("gen-unchanged: {}", madc::parse_generation(h) == gen0 ? 1 : 0);
    var node;
    madc::graph_node(node, h, id);
    println("id-still-fresh: {}", node["name"] == n0["name"] ? 1 : 0);
    var bad = "long g = 1;\nlong f(long x) { return x - ; }\n";
    println("would-accept-bad: {}", madc::parse_would_accept(diags, h, bad.c_str()) ? 0 : 1);
    println("bad-has-diag: {}", php::count(diags) >= 1 ? 1 : 0);
    println("gen-unchanged-after-bad: {}", madc::parse_generation(h) == gen0 ? 1 : 0);
    println("checked-swaps: {}", madc::parse_refresh_checked(diags, h, good.c_str()) ? 1 : 0);
    println("gen-advanced: {}", madc::parse_generation(h) == gen0 + 1 ? 1 : 0);
    madc::graph_node(node, h, id);
    println("old-id-stale: {}", !node["stale"].is_null() && node["stale"].as_boolean() ? 1 : 0);
    long rev = madc::parse_open_tagged(src.c_str(), "would.mad");
    long tag = madc::parse_generation(rev);
    println("would-accept-on-revision: {}", madc::parse_would_accept(diags, rev, good.c_str()) ? 1 : 0);
    println("revision-tag-unchanged: {}", madc::parse_generation(rev) == tag ? 1 : 0);
    println("checked-on-revision-refused: {}", madc::parse_refresh_checked(diags, rev, good.c_str()) ? 0 : 1);
    madc::parse_close(rev);
    madc::parse_close(h);
    return 0;
}
```

`tests/testparsewould.expect`: the twelve lines above each ending in `1`
(`would-accept-good: 1` … `checked-on-revision-refused: 1`).
`tests/testparsewould.expect_quiet`: one line, `stderr must be empty`.

- [ ] **Step 2: Run it to see it fail**: `bash tmp/l3_check.sh tests/testparsewould.mad`
  → `madc::parse_would_accept` unresolved.

- [ ] **Step 3: Engine.** `src/madc_program.cpp`: the signature gains `bool commit`
  (last parameter); the comment above it says "commit=true is
  parse_refresh_checked (swap), commit=false is parse_would_accept (validate,
  discard) — ONE body, one error-count rule"; the tagged-handle refusal
  becomes `if ( commit && graph_handle_is_tagged(st) )`; after the
  error-count compare:

```cpp
    if ( child_error_count(*cand) > child_error_count(*st->child) )
	return false;					// cand deleted here
    if ( !commit )
	return true;					// would accept — nothing swapped, cand deleted
```

- [ ] **Step 4: Plumbing.** `src/parser.cpp:382` fwd decl gains `bool commit`;
  `madc_parse_refresh_checked` passes `true`; after it:

```cpp
// The same validator WITHOUT the swap (Nexus L4c, design §3.4): the
// candidate's verdict + diagnostics, the live tree untouched.
bool madc_parse_would_accept(void *result, int64_t handle, void *source)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;
    madc::value &out = *(madc::value *)result;
    return madc::internal_program_parse_refresh_checked(
	*active, handle, *(const std::string *)source, out, false);
}
```

`include/ns_common.h` after `madc_parse_refresh_checked`:
`bool  madc_parse_would_accept(void *result, int64_t handle, void *source);`.
`src/ns_madc.cpp` after `parse_refresh_checked`:

```cpp
bool parse_would_accept(value &out_diags, int64_t handle, const char *source)
	{ std::string s = source ? source : "";
	  return madc_parse_would_accept(&out_diags, handle, &s); }
```

`include/madc/ns_madc`: extend the comment block with
`//   parse_would_accept(out_diags, handle, source): the same verdict WITHOUT the swap — the live tree, its generation and every id untouched either way (the propose tier's validator).`
and declare `bool parse_would_accept(value &out_diags, int64_t handle, const char *source);`
beside `parse_refresh_checked`.

- [ ] **Step 5: Build + test**: `make -C src 2>&1 | grep -cE 'warning:|error:'` → `0`;
  `bash tmp/l3_check.sh tests/testparsewould.mad` → 0 missing, stderr empty;
  neighbours `testgraphedit`, `testgraphtagged`, `testmadcide_serve_edit` unchanged.

- [ ] **Step 6: Commit (trailers)** — `Hypothesis:` a proposal needs the edit
  verdict without the edit; a second validator would drift from the
  error-count rule. `Layer:` seat → `parse_would_accept` → the ONE internal
  (madc_program.cpp) with a `commit` flag; deepest = where the candidate
  child is built and compared. `Searched:` "validate a candidate TU against
  the live one" → `internal_program_parse_refresh_checked` (the only
  candidate-parse site; `compile_source_child_frontend(self, *cand` once) —
  extended, not copied. `Oracle:` n/a — new capability; tests/testparsewould.mad
  is the oracle (verdict equal in both modes, swap only with commit).

---

### Task 2: The ONE project-scoped stream (texteditor layer + persist/restore) + the two-document changelog case

**Files:**
- Modify: `tools/texteditor/editor_events.inc` (`:221-438`), `tools/madcide/madcide_core.inc`
  (`clog_persist :3549`, `clog_restore :3565`, `open_buffer_doc :3048`),
  `tools/madcide/madcide_past.inc` (`keyed_get :53-60` removed; `:490` the log read;
  `graph_status`'s error loop)
- Modify: `tests/testmadcide_changelog.mad`, `tests/testmadcide_changelog.expect`

**Interfaces (produces):**
```c
void keyed_get(var &out, var &map, const char *key);   // promoted here from madcide_past.inc
long clog_find(long w);                                // the session's log entity; 0 = none yet
long changelog_of(long w, long doc);                   // signature kept; creates the ONE entity on first need
// clog_append stamps seq, ts, and path (the doc's "path" bag value) on every record
void clog_replay(var &out, long w, long doc, long upto);   // folds ONE doc's records; no record -> the live text
void events_since(var &out, long w, long doc, long since); // doc 0 = every doc's events
long clog_head(long w, long doc);                      // the session head (doc kept for the callers)
void clog_compact(long w, long doc, long limit, long keep);  // one checkpoint PER doc at the cut
void clog_adopt(long w, long doc);                     // re-attach restored records to this doc by path
```

- [ ] **Step 1: The failing two-document case** — append to
  `tests/testmadcide_changelog.mad` between the compaction block and the
  persistence block (after `okfail("V4 replay-clamp", …)`), and extend the
  persistence block:

```c
    // Nexus L4c: ONE stream per session. A second document's splices ride the
    // same log; each document replays ONLY its own records; compaction writes
    // one checkpoint PER document at the cut; events_since(doc) filters and
    // events_since(0) answers everything.
    const char *pathB = "madc_ide_clog_b.mad";
    php::file_put_contents(pathB, "");
    long docB = setup_document(w, pathB, false);
    long esB = spawn_view_es(w, docB, pathB);
    ed_text_insert(w, esB, docB, 0, "one");
    ed_text_insert(w, es, doc, 0, "Y");			// doc: "YZ…" — interleaved
    ed_text_insert(w, esB, docB, 3, " two");
    var curB;
    ui::text(curB, w, docB);
    var cur2;
    ui::text(cur2, w, doc);
    var rA;
    clog_replay(rA, w, doc, -1);
    okfail("L4c replay-doc-A", rA == cur2, rA, cur2);
    var rBB;
    clog_replay(rBB, w, docB, -1);
    okfail("L4c replay-doc-B", rBB == curB, rBB, curB);
    var evAll;
    events_since(evAll, w, 0, 0);
    var evB;
    events_since(evB, w, docB, 0);
    println("L4c events-since: all={} docB={}", php::count(evAll), php::count(evB));
    clog_compact(w, doc, 1, 1);				// cut = head - 1: both docs have records at or below it
    long cps = 0;
    var blob2;
    ui::text(blob2, w, le);
    var lines2;
    php::explode(lines2, "\n", blob2.c_str());
    for ( var ln : lines2 )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( js::parse(rec, ln) && clog_kind_of(rec) == ckCHECKPOINT )
	    cps = cps + 1;
    }
    println("L4c per-doc-checkpoints: {}", cps);
    var rA2;
    clog_replay(rA2, w, doc, -1);
    okfail("L4c replay-A-after-compact", rA2 == cur2, rA2, cur2);
    var rB2;
    clog_replay(rB2, w, docB, -1);
    okfail("L4c replay-B-after-compact", rB2 == curB, rB2, curB);
```

and in the persistence block, after `clog_restore(w2, es2, doc2);` +
`okfail("V4 persist-restore", …)` (whose expected text is now `cur2`, the
doc's text after the interleaved insert — rename the variable read there):

```c
    long docB2 = setup_document(w2, pathB, false);	// opened later: adopts its records by path
    clog_adopt(w2, docB2);
    var rrB;
    clog_replay(rrB, w2, docB2, -1);
    okfail("L4c persist-restore-B", rrB == curB, rrB, curB);
    php::unlink(pathB);
```

`tests/testmadcide_changelog.expect` gains the lines
`L4c replay-doc-A: ok`, `L4c replay-doc-B: ok`, `L4c events-since: all=N docB=2`
(pin N on the first run: the surviving records after the first compaction +
the three new splices; write the arithmetic in a comment),
`L4c per-doc-checkpoints: 2`, `L4c replay-A-after-compact: ok`,
`L4c replay-B-after-compact: ok`, `L4c persist-restore-B: ok`. The nine V4
lines stay byte-identical. (`V4 persist-restore` compares against the doc's
CURRENT text — `cur2` after the Y insert — the variable name changes, the
expectation line does not.)

- [ ] **Step 2: Run** `bash tmp/l3_check.sh tests/testmadcide_changelog.mad` →
  `clog_adopt` unresolved / `L4c` lines missing.

- [ ] **Step 3: `editor_events.inc`.** Replace the store paragraph of the
  header comment (`:229-238`) with: "STORE: ONE `changelog` entity per
  SESSION (world), found by name (`clog_find`) — the project-scoped stream
  (Nexus L4c, design §3.1). Every record carries `doc` (the entity) and
  `path` (the asset's durable identity — `clog_adopt` re-attaches a restored
  log's records to this session's ids by it); the readers fold ONE document's
  splice / checkpoint records; compaction writes one checkpoint per document
  at the cut." Then, before `changelog_of`:

```c
// One keyed READ of a map that may still be null or the empty container (`{}`
// is an empty ARRAY; a keyed read before the first keyed write throws). out
// stays null for a missing key. (Promoted from madcide_past.inc, Nexus L4c.)
void keyed_get(var &out, var &map, const char *key)
{
    if ( map.is_object() )
	out = map[key];
}

// The session's change-log entity WITHOUT creating it (0 = none yet): the
// ONE stream every document's records ride (Nexus L4c, design §3.1).
long clog_find(long w)
{
    return ui::entity_by_name(w, "changelog");
}

// The change-log entity, created on first need; seq lives on it. `doc` is the
// caller's document (kept so no caller moves); the entity is the SESSION's.
long changelog_of(long w, long doc)
{
    long le = clog_find(w);
    if ( le > 0 )
	return le;
    le = ui::create(w, "changelog");
    ui::text_load(w, le, "");	// initialise the piece-table component
    ui::set(w, le, "seq", 0);
    return le;
}
```

`clog_append`: after `rec["ts"] = php::time();` add

```c
    var p;
    ui::get(p, w, doc, "path");
    if ( !p.is_null() )
	rec["path"] = p;		// the asset's durable identity (clog_adopt re-attaches by it)
```

`clog_replay`: `long le = clog_find(w);`; declare `bool any = false;` beside
`seen`; inside the loop, right after the torn-line `continue`, add
`if ( rec["doc"].as_integer() != doc ) continue;	// another document's record (the ONE stream)`;
set `any = true;` at the three places the scratch buffer is written (the
clamp load, the checkpoint load, the splice); before the final `ui::text(out,
w, scr)`: `if ( !any ) { ui::text(out, w, doc); return; }	// no record of this doc: the live text IS revision 0`.
Update the "no log yet" comment to say "no log yet".

`clog_head`: `long le = clog_find(w);`. `events_since`: `long le = clog_find(w);`
and in the loop, before the seq test: `if ( doc != 0 && rec["doc"].as_integer() != doc ) continue;	// doc 0 = every document's events`.

`clog_compact` (rewritten body; the header comment gains "one checkpoint PER
document that has a record at or below the cut — each document's replay
starts from its own"):

```c
void clog_compact(long w, long doc, long limit, long keep)
{
    long le = clog_find(w);
    if ( le <= 0 || ui::text_size(w, le) < limit )
	return;
    long cut = es_int(w, le, "seq", 0) - keep;
    if ( cut <= 0 )
	return;
    var blob;
    ui::text(blob, w, le);
    var lines;
    php::explode(lines, "\n", blob.c_str());
    // The documents with a record at or below the cut, in first-seen order,
    // and the path each last carried (the doc entity may be gone).
    var docs = {};
    var seen;			// doc-key -> 1 (a null var becomes the map on its first keyed write)
    var paths;			// doc-key -> path
    for ( var ln : lines )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( !js::parse(rec, ln) || rec["seq"].as_integer() > cut )
	    continue;
	var key = format("{}", rec["doc"]);
	var hit;
	keyed_get(hit, seen, key.c_str());
	if ( hit.is_null() )
	{
	    seen[key.c_str()] = 1;
	    docs.push(rec["doc"]);
	}
	var rp = rec["path"];
	if ( !rp.is_null() )
	    paths[key.c_str()] = rp;
    }
    var rebuilt = "";
    for ( var d : docs )
    {
	var base;
	clog_replay(base, w, d.as_integer(), cut);
	var cp = { "seq": cut, "kind": "checkpoint", "doc": d, "text": base, "ts": php::time() };
	var key = format("{}", d);
	var rp;
	keyed_get(rp, paths, key.c_str());
	if ( !rp.is_null() )
	    cp["path"] = rp;
	var js = js::stringify(cp);
	rebuilt = format("{}{}\n", rebuilt, js);
    }
    for ( var ln : lines )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( !js::parse(rec, ln) )
	    continue;
	if ( rec["seq"].as_integer() > cut )
	    rebuilt = format("{}{}\n", rebuilt, ln.c_str());
    }
    ui::text_load(w, le, rebuilt.c_str());
}

// A restored record's `doc` is an entity id of the SESSION THAT WROTE IT; the
// asset's durable identity is its path. Rewrite every record whose `path` is
// this document's to this document's id, so the per-document fold finds them.
// Called by clog_restore for the restoring document and by a later open of a
// project member; a no-op when nothing matches.
void clog_adopt(long w, long doc)
{
    long le = clog_find(w);
    if ( le <= 0 )
	return;
    var p;
    ui::get(p, w, doc, "path");
    if ( p.is_null() )
	return;
    var blob;
    ui::text(blob, w, le);
    var lines;
    php::explode(lines, "\n", blob.c_str());
    var rebuilt = "";
    bool changed = false;
    for ( var ln : lines )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( !js::parse(rec, ln) )
	{
	    rebuilt = format("{}{}\n", rebuilt, ln.c_str());
	    continue;
	}
	var rp = rec["path"];
	if ( !rp.is_null() && rp == p && rec["doc"].as_integer() != doc )
	{
	    rec["doc"] = doc;
	    changed = true;
	    var js = js::stringify(rec);
	    rebuilt = format("{}{}\n", rebuilt, js);
	}
	else
	    rebuilt = format("{}{}\n", rebuilt, ln.c_str());
    }
    if ( changed )
	ui::text_load(w, le, rebuilt.c_str());
}
```

- [ ] **Step 4: `madcide_core.inc`.** `clog_persist`: `long le = clog_find(w);`
  (comment: "the SESSION's log — one file"). `clog_restore`: after the
  file read succeeds and before loading, guard:

```c
    // ONE stream: loading a file over a session that already logged events
    // would drop the other documents' records (and collide their seqs) —
    // refuse with the reason, never silently (Nexus L4c, decision 4).
    if ( clog_head(w, doc) > 0 )
    {
	ui::set(w, es, "msg", "Project events not restored: this session already logged events.");
	return;
    }
```

and after `ui::set(w, le, "seq", hi);` add `clog_adopt(w, doc);`. Update its
comment: "…seq resumes at the last record's; the restoring document's records
are re-attached to it by path (clog_adopt)". `open_buffer_doc`: after
`if ( nd != 0 ) return nd;` becomes

```c
    if ( nd != 0 )
    {
	clog_adopt(w, nd);		// a project member opened later: its restored records attach
	return nd;
    }
```

- [ ] **Step 5: `madcide_past.inc`.** Delete `keyed_get` (`:53-60`; the
  header comment's "keyed_get" mention stays true — it now lives one layer
  down); `graph_history`: `long le = clog_find(w);`; `graph_status`: replace
  the four-line error loop with `long errors = diag_error_count(diags);`
  (the one reader of the severity code, `madcide_core.inc`).

- [ ] **Step 6: Run**: `bash tmp/l3_check.sh tests/testmadcide_changelog.mad` → pin
  `all=N` (reason in a comment), then 0 missing; `testgraphpast` 12/12;
  `testmadcide_serve_edit` 30/30 ("log records: 3" still — one nodeop + two
  splices); `testmadcide_serve_push`; `testmadcide_serve_graph`; `testvised`
  and `testmadcide` (the texteditor core's pins); `grep -rn '"changelog"'
  tools/ tests/` shows only `ui::create` / `entity_by_name` / comments.

- [ ] **Step 7: Commit** (dialect: no trailers) —
  `feat(changelog): nexus L4c — ONE project-scoped stream per session (clog_find by name; every record carries doc + path; per-document replay / events_since / compaction checkpoints; clog_adopt re-attaches a restored log by path; restore refuses over a live session); keyed_get promoted to the texteditor layer`.

---

### Task 3: The vocabulary + the connection seat — `tierPROPOSER`, `edit_mode`, `proposal_status`, `ide_envelope`; `madcide_seat.inc`

**Files:**
- Modify: `tools/madcide/madcide_enums.inc` (`ide_tier :536`, `tier_name :543`,
  `tier_of_name :554`, `graph_verb :629`, after `nexus_kind_of :660`, `graph_min_tier :680`)
- Create: `tools/madcide/madcide_seat.inc`
- Modify: `tools/madcide/madcide_api.inc` (`:225-287` removed; header `:14-15`),
  `tools/madcide/madcide.mad:49-51`, `tools/madcide/madcide_serve.inc:8-9`,
  `tests/testmadcide_serve_tiers.mad`, `tests/testmadcide_serve_push.mad`,
  `tests/testmadcide_serve_concurrent.mad`, `tests/testmadcide_serve.mad` (include blocks)
- Modify: `scripts/check-madcide-enums.sh` (the converter list + the file set)

**Interfaces (produces):**
```c
enum ide_tier : unsigned char { tierOBSERVER = 0, tierPROPOSER, tierEDITOR, tierOWNER };
enum graph_verb : unsigned char { …, gvDIFF, gvPROPOSALS, gvPROPOSAL, gvACCEPT, gvREJECT, gvWITHDRAW };
enum edit_mode : unsigned char { emAPPLY = 0, emPROPOSE };
enum proposal_status : unsigned char { psNONE = 0, psOPEN, psACCEPTED, psREJECTED, psWITHDRAWN };
const char *proposal_status_name(long s);    // "open" | "accepted" | "rejected" | "withdrawn" | ""
long proposal_status_of(var &word);          // psNONE = unknown / null
enum ide_envelope : unsigned char { envNONE = 0, envAPI, envRPC };
long envelope_of(var &req);                  // {"cmd"} -> envAPI; {"jsonrpc"} -> envRPC
long graph_min_tier(long code);              // mutations -> tierPROPOSER; accept/reject -> tierEDITOR; withdraw -> tierPROPOSER
// madcide_seat.inc
void serve_line_client(IdeSession &S, long doc, madc::channel &client, long self_id);
void serve_api_on(IdeSession &S, long doc, madc::channel &client);   // unchanged name/contract
void serve_client_task(IdeSession *S, long doc, long conn);          // unchanged name/contract
```

- [ ] **Step 1: Enums.** `ide_tier` becomes the four-level ladder; its comment
  gains "a PROPOSER may not mutate but may store a proposal for an editor to
  land (Nexus L4c, design §3.6) — every `<` / `>=` keeps its meaning".
  `tier_name`: `case tierPROPOSER: return "proposer";`; `tier_of_name`:
  `if ( strcmp(name, "proposer") == 0 ) return tierPROPOSER;`. `graph_verb`:
  append after `gvDIFF`:
  `// Nexus L4c (design §3.4, §4.4): the FUTURE verbs — the proposal fold + decisions.`
  `gvPROPOSALS, gvPROPOSAL, gvACCEPT, gvREJECT, gvWITHDRAW`. After
  `nexus_kind_of`:

```c
// How graph_edit_apply ends (Nexus L4c, design §3.4): APPLY commits the
// validated candidate; PROPOSE stores it as a proposal record and touches
// nothing else. One path up to the candidate's validation; the mode decides.
enum edit_mode : unsigned char { emAPPLY = 0, emPROPOSE };

// A proposal's status — DERIVED from the stream (design §3.4): open until a
// decision record names the proposal. The word is the wire form (a decision
// record's `decision`, a fold row's `status`); converted ONCE by
// proposal_status_of, written only through proposal_status_name.
enum proposal_status : unsigned char { psNONE = 0, psOPEN, psACCEPTED, psREJECTED, psWITHDRAWN };
const char *proposal_status_name(long s)
{
    switch ( s )
    {
	case psOPEN: return "open";
	case psACCEPTED: return "accepted";
	case psREJECTED: return "rejected";
	case psWITHDRAWN: return "withdrawn";
    }
    return "";
}
long proposal_status_of(var &word)
{
    if ( word.is_null() )
	return psNONE;
    for ( long k = psOPEN; k <= psWITHDRAWN; k = k + 1 )
	if ( word == proposal_status_name(k) )
	    return k;
    return psNONE;
}

// The ENVELOPE a JSON line on the connection seat carries (Nexus L4c): the api
// shape {"cmd", "args", "seq"} or a JSON-RPC 2.0 message {"jsonrpc", "id",
// "method", "params"} — the MCP seat over the same socket. Read ONCE at the
// line; the loop switches on the code. envNONE = neither (a refusal).
enum ide_envelope : unsigned char { envNONE = 0, envAPI, envRPC };
long envelope_of(var &req)
{
    if ( !req["jsonrpc"].is_null() )
	return envRPC;
    if ( !req["cmd"].is_null() )
	return envAPI;
    return envNONE;
}
```

`graph_min_tier` (comment updated: "the three mutations need the PROPOSER
tier — at exactly that tier graph_call stores a proposal, above it the edit
lands; accept / reject are editor decisions; withdraw is the proposer's own
or an editor's"):

```c
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:
    case gvWITHDRAW:
	return tierPROPOSER;
    case gvACCEPT:
    case gvREJECT:
	return tierEDITOR;
```

- [ ] **Step 2: `tools/madcide/madcide_seat.inc`** — move `serve_api_client`
  (renamed `serve_line_client`), `serve_api_on` and `serve_client_task` out
  of `madcide_api.inc` (lines `:225-287`, comments included) into the new file
  with this header, and the envelope switch in the loop:

```c
// madcide_seat.inc — the JSON-LINE CONNECTION SEAT (client-server V6a, the
// serve loop; Nexus L4c gave it its second envelope). ONE loop reads one JSON
// line per message off an accepted connection and dispatches by ENVELOPE
// (envelope_of, madcide_enums.inc — read once, a switch on the code):
//   envAPI  {"cmd","args","seq"}     -> api_run (madcide_api.inc), reply {seq, ok, …}
//   envRPC  {"jsonrpc","id",…}       -> mcp_handle (madcide_mcp.inc), the JSON-RPC reply
// so an MCP client (an agent host) speaks to `--serve` over the same socket the
// api client uses, under the SAME tier roster (grant_tier / clienttier). The
// engine's serve_web already classifies either line as an api client (a `{`
// first byte). Depends on madcide_api.inc AND madcide_mcp.inc — included after
// both; the tests that drive serve_client_task include all three.
```

In the loop body:

```c
	var req;
	if ( !js::parse(req, line) )
	{
	    api_refuse(client, 0, "malformed JSON request");
	    continue;
	}
	long env = envelope_of(req);
	if ( env == envRPC )
	{
	    var resp;
	    if ( mcp_handle(S, doc, self_id, req, resp) )	// a notification has no reply
		api_write(client, resp);
	    continue;
	}
	long seq = req["seq"].as_integer();
	if ( env != envAPI )
	{
	    api_refuse(client, seq, "request has no 'cmd'");
	    continue;
	}
	var name = req["cmd"];
	var reply;
	api_run(reply, S, doc, self_id, name.c_str(), api_arg(req));
	reply["seq"] = seq;
	api_write(client, reply);
```

`madcide_api.inc`'s header line 14–15 becomes "The connection loop that
carries this envelope (and the MCP envelope beside it) is madcide_seat.inc;
the MCP seat (madcide_mcp.inc) and the LSP endpoint are adapters over
api_run." `madcide.mad`: `#include "madcide_seat.inc"` after
`madcide_mcp.inc`. `madcide_serve.inc:8-9`: "Depends on madcide_seat.inc
(serve_api_on) and madcide_ws.inc (serve_ws_client)." The four tests: add
`#include "../tools/madcide/madcide_mcp.inc"` and
`#include "../tools/madcide/madcide_seat.inc"` after their
`madcide_api.inc` line (if `madcide_mcp.inc` turns out to need
`madcide_client.inc`, add that line before it and record the fact).

- [ ] **Step 3: The enum gate.** `scripts/check-madcide-enums.sh`: the `awk`
  anchor list gains `tier|proposal_status`; the checked file set gains
  `MCP="$ROOT/tools/madcide/madcide_mcp.inc"`, `PAST=…madcide_past.inc`,
  `PROPOSE=…madcide_propose.inc`, `SEAT=…madcide_seat.inc` passed to every
  `grep` beside `$core $client $once` (extend `check`'s parameters; keep the
  negative controls working). Run it; a hit in the seat files is a string
  discriminator to convert (expected: none — `d["severity"] == "error"` left
  with Task 2's `diag_error_count` replacement).

- [ ] **Step 4: Run**: `bash scripts/check-madcide-enums.sh`,
  `bash scripts/check-madcide-single-owners.sh`, `bash scripts/check-dialect-literals.sh`,
  `bash scripts/check-dialect-lean.sh` GREEN; `testmadcide_serve_tiers` 3/3
  (byte-identical: `delline` still "requires editor"), `testmadcide_serve_push`,
  `testmadcide_serve_concurrent`, `testmadcide_serve`, `testmadcide_serve_web`
  unchanged; `testmadcide_serve_mcp` unchanged (stdio path).

- [ ] **Step 5: Commit** (dialect + script: no trailers) —
  `feat(madcide): nexus L4c — the proposer tier (fourth ladder level), edit_mode / proposal_status / ide_envelope enums; the JSON-line connection seat (madcide_seat.inc) dispatches the api AND the JSON-RPC envelope so an MCP client reaches --serve over a socket under the one tier roster`.

---

### Task 4: The seat — `graph_edit_apply` over ops + mode, `madcide_propose.inc`, the five verbs, the reroute

**Files:**
- Create: `tools/madcide/madcide_propose.inc`
- Modify: `tools/madcide/madcide_mcp.inc` (include at the top; descriptors `:153-186`;
  after `graph_verb_of :235` `graph_verb_name`; `graph_edit_apply :349-555` split;
  `graph_call :587-688`)
- Modify: `scripts/check-madcide-single-owners.sh` (the ONE-validator marker)

**Interfaces (produces — the wire):**

| Verb | Arguments | Answer |
|---|---|---|
| `graph.insert / replace / delete` at `proposer` (or `propose: true`) | as L3 + `propose?` | `{ok:true, proposal: seq, preview:{line0,line1,before,after}, diagnostics, constraints:[]}` · rejected: `{ok:false, error, diagnostics}` (nothing stored) |
| `graph.proposals` | `status?` (`open\|accepted\|rejected\|withdrawn`; default all) | `{rows:[{seq, status, actor, base_seq, ts, ops, verb, kind, name}]}` |
| `graph.proposal` | `seq` | the record + `status` (+ `decision` when decided) · `{error:"no proposal N"}` |
| `graph.accept` | `seq` (editor) | `{ok, proposal, node, seq, landed_seq}` · `{error:"proposal N is <status>"}` · `{error:"proposal target changed since seq N; re-propose against fresh ids"}` · the L3 re-sync refusal |
| `graph.reject` | `seq`, `reason?` (editor) | `{ok, proposal, status:"rejected"}` · the not-open refusal |
| `graph.withdraw` | `seq` (proposer: own; editor: any) | `{ok, proposal, status:"withdrawn"}` · `{error:"proposal N is not yours (actor A)"}` |

The internal contracts (`madcide_mcp.inc`):

```c
void graph_verb_name(var &out, long gv);                                   // code -> wire name over the descriptor table
bool edit_preconditions(var &why, long w, long es, long adoc, var &t);     // editable + synced; fills t
bool edit_op_plan(var &op, long w, long adoc, long h, var &t, long gv, var &args);
// op = { verb, where, target:{id,kind,name,span}, text, src, splice:{at,del,ins}, line, column }
void edit_candidate(var &cand, var &order, long w, long adoc, var &t, var &ops);   // highest `at` first
void edit_preview(var &out, long w, long adoc, var &t, var &cand, var &ops);
void graph_edit_apply(var &out, IdeSession &S, long doc, long self_id, var &ops, long mode, long proposal_seq);
// emAPPLY answer: { ok, node, seq, nodeop_seq, splice_at, splice_del }
```

- [ ] **Step 1: `graph_verb_name`** after `graph_verb_of`:

```c
// The OUTPUT converter: a verb code to the wire name (the nodeop record's
// `verb`, a proposal op's `verb`), over the same descriptor table.
void graph_verb_name(var &out, long gv)
{
    var tools;
    graph_tool_descriptors(tools);
    for ( var t : tools )
	if ( t["code"] == gv )
	{
	    out = t["name"];
	    return;
	}
    out = "";
}
```

- [ ] **Step 2: Split `graph_edit_apply`.** Replace `:349-555` with the four
  helpers + the new apply. The L3 comment block ("ONE node-op … Order
  matters") stays above `edit_op_plan` with its step list renumbered to
  name the helpers; `graph_edit_apply`'s comment describes the transaction
  (design §3.4).

```c
// The L3 preconditions every mutation shares: the document is editable (the
// ONE read-only rule — derive_access's verdict) and the tree is THIS text's
// parse (a desynced tree is re-synced so the caller's ids go loudly stale).
// false with `why` = the refusal prose; `t` = the live text either way.
bool edit_preconditions(var &why, long w, long es, long adoc, var &t)
{
    ui::text(t, w, adoc);
    if ( !es_flag(w, es, "editable") )
    {
	var ro;
	ui::get(ro, w, es, "ro_msg");
	why = "the document is read-only";
	if ( !ro.is_null() )
	    why = ro;
	return false;
    }
    var pt;
    ui::get(pt, w, adoc, "phandle_text");
    if ( pt.is_null() || !(pt == t) )
    {
	reparse_buffer(w, es, adoc);
	why = "the buffer changed since these ids were minted (the tree has been re-synced to it) - re-query";
	return false;
    }
    return true;
}

// ONE node-op PLAN: verb + args -> the op (design §3.4's ops[] element).
//   target = graph_span's answer {id, kind, name, span} (the tree's extent, never
//            an agent offset); text = the node's bytes now (the byte-exact
//            re-check at landing); splice = {at, del, ins} for replace / delete
//            / insert with the L3 line rules; line/column = where the edited
//            node starts afterwards (the answer's graph.at).
// false with op = {error} on a miss.
bool edit_op_plan(var &op, long w, long adoc, long h, var &t, long gv, var &args)
{
    long id = args["id"].as_integer();
    var srcv;
    arg_of(srcv, args, "src");
    var src = "";
    if ( !srcv.is_null() )
	src = srcv;
    long where = gwNONE;
    var wherev = "";
    if ( gv == gvINSERT )
    {
	var wv;
	arg_of(wv, args, "where");
	if ( !wv.is_null() )
	{
	    where = graph_where_of(wv.c_str());
	    wherev = wv;
	}
	if ( where == gwNONE )
	{
	    op = { "error": "graph.insert needs where = before | after" };
	    return false;
	}
    }
    if ( gv != gvDELETE && strlen(src.c_str()) == 0 )
    {
	op = { "error": "src is empty (use graph.delete to remove a node)" };
	return false;
    }
    var sp;
    madc::graph_span(sp, h, id);
    var span = sp["span"];
    if ( span.is_null() )
    {
	op = { "error": sp["error"] };
	return false;
    }
    … (the L3 body verbatim: l0/c0/l1/c1, size, at/end, del/ins/new_line/new_col,
       indent, owns, the switch on gv — with `at0 = at` and `end0 = end` captured
       right after they are computed, before the delete case widens them)
    var text = perl::substr(t, at0, end0 - at0);
    var verb;
    graph_verb_name(verb, gv);
    var splice = { "at": at, "del": del, "ins": ins };
    op = { "verb": verb, "where": wherev, "target": sp, "text": text, "src": src,
	   "splice": splice, "line": new_line, "column": new_col };
    return true;
}

// The CANDIDATE: the live text with EVERY op's splice applied on the doc's
// scratch buffer (replaybuf_of, the engine's text_replace — no string surgery),
// HIGHEST offset first so an earlier splice never moves a later target (design
// §3.4: N ops, one candidate). `order` = the op indices in that order — the
// commit walks the same order.
void edit_candidate(var &cand, var &order, long w, long adoc, var &t, var &ops)
{
    long n = php::count(ops);
    var taken = {};
    for ( long i = 0; i < n; i = i + 1 )
	taken.push(0);
    order = {};
    for ( long k = 0; k < n; k = k + 1 )
    {
	long best = -1;
	long bestat = -1;
	for ( long i = 0; i < n; i = i + 1 )
	{
	    if ( taken[i].as_integer() != 0 )
		continue;
	    var s = ops[i]["splice"];
	    long at = s["at"].as_integer();
	    if ( best < 0 || at > bestat )
	    {
		best = i;
		bestat = at;
	    }
	}
	taken[best] = 1;
	order.push(best);
    }
    long scr = replaybuf_of(w, adoc);
    ui::text_load(w, scr, t.c_str());
    for ( var iv : order )
    {
	var s = ops[iv.as_integer()]["splice"];
	var ins = s["ins"];
	ui::text_replace(w, scr, s["at"].as_integer(), s["del"].as_integer(), ins.c_str());
    }
    ui::text(cand, w, scr);
}

// The proposal's PREVIEW (design §3.4): the touched LINES, live vs candidate.
// line0/line1 = the first and last live line any splice touches; `before` =
// those lines' text, `after` = the same region in the candidate (its end
// shifted by the net byte delta).
void edit_preview(var &out, long w, long adoc, var &t, var &cand, var &ops)
{
    long lo = -1;
    long hi = -1;
    long delta = 0;
    for ( var op : ops )
    {
	var s = op["splice"];
	long at = s["at"].as_integer();
	long del = s["del"].as_integer();
	if ( lo < 0 || at < lo )
	    lo = at;
	if ( at + del > hi )
	    hi = at + del;
	delta = delta + strlen(s["ins"].c_str()) - del;
    }
    long line0 = line_of(w, adoc, lo);
    long line1 = line_of(w, adoc, hi > lo ? hi - 1 : hi);
    long rs = line_start_of(w, adoc, line0);
    long re = line_start_of(w, adoc, line1) + ui::text_line_len(w, adoc, line1);
    if ( re < hi )
	re = hi;
    var before = perl::substr(t, rs, re - rs);
    var after = perl::substr(cand, rs, re + delta - rs);
    out = { "line0": line0, "line1": line1, "before": before, "after": after };
}

// The TRANSACTION (design §3.4): every op's splice on ONE candidate, ONE
// validation, then — emAPPLY — one undo checkpoint, a nodeop record per op
// AHEAD of its splices, every splice through the ONE text-mutation owner
// (highest offset first), spans refreshed, the hub fan-out; or — emPROPOSE —
// the candidate's verdict WITHOUT the swap (parse_would_accept), the preview,
// ONE proposal record {ops, preview, diagnostics, constraints, checks,
// base_seq, actor}, and nothing else touched (not the buffer, not the tree,
// not the undo stack). A rejected candidate answers {ok:false, diagnostics}
// and stores NOTHING in either mode. `proposal_seq` > 0 = this landing is a
// proposal's: the nodeops carry it. The ONE seat of both validator calls.
void graph_edit_apply(var &out, IdeSession &S, long doc, long self_id, var &ops, long mode, long proposal_seq)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    var t;
    var why;
    if ( !edit_preconditions(why, w, es, adoc, t) )
    {
	out = { "ok": false, "error": why };
	return;
    }
    var cand;
    var order;
    edit_candidate(cand, order, w, adoc, t, ops);
    var diags;
    if ( mode == emPROPOSE )
    {
	if ( !madc::parse_would_accept(diags, h, cand.c_str()) )
	{
	    out = { "ok": false, "error": "proposal rejected: the result introduces a parse/semantic error (nothing stored)",
		    "diagnostics": diags };
	    return;
	}
	var preview;
	edit_preview(preview, w, adoc, t, cand, ops);
	var constraints = {};		// L4d: the decisions linked to the target's symbol key
	var checks = {};		// L4e: linked test runs
	long base = clog_head(w, adoc);
	var rec = { "kind": "proposal", "es": es, "doc": adoc, "actor": self_id, "base_seq": base,
		    "ops": ops, "preview": preview, "diagnostics": diags,
		    "constraints": constraints, "checks": checks };
	long pseq = clog_append(w, adoc, rec);
	broadcast_events(w, adoc, base, self_id);
	out = { "ok": true, "proposal": pseq, "preview": preview, "diagnostics": diags,
		"constraints": constraints };
	return;
    }
    bool accepted = madc::parse_refresh_checked(diags, h, cand.c_str());
    if ( !accepted )
    {
	out = { "ok": false, "error": "edit rejected: the result introduces a parse/semantic error (nothing changed)",
		"diagnostics": diags };
	return;
    }
    var t2;
    ui::text(t2, w, adoc);
    if ( !(t2 == t) )			// var == var compares the text (never two ring c_str()s)
    {
	reparse_buffer(w, es, adoc);	// re-sync the handle to the live text
	out = { "ok": false, "error": "buffer changed while validating: nothing applied, re-query" };
	return;
    }
    ui::set(w, adoc, "phandle_text", cand);	// the swapped tree describes the post-splice text
    long head_before = clog_head(w, adoc);
    edit_checkpoint(w, es, adoc, clamped_caret(w, es, adoc));	// ONE undo step for the transaction
    long nodeop_seq = 0;
    for ( var iv : order )
    {
	var op = ops[iv.as_integer()];
	var rec = { "kind": "nodeop", "es": es, "doc": adoc, "actor": self_id, "verb": op["verb"],
		    "target": op["target"], "src": op["src"] };
	if ( proposal_seq > 0 )
	    rec["proposal"] = proposal_seq;
	long ns = clog_append(w, adoc, rec);
	if ( nodeop_seq == 0 )
	    nodeop_seq = ns;
	var s = op["splice"];
	long at = s["at"].as_integer();
	long del = s["del"].as_integer();
	var ins = s["ins"];
	if ( del > 0 )
	    ed_text_erase(w, es, adoc, at, del);
	if ( strlen(ins.c_str()) > 0 )
	    ed_text_insert(w, es, adoc, at, ins.c_str());
    }
    refresh_spans(w, es, adoc, h);
    broadcast_events(w, adoc, head_before, self_id);
    var first = ops[0];
    var node = {};
    if ( graph_verb_of(first["verb"].c_str()) != gvDELETE )
	madc::graph_at(node, h, first["line"].as_integer(), first["column"].as_integer());
    var fs = first["splice"];
    long seq = clog_head(w, adoc);
    out = { "ok": true, "node": node, "seq": seq, "nodeop_seq": nodeop_seq,
	    "splice_at": fs["at"], "splice_del": fs["del"] };
}
```

- [ ] **Step 3: `tools/madcide/madcide_propose.inc`** (included by
  `madcide_mcp.inc` right after `madcide_past.inc`; it uses `graph_source`,
  `edit_preconditions`, `edit_op_plan`, `graph_edit_apply` — the latter three
  are defined later in `madcide_mcp.inc`, so the include line goes AFTER
  `graph_edit_apply` and BEFORE `graph_call`; say so in both headers):

```c
// madcide_propose.inc — the FUTURE axis of the code-graph MCP (Nexus L4c;
// design docs/plans/2026-09-13-nexus-L4-design.md §3.4, §3.6, §4.4; plan
// 2026-09-14-nexus-L4c-propose-plan.md). The proposal FOLD over the ONE stream
// and the five verbs. Included by madcide_mcp.inc AFTER graph_edit_apply
// (graph_accept lands through it) and BEFORE graph_call (which dispatches here).
//
// Which structure answers what:
//   proposals / proposal — this doc's proposal records + the decision naming each
//                          (status DERIVED: open until a decision; the last wins)
//   accept   — the fold (open?), the L3 preconditions, the three re-checks PER OP
//              against the fresh tree, the ordinary graph_edit_apply(emAPPLY),
//              then a decision record {accepted, landed_seq}
//   reject / withdraw — the fold (open?; withdraw at the proposer tier: own only),
//              a decision record
// Thread contract: the fold is per-call state over the session-thread stream.

// Fold this document's proposals: `seqs` (creation order) and `props` (a map
// seq-key -> the record with `status` (the wire word) and `decision` (the
// deciding record) — a null var made an object by its first keyed write).
void proposal_fold(var &seqs, var &props, long w, long adoc)
{
    seqs = {};
    long le = clog_find(w);
    if ( le <= 0 )
	return;
    var blob;
    ui::text(blob, w, le);
    var lines;
    php::explode(lines, "\n", blob.c_str());
    for ( var ln : lines )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( !js::parse(rec, ln) )
	    break;			// a torn tail line: stop trusting the rest
	if ( rec["doc"].as_integer() != adoc || clog_kind_of(rec) != ckOTHER )
	    continue;
	long nk = nexus_kind_of(rec);
	if ( nk == nkPROPOSAL )
	{
	    var key = format("{}", rec["seq"]);
	    rec["status"] = proposal_status_name(psOPEN);
	    props[key.c_str()] = rec;
	    seqs.push(rec["seq"]);
	}
	else if ( nk == nkDECISION )
	{
	    var key = format("{}", rec["proposal"]);
	    var p;
	    keyed_get(p, props, key.c_str());
	    if ( p.is_null() )
		continue;		// a decision naming no proposal here: never trusted
	    p["status"] = rec["decision"];	// the LAST decision wins (design §3.4)
	    p["decision"] = rec;
	    props[key.c_str()] = p;
	}
    }
}

// One proposal by seq: `out` = the folded record, or left null.
void proposal_of(var &out, long w, long adoc, long seq)
{
    var seqs;
    var props;
    proposal_fold(seqs, props, w, adoc);
    var key = format("{}", seq);
    keyed_get(out, props, key.c_str());
}

// graph.proposals(status?): compact rows, creation order; `want` filters
// (psNONE = every status).
void graph_proposals(var &out, long w, long adoc, long want)
{
    var seqs;
    var props;
    proposal_fold(seqs, props, w, adoc);
    var rows = {};
    for ( var sv : seqs )
    {
	var key = format("{}", sv);
	var p;
	keyed_get(p, props, key.c_str());
	if ( want != psNONE && proposal_status_of(p["status"]) != want )
	    continue;
	var ops = p["ops"];
	var first = ops[0];
	var tgt = first["target"];
	long nops = php::count(ops);
	var row = { "seq": p["seq"], "status": p["status"], "actor": p["actor"], "base_seq": p["base_seq"],
		    "ts": p["ts"], "ops": nops, "verb": first["verb"], "kind": tgt["kind"], "name": tgt["name"] };
	rows.push(row);
    }
    out = { "rows": rows };
}

// graph.proposal(seq): the whole folded record.
void graph_proposal(var &out, long w, long adoc, long seq)
{
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    out = p;
}

// graph.accept(seq): land an OPEN proposal through the ORDINARY apply after
// the three re-checks per op (design §3.4): (1) the doc is text-synced (the
// L3 precondition); (2) the node at the target's span start has the same
// kind + name; (3) its text equals the proposal's byte-exact copy. Any miss
// refuses and the proposal stays open. The ops are RE-PLANNED against the
// fresh tree (a stored id is generation-bound) — one plan owner, edit_op_plan.
void graph_accept(var &out, IdeSession &S, long doc, long self_id, long seq)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    if ( proposal_status_of(p["status"]) != psOPEN )
    {
	var why = format("proposal {} is {}", seq, p["status"]);
	out = { "error": why };
	return;
    }
    long h = ensure_phandle(w, es, adoc);
    var t;
    var why;
    if ( !edit_preconditions(why, w, es, adoc, t) )
    {
	out = { "error": why };
	return;
    }
    var changed = format("proposal target changed since seq {}; re-propose against fresh ids", seq);
    var fresh = {};
    for ( var op : p["ops"] )
    {
	var tgt = op["target"];
	var span = tgt["span"];
	var node;
	madc::graph_at(node, h, span["line"].as_integer(), span["column"].as_integer());
	if ( node["id"].is_null() || !(node["kind"] == tgt["kind"]) || !(node["name"] == tgt["name"]) )
	{
	    out = { "error": changed };
	    return;
	}
	var src;
	graph_source(src, w, adoc, h, h, node["id"].as_integer());
	if ( !(src["text"] == op["text"]) )
	{
	    out = { "error": changed };
	    return;
	}
	var args = { "id": node["id"], "src": op["src"], "where": op["where"] };
	var fop;
	if ( !edit_op_plan(fop, w, adoc, h, t, graph_verb_of(op["verb"].c_str()), args) )
	{
	    out = { "error": fop["error"] };
	    return;
	}
	fresh.push(fop);
    }
    var landed;
    graph_edit_apply(landed, S, doc, self_id, fresh, emAPPLY, seq);
    if ( !landed["ok"].as_boolean() )
    {
	out = { "error": landed["error"], "diagnostics": landed["diagnostics"] };
	return;
    }
    long head = clog_head(w, adoc);
    var dec = { "kind": "decision", "es": es, "doc": adoc, "actor": self_id, "proposal": seq,
		"decision": proposal_status_name(psACCEPTED), "landed_seq": landed["nodeop_seq"], "reason": "" };
    clog_append(w, adoc, dec);
    broadcast_events(w, adoc, head, self_id);
    out = { "ok": true, "proposal": seq, "node": landed["node"], "seq": landed["seq"],
	    "landed_seq": landed["nodeop_seq"] };
}

// A decision on an OPEN proposal (graph.reject / graph.withdraw): the record
// and the answer. `want` = the status the decision sets; `own_only` = the
// proposer tier's rule (only the proposal's own actor may withdraw it).
void proposal_decide(var &out, long w, long es, long adoc, long self_id, long seq, long want,
		     const char *reason0, bool own_only)
{
    var reason = reason0;		// ring discipline
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    if ( proposal_status_of(p["status"]) != psOPEN )
    {
	var why = format("proposal {} is {}", seq, p["status"]);
	out = { "error": why };
	return;
    }
    if ( own_only && p["actor"].as_integer() != self_id )
    {
	var why = format("proposal {} is not yours (actor {})", seq, p["actor"]);
	out = { "error": why };
	return;
    }
    long head = clog_head(w, adoc);
    var dec = { "kind": "decision", "es": es, "doc": adoc, "actor": self_id, "proposal": seq,
		"decision": proposal_status_name(want), "landed_seq": 0, "reason": reason };
    clog_append(w, adoc, dec);
    broadcast_events(w, adoc, head, self_id);
    out = { "ok": true, "proposal": seq, "status": proposal_status_name(want) };
}
```

- [ ] **Step 4: Descriptors + `graph_call`.** In `graph_tool_descriptors`: a
  `proposeprop = { "type": "boolean", "description": "store the edit as a PROPOSAL for an editor to accept instead of applying it (the proposer tier always does; optional)" }`;
  `insprops` gains `"propose": proposeprop`, `repprops` too; `graph.delete`
  gets its own `delprops = { "id": idprop, "propose": proposeprop }` /
  `delargs`; the three descriptions say "(proposer tier stores a proposal;
  editor tier applies)". After `t24`:

```c
    // Nexus L4c (design §3.4, §4.4): the FUTURE verbs. A proposal is a stored,
    // validated, previewed node-op transaction; its status is derived from the
    // decision records; landing re-checks the target byte-exact.
    var seqprop = { "type": "integer", "description": "a proposal seq (from a proposer's graph.insert/replace/delete answer or graph.proposals)" };
    var statusprop = { "type": "string", "description": "open | accepted | rejected | withdrawn (optional; default: every status)" };
    var reasonprop = { "type": "string", "description": "why (optional)" };
    var seqprops = { "seq": seqprop };
    var seqargs = { "type": "object", "properties": seqprops };
    var statusprops = { "status": statusprop };
    var statusargs = { "type": "object", "properties": statusprops };
    var rejprops = { "seq": seqprop, "reason": reasonprop };
    var rejargs = { "type": "object", "properties": rejprops };
    var t25 = { "name": "graph.proposals", "code": gvPROPOSALS,
	"description": "the served file's proposals (compact rows; optional status filter)",
	"inputSchema": statusargs };
    tools[] = t25;
    var t26 = { "name": "graph.proposal", "code": gvPROPOSAL,
	"description": "one proposal in full: ops, preview, diagnostics, status, decision",
	"inputSchema": seqargs };
    tools[] = t26;
    var t27 = { "name": "graph.accept", "code": gvACCEPT,
	"description": "land an open proposal (editor tier): re-checks the target byte-exact, applies, records the decision",
	"inputSchema": seqargs };
    tools[] = t27;
    var t28 = { "name": "graph.reject", "code": gvREJECT,
	"description": "reject an open proposal with a reason (editor tier)",
	"inputSchema": rejargs };
    tools[] = t28;
    var t29 = { "name": "graph.withdraw", "code": gvWITHDRAW,
	"description": "withdraw an open proposal (its proposer, or an editor)",
	"inputSchema": seqargs };
    tools[] = t29;
```

`graph_call`: the revision refusal becomes `if ( is_rev && need >= tierPROPOSER )`
(comment: "a mutation — or a proposal — aimed at a revision refuses"); the
mutation case and the five new cases:

```c
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:
    {
	// The proposer's reroute (design §3.6): at exactly tierPROPOSER — or
	// any higher tier asking `propose: true` — the op is STORED as a
	// proposal; an editor's plain call lands it. ONE path up to the
	// candidate's validation (graph_edit_apply); the mode decides.
	long mode = emAPPLY;
	var pv;
	arg_of(pv, args, "propose");
	bool asked = !pv.is_null() && pv.as_boolean();
	if ( my_tier == tierPROPOSER || asked )
	    mode = emPROPOSE;
	var t;
	var why;
	if ( !edit_preconditions(why, w, es, adoc, t) )
	{
	    snap = { "ok": false, "error": why };
	    break;
	}
	var op;
	if ( !edit_op_plan(op, w, adoc, h, t, gv, args) )
	{
	    snap = { "ok": false, "error": op["error"] };
	    break;
	}
	var ops = {};
	ops.push(op);
	graph_edit_apply(snap, S, doc, self_id, ops, mode, 0);
	break;
    }
    // Nexus L4c: the FUTURE verbs (madcide_propose.inc).
    case gvPROPOSALS:
    {
	var sv;
	arg_of(sv, args, "status");
	long want = psNONE;
	if ( !sv.is_null() && strlen(sv.c_str()) > 0 )
	{
	    want = proposal_status_of(sv);
	    if ( want == psNONE )
	    {
		var why = format("unknown proposal status '{}'", sv);
		snap = { "error": why };
		break;
	    }
	}
	graph_proposals(snap, w, adoc, want);
	break;
    }
    case gvPROPOSAL: graph_proposal(snap, w, adoc, args["seq"].as_integer()); break;
    case gvACCEPT:   graph_accept(snap, S, doc, self_id, args["seq"].as_integer()); break;
    case gvREJECT:
    {
	var rv;
	arg_of(rv, args, "reason");
	var reason = "";
	if ( !rv.is_null() )
	    reason = rv;
	proposal_decide(snap, w, es, adoc, self_id, args["seq"].as_integer(), psREJECTED, reason.c_str(), false);
	break;
    }
    case gvWITHDRAW:
	proposal_decide(snap, w, es, adoc, self_id, args["seq"].as_integer(), psWITHDRAWN, "", my_tier < tierEDITOR);
	break;
```

- [ ] **Step 5: The ONE-validator gate.** Append to
  `scripts/check-madcide-single-owners.sh` (before the final `echo`): the
  seat calls each validator form ONCE, both inside `graph_edit_apply`
  (`count_validator_calls` = `grep -c 'madc::parse_refresh_checked(\|madc::parse_would_accept('`
  over `tools/madcide/*.inc` must be 2; negative control: a synthetic
  `madc::parse_would_accept(d, h, s)` line makes it 3). The FAIL prose:
  "ONE validator with a commit flag — route every candidate through
  graph_edit_apply". Extend the final OK line.

- [ ] **Step 6: Run**: gates `check-madcide-single-owners` / `check-madcide-enums` /
  `check-dialect-literals` / `check-dialect-lean` GREEN; `testmadcide_serve_edit`
  30/30 (the L3 answers unchanged: `replace ok / applied / log records: 3 /
  nodeop before splice`), `testmadcide_serve_graph`, `testgraphpast` 12/12,
  `testmadcide_serve_mcp`.

- [ ] **Step 7: Commit** (dialect + script: no trailers) —
  `feat(graph-mcp): nexus L4c — graph_edit_apply is the ONE transaction over an ops LIST in APPLY | PROPOSE mode (one candidate, one validation, one checkpoint, all splices or none); proposal + decision records; madcide_propose.inc = the fold + graph.proposals / proposal / accept / reject / withdraw; the proposer reroute in graph_call; the ONE-validator gate`.

---

### Task 5: `tests/testmadcide_serve_propose.mad` — the connection-level wiring test

**Files:** create `tests/testmadcide_serve_propose.mad`, `.expect`, `.expect_quiet`.

- [ ] **Step 1: The test** (the `serve_tiers` harness; the OWNER speaks the api
  envelope, the two agents speak JSON-RPC on the SAME seat):

```c
// Nexus L4c through REAL connections (the connection-level wiring test the
// L3 review deferred): three clients on one --serve seat — the owner (api
// envelope, share id 1) and two agents (JSON-RPC envelope, ids 2 and 3). An
// observer's mutation is refused; a proposer's is STORED (preview, the buffer
// untouched, the ids still fresh); accept needs an editor; a broken proposal
// stores nothing; the editor lands it (nodeop carries the proposal, a decision
// follows); a proposal whose target then changes is refused at accept and
// stays open; reject / withdraw; a proposer may withdraw only its own.
#include "../tools/texteditor/lined_core.inc"
#include "../tools/texteditor/editor_events.inc"
#include "../tools/madcide/madcide_core.inc"
#include "../tools/madcide/madcide_api.inc"
#include "../tools/madcide/madcide_mcp.inc"
#include "../tools/madcide/madcide_seat.inc"

// One JSON-RPC tools/call over a connection: `snap` = the parsed structured
// content, `iserr` = the envelope's isError.
void rpc(var &snap, bool &iserr, madc::channel &c, long id, const char *name, var &args)
{
    var params = { "name": name, "arguments": args };
    var req = { "jsonrpc": "2.0", "id": id, "method": "tools/call", "params": params };
    var js = js::stringify(req);
    var line = format("{}\n", js);
    c.write(line.c_str());
    yield();
    var r;
    c.readline(r);
    var resp;
    js::parse(resp, r);
    var result = resp["result"];
    iserr = result["isError"].as_boolean();
    var content = result["content"];
    var item0 = content[0];
    var txt = item0["text"];
    if ( !js::parse(snap, txt.c_str()) )
	snap = { "error": txt };		// a refusal is prose, not JSON
}

// One api-envelope command over a connection (the owner's clienttier).
void api(var &reply, madc::channel &c, long seq, const char *cmd, const char *args)
{
    var req = { "cmd": cmd, "args": args, "seq": seq };
    var js = js::stringify(req);
    var line = format("{}\n", js);
    c.write(line.c_str());
    yield();
    var r;
    c.readline(r);
    js::parse(reply, r);
}

int main()
{
    const char *path = "madc_ide_serve_propose.mad";
    php::file_put_contents(path,
	"long add(long a, long b) {\n"
	"    return a + b;\n"
	"}\n"
	"long run(long n) {\n"
	"    return add(n, 1);\n"
	"}\n");

    IdeSession S;
    long doc = S.open(path, false);
    S.terminal(false);

    madc::channel listener("listen://127.0.0.1:0");
    var addr = listener.local_endpoint();
    var uri = format("tcp://{}", addr);
    madc::channel own(uri.c_str());
    madc::channel a2(uri.c_str());
    madc::channel a3(uri.c_str());
    madc::channel t1;
    listener.wait_readable();
    listener.accept(t1);
    long c1 = t1.detach();
    madc::channel t2;
    listener.wait_readable();
    listener.accept(t2);
    long c2 = t2.detach();
    madc::channel t3;
    listener.wait_readable();
    listener.accept(t3);
    long c3 = t3.detach();
    go serve_client_task(&S, doc, c1);
    yield();				// owner (share id 1)
    go serve_client_task(&S, doc, c2);
    yield();				// agent (id 2), observer
    go serve_client_task(&S, doc, c3);
    yield();				// agent (id 3), observer

    var snap;
    bool err;
    var none;
    rpc(snap, err, a2, 1, "graph.symbols", none);
    var add = snap["nodes"][0];
    long addid = add["id"].as_integer();
    println("observer-read: err={} name={}", err ? 1 : 0, add["name"]);
    var rep1 = { "id": addid, "src": "long add(long a, long b) {\n    return a - b;\n}" };
    rpc(snap, err, a2, 2, "graph.replace", rep1);
    println("observer-propose: err={} why={}", err ? 1 : 0, snap["error"]);

    var reply;
    api(reply, own, 1, "clienttier", "2 proposer");
    println("promote-proposer: ok={} tier={}", reply["ok"].as_boolean() ? 1 : 0, reply["tier"]);

    rpc(snap, err, a2, 3, "graph.replace", rep1);
    var pv = snap["preview"];
    var before = pv["before"];
    var after = pv["after"];
    println("propose: ok={} proposal={} before-has-plus={} after-has-minus={}",
	    snap["ok"].as_boolean() ? 1 : 0, snap["proposal"],
	    before.index("a + b") >= 0 ? 1 : 0, after.index("a - b") >= 0 ? 1 : 0);
    var idargs = { "id": addid };
    rpc(snap, err, a2, 4, "graph.source", idargs);
    var stext = snap["text"];
    rpc(snap, err, a2, 5, "graph.status", none);
    println("buffer-unchanged: err={} source-has-plus={} log_head={}", err ? 1 : 0,
	    stext.index("a + b") >= 0 ? 1 : 0, snap["log_head"]);
    var openargs = { "status": "open" };
    rpc(snap, err, a2, 6, "graph.proposals", openargs);
    println("proposals-open: {}", php::count(snap["rows"]));
    var seq1 = { "seq": 1 };
    rpc(snap, err, a2, 7, "graph.proposal", seq1);
    println("proposal: status={} ops={} actor={}", snap["status"], php::count(snap["ops"]), snap["actor"]);
    rpc(snap, err, a2, 8, "graph.accept", seq1);
    println("proposer-accept: err={} why={}", err ? 1 : 0, snap["error"]);
    var broken = { "id": addid, "src": "long add(long a, long b) {\n    return a - ;\n}" };
    rpc(snap, err, a2, 9, "graph.replace", broken);
    long ndiag = php::count(snap["diagnostics"]);
    rpc(snap, err, a2, 10, "graph.proposals", openargs);
    println("propose-broken: err={} diags-positive={} open-still={}", err ? 1 : 0, ndiag > 0 ? 1 : 0, php::count(snap["rows"]));

    api(reply, own, 2, "clienttier", "2 editor");
    println("promote-editor: ok={} tier={}", reply["ok"].as_boolean() ? 1 : 0, reply["tier"]);
    rpc(snap, err, a2, 11, "graph.accept", seq1);
    var landed = snap["node"];
    long newid = landed["id"].as_integer();
    println("accept: ok={} landed_seq={} seq={} node={}", snap["ok"].as_boolean() ? 1 : 0,
	    snap["landed_seq"], snap["seq"], landed["name"]);
    var nidargs = { "id": newid };
    rpc(snap, err, a2, 12, "graph.source", nidargs);
    var stext2 = snap["text"];
    rpc(snap, err, a2, 13, "graph.proposal", seq1);
    var dec = snap["decision"];
    rpc(snap, err, a2, 14, "graph.status", none);
    println("after-accept: source-has-minus={} status={} decision-landed={} log_head={}",
	    stext2.index("a - b") >= 0 ? 1 : 0, snap["status"], dec["landed_seq"], snap["log_head"]);
```

(continue in the same `main`)

```c
    // An editor proposes explicitly; then edits the same node directly; the
    // proposal's target has changed -> accept refuses, the proposal stays open.
    var rep2 = { "id": newid, "src": "long add(long a, long b) {\n    return a * b;\n}", "propose": true };
    rpc(snap, err, a2, 15, "graph.replace", rep2);
    long p2 = snap["proposal"].as_integer();
    println("editor-propose: ok={} proposal={}", snap["ok"].as_boolean() ? 1 : 0, p2);
    var rep3 = { "id": newid, "src": "long add(long a, long b) {\n    return a / b;\n}" };
    rpc(snap, err, a2, 16, "graph.replace", rep3);
    println("direct-edit: ok={}", snap["ok"].as_boolean() ? 1 : 0);
    var seq2 = { "seq": p2 };
    rpc(snap, err, a2, 17, "graph.accept", seq2);
    println("accept-moved: err={} why={}", err ? 1 : 0, snap["error"]);
    rpc(snap, err, a2, 18, "graph.proposal", seq2);
    println("still-open: status={}", snap["status"]);
    var rej = { "seq": p2, "reason": "superseded by the direct edit" };
    rpc(snap, err, a2, 19, "graph.reject", rej);
    println("reject: ok={} status={}", snap["ok"].as_boolean() ? 1 : 0, snap["status"]);
    rpc(snap, err, a2, 20, "graph.accept", seq2);
    println("accept-rejected: err={} why={}", err ? 1 : 0, snap["error"]);

    // Withdraw: a proposer may withdraw only its own; an editor any.
    rpc(snap, err, a2, 21, "graph.symbols", none);
    var add3 = snap["nodes"][0];
    var rep4 = { "id": add3["id"], "src": "long add(long a, long b) {\n    return b;\n}", "propose": true };
    rpc(snap, err, a2, 22, "graph.replace", rep4);
    long p3 = snap["proposal"].as_integer();
    println("third-propose: ok={} proposal={}", snap["ok"].as_boolean() ? 1 : 0, p3);
    api(reply, own, 3, "clienttier", "3 proposer");
    println("promote-3: ok={} tier={}", reply["ok"].as_boolean() ? 1 : 0, reply["tier"]);
    var seq3 = { "seq": p3 };
    rpc(snap, err, a3, 23, "graph.withdraw", seq3);
    println("withdraw-other: err={} why={}", err ? 1 : 0, snap["error"]);
    api(reply, own, 4, "clienttier", "2 proposer");
    rpc(snap, err, a2, 24, "graph.withdraw", seq3);
    println("withdraw-own: ok={} status={}", snap["ok"].as_boolean() ? 1 : 0, snap["status"]);
    rpc(snap, err, a2, 25, "graph.proposals", none);
    long total = php::count(snap["rows"]);
    rpc(snap, err, a2, 26, "graph.proposals", openargs);
    println("proposals: all={} open={}", total, php::count(snap["rows"]));
    var bogus = { "status": "pending" };
    rpc(snap, err, a2, 27, "graph.proposals", bogus);
    println("bad-status: err={}", err ? 1 : 0);

    own.close_write();
    a2.close_write();
    a3.close_write();
    madc::task_drain();
    own.close();
    a2.close();
    a3.close();
    listener.close();
    S.close();
    php::unlink(path);
    return 0;
}
```

`tests/testmadcide_serve_propose.expect` (the seq arithmetic: the stream is
empty at start; proposal 1 → seq 1; accept → nodeop 2, erase 3, insert 4,
decision 5; the editor's proposal → 6; the direct edit → nodeop 7, erase 8,
insert 9; reject → 10; the third proposal → 11; withdraw → 12 — VERIFY on the
first run and pin with the reason in a comment):

```
observer-read: err=0 name=add
observer-propose: err=1 why='graph.replace' requires proposer (you are observer)
promote-proposer: ok=1 tier=proposer
propose: ok=1 proposal=1 before-has-plus=1 after-has-minus=1
buffer-unchanged: err=0 source-has-plus=1 log_head=1
proposals-open: 1
proposal: status=open ops=1 actor=2
proposer-accept: err=1 why='graph.accept' requires editor (you are proposer)
propose-broken: err=1 diags-positive=1 open-still=1
promote-editor: ok=1 tier=editor
accept: ok=1 landed_seq=2 seq=4 node=add
after-accept: source-has-minus=1 status=accepted decision-landed=2 log_head=5
editor-propose: ok=1 proposal=6
direct-edit: ok=1
accept-moved: err=1 why=proposal target changed since seq 6; re-propose against fresh ids
still-open: status=open
reject: ok=1 status=rejected
accept-rejected: err=1 why=proposal 6 is rejected
third-propose: ok=1 proposal=11
promote-3: ok=1 tier=proposer
withdraw-other: err=1 why=proposal 11 is not yours (actor 2)
withdraw-own: ok=1 status=withdrawn
proposals: all=3 open=0
bad-status: err=1
```

`tests/testmadcide_serve_propose.expect_quiet`: one line.

- [ ] **Step 2: Run** `bash tmp/l3_check.sh tests/testmadcide_serve_propose.mad` → 0
  missing, stderr empty; the fixture file removed.

- [ ] **Step 3: Commit** — `test(graph-mcp): nexus L4c through real connections — observer refused, proposer stored, editor lands, a moved target refused at accept, reject / withdraw / own-only (tests/testmadcide_serve_propose)`.

---

### Task 6: Docs, hand-off, push

- [ ] **Step 1**: design doc — §3.2: "records carry `actor` (the connection's
  share id) beside `es`" (decision 1); §3.1: one sentence on `path` +
  `clog_adopt` and the restore guard (decisions 3–4); §3.4: `text` rides the
  op (decision 2); §4.4 last bullet: the wiring test runs both envelopes on
  the one seat (`madcide_seat.inc`); §9: L4c SHIPPED with its contents.
- [ ] **Step 2**: `claude_status.json` live_handoff UPDATE 14 (L4c shipped; NEXT =
  writing-plans L4d: record/link kinds + `nexus_fold`, `nexus.*` verbs, the
  MCP client over `exec://`, manifests as data, `nexus_sync`, the fixture
  server, the `test` record kind); the L4 ledger; memory.
- [ ] **Step 3**: `git push origin feature/client-server-views-claude`.

---

## Self-review

- **Spec coverage**: §3.1 one stream + per-doc fold + per-doc checkpoints +
  the two-doc case → Task 2; §3.2 actors (amended: connection id) → Tasks 4/6;
  §3.4 proposals (ops list, all-or-nothing, byte-exact re-check, derived
  status, decision records, `constraints`/`checks` seats) → Task 4; §3.6
  `tierPROPOSER` + the table (observer ✗ / proposer stored / editor applied or
  `propose: true` / accept-reject editor / withdraw own-or-editor) → Tasks 3–4;
  §4.4 `parse_would_accept`, `edit_mode`, the five verbs, the wiring test →
  Tasks 1/4/5; §5 flow 3 → Tasks 4/5; §6 refusals (tier shape, not open,
  target changed, re-sync, rejected candidate stores nothing, revision id to a
  mutation/proposal) → Tasks 4/5 (each asserted `isError` or `ok=0`); §7
  contracts stated in the headers; §8 L4c tests → Tasks 2/5; §10 (1) one
  stream, (4) fourth tier level → Tasks 2/3.
- **Placeholders**: the two "pin on the first run" values (`all=N`, the seq
  arithmetic) are verification steps with the arithmetic written down; the
  `madcide_client.inc` include note names its fallback.
- **Type consistency**: `edit_op_plan(op, w, adoc, h, t, gv, args) -> bool`
  used by `graph_call` and `graph_accept` with the same order;
  `graph_edit_apply(out, S, doc, self_id, ops, mode, proposal_seq)` called from
  both; `proposal_decide(out, w, es, adoc, self_id, seq, want, reason, own_only)`
  from the two cases; `edit_preconditions(why, w, es, adoc, t)` fills `t` for
  the plan; the emAPPLY answer's `nodeop_seq` is read by `graph_accept`;
  `keyed_get` resolves from `editor_events.inc` for both `madcide_past.inc` and
  `madcide_propose.inc`; `clog_find(w)` replaces every `es_int(w, doc,
  "changelog", 0)`; `envelope_of` / `proposal_status_of` take `var &`.
