# Nexus L4e — asset LAYERS as one capability bitset with per-family layer gates, `--report=json` on the canonical runner, the `test.*` runner seat (`list / candidates / run / results`), node-tagged `testrun` / `buildrun` events, proposal `checks`, the multi-master LOCAL shape (`target`, offers) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Nexus its EVIDENCE axis and make every axis an optional
LAYER per asset: ONE function answers which layers an asset has on a node
(text / versioned / managed / lexable / parseable / executable / testable),
every verb family declares the layers it needs as data beside its tier and
refuses by name when one is missing; the canonical test runner gains a
machine-readable report and the Nexus becomes its CLIENT (`test.run` spawns
it, reads one JSON object per test, records one `testrun` EVENT per verdict
tagged by the node that ran it); tests linked to symbols become a proposal's
`checks`; the command envelope's `target` slot lands with this node's offers
recorded — the mesh's LOCAL shape.

**Architecture:** Dialect + one bash runner change + tests; no engine change.
(1) **Vocabulary** (`madcide_enums.inc`): `asset_layer` bits + converters,
`test_result`, `node_serve`, the four new `test_verb` codes, `nkBUILDRUN`, the
three `*_min_layer` tables. (2) **`madcide_layers.inc`** (new): the ONE
`asset_layers_of` owner, `node_offers` (this node's Client record), the
`target` resolver, the layer refusal envelope. (3) **`scripts/run_tests.sh`**:
`--report=json` — every verdict, skip, note and the summary as one JSON line
each; the human output unchanged without it. (4) **`madcide_tests.inc`**: the
`testrun` fold, `test.list / candidates / run / results`, one runner spawn per
test directory, the external-command seat, `proposal_checks`. (5) **Seat
wiring** (`madcide_mcp.inc`, `madcide_past.inc`, `madcide_propose.inc`,
`madcide_nexus.inc`, `madcide_core.inc`): the layer gate at every family's
entry after the tier gate with the parse handle opened only for a parseable
asset; `graph.status` reports layers + node; `nexus.explain` reports layers;
`graph.accept` and a `checks: true` proposal run the linked tests; the
proposal fold attaches its runs; the build pump writes a `buildrun` event.

**Tech Stack:** dialect `.inc` (value-first), `madc::channel exec://`
(`poll_state`, `sleep_ms`, `cancel`, `exit_status`), `<bits/file_kinds>`
ranges, the L4c stream (`clog_append`, `clog_find`), the L4d fold and refs,
bash (the runner), `.mad` tests.

**Spec:** `docs/plans/2026-09-13-nexus-L4-design.md` §1 (the fifth question),
§3.9 (asset layers), §3.10 (the verification axis), §3.11 (multi-master
local shape), §4.5, §8 L4e, §9 L4e row, §10 defaults 11–14. Owner approval
2026-09-13; the L4d recap named this slice as NEXT (handoff UPDATE 15).

## Decisions this plan settles

1. **`asset_layers_of(w, es, doc, node)` takes `es`** (the design wrote
   `(w, doc, node)`): the manifest and the git handle ride the editor-state
   bag. `node` 0 = this node; any other node has no offers yet (v1) — its
   node-relative layers are 0 and the `target` resolver refuses before a
   verb reaches the layers.
2. **The SESSION is a project.** `alMANAGED` is true for every asset document
   of the session — the implicit single-file project ("or the one buffer of
   the implicit project", §3.9) and every manifest member alike; it is false
   for a pseudo-buffer (`[build]`, `[terminal]`: a path starting with `[`).
   Every L4b–L4d test serves an implicit project and stays green.
3. **`alVERSIONED` = managed + a repository above the file + the file inside
   its work tree** (`git_relpath` non-empty). An untracked scratch file inside
   the madc checkout IS versioned (the commit verbs answer zero rows) — the
   layer says the axis exists, not that it has rows.
4. **`alLEXABLE` and `alPARSEABLE` are the compiler's kinds today**: the C,
   C++ and madc ranges of `<bits/file_kinds>` through ONE range predicate
   (`kind_compiler_range`). The engine's lexer (`lex_spans`) is the C-family
   tokenizer; `<bits/file_kinds>` says depth of support is a table per layer
   owned by that layer — this predicate is the seat's table. The two bits
   diverge the day a lexer-only kind (json) gains rows.
5. **`alEXECUTABLE` (node 0) = parseable**: the running madc builds and runs
   what it parses (`parse_run`, `madcrun://`). **`alTESTABLE` (node 0)** = at
   least one LIVE `test` record exists AND this node has a runner for one of
   their families: `mad` → `scripts/run_tests.sh` exists under the cwd and
   the platform is not win32 (the runner is bash; the `env` prefix is posix —
   the L4d ruling); `external` → the record carries a `command`; `unit` → no
   runner on any node in v1 (`make -C src test` owns it).
6. **Layer tables as data**: `graph_min_layer` (status → text; commits →
   versioned; history / revision / diff → versioned + parseable; every other
   verb → parseable), `nexus_min_layer` (explain → managed + parseable; the
   rest → managed), `test_min_layer` (run → managed + testable; the rest →
   managed). The gate runs after the tier gate; the parse handle is opened
   ONLY when the asset is parseable (a binary never reaches `parse_open`).
7. **The refusal names the missing layer and what the asset has**:
   `no parseable layer for kind image ('graph.symbols' needs it; this asset
   has: text, managed, versioned)`.
8. **`--report=json` replaces every human line with one JSON object per
   line**: a verdict `{test, family: "mad", result: pass|fail|timeout,
   exit, seconds, detail}`; a skip `{test, family, result: "skip", reason}`;
   the exe / obj lanes `{test, family, lane, result}`; caveats `{note}`; the
   summary `{summary: {passed, failed, timed_out, skipped}}`. Exit status
   unchanged. `seconds` is the bash `SECONDS` delta (integer; bash 3.2 and
   BSD date safe). `detail` = the first unmet expect line, `rc=N`, or
   `noisy stderr`; JSON-escaped (`\` `"` tab).
9. **`test.run(ids, target?, proposal?)` groups `mad` records by their
   directory** — ONE runner spawn per directory: `env MADC_BIN=<the running
   madc> MADC_TEST_DIR=<dir> bash scripts/run_tests.sh --report=json <base…>`
   (the runner's existing generic env knobs; `{madc}` = `compiler_path`,
   the ONE compiler). An `external` record runs its `command` through
   `build_subst` (exit 0 = pass, else fail; a silent child past the idle
   deadline = timeout after `cancel()`). A record whose family has no runner
   here refuses the WHOLE call naming the family and the runner it looked
   for — nothing half-runs.
10. **Every verdict is ONE `testrun` event**: `{kind: testrun, es, doc,
    actor, test: <record id>, name, family, node: 0, result: <word>, exit,
    seconds, detail, output_ref: "", synced: <bool>, proposal?}`. `synced` =
    the served document's buffer equals its file on disk (the runner tests
    the DISK; an unsaved buffer's runs are labelled, never blocked). The
    latest run per test is the fold's derived state (`test.list` shows it).
11. **`test.candidates(ref)` = the test records linked `tests` → ref;
    the fallback is NAMED, never run**: `{tests: [], fallback: true, suite:
    {family: "mad", count: N}}`. `proposal_checks` runs explicit candidates
    only — a whole-suite run inside an accept call would park the seat for
    minutes; the answer says the fallback set exists so an agent runs it
    explicitly with `test.run`.
12. **Checks are DERIVED like status**: `graph.accept` always runs the
    explicit candidates of every op's target after landing; a proposal with
    `checks: true` runs them right after its record is appended (the events
    need the proposal's seq). The runs carry `proposal: seq`; `proposal_fold`
    attaches them as the proposal's `checks` rows. The answer carries the
    summary `{ran, passed, failed, fallback, suite?}`. A failing check never
    blocks (§3.4).
13. **The `target` slot** (§3.11): null / "" / 0 / this node's platform word
    (`madc::sys.platform`) = here; anything else refuses `no node offers
    '<target>' (this node: <platform>; serves build, run, test)`. **This
    node's Client record** = `node_offers` `{node: 0, platform, toolchains:
    ["madc"], serves: [build, run, test]}` computed once onto the es bag
    (`offers`) and reported by `graph.status` as `node`; `serves` words come
    from the `node_serve` enum, the toolchain word from `ide_gen`.
14. **A `buildrun` event at the build pump's end** `{kind: buildrun, es,
    doc, actor: -1, node: 0, uri, exit, stopped}` — the build/run verdict of
    this node in the one stream (§3.11). Appended by the pump (a session-
    thread task); not pushed to other clients until their next command
    (the hub pushes per command — a named residue, not a defect).
15. **`nexus.explain` and `graph.status` report `layers`** as the wire words
    (ascending bit order); `graph.status` drops its L4b preview list.
16. **No engine change**; no rule trailers. The one `src/`-adjacent file is
    `scripts/run_tests.sh` (a script, not `src/`).

## Global Constraints

- **ONE stream, ONE fold per record family**: `testrun` / `buildrun` are
  events through `clog_append`; `testrun_fold` is their only reader.
- **Enums, not strings**: layer words, result words, serve words, verb names
  have enum + `*_name` / `*_of` converters; a misspelling refuses at the
  boundary; dispatch is a `switch`. The enum gate's anchor list grows with
  them (`layer`, `test_result`, `node_serve`). No `== "<word>"` compare
  against a converter word in the seat files (the gate's rule 2).
- **No `"dir": "…"` / `"mode": "…"` literal field in a seat file** (the enum
  gate's rule 5 reads them as layout discriminators) — spell `"directory"`.
- **The runner stays the ONE harness** — the seat never re-reads a fixture
  convention; it reads the runner's JSON.
- **Value-first dialect code**; `var`; out-param carriers; literals for
  objects; `keyed_get` / `arg_of`; a map is a null var made an object by its
  first keyed write; `{}` is an empty ARRAY; an unsigned-char enumerator in
  `format` prints as a CHARACTER (route through a `long`); two `c_str()` in
  one call is fine, hoist `format(...).c_str()`.
- **Thread contract**: layers are computed per call from bag facts (no
  cache); a run's child lives inside the one verb call on the session
  thread (cooperative `poll_state` + `sleep_ms`, `cancel` on the idle
  deadline); the offers record is written once per session.
- Targeted tests only; the battery rides the V6 seam.

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `tools/madcide/madcide_enums.inc` | modify | `asset_layer`, `test_result`, `node_serve` (+ converters); `test_verb` codes; `nkBUILDRUN`; `*_min_layer`; `test_min_tier(tvRUN)` |
| `tools/madcide/madcide_layers.inc` | create | `kind_compiler_range`, `path_is_asset`, `runner_path_of`, `tests_runnable_here`, `asset_layers_of`, `node_offers`, `target_node_of`, `layer_refuse` |
| `tools/madcide/madcide_tests.inc` | modify | `testrun_fold`, `testrun_write`, `doc_synced_to_disk`, `child_collect`, `test_list`, `test_candidates`, `test_run_ids`, `test_results`, `proposal_checks` |
| `tools/madcide/madcide_mcp.inc` | modify | include `madcide_layers.inc`; forward decl `proposal_checks`; descriptors t30–t33; layer gates in `graph_call` / `nexus_call` / `test_call` (handle opened lazily); `checks` on a proposal answer; `test_call` cases |
| `tools/madcide/madcide_past.inc` | modify | `graph_status`: layers via `asset_layers_of`, `node`, a 0 handle |
| `tools/madcide/madcide_propose.inc` | modify | `proposal_fold` attaches `checks`; `graph_accept` runs `proposal_checks` |
| `tools/madcide/madcide_nexus.inc` | modify | `nexus_explain` gains `layers` |
| `tools/madcide/madcide_core.inc` | modify | `build_pump` appends the `buildrun` event |
| `scripts/run_tests.sh` | modify | `--report=json` |
| `scripts/check-madcide-enums.sh` | modify | anchors `layer|test_result|node_serve`; `madcide_layers.inc` joins the seat files |
| `tests/testnexus_layers.mad` (+ `.expect`, `.expect_quiet`, `.win64_skip`) | create | the two-asset fixture project; per-layer refusals; the runner run; testrun events; results; candidates; checks; the target slot |
| `tests/testgraphpast.expect` | modify | `layers=6` |
| `tests/testnexus_records.mad` (+ `.expect`) | modify | the tiers line gains `run=editor` |
| `docs/plans/2026-09-13-nexus-L4-design.md` | modify | §3.9/§3.10/§3.11 "as landed"; §9 L4e shipped |

---

### Task 1: The vocabulary + gates as data (`madcide_enums.inc`) + the enum gate

**Files:** modify `tools/madcide/madcide_enums.inc` (the `nexus_kind` block;
after `test_min_tier`), `scripts/check-madcide-enums.sh`.

**Interfaces (produces):**
```c
enum asset_layer : unsigned char { alTEXT = 1, alVERSIONED = 2, alMANAGED = 4, alLEXABLE = 8, alPARSEABLE = 16, alEXECUTABLE = 32, alTESTABLE = 64 };
const char *layer_name(long bit); long layer_of(var &word);
void layer_words(var &out, long bits); long layer_missing(long have, long need);
enum test_result : unsigned char { trNONE = 0, trPASS, trFAIL, trSKIP, trTIMEOUT, trERROR };
const char *test_result_name(long r); long test_result_of(var &word);
enum node_serve : unsigned char { nsNONE = 0, nsBUILD, nsRUN, nsTEST };
const char *node_serve_name(long s); long node_serve_of(var &word);
enum test_verb : unsigned char { tvNONE = 0, tvDISCOVER, tvLIST, tvCANDIDATES, tvRUN, tvRESULTS };
enum nexus_kind { …, nkTESTRUN, nkBUILDRUN };   // nexus_kind_of reads "buildrun"
long graph_min_layer(long code); long nexus_min_layer(long code); long test_min_layer(long code);
long test_min_tier(long code);   // run = editor
```

- [ ] **Step 1: `nexus_kind`** — append `nkBUILDRUN`; `nexus_kind_of` gains
  `if ( k == "buildrun" ) return nkBUILDRUN;`. The block comment: "the later
  kinds' writers arrive with L4c–L4e" → "(L4e: testrun / buildrun)".
- [ ] **Step 2: `test_verb`** → `{ tvNONE = 0, tvDISCOVER, tvLIST, tvCANDIDATES, tvRUN, tvRESULTS }`;
  `test_min_tier`: `case tvRUN: return tierEDITOR;` (it spends build/run
  resources — `cmd_min_tier`'s rule) beside `tvDISCOVER → tierPROPOSER`.
- [ ] **Step 3: The layer vocabulary** (after `test_min_tier`):

```c
// ---- asset LAYERS (Nexus L4e; design §3.9) ----------------------------------
// Every axis is a layer an asset has or lacks, computed by ONE function
// (asset_layers_of, madcide_layers.inc) from facts the engine holds — never
// assumed by a verb. A BITSET: the wire words are the review's discovery
// vocabulary, converted once (layer_words out, layer_of in). executable and
// testable are NODE-relative (this node's offers); the rest are the asset's.
enum asset_layer : unsigned char {
    alTEXT = 1, alVERSIONED = 2, alMANAGED = 4, alLEXABLE = 8, alPARSEABLE = 16,
    alEXECUTABLE = 32, alTESTABLE = 64
};
const char *layer_name(long bit)
{
    switch ( bit )
    {
	case alTEXT: return "text";
	case alVERSIONED: return "versioned";
	case alMANAGED: return "managed";
	case alLEXABLE: return "lexable";
	case alPARSEABLE: return "parseable";
	case alEXECUTABLE: return "executable";
	case alTESTABLE: return "testable";
    }
    return "";
}
long layer_of(var &word)
{
    if ( word.is_null() )
	return 0;
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( word == layer_name(b) )
	    return b;
    return 0;
}
// The set as wire words, ascending bit order.
void layer_words(var &out, long bits)
{
    out = {};
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( (bits & b) != 0 )
	    out.push(layer_name(b));
}
// The first layer `need` asks for that `have` lacks; 0 = every layer present.
long layer_missing(long have, long need)
{
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( (need & b) != 0 && (have & b) == 0 )
	    return b;
    return 0;
}

// The layers a verb family needs, DATA beside its tier (design §3.9): the
// gate runs at graph_call / nexus_call / test_call entry after the tier gate,
// before any structure is touched. graph.status needs only the bytes (it
// REPORTS the layers); the commit-level history verbs need the repository
// (a binary member has commit history); the line-level PAST verbs need the
// tree too; every other graph verb reads the tree.
long graph_min_layer(long code)
{
    switch ( code )
    {
    case gvSTATUS:
	return alTEXT;
    case gvCOMMITS:
	return alVERSIONED;
    case gvHISTORY:
    case gvREVISION:
    case gvDIFF:
	return alVERSIONED | alPARSEABLE;
    }
    return alPARSEABLE;
}
// Records, links, context and the sources need tracking (a project — every
// session asset has it); explain projects the tree too.
long nexus_min_layer(long code)
{
    if ( code == nvEXPLAIN )
	return alMANAGED | alPARSEABLE;
    return alMANAGED;
}
// Test records need tracking; RUNNING needs a runner on the target node.
long test_min_layer(long code)
{
    if ( code == tvRUN )
	return alMANAGED | alTESTABLE;
    return alMANAGED;
}

// A test run's verdict (design §3.10) — the runner's word converted once
// (the boundary), the enum in every reader and on every testrun event.
enum test_result : unsigned char { trNONE = 0, trPASS, trFAIL, trSKIP, trTIMEOUT, trERROR };
const char *test_result_name(long r)
{
    switch ( r )
    {
	case trPASS: return "pass";
	case trFAIL: return "fail";
	case trSKIP: return "skip";
	case trTIMEOUT: return "timeout";
	case trERROR: return "error";
    }
    return "";
}
long test_result_of(var &word)
{
    if ( word.is_null() )
	return trNONE;
    for ( long k = trPASS; k <= trERROR; k = k + 1 )
	if ( word == test_result_name(k) )
	    return k;
    return trNONE;
}

// What a node OFFERS (design §3.11, client-server §2.8 invariant 2): the
// services its Client record advertises. This node serves all three.
enum node_serve : unsigned char { nsNONE = 0, nsBUILD, nsRUN, nsTEST };
const char *node_serve_name(long s)
{
    switch ( s )
    {
	case nsBUILD: return "build";
	case nsRUN: return "run";
	case nsTEST: return "test";
    }
    return "";
}
long node_serve_of(var &word)
{
    if ( word.is_null() )
	return nsNONE;
    for ( long k = nsBUILD; k <= nsTEST; k = k + 1 )
	if ( word == node_serve_name(k) )
	    return k;
    return nsNONE;
}
```

- [ ] **Step 4: The gate.** `scripts/check-madcide-enums.sh`: the awk anchor
  gains `|layer|test_result|node_serve`; `SEAT_FILES` gains
  `madcide_layers.inc`; the header comment names L4e. Run it: GREEN (no
  compare against the new words exists — verified by grep before this plan).

---

### Task 2: `madcide_layers.inc` — the ONE layer owner, this node's offers, the `target` slot

**Files:** create `tools/madcide/madcide_layers.inc`; modify
`tools/madcide/madcide_mcp.inc` (include it right after `madcide_past.inc`).

**Interfaces (produces):**
```c
bool kind_compiler_range(long k);
bool path_is_asset(var &p);
void runner_path_of(var &out, long family);        // "" = no runner here
bool tests_runnable_here(long w);
long asset_layers_of(long w, long es, long doc, long node);
void node_offers(var &out, long w, long es);        // {node, platform, toolchains, serves}
bool target_node_of(long &node, var &why, var &target, long w, long es);
void layer_refuse(var &out, var &nm, long miss, long have, long w, long doc);
```

- [ ] **Step 1: The file.**

```c
// madcide_layers.inc — asset LAYERS (Nexus L4e; design §3.9, §3.11; plan
// 2026-09-14-nexus-L4e-verification-plan.md). Every axis — text, git,
// tracking, lexing, parsing, execution, testing — is a layer an asset has or
// lacks; ONE function computes the set from facts the engine already holds
// (the document's kind from <bits/file_kinds>, the repository above it, the
// session's project, the test records and this node's runners) — no verb
// assumes a layer below it exists. Included by madcide_mcp.inc after
// madcide_past.inc (git_handle_of / git_relpath_of) and before the verb
// families; the test-record read reaches madcide_nexus.inc's fold through the
// declaration below (the L4d precedent: a declaration resolves a later body).
//
// Thread contract: pure reads of session-thread bag state per call (no
// cache); node_offers writes the es bag once per session.

void nexus_fold(var &out, long w);		// madcide_nexus.inc (included later)

// The compiler's kinds: the C, C++ and madc ranges of <bits/file_kinds> —
// what the tokenizer lexes and the parser accepts. <bits/file_kinds> makes
// depth of support a table per layer owned by that layer; this predicate is
// the seat's table for BOTH the lexed and the parsed layer today (they part
// the day a lexer-only kind gains rows).
bool kind_compiler_range(long k)
{
    if ( k >= madc::fkC && k <= madc::fkC_LAST )
	return true;
    if ( k >= madc::fkCPP && k <= madc::fkCPP_LAST )
	return true;
    if ( k >= madc::fkMADC && k <= madc::fkMADC_LAST )
	return true;
    return false;
}

// An ASSET is a file; a pseudo-buffer ([build], [terminal]) is bytes only.
bool path_is_asset(var &p)
{
    if ( p.is_null() )
	return false;
    const char *s = p.c_str();
    if ( strlen(s) == 0 )
	return false;
    return s[0] != '[';
}

// The runner THIS node has for a test family ("" = none): the canonical
// runner for the mad family (bash, under the project root = the cwd; posix
// only — the env prefix and bash are not win32's); an external record
// carries its own command; the unit family runs through make -C src test —
// no seat here (v1).
void runner_path_of(var &out, long family)
{
    out = "";
    if ( family != tfMAD )
	return;
    if ( strcmp(madc::sys.platform, "win32") == 0 )
	return;
    var rp = "scripts/run_tests.sh";
    if ( php::file_exists(rp) )
	out = rp;
}

// Does any LIVE test record have a runner on this node?
bool tests_runnable_here(long w)
{
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	    continue;
	long st = record_state_of(r["state"]);
	if ( st == rsSUPERSEDED || st == rsREJECTED )
	    continue;
	var flds = r["fields"];
	long fam = test_family_of(flds["family"]);
	if ( fam == tfEXTERNAL )
	{
	    var cmd = flds["command"];
	    if ( !cmd.is_null() && strlen(cmd.c_str()) > 0 )
		return true;
	    continue;
	}
	var rp;
	runner_path_of(rp, fam);
	if ( strlen(rp.c_str()) > 0 )
	    return true;
    }
    return false;
}

// THE layer owner (design §3.9). The bytes always; tracking for every asset
// of the session (the implicit single-file project IS a project, a manifest
// member too — never a pseudo-buffer); the git layer when a repository is
// above the file and the file is inside its work tree; lexing and parsing by
// the compiler's kinds; on THIS node (0) execution = what madc parses it
// runs, testing = live test records with a runner here. Another node has no
// offers yet (v1): its node-relative layers are 0.
long asset_layers_of(long w, long es, long doc, long node)
{
    long layers = alTEXT;
    var p;
    ui::get(p, w, doc, "path");
    if ( !path_is_asset(p) )
	return layers;
    layers = layers | alMANAGED;
    if ( kind_compiler_range(doc_kind(w, doc)) )
	layers = layers | alLEXABLE | alPARSEABLE;
    if ( git_handle_of(w, es, doc) > 0 )
    {
	var rel;
	git_relpath_of(rel, w, doc);
	if ( strlen(rel.c_str()) > 0 )
	    layers = layers | alVERSIONED;
    }
    if ( node == 0 )
    {
	if ( (layers & alPARSEABLE) != 0 )
	    layers = layers | alEXECUTABLE;
	if ( tests_runnable_here(w) )
	    layers = layers | alTESTABLE;
    }
    return layers;
}

// This node's Client record (design §3.11; client-server §2.8 invariants 1–2):
// what it offers — its platform, its toolchains (the running madc), the
// services it serves. Computed once onto the es bag ("offers"); graph.status
// reports it; a refusal of a foreign target quotes it.
void node_offers(var &out, long w, long es)
{
    ui::get(out, w, es, "offers");
    if ( !out.is_null() )
	return;
    var plat = madc::sys.platform;
    var toolchains = {};
    toolchains.push(gen_name(genMADC));
    var serves = {};
    for ( long s = nsBUILD; s <= nsTEST; s = s + 1 )
	serves.push(node_serve_name(s));
    out = { "node": 0, "platform": plat, "toolchains": toolchains, "serves": serves };
    ui::set(w, es, "offers", out);
}

// The command envelope's `target` (design §3.11, invariant 3): null / "" /
// 0 / this node's platform word = here (node 0); anything else names a node
// no one offers — refused with prose, never run locally by surprise.
bool target_node_of(long &node, var &why, var &target, long w, long es)
{
    node = 0;
    if ( target.is_null() )
	return true;
    var offers;
    node_offers(offers, w, es);
    if ( target.is_integer() )
    {
	if ( target.as_integer() == 0 )
	    return true;
    }
    else
    {
	if ( strlen(target.c_str()) == 0 || target == offers["platform"] )
	    return true;
    }
    var sv = php::implode(", ", offers["serves"]);
    why = format("no node offers '{}' (this node: {}; serves {})", target, offers["platform"], sv);
    return false;
}

// The layer refusal every family shares: the missing layer by name, the
// asset's kind, the verb, and what the asset HAS.
void layer_refuse(var &out, var &nm, long miss, long have, long w, long doc)
{
    var words;
    layer_words(words, have);
    var kw = madc::file_kind_name(doc_kind(w, doc));
    var has = php::implode(", ", words);
    var why = format("no {} layer for kind {} ('{}' needs it; this asset has: {})", layer_name(miss), kw, nm, has);
    tool_refuse(out, why);
}
```

(`tool_refuse` is defined in madcide_mcp.inc AFTER the includes — add a
forward declaration `void tool_refuse(var &out, var &why);` at the top of
this file, or move `tool_refuse` / `tool_answer` above the includes. Choose:
MOVE them above `#include "madcide_past.inc"` — they are envelope helpers
every family shares; the move is a cut-and-paste within madcide_mcp.inc.)

- [ ] **Step 2: Include.** In `madcide_mcp.inc`, after `#include "madcide_past.inc"`:
  `#include "madcide_layers.inc"` with a two-line comment.

---

### Task 3: `scripts/run_tests.sh --report=json`

- [ ] **Step 1: Option + helpers** (after `MADC_EXE_ADVISORY`):

```bash
# --report=json (Nexus L4e, design §3.10): the runner is the ONE test harness
# and the Nexus its CLIENT — every verdict, skip, lane result, caveat and the
# summary leaves as ONE JSON object per line instead of the human lines, so a
# machine reads the fixture protocol's answer without re-implementing it.
# The exit status is unchanged. Human output is untouched without the flag.
REPORT=""
json_str() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g'
}
# verdict <base> <result> <rc> <seconds> <detail>
verdict() {
    [ "$REPORT" = json ] || return 0
    printf '{"test":"%s","family":"mad","result":"%s","exit":%s,"seconds":%s,"detail":"%s"}\n' \
        "$1" "$2" "$3" "$4" "$(json_str "$5")"
}
# skipped <base> <reason>
skipped() {
    [ "$REPORT" = json ] || return 0
    printf '{"test":"%s","family":"mad","result":"skip","reason":"%s"}\n' "$1" "$2"
}
# lane <base> <lane> <result>
lane() {
    [ "$REPORT" = json ] || return 0
    printf '{"test":"%s","family":"mad","lane":"%s","result":"%s"}\n' "$1" "$2" "$3"
}
# say <human line>: the human line, or {note} in report mode
say() {
    if [ "$REPORT" = json ]; then printf '{"note":"%s"}\n' "$(json_str "$1")"; else echo "$1"; fi
}
```
  Option parse: `--report=*) REPORT="${1#--report=}"; shift ;;` — an unknown
  value refuses: after the loop `if [ -n "$REPORT" ] && [ "$REPORT" != json ]; then echo "run_tests.sh: --report takes json" >&2; exit 2; fi`.
- [ ] **Step 2: The sites.** `echo "TEST DIRECTORY: …"` → `say`. The three skip
  `continue`s → `skipped "$base" mir` / `stdlib` / `domain` before `continue`.
  Around the JIT run: `t0=$SECONDS` before the `if [ -f "$expect_err_file" ]`
  block; after it `secs=$((SECONDS - t0))`. `echo "NOISY(stderr): $t"` →
  `[ "$REPORT" = json ] || echo …; unmet="noisy stderr"`. The verdict block:

```bash
    if [ $ok -eq 1 ]; then
        PASS=$((PASS+1))
        verdict "$base" pass "$rc" "$secs" ""
    else
        if [ $timed_out -eq 1 ]; then
            [ "$REPORT" = json ] || echo "TIMEOUT: $t"
            TIMEOUTS=$((TIMEOUTS+1))
            verdict "$base" timeout "$rc" "$secs" ""
        else
            [ "$REPORT" = json ] || echo "FAIL: $t"
            FAIL=$((FAIL+1))
            verdict "$base" fail "$rc" "$secs" "${unmet:-rc=$rc}"
        fi
        …detail block unchanged…
    fi
```
  OBJ: `OBJ_PASS…; lane "$base" obj pass` / `[ "$REPORT" = json ] || echo "FAIL(obj): $t"; …; lane "$base" obj fail` (and `obj-build` → `lane "$base" obj fail`). EXE likewise with `exe`.
  The tail: the three caveat `echo`s → `say`; the summary:
```bash
if [ "$REPORT" = json ]; then
    printf '{"summary":{"passed":%s,"failed":%s,"timed_out":%s,"skipped":%s}}\n' "$PASS" "$FAIL" "$TIMEOUTS" "$SKIP"
else
    echo "$PASS passed, $FAIL failed, $TIMEOUTS timed out, $SKIP skipped"
fi
```
  and the EXE/OBJ summary lines under `[ "$REPORT" = json ] ||` (their counts ride the lane rows).
- [ ] **Step 3: Verify.** `bash scripts/run_tests.sh --report=json testint testphpdirs`
  → three JSON lines (two verdicts + a `{note}` for the SUBSET caveat + the
  summary); `bash scripts/run_tests.sh testint` → unchanged human output;
  `bash scripts/run_tests.sh --report=xml testint` → exit 2. A deliberately
  failing scratch dir (`tmp/rt_json/bad.mad` + `.expect` "nope") with
  `MADC_TEST_DIR=tmp/rt_json` → `result: fail, detail: nope`, exit 1.
- [ ] **Step 4: Commit** — `feat(runner): --report=json — one JSON object per verdict / skip / lane / note + the summary; the exit status unchanged (Nexus L4e: the runner is the ONE harness, the Nexus its client)`.

---

### Task 4: `madcide_tests.inc` — the runs fold, `test.list / candidates / run / results`, `proposal_checks`

**Interfaces (produces):**
```c
void testrun_fold(var &out, long w, long test, long since, long node);   // {rows, latest: test-key -> row}
long testrun_write(long w, long es, long adoc, long self_id, var &rec_test, long node, long result, long exit, long seconds, var &detail, long proposal, bool synced);
bool doc_synced_to_disk(long w, long doc);
bool child_collect(var &lines, madc::channel &c, long idle_ms);          // false = idle deadline (cancelled)
void test_list(var &out, long w, long family);
void test_candidates(var &out, long w, var &ref);
void test_run_ids(var &out, IdeSession &S, long doc, long self_id, var &ids, long node, long proposal);
void test_results(var &out, long w, long test, long since, long node);
void proposal_checks(var &out, IdeSession &S, long doc, long self_id, var &ops, long seq);
```

- [ ] **Step 1: Header comment** — "L4e adds …" becomes the description of
  the runner seat: "Which structure answers what: list — the fold's test
  records + the LATEST testrun per test; candidates — the fold's `tests`
  links; run — the canonical runner spawned per directory (`--report=json`),
  an external record's command, every verdict ONE testrun event tagged node
  0; results — the testrun fold. Thread contract: a run's child lives inside
  the one verb call (cooperative poll + sleep; cancel on the idle deadline)."

- [ ] **Step 2: The fold + writer + helpers** (after `test_discover`):

```c
// ---- the runs: testrun EVENTS folded (design §3.10) --------------------------
// rows = every testrun (oldest first) passing the filters (test 0 = any test;
// since = seq > since; node < 0 = any node); latest = test-id-key -> its
// newest row (the derived state).
void testrun_fold(var &out, long w, long test, long since, long node)
{
    var rows = {};
    var latest;
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
	    if ( clog_kind_of(rec) != ckOTHER || nexus_kind_of(rec) != nkTESTRUN )
		continue;
	    if ( test != 0 && rec["test"].as_integer() != test )
		continue;
	    if ( rec["seq"].as_integer() <= since )
		continue;
	    if ( node >= 0 && rec["node"].as_integer() != node )
		continue;
	    rows.push(rec);
	    var key = format("{}", rec["test"]);
	    latest[key.c_str()] = rec;
	}
    }
    out = { "rows": rows, "latest": latest };
}

// Is the served document's buffer what its file on disk holds? The runner
// tests the DISK; a run against an unsaved buffer is labelled, never blocked.
bool doc_synced_to_disk(long w, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    var disk;
    if ( !php::file_get_contents(disk, p.c_str()) )
	return false;
    var t;
    ui::text(t, w, doc);
    return disk == t;
}

// ONE testrun event per verdict (design §3.10): the test record's id and
// names, the node that ran it, the result word, the exit status, the seconds,
// the runner's detail, the proposal it checks (0 = none).
long testrun_write(long w, long es, long adoc, long self_id, var &rec_test, long node, long result,
		   long exit, long seconds, var &detail, long proposal, bool synced)
{
    var word = test_result_name(result);
    var flds = rec_test["fields"];
    var rec = { "kind": "testrun", "es": es, "doc": adoc, "actor": self_id, "test": rec_test["id"],
		"name": flds["name"], "family": flds["family"], "node": node, "result": word, "exit": exit,
		"seconds": seconds, "detail": detail, "output_ref": "", "synced": synced };
    if ( proposal > 0 )
	rec["proposal"] = proposal;
    return clog_append(w, adoc, rec);
}

// Every stdout line of a child until it drains, under an IDLE deadline: no
// line within idle_ms cancels the child (SIGTERM) and answers false.
// Cooperative — poll_state + sleep_ms, never a blocking read on a silent
// pipe (the mcp_client_call idiom, collecting every line instead of one).
bool child_collect(var &lines, madc::channel &c, long idle_ms)
{
    lines = {};
    long waited = 0;
    for ( ;; )
    {
	long st = c.poll_state();
	if ( st < 0 )
	    return true;		// drained
	if ( st == 0 )
	{
	    if ( waited >= idle_ms )
	    {
		c.cancel();
		return false;
	    }
	    madc::sleep_ms(20);
	    waited = waited + 20;
	    continue;
	}
	var ln;
	if ( !c.readline(ln) )
	    return true;
	waited = 0;
	lines.push(ln);
    }
    return true;
}
```

- [ ] **Step 3: `test.list` / `test.candidates` / `test.results`**:

```c
// The live test records (family tfNONE = every family).
void test_records(var &out, long w, long family)
{
    out = {};
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	    continue;
	long st = record_state_of(r["state"]);
	if ( st == rsSUPERSEDED || st == rsREJECTED )
	    continue;
	var flds = r["fields"];
	if ( family != tfNONE && test_family_of(flds["family"]) != family )
	    continue;
	out.push(r);
    }
}

// test.list(family?): every live test record with its LATEST run (`last`,
// null when it never ran) — the derived state of the evidence axis.
void test_list(var &out, long w, long family)
{
    var recs;
    test_records(recs, w, family);
    var runs;
    testrun_fold(runs, w, 0, 0, -1);
    var rows = {};
    for ( var r : recs )
    {
	var key = format("{}", r["id"]);
	var last;
	keyed_get(last, runs["latest"], key.c_str());
	var flds = r["fields"];
	var row = { "id": r["id"], "name": flds["name"], "family": flds["family"], "asset": flds["asset"],
		    "state": r["state"], "origin": r["origin"], "last": last };
	rows.push(row);
    }
    out = { "rows": rows };
}

// test.candidates(ref): the test records linked `tests` -> ref (explicit
// links only, ground truth by declaration). None linked: the HONEST fallback
// is the mad family's whole suite — NAMED, never run here (design §3.10; an
// agent runs it with test.run explicitly).
void test_candidates(var &out, long w, var &ref)
{
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var recs = f["records"];
    var tests = {};
    var seen;
    for ( var l : f["links"] )
    {
	if ( rel_of(l["rel"]) != relTESTS || !(l["to_key"] == key) )
	    continue;
	var from = l["from"];
	if ( ref_kind_of(from["ref"]) != refRECORD )
	    continue;
	var rkey = format("{}", from["id"]);
	var was;
	keyed_get(was, seen, rkey.c_str());
	if ( !was.is_null() )
	    continue;
	var r;
	keyed_get(r, recs, rkey.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	    continue;
	seen[rkey.c_str()] = 1;
	tests.push(r);
    }
    if ( php::count(tests) > 0 )
    {
	out = { "ref": ref, "tests": tests, "fallback": false };
	return;
    }
    var mads;
    test_records(mads, w, tfMAD);
    var fam = test_family_name(tfMAD);
    var suite = { "family": fam, "count": php::count(mads) };
    out = { "ref": ref, "tests": tests, "fallback": true, "suite": suite };
}

// test.results(test?, since?, node?): the testrun rows + the latest per test.
void test_results(var &out, long w, long test, long since, long node)
{
    testrun_fold(out, w, test, since, node);
}
```

- [ ] **Step 4: `test.run`**:

```c
// The row the runner reported for a test base, or null.
void report_row_of(var &out, var &rows, var &base)
{
    for ( var r : rows )
	if ( r["lane"].is_null() && r["test"] == base )
	{
	    out = r;
	    return;
	}
}

// test.run(ids, target?, proposal?): the mad records grouped by DIRECTORY —
// ONE canonical-runner spawn per directory (`--report=json`, the fixture
// protocol's answer read as data); an external record's command through
// build_subst (exit 0 = pass); a family with no runner on this node refuses
// the WHOLE call (nothing half-runs). Every verdict = one testrun event
// tagged with the node (0 = here) and the proposal it checks.
void test_run_ids(var &out, IdeSession &S, long doc, long self_id, var &ids, long node, long proposal)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    var groups;			// directory -> [record…] (dynamic keys)
    var dirs = {};
    var externals = {};
    var runner;
    runner_path_of(runner, tfMAD);
    for ( var idv : ids )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	{
	    var why = format("no test record {}", idv);
	    out = { "error": why };
	    return;
	}
	var flds = r["fields"];
	long fam = test_family_of(flds["family"]);
	if ( fam == tfMAD )
	{
	    if ( strlen(runner.c_str()) == 0 )
	    {
		var why = format("no runner for family {} on this node (looked for scripts/run_tests.sh under the project root, posix)", flds["family"]);
		out = { "error": why };
		return;
	    }
	    var dir = php::dirname(flds["asset"]);
	    var have;
	    keyed_get(have, groups, dir.c_str());
	    if ( have.is_null() )
	    {
		have = {};
		dirs.push(dir);
	    }
	    have.push(r);
	    groups[dir.c_str()] = have;
	}
	else if ( fam == tfEXTERNAL )
	{
	    var cmd = flds["command"];
	    if ( cmd.is_null() || strlen(cmd.c_str()) == 0 )
	    {
		var why = format("test {} (external) names no command", idv);
		out = { "error": why };
		return;
	    }
	    externals.push(r);
	}
	else
	{
	    var why = format("no runner for family {} on this node (make -C src test owns the unit binaries)", flds["family"]);
	    out = { "error": why };
	    return;
	}
    }
    bool synced = doc_synced_to_disk(w, adoc);
    var self = madc::compiler_path();
    var rows = {};
    long passed = 0;
    long failed = 0;
    for ( var dir : dirs )
    {
	var members;
	keyed_get(members, groups, dir.c_str());
	var bases = "";
	for ( var r : members )
	{
	    var flds = r["fields"];
	    var base;
	    path_sans_ext(base, flds["asset"]);
	    var bn = php::basename(base);
	    bases = format("{} {}", bases, bn);
	}
	var cmd = format("env MADC_BIN={} MADC_TEST_DIR={} bash {} --report=json{}", self, dir, runner, bases);
	var uri = format("exec://{}", cmd);
	madc::channel c(uri.c_str());
	if ( !c.ok() )
	{
	    var why = format("cannot spawn the runner: {}", c.last_error());
	    out = { "error": why };
	    return;
	}
	c.close_write();
	var lines;
	bool drained = child_collect(lines, c, 120000);
	c.close();
	long xs = c.exit_status();
	var reports = {};
	for ( var ln : lines )
	{
	    var rec;
	    if ( js::parse(rec, ln) && !rec["test"].is_null() )
		reports.push(rec);
	}
	for ( var r : members )
	{
	    var flds = r["fields"];
	    var base;
	    path_sans_ext(base, flds["asset"]);
	    var bn = php::basename(base);
	    var rep;
	    report_row_of(rep, reports, bn);
	    long result = trERROR;
	    long exit = xs;
	    long seconds = 0;
	    var detail = "no report line from the runner";
	    if ( !drained )
		detail = "the runner went silent (cancelled)";
	    if ( !rep.is_null() )
	    {
		result = test_result_of(rep["result"]);
		if ( result == trNONE )
		    result = trERROR;
		if ( !rep["exit"].is_null() )
		    exit = rep["exit"].as_integer();
		if ( !rep["seconds"].is_null() )
		    seconds = rep["seconds"].as_integer();
		detail = rep["detail"];
		if ( detail.is_null() )
		    detail = "";
	    }
	    long seq = testrun_write(w, es, adoc, self_id, r, node, result, exit, seconds, detail, proposal, synced);
	    if ( result == trPASS )
		passed = passed + 1;
	    else
		failed = failed + 1;
	    var row = { "test": r["id"], "name": flds["name"], "result": test_result_name(result), "exit": exit,
			"seconds": seconds, "detail": detail, "seq": seq };
	    rows.push(row);
	}
    }
    for ( var r : externals )
    {
	var flds = r["fields"];
	var cmd;
	build_subst(cmd, flds["command"].c_str(), w, es, adoc);
	var uri = format("exec://{}", cmd);
	madc::channel c(uri.c_str());
	long result = trERROR;
	long exit = -1;
	var detail = "";
	if ( !c.ok() )
	    detail = c.last_error();
	else
	{
	    c.close_write();
	    var lines;
	    bool drained = child_collect(lines, c, 60000);
	    c.close();
	    exit = c.exit_status();
	    if ( !drained )
	    {
		result = trTIMEOUT;
		detail = "silent past the idle deadline (cancelled)";
	    }
	    else if ( exit == 0 )
		result = trPASS;
	    else
	    {
		result = trFAIL;
		detail = format("exit {}", exit);
	    }
	}
	long seq = testrun_write(w, es, adoc, self_id, r, node, result, exit, 0, detail, proposal, synced);
	if ( result == trPASS )
	    passed = passed + 1;
	else
	    failed = failed + 1;
	var row = { "test": r["id"], "name": flds["name"], "result": test_result_name(result), "exit": exit,
		    "seconds": 0, "detail": detail, "seq": seq };
	rows.push(row);
    }
    out = { "ok": true, "node": node, "ran": php::count(rows), "passed": passed, "failed": failed,
	    "synced": synced, "rows": rows };
}

// A proposal's CHECKS (design §3.10): the tests linked `tests` to every op's
// target symbol, run here and recorded under the proposal (the events carry
// its seq; the fold attaches them). Explicit candidates only — the family's
// whole suite is NAMED as the fallback, never run inside an accept.
// Informing, never blocking.
void proposal_checks(var &out, IdeSession &S, long doc, long self_id, var &ops, long seq)
{
    long w = S.world();
    long adoc = S.active(doc);
    var ids = {};
    var seen;
    var suite;
    bool fallback = false;
    for ( var op : ops )
    {
	var ref;
	symbol_ref_of_doc(ref, w, adoc, op["target"]);
	var cands;
	test_candidates(cands, w, ref);
	if ( cands["fallback"].as_boolean() )
	{
	    fallback = true;
	    suite = cands["suite"];
	    continue;
	}
	for ( var t : cands["tests"] )
	{
	    var key = format("{}", t["id"]);
	    var was;
	    keyed_get(was, seen, key.c_str());
	    if ( !was.is_null() )
		continue;
	    seen[key.c_str()] = 1;
	    ids.push(t["id"]);
	}
    }
    if ( php::count(ids) == 0 )
    {
	out = { "ran": 0, "passed": 0, "failed": 0, "fallback": fallback, "suite": suite };
	return;
    }
    var run;
    test_run_ids(run, S, doc, self_id, ids, 0, seq);
    if ( !run["error"].is_null() )
    {
	out = { "ran": 0, "passed": 0, "failed": 0, "fallback": fallback, "error": run["error"] };
	return;
    }
    out = { "ran": run["ran"], "passed": run["passed"], "failed": run["failed"], "fallback": fallback,
	    "suite": suite, "rows": run["rows"] };
}
```

(`php::basename` — confirm it exists in `include/madc/ns_php` (dirname does);
else derive with `perl::substr` after the last `/`. `"ran": run["ran"]` and a
null `suite` in a literal — a null value serialises as `null`; fine.)

---

### Task 5: Seat wiring — descriptors, the layer gates, `test_call`, `checks`, `graph.status`, `nexus.explain`, `proposal_fold`, `buildrun`

- [ ] **Step 1: `tool_refuse` / `tool_answer` move** above the includes in
  `madcide_mcp.inc` (Task 2's note); `ref_arg` stays where it is.
- [ ] **Step 2: Forward declaration** before `#include "madcide_propose.inc"`:
  `void proposal_checks(var &out, IdeSession &S, long doc, long self_id, var &ops, long seq);` (defined in madcide_tests.inc, included after).
- [ ] **Step 3: Descriptors** in `test_tool_descriptors`: `test.list {family?}`,
  `test.candidates {ref}`, `test.run {ids: [record ids], target?: node id |
  platform word (null = here), proposal?: seq}` (editor tier), `test.results
  {test?, since?, node?}`. `graph_tool_descriptors`: the three mutation
  schemas gain `checks` (boolean: "with propose: run the tests linked to the
  target and attach them to the proposal"); `graph.accept` description: "…
  records the decision, runs the linked tests as its checks".
- [ ] **Step 4: `graph_call`** — after the tier gate:

```c
    // The LAYER gate (Nexus L4e, design §3.9): the verb's layers as data
    // beside its tier; the parse handle opens ONLY for a parseable asset (a
    // binary member never reaches parse_open).
    long have = asset_layers_of(w, es, adoc, 0);
    long miss = layer_missing(have, graph_min_layer(gv));
    if ( miss != 0 )
    {
	layer_refuse(out, nm, miss, have, w, adoc);
	return;
    }
    long h = 0;
    if ( (have & alPARSEABLE) != 0 )
	h = ensure_phandle(w, es, adoc);
```
  (remove the `long h = ensure_phandle(w, es, adoc);` at the top.) In the
  mutation case: `var cv; arg_of(cv, args, "checks"); bool want_checks = !cv.is_null() && cv.as_boolean();`
  and after `graph_edit_apply(snap, …)`: `if ( mode == emPROPOSE && want_checks && !snap["proposal"].is_null() ) { var chk; proposal_checks(chk, S, doc, self_id, ops, snap["proposal"].as_integer()); snap["checks"] = chk; }`.
  `case gvSTATUS: graph_status(snap, w, es, adoc, h); break;` — h may be 0 (Step 7).
- [ ] **Step 5: `nexus_call`** — the same gate with `nexus_min_layer(nv)`;
  `long h = 0; if ( (have & alPARSEABLE) != 0 ) h = ensure_phandle(w, es, adoc);`
  (nexus_context on a non-parseable asset: `symbol_node_of` gets `h == 0` →
  guard `if ( h == 0 ) return false;` at its top in madcide_nexus.inc).
- [ ] **Step 6: `test_call`** — the gate with `test_min_layer(tv)`; the cases:

```c
    case tvLIST:
    {
	var fv;
	arg_of(fv, args, "family");
	long fam = tfNONE;
	if ( !fv.is_null() && strlen(fv.c_str()) > 0 )
	{
	    fam = test_family_of(fv);
	    if ( fam == tfNONE )
	    {
		var why = format("unknown test family '{}'", fv);
		snap = { "error": why };
		break;
	    }
	}
	test_list(snap, w, fam);
	break;
    }
    case tvCANDIDATES:
    {
	var ref;
	if ( !ref_arg(ref, out, args, "ref") )
	    return;
	test_candidates(snap, w, ref);
	break;
    }
    case tvRUN:
    {
	var idsv;
	arg_of(idsv, args, "ids");
	if ( idsv.is_null() || php::count(idsv) == 0 )
	{
	    snap = { "error": "test.run needs ids: [record id, ...] (test.list names them)" };
	    break;
	}
	var tv2;
	arg_of(tv2, args, "target");
	long node = 0;
	var why;
	if ( !target_node_of(node, why, tv2, w, es) )
	{
	    snap = { "error": why };
	    break;
	}
	var pv;
	arg_of(pv, args, "proposal");
	long prop = 0;
	if ( !pv.is_null() )
	    prop = pv.as_integer();
	test_run_ids(snap, S, doc, self_id, idsv, node, prop);
	break;
    }
    case tvRESULTS:
    {
	var tv2;
	arg_of(tv2, args, "test");
	var sv;
	arg_of(sv, args, "since");
	var nv2;
	arg_of(nv2, args, "node");
	long test = 0;
	long since = 0;
	long node = -1;
	if ( !tv2.is_null() )
	    test = tv2.as_integer();
	if ( !sv.is_null() )
	    since = sv.as_integer();
	if ( !nv2.is_null() )
	    node = nv2.as_integer();
	test_results(snap, w, test, since, node);
	break;
    }
```
- [ ] **Step 7: `graph_status`** (madcide_past.inc): `errors` / `gen` / `synced`
  computed only when `live != 0` (else 0 / 0 / true); the preview `layers`
  block replaced by `var layers; layer_words(layers, asset_layers_of(w, es, adoc, 0));`
  (the git block keeps its `git` object; drop its `layers.push`); the answer
  gains `"node": offers` from `node_offers(offers, w, es)`. Header comment:
  status reads the layer owner.
- [ ] **Step 8: `nexus_explain`** (madcide_nexus.inc): `var lw; layer_words(lw, asset_layers_of(w, es, adoc, 0));`
  and `"layers": lw` in the answer literal.
- [ ] **Step 9: `proposal_fold`** (madcide_propose.inc): `nkTESTRUN` records
  with a `proposal` naming a folded proposal push onto `p["checks"]`
  (`var chk = p["checks"]; if ( chk.is_null() ) chk = {}; chk.push(rec); p["checks"] = chk;`).
  `graph_accept`: after the decision record and before the answer:
  `var chk; proposal_checks(chk, S, doc, self_id, p["ops"], seq);` and
  `"checks": chk` in the answer; the broadcast moves AFTER the checks (their
  events ride the same push).
- [ ] **Step 10: `build_pump`** (madcide_core.inc) after the exit-status lines:

```c
    // The EVIDENCE record (Nexus L4e, design §3.11): this node's build / run
    // verdict in the ONE stream, tagged by the node that produced it.
    long adoc = active_doc(w, es, 0);
    if ( adoc != 0 )
    {
	var brec = { "kind": "buildrun", "es": es, "doc": adoc, "actor": -1, "node": 0, "uri": uri,
		     "exit": xc, "stopped": stopped };
	clog_append(w, adoc, brec);
    }
```
- [ ] **Step 11: Gates + neighbours.** `check-madcide-enums`,
  `check-madcide-single-owners`, `check-dialect-literals`, `check-dialect-lean`
  GREEN. `bash tmp/l3_check.sh` over: testgraphpast (expect `layers=6`),
  testnexus_records (tiers line += `run=editor`), testmcpclient,
  testmadcide_serve_propose, testmadcide_serve_edit, testmadcide_serve_graph,
  testmadcide_serve_mcp, testgraphedit, testidepanel, testmadcide.
- [ ] **Step 12: Commit** (dialect + gate script + the two expect updates) —
  `feat(graph-mcp): nexus L4e — asset layers as ONE capability bitset (asset_layers_of) with per-family layer gates and refusals by name, this node's offers + the target slot, the test.* runner seat (list / candidates / run / results) over the canonical runner's JSON report, testrun / buildrun events tagged by node, proposal checks from linked tests`.

---

### Task 6: `tests/testnexus_layers.mad`

The served file `madc_ide_layers.mad` = `long add(long a, long b) { return a + b; }\nlong run(long n) { return add(n, 1); }\n`;
the binary `madc_ide_layers.png` = the bytes `\x89PNG\r\n\x1a\n` (written
with `php::file_put_contents`); the manifest `madc_ide_layers.prj.json` =
`{"tus": ["madc_ide_layers.mad", "madc_ide_layers.png"], "entry": "main"}`
(written BEFORE `S.open` so `proj_startup` loads it). A scratch tests dir
`madc_ide_ltests/` with `ok.mad` (`int main() { println("hello"); return 0; }`
+ `ok.expect` `hello`), `bad.mad` (prints `bye`, `bad.expect` `hello`),
`slow.mad` (`int main() { madc::sleep_ms(4000); return 0; }` + `slow.timeout` `1`).

- `graph.status` → `status: layers=text,lexable,managed,parseable,versioned,executable` … print the joined words sorted? No — print `php::implode(",", layers)` (ascending bits: text, versioned, managed, lexable, parseable, executable) and `node.platform` non-empty, `serves=3`.
- `edit_file(w, S.es, doc, "madc_ide_layers.png")` → `graph.status` → `png-status: layers=text,versioned,managed kind=image` (kind via `madc::file_kind_name(doc_kind(...))`); `graph.symbols` → isError containing `no parseable layer for kind image`; `graph.commits` → not an error (rows 0); `nexus.records` → not an error; `nexus.explain` → isError `no parseable layer`; `test.list` → not an error; `test.run {ids: [1]}` → isError `no testable layer` (no test records yet). `edit_file` back to the `.mad`.
- `test.discover {dir: "madc_ide_ltests"}` → 3 created; `graph.status` → `testable` present now; `test.list` → 3 rows, `last` null.
- `test.run {ids: [<ok>, <bad>, <slow>]}` → `run: ok=1 ran=3 passed=1 failed=2 synced=1`; rows by name: ok=pass, bad=fail detail=hello, slow=timeout.
- `test.results {}` → 3 rows; latest keys 3; `test.results {test: <ok>}` → 1; `test.results {node: 7}` → 0.
- `test.list {family: "mad"}` → `last.result` of ok = pass.
- A second `test.run {ids: [<ok>]}`; `test.results {since: <seq of first run's last row>}` → 1.
- External: `nexus.create {rkind: "test", fields: {name: "ext-ok", family: "external", asset: "", command: "{madc} --no-config madc_ide_ltests/ok.mad"}}` → id; `test.run` → pass exit 0; `nexus.create … command: "{madc} --no-config madc_ide_ltests/exit3.mad"` (`int main() { return 3; }`) → fail exit 3.
- Unit: `nexus.create {rkind: "test", fields: {name: "u", family: "unit"}}` → `test.run {ids: [<u>]}` → isError `no runner for family unit`.
- Target: `test.run {ids: [<ok>], target: "darwin-arm64"}` → isError containing `no node offers 'darwin-arm64'`; `target: <platform word>` → runs (ok=1).
- Candidates: `nexus.link {from: {ref: record, id: <ok>}, rel: "tests", to: {ref: symbol, file: "madc_ide_layers.mad", kind: "Function", name: "add"}}`; `test.candidates {ref: add}` → `tests=1 fallback=0`; `{ref: run}` → `tests=0 fallback=1 suite.family=mad suite.count=3`.
- Checks: `graph.replace {id: <add>, src: "long add(long a, long b) { return a + b + 0; }", propose: true, checks: true}` → `checks.ran=1 passed=1 fallback=0`; `graph.proposal {seq}` → `checks` rows 1 whose `proposal` == seq; `graph.accept {seq}` → `ok=1 checks.ran=1`; `test.results {}` count grew by 2; a proposal on `run` with `checks: true` → `checks.ran=0 fallback=1`.
- Tier policy as data: `test_min_tier(tvRUN) == tierEDITOR`, `test_min_layer(tvRUN) & alTESTABLE`, `graph_min_layer(gvCOMMITS) == alVERSIONED`, `nexus_min_layer(nvEXPLAIN) & alPARSEABLE`.
- Cleanup: every scratch file and dir; `madc_ide_layers.prj.json`.

`.expect` pinned from the run; `.expect_quiet`; `.win64_skip` = `the runner is bash and the env prefix is posix (test.run on win32 refuses by design)`.

- [ ] Commit — `test(graph-mcp): nexus L4e — the two-asset fixture project refused / served per layer, the canonical runner run through test.run (pass / fail / timeout as testrun events), results / list / candidates, external and unit families, the target slot, proposal checks (testnexus_layers)`.

---

### Task 7: Docs, hand-off, push

- [ ] design doc: §3.9 "as landed" (es parameter; the session is a project;
  versioned = repo + in-tree; compiler ranges for lexed + parsed; testable
  needs a runner here); §3.10 "as landed" (`--report=json` shape; one spawn
  per directory; external commands; unit refused; checks = explicit
  candidates, fallback named; synced flag); §3.11 "as landed" (target
  resolver; offers on the es bag; buildrun at the pump); §9 L4e SHIPPED.
- [ ] `claude_status.json` live_handoff UPDATE 16 (L4e shipped; the L4 arc
  content complete on the branch; NEXT = the V6 arc RELEASE seam
  preparation: /dupaudit scoped to madcdis + the seat, then the ONE battery
  — the owner's call), the L4 ledger, memory, KG Decisions
  `nexus_asset_layers_session_is_project`, `nexus_checks_explicit_only`.
- [ ] `git push origin feature/client-server-views-claude`.

---

## Self-review

- **Spec coverage**: §3.9 layers as a bitset from ONE function, per-family
  layer data, refusals naming the layer, status/explain report the set, a
  binary member keeps commit history while line verbs refuse → Tasks 1, 2,
  5, 6. §3.10 tests as records (L4d), runs as events tagged by node, the
  runner as the ONE harness with `--report=json`, the Nexus as client
  (`test.run` spawns it), the external seat, `test.list / candidates / run /
  results`, checks on accept/propose informing never blocking → Tasks 3, 4,
  5, 6. §3.11 local shape: `target` accepted/refused, node-tagged buildrun /
  testrun, offers on the Client record → Tasks 2, 4, 5. §8 L4e gate (a
  two-asset fixture refused/served per layer; a run through the real runner
  yields a testrun event; a proposal with a linked failing test stays open
  with the run attached — the `bad` test linked in a variant: Task 6 links
  `ok`; ADD a `bad`-linked proposal to Task 6: link `bad` tests `run`,
  propose on `run` with checks → `checks.failed=1`, `graph.proposal` status
  open). Deviations named: decisions 1–3, 5, 9, 11, 14.
- **Placeholders**: `php::basename` is a CONFIRM (fallback named); the pinned
  counts are verify-on-run.
- **Type consistency**: `asset_layers_of(w, es, doc, node) -> long` used by
  the three `*_call`s, `graph_status`, `nexus_explain`; `layer_refuse(out,
  nm, miss, have, w, doc)`; `test_run_ids(out, S, doc, self_id, ids, node,
  proposal)` used by `test_call` and `proposal_checks`; `testrun_fold(out, w,
  test, since, node)` used by `test_list` / `test_results`;
  `proposal_checks(out, S, doc, self_id, ops, seq)` declared before
  `madcide_propose.inc`, defined in `madcide_tests.inc`; `target_node_of(node,
  why, target, w, es) -> bool`.
