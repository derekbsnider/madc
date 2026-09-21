# Nexus L4d — intent records + links in the ONE stream, the `nexus.*` family (`explain` as one compound verb), the MCP CLIENT over `exec://`, manifests as data, `nexus.sync`, the fixture server, `test.discover` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Nexus its INTENT axis: tasks, requirements, decisions,
worksets, milestones and tests as RECORDS in the one change stream, linked to
code by durable symbol keys through an enumerated relation vocabulary; one
compound `nexus.explain(ref)` that projects the three axes (compiler, history,
intent) for a symbol in one call; proposals informed by the decisions that
constrain their target; and the first external source — an MCP server the
madc engine CALLS as a client (mycenode first, a fixture server in the tests),
with its manifest as data and `nexus.sync` folding its rows into records.

**Architecture:** Four dialect pieces on top of L4c's stream and seat.
(1) **Vocabulary** (`madcide_enums.inc`): record kinds / states / ops, link
ops, ref kinds, relations, provenance, the `nexus.*` and `test.*` verb codes,
the manifest's operation vocabulary, the transport enum, the test families —
every wire word converted once. (2) **`madcide_nexus.inc`**: `nexus_fold`
(records + links folded from the stream, cached on the log entity by seq),
refs (`ref_of`, `ref_key`, symbol resolution through `graph.definition`), the
record / link writers, `nexus.record / records / create / update / close /
link / unlink / context / explain`, `nexus_constraints` (the decisions that
constrain a symbol — the `constraints` seat L4c left empty), and
`test.discover` (test records from the canonical fixture conventions).
(3) **`madcide_mcpclient.inc`**: manifests loaded ONCE from
`<profiles>/mcp/*.mcp.json` into an enum-indexed table (`nexus.sources`), the
MCP client over `madc::channel exec://` (spawn, `initialize`,
`notifications/initialized`, `tools/list`, `tools/call` with ids matched by
id under a cooperative deadline, a dead child answered with `exit_status`),
and `nexus.sync(source)` folding the source's rows into `record` events
(upsert by `(origin, ext_key)`). (4) **Seat wiring** (`madcide_mcp.inc`):
the `nexus.` and `test.` families dispatched beside `graph.`, their
descriptors in `tools/list`, `nexus_min_tier` / `test_min_tier`; the `--mcp`
stdio server flushes every reply (found on the way: a piped reply sat in the
stdio buffer).

**Tech Stack:** dialect `.inc` (value-first), `madc::channel` (`exec://`,
`readline`, `poll_state`, `sleep_ms`, `exit_status`), the L4c stream
(`clog_find`, `clog_append`, `keyed_get`), the L1–L4c `graph.*` verbs,
`perl::glob`, `php::file_get_contents`, `js::parse/stringify`, `build_subst`
(`{madc}`), `.mad` tests + a `.helper` fixture server.

**Spec:** `docs/plans/2026-09-13-nexus-L4-design.md` §3.1 (one stream), §3.2
(actors — `actor` = the connection id, L4c amendment), §3.5 (records, links,
refs, relations, the fold), §3.7 (provenance), §3.10 (the `test` record kind,
discovery by convention — records only here), §4.5 (the `nexus.*` family,
`explain`, the client, manifests, the source adapter, mycenode first, the
fixture server), §5 flow 4, §6 refusals, §7 contracts, §8 L4d tests, §9 L4d
row, §10 defaults 3, 6, 8, 9. Owner approval of the design: 2026-09-13; the
L4c recap's "Next is the L4d plan" acknowledged 2026-09-14 ("ok, I think
that's fine").

## Decisions this plan settles

1. **A record's `id` is the seq of its `create` event, DERIVED by the fold.**
   The create event does not carry `id` (its seq is assigned by `clog_append`
   after the literal is built); `update` / `close` events name `id`. The fold
   applies them in seq order; `updated_seq` = the last event's seq,
   `created_seq` = the id.
2. **A ref's wire shape is `{ref: <word>, …fields}`** — `{ref:"record", id}`,
   `{ref:"symbol", file, kind, name}`, `{ref:"commit", sha}`,
   `{ref:"proposal", seq}`, `{ref:"external", source, key}`. `ref_kind_of`
   converts the word once; `ref_key(out, ref)` spells the ONE canonical key
   (`symbol:<file>\t<kind>\t<name>` etc.) links are matched by. A symbol's
   `file` is the doc's `path` bag value (the same spelling the stream's
   records carry).
3. **`nexus.close(id, state?)`** sets `state` to `done` by default, or to
   `superseded` / `rejected` when asked (a rejected decision is a record with
   `state: rejected`, design §3.5). `nexus.records` without a `state` answers
   the LIVE states (open / active / done); `state: "<word>"` answers that one.
4. **The fold is cached on the log entity** (`nfold`, `nfold_seq` on the
   `changelog` bag — the session's one entity) and recomputed when
   `clog_head` moved. Hundreds of records; no index.
5. **One child per `nexus.sync`.** A `madc::channel` is not a value (it cannot
   sit in a bag), and a sync is rare: spawn → `initialize` →
   `notifications/initialized` → `tools/list` (validate the vocabulary) →
   the calls → `close_write` + `close`. No persistent client handles in v1
   (design §7: "one outstanding request per client").
6. **A manifest's `env` renders as an `env K=V …` prefix** on the spawned
   command (the `exec://` factory carries args only — `ProcessOptions` has an
   `environment` slot no channel spelling reaches yet; a madcdis follow-up).
   On win32 a manifest with `env` refuses at load ("env: not supported on
   this platform"). `command` is an array joined by single spaces (the
   factory splits on single spaces — no quoting layer, like build commands);
   `{madc}` / `{project}` substitute through `build_subst`, the ONE owner.
7. **The manifest's location is `<profile_dir>/mcp/*.mcp.json`**
   (`resolve_profile_dir`'s owner); `nexus.sources(dir?)` — an OWNER verb when
   `dir` is given — (re)loads from another directory (the tests load their
   scratch manifests this way). A manifest refuses with its FILE and the
   offending KEY (a parsed JSON value has no line number — the design's "with
   its line" becomes "with its file and key").
8. **`nexus.sync` results are the tool's first text content parsed as JSON**
   (an array of rows, or `{rows: [...]}`); `map` renames result fields to
   record fields (`{"summary": "title", "status": "state"}`); `state` words
   map through `record_state_of` (an unknown word → `open`); the row's `key`
   field (after mapping) is `ext_key`. Upsert: an existing record with the
   same `(origin, ext_key)` gets an `update` event, else a `create`.
9. **`test.discover(dir?)`** (default `tests`) folds `<dir>/*.mad` (skipping
   those with a `.helper` sibling — the runner's rule) into `rkind: test,
   family: mad` records and `<dir>/unit/*.cpp` into `family: unit`, `origin:
   "convention"`, `ext_key` = the path, upserted like a sync. No per-test
   knowledge. `test.*` gets its own file (`madcide_tests.inc`) so L4e's
   runner seat grows there.
10. **`explain` takes a SYMBOL ref (v1)** resolved against the served
    document through `graph.definition` filtered by kind; sections: `what`
    (node + span + type_of), `depends` (callees / callers / impact counts —
    functions only), `history` (graph.history rows), `why` (linked decisions +
    requirements + the proposals that touched it), `plan` (live tasks /
    milestones linked by `planned_for` / `requested_by` / `implements`),
    `constraints` (decisions linked by `affects` / `documents`). Every
    row carries `provenance`; `detail: full` adds `members`/`bases` for a
    type and the full history; `summary` (default) caps history at 10 rows.
11. **`graph_history`'s literal provenance words move behind
    `provenance_name`** (the enum law; found while adding the converter).
12. **`run_mcp` flushes stdout after every reply** (found on the way: the
    `--mcp` stdio server's replies sat in libc's pipe buffer until exit — the
    stdio test never saw it because it reads after exit). The fixture server
    flushes the same way. Own commit? No — the same dialect commit, named in
    its message (a one-line fix in the file the slice edits).

13. **Three PHP-parity publics land first (engine, Task 0):** `php::array_keys`
    (the dialect cannot enumerate an object's keys — range-for over an
    object-kind carrier reads size 0 by design, and the fold's field merge,
    the manifest's vocabulary and `map` tables, and a sync row's fields all
    need the keys), `php::mkdir` and `php::rmdir` (the tests' scratch
    directories; the engine spells the UCRT `_mkdir`/`_rmdir` as it does for
    `unlink`). A missing capability is fixed at the carrier, never spelled
    around (value-first law). Rule trailers on that commit.

## Global Constraints

Every task's requirements implicitly include this section.

- **ONE stream, ONE fold.** Records and links are events appended through
  `clog_append`; `nexus_fold` is the only reader that materialises them.
- **Enums, not strings**: every wire word (record kind / state / op, link op,
  ref kind, relation, provenance, verb names, manifest operations, transport,
  test family, explain detail) has an enum + `*_name` / `*_of` converters in
  `madcide_enums.inc`; a misspelling is a REFUSAL at the boundary; dispatch is
  a `switch`. The enum gate's converter list grows with them.
- **Refusals are prose, `isError` on the wire**; asserted in the tests.
- **Value-first dialect code**; `var`; out-param carriers; literals for
  objects; `keyed_get` / `arg_of` for optional keys; a map is a null var made
  an object by its first keyed write; `{}` is an empty ARRAY; no mixed-type
  ternary; `var == long` compares as an integer.
- **No engine change** in this slice (dialect + tests + one flush); no rule
  trailers needed unless `src/`/`include/` is touched.
- **Thread contract**: the fold is a per-session cache invalidated by seq; a
  sync's child lives inside one verb call on the session thread (cooperative
  reads, never a blocking thread); manifests load once and are immutable
  until `nexus.sources(dir)` reloads them (design §7).
- Targeted tests only; the battery rides the V6 seam.

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `tools/madcide/madcide_enums.inc` | modify | the L4d vocabularies + converters; `nexus_min_tier`, `test_min_tier` |
| `tools/madcide/madcide_nexus.inc` | create | fold, refs, writers, the `nexus.*` bodies, `nexus_constraints` |
| `tools/madcide/madcide_tests.inc` | create | `test.discover` (L4e grows the runner seat here) |
| `tools/madcide/madcide_mcpclient.inc` | create | manifests, the MCP client, `nexus.sources`, `nexus.sync` |
| `tools/madcide/madcide_mcp.inc` | modify | includes; descriptors for both families; `nexus_call` / `test_call` dispatch in `mcp_handle`; `constraints` in `graph_edit_apply`; `run_mcp` flushes |
| `tools/madcide/madcide_past.inc` | modify | provenance words through `provenance_name` |
| `scripts/check-madcide-enums.sh` | modify | the new converters join the anchor list; the two new files join the checked set |
| `tests/mcp_fixture_server.mad` (+ `.helper`) | create | the external system's stand-in: five tools, canned rows |
| `tests/testnexus_records.mad` (+ `.expect`, `.expect_quiet`) | create | records / links / context / explain / constraints / persist-restore / refusals |
| `tests/testmcpclient.mad` (+ `.expect`, `.expect_quiet`) | create | sources, sync (upsert), a dead server, a misspelled manifest, `test.discover` |
| `docs/plans/2026-09-13-nexus-L4-design.md` | modify | §9 L4d shipped; §4.5 amendments (decisions 5–8) |

---

### Task 0: Engine — `php::array_keys`, `php::mkdir`, `php::rmdir` (PHP parity)

**Files:** modify `src/ns_php.cpp` (beside `php_count` / `php_unlink`; the
extern shims beside `__php_count` / `__php_unlink_cstr`), `include/madc/ns_php`
(shims + publics beside `count` / `unlink`); create `tests/testphpdirs.mad`
(+ `.expect`, `.expect_quiet`).

**Interfaces (produces):**
```cpp
namespace php {
    void array_keys(array &out, value &arr);   // PHP array_keys(): an object's keys (sorted, the carrier's order) or a list's indices 0..n-1
    bool mkdir(const char *dir);               // PHP mkdir(): mode 0777 & ~umask; false on failure
    bool mkdir(const char *dir, int64_t mode);
    bool rmdir(const char *dir);               // PHP rmdir(): false on failure (non-empty, missing)
}
```

- [ ] **Step 1: The test** `tests/testphpdirs.mad`:

```c
// PHP parity (Nexus L4d task 0): array_keys over a map and a list; mkdir /
// rmdir round trip; the failures answer false, never throw.
int main()
{
    var m;
    m["beta"] = 2;
    m["alpha"] = 1;
    array keys;
    php::array_keys(keys, m);
    println("keys-map: n={} k0={} k1={}", php::count(keys), keys[0], keys[1]);
    var list = {};
    list.push("x");
    list.push("y");
    array idx;
    php::array_keys(idx, list);
    println("keys-list: n={} i0={} i1={}", php::count(idx), idx[0], idx[1]);
    var none;
    array nk;
    php::array_keys(nk, none);
    println("keys-null: n={}", php::count(nk));
    println("mkdir: {}", php::mkdir("madc_php_dirs_tmp") ? 1 : 0);
    println("exists: {}", php::file_exists("madc_php_dirs_tmp") ? 1 : 0);
    println("mkdir-again: {}", php::mkdir("madc_php_dirs_tmp") ? 1 : 0);
    php::file_put_contents("madc_php_dirs_tmp/a.txt", "a");
    println("rmdir-nonempty: {}", php::rmdir("madc_php_dirs_tmp") ? 1 : 0);
    php::unlink("madc_php_dirs_tmp/a.txt");
    println("rmdir: {}", php::rmdir("madc_php_dirs_tmp") ? 1 : 0);
    println("gone: {}", php::file_exists("madc_php_dirs_tmp") ? 0 : 1);
    println("rmdir-missing: {}", php::rmdir("madc_php_dirs_tmp") ? 0 : 1);
    return 0;
}
```

`.expect`: `keys-map: n=2 k0=alpha k1=beta` · `keys-list: n=2 i0=0 i1=1` ·
`keys-null: n=0` · `mkdir: 1` · `exists: 1` · `mkdir-again: 0` ·
`rmdir-nonempty: 0` · `rmdir: 1` · `gone: 1` · `rmdir-missing: 1`. `.expect_quiet`.

- [ ] **Step 2: Engine.** `php_array_keys(value *out, value *arr)`: `*out =
  value::make_array({})`; object → each key as a string value in
  `as_object()` order; array → `value((int64_t)i)` for `0..count-1`; else
  empty. `php_mkdir(const char *dir, int64_t mode)`: `::mkdir(dir, (mode_t)mode)
  == 0` (win32: `::_mkdir(dir) == 0`, `<direct.h>`); `php_rmdir`: `::rmdir(dir)
  == 0` (win32 `::_rmdir`). Shims `__php_array_keys(value *, value *)`,
  `__php_mkdir(const char *, int64_t)`, `__php_rmdir(const char *)`; the
  header wrappers with PHP's default mode `0777`.
- [ ] **Step 3**: build 0 warnings; `bash tmp/l3_check.sh tests/testphpdirs.mad`;
  `tests/testphp.mad` unchanged.
- [ ] **Step 4: Commit (trailers)** — `Hypothesis:` the dialect has no way to
  enumerate an object's keys (range-for over an object reads size 0 by the
  madarray_size rule) and no directory verbs; the L4d fold, manifests and
  tests need both — PHP parity fills each exactly. `Layer:` dialect
  (nexus_fold / manifests_load) → php::array_keys → value::as_object (the
  carrier); php::mkdir/rmdir → libc, the UCRT spelling in the engine as
  php::unlink does. `Searched:` "enumerate a value object's keys" → none in
  include/madc/ns_php / ns_madc (the L4b plan noted the gap: `php::array_keys
  does NOT exist`); "make a directory" → no public, no test precedent.
  `Oracle:` PHP array_keys / mkdir / rmdir semantics (keys of a map, indices
  of a list; false on failure); tests/testphpdirs.mad.

---

### Task 1: The vocabulary (`madcide_enums.inc`) + the enum gate

**Files:** modify `tools/madcide/madcide_enums.inc` (after `envelope_of`),
`scripts/check-madcide-enums.sh`, `tools/madcide/madcide_past.inc`
(`graph_history`'s two `"provenance": "git"` / `"event"` literals).

**Interfaces (produces):**
```c
enum record_kind  : unsigned char { rkNONE = 0, rkTASK, rkREQUIREMENT, rkDECISION, rkWORKSET, rkMILESTONE, rkTEST };
enum record_state : unsigned char { rsNONE = 0, rsOPEN, rsACTIVE, rsDONE, rsSUPERSEDED, rsREJECTED };
enum record_op    : unsigned char { roNONE = 0, roCREATE, roUPDATE, roCLOSE };
enum link_op      : unsigned char { loNONE = 0, loLINK, loUNLINK };
enum ref_kind     : unsigned char { refNONE = 0, refRECORD, refSYMBOL, refCOMMIT, refPROPOSAL, refEXTERNAL };
enum nexus_rel    : unsigned char { relNONE = 0, relIMPLEMENTS, relTESTS, relFIXES, relAFFECTS, relREQUESTED_BY,
                                    relDOCUMENTS, relSUPERSEDES, relBLOCKED_BY, relPLANNED_FOR, relREVIEWED_BY,
                                    relGENERATED_BY, relINTRODUCED_BY, relMODIFIED_BY };
enum provenance   : unsigned char { pvNONE = 0, pvCOMPILER, pvGIT, pvEVENT, pvRECORD, pvINFERRED };
enum nexus_verb   : unsigned char { nvNONE = 0, nvRECORD, nvRECORDS, nvCREATE, nvUPDATE, nvCLOSE, nvLINK, nvUNLINK,
                                    nvCONTEXT, nvSOURCES, nvSYNC, nvEXPLAIN };
enum test_verb    : unsigned char { tvNONE = 0, tvDISCOVER };
enum test_family  : unsigned char { tfNONE = 0, tfMAD, tfUNIT, tfEXTERNAL };
enum nexus_op     : unsigned char { noNONE = 0, noLIST_TASKS, noGET_TASK, noCREATE_TASK, noUPDATE_TASK, noCOMMENT };
enum mcp_transport: unsigned char { mtNONE = 0, mtSTDIO, mtHTTP };
enum explain_detail : unsigned char { edSUMMARY = 0, edFULL };
// each: const char *X_name(long) and long X_of(var &word) (NONE = unknown / null)
long nexus_min_tier(long code);   // reads observer; create/update/close/link/unlink proposer; sync editor; sources observer
long test_min_tier(long code);    // discover proposer (it writes records)
```

- [ ] **Step 1: The enums.** After `envelope_of`, one block per enum with its
  `_name` switch and `_of` loop (the `slot_of` shape: `for ( long k = FIRST; k
  <= LAST; k = k + 1 ) if ( word == X_name(k) ) return k;`). Words: record
  kinds `task requirement decision workset milestone test`; states `open
  active done superseded rejected`; ops `create update close`; link ops `link
  unlink`; refs `record symbol commit proposal external`; relations
  `implements tests fixes affects requested_by documents supersedes blocked_by
  planned_for reviewed_by generated_by introduced_by modified_by`; provenance
  `compiler git event record inferred`; families `mad unit external`; ops
  `list_tasks get_task create_task update_task comment`; transports `stdio
  http`; detail `summary full`. Header comment: "The INTENT axis's vocabulary
  (Nexus L4d, design §3.5, §3.7, §3.10, §4.5): text on the wire and on the
  persisted line, a code in every reader."
- [ ] **Step 2: Tiers.**

```c
// The nexus.* ladder (design §3.6's table): reads observe; a record is
// reviewable DATA, not source — create / update / close / link / unlink need
// the proposer tier; sync (pull from an external source) needs an editor;
// sources(dir) — a reload from another directory — is owner-only inside the
// verb (the plain listing observes).
long nexus_min_tier(long code)
{
    switch ( code )
    {
    case nvCREATE:
    case nvUPDATE:
    case nvCLOSE:
    case nvLINK:
    case nvUNLINK:
	return tierPROPOSER;
    case nvSYNC:
	return tierEDITOR;
    }
    return tierOBSERVER;
}

// The test.* ladder: discover writes records (proposer); L4e's run spends
// build resources (editor).
long test_min_tier(long code)
{
    switch ( code )
    {
    case tvDISCOVER:
	return tierPROPOSER;
    }
    return tierOBSERVER;
}
```

- [ ] **Step 3: The gate.** `scripts/check-madcide-enums.sh`: the awk anchor
  gains `|record_kind|record_state|record_op|link_op|ref_kind|rel|provenance|test_family|nexus_op|mcp_transport|explain_detail`
  (the converters are named `record_kind_name`, `rel_name`, …); `SEAT_FILES`
  gains `madcide_nexus.inc`, `madcide_tests.inc`, `madcide_mcpclient.inc`.
- [ ] **Step 4: `graph_history`**: the two row literals use
  `"provenance": provenance_name(pvGIT)` / `provenance_name(pvEVENT)` (a
  local `var pgit = provenance_name(pvGIT);` placed in the literal).
- [ ] **Step 5: Run** `bash scripts/check-madcide-enums.sh`, `bash tmp/l3_check.sh tests/testgraphpast.mad`
  (history rows unchanged: the words are the same).
- [ ] **Step 6**: no commit yet — the vocabulary rides the seat commit (Task 5).

---

### Task 2: `madcide_nexus.inc` — the fold, refs, writers, `nexus.*` bodies, `nexus_constraints`

**Files:** create `tools/madcide/madcide_nexus.inc`.

**Interfaces (produces):**
```c
void nexus_fold(var &out, long w);                 // {records: id-key -> rec, ids: [id…], links: [{from_key, rel, to_key, from, to, seq}]}
long ref_of(var &out, var &wire);                  // the ref kind code; out = the ref with `ref` word kept; refNONE = refused
void ref_key(var &out, var &ref);                  // the canonical key text
void symbol_ref_of_doc(var &out, long w, long adoc, var &node);   // {ref:"symbol", file: doc path, kind, name}
bool symbol_node_of(var &node, long w, long es, long adoc, long h, var &ref);   // graph.definition + kind filter
long record_create(long w, long es, long adoc, long self_id, long rkind, var &fields, const char *origin, const char *ext_key, long state);
long record_update(long w, long es, long adoc, long self_id, long id, var &fields, long state);   // state rsNONE = keep
long link_write(long w, long es, long adoc, long self_id, long op, var &from, long rel, var &to);
void nexus_record(var &out, long w, long id);
void nexus_records(var &out, long w, long rkind, long state);      // rsNONE = the live states
void nexus_context(var &out, long w, long es, long adoc, long h, var &ref);
void nexus_explain(var &out, long w, long es, long adoc, long h, var &ref, long detail);
void nexus_constraints(var &out, long w, long adoc, var &target);  // the decisions constraining a target node
```

- [ ] **Step 1: The file.**

```c
// madcide_nexus.inc — the INTENT axis of the code-graph MCP (Nexus L4d; design
// docs/plans/2026-09-13-nexus-L4-design.md §3.5, §3.7, §4.5; plan
// 2026-09-14-nexus-L4d-intent-plan.md). Records and links are EVENTS in the
// ONE stream (record: op create|update|close; link: op link|unlink); the fold
// materialises them, cached on the log entity by seq. A ref is a durable key
// (a symbol by file+kind+name, never a graph id). Included by madcide_mcp.inc
// after madcide_past.inc (it reads the graph verbs and graph_history) and
// before graph_edit_apply (nexus_constraints feeds a proposal's `constraints`).
//
// Which structure answers what (the index-is-not-the-graph law):
//   record / records — the fold (records, states derived from the ops)
//   context  — the fold's links touching the ref + the proposals whose target
//              is the symbol + the symbol's git rows (graph_history) + the node
//   explain  — ONE projection over the existing verbs: what (graph.node/span/
//              type_of), depends (callees/callers/impact), history
//              (graph_history), why / plan / constraints (the fold)
// Thread contract: the fold is a per-session cache on the changelog entity,
// invalidated by seq; every write is one clog_append on the session thread.

// ---- the fold --------------------------------------------------------------
// out = { records: (id-key -> record), ids: [id…], links: [link…] }. A record
// = the create event's fields + envelope {id, rkind, state, origin, ext_key,
// actor, created_seq, updated_seq, fields}; update merges `fields` and may set
// state; close sets state. A link row = {seq, from, rel, to, from_key, to_key,
// actor}; unlink removes the exact (from_key, rel, to_key) triple.
void nexus_fold_compute(var &out, long w)
{
    var recs;
    var ids = {};
    var links = {};
    long le = clog_find(w);
    if ( le > 0 )
    {
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
		break;
	    if ( clog_kind_of(rec) != ckOTHER )
		continue;
	    long nk = nexus_kind_of(rec);
	    if ( nk == nkRECORD )
	    {
		long op = record_op_of(rec["op"]);
		if ( op == roCREATE )
		{
		    var key = format("{}", rec["seq"]);
		    var st = rec["state"];
		    if ( record_state_of(st) == rsNONE )
			st = record_state_name(rsOPEN);
		    var r = { "id": rec["seq"], "rkind": rec["rkind"], "state": st, "origin": rec["origin"],
			      "ext_key": rec["ext_key"], "actor": rec["actor"], "es": rec["es"], "doc": rec["doc"],
			      "created_seq": rec["seq"], "updated_seq": rec["seq"], "fields": rec["fields"] };
		    recs[key.c_str()] = r;
		    ids.push(rec["seq"]);
		}
		else if ( op == roUPDATE || op == roCLOSE )
		{
		    var key = format("{}", rec["id"]);
		    var r;
		    keyed_get(r, recs, key.c_str());
		    if ( r.is_null() )
			continue;			// an op on an unknown record: never trusted
		    var nf = rec["fields"];
		    if ( !nf.is_null() )
		    {
			var f = r["fields"];
			for ( var kv : nf )		// merge: the update's keys win
			    f[kv.key().c_str()] = kv;
			r["fields"] = f;
		    }
		    var st = rec["state"];
		    if ( record_state_of(st) != rsNONE )
			r["state"] = st;
		    r["updated_seq"] = rec["seq"];
		    recs[key.c_str()] = r;
		}
	    }
	    else if ( nk == nkLINK )
	    {
		long op = link_op_of(rec["op"]);
		var fk;
		ref_key(fk, rec["from"]);
		var tk;
		ref_key(tk, rec["to"]);
		if ( op == loLINK )
		{
		    var row = { "seq": rec["seq"], "from": rec["from"], "rel": rec["rel"], "to": rec["to"],
				"from_key": fk, "to_key": tk, "actor": rec["actor"] };
		    links.push(row);
		}
		else if ( op == loUNLINK )
		{
		    var kept = {};
		    for ( var l : links )
			if ( !(l["from_key"] == fk && l["rel"] == rec["rel"] && l["to_key"] == tk) )
			    kept.push(l);
		    links = kept;
		}
	    }
	}
    }
    out = { "records": recs, "ids": ids, "links": links };
}
```

(The object-iteration idiom `for ( var kv : nf )` + `kv.key()` — CONFIRM against
a `tmp/` reducer first; if the dialect iterates an object's VALUES only, carry
the update's field NAMES as a parallel `keys` array in the event
(`record_update` writes `fields` + `field_keys`) and merge by that list. Record
which in the file's comment.)

```c
// The cached fold: recomputed when the stream's head moved.
void nexus_fold(var &out, long w)
{
    long le = clog_find(w);
    long head = clog_head(w, 0);
    if ( le > 0 )
    {
	long at = es_int(w, le, "nfold_seq", -1);
	if ( at == head )
	{
	    ui::get(out, w, le, "nfold");
	    if ( !out.is_null() )
		return;
	}
    }
    nexus_fold_compute(out, w);
    if ( le > 0 )
    {
	ui::set(w, le, "nfold", out);
	ui::set(w, le, "nfold_seq", head);
    }
}

// ---- refs ------------------------------------------------------------------
// The wire shape {ref: <word>, …}: converted ONCE here; refNONE = refused.
long ref_of(var &out, var &wire)
{
    if ( wire.is_null() || !wire.is_object() )
	return refNONE;
    long k = ref_kind_of(wire["ref"]);
    if ( k == refNONE )
	return refNONE;
    out = wire;
    return k;
}

// The ONE canonical key links are matched by.
void ref_key(var &out, var &ref)
{
    long k = ref_kind_of(ref["ref"]);
    switch ( k )
    {
    case refRECORD:   out = format("record:{}", ref["id"]); return;
    case refSYMBOL:   out = format("symbol:{}\t{}\t{}", ref["file"], ref["kind"], ref["name"]); return;
    case refCOMMIT:   out = format("commit:{}", ref["sha"]); return;
    case refPROPOSAL: out = format("proposal:{}", ref["seq"]); return;
    case refEXTERNAL: out = format("external:{}\t{}", ref["source"], ref["key"]); return;
    }
    out = "";
}

// A symbol ref for a node of the served document.
void symbol_ref_of_doc(var &out, long w, long adoc, var &node)
{
    var p;
    ui::get(p, w, adoc, "path");
    var word = ref_kind_name(refSYMBOL);
    out = { "ref": word, "file": p, "kind": node["kind"], "name": node["name"] };
}

// Resolve a symbol ref against the served document's live tree: graph.definition
// by name, filtered by kind (and the file must be the served one). false = no
// such node here.
bool symbol_node_of(var &node, long w, long es, long adoc, long h, var &ref)
{
    var p;
    ui::get(p, w, adoc, "path");
    if ( !(ref["file"] == p) )
	return false;
    var d;
    madc::graph_definition(d, h, ref["name"].c_str());
    var nodes = d["nodes"];
    if ( nodes.is_null() )
	return false;
    for ( var n : nodes )
	if ( n["kind"] == ref["kind"] )
	{
	    node = n;
	    return true;
	}
    return false;
}

// ---- writers ---------------------------------------------------------------
long record_create(long w, long es, long adoc, long self_id, long rkind, var &fields,
		   const char *origin0, const char *ext_key0, long state)
{
    var origin = origin0;
    var ext_key = ext_key0;
    var rec = { "kind": "record", "op": record_op_name(roCREATE), "es": es, "doc": adoc, "actor": self_id,
		"rkind": record_kind_name(rkind), "state": record_state_name(state == rsNONE ? rsOPEN : state),
		"origin": origin, "ext_key": ext_key, "fields": fields };
    return clog_append(w, adoc, rec);
}

long record_update(long w, long es, long adoc, long self_id, long id, var &fields, long state, long op)
{
    var rec = { "kind": "record", "op": record_op_name(op), "es": es, "doc": adoc, "actor": self_id,
		"id": id, "fields": fields };
    if ( state != rsNONE )
	rec["state"] = record_state_name(state);
    return clog_append(w, adoc, rec);
}

long link_write(long w, long es, long adoc, long self_id, long op, var &from, long rel, var &to)
{
    var rec = { "kind": "link", "op": link_op_name(op), "es": es, "doc": adoc, "actor": self_id,
		"from": from, "rel": rel_name(rel), "to": to };
    return clog_append(w, adoc, rec);
}
```

(`record_state_name(state == rsNONE ? rsOPEN : state)` — a same-type ternary
on longs inside a call is fine; the literal rule bans a ternary as a literal
VALUE only where its `:` collides — here it is an argument. If the parser
objects, hoist to a local.)

```c
// ---- the verbs -------------------------------------------------------------
void nexus_record(var &out, long w, long id)
{
    var f;
    nexus_fold(f, w);
    var key = format("{}", id);
    var r;
    keyed_get(r, f["records"], key.c_str());
    if ( r.is_null() )
    {
	var why = format("no record {}", id);
	out = { "error": why };
	return;
    }
    out = r;
}

// rkind rkNONE = every kind; state rsNONE = the LIVE states (open, active,
// done) — a superseded or rejected record answers only when asked for.
void nexus_records(var &out, long w, long rkind, long state)
{
    var f;
    nexus_fold(f, w);
    var rows = {};
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, f["records"], key.c_str());
	if ( rkind != rkNONE && record_kind_of(r["rkind"]) != rkind )
	    continue;
	long st = record_state_of(r["state"]);
	if ( state == rsNONE )
	{
	    if ( st == rsSUPERSEDED || st == rsREJECTED )
		continue;
	}
	else if ( st != state )
	    continue;
	rows.push(r);
    }
    out = { "rows": rows };
}

// The links touching a key, as context rows (provenance record), each with the
// record on the other end when there is one.
void context_link_rows(var &rows, var &f, var &key)
{
    var pv = provenance_name(pvRECORD);
    for ( var l : f["links"] )
    {
	bool from_me = l["from_key"] == key;
	bool to_me = l["to_key"] == key;
	if ( !from_me && !to_me )
	    continue;
	var other = from_me ? l["to"] : l["from"];
	var row = { "provenance": pv, "rel": l["rel"], "direction": from_me ? "out" : "in", "ref": other, "seq": l["seq"] };
	if ( ref_kind_of(other["ref"]) == refRECORD )
	{
	    var rkey = format("{}", other["id"]);
	    var r;
	    keyed_get(r, f["records"], rkey.c_str());
	    if ( !r.is_null() )
		row["record"] = r;
	}
	rows.push(row);
    }
}
```

(Two ternaries with `var` operands of the same kind — `from_me ? l["to"] :
l["from"]` — CONFIRM on the reducer; else two `if` assignments. `"direction":
from_me ? "out" : "in"` inside a literal collides with the key colon per the
literal rule: hoist `var dir = "in"; if ( from_me ) dir = "out";`.)

```c
// nexus.context(ref): the RAW list of everything linked to a ref, each row
// provenance-tagged — links + records (record), the proposals whose target
// is the symbol (record), the symbol's git rows (git) and its node (compiler).
void nexus_context(var &out, long w, long es, long adoc, long h, var &ref)
{
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var rows = {};
    context_link_rows(rows, f, key);
    long k = ref_kind_of(ref["ref"]);
    if ( k == refSYMBOL )
    {
	var node;
	if ( symbol_node_of(node, w, es, adoc, h, ref) )
	{
	    var cpv = provenance_name(pvCOMPILER);
	    var nrow = { "provenance": cpv, "node": node };
	    rows.push(nrow);
	    var hist;
	    graph_history(hist, w, es, adoc, h, node["id"].as_integer());
	    var hrows = hist["rows"];
	    if ( !hrows.is_null() )
		for ( var hr : hrows )
		    rows.push(hr);
	}
	// proposals that touched this symbol (the record axis)
	var seqs;
	var props;
	proposal_fold(seqs, props, w, adoc);
	var rpv = provenance_name(pvRECORD);
	for ( var sv : seqs )
	{
	    var pkey = format("{}", sv);
	    var p;
	    keyed_get(p, props, pkey.c_str());
	    for ( var op : p["ops"] )
	    {
		var tgt = op["target"];
		if ( tgt["kind"] == ref["kind"] && tgt["name"] == ref["name"] )
		{
		    var prow = { "provenance": rpv, "proposal": p["seq"], "status": p["status"], "verb": op["verb"] };
		    rows.push(prow);
		    break;
		}
	    }
	}
    }
    out = { "ref": ref, "key": key, "rows": rows };
}

// The decisions that constrain a node: records of kind decision linked by
// affects / documents to the node's symbol key (either direction). Informing,
// never blocking (design §3.4) — a proposal's `constraints`.
void nexus_constraints(var &out, long w, long adoc, var &target)
{
    out = {};
    var ref;
    symbol_ref_of_doc(ref, w, adoc, target);
    var key;
    ref_key(key, ref);
    var f;
    nexus_fold(f, w);
    for ( var l : f["links"] )
    {
	long rel = rel_of(l["rel"]);
	if ( rel != relAFFECTS && rel != relDOCUMENTS )
	    continue;
	var other;
	if ( l["to_key"] == key )
	    other = l["from"];
	else if ( l["from_key"] == key )
	    other = l["to"];
	else
	    continue;
	if ( ref_kind_of(other["ref"]) != refRECORD )
	    continue;
	var rkey = format("{}", other["id"]);
	var r;
	keyed_get(r, f["records"], rkey.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkDECISION )
	    continue;
	var flds = r["fields"];
	var row = { "id": r["id"], "state": r["state"], "title": flds["title"], "rel": l["rel"] };
	out.push(row);
    }
}

// nexus.explain(ref, detail): ONE compound projection over the existing verbs
// (design §4.5) — what / depends / history / why / plan / constraints.
void nexus_explain(var &out, long w, long es, long adoc, long h, var &ref, long detail)
{
    if ( ref_kind_of(ref["ref"]) != refSYMBOL )
    {
	out = { "error": "explain takes a symbol ref {ref:\"symbol\", file, kind, name} (v1)" };
	return;
    }
    var node;
    if ( !symbol_node_of(node, w, es, adoc, h, ref) )
    {
	var why = format("no {} named {} in the served document", ref["kind"], ref["name"]);
	out = { "error": why };
	return;
    }
    long id = node["id"].as_integer();
    var cpv = provenance_name(pvCOMPILER);
    var sp;
    madc::graph_span(sp, h, id);
    var what = { "provenance": cpv, "node": node, "span": sp["span"] };
    if ( detail == edFULL )
    {
	var ty;
	madc::graph_type_of(ty, h, id);
	what["type"] = ty;
    }
    var depends = { "provenance": cpv };
    if ( node["kind"] == "Function" )
    {
	var ce;
	madc::graph_callees(ce, h, id);
	var cr;
	madc::graph_callers(cr, h, id);
	var im;
	madc::graph_impact(im, h, id);
	depends["callees"] = php::count(ce["nodes"]);
	depends["callers"] = php::count(cr["nodes"]);
	depends["impact"] = im;
    }
    var hist;
    graph_history(hist, w, es, adoc, h, id);
    var hrows = hist["rows"];
    if ( detail != edFULL && !hrows.is_null() && php::count(hrows) > 10 )
    {
	var capped = {};
	for ( long i = 0; i < 10; i = i + 1 )
	    capped.push(hrows[i]);
	hrows = capped;
    }
    var history = { "rows": hrows, "complete": hist["complete"] };
    // the record axis: why (decisions + requirements + proposals), plan
    // (live tasks / milestones), constraints (affects / documents decisions)
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var lrows = {};
    context_link_rows(lrows, f, key);
    var why = {};
    var plan = {};
    for ( var lr : lrows )
    {
	var r = lr["record"];
	if ( r.is_null() )
	    continue;
	long rk = record_kind_of(r["rkind"]);
	long st = record_state_of(r["state"]);
	long rel = rel_of(lr["rel"]);
	if ( rk == rkDECISION || rk == rkREQUIREMENT )
	    why.push(lr);
	if ( (rk == rkTASK || rk == rkMILESTONE) && st != rsDONE && st != rsSUPERSEDED && st != rsREJECTED
	     && (rel == relPLANNED_FOR || rel == relREQUESTED_BY || rel == relIMPLEMENTS) )
	    plan.push(lr);
    }
    var ctx;
    nexus_context(ctx, w, es, adoc, h, ref);
    var rpv = provenance_name(pvRECORD);
    for ( var cr : ctx["rows"] )
	if ( cr["provenance"] == rpv && !cr["proposal"].is_null() )
	    why.push(cr);
    var constraints;
    nexus_constraints(constraints, w, adoc, node);
    out = { "ref": ref, "what": what, "depends": depends, "history": history, "why": why, "plan": plan,
	    "constraints": constraints };
}
```

(`"Function"` — the node kind word is the ENGINE's vocabulary; compare through
the graph's kind the way `graph.search`'s `kind` argument spells it. If
`madc::graph_kind_of` or an enum exists for node kinds, use it; else this one
compare stays with a comment naming the engine as the owner of the word.)

- [ ] **Step 2**: the `tmp/` reducers for the three "CONFIRM" idioms (object
  iteration with keys; a same-kind `var` ternary; ternary as an argument),
  recorded in the file's header comment.

---

### Task 3: `madcide_tests.inc` — `test.discover`

```c
// madcide_tests.inc — the EVIDENCE axis's records (Nexus L4d; design §3.10):
// test.discover folds the project's test families from the SAME conventions
// the canonical runner reads (tests/<name>.mad + its fixtures, skipping a
// .helper-owned unit; tests/unit/*.cpp) into `test` records — origin
// "convention", ext_key = the path, upserted like a sync. Rule #7: no per-test
// knowledge here. L4e adds test.list / candidates / run / results (the runner
// seat) to this file. Included by madcide_mcp.inc after madcide_nexus.inc.

// Upsert one test record: an existing (origin, ext_key) gets an update, else a
// create. Answers 1 for created, 0 for updated.
long test_record_upsert(long w, long es, long adoc, long self_id, const char *name0, long family,
			const char *asset0)
{
    var name = name0;
    var asset = asset0;
    var fam = test_family_name(family);
    var fields = { "name": name, "family": fam, "asset": asset, "command": "" };
    var f;
    nexus_fold(f, w);
    var origin = "convention";
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, f["records"], key.c_str());
	if ( r["origin"] == origin && r["ext_key"] == asset )
	{
	    record_update(w, es, adoc, self_id, idv.as_integer(), fields, rsNONE, roUPDATE);
	    return 0;
	}
    }
    record_create(w, es, adoc, self_id, rkTEST, fields, origin.c_str(), asset.c_str(), rsOPEN);
    return 1;
}

// test.discover(dir?): the .mad family (a .helper sibling = a unit owned by
// another test — skipped, the runner's rule) and the unit family.
void test_discover(var &out, long w, long es, long adoc, long self_id, const char *dir0)
{
    var dir = dir0;
    if ( strlen(dir.c_str()) == 0 )
	dir = "tests";
    long created = 0;
    long seen = 0;
    array mads;
    perl::glob(mads, format("{}/*.mad", dir));
    php::sort(mads);
    for ( var p : mads )
    {
	var base;
	path_sans_ext(base, p);
	var helper = format("{}.helper", base);
	if ( php::file_exists(helper) )
	    continue;
	var name = php::str_replace(format("{}/", dir).c_str(), "", base.c_str());
	seen = seen + 1;
	created = created + test_record_upsert(w, es, adoc, self_id, name.c_str(), tfMAD, p.c_str());
    }
    array units;
    perl::glob(units, format("{}/unit/*.cpp", dir));
    php::sort(units);
    for ( var p : units )
    {
	var base;
	path_sans_ext(base, p);
	var name = php::str_replace(format("{}/unit/", dir).c_str(), "", base.c_str());
	seen = seen + 1;
	created = created + test_record_upsert(w, es, adoc, self_id, name.c_str(), tfUNIT, p.c_str());
    }
    long updated = seen - created;
    out = { "ok": true, "dir": dir, "discovered": seen, "created": created, "updated": updated };
}
```

(`php::str_replace(a, b, c)` returns ring text — own it in a var at once as
shown; `format(...).c_str()` as an argument — confirm the ring survives two
`c_str()`s in one call; else hoist each into a var first.)

---

### Task 4: `madcide_mcpclient.inc` — manifests, the client, `nexus.sources`, `nexus.sync`

**Interfaces (produces):**
```c
long sources_entity(long w);                                   // the "nexus-sources" entity (created on first need)
bool manifests_load(var &out_err, long w, long es, long doc, const char *dir);   // <dir>/*.mcp.json -> the table on the entity
void nexus_sources(var &out, long w);                          // {rows: [{name, transport, tools: […]}]}
bool mcp_client_call(var &out, madc::channel &c, long &next_id, const char *method, var &params, long deadline_ms);
void nexus_sync(var &out, long w, long es, long adoc, long self_id, const char *source);
```

```c
// madcide_mcpclient.inc — madc as an MCP CLIENT (Nexus L4d; design §4.5, §5
// flow 4, §10 (3)): an external system's MCP server is a child over
// madc::channel exec:// (stdio JSON-RPC, one message per line); its manifest
// is DATA (<profiles>/mcp/<source>.mcp.json) loaded ONCE into an enum-indexed
// vocabulary table; nexus.sync pulls its rows into `record` events with
// origin = the source (the cached projection of design §3.5 ruling 2). ONE
// child per sync (a channel is not a value; a sync is rare). Transport: stdio
// only (mtSTDIO); http:// waits on a madcdis channel — a stdio<->HTTP bridge
// process (mcp-remote) is the owner's configuration meanwhile.
//
// Manifest shape: { "name": "mycenode", "transport": "stdio",
//   "command": ["{madc}", "--no-config", "path/to/server.mad"],   // joined by single spaces; {madc}/{project} substitute (build_subst)
//   "env": { "TOKEN": "…" },                                       // rendered as an `env K=V …` prefix (posix); refused on win32
//   "vocabulary": { "list_tasks": { "tool": "list_tasks", "args": {}, "map": { "summary": "title", "status": "state" } },
//                   "get_task": …, "create_task": …, "update_task": …, "comment": … } }
// An unknown vocabulary key, a missing name/command, or (at sync) a tool the
// server does not advertise REFUSES with the file and the key — the enum-law
// boundary. Thread contract: manifests are immutable after load; a sync's
// child lives inside the one verb call (cooperative reads; a deadline through
// poll_state + sleep_ms, never a blocking thread).

long sources_entity(long w)
{
    long se = ui::entity_by_name(w, "nexus-sources");
    if ( se > 0 )
	return se;
    se = ui::create(w, "nexus-sources");
    var none = {};
    ui::set(w, se, "sources", none);
    return se;
}

// Load every <dir>/*.mcp.json into the table {name -> source}; false with
// out_err = "<file>: <why>" on the first refusal (nothing replaced).
bool manifests_load(var &out_err, long w, long es, long doc, const char *dir0)
{
    var dir = dir0;
    array files;
    perl::glob(files, format("{}/*.mcp.json", dir));
    php::sort(files);
    var table;			// name -> source (a null var made an object on the first keyed write)
    var names = {};
    for ( var fpath : files )
    {
	var text;
	if ( !php::file_get_contents(text, fpath.c_str()) )
	{
	    out_err = format("{}: unreadable", fpath);
	    return false;
	}
	var m;
	if ( !js::parse(m, text) )
	{
	    out_err = format("{}: not valid JSON", fpath);
	    return false;
	}
	var name = m["name"];
	if ( name.is_null() || strlen(name.c_str()) == 0 )
	{
	    out_err = format("{}: no name", fpath);
	    return false;
	}
	long tr = mcp_transport_of(m["transport"]);
	if ( tr == mtNONE )
	    tr = mtSTDIO;
	if ( tr != mtSTDIO )
	{
	    out_err = format("{}: transport '{}' is not served yet (stdio only; bridge with mcp-remote)", fpath, m["transport"]);
	    return false;
	}
	var cmd = m["command"];
	if ( cmd.is_null() || php::count(cmd) == 0 )
	{
	    out_err = format("{}: no command", fpath);
	    return false;
	}
	var joined = "";
	for ( var word : cmd )
	    joined = strlen(joined.c_str()) == 0 ? word : format("{} {}", joined, word);
	var env = m["env"];
	var prefix = "";
	if ( !env.is_null() )
	{
	    if ( madc::sys.os == "win32" )   // CONFIRM the spelling of the os probe (madc::sys / <bits/...>)
	    {
		out_err = format("{}: env: not supported on this platform", fpath);
		return false;
	    }
	    prefix = "env";
	    for ( var kv : env )
		prefix = format("{} {}={}", prefix, kv.key(), kv);
	}
	var full = strlen(prefix.c_str()) == 0 ? joined : format("{} {}", prefix, joined);
	var uri0;
	build_subst(uri0, full.c_str(), w, es, doc);	// {madc} / {project} — the ONE owner
	var uri = format("exec://{}", uri0);
	// the vocabulary: enum-indexed, every key known
	var vocab = m["vocabulary"];
	var ops;			// op-code-key -> {tool, args, map}
	if ( !vocab.is_null() )
	    for ( var entry : vocab )
	    {
		long op = nexus_op_of(entry.key());
		if ( op == noNONE )
		{
		    out_err = format("{}: unknown vocabulary key '{}'", fpath, entry.key());
		    return false;
		}
		var tool = entry["tool"];
		if ( tool.is_null() )
		{
		    out_err = format("{}: vocabulary '{}' names no tool", fpath, entry.key());
		    return false;
		}
		var okey = format("{}", op);
		ops[okey.c_str()] = entry;
	    }
	var src = { "name": name, "file": fpath, "transport": mcp_transport_name(tr), "uri": uri, "ops": ops };
	table[name.c_str()] = src;
	names.push(name);
    }
    long se = sources_entity(w);
    ui::set(w, se, "sources", table);
    ui::set(w, se, "names", names);
    return true;
}
```

(The object iteration with `.key()` is the Task 2 CONFIRM; the `madc::sys.os`
probe: find the engine's platform word — `grep -n 'os\b\|platform' include/madc/ns_madc`
around the `sys` object — and use it; if there is none, skip the win32 refusal
and NOTE it: the `env` prefix then fails loudly at spawn on Windows.)

```c
// nexus.sources: the loaded manifests (names, transports, the tools each maps).
void nexus_sources(var &out, long w)
{
    long se = sources_entity(w);
    var names;
    ui::get(names, w, se, "names");
    var table;
    ui::get(table, w, se, "sources");
    var rows = {};
    if ( !names.is_null() )
	for ( var nm : names )
	{
	    var s;
	    keyed_get(s, table, nm.c_str());
	    var tools = {};
	    var ops = s["ops"];
	    if ( !ops.is_null() )
		for ( long op = noLIST_TASKS; op <= noCOMMENT; op = op + 1 )
		{
		    var okey = format("{}", op);
		    var e;
		    keyed_get(e, ops, okey.c_str());
		    if ( !e.is_null() )
		    {
			var trow = { "op": nexus_op_name(op), "tool": e["tool"] };
			tools.push(trow);
		    }
		}
	    var row = { "name": s["name"], "transport": s["transport"], "file": s["file"], "tools": tools };
	    rows.push(row);
	}
    out = { "rows": rows };
}

// ---- the client ------------------------------------------------------------
// One JSON-RPC request over the child's stdio; the reply matched by id (a
// server notification — no id — is skipped); a dead child (EOF / error) or a
// silent one past `deadline_ms` answers {error, exit_status}. Cooperative: the
// wait is poll_state + sleep_ms, never a blocking read on a silent pipe.
bool mcp_client_call(var &out, madc::channel &c, long &next_id, const char *method, var &params, long deadline_ms)
{
    long id = next_id;
    next_id = next_id + 1;
    var req = { "jsonrpc": "2.0", "id": id, "method": method, "params": params };
    var js = js::stringify(req);
    var line = format("{}\n", js);
    if ( !c.write(line.c_str()) )
    {
	out = { "error": c.last_error(), "exit_status": c.exit_status() };
	return false;
    }
    long waited = 0;
    while ( true )
    {
	long st = c.poll_state();
	if ( st < 0 )
	{
	    out = { "error": "the server closed its pipe", "exit_status": c.exit_status() };
	    return false;
	}
	if ( st == 0 )
	{
	    if ( waited >= deadline_ms )
	    {
		var why = format("no reply to {} within {} ms", method, deadline_ms);
		out = { "error": why, "exit_status": -1 };
		return false;
	    }
	    madc::sleep_ms(10);
	    waited = waited + 10;
	    continue;
	}
	var r;
	if ( !c.readline(r) )
	{
	    out = { "error": "the server exited", "exit_status": c.exit_status() };
	    return false;
	}
	var resp;
	if ( !js::parse(resp, r) )
	    continue;			// not JSON: a stray line on stdout — skipped, never trusted
	var rid = resp["id"];
	if ( rid.is_null() || rid.as_integer() != id )
	    continue;			// a notification or another id
	var err = resp["error"];
	if ( !err.is_null() )
	{
	    out = { "error": err["message"], "code": err["code"] };
	    return false;
	}
	out = resp["result"];
	return true;
    }
    return false;
}

// A notification (no id, no reply).
void mcp_client_notify(madc::channel &c, const char *method)
{
    var params = {};
    var req = { "jsonrpc": "2.0", "method": method, "params": params };
    var js = js::stringify(req);
    var line = format("{}\n", js);
    c.write(line.c_str());
}

// A tool result's ROWS: the first text content parsed as JSON — an array, or
// an object with `rows`. Empty on anything else.
void tool_result_rows(var &rows, var &result)
{
    rows = {};
    var content = result["content"];
    if ( content.is_null() || php::count(content) == 0 )
	return;
    var item0 = content[0];
    var txt = item0["text"];
    if ( txt.is_null() )
	return;
    var parsed;
    if ( !js::parse(parsed, txt.c_str()) )
	return;
    if ( parsed.is_object() )
    {
	var r = parsed["rows"];
	if ( !r.is_null() )
	    rows = r;
	return;
    }
    rows = parsed;
}

// nexus.sync(source): spawn the manifest's server, handshake, validate the
// vocabulary against tools/list, pull list_tasks, fold each row into a
// `record` event with origin = source (upsert by (origin, ext_key)); close.
void nexus_sync(var &out, long w, long es, long adoc, long self_id, const char *source0)
{
    var source = source0;
    long se = sources_entity(w);
    var table;
    ui::get(table, w, se, "sources");
    var s;
    keyed_get(s, table, source.c_str());
    if ( s.is_null() )
    {
	var why = format("no source '{}' (nexus.sources lists the loaded manifests)", source);
	out = { "error": why };
	return;
    }
    var uri = s["uri"];
    madc::channel c(uri.c_str());
    if ( !c.ok() )
    {
	var why = format("{}: cannot spawn: {}", source, c.last_error());
	out = { "error": why };
	return;
    }
    long next_id = 1;
    var caps = {};
    var info = { "name": "madcide", "version": "1.0.0" };
    var iparams = { "protocolVersion": "2024-11-05", "capabilities": caps, "clientInfo": info };
    var ires;
    if ( !mcp_client_call(ires, c, next_id, "initialize", iparams, 5000) )
    {
	c.close();
	out = { "error": ires["error"], "exit_status": ires["exit_status"], "source": source };
	return;
    }
    mcp_client_notify(c, "notifications/initialized");
    var lparams = {};
    var lres;
    if ( !mcp_client_call(lres, c, next_id, "tools/list", lparams, 5000) )
    {
	c.close();
	out = { "error": lres["error"], "exit_status": lres["exit_status"], "source": source };
	return;
    }
    var advertised;			// tool name -> 1
    for ( var t : lres["tools"] )
	advertised[t["name"].c_str()] = 1;
    var ops = s["ops"];
    for ( long op = noLIST_TASKS; op <= noCOMMENT; op = op + 1 )
    {
	var okey = format("{}", op);
	var e;
	keyed_get(e, ops, okey.c_str());
	if ( e.is_null() )
	    continue;
	var hit;
	keyed_get(hit, advertised, e["tool"].c_str());
	if ( hit.is_null() )
	{
	    c.close();
	    var why = format("{}: tool '{}' (vocabulary {}) is not advertised by the server", source, e["tool"], nexus_op_name(op));
	    out = { "error": why };
	    return;
	}
    }
    var lkey = format("{}", noLIST_TASKS);
    var lt;
    keyed_get(lt, ops, lkey.c_str());
    if ( lt.is_null() )
    {
	c.close();
	var why = format("{}: the manifest maps no list_tasks", source);
	out = { "error": why };
	return;
    }
    var args = lt["args"];
    if ( args.is_null() )
	args = {};
    var cparams = { "name": lt["tool"], "arguments": args };
    var cres;
    if ( !mcp_client_call(cres, c, next_id, "tools/call", cparams, 10000) )
    {
	c.close();
	out = { "error": cres["error"], "exit_status": cres["exit_status"], "source": source };
	return;
    }
    c.close_write();
    c.close();
    var rows;
    tool_result_rows(rows, cres);
    var map = lt["map"];
    long created = 0;
    long updated = 0;
    var f;
    nexus_fold(f, w);
    for ( var row : rows )
    {
	// map the result's fields to record fields (a flat rename table)
	var fields;
	var state = rsOPEN;
	var ext_key = "";
	for ( var kv : row )
	{
	    var to = kv.key();
	    if ( !map.is_null() )
	    {
		var m;
		keyed_get(m, map, kv.key().c_str());
		if ( !m.is_null() )
		    to = m;
	    }
	    if ( to == "key" )
		ext_key = kv;
	    else if ( to == "state" )
	    {
		long st = record_state_of(kv);
		if ( st != rsNONE )
		    state = st;
	    }
	    else
		fields[to.c_str()] = kv;
	}
	if ( strlen(ext_key.c_str()) == 0 )
	    continue;			// a row with no key cannot be upserted: skipped, counted
	long have = 0;
	for ( var idv : f["ids"] )
	{
	    var rkey = format("{}", idv);
	    var r;
	    keyed_get(r, f["records"], rkey.c_str());
	    if ( r["origin"] == source && r["ext_key"] == ext_key )
	    {
		have = idv.as_integer();
		break;
	    }
	}
	if ( have > 0 )
	{
	    record_update(w, es, adoc, self_id, have, fields, state, roUPDATE);
	    updated = updated + 1;
	}
	else
	{
	    record_create(w, es, adoc, self_id, rkTASK, fields, source.c_str(), ext_key.c_str(), state);
	    created = created + 1;
	}
    }
    out = { "ok": true, "source": source, "rows": php::count(rows), "created": created, "updated": updated };
}
```

(`while ( true )` — confirm the dialect accepts it; else `bool going = true;
while ( going )`. `for ( var t : lres["tools"] )` iterates a subscript — fine
(L4c fact). `c.write(line.c_str())` returns bool per the channel header.)

---

### Task 5: Seat wiring (`madcide_mcp.inc`) — descriptors, dispatch, `constraints`, the flush

- [ ] **Step 1: Includes.** After `#include "madcide_past.inc"`:
  `#include "madcide_nexus.inc"` (before `graph_edit_apply` — it feeds
  `constraints`), then `#include "madcide_tests.inc"` and
  `#include "madcide_mcpclient.inc"` (they use `record_create` from nexus and
  `build_subst`; place both right after nexus). Hmm — `nexus_context` calls
  `proposal_fold` (madcide_propose.inc, included AFTER graph_edit_apply). Move
  `#include "madcide_propose.inc"` up to right after `madcide_past.inc`?
  `graph_accept` calls `graph_edit_apply` (defined later) — the dialect
  resolves against EARLIER definitions, so propose.inc must stay after
  graph_edit_apply. Therefore: split `proposal_fold` + `proposal_of` out of
  madcide_propose.inc into the top of madcide_nexus.inc? No — one owner, one
  home: keep the fold in madcide_propose.inc and include madcide_nexus.inc
  AFTER madcide_propose.inc (i.e. after graph_edit_apply), and have
  `graph_edit_apply` reach `nexus_constraints` through a FORWARD DECLARATION
  at the top of madcide_mcp.inc: `void nexus_constraints(var &out, long w, long adoc, var &target);`
  (a declaration counts as "declared earlier" — confirmed by the L1 note
  "defined/declared earlier in the same file"). Include order then:
  past → (graph_edit_apply…) → propose → nexus → tests → mcpclient → graph_call → mcp_handle.
- [ ] **Step 2: `constraints`.** In `graph_edit_apply`'s `emPROPOSE` branch:
  `var constraints; nexus_constraints(constraints, w, adoc, ops[0]["target"]);`
  (the first op's target; an N-op proposal lists every op's — loop and
  concatenate) replacing `var constraints = {};`.
- [ ] **Step 3: Descriptors.** `nexus_tool_descriptors(out)` and
  `test_tool_descriptors(out)` beside `graph_tool_descriptors`, the same
  shape (`{name, code, description, inputSchema}`); `nexus_verb_of(name)` /
  `test_verb_of(name)` over them; `mcp_tools` appends both families (wire
  fields only). Schemas: `ref` = `{type: object, description: "{ref: record|symbol|commit|proposal|external, …}"}`;
  `nexus.records {rkind?, state?}`, `nexus.record {id}`, `nexus.create {rkind,
  fields, state?}`, `nexus.update {id, fields?, state?}`, `nexus.close {id,
  state?}`, `nexus.link {from, rel, to}`, `nexus.unlink {from, rel, to}`,
  `nexus.context {ref}`, `nexus.sources {dir?}`, `nexus.sync {source}`,
  `nexus.explain {ref, detail?}`, `test.discover {dir?}`.
- [ ] **Step 4: `nexus_call` / `test_call`** (mirroring `graph_call`: verb code,
  tier gate with the same refusal prose, the switch, isError on `error`):

```c
    case nvRECORDS:
    {
	var kv; arg_of(kv, args, "rkind"); long rk = rkNONE;
	if ( !kv.is_null() && strlen(kv.c_str()) > 0 ) { rk = record_kind_of(kv); if ( rk == rkNONE ) { refuse "unknown record kind"; } }
	var sv; arg_of(sv, args, "state"); long st = rsNONE;
	if ( !sv.is_null() && strlen(sv.c_str()) > 0 ) { st = record_state_of(sv); if ( st == rsNONE ) { refuse "unknown record state"; } }
	nexus_records(snap, w, rk, st); break;
    }
    case nvCREATE: rkind required (refuse unknown); fields = arg_of "fields" (null -> {}); state optional;
		   long id = record_create(...); snap = { "ok": true, "id": id }; broadcast_events(w, adoc, id - 1, self_id); break;
    case nvUPDATE / nvCLOSE: id required; the record must exist (nexus_record -> error); state optional (close default rsDONE);
		   record_update(..., op roUPDATE|roCLOSE); snap = { "ok": true, "id": id, "seq": seq }; broadcast; break;
    case nvLINK / nvUNLINK: from/to through ref_of (refNONE -> refuse "ref needs {ref: record|symbol|commit|proposal|external, …}");
		   rel through rel_of (relNONE -> refuse "unknown relation 'x'"); link_write; snap = { "ok": true, "seq": seq }; broadcast; break;
    case nvCONTEXT: ref_of or refuse; nexus_context(snap, w, es, adoc, h, ref); break;
    case nvSOURCES: { var dv; arg_of(dv, args, "dir");
		      if ( !dv.is_null() && strlen(dv.c_str()) > 0 ) { if ( my_tier < tierOWNER ) refuse "nexus.sources(dir) requires owner";
			  var err; if ( !manifests_load(err, w, es, doc, dv.c_str()) ) { snap = { "error": err }; break; } }
		      else if ( sources_entity has no "names" yet ) { var pd; resolve_profile_dir(pd); var mdir = format("{}/mcp", pd); var err; manifests_load(err, w, es, doc, mdir.c_str()); /* a missing dir = zero sources, no error */ }
		      nexus_sources(snap, w); break; }
    case nvSYNC: source required; nexus_sync(snap, w, es, adoc, self_id, src.c_str()); if ok -> broadcast_events(w, adoc, head_before, self_id); break;
    case nvEXPLAIN: ref_of or refuse; detail = explain_detail_of(arg "detail") (null -> edSUMMARY; unknown -> refuse); nexus_explain(...); break;
```

  `test_call`: `case tvDISCOVER: test_discover(snap, w, es, adoc, self_id, dir); broadcast; break;`.
  In `mcp_handle`'s `tools/call`: after the `graph.` prefix test, `nexus.` →
  `nexus_call`, `test.` → `test_call`.
- [ ] **Step 5: The flush.** `run_mcp`: `fflush(stdout);` after each
  `println("{}", js);` (both sites), with the comment "a piped reply must
  leave libc's buffer now — the client is waiting on it".
- [ ] **Step 6: Gates + neighbours**: `check-madcide-enums`, `check-madcide-single-owners`,
  `check-dialect-literals`, `check-dialect-lean` GREEN; `testmadcide_serve_edit`,
  `testmadcide_serve_propose`, `testgraphpast`, `testmadcide_serve_mcp` unchanged.
- [ ] **Step 7: Commit** (dialect + script) —
  `feat(graph-mcp): nexus L4d — records + links in the ONE stream (nexus_fold), refs by durable key, the nexus.* family with explain as one compound verb, proposals gain constraints, the MCP client over exec:// with manifests as data (nexus.sources / nexus.sync), test.discover; the --mcp server flushes every reply`.

---

### Task 6: The fixture server + `tests/testmcpclient.mad`

- [ ] **Step 1: `tests/mcp_fixture_server.mad`** (+ `tests/mcp_fixture_server.helper`
  = `the MCP fixture server testmcpclient.mad spawns — not a standalone test`):

```c
// The external system's stand-in (Nexus L4d): a tiny MCP server over stdio —
// initialize, tools/list (five tools), tools/call with canned rows. Spawned by
// tests/testmcpclient.mad through the manifest it writes; never run alone.
void reply(var &id, var &result)
{
    var out = { "jsonrpc": "2.0", "id": id, "result": result };
    var js = js::stringify(out);
    println("{}", js);
    fflush(stdout);			// the client waits on this line
}

void text_result(var &out, var &payload)
{
    var js = js::stringify(payload);
    var txt = js;
    var item = { "type": "text", "text": txt };
    var content = {};
    content[] = item;
    out = { "content": content, "isError": false };
}

int main()
{
    var line;
    while ( madc::getline(line) )
    {
	var req;
	if ( !js::parse(req, line) )
	    continue;
	var id = req["id"];
	if ( id.is_null() )
	    continue;			// a notification
	var method = req["method"];
	if ( method == "initialize" )
	{
	    var caps = { "tools": { "listChanged": false } };   // nested literal not allowed: pre-build
	    …
	    var info = { "name": "fixture", "version": "0" };
	    var result = { "protocolVersion": "2024-11-05", "capabilities": caps, "serverInfo": info };
	    reply(id, result);
	}
	else if ( method == "tools/list" )
	{
	    var tools = {};
	    // five tools, one input schema shape {type: object}
	    … push { "name": "list_tasks", "description": "…", "inputSchema": noargs } ×5 (list_tasks get_task create_task update_task comment)
	    var result = { "tools": tools };
	    reply(id, result);
	}
	else if ( method == "tools/call" )
	{
	    var params = req["params"];
	    var name = params["name"];
	    var result;
	    if ( name == "list_tasks" )
	    {
		var rows = {};
		rows[] = { "key": "MC-1", "summary": "wire the fixture", "status": "open" };
		rows[] = { "key": "MC-2", "summary": "map the vocabulary", "status": "open" };
		rows[] = { "key": "MC-3", "summary": "fold the rows", "status": "done" };
		rows[] = { "key": "MC-4", "summary": "refuse a bad key", "status": "open" };
		rows[] = { "key": "MC-5", "summary": "second sync updates", "status": "active" };
		text_result(result, rows);
	    }
	    else if ( name == "get_task" ) { var row = { "key": "MC-1", "summary": "wire the fixture", "status": "open" }; text_result(result, row); }
	    else if ( name == "create_task" ) { var row = { "key": "MC-6" }; text_result(result, row); }
	    else if ( name == "update_task" || name == "comment" ) { var row = { "ok": true }; text_result(result, row); }
	    else { var item = { "type": "text", "text": "unknown tool" }; var content = {}; content[] = item; result = { "content": content, "isError": true }; }
	    reply(id, result);
	}
	else
	{
	    var err = { "code": -32601, "message": "Method not found" };
	    var out = { "jsonrpc": "2.0", "id": id, "error": err };
	    var js = js::stringify(out);
	    println("{}", js);
	    fflush(stdout);
	}
    }
    return 0;
}
```

- [ ] **Step 2: `tests/testmcpclient.mad`** (in-process `mcp_handle`, the
  `call_tool` helper of `testgraphpast.mad`; the served file
  `madc_ide_mcpclient.mad` = two functions; a scratch manifest dir
  `madc_ide_mcp/` written by the test and removed at the end):

  - writes `madc_ide_mcp/fixture.mcp.json` with `command: ["{madc}", "--no-config", "tests/mcp_fixture_server.mad"]`
    and the five vocabulary entries, `map: {"summary": "title", "status": "state"}`;
  - `nexus.sources {dir: "madc_ide_mcp"}` (self_id -1 = owner) → `sources: rows=1 name=fixture tools=5`;
  - `nexus.sync {source: "fixture"}` → `sync: ok=1 rows=5 created=5 updated=0`;
  - `nexus.records {rkind: "task"}` → `records: 4` (MC-3 is done → live; ALL five are live states: open/active/done → 5; pin the true count: 5);
    `nexus.record {id: <first>}` → `record: origin=fixture ext_key=MC-1 title=wire the fixture state=open`;
  - a second `nexus.sync` → `sync2: created=0 updated=5`; `records` still 5;
  - `nexus.sync {source: "nosuch"}` → isError `no source 'nosuch'`;
  - a manifest `madc_ide_mcp_bad/bad.mcp.json` with vocabulary key `list_tickets` → `nexus.sources {dir}` isError containing `unknown vocabulary key 'list_tickets'`;
  - a manifest `madc_ide_mcp_dead/dead.mcp.json` whose command is `["{madc}", "--no-config", "madc_ide_mcp_dead/exit.mad"]` where exit.mad is `int main() { return 3; }` → `nexus.sources` ok then `nexus.sync {source: "dead"}` isError with `exit_status=3` (pin what the engine reports; 3 expected);
  - `test.discover {dir: "madc_ide_tdisc"}` over a scratch dir holding `a.mad`, `b.mad`, `b.helper`, `unit/u.cpp` → `discover: discovered=2 created=2` (a.mad + unit/u.cpp; b skipped); a second discover → `created=0 updated=2`; `nexus.records {rkind: "test"}` → 2 rows, `fields.family` mad / unit;
  - cleanup of every scratch file and dir (`php::unlink`, `rmdir`).

  `.expect` lines pinned from the run; `.expect_quiet` present (the child's
  stderr is inherited — the fixture must stay silent).

- [ ] **Step 3: Commit** — `test(graph-mcp): nexus L4d — the MCP fixture server and tests/testmcpclient (sources, sync upsert, a dead server, a misspelled manifest, test.discover)`.

---

### Task 7: `tests/testnexus_records.mad`

The served file (`madc_ide_nexus.mad`): `long add(long a, long b) { return a + b; }\nlong run(long n) { return add(n, 1); }\n`.

- `nexus.create {rkind: "decision", fields: {title: "add stays pure", reason: "no I/O in arithmetic"}}` → `create-decision: ok=1 id=1`;
- `nexus.create {rkind: "task", fields: {title: "speed up add"}}` → id 2; `nexus.create {rkind: "milestone", fields: {title: "v1.1"}}` → id 3; `nexus.create {rkind: "requirement", fields: {title: "add must not overflow"}}` → id 4;
- `nexus.link {from: {ref: "record", id: 1}, rel: "affects", to: {ref: "symbol", file: "madc_ide_nexus.mad", kind: "Function", name: "add"}}` → ok; `nexus.link` task 2 `implements` symbol add; task 2 `planned_for` milestone 3; requirement 4 `documents` symbol add;
- `nexus.records {}` → 4; `nexus.records {rkind: "task"}` → 1; `nexus.update {id: 2, fields: {owner: "me"}, state: "active"}` → ok; `nexus.record {id: 2}` → `state=active owner=me updated_seq>created_seq`;
- `nexus.context {ref: symbol add}` → rows: record rows 3 (affects in, implements in, documents in), compiler 1, git rows 0 (an untracked scratch file — `git=0` shape), proposals 0 → print counts by provenance;
- a proposal: `graph.replace {id: <add>, src: "long add(long a, long b) { return a - b; }", propose: true}` → `constraints=1 title=add stays pure`;
- `nexus.context` again → proposal rows 1;
- `nexus.explain {ref: symbol add}` → `what.node.name=add`, `depends.callers=1`, `why=3` (decision + requirement + the proposal), `plan=1` (the task via implements), `constraints=1`; `{detail: "full"}` adds `what.type` non-null;
- `nexus.close {id: 4, state: "rejected"}` → ok; `nexus.records {}` → 3 (rejected hidden); `nexus.records {state: "rejected"}` → 1;
- `nexus.unlink` task 2 `implements` add → `nexus.explain` plan=0;
- refusals: `nexus.link` with rel `depends_on` → isError `unknown relation 'depends_on'`; `nexus.link` with `from: {ref: "thing"}` → isError `ref needs`; `nexus.records {rkind: "bug"}` → isError; `nexus.create {rkind: "task"}` by a `self_id` that is an observer? (in-process self_id -1 = owner — the tier refusal for `nexus.create` at observer is covered by the connection harness precedent; assert `nexus_min_tier(nvCREATE) == tierPROPOSER` and `nexus_min_tier(nvSYNC) == tierEDITOR` as data);
- persist → restore: `ui::set(w, S.es, "projfile", "madc_ide_nexus.prj.json"); clog_persist(w, S.es, doc);` then a fresh `IdeSession S2; S2.open(path)`, `ui::set(w2, S2.es, "projfile", …); clog_restore(w2, S2.es, doc2);` → `nexus.records {}` through `mcp_handle(S2, …)` → 3; `nexus.record {id: 1}` title intact; remove the events file.

- [ ] Commit — `test(graph-mcp): nexus L4d — records, links, context, explain, constraints on a proposal, close/rejected hidden, unlink, refusals, persist -> restore (tests/testnexus_records)`.

---

### Task 8: Docs, hand-off, push

- [ ] design doc §9: L4d SHIPPED row with contents; §4.5: decisions 5–8 (one
  child per sync; env prefix; manifest dir + `sources(dir)` owner reload;
  rows = the first text content); §3.5: `id` derived by the fold.
- [ ] `claude_status.json` live_handoff UPDATE 15 (L4d shipped; NEXT =
  writing-plans L4e: `asset_layers_of` + `*_min_layer` gates, `--report=json`
  on the runner, `test.list / candidates / run / results`, `testrun` events
  tagged by node, proposal `checks`, the `target` slot); the L4 ledger; memory;
  KG Decisions `nexus_records_in_stream_fold`, `nexus_mcp_client_one_child_per_sync`.
- [ ] `git push origin feature/client-server-views-claude`.

---

## Self-review

- **Spec coverage**: §3.5 records/links/refs/relations/envelope/default
  live-state reads/the fold → Tasks 1–2; §3.7 provenance on context/explain
  rows (+ history's words behind the converter) → Tasks 1–2; §3.10 `test`
  record kind, `tests` relation, discovery by convention → Tasks 1, 3; §4.5
  the `nexus.*` family, `explain` as one verb with sections, the client, the
  manifests, the adapter, mycenode first (the manifest slot = `<profiles>/mcp/`),
  the fixture server → Tasks 2, 4–6; §5 flow 4 → Task 4; §6 refusals (unknown
  vocabulary key / tool not advertised / server exits / bad ref / bad relation)
  → Tasks 4–7; §8 L4d tests → Tasks 6–7; §10 (3) client in dialect over
  exec://, (6) fold in dialect, (8) milestone kind, (9) explain as one verb →
  Tasks 1–2, 4. Left for L4e by design: `checks`, runs, the runner seat, the
  layer gates. Deviations named: `id` fold-derived (1), env prefix (6), the
  manifest refusal names file + key (7), one child per sync (5).
- **Placeholders**: the CONFIRM notes (object iteration with keys; a
  same-kind var ternary; ternary as an argument; `while (true)`; the platform
  word; two `c_str()`s in one call) are reducer steps with their fallback
  named; the pinned counts (`records: 5`, `exit_status=3`) are verify-on-run.
- **Type consistency**: `ref_of(out, wire) -> long`, `ref_key(out, ref)`,
  `symbol_node_of(node, w, es, adoc, h, ref) -> bool` used by context /
  explain / constraints alike; `record_create(w, es, adoc, self_id, rkind,
  fields, origin, ext_key, state) -> seq` and `record_update(w, es, adoc,
  self_id, id, fields, state, op) -> seq` used by the verbs, the sync and the
  discoverer; `nexus_fold(out, w)` shape `{records, ids, links}` read by every
  consumer; `mcp_client_call(out, c, next_id, method, params, deadline) -> bool`.
