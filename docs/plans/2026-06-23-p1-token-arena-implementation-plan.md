# P1 token arena — IMPLEMENTATION PLAN + CONTINUATION CONTRACT

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **HEAD at write:** `7d6bc31`
**Read this BEFORE acting. Imperative, not advisory. Do not re-scope, re-derive, or
re-litigate the design in §1 — it was reached after auditing the real code and it
resolves the polymorphism fence the earlier contract worried about.**

---

## 0. SETTLED — current state (verify with `git log --oneline -8`, don't re-measure to "discover")

**Front-end perf work landed this session (newest first):**
- `7d6bc31` P0 step 3 — flat sid-indexed maps (`InternKeyedMap` = dense value pool +
  sid-indexed `int32` slot array, O(1), no tree) + incremental hash folded into the
  identifier read loop. keyword/define/macro/cpp-operator string-map trees GONE.
- `42c1cee` P0 step 2 — intern identifier spellings at the lexer (`TokenIdent.spelling_id`).
- `1a26d38` P2 — `Source` backed by flat `char` buffer (no `std::stringstream`).
- `68d152b` P0 step 1 — `StringPool` (arena-model hashstr, `include/stringpool.h`).

**Measured NOW (testmap, -O0, `--std=c++17 --no-embedded-headers`, callgrind):**
- lex **0.375 s** (was 0.595 orig → **-37%**), parse ~0.78 s, total Ir **3.719 B**.
- Cost breakdown (use THESE, not the stale pre-P1/P2 "47% malloc" -O2 figures):
  - malloc family (`_int_malloc`+`malloc`+`_int_free`+`free`+`new`/`delete`) ≈ **7.5%**
    of Ir — ALL allocations (tokens are a *fraction*, alongside std::string/cir_node/vectors).
  - `TokenBase`/`TokenIdent` constructors ≈ **1.5%**.
  - `TokenIdent::clone()` inclusive ≈ **1.35%** (template-instantiation deep copy).
  - `datatype_map` (the one remaining string-keyed map) ≈ **1.75%** (`std::less<string>`
    0.64% + lower_bound/_S_key/end). **Left string-keyed on purpose** — it is iterated
    BY STRING KEY (`for(it=…begin();…) datatype_map[it->first]=it->second`, parser.cpp
    ~21155/21401) so the flat sid-array can't replace it without large churn. DO NOT
    convert it as part of P1.

**KEY CORRECTION (the thing that prompted this plan):** there is **NO `TokenRec` type
in the codebase** (only an aspirational comment in `stringpool.h`). `TokenStream::_buf`
(madc.h:1077) is **`std::vector<TokenBase*>` + cursor** — a flat array of POINTERS to
individually-`new`'d polymorphic objects. P1 step 1/2 (`c660eb6`,`3aaa537`) changed the
CONTAINER (deque→vector+cursor; the "43% deque-copy" is gone) but NOT the CONTENT. The
tokens are still scattered heap objects. `vector<TokenRec>` (inline POD records) does
not exist yet — building it is THIS plan.

**Architecture facts (audited 2026-06-23 — do not re-discover):**
- `TokenBase` (tokens.h:76) is polymorphic: virtual `~`, `clone`, `type`, `id`, `get`,
  `ival`, `dval`, `parse`, `datadef`, … Fields: `_token`,`_datatype`(DataDef*),`_flags`,
  `file`(const char*),`parent`,`line`,`column`,`pos`,`read_count`,`leading_trivia`(string).
- **115 `TokenBase` subclasses**; `parse(Program&)` is virtual and overridden on ~all of
  them — **the parser's recursive descent IS per-token-type virtual `parse()` dispatch.**
- **326 `dynamic_cast<TokenX*>`** sites across src (TokenVar 61, TokenMember 29, …).
- `compile()`/`operand()` (asmjit-era) appear REMOVED — only `parse()` + data accessors
  remain virtual. CONFIRM in Phase 0.
- Consequence: a *pure* flat POD `vector<TokenRec>` with switch-dispatch would be a
  **parser rearchitecture** (115 parse methods + 326 casts). That is the FENCED change.
  **This plan does NOT do that.** See §1.

---

## 1. THE DESIGN — data/behavior split (SETTLED; this is the spine)

Split the token's **data** (flat POD `TokenRec`) from its **behavior** (the polymorphic
`TokenBase` shell). Keep every virtual `parse()` and every `dynamic_cast` working — the
live object stays a real polymorphic `TokenBase`. Only move the *data* into a flat record
and the *storage* into a stable arena. This respects the fence (no polymorphism collapse,
zero `parse()`/`dynamic_cast` rewrites) AND yields a flat, serializable data array.

```cpp
struct TokenRec {                 // POD: every field an int/index — dumpable/mmappable
    uint16_t kind;                // which subclass (drives dispatch + shell rebuild on load)
    uint16_t flags;
    int64_t  value;               // ival / char code / token code
    uint32_t spelling_id;         // -> StringPool            (DONE: interning)
    uint32_t type_id;             // -> segmented type table  (replaces DataDef*)
    uint32_t file_id;             // -> interned files        (replaces const char*)
    int32_t  line, column;
    uint32_t first_child, child_count;  // -> arena slot-ids  (replaces vector<TokenBase*>)
    // small union for the few subclasses with extra scalar data (e.g. TokenReal double)
};
```

- The **shell** (`TokenBase` + 115 subclasses) holds behavior, reads/writes data through
  its `TokenRec`. `->id()`,`->type()`,`->parse()`,`dynamic_cast<TokenX*>` all UNCHANGED.
- **Live storage = stable slab arena**: fixed-size cells (`sizeof(largest shell)`),
  chunked, **chunks never relocate** so raw `TokenBase*` (held by `cir_node`, `parent`,
  children vectors) stay valid. One big alloc instead of ~100K `new`s. `clone()` = bump
  alloc + record copy. THIS is the perf win.
- **Serialization (later)**: once `TokenRec` is fully POD, dump `TokenRec[]` + string/type
  pools; on load `mmap` the records and **rebuild shells by `kind`** (placement-new the
  right subclass per cell). The vtable ptr is the only non-serializable thing and it is
  reconstructed from `kind`, never stored.

**Rejected alternatives (do not revisit):**
- *Pure flat POD + switch dispatch* — the parser rearchitecture (115 parse + 326 casts). Fenced.
- *Freelist `operator new` pool on TokenBase* — built and REVERTED this campaign (no -O0
  win; optimizes the pointer-object model). The arena here is a **bump/slab** (no per-alloc
  bookkeeping, bulk free, stable chunks), NOT a freelist pool.

**Hard invariant: POINTER STABILITY.** `cir_node`, `TokenBase::parent`, and every
`vector<TokenBase*>` hold raw pointers to token objects. The arena MUST NOT relocate a
live cell. Use chunked storage (vector of fixed chunks; never move a chunk), NEVER a
single reallocating `vector<cell>`.

---

## 2. PHASES (each its own commit; fulltest 669/0/0/18 + torture byte-identical between)

**Phase 0 — audit (no code change; ~30 min).**
- `sizeof` distribution of all 115 token subclasses (find the fat ones, e.g. `TokenCpnd`
  with member vectors). Decide cell size; if a few are huge, plan to move their data into
  the arena (children→slot-ids) so cells trend uniform-small. Record the numbers here.
- Confirm `compile()`/`operand()` are gone (grep `tokens.h`); list the live virtuals.
- Inventory who holds `TokenBase*` long-term (cir_node fields, parent, the
  `std::vector<TokenBase*>` members in madc.h) — these are the pointer-stability clients.
- Confirm token DESTRUCTION model: where are tokens freed today? (If never individually
  freed → arena bulk-free is a clean drop-in. If freed individually → audit those sites.)

**Phase 1 — stable slab arena under the existing polymorphic tokens (THE perf win).**
- Add a `TokenArena` (chunked bump allocator; fixed cell size = max token `sizeof`;
  chunks never move; bulk `reset()` frees all). One per `Program` (or per compile).
- Route token creation through it: a `Program::new_token<T>(args…)` (placement-new into a
  cell) replacing the `new TokenX(...)` sites in the lexer/parser. Start with the LEXER
  hot path (`_getToken`'s `new TokenIdent` etc.), then the parser's synthetic tokens.
- Keep `TokenBase*` everywhere — NO call-site/dispatch changes. `clone()` allocates from
  the arena.
- Gate: fulltest 669/0/0/18; torture byte-identical; `--emit=c11` byte-identical;
  callgrind perf-oracle (malloc/`_int_malloc`/`_int_free` share drops; confirm the token
  ctors are now arena bump-allocs). Expect modest -O0 (~3-5%), more at -O2.
- NOTE the destruction change: tokens now die with the arena, not via `delete`. Make sure
  nothing `delete`s a token (Phase 0 inventory).

**Phase 2 — introduce `TokenRec` as the shell's data member; flatten fields incrementally.**
Each field its own commit, fulltest between. ORDER:
  1. `spelling_id` — already on `TokenIdent`; generalize into `TokenRec` (DONE-ish).
  2. `type_id` — replace `DataDef* _datatype` reads via the segmented type table
     (`docs/plans/2026-06-12-type-table-value-abi-design.md`). Keep `DataDef*` resolvable
     from `type_id` during transition.
  3. `file_id` — replace `const char* file` with an index into `interned_files`.
  4. `first_child`/`child_count` — convert the per-subclass `vector<TokenBase*>` children
     to arena slot-id ranges. (Biggest sub-step; shrinks fat cells.)
- Gate per sub-step: fulltest green + `--emit=c11` byte-identical (data moved, emission
  must not change).

**Phase 3 — serialization (ties to the embedded-header forest, NOT a perf item).**
- Dump `TokenRec[]` + `StringPool` + type table + file table; on load mmap + rebuild
  shells by `kind`. This is the forest/PCH keystone
  (`docs/plans/2026-06-22-embedded-header-forest-execution-plan.md`,
  `docs/plans/2026-06-13-embedded-ast-frontend-design.md`). Schedule with that track.

---

## 3. Authoritative designs to build AGAINST (do not write a competing plan)
- `docs/plans/2026-06-13-embedded-ast-frontend-design.md` — arena + u32 index handles,
  cir_node backbone, segmented mmap. THE architecture.
- `docs/plans/2026-06-12-type-table-value-abi-design.md` — segmented `u32` type-id table
  (the `type_id` source in Phase 2.2).
- `docs/plans/2026-06-09-frontend-representation-refactor.md` — phased P0..P5 ordering.
- `docs/plans/2026-06-23-arena-interning-HANDOFF.md` — the interning contract (P0 steps;
  step 4 = drop per-token `std::string str` once readers use `spelling(spelling_id)` —
  this composes with Phase 2.1 here).

## 4. Process rules (non-negotiable)
- Commit via `git commit -F -` heredoc (NOT `-m` w/ backticks). Stage files EXPLICITLY —
  never `git add -A`. Leave `mir-debug-support.md` UNTRACKED (not ours).
- Commit trailers:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_015eGuYph7nFzyq2e8vc9B1Y`.
- `-O0` is the dev default (optimization = last lap; -O2 re-measure is a valid task to
  cash structural wins). Test cap 10s. One heavy job at a time; cap every run
  `( ulimit -t N; timeout M … )`. No poll-loop sleeps. Background long builds/tests.
- `make -C src fulltest` after every change. Perf-oracle: re-callgrind testmap and confirm
  the targeted cost center dropped (`scripts/perf_vs_gcc.sh`, baseline in
  `docs/parity/perf-baseline.tsv`).
- Memory: `project_frontend_performance.md` is the active-track file; keep it synced.

## 5. HONEST ROI (do not oversell)
P1's *raw -O0 speed* payoff is **modest** (~3-5%): the big deque-copy is already gone, and
the malloc share at -O0 is ~7.5% with tokens only a fraction. It is bigger at -O2. The
REAL driver for the flat `TokenRec` is **serialization** (the forest/PCH/mmap keystone),
not front-end speed. Phase 1 (slab arena) is the self-contained perf slice; Phases 2-3 are
the serialization investment. If the user wants a quick perf check instead, the highest-
value cheap move is **re-measure at -O2** to cash P2+interning (they compound under
inlining) before committing to Phases 2-3.

## 6. Rehydrate
`scripts/resume.sh`; read THIS file fully; `git log --oneline -8` to confirm HEAD;
then start at Phase 0. Do not begin Phase 1 edits before the Phase 0 audit numbers are in.
