# Nexus L4b — the PAST verbs (`graph.status / source / history / commits / revision / diff`), revision handles by generation TAG, the change-log kind enum — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Answer "how did this node get this way" and "what did this file look
like at commit X" through the code-graph MCP: a merged git + session history
per node, the commits touching the served file, a REVISION HANDLE (the blob at
a commit parsed by the same engine) whose ids route themselves, a
declaration-level diff between revisions or against the live buffer, a node's
source text, and a one-call status of the served asset.

**Architecture:** Three layers, each doing one thing. (1) **Engine**
(`src/madc_program.cpp` + the five-layer plumbing): `parse_open_tagged` opens a
READ-ONLY parse handle whose generation is a fresh TAG in `[2^20, 2^21)`, so
every id it mints names its handle; `graph_route(handle, id)` returns the handle
an id belongs to (the live one, a tagged one, or 0); a refresh of a tagged
handle refuses; `GitRepo::blame_buffer` blames the LIVE buffer text against the
committed file; `git_relpath` canonicalises a path against the repository root
through the ONE canonicalizer madc already has; `php::time()` (PHP parity)
gives the log its timestamp. (2) **Texteditor layer**
(`tools/texteditor/editor_events.inc`): the log's record KIND becomes an enum at
the reader (`clog_kind_of`, once), and every record carries `ts`. (3) **Seat**
(`tools/madcide/madcide_past.inc`, included by `madcide_mcp.inc`): the six PAST
verbs over `madc::git_*` (L4a) + the graph reads, a per-document revision cache
(sha → tagged handle + its text entity, LRU-bounded), id/`rev` routing in
`graph_call`, and the read-only refusal for a mutation aimed at a revision id.

**Tech Stack:** C++11 engine (`src/madc_program.cpp`, `src/madcdis_git_repo.cpp`,
`src/madc_git.cpp`, `src/ns_php.cpp`), the C bridge (`src/parser.cpp`,
`include/ns_common.h`), the dialect wrappers (`src/ns_madc.cpp`,
`include/madc/ns_madc`, `include/madc/ns_php`), the texteditor core and the
madcide seat (dialect `.inc`), value-first `.mad` tests, doctest.

**Spec:** `docs/plans/2026-09-13-nexus-L4-design.md` §3.1 (the stream; the
`clog_kind` enum), §3.3 (revision handles by generation tag), §3.7 (provenance),
§3.9 (asset layers — L4e retrofits the gates; this slice reports what it can),
§4.3 (the PAST verbs and the structure each reads), §5 flows 1–2, §6 refusals,
§7 thread contracts, §8 L4b tests, §9 slice table, §10 (2). Owner approval of
the design: 2026-09-13; "Start it": 2026-09-13.

## Decisions this plan settles

1. **The project-scoped event stream moves to L4c.** The design listed it under
   L4b, but its first CROSS-DOCUMENT consumer is the proposal record (L4c);
   every PAST verb here reads ONE document's records. Keeping V4's per-document
   log for one more slice removes the riskiest change from this one and lets
   L4c land the scoping beside the record kinds that need it. The reader-side
   enum tidy (`clog_kind`) DOES land here — the history walk reads the stream.
2. **Tags are ALLOCATED BY THE ENGINE, never supplied by a caller.**
   `parse_open_tagged(source, filename)` stamps the next tag from a process-wide
   counter starting at `2^20`; the seat never touches id bits. `graph_route`
   is the ONE place an id's generation is read against handles (the engine
   owns the id layout — one spelling of `GRAPH_GEN_SHIFT`).
3. **A tagged handle is read-only at the engine**, not only at the seat:
   `parse_refresh` / `parse_refresh_checked` refuse it (a bump would make its
   generation collide with the next tag). `parse_close` forgets its tag.
4. **History = two exact sources, no heuristics.** GIT rows: `git_blame_buffer`
   over the live text (uncommitted lines blame to a zero oid and are dropped —
   the event rows cover them), folded to distinct commits. EVENT rows: (a)
   every `nodeop` record whose target has the node's kind+name (declared
   identity), and (b) the text splices that intersected the node's span,
   walked NEWEST → OLDEST with the inverse of `shift_anchors`' arithmetic up to
   and including the most recent overlapping splice — the row after it is not
   derivable from coordinates alone, so the walk stops there and says so
   (`complete: false` on the answer). Rows carry `provenance: git|event`.
5. **`rev` and ids route; `""` is the live buffer.** An id-taking read routes
   by the id's generation (`graph_route`); an id-less read (`symbols`,
   `definition`, `search`, `enclosing`, `at`) takes an optional `rev`
   (any rev-parse spec or a sha) and runs against that revision's handle.
   `graph.diff(a, b)` takes two specs; `""` = the live buffer.
6. **Revision text lives in its own text ENTITY** (`ui::create` +
   `ui::text_load`, the replay-buffer precedent) so `line_start_of` and every
   offset helper work unchanged; `graph.source` is one substring over the
   handle's text entity (the live doc or the revision's).
7. **`REV_HANDLES_MAX = 8` per document, LRU.** Eviction closes the parse
   handle (its ids go stale by construction — the tag is forgotten) and
   reuses the text entity slot. A constant beside the enums, not a config
   knob until someone needs to turn it.
8. **`php::time()` lands as a PHP-parity public** (`int time()`: seconds since
   the epoch). The dialect has no wall clock today; the event log's `ts` is
   its first consumer (design §2.4 lists `ts` on the ChangeEvent).
9. **Fixture = this repository's own `tests/testint.mad`** served from the
   checkout root (the runner's cwd on every lane): its history is ancient and
   stable, the working tree is clean at HEAD, and an in-session edit never
   touches the disk. Git rows are asserted by SHAPE, event rows and the diff
   EXACTLY.

## Global Constraints

Every task's requirements implicitly include this section.

- **Ids are engine-owned.** No dialect code reads or writes id bits; routing
  goes through `madc::graph_route`. Every id a tagged handle emits carries its
  tag by the existing `graph_id_stamp`.
- **Tagged handles are read-only everywhere**: the engine refuses a refresh; the
  seat refuses `insert / replace / delete` on a routed revision id with
  `"revision handles are read-only"`.
- **No heuristics in history**: a row is a blame hunk, a matching `nodeop`, or
  an intersecting splice — nothing inferred; `provenance` on every row.
- **ONE reader of a record's kind**: `clog_kind_of` (texteditor layer) and
  `nexus_kind_of` (madcide layer); no `rec["kind"] == "…"` literal compare
  survives in `tools/` (gated).
- **The canonical path owner is reused, not copied**: `git_relpath` promotes
  `canonical_path_for_compare` (`src/lexer.cpp:4724`, static today) to a
  declared function and calls it; no second canonicalizer.
- **Value-first dialect code**; `var`; out-param carriers; literals for objects;
  `perl::substr` for text ranges; `php::count` for array sizes; `var == var`
  compares text.
- **Rule trailers** on every `src/` / `include/` commit; **zero warnings**;
  targeted tests only (the battery rides the V6 seam).
- **Thread contract**: revision handles and text entities are per-document
  session-thread state like the live handle; `GitRepo` handles per document,
  opened lazily, closed with the document (design §7).

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `src/madc_program.cpp` | modify | tag counter + tag→handle map; `parse_open_tagged`; `parse_generation`; `graph_route`; refresh refusals; `parse_close` forgets a tag |
| `src/lexer.cpp`, `include/madc.h` | modify | `canonical_path_for_compare` declared (promoted from static) |
| `include/madcdis/git_repo.h`, `src/madcdis_git_repo.cpp` | modify | `GitRepo::blame_buffer`, `GitRepo::relpath` |
| `src/madc_git.cpp` | modify | `internal_program_git_blame_text`, `internal_program_git_relpath` |
| `src/ns_php.cpp`, `include/madc/ns_php` | modify | `php::time()` |
| `src/parser.cpp`, `include/ns_common.h`, `src/ns_madc.cpp`, `include/madc/ns_madc` | modify | bridges + publics for the five new engine entry points |
| `tests/testgraphtagged.mad` (+ `.expect`, `.expect_quiet`) | create | engine-level: tagged handles, routing, refusals |
| `tests/unit/test_gitrepo.cpp` | modify | `blame_buffer` + `relpath` cases |
| `tools/texteditor/editor_events.inc` | modify | `enum clog_kind` + `clog_kind_of`; switches in replay / compact / events_since; `ts` on every record |
| `tools/madcide/madcide_enums.inc` | modify | `graph_verb` += six; `enum nexus_kind` + `nexus_kind_of`; `REV_HANDLES_MAX` |
| `tools/madcide/madcide_past.inc` | create | the PAST helpers + the six verb bodies |
| `tools/madcide/madcide_mcp.inc` | modify | `#include "madcide_past.inc"`; six descriptors; routing + six cases in `graph_call`; the revision read-only refusal |
| `scripts/check-madcide-single-owners.sh` | modify | the kind-compare marker (+ negative control) |
| `tests/testgraphpast.mad` (+ `.expect`, `.expect_quiet`) | create | the six verbs through the seat over `tests/testint.mad` |
| `docs/plans/2026-09-13-nexus-L4-design.md` | modify | §9: the stream scoping → L4c; L4b shipped |

---

### Task 1: Engine — tagged handles, routing, read-only refusals

**Files:**
- Modify: `src/madc_program.cpp` (`parse_tu_state` at `:4950`, the id helpers at
  `:5647-5680`, `internal_program_parse_open` at `:5204`, `parse_refresh` at
  `:5266`, `parse_refresh_checked` (after `:5300`), `parse_close` at `:5339`)
- Modify: `src/parser.cpp` (fwd decls after the git block; bridges after
  `madc_git_dirty`), `include/ns_common.h` (after the `madc_git_*` block),
  `src/ns_madc.cpp` (after `git_dirty`), `include/madc/ns_madc` (after the
  `git_*` block)
- Create: `tests/testgraphtagged.mad`, `.expect`, `.expect_quiet`

**Interfaces (produces):**
```cpp
namespace madc {
    // A READ-ONLY parse handle of `source` stamped with a fresh generation TAG
    // (>= 2^20, engine-allocated): every id it mints names it (graph_route).
    // 0 = the open failed. Refresh refuses it; parse_close frees it.
    int64_t parse_open_tagged(const char *source, const char *filename);
    // The handle's current generation (a tag for a revision handle); -1 = none.
    int64_t parse_generation(int64_t handle);
    // The handle an id belongs to: `handle` when the id carries its generation,
    // else the open tagged handle whose tag the id carries, else 0 (stale or
    // unknown — call the verb on `handle` and let it refuse with the reason).
    int64_t graph_route(int64_t handle, int64_t id);
}
```

- [ ] **Step 1: Write the failing engine test** `tests/testgraphtagged.mad`:

```c
// Nexus L4b: revision handles carry a generation TAG so their ids route to
// them; a tagged handle is read-only at the engine; a closed tag is forgotten.
int main()
{
    var src = "long g = 1;\nlong f(long x) { return x + g; }\n";
    long live = madc::parse_open(src.c_str(), "tagged.mad");
    long rev = madc::parse_open_tagged(src.c_str(), "tagged.mad");
    println("opened: {}", live > 0 && rev > 0 && live != rev ? 1 : 0);
    long glive = madc::parse_generation(live);
    long grev = madc::parse_generation(rev);
    println("live-gen-zero: {}", glive == 0 ? 1 : 0);
    println("rev-gen-tagged: {}", grev >= 1048576 && grev < 2097152 ? 1 : 0);

    var sl;
    madc::graph_symbols(sl, live);
    var sr;
    madc::graph_symbols(sr, rev);
    var nl = sl["nodes"][0];
    var nr = sr["nodes"][0];
    long idl = nl["id"].as_integer();
    long idr = nr["id"].as_integer();
    println("same-name: {}", nl["name"] == nr["name"] ? 1 : 0);
    println("ids-differ: {}", idl != idr ? 1 : 0);
    println("route-live: {}", madc::graph_route(live, idl) == live ? 1 : 0);
    println("route-rev: {}", madc::graph_route(live, idr) == rev ? 1 : 0);
    println("route-rev-self: {}", madc::graph_route(rev, idr) == rev ? 1 : 0);
    println("route-unknown: {}", madc::graph_route(live, idr + 1048576) == 0 ? 1 : 0);

    var node;
    madc::graph_node(node, rev, idr);
    println("rev-node-answers: {}", node["name"] == nr["name"] ? 1 : 0);
    madc::graph_node(node, live, idr);
    println("rev-id-on-live-stale: {}", !node["stale"].is_null() && node["stale"].as_boolean() ? 1 : 0);

    println("refresh-refused: {}", madc::parse_refresh(rev, src.c_str()) ? 0 : 1);
    var diags;
    println("checked-refresh-refused: {}", madc::parse_refresh_checked(diags, rev, src.c_str()) ? 0 : 1);
    println("refusal-has-row: {}", php::count(diags) >= 1 ? 1 : 0);
    println("rev-gen-unchanged: {}", madc::parse_generation(rev) == grev ? 1 : 0);

    long rev2 = madc::parse_open_tagged(src.c_str(), "tagged.mad");
    println("second-tag-distinct: {}", madc::parse_generation(rev2) != grev ? 1 : 0);
    println("closed: {}", madc::parse_close(rev) ? 1 : 0);
    println("route-after-close: {}", madc::graph_route(live, idr) == 0 ? 1 : 0);
    println("gen-after-close: {}", madc::parse_generation(rev) == -1 ? 1 : 0);
    madc::parse_close(rev2);
    madc::parse_close(live);
    return 0;
}
```

`tests/testgraphtagged.expect`: the sixteen lines above each ending in `1`
(`opened: 1` … `gen-after-close: 1`). `tests/testgraphtagged.expect_quiet`:
one line, `stderr must be empty`.

- [ ] **Step 2: Run it to see it fail**: `bash tmp/l3_check.sh tests/testgraphtagged.mad`
  → `madc::parse_open_tagged` unresolved.

- [ ] **Step 3: Engine** — in `src/madc_program.cpp`, beside the id helpers
  (after `graph_stale_result`, `:5680`):

```cpp
// ---- L4b: revision handles carry a generation TAG (design §3.3) ------------
// The live handle's generation counts up from 0 per refresh; a REVISION handle
// (a blob at a commit parsed by this engine) is stamped once with a tag from
// [2^20, 2^21) — the top bit of the 21-bit field — so every id it mints names
// it and can be routed back (graph_route). Tags are engine-allocated (a caller
// never touches id bits); a tagged handle refuses refresh (a bump would make
// its generation collide with the next tag) and parse_close forgets its tag.
static const uint32_t GRAPH_GEN_TAG_BASE = 1u << 20;

static uint32_t &graph_next_tag()
{
    static uint32_t next = GRAPH_GEN_TAG_BASE;
    return next;
}

static std::map<uint32_t, int64_t> &graph_tagged_handles()
{
    static std::map<uint32_t, int64_t> tags;	// tag -> handle; erased on close
    return tags;
}

static bool graph_handle_is_tagged(const parse_tu_state *st)
{
    return st->generation >= GRAPH_GEN_TAG_BASE;
}
```

then the three entry points (after `internal_program_parse_open`):

```cpp
int64_t internal_program_parse_open_tagged(::Program &self,
					   const std::string &source_text,
					   const std::string &display_name)
{
    int64_t h = internal_program_parse_open(self, source_text, display_name);
    if ( h <= 0 )
	return h;
    parse_tu_state *st = parse_tu_get(h);
    uint32_t &next = graph_next_tag();
    if ( next >= (GRAPH_GEN_TAG_BASE << 1) )	// the 21-bit field is exhausted
    {
	internal_program_parse_close(h);
	return 0;
    }
    st->generation = next++;
    graph_tagged_handles()[st->generation] = h;
    return h;
}

int64_t internal_program_parse_generation(int64_t handle)
{
    parse_tu_state *st = parse_tu_get(handle);
    return st ? (int64_t)st->generation : -1;
}

int64_t internal_program_graph_route(int64_t handle, int64_t id)
{
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return 0;
    if ( graph_id_fresh(st, id) )
	return handle;
    std::map<uint32_t, int64_t>::const_iterator it =
	graph_tagged_handles().find(graph_id_gen(id));
    if ( it == graph_tagged_handles().end() || !parse_tu_get(it->second) )
	return 0;
    return it->second;
}
```

`internal_program_parse_refresh`: after `parse_tu_get`, `if (
graph_handle_is_tagged(st) ) return false;`. `internal_program_parse_refresh_checked`:
the same test, and `out_diags` gets ONE error row (the same row shape
`diagnostic_rows_from_child` emits: `{severity:"error", message:"revision
handles are read-only", line:0, column:0}` — build it with the existing row
builder if one takes a message, else the four-field map) before `return false`.
`internal_program_parse_close`: read `st` first; if tagged, erase its tag from
the map; then close. (Every `graph_*` verb already checks freshness against
`st->generation`, so a tagged handle answers its own ids and refuses others —
no verb changes.)

- [ ] **Step 4: Plumbing** — `parser.cpp` fwd decls (after the git block):

```cpp
// L4b (design §3.3): revision handles by generation tag.
int64_t internal_program_parse_open_tagged(::Program &self, const std::string &source_text,
					   const std::string &display_name);
int64_t internal_program_parse_generation(int64_t handle);
int64_t internal_program_graph_route(int64_t handle, int64_t id);
```

bridges (after `madc_git_dirty`; `parse_open_tagged` needs the active Program
like `madc_parse_open`):

```cpp
int64_t madc_parse_open_tagged(void *source, void *filename)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0;
    const std::string &src = *(const std::string *)source;
    const std::string &disp = *(const std::string *)filename;
    return madc::internal_program_parse_open_tagged(*active, src, disp.empty() ? "<source>" : disp);
}
int64_t madc_parse_generation(int64_t handle) { return madc::internal_program_parse_generation(handle); }
int64_t madc_graph_route(int64_t handle, int64_t id) { return madc::internal_program_graph_route(handle, id); }
```

`ns_common.h`: the three decls. `ns_madc.cpp`:

```cpp
int64_t parse_open_tagged(const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_parse_open_tagged(&s, &f); }
int64_t parse_generation(int64_t handle) { return madc_parse_generation(handle); }
int64_t graph_route(int64_t handle, int64_t id) { return madc_graph_route(handle, id); }
```

`include/madc/ns_madc`: the **Interfaces** block with its comment, after the
`git_*` declarations.

- [ ] **Step 5: Build + test**: `make -C src 2>&1 | grep -cE 'warning:|error:'` → `0`;
  `bash tmp/l3_check.sh tests/testgraphtagged.mad` → 0 missing, stderr empty;
  neighbours `testgraphedit`, `testparsehandle`, `testgraphaccessors` unchanged.

- [ ] **Step 6: Commit (trailers)** — `Hypothesis:` a revision's ids must name
  their handle or a wrong-handle id answers a wrong node; the generation field
  already partitions ids, so a tag in its top range makes routing exact.
  `Layer:` seat → `graph_route` (engine, the id layout's ONE owner) →
  `parse_tu_state::generation`; deepest: the stamp is where the id is minted.
  `Searched:` `graph_id_gen|graph_id_fresh|GRAPH_GEN_` in madc_program.cpp
  (the L3 helpers — reused, not re-spelled); `parse_tu_handles().close`.
  `Oracle:` n/a — tests/testgraphtagged.mad is the oracle (route/refuse/forget).

---

### Task 2: Engine — `GitRepo::blame_buffer`, `relpath`, `php::time()`

**Files:**
- Modify: `include/madcdis/git_repo.h`, `src/madcdis_git_repo.cpp`, `src/madc_git.cpp`,
  `src/lexer.cpp:4724` + `include/madc.h` (declare `canonical_path_for_compare`),
  `src/ns_php.cpp` + `include/madc/ns_php`, `src/parser.cpp`, `include/ns_common.h`,
  `src/ns_madc.cpp`, `include/madc/ns_madc`, `tests/unit/test_gitrepo.cpp`

**Interfaces (produces):**
```cpp
// GitRepo
bool blame_buffer(std::vector<GitBlameRow> &out, const std::string &path, const std::string &text,
                  size_t line0, size_t count, error *err = 0) const;   // uncommitted lines: sha ""
bool relpath(const std::string &path, std::string &out, error *err = 0) const; // canonical, workdir-relative
// madc::
value &git_blame_text(value &out, int64_t h, const char *path, const char *text, int64_t line, int64_t count);
value &git_relpath(value &out, int64_t h, const char *path);   // {path} | {error}
// php::
int64_t time();    // PHP time(): seconds since the Unix epoch
```

- [ ] **Step 1: Failing unit cases** in `tests/unit/test_gitrepo.cpp`:

```cpp
TEST_CASE("GitRepo blames a modified buffer against the committed file and resolves relpaths")
{
    Fixture fx;
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    std::vector<madc::GitBlameRow> b;
    REQUIRE(repo.blame_buffer(b, "a.txt", "one\ntwo\nthree\n", 1, 0, &err));
    REQUIRE(b.size() == 3);
    CHECK(b[0].sha == fx.first_sha);
    CHECK(b[1].sha == fx.second_sha);
    CHECK(b[2].sha.empty());                    // the uncommitted line
    CHECK(b[2].line == 3);
    std::string rel;
    REQUIRE(repo.relpath(fx.dir + "/a.txt", rel, &err));
    CHECK(rel == "a.txt");
    REQUIRE(repo.relpath(fx.dir + "/./sub/../a.txt", rel, &err));
    CHECK(rel == "a.txt");
    CHECK(!repo.relpath("/", rel, &err));       // outside the work tree
}
```

- [ ] **Step 2: Run** `make -C src ../bin/test_gitrepo` → fails to compile.

- [ ] **Step 3: Implement.** `blame_buffer`: `git_blame_file` for the reference
  (same `opts` as `blame`), then `git_blame_buffer(&bl2, ref, text.data(),
  text.size())`; rows from `bl2` exactly as `blame` builds them, with
  `git_oid_is_zero(&h->final_commit_id)` → `row.sha = ""` and no summary
  lookup; both blames freed. Factor the hunk → row loop into ONE static helper
  `blame_rows(git_repository*, git_blame*, std::vector<GitBlameRow>&)` used by
  `blame` and `blame_buffer` (no second spelling). `relpath`: promote
  `canonical_path_for_compare` — `src/lexer.cpp:4724` loses `static`, and
  `include/madc.h` declares `std::string canonical_path_for_compare(const
  std::string &path);` beside the other lexer-level path helpers (search the
  header for `resolve_real_path` mentions to place it); `relpath` canonicalises
  both `workdir()` and `path`, requires the workdir prefix (with the trailing
  slash), and answers the remainder; outside → false with prose. `php::time()`:
  `src/ns_php.cpp` `int64_t time() { return (int64_t)::time((time_t *)0); }`
  declared in `include/madc/ns_php` beside the other scalar publics (follow the
  header's declaration style — a mangled-direct namespace function).
  `madc_git.cpp`: `internal_program_git_blame_text` (rows via `git_blame_value`)
  and `internal_program_git_relpath` (`{path}` / `{error}`); bridges +
  wrappers + declarations in the four plumbing files, `std::string *` for text.

- [ ] **Step 4: Build + test**: 0 warnings; `bin/test_gitrepo` 5 cases SUCCESS;
  `tests/testgit.mad` still 16/16; a php parity smoke:
  `printf 'int main(){ println("{}", php::time() > 1700000000 ? 1 : 0); return 0; }' > tmp/l4b_time.mad`
  → prints `1`.

- [ ] **Step 5: Commit (trailers)** — `Hypothesis:` the history verb blames the
  LIVE buffer, not the file on disk; the seat needs a workdir-relative path;
  the log needs a clock. `Layer:` seat → `git_blame_text`/`git_relpath` →
  GitRepo → libgit2 (`git_blame_buffer`); the path canonicalizer is the
  existing lexer owner, promoted. `Searched:` "canonicalize a path for
  comparison" → `canonical_path_for_compare` (lexer.cpp:4724, static; the
  standing instance in AGENTS.md) — promoted, not copied; "wall clock in the
  dialect" → none in ns_php/ns_madc (a gap; PHP parity fills it exactly).
  `Oracle:` PHP `time()` semantics (seconds since epoch) mirrored exactly; the
  fixture repository is the blame oracle (line 3 uncommitted → zero oid).

---

### Task 3: The texteditor layer — `clog_kind` at the reader, `ts` on every record; the madcide `nexus_kind`

**Files:**
- Modify: `tools/texteditor/editor_events.inc` (`:221-430`), `tools/madcide/madcide_enums.inc`,
  `scripts/check-madcide-single-owners.sh`

- [ ] **Step 1: The enum + the one reader**, in `editor_events.inc` before
  `changelog_of`:

```c
// The log's record KIND is text only on the persisted line (the wire word);
// the ONE reader converts it here and every consumer switches on the code —
// the enum-boundary rule. ckOTHER = an annotation kind another layer owns
// (madcide's nodeop / proposal / record …): the texteditor layer replays
// splices and checkpoints and skips every other kind by construction.
enum clog_kind : unsigned char { ckNONE = 0, ckSPLICE, ckCHECKPOINT, ckOTHER };

long clog_kind_of(var &rec)
{
    var k = rec["kind"];
    if ( k.is_null() )
	return ckNONE;
    if ( k == "splice" )
	return ckSPLICE;
    if ( k == "checkpoint" )
	return ckCHECKPOINT;
    return ckOTHER;
}
```

Then `clog_replay`: `long kind = clog_kind_of(rec);` and the two compares
become `kind == ckCHECKPOINT` / `kind == ckSPLICE` (three sites at `:327`,
`:332`, `:334`). `clog_append`: `rec["ts"] = php::time();` beside `rec["seq"]`.

- [ ] **Step 2: madcide's own kinds**, in `madcide_enums.inc` beside the graph
  enums:

```c
// The annotation kinds madcide writes into the ONE stream (design §3.1); the
// texteditor layer sees them as ckOTHER. Text on the line, a code in the reader.
enum nexus_kind : unsigned char { nkNONE = 0, nkNODEOP, nkPROPOSAL, nkDECISION, nkRECORD, nkLINK, nkTESTRUN };

long nexus_kind_of(var &rec)
{
    var k = rec["kind"];
    if ( k.is_null() )
	return nkNONE;
    if ( k == "nodeop" )   return nkNODEOP;
    if ( k == "proposal" ) return nkPROPOSAL;
    if ( k == "decision" ) return nkDECISION;
    if ( k == "record" )   return nkRECORD;
    if ( k == "link" )     return nkLINK;
    if ( k == "testrun" )  return nkTESTRUN;
    return nkNONE;
}
```

(Only `nkNODEOP` has a writer today; the later kinds are the design's named
vocabulary and their writers arrive with L4c–L4e — the enum text is the one
place the names live.)

- [ ] **Step 3: The gate** — append to `scripts/check-madcide-single-owners.sh`
  a section: the record kind has ONE reader per layer; marker = a literal
  `["kind"] == "` compare anywhere under `tools/texteditor` and `tools/madcide`
  must count 0 (`grep -rcE '\["kind"\] == "' …`), with a negative control
  (`echo 'if ( rec["kind"] == "splice" )' >> $tmp` must count 1). Keep the
  script's existing style (a `count_*` function, the FAIL prose naming the
  owner: "route through clog_kind_of / nexus_kind_of").

- [ ] **Step 4: Run**: `bash scripts/check-madcide-single-owners.sh` GREEN;
  `bash tmp/l3_check.sh tests/testmadcide_changelog.mad` 0 missing (records
  gained `ts`; the test pins no record bytes — verified 2026-09-13 by grep);
  `testmadcide_serve_edit` 30/30.

- [ ] **Step 5: Commit** (dialect + script: no trailers) —
  `refactor(changelog): the record kind is read ONCE (clog_kind_of / nexus_kind_of), every record carries ts; the single-owners gate fails a literal kind compare`.

---

### Task 4: The seat — `madcide_past.inc`, six verbs, routing

**Files:**
- Create: `tools/madcide/madcide_past.inc`
- Modify: `tools/madcide/madcide_enums.inc` (`graph_verb`, `REV_HANDLES_MAX`),
  `tools/madcide/madcide_mcp.inc` (include line at the top, descriptors after
  `t18`, `graph_call` routing + cases)

**Interfaces (produces — the wire):**

| Verb | Arguments | Answer |
|---|---|---|
| `graph.status` | — | `{handle, generation, synced, errors, log_head, revisions:[{sha,tag}], git:{head,branch,detached,dirty,path} \| null, layers:[…text names…]}` |
| `graph.source` | `id` (routes) | `{node, text}` |
| `graph.history` | `id` (a LIVE id) | `{node, rows:[…], complete}` rows `{provenance:"git", sha, author, when, summary}` / `{provenance:"event", seq, ts, es, verb, at?, del?, ins?}` |
| `graph.commits` | `limit?` (default 20) | `{rows:[{sha, author, email, when, summary}]}` |
| `graph.revision` | `rev` | `{sha, tag}` (opens or reuses the revision handle) |
| `graph.diff` | `a`, `b` (rev specs; `""` = live) | `{added:[node], removed:[node], changed:[{a:node, b:node}]}` |
| every id-less read | `rev?` | answered against that revision |
| every mutation | a revision id | `{error:"revision handles are read-only"}` |

- [ ] **Step 1: Enums** — `graph_verb`: append `gvSTATUS, gvSOURCE, gvHISTORY,
  gvCOMMITS, gvREVISION, gvDIFF` (after `gvDELETE`; `graph_min_tier` answers
  `tierOBSERVER` for them by falling through). Add
  `const long REV_HANDLES_MAX = 8;	// revision handles kept per document (LRU)`
  beside the enums.

- [ ] **Step 2: `tools/madcide/madcide_past.inc`** (its header comment names
  the design §4.3 and this plan; every function is small, one job each):

```c
// madcide_past.inc — the PAST axis of the code-graph MCP (Nexus L4b; design
// docs/plans/2026-09-13-nexus-L4-design.md §4.3, plan …-L4b-past-verbs-plan.md).
// Reads over the L4a git substrate (madc::git_*) and the graph reads; owns the
// per-document revision cache (sha -> a READ-ONLY tagged parse handle + the
// text entity that handle was parsed from), the id/rev routing graph_call
// uses, and the six verb bodies. Included by madcide_mcp.inc.
//
// Which structure answers what (the index-is-not-the-graph law):
//   status   — the doc bag (phandle_text), parse_check, clog_head, GitRepo::head/dirty
//   source   — graph_span + the handle's TEXT entity (the live doc or a revision's)
//   history  — GIT: git_blame_text over the live text folded to commits;
//              EVENT: nodeop records with the node's kind+name, splices that
//              intersected the span (walked newest->oldest, inverse anchor shift)
//   commits  — GitRepo::log filtered by the served file
//   revision — git_show at the sha -> parse_open_tagged (the same engine)
//   diff     — graph_symbols of two handles keyed by kind+name; changed = text differs

// The doc's git handle (0 = no repository above it) and its workdir-relative
// path, opened once and kept on the doc bag ("ghandle", "grelpath").
long git_handle_of(long w, long es, long adoc)
{
    long gh = es_int(w, adoc, "ghandle", -1);
    if ( gh >= 0 )
	return gh;
    var p;
    ui::get(p, w, adoc, "path");
    var dir = php::dirname(p);		// ring: own it
    var d = dir;
    gh = madc::git_open(d.c_str());
    ui::set(w, adoc, "ghandle", gh);
    if ( gh > 0 )
    {
	var rp;
	madc::git_relpath(rp, gh, p.c_str());
	var rel = rp["path"];
	if ( rel.is_null() )
	    rel = "";
	ui::set(w, adoc, "grelpath", rel);
    }
    return gh;
}

void git_relpath_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "grelpath");
    if ( out.is_null() )
	out = "";
}

// ---- the revision cache: sha -> {tag, handle, ent}, LRU-bounded ----------
// A revision's text lives in its own text ENTITY so line_start_of and every
// offset helper work unchanged (the replay-buffer precedent).
void revs_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "revs");
    if ( out.is_null() )
	out = {};
}

void rev_order_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "rev_order");
    if ( out.is_null() )
	out = {};
}

// Resolve `spec` (a rev-parse spec or a sha) to the revision's entry, opening
// it on a miss and evicting the least-recently-used entry past REV_HANDLES_MAX.
// out = {sha, tag, handle, ent} or {error}.
void rev_handle_of(var &out, long w, long es, long adoc, const char *spec0)
{
    var spec = spec0;
    long gh = git_handle_of(w, es, adoc);
    if ( gh <= 0 )
    {
	out = { "error": "no repository above the served file" };
	return;
    }
    var rp;
    madc::git_revparse(rp, gh, spec.c_str());
    var sha = rp["sha"];
    if ( sha.is_null() )
    {
	out = { "error": rp["error"] };
	return;
    }
    var revs;
    revs_of(revs, w, adoc);
    var order;
    rev_order_of(order, w, adoc);
    var hit = revs[sha.c_str()];
    if ( !hit.is_null() )
    {
	// refresh recency: move sha to the end of the order
	var reordered = {};
	for ( var s : order )
	    if ( !(s == sha) )
		reordered.push(s);
	reordered.push(sha);
	ui::set(w, adoc, "rev_order", reordered);
	out = hit;
	return;
    }
    var rel;
    git_relpath_of(rel, w, adoc);
    var blob;
    madc::git_show(blob, gh, sha.c_str(), rel.c_str());
    var text = blob["text"];
    if ( text.is_null() )
    {
	out = { "error": blob["error"] };
	return;
    }
    var p;
    ui::get(p, w, adoc, "path");
    long h = madc::parse_open_tagged(text.c_str(), p.c_str());
    if ( h <= 0 )
    {
	out = { "error": "the revision's text did not open as a parse handle" };
	return;
    }
    long ent = ui::create(w, "revision-text");
    ui::text_load(w, ent, text.c_str());
    long tag = madc::parse_generation(h);
    var entry = { "sha": sha, "tag": tag, "handle": h, "ent": ent };
    // evict the least recently used past the bound
    while ( php::count(order) >= REV_HANDLES_MAX )
    {
	var victim = order[0];
	var ve = revs[victim.c_str()];
	if ( !ve.is_null() )
	{
	    madc::parse_close(ve["handle"].as_integer());
	    ui::text_load(w, ve["ent"].as_integer(), "");
	}
	var rest = {};
	for ( long i = 1; i < php::count(order); i = i + 1 )
	    rest.push(order[i]);
	order = rest;
	revs[victim.c_str()] = null;
    }
    revs[sha.c_str()] = entry;
    order.push(sha);
    ui::set(w, adoc, "revs", revs);
    ui::set(w, adoc, "rev_order", order);
    out = entry;
}

// The revision entry that owns tagged handle `h` (null = none / the live handle).
void rev_entry_of_handle(var &out, long w, long adoc, long h)
{
    var revs;
    revs_of(revs, w, adoc);
    out = null;
    for ( var e : revs )
	if ( !e.is_null() && e["handle"].as_integer() == h )
	    out = e;
}

// The handle a verb should run against: an id routes itself (graph_route —
// 0 = unknown/stale -> the live handle, whose verb refuses with the reason);
// an id-less read takes an optional `rev`. `is_rev` tells the caller the
// answer came from a revision (mutations refuse).
long route_handle(long w, long es, long adoc, long live, var &args, bool &is_rev)
{
    is_rev = false;
    var idv = args["id"];
    if ( idv.is_null() )
	idv = args["func"];
    if ( !idv.is_null() )
    {
	long h = madc::graph_route(live, idv.as_integer());
	if ( h == 0 )
	    return live;
	is_rev = h != live;
	return h;
    }
    var rv = args["rev"];
    if ( rv.is_null() || strlen(rv.c_str()) == 0 )
	return live;
    var e;
    rev_handle_of(e, w, es, adoc, rv.c_str());
    if ( e["handle"].is_null() )
	return live;
    is_rev = true;
    return e["handle"].as_integer();
}

// The text entity a handle was parsed from: the live doc, or a revision's.
long handle_text_ent(long w, long adoc, long live, long h)
{
    if ( h == live )
	return adoc;
    var e;
    rev_entry_of_handle(e, w, adoc, h);
    if ( e.is_null() )
	return adoc;
    return e["ent"].as_integer();
}

// ---- the verbs ------------------------------------------------------------

// graph.source: a node's text = the span's bytes in ITS handle's text entity.
void graph_source(var &out, long w, long adoc, long live, long h, long id)
{
    var sp;
    madc::graph_span(sp, h, id);
    var span = sp["span"];
    if ( span.is_null() )
    {
	out = { "error": sp["error"] };
	return;
    }
    long ent = handle_text_ent(w, adoc, live, h);
    long at = line_start_of(w, ent, span["line"].as_integer()) + span["column"].as_integer();
    long end = line_start_of(w, ent, span["end_line"].as_integer()) + span["end_column"].as_integer();
    var t;
    ui::text(t, w, ent);
    var text = perl::substr(t, at, end - at);
    var node = sp;
    out = { "node": node, "text": text };
}

// graph.status: what this asset can answer, in one call.
void graph_status(var &out, long w, long es, long adoc, long live)
{
    var t;
    ui::text(t, w, adoc);
    var pt;
    ui::get(pt, w, adoc, "phandle_text");
    bool synced = !pt.is_null() && pt == t;
    var diags;
    madc::parse_check(diags, live);
    long errors = 0;
    for ( var d : diags )
	if ( d["severity"] == "error" )
	    errors = errors + 1;
    var revs;
    revs_of(revs, w, adoc);
    var revrows = {};
    for ( var e : revs )
	if ( !e.is_null() )
	{
	    var row = { "sha": e["sha"], "tag": e["tag"] };
	    revrows.push(row);
	}
    long gh = git_handle_of(w, es, adoc);
    var git = null;
    // The asset's layers as the design §3.9 wire words. A PREVIEW: the served
    // doc is a parse handle, so it is lexable + parseable; versioned when a
    // repository is above it. L4e replaces this with asset_layers_of (managed,
    // executable, testable need the manifest and the node's offers).
    var layers = {};
    layers.push("text");
    layers.push("lexable");
    layers.push("parseable");
    if ( gh > 0 )
    {
	var hd;
	madc::git_head(hd, gh);
	var rel;
	git_relpath_of(rel, w, adoc);
	var dt;
	madc::git_dirty(dt, gh, rel.c_str());
	var dirty = dt["dirty"];
	git = { "head": hd["sha"], "branch": hd["branch"], "detached": hd["detached"],
		"dirty": dirty, "path": rel };
	layers.push("versioned");
    }
    long gen = madc::parse_generation(live);
    long head = clog_head(w, adoc);
    out = { "handle": live, "generation": gen, "synced": synced, "errors": errors,
	    "log_head": head, "revisions": revrows, "git": git, "layers": layers };
}

// graph.commits: the commits touching the served file, newest first.
void graph_commits(var &out, long w, long es, long adoc, long limit)
{
    long gh = git_handle_of(w, es, adoc);
    if ( gh <= 0 )
    {
	out = { "error": "no repository above the served file" };
	return;
    }
    var rel;
    git_relpath_of(rel, w, adoc);
    madc::git_log(out, gh, rel.c_str(), limit > 0 ? limit : 20);
}

// graph.revision: open (or reuse) the revision handle; answer its identity.
void graph_revision(var &out, long w, long es, long adoc, const char *spec)
{
    var e;
    rev_handle_of(e, w, es, adoc, spec);
    if ( e["handle"].is_null() )
    {
	out = { "error": e["error"] };
	return;
    }
    out = { "sha": e["sha"], "tag": e["tag"] };
}

// The {kind+name -> node} index of a handle's symbols.
void symbol_index(var &out, long h)
{
    var sy;
    madc::graph_symbols(sy, h);
    out = {};
    var nodes = sy["nodes"];
    if ( nodes.is_null() )
	return;
    for ( var n : nodes )
    {
	var key = format("{}\t{}", n["kind"], n["name"]);
	out[key.c_str()] = n;
    }
}

// graph.diff at declaration granularity: symbols keyed by kind+name; changed =
// the node's text differs. "" = the live buffer.
void graph_diff(var &out, long w, long es, long adoc, long live, const char *a0, const char *b0)
{
    var a = a0;
    var b = b0;
    long ha = live;
    long hb = live;
    if ( strlen(a.c_str()) > 0 )
    {
	var ea;
	rev_handle_of(ea, w, es, adoc, a.c_str());
	if ( ea["handle"].is_null() ) { out = { "error": ea["error"] }; return; }
	ha = ea["handle"].as_integer();
    }
    if ( strlen(b.c_str()) > 0 )
    {
	var eb;
	rev_handle_of(eb, w, es, adoc, b.c_str());
	if ( eb["handle"].is_null() ) { out = { "error": eb["error"] }; return; }
	hb = eb["handle"].as_integer();
    }
    var ia;
    symbol_index(ia, ha);
    var ib;
    symbol_index(ib, hb);
    var added = {};
    var removed = {};
    var changed = {};
    for ( var key : php::array_keys(ia) )
    {
	var na = ia[key.c_str()];
	var nb = ib[key.c_str()];
	if ( nb.is_null() )
	{
	    removed.push(na);
	    continue;
	}
	var sa;
	graph_source(sa, w, adoc, live, ha, na["id"].as_integer());
	var sb;
	graph_source(sb, w, adoc, live, hb, nb["id"].as_integer());
	var ta = sa["text"];
	var tb = sb["text"];
	if ( !(ta == tb) )
	{
	    var pair = { "a": na, "b": nb };
	    changed.push(pair);
	}
    }
    for ( var key : php::array_keys(ib) )
	if ( ia[key.c_str()].is_null() )
	    added.push(ib[key.c_str()]);
    out = { "added": added, "removed": removed, "changed": changed };
}

// graph.history: GIT rows (blame of the live text, folded to commits) + EVENT
// rows (matching nodeops; intersecting splices walked back with the inverse
// anchor shift up to the most recent overlapping one). Live ids only: a
// revision's node has no session history and its git history IS the log.
void graph_history(var &out, long w, long es, long adoc, long live, long id)
{
    var sp;
    madc::graph_span(sp, live, id);
    var span = sp["span"];
    if ( span.is_null() )
    {
	out = { "error": sp["error"] };
	return;
    }
    long l0 = span["line"].as_integer();
    long l1 = span["end_line"].as_integer();
    long a = line_start_of(w, adoc, l0) + span["column"].as_integer();
    long b = line_start_of(w, adoc, l1) + span["end_column"].as_integer();
    var rows = {};
    // GIT
    long gh = git_handle_of(w, es, adoc);
    if ( gh > 0 )
    {
	var rel;
	git_relpath_of(rel, w, adoc);
	var t;
	ui::text(t, w, adoc);
	var bl;
	madc::git_blame_text(bl, gh, rel.c_str(), t.c_str(), l0, l1 - l0 + 1);
	var seen = {};
	var hunks = bl["rows"];
	if ( !hunks.is_null() )
	    for ( var h : hunks )
	    {
		var sha = h["sha"];
		if ( strlen(sha.c_str()) == 0 || !seen[sha.c_str()].is_null() )
		    continue;		// uncommitted (the event rows cover it) or already listed
		seen[sha.c_str()] = 1;
		var row = { "provenance": "git", "sha": sha, "author": h["author"],
			    "when": h["when"], "summary": h["summary"] };
		rows.push(row);
	    }
    }
    // EVENT: walk the doc's log newest -> oldest
    bool complete = true;
    long le = es_int(w, adoc, "changelog", 0);
    if ( le > 0 )
    {
	var blob;
	ui::text(blob, w, le);
	var lines;
	php::explode(lines, "\n", blob.c_str());
	var recs = {};
	for ( var ln : lines )
	{
	    if ( strlen(ln.c_str()) == 0 )
		continue;
	    var rec;
	    if ( js::parse(rec, ln) )
		recs.push(rec);
	}
	var evrows = {};
	bool walking = true;
	for ( long i = php::count(recs) - 1; i >= 0; i = i - 1 )
	{
	    var rec = recs[i];
	    if ( rec["doc"].as_integer() != adoc )
		continue;
	    long ck = clog_kind_of(rec);
	    if ( ck == ckSPLICE && walking )
	    {
		long at = rec["at"].as_integer();
		long del = rec["del"].as_integer();
		long ins = strlen(rec["ins"].c_str());
		if ( at + ins <= a )
		{   // entirely before the span: undo its shift
		    a = a - (ins - del);
		    b = b - (ins - del);
		}
		else if ( at < b )
		{   // intersects: the most recent text edit touching this node
		    var row = { "provenance": "event", "seq": rec["seq"], "ts": rec["ts"],
				"es": rec["es"], "verb": "splice", "at": at, "del": del, "ins": ins };
		    evrows.push(row);
		    walking = false;	// earlier coordinates are not derivable
		    complete = false;
		}
	    }
	    else if ( ck == ckOTHER && nexus_kind_of(rec) == nkNODEOP )
	    {
		var tgt = rec["target"];
		if ( !tgt.is_null() && tgt["kind"] == sp["kind"] && tgt["name"] == sp["name"] )
		{
		    var row = { "provenance": "event", "seq": rec["seq"], "ts": rec["ts"],
				"es": rec["es"], "verb": rec["verb"] };
		    evrows.push(row);
		}
	    }
	}
	// oldest first, after the git rows
	for ( long i = php::count(evrows) - 1; i >= 0; i = i - 1 )
	    rows.push(evrows[i]);
    }
    var node = sp;
    out = { "node": node, "rows": rows, "complete": complete };
}
```

(`php::array_keys` — confirm the public exists in `include/madc/ns_php`; if it
does not, iterate the object with `for ( var kv : ia )` and take the key via
the object-iteration idiom the L1 tests use, and record which. `null` as a var
literal: confirm against `tmp/` first; else use `var none; revs[...] = none;`.)

- [ ] **Step 3: `madcide_mcp.inc`** — at the top, after its header comment:
  `#include "madcide_past.inc"`. Descriptors after `t18`:

```c
    // Code-graph MCP L4b (design 2026-09-13 §4.3): the PAST verbs. `rev` is
    // any rev-parse spec or a sha; "" = the live buffer.
    var revprop = { "type": "string", "description": "a git revision (rev-parse spec or sha); \"\" = the live buffer" };
    var limitprop = { "type": "integer", "description": "at most this many rows (optional; default 20)" };
    var revprops = { "rev": revprop };
    var revargs = { "type": "object", "properties": revprops };
    var limitprops = { "limit": limitprop };
    var limitargs = { "type": "object", "properties": limitprops };
    var diffprops = { "a": revprop, "b": revprop };
    var diffargs = { "type": "object", "properties": diffprops };
    var t19 = { "name": "graph.status", "code": gvSTATUS,
	"description": "the served asset in one call: handle, generation, synced, error count, log head, open revisions, git head/branch/dirty, layers",
	"inputSchema": noargs };
    tools[] = t19;
    var t20 = { "name": "graph.source", "code": gvSOURCE,
	"description": "a node's source text (live or revision id)", "inputSchema": idargs };
    tools[] = t20;
    var t21 = { "name": "graph.history", "code": gvHISTORY,
	"description": "who changed this node and when: git commits (blame) + session events (node-ops, text edits), provenance-tagged",
	"inputSchema": idargs };
    tools[] = t21;
    var t22 = { "name": "graph.commits", "code": gvCOMMITS,
	"description": "the commits touching the served file, newest first", "inputSchema": limitargs };
    tools[] = t22;
    var t23 = { "name": "graph.revision", "code": gvREVISION,
	"description": "open the served file at a git revision as a read-only handle; its ids route themselves to it",
	"inputSchema": revargs };
    tools[] = t23;
    var t24 = { "name": "graph.diff", "code": gvDIFF,
	"description": "declaration-level diff between two revisions (\"\" = live): added / removed / changed symbols",
	"inputSchema": diffargs };
    tools[] = t24;
```

`graph_call`: after the tier gate and before the switch —

```c
    // L4b routing (design §3.3): an id names its handle; an id-less read may
    // name a revision. A mutation aimed at a revision refuses — revision
    // handles are read-only.
    bool is_rev = false;
    long hh = route_handle(w, es, adoc, h, args, is_rev);
    if ( is_rev && graph_min_tier(gv) >= tierEDITOR )
    {
	var item = { "type": "text", "text": "revision handles are read-only" };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
```

then every existing `case` that passes `h` passes `hh` instead (the L1/L1b/L2
reads and `gvSPAN`/`gvAT`; the mutations keep `S`), and the six new cases:

```c
    case gvSTATUS:   graph_status(snap, w, es, adoc, h); break;
    case gvSOURCE:   graph_source(snap, w, adoc, h, hh, args["id"].as_integer()); break;
    case gvHISTORY:  graph_history(snap, w, es, adoc, h, args["id"].as_integer()); break;
    case gvCOMMITS:
    {
	var lv = args["limit"];
	graph_commits(snap, w, es, adoc, lv.is_null() ? 20 : lv.as_integer());
	break;
    }
    case gvREVISION: graph_revision(snap, w, es, adoc, args["rev"].c_str()); break;
    case gvDIFF:
    {
	var av = args["a"];
	var bv = args["b"];
	var a = av.is_null() ? "" : av;
	var b = bv.is_null() ? "" : bv;
	graph_diff(snap, w, es, adoc, h, a.c_str(), b.c_str());
	break;
    }
```

(`args["rev"].c_str()` on a null var — confirm it yields "" rather than
throwing; else guard as `a`/`b` are.)

- [ ] **Step 4: Gates**: `bash scripts/check-dialect-literals.sh`,
  `bash scripts/check-dialect-lean.sh`, `bash scripts/check-madcide-single-owners.sh`,
  `bash scripts/check-madcide-enums.sh` GREEN; `testmadcide_serve_edit` 30/30,
  `testmadcide_serve_graph`, `testmadcide_serve_mcp` unchanged (the routed
  handle equals the live one when no `rev`/revision id is involved).

- [ ] **Step 5: Commit** (dialect: no trailers) —
  `feat(graph-mcp): L4b PAST verbs — graph.status / source / history / commits / revision / diff; revision handles by generation tag routed in graph_call; read-only refusal for a mutation on a revision id`.

---

### Task 5: `tests/testgraphpast.mad` — the six verbs through the seat over this repository

**Files:** create `tests/testgraphpast.mad`, `.expect`, `.expect_quiet`.

- [ ] **Step 1: The test** (the `call_tool` helper of `testmadcide_serve_edit.mad`,
  the fixture `tests/testint.mad` — tracked, clean, ancient):

```c
// Nexus L4b through the SEAT: the PAST verbs over the repository this test runs
// in. Git rows by SHAPE (history moves), event rows and the diff EXACTLY (an
// in-session edit never touches the disk).
#include "../tools/texteditor/lined_core.inc"
#include "../tools/texteditor/editor_events.inc"
#include "../tools/madcide/madcide_core.inc"
#include "../tools/madcide/madcide_client.inc"
#include "../tools/madcide/madcide_api.inc"
#include "../tools/madcide/madcide_mcp.inc"

bool call_tool(var &snap, bool &iserr, IdeSession &S, long doc, long id, const char *name, var &args)
{
    var params = { "name": name, "arguments": args };
    var req = { "jsonrpc": "2.0", "id": id, "method": "tools/call", "params": params };
    var resp;
    mcp_handle(S, doc, -1, req, resp);
    var result = resp["result"];
    iserr = result["isError"].as_boolean();
    var content = result["content"];
    var item0 = content[0];
    var txt = item0["text"];
    return js::parse(snap, txt.c_str());
}

int main()
{
    IdeSession S;
    long doc = S.open("tests/testint.mad", false);
    if ( doc <= 0 ) { println(stderr, "cannot open tests/testint.mad"); return 1; }
    S.terminal(false);
    var snap;
    bool err;
    var none = {};

    call_tool(snap, err, S, doc, 1, "graph.status", none);
    println("status: synced={} errors={} git={} branch-set={} layers={}",
	    snap["synced"].as_boolean() ? 1 : 0, snap["errors"],
	    snap["git"].is_null() ? 0 : 1,
	    strlen(snap["git"]["branch"].c_str()) > 0 ? 1 : 0, php::count(snap["layers"]));

    call_tool(snap, err, S, doc, 2, "graph.commits", none);
    var c0 = snap["rows"][0];
    println("commits: rows-positive={} sha-len={}", php::count(snap["rows"]) > 0 ? 1 : 0, strlen(c0["sha"].c_str()));

    var revargs = { "rev": "HEAD" };
    call_tool(snap, err, S, doc, 3, "graph.revision", revargs);
    long tag = snap["tag"].as_integer();
    var sha = snap["sha"];
    println("revision: sha-len={} tag-tagged={}", strlen(sha.c_str()), tag >= 1048576 ? 1 : 0);

    call_tool(snap, err, S, doc, 4, "graph.symbols", revargs);
    var rn = snap["nodes"][0];
    long rid = rn["id"].as_integer();
    println("rev-symbols: name={}", rn["name"]);

    var idargs = { "id": rid };
    call_tool(snap, err, S, doc, 5, "graph.node", idargs);
    println("rev-id-routes: name={} err={}", snap["name"], err ? 1 : 0);
    var repargs = { "id": rid, "src": "int main() { return 1; }" };
    call_tool(snap, err, S, doc, 6, "graph.replace", repargs);
    println("rev-mutation-refused: {}", err ? 1 : 0);

    var diffargs = { "a": "HEAD", "b": "" };
    call_tool(snap, err, S, doc, 7, "graph.diff", diffargs);
    println("diff-clean: added={} removed={} changed={}",
	    php::count(snap["added"]), php::count(snap["removed"]), php::count(snap["changed"]));

    call_tool(snap, err, S, doc, 8, "graph.symbols", none);
    var ln = snap["nodes"][0];
    long lid = ln["id"].as_integer();
    var srcargs = { "id": lid };
    call_tool(snap, err, S, doc, 9, "graph.source", srcargs);
    var text = snap["text"];
    println("source: starts-int-main={}", php::strpos(text, "int main") == 0 ? 1 : 0);

    // edit main in-session: insert a statement after its first statement
    var atargs = { "line": 8, "column": 4 };
    call_tool(snap, err, S, doc, 10, "graph.at", atargs);
    var stmt = snap["id"];
    var insargs = { "id": stmt, "where": "after", "src": "puti(7);" };
    call_tool(snap, err, S, doc, 11, "graph.insert", insargs);
    println("insert: ok={}", snap["ok"].as_boolean() ? 1 : 0);

    call_tool(snap, err, S, doc, 12, "graph.diff", diffargs);
    var ch = snap["changed"][0];
    println("diff-after-edit: added={} removed={} changed={} name={}",
	    php::count(snap["added"]), php::count(snap["removed"]), php::count(snap["changed"]), ch["b"]["name"]);

    call_tool(snap, err, S, doc, 13, "graph.symbols", none);
    var ln2 = snap["nodes"][0];
    var hargs = { "id": ln2["id"] };
    call_tool(snap, err, S, doc, 14, "graph.history", hargs);
    long gitrows = 0;
    long evrows = 0;
    long splices = 0;
    for ( var r : snap["rows"] )
    {
	if ( r["provenance"] == "git" ) gitrows = gitrows + 1;
	if ( r["provenance"] == "event" ) { evrows = evrows + 1; if ( r["verb"] == "splice" ) splices = splices + 1; }
    }
    println("history: git-rows-positive={} event-rows={} splice-rows={} complete={}",
	    gitrows > 0 ? 1 : 0, evrows, splices, snap["complete"].as_boolean() ? 1 : 0);

    call_tool(snap, err, S, doc, 15, "graph.status", none);
    println("status-after: synced={} log_head={} revisions={}",
	    snap["synced"].as_boolean() ? 1 : 0, snap["log_head"], php::count(snap["revisions"]));
    return 0;
}
```

`tests/testgraphpast.expect`:

```
status: synced=1 errors=0 git=1 branch-set=1 layers=4
commits: rows-positive=1 sha-len=40
revision: sha-len=40 tag-tagged=1
rev-symbols: name=main
rev-id-routes: name=main err=0
rev-mutation-refused: 1
diff-clean: added=0 removed=0 changed=0
source: starts-int-main=1
insert: ok=1
diff-after-edit: added=0 removed=0 changed=1 name=main
history: git-rows-positive=1 event-rows=1 splice-rows=1 complete=0
status-after: synced=1 log_head=3 revisions=1
```

(`log_head=3`: the insert logs one checkpoint? — no: the log records are the
nodeop + the splice(s); `graph_edit_apply` appends the nodeop record, then
`ed_text_insert` one splice → head 2; `edit_checkpoint` is the undo stack, not
the log. VERIFY on the first run and pin the true value with the reason in a
comment. `tests/testint.mad` line 8 is `    puti(123);` — confirm the fixture's
line before pinning `atargs`.) `tests/testgraphpast.expect_quiet`: one line.

- [ ] **Step 2: Run** `bash tmp/l3_check.sh tests/testgraphpast.mad` → 0 missing,
  stderr empty. Also `git status --short tests/testint.mad` → clean (the edit
  never touched the disk).

- [ ] **Step 3: Commit** — `test(graph-mcp): L4b PAST verbs through the seat over the repository itself (tests/testgraphpast)`.

---

### Task 6: Docs, hand-off

- [ ] **Step 1**: design doc §9 — L4b row marked shipped with its contents; the
  "project-scoped stream + `clog_kind`" entry becomes "`clog_kind` (L4b) /
  project-scoped stream (L4c)"; §3.1 gets one sentence: the scoping lands in
  L4c with its first cross-document consumer.
- [ ] **Step 2**: `claude_status.json` live_handoff UPDATE 13 (L4b shipped;
  NEXT = writing-plans L4c: project-scoped stream, tierPROPOSER,
  `parse_would_accept`, proposal + decision records, the five proposal verbs,
  the connection-level wiring test); the L4 ledger; memory.
- [ ] **Step 3**: `git push origin feature/client-server-views-claude`.

---

## Self-review

- **Spec coverage**: §3.3 tags/routing/read-only → Task 1; §4.3 six verbs →
  Task 4 (each names its structure); §3.1 enum at the reader → Task 3; §3.7
  provenance on rows → Task 4 (`graph_history`); §5 flows 1–2 → Tasks 4/5;
  §6 refusals (no repo, rev unknown, revision mutation, unknown id → stale)
  → Tasks 1/4; §7 contracts stated in the `.inc` header + engine comments;
  §8 L4b tests → Tasks 1/2/5 (`testgraphtagged`, unit cases, `testgraphpast`).
  Deferred from the design's L4b row: the project-scoped stream (→ L4c,
  decision 1), the two-doc changelog case (rides with it).
- **Placeholders**: the three "confirm" notes (`php::array_keys`, a `null`
  literal, `.c_str()` on a null var, the fixture's line 8 / `log_head`) are
  verification steps with their fallback named, not open work.
- **Type consistency**: `route_handle(...) -> long` + `bool &is_rev` used by
  `graph_call`; `rev_handle_of` answers `{sha, tag, handle, ent}` consumed by
  `graph_revision`, `graph_diff`, `route_handle`; `graph_source(out, w, adoc,
  live, h, id)` called by `graph_call` and `graph_diff` with the same order;
  engine publics match their bridges (`std::string *` text, `int64_t` ids).
