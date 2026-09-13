# The Nexus (code-graph MCP L4) — PAST via libgit2, the `propose` tier, nexus records + an MCP client

**Status:** DESIGN — written 2026-09-13 for owner review; the writing-plans pass
follows approval. Track: the code-graph MCP
(`2026-09-12-ast-graph-mcp-for-agents.md` §7 L4) on the arc branch
`feature/client-server-views-claude`. Battery: the V6 arc RELEASE seam only —
never per slice (`testing-fulltest.md`).

**Owner decisions already SETTLED (2026-09-13, do not re-ask):**
1. PAST axis FIRST, via **libgit2** vendored the MIR way (a `third_party`
   subtree built by the Makefile into `obj/`, static, network transports OFF —
   the Nexus only READS local history; push/fetch stay git's) behind a
   **madcdis source adapter** so no consumer sees the engine.
2. The **`propose` tier** is the FUTURE axis's first verb family: an agent
   between observe and edit issues the L3 verbs; its node-ops are STORED as
   proposals in the ONE JSONL stream; a human lands or drops them.
3. Project management = **ONE native nexus record vocabulary** in madcdis
   (task, requirement, decision, workset) + **N source adapters** for external
   systems (mycenode's MCP first, Jira's MCP second) → an **MCP CLIENT** in madc
   (today madc is only an MCP server), per-server tool→vocabulary mappings as
   DATA. Naming: `madcdat` = the storage backends under `madcdis`; drivers and
   source adapters live in `madcdis`.
4. ORDER: PAST → propose → MCP client + mycenode adapter.

This document turns those four into a buildable model: records, ids, tiers,
verbs, components, data flow, refusals, thread-safety, gates and slices.

---

## 1. Goal

Give the agent (and every other client) the two axes the code graph lacks:

- **PAST** — "who changed this node, when, why; what did it look like at
  commit X; what changed between X and Y" — answered from the session's own
  change log AND the repository's git history, through one verb family.
- **FUTURE** — an agent's edit as a **reviewable proposal**: validated by the
  real parser, previewed, stored with actor and causal parent, landed or
  dropped by a human through the ordinary L3 path.

And the first **intent → code** edges: native nexus records (task,
requirement, decision, workset) linked to symbols, commits and proposals, with
external project-management systems projected in through an MCP client.

The positioning (the external review's wording, adopted): madc does not merely
give agents access to an AST — it **maintains a live semantic model of a
software project across time**: compiler-resolved present state, semantic
development history, and explicit development intent, so humans and agents do
not reconstruct the project from source files every time. Four questions, one
verb family each:

| Question | Authority | Verbs |
|---|---|---|
| What does this code mean now? | the compiler (L1–L3) | `graph.node / type_of / members / body / references / callers / callees / impact` |
| How did it get this way? | git + the event stream (L4b) | `graph.history / commits / revision / diff` |
| Why does it exist? | records, decisions, proposals, provenance (L4c–d) | `nexus.context / explain` (its `why` section) |
| Where is it supposed to go? | the intent graph (L4d) | `nexus.records(planned_for…) / explain` (its `plan` section) |
| Does it still work? | the verification axis: tests as records, runs as events (L4e) | `test.list / test.run / test.results`; a proposal's `checks` |

Every axis is a LAYER an asset may or may not have (§3.9): the source text is
always there; git and tracking exist for project members; lexing and parsing
exist for the formats the engine knows; testing exists for projects and RUNS
only where a runner exists. A verb refuses with the layer it lacks, never
pretends.

## 2. Where this sits — facts (recon 2026-09-13, at HEAD 870ad3e2)

Shipped substrate this design projects over (reuse; nothing is rebuilt):

| Fact | Where |
|---|---|
| The change event log: `changelog_of / clog_append / journal_splice / clog_checkpoint / clog_replay / clog_head / events_since / clog_compact`; records `{seq, kind: splice\|checkpoint\|nodeop, es, doc, …}`; `seq` = the LSN (a rewrite-stable counter); replay applies `splice` + `checkpoint` only | `tools/texteditor/editor_events.inc:230-416` |
| Persistence: `<base>.prj.events` beside the manifest via `clog_persist / clog_restore`, hooked into `proj_write / proj_open / proj_startup`; memory-only for the implicit project (RULED) | `tools/madcide/madcide_core.inc:3529-3865` |
| ONE text-mutation owner `ed_text_insert / ed_text_erase` (buffer + `shift_anchors` + `journal_splice`) | `editor_events.inc:418-429`; `shift_anchors` at 181 |
| L3 node-ops: `graph_edit_apply` (sync precondition → read-only rule → span → splice → candidate on the scratch buffer → `parse_refresh_checked` → ONE checkpoint → `nodeop` record → `ed_text_*` → spans → broadcast → `graph.at`) | `tools/madcide/madcide_mcp.inc:312-518` |
| Generation-stamped ids (bits 40..60), `graph_id_stamp/raw/gen/fresh`, stale = `{error, stale:true}`; `parse_refresh_checked` = fresh child, swap iff the error count does not rise | `src/madc_program.cpp`; `include/madc/ns_madc:153-235` |
| Tiers: `enum ide_tier : unsigned char { tierOBSERVER = 0, tierEDITOR, tierOWNER }`; `cmd_min_tier`, `graph_min_tier` (mutations = tierEDITOR); a connection's tier = `madc::conn_cookie`; `clienttier <id> <tier>`; a real two-connection harness exists | `tools/madcide/madcide_enums.inc:536-660`; `madcide_api.inc:70-134`; `tests/testmadcide_serve_tiers.mad` |
| The MCP **server** seat: `mcp_handle` (JSON-RPC 2.0, one line per message) over `api_run`; `run_mcp` over stdio; `graph_call` = enum switch + tier gate | `tools/madcide/madcide_mcp.inc` |
| madcdis extension points: `SourceAdapter` (`name / can_read / discover_types / extract`) with `world_text_adapter` as the in-tree precedent; `DataDriver` + `DataDriverRegistry` (`dsv/flr/vlr` core; `sqlite/bdb/gdbm/qdbm` = madcdat, optional) | `include/madcdis/source_adapter.h`; `include/madcdis/world_text.h:568`; `include/madcdis/driver.h` |
| `DataSource` already knows the schemes `mcp` (service/service_api) and `exec` (execution/process); a `git` scheme does not exist yet | `include/libmadc/datasource.h:113-152` |
| `madc::channel`: `exec://` (spawn a child, read its stdout / write its stdin, `readline`, `wait_readable`, `exit_status`), `tcp://`, `listen://`, the WebSocket facet | `include/madc/ns_madc:476-596`; `src/madc_process.cpp` |
| Parse handles: `parse_open(source, filename)` compiles text into a child (never executes); every `graph_*` accessor takes a handle | `include/madc/ns_madc:131-235` |

Searches that came back EMPTY (rule #4, recorded so the new names below are
not reinventions): `libgit2` anywhere outside `tmp/` and `third_party/`;
an MCP *client* (`mcp_client`, `jsonrpc … client`, `mcp_connect`) in `src/`,
`include/`, `tools/`; any git integration (`"git `, `git_`) in
`tools/madcide`, `src/madc_project.cpp`, `include/madc/ns_madc`. The
container has `git 2.43` and `cmake`; no system libgit2.

External recon (`tmp/sdd-ast-graph-mcp-L4/recon.md`, summarised in
`claude_status.json` UPDATE 10): yailPeralta/ast-mcp-server's
prepare → review → apply (hash-bound plan, diagnostic delta, retained diff,
`AMBIGUOUS_APPLY` on lost ownership) IS the propose tier; codegraph's
`status` / batched explore / provenance labels are cheap adoptions;
OpenRewrite's recipe (scan → edit → per-file diff → PR review) is the
programmatic form of a proposal. Adopted below: `graph.status`,
`graph.source`, the preview as the proposal's review artifact, a proposal
that holds a LIST of ops (a recipe = one proposal), byte-exact target
re-check at landing. Not copied: regenerating the tree per run; warning on
staleness (we refuse).

### 2.1 Cross-reference: the external review (ChatGPT, handed over 2026-09-13)

The owner supplied an independent review of the Nexus concept as a
cross-reference guide, with the caveat that the reviewer did not read the
madc code — so each premise below is checked against the code, not accepted.
Point by point against this design:

| Review point | Standing | Where |
|---|---|---|
| Live compiler graph as ground truth; no second semantic index | already an invariant | design doc §5 (1)(7)(8); §2 here |
| Generation numbers in temporary handles so stale refs fail deterministically | shipped (L3) | §3.3 extends it to revision handles |
| Persistent semantic identity SEPARATE from temporary node identity | v1 = the symbol KEY (`rkSYMBOL`); a persistent `entity` record with lineage is the review's step 7 and ours | §3.5; §12 |
| History attached to semantic entities; rich ChangeEvents (rename / move / signature-change / subtree-replace / extract) recorded, not reconstructed from diffs | the `nodeop` record is the seat; the verbs are L3 increments recorded as semantic ops; `graph.rename` = the first N-op proposal | §3.4 (ops list), §12 |
| Intent as a first-class graph with provenance, date and status so obsolete plans are never mistaken for current truth | ADOPTED: a common record envelope `{state, created_seq, updated_seq, actor, origin}`; reads default to live states; `milestone` added as the target of `planned_for` | §3.5 |
| Clear authorities; inference explicitly marked | ADOPTED: §3.7 authority table + a `provenance` enum on every history/context row (`compiler / git / event / record / inferred`); v1 emits no `inferred` row | §3.7, §4.3 |
| Bounded semantic operations, not a query language; `history / lineage / why / plan / constraints / explain` | `history` L4b; `explain` ADOPTED as ONE compound verb with `what / depends / history / why / plan / constraints` sections (`why(this)` = its `why` section); `lineage` deferred with persistent identity; `constraints` = decisions linked to the target, attached to every proposal answer | §4.5, §3.4 |
| Mutation = fragments, never ranges; transactional (candidate → validate → checks → commit → render); invalid intermediate states never touch the live project | fragments: settled (L3); ADOPTED: the proposal's `ops[]` IS the transaction — `graph_edit_apply(ops)` is all-or-nothing from day one (one candidate, one validation, one checkpoint, all splices or none) | §3.4, §4.4 |
| Source stays the durable representation "until MC11-IR is lossless for comments / macros / pragmas" | the PREMISE is stale for madc (the review did not read the code): MC11-IR already retains comments (trivia), macro origins and definitions, include directives and error NODES; source stays durable for the replication/one-mutation-owner reason instead, and a node-level render becomes a refinement, not a prerequisite | §3.8 |
| No Cypher/GQL or graph database until use demonstrates the need | settled invariant | design doc §5 (1)(2) |
| Order: live reads → compound queries → git + lineage → intent → rich events → transactions → persistent identity | ours: PAST → propose → intent (owner-settled). Differences: `explain` lands when all three axes exist (L4d); the single-op propose tier precedes intent because it is cheap and the record shape carries the N-op transaction | §9, §10 |

## 3. The model

### 3.1 ONE project-scoped event stream (extends the V4 log; RULED shape)

The change log is today a per-document JSONL stream (`changelog_of(w, doc)`),
persisted as ONE file `<base>.prj.events` (V4's "one project events file
across docs for a multi-file manifest" is a recorded follow-up). L4 makes the
stream **project-scoped**: one `changelog` entity per session, every record
carries `doc` (splice / checkpoint / nodeop already do), `clog_replay(doc)`
folds only that doc's `splice` + `checkpoint` records, `clog_compact` writes
one checkpoint PER DOC at the cut, `clog_persist / clog_restore` are unchanged
(one file). `changelog_of(w, doc)` keeps its signature and returns the one
entity — no caller moves.

Why one stream (not a second log for proposals and records): cross-reference
ruling 1 — "one record that edit history, the mirror, provenance and replay
all read … prevents three parallel logs later". Proposals and nexus records
are project-level facts with the same actor / causal-parent / `seq` needs;
a second file would be the parallel log the ruling bans. State is
`events + snapshots` (Nexus §20): the stream is the truth, any index over it
is a disposable derivative.

Record kinds become an ENUM at the reader (enum law; today `clog_replay`
compares `rec["kind"] == "splice"` as text — the L3 nodeop was the second
such compare, so the boundary conversion lands now):

```
enum clog_kind : unsigned char {
    ckNONE = 0,        // unknown / torn — never trusted
    ckSPLICE,          // {es, doc, at, del, ins}          text redo
    ckCHECKPOINT,      // {doc, text}                       text snapshot
    ckNODEOP,          // {es, doc, verb, target, src}      L3 annotation
    ckPROPOSAL,        // §3.4                               FUTURE
    ckDECISION,        // {proposal, decision, actor, landed_seq, reason}
    ckRECORD,          // §3.5 a nexus record event (create/update/close)
    ckLINK             // §3.5 {from, rel, to} / unlink
};
long clog_kind_of(const char *text);   // ONE boundary; misspelling = ckNONE
```

`clog_replay` / `events_since` / `clog_compact` switch on the code. A record
of kind `ckNONE` is skipped exactly like a torn line (never applied, never
counted). Gate: the single-owners check fails a literal `rec["kind"] == "…"`
compare anywhere in `tools/` (negative control included).

### 3.2 Actors

Every record already carries `es` (the editing-state entity of the client
that posted it). L4 adds nothing to the actor model; the seat's connection
cookie (`madc::conn_cookie`) is the tier, `es` is the identity, and a
proposal's `actor` is the `es` that created it. A landed proposal's `nodeop`
carries `proposal: <id>` so BOTH actors are on the record (the proposer on
the proposal, the lander on the nodeop + decision) — Nexus §10 provenance
("who changed it, what process generated the change, who reviewed it").

### 3.3 Ids across handles — revision handles carry a generation TAG

A PAST query at a commit opens a **revision handle**: libgit2 yields the
served file's blob at `rev`, `parse_open(text, filename)` compiles it (the
proven path; never executes), and every `graph_*` read verb answers against
it. Its ids must not be confusable with the live handle's: gen-0 ids of two
handles are bit-identical today.

Rule: a revision handle's `parse_tu_state::generation` is set at open to a
**tag** from the range `[2^20, 2^21)` — the top bit of the 21-bit field
(`GRAPH_GEN_MASK` = `0x1FFFFF`) marks "not the live handle". Live generations
count up from 0 per refresh and never reach 2^20 in a handle's lifetime (a
million accepted edits; the seat's `reparse_buffer` bumps too — still far).
The seat keeps `doc.rev_handles: tag → {handle, sha}` and routes an incoming
id by its generation: live gen → the live handle; a tag in the table → that
revision handle; anything else → the ordinary stale refusal. A revision id
passed after its handle was closed is stale — loud, never a wrong node.

Engine surface: `parse_open` gains a sibling `parse_open_tagged(source,
filename, tag)` (same body, one extra assignment) — one implementation, the
tag an argument. Revision handles are READ-ONLY at the seat (mutations refuse
"revision handles are read-only"; the engine never sees the request).

Bound: at most `N` revision handles per doc (config, default 8), LRU-closed
through `parse_close` — a bounded cache of derived state, never persisted
(no-program-cache law).

### 3.4 Proposals (the FUTURE axis, v1)

A proposal is ONE record in the stream holding a LIST of node-ops. The list
IS the transaction: v1 proposals hold one op, a rename or a recipe later holds
N — the same record, the same landing contract:

```
{ seq, kind: "proposal", es, doc, base_seq,
  ops: [ { verb: insert|replace|delete, where?: before|after,
           target: { id, kind, name, span, text },   // graph.span + graph.source at creation
           src,                                       // the fragment
           splice: { at, del, ins } } ],
  preview: { line0, line1, before, after },          // the touched lines, live vs candidate
  diagnostics: [ … ],                                // the candidate's delta (may be empty)
  constraints: [ <decision refs> ],                  // decisions linked to the target (L4d; informs, never blocks)
  checks: [ ],                                       // the seat for test/check results (later)
  status: "open" }                                   // derived: the last ckDECISION wins
```

- **All-or-nothing** (the review's transactional model): `graph_edit_apply`
  takes the `ops` LIST — every splice is applied to ONE candidate (descending
  offsets, so earlier splices never move later targets), the candidate is
  validated ONCE, one checkpoint is taken, and every splice commits through
  `ed_text_*` or none does. An invalid intermediate state never touches the
  live buffer, the tree, or the log. The L3 function becomes the one-element
  case of this contract in L4c (no second path when N arrives).
- `base_seq` = `clog_head` when created (the causal parent — "based on").
- `target.text` is the span's bytes at creation: the byte-exact re-check at
  landing (yailPeralta's hash check, spelled as the text itself — the L3
  `phandle_text` precedent: exact, no checksum function in the dialect).
- The candidate is validated with the SAME machinery as an edit
  (`parse_refresh_checked`'s child parse + error-count rule) but WITHOUT the
  swap: a `commit` flag on the one internal
  (`internal_program_parse_refresh_checked(…, bool commit)`), exposed as
  `madc::parse_would_accept(out_diags, handle, source)`. No second validator.
- `status` is DERIVED: `open` until a `ckDECISION` record names the proposal;
  `accepted` / `rejected` / `withdrawn` then. Decisions are records too
  (actor, reason, `landed_seq` = the nodeop's seq on accept) — Nexus §16's
  `proposal.created / reviewed / accepted` shape in the one stream.

Landing (`graph.accept`) = the ORDINARY `graph_edit_apply` with the
proposal's op, after three re-checks in order: (1) the doc is text-synced
(the L3 precondition); (2) `graph.at(target.span.start)` yields a node of the
same kind + name; (3) `graph.source` of it equals `target.text` byte-exact.
Any miss → refuse `"proposal target changed since seq N; re-propose against
fresh ids"` and the proposal stays `open` (the human decides; nothing lands
silently on other bytes). On success the nodeop carries `proposal: seq`, and
a `ckDECISION {decision: accepted, landed_seq}` follows it.

### 3.5 Nexus records and links (intent; ruling 2)

Native records when no external tool owns them; identity + links + a cached
projection when one does. Both are events in the stream:

```
{ seq, kind: "record", es, op: create|update|close,
  id,                       // = the seq of the creating record (rewrite-stable)
  rkind: task|requirement|decision|workset|milestone,
  state: open|active|done|superseded|rejected,      // enum; the fold's default reads exclude superseded/rejected
  origin: "" | "<source name>",   // "" = native; else the adapter that owns it
  ext_key: "",              // the owner system's key (mycenode id, MADC-42)
  fields: { title, body, … } }                    // rkind-specific, sorted keys

{ seq, kind: "link", es, op: link|unlink,
  from: <ref>, rel: <relation>, to: <ref> }
```

Every record carries the same **envelope** — `state`, `created_seq`,
`updated_seq` (both derived by the fold from the stream: the stream IS the
date), `actor` (`es`), `origin`, `ext_key` — so an obsolete plan is never
mistaken for current truth (the review's point): a superseded requirement or a
rejected decision stays, with its provenance, and answers only when asked for
(`nexus.records(rkind, state)`; the default is the live states). `milestone`
is the target of `planned_for` (the owner's four kinds plus this one; §10).
A decision's `fields` carry `reason` and `alternatives` (Nexus §8) — a
rejected approach is a decision with `state: rejected`, or an alternative
inside the decision that won.

A **ref** is one of (an enum `ref_kind`, text only on the wire):
`rkRECORD {id}` · `rkSYMBOL {file, kind, name}` — a durable code key, NEVER
a raw graph id (ids are generation-bound; a symbol key survives edits and
re-resolves through `graph.definition`) · `rkCOMMIT {sha}` · `rkPROPOSAL
{seq}` · `rkEXTERNAL {source, key}`.

**Relations** (an enum, the Nexus §2 vocabulary that is INTENT, not code —
code edges stay the graph's `calls/references/…` ground truth):
`implements · tests · fixes · affects · requested_by · documents · supersedes
· blocked_by · planned_for · reviewed_by · generated_by · introduced_by ·
modified_by`.

Reads FOLD the stream (the same walk `clog_replay` does; one reader
`nexus_fold(out, w)` yields `{records: id → record, links: [...]}`), cached on
the session entity and invalidated by `seq` (a record or link appended
advances `seq`; the fold is recomputed lazily on the next read — v1 sizes are
hundreds of records). A derived index in a madcdat backend is a later,
disposable optimisation, never the truth.

The enums (`clog_kind`, `ref_kind`, `nexus_rel`, `record_kind`,
`proposal_status`) live in `tools/madcide/madcide_enums.inc` beside
`ide_tier` for L4 (dialect-only consumers); they move to a
`<bits/nexus_enums>` fragment the day the engine reads them (the
`bits/ui_enums` precedent — one enum text for both sides).

### 3.6 Tiers: `proposer` is a fourth level in the ONE ladder

```
enum ide_tier : unsigned char { tierOBSERVER = 0, tierPROPOSER, tierEDITOR, tierOWNER };
```

Every existing `<` / `>=` comparison keeps its meaning (the ladder stays
monotone); `tier_name / tier_of` learn `"proposer"`; `clienttier <id>
proposer` promotes. An MCP client is still born `observer`.

| Verb family | observer | proposer | editor | owner |
|---|---|---|---|---|
| graph.* reads (L1–L2, PAST) | ✓ | ✓ | ✓ | ✓ |
| graph.insert / replace / delete | ✗ | **stored as a proposal** | applied (or stored with `propose: true`) | applied |
| graph.accept / graph.reject | ✗ | ✗ | ✓ | ✓ |
| graph.withdraw | ✗ | own proposals | ✓ | ✓ |
| nexus.* reads (record, records, context, sources) | ✓ | ✓ | ✓ | ✓ |
| nexus.create / update / close / link / unlink | ✗ | ✓ (records are reviewable data, not source) | ✓ | ✓ |
| nexus.sync (pull from an external source) | ✗ | ✗ | ✓ | ✓ |

The seat's gate stays ONE function per family (`graph_min_tier`, a new
`nexus_min_tier`); the proposer's reroute is inside `graph_call`: a mutation
verb at exactly `tierPROPOSER` (or any tier ≥ proposer with `propose: true`)
runs `graph_edit_apply` in PROPOSE mode. One code path up to the candidate's
validation; the mode decides commit-or-store.

### 3.7 Authorities and provenance (Nexus §21; the review's table)

Every answer names the authority that produced it; nothing is blended
silently:

| Question | Authority | How the answer is marked |
|---|---|---|
| What does the code mean? | the compiler (the live handle) | graph nodes/edges — ground truth by construction, unmarked |
| What changed textually, when, by whom (committed)? | git | `provenance: git` |
| What semantic operation occurred (uncommitted / madc-originated)? | the event stream (`nodeop`, `splice`) | `provenance: event` |
| Why was it changed / what is planned? | records, decisions, proposals | `provenance: record` |
| What is inferred? | nothing in v1 | `provenance: inferred` — the enum value exists so a future heuristic (rename survival, dynamic dispatch) is LABELLED, never passed off as truth |

```
enum provenance : unsigned char { pvNONE = 0, pvCOMPILER, pvGIT, pvEVENT, pvRECORD, pvINFERRED };
```

`graph.history` rows, `nexus.context` rows and every `nexus.explain` section
entry carry `provenance` (text on the wire, the enum inside). Conflicting
observations coexist as separate rows (Nexus §21 — a local PASS beside a CI
FAIL is information, not a sync error).

### 3.8 Source stays the durable representation (invariant) — and why

The review's premise — "keep source durable UNTIL the IR can preserve
comments, macros, formatting, pragmas losslessly" — does not describe madc
(the owner's caveat: the review did not read the code). MC11-IR already
retains what it names: every `cir_node` carries `origin_id` (its originating
token's arena slot — the single source of truth for position and provenance),
`datadef_id`, `src_lang`, `synth_from_origin`, `tree1_origin`; parse handles
lex in fidelity mode (`Program::keep_trivia`), so every token keeps its
`leading_trivia` (whitespace + comments; `TokenREM` carries comment content;
`trivia_comment_rows` projects them); macro-expanded tokens are marked
`tfSYNTHPOS` and name their invocation site, `MacroDef`/`macro_map` retain the
definitions, `fidelity_include_directives` the include lines; syntax errors are
NODES (`TokenError` holes + debris with spellings, positions and trivia
retained; `cir_node::error_msg_id`; `Program::error_nodes` gates translate).
The `--emit=c++` reverse-render already echoes the retained tokens with their
trivia and include records (`madc_program.cpp:4807`).

Source is durable for a different reason: the ONE replication and mutation
substrate is the text splice log (client-server §2.8 invariant 4; the one
text-mutation owner), and the running madc IS the compiler — the tree is
what the parser says about the buffer, never a second truth to reconcile. So
L3's statement-granularity rule (the fragment IS the render of the new
statement; the buffer IS the render of an unchanged one) holds through L4
because it is the cheapest exact render, not because the IR is lossy. A
node-level render (tree → text through the retained tokens) is therefore a
REFINEMENT the faithful IR permits when a verb needs it (a `graph.rename`
rendering N reference sites is the first candidate), not a prerequisite.
A proposal's `preview` is text because the reviewer reads text.

### 3.9 Asset layers — every axis is optional per asset (owner, 2026-09-13)

An asset (a file in a project, or the one buffer of the implicit project) has
its bytes and then some subset of the axes, each a LAYER the asset either has
or lacks. The layers are a capability set computed by ONE function from facts
the engine already holds — never guessed, never assumed by a verb:

```
enum asset_layer : unsigned char {
    alTEXT     = 1,   // the bytes; a text kind is editable, a binary kind is opened by name
    alHISTORY  = 2,   // a repository above the asset AND a project manifest
    alTRACKING = 4,   // a project manifest (records, links, provenance — binary assets too)
    alLEXED    = 8,   // the file kind has a lexer (fkTEXT range with a lexer, fkC/fkCPP/fkMADC, fkOTHER with rows)
    alPARSED   = 16,  // the file kind has a parser: fkC / fkCPP / fkMADC ranges -> the MC11 tree, the graph verbs
    alTESTABLE = 32,  // a project manifest: tests are records here
    alRUNNABLE = 64   // a runner exists for the asset's tests: internal (madc compiles + runs
                      // the kind: parse_build / parse_run, madcrun:// / madcproj://) or
                      // external (the manifest names a command)
};
long asset_layers_of(long w, long doc);   // ONE owner, in the seat; reads file kind,
                                          // manifest presence, GitRepo::open, the runner table
```

Rules the layers encode (each a fact, not a policy knob):
- **Text is always there** — even a binary asset has bytes and a name; only
  `alTEXT`'s editable half is a text-kind property (the `fkBINARY` range is
  opened by name, never edited as text — `<bits/file_kinds>` already says so).
- **History needs a project.** The git layer exists for project members with a
  repository above them. Binary members get COMMIT-level history (`log`,
  `show` of the bytes); LINE-level verbs (`blame`, revision handles,
  `graph.diff`) exist for text kinds only — a blame of a PNG is not an answer.
- **Tracking needs a project, nothing else.** A binary asset is a first-class
  record target (identity + links + provenance): an image a task references, a
  fixture blob a test needs.
- **Lexing and parsing follow the file-kind registry**, not a flag here: a
  kind gains the lexed layer when its lexer rows land and the parsed layer
  when its parser rows land (`<bits/file_kinds>`'s own rule). The MC11 tree —
  and with it every `graph.*` verb — exists only for parseable kinds; a JSON
  member has history, tracking and colour, and `graph.symbols` on it refuses
  with `"no parsing layer for kind json"`.
- **Testing needs a project; RUNNING needs a runner.** Test records exist for
  any project; `test.run` refuses on a project with no runner rather than
  faking a result — and says which runner it looked for.

Every verb family declares the layer(s) it needs as DATA beside its tier
(`graph_min_layer` next to `graph_min_tier`; `nexus_min_layer`,
`test_min_layer`): `graph.*` → `alPARSED`; `graph.history` → `alHISTORY`
(+ `alPARSED` for a node, `alTEXT` suffices for the asset's own history);
`graph.revision / diff / blame-backed rows` → `alHISTORY` on a text kind;
`nexus.*` → `alTRACKING`; `test.list / results` → `alTESTABLE`; `test.run` →
`alRUNNABLE`. The gate runs at `graph_call`/`nexus_call`/`test_call` entry, after
the tier gate, before any structure is touched. `graph.status` and
`nexus.explain` report the asset's layer set (as text names on the wire,
converted once) so an agent learns in one call what this asset can answer —
per-asset capability negotiation, the §2.8 invariant 2 shape applied inward.

### 3.10 The verification axis — tests as records, runs as events (owner, 2026-09-13)

The triad (git = past, AST = present, intent = future) neglected the axis
that says whether any of it WORKS. Nexus §11 names it ("build, test, debug
and profile as one model"); the external review's transactional mutation
("validate, run checks/tests, commit") needs it; the proposal record already
carries the `checks` seat for it. The model:

- **A test is a record** (`rkind: test`) with the envelope of §3.5 and fields
  `{name, family, asset, command}` — `family` an enum `test_family { tfMAD
  (tests/*.mad with its fixtures), tfUNIT (tests/unit/*.cpp), tfEXTERNAL (a
  manifest-declared command) }`; `asset` the test's own file (a ref); a
  `tests` link from the test record to the symbols / assets it covers (the
  `tests` edge the recon found in yailPeralta's test candidates and codegraph's
  blast radius — here it is a RECORD, ground truth by declaration, never
  inferred from names).
- **Discovery is convention, not a hand list**: `test.discover` folds the
  project's test families from the SAME conventions the canonical runner reads
  (`.claude/rules/test-fixtures.md`: `tests/<name>.mad` + `.expect / .input /
  .argv / .flags / …`; `tests/unit/*.cpp`) into `origin: "convention"` records;
  a manifest may add `tfEXTERNAL` tests as data (`tests: [{name, command}]`).
  Rule #7: no per-test knowledge in the discoverer.
- **A run is an event** in the ONE stream: `{kind: "testrun", test, actor,
  node, when, result: pass|fail|skip|timeout|error, exit, output_ref}` —
  `result` an enum, `node` the client/node that ran it (client-server §2.8
  invariant 5: a result is tagged by the serving node — a local PASS and a CI
  FAIL coexist, Nexus §21). The latest run per test is the derived `state`
  the fold shows; history is the stream.
- **ONE runner, two execution seats.** The canonical runner
  (`scripts/run_tests.sh`) owns the fixture protocol and the pass/fail
  verdict; the Nexus is its CLIENT, never a second implementation of the
  protocol (no-parallel-implementations): v1 `test.run` spawns the runner for
  the named test(s) through the process owner (`exec://`, the §3 subprocess
  contract) and reads a machine-readable result line the runner gains
  (`--report=json`: one JSON object per test — the boundary conversion, once).
  The two seats are the owner's "preferably internal, but might require
  external": **internal** = the test's program is compiled and run by madc
  itself (`.mad` tests, C-family sources madc parses — the running madc IS
  the compiler; the runner already invokes `bin/madc`, and a later refinement
  may hand the `.mad` family to `parse_run` in-process behind the SAME
  fixture owner); **external** = a toolchain madc does not embody (the g++
  doctest binaries, a gcc/clang oracle, a project's own command) — the
  manifest names the command, the runner or the process owner runs it,
  the result is the same event. `alRUNNABLE` is true when either seat can
  serve the asset's family on this node.
- **Proposals get their `checks`**: `graph.accept` (and `graph.propose` when
  asked with `checks: true`) runs the tests linked by `tests` to the touched
  symbols (`test.candidates(ref)` — explicit links only; the honest fallback
  when none are linked is the family's whole suite, named as such) and
  records the `testrun` events under the proposal (`proposal: seq`), so the
  Nexus §16 shape (`Tests 281/281 passed` beside `Diagnostics 0 errors`) is
  data on the record. A failing check never blocks a human's `accept` — it
  informs (the §3.4 constraints rule); an agent's proposal with failing checks
  stays `open` with the runs attached.
- **Verbs** (a fourth family, `test.*`; tiers: reads observer, `test.run`
  editor — it spends build/run resources, `cmd_min_tier`'s rule): `test.list
  (family?)`, `test.discover`, `test.candidates(ref)`, `test.run(ids…)`,
  `test.results(test?, since?)`. Layers: `alTESTABLE` for the reads,
  `alRUNNABLE` for `run`.

## 4. Components

### 4.1 `third_party/libgit2` — vendored the MIR way (L4a)

- **Subtree** of upstream `libgit2/libgit2` at a release tag (1.9.x), no
  source edits (ZERO divergence — unlike MIR, we carry no fixes; a needed fix
  goes upstream first). History preserved like MIR (`docs/plans/mir-into-madc-
  repo-2026-08-11.md` §4.2).
- **Build**: libgit2 ships CMake; we do not run it. A madc-owned
  `third_party/libgit2-madc/Makefile.madc` (BESIDE the subtree, so a subtree
  pull can never touch a madc file — the webview precedent) lists the same
  sources and defines CMake would use and builds `obj/libgit2/<variant>/libgit2.a`,
  invoked from `src/Makefile` exactly like `$(MIRLIB)` (`FORCE` delegation,
  per-variant dirs, `git2clean` beside `mirclean`). Beside it, the two headers
  CMake would generate, committed per platform (`linux/`, `darwin/`, `win32/`):
  `git2_features.h` and the bundled pcre2's `config.h`. (SHIPPED as L4a,
  2026-09-13 — the plan `2026-09-13-nexus-L4a-git-substrate-plan.md`.)
- **Features** (the owner's "network off"): `GIT_THREADS 1` (libgit2's own
  locking; we still confine use to one thread, §7); `GIT_HTTPS`, `GIT_SSH`,
  `GIT_NTLM`, `GIT_GSSAPI`, `GIT_WINHTTP` and every TLS backend UNDEFINED;
  regex = the bundled `deps/pcre2` (`GIT_REGEX_BUILTIN` — one behaviour on all
  three lanes; mingw has no `regcomp`); SHA1 = the bundled collision-detecting
  backend (`GIT_SHA1_COLLISIONDETECT` — libgit2 1.9 has no plain builtin SHA1)
  + `GIT_SHA256_BUILTIN` (no OpenSSL/CommonCrypto dependency); zlib = the
  system `-lz` madc already links (every lane); `deps/xdiff` (diff/blame) and
  `deps/llhttp` (the http parser upstream compiles unconditionally) in;
  `deps/ntlmclient`, `deps/winhttp`, `deps/zlib`, `deps/chromium-zlib` OUT.
  The plain-HTTP / smart-protocol / `local` transport objects still link
  (`branch.c → remote.c → transport.c`'s table references them; ~150 KB;
  unreachable from madc — the one-git-owner gate) rather than patching
  upstream.
- **Gate** `scripts/check-libgit2-features.sh` (fulltest): fails when any
  committed features header enables HTTPS / SSH / NTLM / GSSAPI, and fails
  when a file under `deps/{llhttp,ntlmclient}` appears in `Makefile.madc`;
  negative control = a temp copy with `GIT_HTTPS 1`.
- **Size — MEASURED 2026-09-13** (the L4a spike, this container, clang 18
  -O2): archive 3.2 MB; a program linking exactly the read API `GitRepo` uses
  is 1.40 MB stripped; zero warnings under upstream's own warning set. Under
  the 3 MB stop-and-ask line, so the slice proceeded. darwin and win64 lanes
  build it through the same per-variant rules with their toolchains; libgit2
  CI covers MinGW and macOS, so the risk is our feature header, not upstream
  — a wrong macro is a compile error there, never a silent difference.

### 4.2 `madc::GitRepo` + the `git` source adapter (L4a, C++ in madcdis)

ONE libgit2 wrapper (`include/madcdis/git_repo.h`, `src/madcdis_git_repo.cpp`)
— the single owner of every `git_*` call in madc. READ-ONLY by design:

```
class GitRepo {                       // open(path) discovers the repo upward
    bool open(const std::string &path, error *err);   // worktree or .git
    bool head(GitRef &out);            // {sha, branch, detached}
    bool revparse(const std::string &spec, std::string &sha);
    bool log(std::vector<GitCommit> &out, const std::string &path, size_t limit);
                                       // revwalk time-ordered; `path` filters by
                                       // comparing the entry OID with each parent's
                                       // (exact, two tree lookups per commit)
    bool show(const std::string &sha, const std::string &path, std::string &text);
    bool blame(std::vector<GitBlameRow> &out, const std::string &path,
               size_t line0, size_t count);           // {line, sha, author, when, summary}
    bool status_dirty(const std::string &path, bool &dirty);
};
```

`git_libgit2_init / shutdown` happen once per process (a static guard in the
wrapper). Errors go through `madc::error` with libgit2's message; a
non-repository path answers `false` + prose, never a crash.

The **source adapter** `git_source_adapter : SourceAdapter` (in
`src/madcdis_git_repo.cpp` with `GitRepo` — one TU for the substrate) accepts
`git://<repo path>[?path=<file>]` (a `{ "git", storage, file, path_like, local }`
row in `DataSource::scheme_info`; the adapter owns the one split of its query)
and extracts record families `commit` (the log, path-filtered when `?path=`
is given), `ref` (every branch and tag) and `blame` (over the whole file
named by `?path=`) as `value` objects with a `SourceLocator` — the madcdis
query layer's face on history (a `DataSet<commit>` later; not built until it
has a caller). The row shapers (`git_commit_value` / `git_ref_value` /
`git_blame_value`) are declared beside the structs so the adapter and the
engine face (`src/madc_git.cpp`: the handle table + the value-shaped
`internal_program_git_*`) answer ONE spelling of each row.

The **dialect face** (how the seat reaches it — the `parse_*` precedent,
mangled-direct in `include/madc/ns_madc`): `madc::git_open(path) → handle`,
`git_close`, `git_head(out, h)`, `git_revparse(out, h, spec)`,
`git_log(out, h, path, limit)`, `git_show(out, h, sha, path)`,
`git_blame(out, h, path, line0, count)`, `git_dirty(h, path)`. All are thin
`value`-shaped wrappers over `GitRepo` (five-layer plumbing as L1–L3).

Why libgit2 in-process and not `exec://git` (the client-server design §3
anticipated a subprocess provider): the release lanes must not depend on a
`git` binary at runtime (Windows/mac installs), blame per node needs
structured rows not text scraping, and revision handles need blob bytes fast.
The exec form stays available to TESTS for building fixtures (§8).

Gate `scripts/check-one-git-owner.sh`: no `git_` call and no `exec://git`
outside `madcdis_git_repo.cpp` in `src/`, `include/`, `tools/` (tests exempt
for fixtures); negative control included.

### 4.3 PAST verbs in the seat (L4b; all observer-tier reads)

Added to the ONE descriptor table + `graph_verb` enum + `graph_call` switch
(which structure answers each — the index-is-not-the-graph law):

| Verb | Answer | Structure |
|---|---|---|
| `graph.status()` | `{handle, generation, synced, errors, log_head, revisions: [tags], git: {head, branch, dirty}}` | the doc bag + `clog_head` + `GitRepo::head/status_dirty` |
| `graph.source(id)` | `{node, text}` — the span's bytes (live or revision handle) | `graph_span` + the handle's text |
| `graph.history(id)` | `{node, rows: [{provenance: git\|event, when, actor, sha?, seq?, verb?, summary}]}` merged oldest → newest | git: `blame` over the span's lines, folded to distinct commits; event: `nodeop` records whose target symbol key matches + `splice` records that intersected the span AT THEIR TIME (the span walked backwards through newer splices with the inverse of `shift_anchors`' arithmetic — one helper `span_before_splice`, beside the owner, never a copy) |
| `graph.commits(limit, path?)` | `{rows: [{sha, author, when, summary}]}` | `GitRepo::log` |
| `graph.revision(rev)` | `{sha, tag}` — opens (or reuses) the revision handle; every id it later answers carries `tag` | `git_show` → `parse_open_tagged` |
| `graph.diff(rev_a, rev_b)` | `{added, removed, changed}` at DECLARATION granularity: `graph.symbols` of both, keyed `{kind, name}`, `changed` = span text differs; `""` = the live buffer | two handles' `graph_symbols` + `graph_source` |

`graph.history` rows carry `provenance` (§3.7); nothing in it is heuristic
(no `inferred` row exists in v1). Rename / move
survival (Nexus §9 "Moved … Renamed …") is NOT v1: history follows the symbol
KEY within its file; a renamed symbol's history begins at the rename, and the
row says so — the named hard problem stays named (client-server §6).

Every other `graph.*` read verb is unchanged and transparently answers for a
revision id (§3.3 routing). The three mutation verbs refuse a revision id.

### 4.4 The `propose` tier in the seat (L4c)

- `tierPROPOSER` (§3.6); `graph_edit_apply(…, ops, mode)` with
  `enum edit_mode { emAPPLY, emPROPOSE }` over the ops LIST (§3.4 — the L3
  single-op body becomes the one-element case: one candidate carrying every
  splice, one validation, one checkpoint, all splices or none): identical
  through the candidate's validation; `emPROPOSE` calls `parse_would_accept`,
  computes `preview`, appends the `ckPROPOSAL` record, answers `{ok,
  proposal: seq, preview, diagnostics, constraints}`; nothing else changes
  (not the buffer, not the tree, not the undo stack). A rejected candidate
  still answers `{ok:false, diagnostics}` and stores NOTHING (a proposal that
  cannot parse is not a proposal). `constraints` is empty until L4d's
  records exist; then it lists the decisions linked (`affects`, `documents`)
  to the target's symbol key — informing the proposer (Nexus §8: "this
  proposal conflicts with Decision #37"), never blocking.
- New verbs (descriptor rows + enum + switch): `graph.proposals(status?)`,
  `graph.proposal(seq)`, `graph.accept(seq)`, `graph.reject(seq, reason)`,
  `graph.withdraw(seq)`; the fold `proposal_status_of(seq)` reads the
  stream's decisions (§3.4).
- The connection-level wiring test the L3 review deferred lands HERE with
  the `testmadcide_serve_tiers` harness: a real observer refused, a real
  proposer stored, a real editor landing — through `serve_client_task`.

### 4.5 Nexus records + the MCP client + the mycenode adapter (L4d)

- **`nexus.*` verb family** (a third family beside the registry commands and
  `graph.*`; `nexus_verb` enum, `nexus_min_tier`, one switch):
  `nexus.record(id)`, `nexus.records(rkind, state?)`, `nexus.create(rkind,
  fields)`, `nexus.update(id, fields)`, `nexus.close(id)`, `nexus.link(from,
  rel, to)`, `nexus.unlink(...)`, `nexus.context(ref)` — the RAW list of
  everything linked to a ref (records, links, proposals, commits), each row
  with `provenance` (the Context view, Nexus §6, as data), `nexus.sources()`,
  `nexus.sync(source)`, and ONE compound verb:
  **`nexus.explain(ref, detail?)`** — the review's `explain(entity)` and the
  codegraph "one call instead of eight" lesson in one: a compact projection
  over the existing verbs, no new structure —
  `{ what: graph.node + type_of + span (compiler),
     depends: callers/callees counts + impact summary (compiler),
     history: graph.history rows (git + event),
     why: linked decisions + requirements + the proposals that touched it (record),
     plan: open tasks / milestones linked by planned_for / requested_by (record),
     constraints: the decisions that affect it (record) }`.
  `why(this)` is `explain`'s `why` section; `plan(this)` its `plan` section
  — sections, not separate verbs (one implementation, one shape). `detail`
  is an enum (`summary | full`; the L2 `detail` increment shares it).
- **The MCP client** (`tools/madcide/madcide_mcpclient.inc`, dialect, over
  `madc::channel`): `mcp_client_open(out, manifest)` spawns the server
  (`exec://<command…>`), performs `initialize` + `notifications/initialized`;
  `mcp_client_tools(out, c)`; `mcp_client_call(out, c, tool, args)` with
  JSON-RPC ids, replies matched by id, a deadline through `poll_state` /
  `sleep_ms` in the cooperative loop (never a blocking read), a dead child
  answered as `{error}` with `exit_status`. Transport = `enum mcp_transport {
  mtSTDIO, mtHTTP }`; **stdio only in L4** — Streamable HTTP needs an
  `http://` channel in madcdis (the WebSocket framer already writes the
  handshake half), a later madcdis slice; until then a stdio ↔ HTTP bridge
  process (the standard `mcp-remote`) is the owner's configuration, not
  our code.
- **Server manifests as DATA**: `profiles/mcp/<source>.mcp.json`:
  `{ name, transport: "stdio", command: [...], env: {...}, vocabulary: {
  list_tasks: { tool, args, map }, get_task: …, create_task: …, update_task:
  …, comment: … } }`. Loaded ONCE into an enum-indexed table
  (`enum nexus_op { noLIST_TASKS, noGET_TASK, noCREATE_TASK, noUPDATE_TASK,
  noCOMMENT }`); an unknown vocabulary key or a tool the server's
  `tools/list` does not advertise REFUSES the manifest with its line — the
  enum-law boundary. `map` = result field → record field (a flat rename
  table; a nested path is `a.b`).
- **The source adapter** (dialect, `nexus_sync(source)`): pulls through the
  client, folds results into `ckRECORD` events with `origin = source`,
  `ext_key` = the owner system's id, `state` mapped — the cached projection of
  ruling 2; native records (`origin ""`) are never touched by a sync. A
  `nexus.create` on an external-owned kind with `origin` set pushes through
  `create_task` and records the returned key.
- **mycenode first** (the owner wires its manifest — command, auth, scope);
  the test's server is a small dialect MCP FIXTURE server
  (`tests/mcp_fixture_server.mad` + `.helper`) advertising the five tools with
  canned rows — the external system's stand-in (we cannot ship mycenode). Jira
  second: only a manifest, IF its transport is stdio-bridged; otherwise it
  waits for the `http://` channel.

## 5. Data flow (the four paths)

1. **History**: `graph.history(id)` → route id (§3.3) → `graph_span` →
   [git] `git_blame(path, l0, n)` → distinct shas → rows; [session] walk the
   stream newest → oldest: `nodeop` with the same symbol key → row; `splice`
   → intersect with the span-before-this-splice → row; merge by time; answer.
2. **Revision**: `graph.revision("HEAD~3")` → `git_revparse` → `git_show(sha,
   path)` → `parse_open_tagged(text, path, tag)` → cache → `{sha, tag}`; a
   later `graph.symbols`/`graph.body`/… with a tagged id → the revision
   handle; LRU close through `parse_close`.
3. **Propose → land**: proposer `graph.replace(id, src)` → sync precondition →
   read-only rule → span → splice(s) → ONE candidate on the scratch buffer →
   `parse_would_accept` → preview → constraints (decisions linked to the
   target, L4d) → `ckPROPOSAL` → answer. Editor `graph.accept(seq)` → fold
   status (`open`?) → the three re-checks (§3.4) per op →
   `graph_edit_apply(ops, emAPPLY)` — all splices or none → nodeop carries
   `proposal` → `ckDECISION accepted {landed_seq}` → broadcast → answer
   `{ok, node, seq}`.
4. **Sync**: `nexus.sync("mycenode")` → manifest (loaded once) →
   `mcp_client_call(list_tasks)` → map → for each row: `ckRECORD` upsert by
   `(origin, ext_key)` (the fold dedupes: latest wins) → answer counts.

## 6. Refusals (never silent; the registered-prose rule)

| Condition | Answer |
|---|---|
| not a git repository / rev unknown / path not in tree at rev | `{error: "<libgit2 message>"}` isError |
| a revision id to a mutation verb | `"revision handles are read-only"` |
| a tagged id whose handle was closed | the ordinary stale refusal `{stale:true}` |
| proposer calls a mutation without a stored proposal path (e.g. `graph.accept`) | the tier refusal shape (`graph_min_tier`) |
| proposal not `open` / target changed / doc moved | `"proposal <seq> is <status>"` · `"proposal target changed since seq N; re-propose against fresh ids"` · the L3 re-sync refusal |
| candidate rejected at propose time | `{ok:false, diagnostics}`; nothing stored |
| manifest names an unknown vocabulary key / tool not advertised / server exits | refuse the manifest with its line · `{error, exit_status}` |
| a `ref` of unknown kind or a relation not in the enum | refused at the boundary (`ref_kind_of` / `nexus_rel_of` = NONE) |
| `graph.diff` between two revisions with different files | `"path <p> absent at <sha>"` |

## 7. Thread-safety contracts (the law: stated per piece)

| Piece | Contract |
|---|---|
| `GitRepo` / libgit2 | CONFINED to the session (UI) thread like every handle; `GIT_THREADS` is on for libgit2's internal correctness only. `git_libgit2_init` once per process behind a static guard. A blame or a log over a large history runs synchronously on the session thread in v1 (bounded by `limit`); the cooperative-task pump for long walks is the F2 seam, signatures unchanged. |
| Revision handles | Per-doc, session-thread-owned, closed with the doc. Same contract as the live handle. |
| The one event stream | Appended by the session thread only (unchanged); the nexus fold is a per-session cache invalidated by `seq`. |
| The MCP client | A child process over `exec://`; reads/writes from cooperative tasks on the session thread (channels park, never block a thread); one outstanding request per client in v1. |
| Manifests | Loaded once at session start (or `nexus.sources` reload, an owner verb); immutable after load. |

## 8. Testing / gates (targeted per slice; the battery at the V6 seam)

- **L4a** unit `tests/unit/test_gitrepo.cpp`: builds a fixture repository IN
  THE TEST through libgit2's own write API (init, two commits touching one
  file, a rename), then asserts `head`, `revparse`, `log` filtered by path,
  `show` bytes, `blame` rows, a non-repo path refuses. Dialect
  `tests/testgit.mad`: the fixture built with `git` via `exec://` (every lane
  that runs the suite checked the repo out with git), then the `madc::git_*`
  publics. Gates: `check-libgit2-features.sh`, `check-one-git-owner.sh`,
  the size report. Rule trailers on every `src/`/`include/` commit.
- **L4b** `tests/testgraphpast.mad`: `graph.status` shape; `graph.source` ==
  the span bytes; `graph.revision` → tagged ids route (a live verb answers
  the revision's node; a mutation refuses; a closed tag is stale);
  `graph.diff` added/removed/changed on a two-commit fixture;
  `graph.history` rows from a splice + a nodeop + a commit, ordered.
  `tests/testmadcide_changelog` byte-identical for one doc + a two-doc
  project-stream case; the single-owners gate extended to `clog_kind_of`.
- **L4c** `tests/testmadcide_serve_propose.mad` (the `serve_tiers` harness,
  real connections): observer refused; proposer's `graph.replace` stored
  (preview + diagnostics; buffer unchanged; log has `ckPROPOSAL`); editor
  `graph.accept` lands (nodeop carries `proposal`; decision record; `seq`
  advanced); a second proposal whose target then changes is refused at
  accept; `graph.reject` + `graph.withdraw`; `graph.proposals(open)` counts.
- **L4d** `tests/testnexus_records.mad`: create / update / close / link /
  unlink / records / context; persist → restore round trip; a bad relation
  refused. `tests/testmcpclient.mad`: spawns the fixture server, `tools/list`,
  a call, a dead-server error; `nexus.sync` folds five rows with `origin`;
  a manifest with a misspelled vocabulary key refuses with its line.
- Every new refusal in §6 is asserted as `isError` in the seat tests.

## 9. Slices (each banks on the arc branch with targeted gates)

| Slice | Content | Gate |
|---|---|---|
| **L4a** git substrate | libgit2 subtree + `Makefile.madc` + features headers + Makefile wiring (all variants) + size spike; `GitRepo`; `git_source_adapter` + the `git` scheme row; `madc::git_*` publics | §8 L4a |
| **L4b** PAST verbs | project-scoped stream + `clog_kind` enum; `parse_open_tagged` + revision-handle routing; `graph.status / source / history / commits / revision / diff` | §8 L4b |
| **L4c** propose | `tierPROPOSER`; `parse_would_accept`; `edit_mode`; proposal + decision records; `graph.proposals / proposal / accept / reject / withdraw`; the connection-level wiring test | §8 L4c |
| **L4d** intent | record/link kinds + `nexus_fold`; `nexus.*` verbs; the MCP client (stdio); manifests as data; `nexus_sync`; the fixture server; mycenode manifest slot; the `test` record kind + `tests` relation + `test.discover` (records only) | §8 L4d |
| **L4e** verification | `asset_layers_of` + per-family `*_min_layer` gates (retrofits `graph.*` and `nexus.*`; `graph.status` / `nexus.explain` report the layer set); `--report=json` on the canonical runner; `test.list / candidates / run / results`; `testrun` events tagged by node; proposal `checks` on accept/propose | a two-asset fixture project (a `.mad` and a binary) refused/served per layer; a run through the real runner yields a `testrun` event; a proposal with a linked failing test stays open with the run attached |
| later (not L4) | `http://` channel → Streamable HTTP MCP servers (Jira); recipes (N-op proposals with a per-file diff view); rename/move survival; a madcdat index over the stream; `graph.explore` / `detail` enum / `graph.impact(depth)` (L2 increments); the `.mad` family run in-process behind the one fixture owner | own plans |

Engine commits (`src/`, `include/`, `third_party/libgit2/Makefile.madc`)
carry the four rule trailers; dialect and doc commits ride without.
`/dupaudit` scoped to madcdis + the seat before the seam merge.

## 10. Decided defaults (owner veto welcome) and the forks behind them

1. **One project-scoped stream for proposals and records** (chosen) vs a
   separate madcdat-backed store. One stream = one `seq`, one persistence
   rule, one compaction, one fold; the store is a later disposable index.
   Cost if wrong: a very large record set makes the fold slow — the index
   arrives then.
2. **Revision ids by generation TAG** (chosen) vs a `rev` argument on every
   id-taking call vs a parallel `graph.rev.*` family. The tag makes a wrong
   handle impossible (a mismatch is the stale refusal), keeps every verb's
   signature, and costs one assignment in the engine.
3. **The MCP client in dialect over `exec://`** (chosen) vs a C++ `mcp://`
   DataDriver. The server seat is dialect; the client mirrors it; `madc::channel`
   already spawns and pumps a child. The `mcp` scheme row stays reserved for
   the C++ driver the query layer may want later (one implementation then —
   the dialect client would move behind it, not beside it).
4. **`proposer` as a fourth tier level** (chosen) vs a per-client flag. A
   level rides every existing gate and `clienttier`; a flag is a second
   mechanism (client-server §2.5: "no third mechanism").
5. **Byte-exact target re-check at landing** (chosen) vs. re-locating a moved
   target. Exact and loud; re-location (yailPeralta re-anchors by symbol
   path) is a later refinement once rename survival exists.
6. **Records fold in dialect** (chosen) vs. an engine-side query. Hundreds of
   records; the same walk `clog_replay` does; the engine's madcdis DataSet face
   arrives with the `git://` adapter's first caller.
7. **Bundled pcre for libgit2's regex on every lane** (chosen) vs. POSIX
   `regcomp` on posix + pcre on win64. One behaviour everywhere; +~300 KB.
8. **`milestone` as a fifth record kind** (chosen; the owner named four) vs.
   a task with a flag. `planned_for` needs a target that is not itself work;
   a milestone is that target (Nexus §7 "Target v1.1").
9. **`nexus.explain` as ONE compound verb with sections** (chosen) vs.
   separate `why / plan / constraints / explain` verbs (the review's list).
   One implementation, one shape, one call for the agent; the sections keep
   the review's names.
10. **Order PAST → propose → intent** (owner-settled) vs. the review's
    "intent before transactions". Kept: the propose tier v1 is single-op and
    cheap, and its record already carries the N-op transaction, so nothing is
    built twice; `explain` lands last because it projects all three axes.
11. **Asset layers as a capability bitset computed by ONE function** (chosen)
    vs. per-verb ad-hoc checks. One owner reads the file-kind registry, the
    manifest, the repository and the runner table; every verb family declares
    its layer as data beside its tier; refusals name the missing layer.
12. **Binary project members keep COMMIT-level git history** (`log`, `show`
    bytes) while the LINE-level verbs (`blame`, revision handles, `diff`) are
    text-only (chosen) vs. no git layer at all for binaries (the owner's first
    phrasing). A binary asset's "who last changed this" is still a fact worth
    answering; only line semantics are meaningless for it.
13. **The canonical runner stays the ONE test harness; the Nexus is its
    client** (chosen) vs. a dialect re-implementation of the fixture protocol.
    Two readers of `.claude/rules/test-fixtures.md` would be the divergence
    class `/dupaudit` hunts; the runner gains a machine-readable report line
    instead. Moving the `.mad` family in-process later happens behind that one
    owner, not beside it.
14. **Verification lands as its own slice L4e after intent** (chosen) vs.
    folding it into L4c/L4d. Test records need L4d's record machinery; the
    runner seat and the layer gates are one coherent slice with one gate.
    Owner veto welcome if tests should come before the MCP client.

## 11. Owner laws honoured

Running-madc-IS-the-compiler (revision handles are parse handles of the same
process) · no-user-program-cache (revision handles and the fold are bounded
derivatives; the stream is the owner's record) · entity-handles-never-byte-
offsets (a proposal's op re-checks the NODE, the splice is derived) · index-is-
not-the-graph (each verb names its structure, §4.3) · enums-not-strings
(`clog_kind`, `ref_kind`, `nexus_rel`, `record_kind`, `edit_mode`,
`mcp_transport`, `nexus_op`, `tierPROPOSER`; text only on the wire, converted
once) · thread-safety (§7) · no-parallel-implementations (one validator with a
`commit` flag; one git owner; one stream; one fold) · every-mutation-through-
a-verb (landing = `graph_edit_apply`) · MCP-is-an-adapter (the client is a
channel + a manifest) · madcdis-stays-DataDef-agnostic (the git adapter emits
`value` records) · dialect-lean / value-first (`var`, out-param carriers, no
`std::` in `.inc`) · push only to owner remotes · the battery at the seam.

## 12. Deferred (named, not blockers)

Rename/move survival (GumTree / semantic diff); body-level `graph.diff`;
recipes as N-op proposals with a per-file diff View; `http://` channel and
Streamable HTTP MCP; a madcdat index over the stream; test records + the
`tests` edge (test candidates; the proposal's `checks` seat); the L2
increments (`graph.explore`, `detail` enum, `graph.impact(depth)`); the
parser `;` quirk (its own session); L3 minors M5/M8; fixture leaks in
`testmadcide*`.

From the external review, deferred with their seats named:
- **Rich ChangeEvents** — `rename`, `move`, `signature-change`,
  `subtree-replace`, `extract-function` as SEMANTIC verbs recorded in the
  `nodeop` record (the `graph_verb` enum grows; `graph.history` reads them
  generically). `graph.rename(id, name)` is the first: an N-op proposal
  (the declaration + every L2 reference) landing all-or-nothing under §3.4's
  contract — the transactional mutation the review describes, built on the
  seat this design lays.
- **Persistent semantic identity** (the review's step 7, our named hard
  problem): an `entity` record kind whose lineage links (`renamed_from`,
  `moved_from`, `signature_changed_from`) are CREATED by the rich
  ChangeEvents above — never reconstructed from diffs. `rkSYMBOL` refs
  re-resolve through the entity when it exists. `graph.lineage(ref)` reads
  that chain. No enum value is reserved ahead of use (cruft law); the wire
  names above are the design's, so a later slice does not collide.

## 13. Traceability

- Design doc §7 L4 → this document; client-server design §2.4 / §2.5 / §2.7 /
  §3; Nexus vision §2 / §6 / §7 / §8 / §9 / §10 / §16 / §20 / §21 / §22 / §25;
  cross-reference §5 rulings 1 / 2 / 6.
- Recon: `tmp/sdd-ast-graph-mcp-L4/recon.md` (gitignored; verdict in
  `claude_status.json` UPDATE 10).
- The external review (ChatGPT, 2026-09-13, supplied by the owner) → §2.1
  point by point; it changed §1 (positioning), §3.4 (transactional ops,
  `constraints`, `checks`), §3.5 (envelope, `state`, `milestone`), §3.7
  (authorities + `provenance`), §3.8 (source durable), §4.5 (`explain`), §10
  (8–10), §12 (rich ChangeEvents, persistent identity).
- Owner amendment 2026-09-13 (after L4a shipped): the neglected verification
  axis and the optional layering of every axis per asset → §1 (a fifth
  question + the layer sentence), §3.9 (asset layers), §3.10 (tests as
  records, runs as events, the canonical runner as the one harness), §9
  (slice L4e; `test` records in L4d), §10 (11–14). KG Decisions
  `nexus_asset_layers`, `nexus_verification_axis`.
- KG: Feature `code_graph_mcp` L4 opened; Decisions `nexus_past_via_libgit2`,
  `nexus_propose_tier_fourth_level`, `nexus_one_stream_for_records`,
  `nexus_revision_ids_by_generation_tag`, `nexus_mcp_client_dialect_stdio` to
  land with the owner's approval of this document.
