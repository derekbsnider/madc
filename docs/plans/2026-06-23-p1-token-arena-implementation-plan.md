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
- **Live storage = stable bump arena**: chunked, **chunks never relocate** so raw
  `TokenBase*` (held by `cir_node`, `parent`, children vectors) stay valid. One big alloc
  instead of ~100K `new`s. `clone()` = bump alloc + record copy. THIS is the perf win.
  **NOTE (Phase 0 refinement):** allocation is **VARIABLE-SIZE** (exactly `sizeof(T)`,
  aligned), NOT fixed `sizeof(largest)` cells — the user's "no per-token deletion" rule
  removes the only reason for uniform cells (freelist interchange). See §2 Phase 0 RESULTS.
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

**Phase 0 — RESULTS (2026-06-23, HEAD `9c9e7e2`). AUDIT DONE. Cleared to start Phase 1.**
- **0.1 virtuals:** `compile()`/`operand()` CONFIRMED removed (only matches are member
  vars `TokenDynamicCast::operand`/`TokenTypeid::operand` + a comment). Live virtuals:
  `~`,`clone`,`set`,`setDataType`,`setFlag`,`is_*`,`inc/dec`,`get/ival/dval`,`argc`,
  `type`,`id`,`datatype`,`datadef`,`associativity`,`parse(Program&)` (+ operator-only:
  `precedence`,`ioperate`,`foperate`,`assoc`).
- **0.2 sizeof (clang++ -std=c++11, via tmp/sizeof_probe.cpp):** base 112. Lexer hot-path
  cluster **112–168** (TokenSymbol/Char/Stmt 112, TokenOperator/Primary 136, TokenInt 144,
  TokenIdent/Str/Keyword 152, TokenMultiOp 168). Parser-synth fat **176–368** (StructLit
  176, Decl 192, CallFunc 216, Member/CallMethod 240, Cpnd 312, Func 328, Program 368).
  Max 368 = TokenProgram (≈singleton). Spread base→max = 3.3×.
- **0.3 inheritance:** DIAMOND virtual inheritance — `TokenBase ←virtual← TokenVar`
  (datatokens.h:222) and `←virtual← TokenCpnd` (madc.h:315); `TokenFunc : TokenVar,
  TokenCpnd` joins. Shells carry vtable + vbase ptrs ⇒ `TokenRec` MUST be a **data member
  (composition), not a base**. (Confirms §2 Phase 2; no plan change.)
- **0.4 pointer-stability clients (raw `TokenBase*` held long-term):** `cir_node`,
  `TokenBase::parent`, `TokenStream::_buf`/`_pushback`, `_prv_token`/`_cur_token`, ~15
  `std::vector<TokenBase*>` members (body, deferred, parameters, init_list, ctor_args,
  inits, args, member_template_decl/return_tokens, spec_pattern, constraint, …), and
  per-subclass `expr`/`index`/`base_expr`/`parent_expr`. ⇒ chunked, never-relocate. HOLDS.
- **0.5 destruction model:** tokens mostly NOT individually freed (live for the compile;
  arena-friendly). Only 3 explicit `delete` sites — lexer.cpp:1000 (adjacent string-literal
  merge frees the absorbed token), parser.cpp:3033 & 3045 (`__integer_pack` expansion frees
  consumed pattern tokens) — plus 2 commented-out "delete tb?" (parser.cpp:17593, 20113).

**Phase 0 → DESIGN REFINEMENT (user directive 2026-06-23: "no deletion — once a token goes
into the arena it is there permanently").** This SUPERSEDES the "fixed-size cells
(`sizeof(largest)`=368)" wording in §1:
- The arena is a **VARIABLE-SIZE BUMP allocator**: allocate exactly `sizeof(T)` (8/16-aligned),
  bump, NEVER reclaim a single token, bulk-free the whole chunk set at `reset()`. Uniform
  cells were only needed for a freelist (interchangeable freed cells); with no individual
  free they are pure waste (368-cell vs 152 ident ≈ 2.3× bloat over ~100K tokens). Variable
  bump is both denser AND simpler (no cell-size constant, no size-class routing).
- The 3 `delete` sites become no-ops once their tokens are arena-allocated (absorbed/consumed
  tokens simply remain in the arena until reset). Retire them in Phase 1 alongside routing.

**Phase 1 — stable slab arena under the existing polymorphic tokens (THE perf win).**
- Add a `TokenArena` (chunked bump allocator; fixed cell size = max token `sizeof`;
  chunks never move; bulk `reset()` frees all). One per `Program` (or per compile).
- Route token creation through it: a `Program::new_token<T>(args…)` (placement-new into a
  cell) replacing the `new TokenX(...)` sites in the lexer/parser. Start with the LEXER
  hot path (`_getToken`'s `new TokenIdent` etc.), then the parser's synthetic tokens.
- Keep `TokenBase*` everywhere — NO call-site/dispatch changes. `clone()` allocates from
  the arena.
- **Gate = CORRECTNESS ONLY (no perf gate — see below):** fulltest 669/0/0/18; torture
  byte-identical; `--emit=c11` byte-identical. Callgrind is INFORMATIONAL, not a gate.
  Per `fc59be0`, the arena under polymorphic tokens shows ~0% at -O0 — that is EXPECTED
  and NOT a failure; this phase is the structural FOUNDATION for the flatten/serialize
  payoff in Phases 2-3, not a standalone -O0 speedup. Do not stop or re-evaluate on the
  flat -O0 number; proceed through the full plan.
- NOTE the destruction change: tokens now die with the arena, not via `delete`. Make sure
  nothing `delete`s a token (Phase 0 inventory: lexer.cpp:1000, parser.cpp:3033/3045 →
  these become no-ops once tokens are arena-allocated).

**Phase 2 — introduce `TokenRec` as the shell's data member; flatten fields incrementally.**
Each field its own commit, fulltest between.

**REORDER (decided 2026-06-23, user: "order is not as important as ensuring the plan is
executed to completion"; HEAD @8bb60a2). Rationale grounded in the audited type table.**
The segmented type table is already BIDIRECTIONAL — `DataDef::type_id` (plain field,
no `Program` needed), forward `Program::type_id_for(dd)`, reverse `Program::type_from_id(id)`
(parser.cpp:8995-9067). So a token's type/file are ALREADY recoverable as stable indices.
Consequence for a PERF track:
  - `type_id`/`file_id` are **serialize-time derivations**, NOT live-rep changes. Keep
    `_datatype` (one pointer deref, no `Program`) and `file` as the SHELL's live resolved
    values (the data/behavior split: shell holds live pointers, record holds indices). A
    pre-dump pass materializes `tok->rec.type_id = type_id_for(tok->_datatype)` etc. with
    `Program` in scope. Ripping `_datatype` reads out into `type_from_id` lookups would ADD
    hot-path indirection + force `Program`-threading into `datadef()`/`setDataType()` for
    ZERO benefit before serialization — wrong for this track. → these MOVE to Phase 3.
  - The genuine Phase-2 LIVE-REP change is children→slot-ids, AND it is the high-value one
    (enables no-clone id-vector substitution → retires the 109 `clone()` sites). Do it next.

Revised step list:
  1. `spelling_id` — DONE @8bb60a2 (TokenRec introduced; field migrated onto the base record).
  1b. indexable arena (slot-id ↔ token bridge) — DONE @45d2b88 (`TokenRec.slot_id`,
     `TokenArena` slot registry, `madc_slot_id_for`/`madc_token_for_slot`, test_token_arena).
  1c. `cir_node::origin` → `origin_id` (slot-id) — DONE @617a5fa. First id-vector consumer +
     first of "all pointer classes become indices". READ-ONLY provenance ⇒ safe (no mutation
     of the referenced token). Proved the slot registry under production load (669 compiles).
  2. **children → slot-ids + no-clone substitution — THE deep step. BLOCKED on an
     architectural sub-project (see audit below).** Converting the macro/template body
     vectors to shared id-vectors is the ~43% instantiate-cost win, but it is NOT a simple
     field swap.
  3. `type_id`, `file_id` — RELOCATED to Phase 3 (dump-time materialization, below). The
     live rep keeps `_datatype`/`file`; the serializer writes the indices.
- Gate per sub-step: fulltest green + `--emit=c11` byte-identical (data moved, emission
  must not change).

**MUTATION-SAFETY AUDIT (2026-06-23, gating step 2) — CONCLUSION: no-clone needs
identity/occurrence separation.** The parser builds the parse tree by MUTATING consumed
tokens in place: `parent =` (25 sites — `lhs->parent = assign`, `expr->parent = assign`,
`ret_value->parent = ret`, parser.cpp:14360/14361/14369…) and the operator `left`/`right`
pointers. (`setDataType` — 37 sites — mostly targets parser-synthesized temporaries `ti`/
`np`/`zero`, NOT body tokens; `read_count` is a harmless diagnostic.) Today `clone()` is
what makes this safe: each macro/template instantiation re-parses INDEPENDENT cloned tokens,
so the in-place `parent`/`left`/`right` mutation can't collide. To drop the clone and SHARE
body tokens across instantiations, that mutable tree-structure state (`parent`/`left`/
`right`) must move OFF the shared token identity and INTO per-occurrence storage (the
identity-vs-occurrence split the flattening plan names). That is a substantial sub-project
spanning the ~25 `parent=` sites, the operator child pointers, and the substitution
machinery — sequence it on its own; do NOT attempt it as a single commit. Until it lands,
`clone()` (now arena-cheap, Phase 1) stays.

**Phase 3 — serialization (ties to the embedded-header forest, NOT a perf item).**
- **Pre-dump materialization pass (the relocated Phase-2 steps 2/3):** walk the tree with
  `Program` in scope and write each `TokenRec`'s index fields from the shell's live values —
  `rec.type_id = type_id_for(tok->_datatype)`, `rec.file_id = intern(tok->file)`. The live
  rep is unchanged; only the on-disk record is fully index-based.
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
- `make -C src fulltest` after every change (CORRECTNESS gate). Callgrind/`perf_vs_gcc.sh`
  is INFORMATIONAL only — record numbers, never gate a phase on them (§5).
- Memory: `project_frontend_performance.md` is the active-track file; keep it synced.

## 5. ROI — the payoff is SERIALIZATION, not -O0 speed (do not oversell, do not perf-gate)
DIRECTIVE (user, 2026-06-23): implement the FULL plan (1→2→3) through serialization. Do NOT
gate any phase on a perf number; the gate is correctness + byte-identical output.
- Phase 1 (arena under polymorphic tokens) shows **~0% at -O0** — MEASURED and reverted once
  as a standalone slice (`fc59be0`). That is EXPECTED. Phase 1 is the FOUNDATION (a stable,
  never-relocate arena) the later phases require, not a speed win. Keep it; do not re-debate.
- The REAL driver is the flat, serializable `TokenRec` (Phases 2-3): de-polymorphization
  (vtable → `kind`; thousands of `->id()`/`->type()` sites), `str`→`spelling_id` (~480
  sites), then dump/`mmap`+rebuild-by-`kind`. THIS is the forest/PCH keystone
  (`docs/plans/2026-06-22-embedded-header-forest-execution-plan.md`). It is multi-session;
  that is accepted, not a reason to stop short.

## 6. Rehydrate
`scripts/resume.sh`; read THIS file fully; `git log --oneline -8` to confirm HEAD.
**Phase 0 is DONE (results in §2, committed `ca898e1`). Current work = Phase 1** (variable-
size bump arena, routed via `TokenBase::operator new`/`delete` into a per-compile current
arena — routes every `new TokenX` AND every `clone()` with zero call-site changes; bulk
reset, no per-token free). Implement the FULL plan through Phase 3; correctness gate only.
