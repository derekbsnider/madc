# P1 token arena — IMPLEMENTATION PLAN + CONTINUATION CONTRACT

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **HEAD at write:** `2a7868c`

> ⛔ **DISTRUST THE /compact AUTO-SUMMARY.** It has been WRONG about this exact work — it said
> "slab arena" (it is a **variable-size bump** arena) and "modest ~3-5%" (Phase 1 is **~0% at
> -O0**, by design). If anything you "remember" from a summary conflicts with this doc, the
> doc wins. **Do not act on the summary. Answer the READ-CHECK below (from §1) before editing.**

## READ-CHECK — answer ALL of these OUT LOUD in your first post-compaction message, BEFORE any edit
If your answer differs from the one given, STOP and read §1 fully — you have drifted. These ARE
the load-bearing disambiguations past sessions got wrong.

1. **How many token populations, and what is each?** → **pop-1** = LEXER tokens (Ident/Int/
   Real/Str/operators/punctuation) — a **linear stream**, target form = flat POD `TokenRec`,
   **immutable once lexed**. **pop-2** = PARSER-built AST nodes (`TokenOperator` `left`/`right`,
   `Ternary`, `Case`, `Assign`, `Var`…) — the tree, **stay polymorphic objects**.
2. **Do pop-1 tokens have children?** → **NO.** Linear stream. Trees belong to pop-2 + cir_node.
3. **Where do `parent`/`left`/`right` live, and when are they written?** → On **pop-2 AST objects**,
   written **fresh during parse** (`new TokenAssign` → `lhs->parent = assign`). **NOT** on shared
   pop-1 tokens.
4. **Is no-clone macro/template substitution blocked by mutation?** → **NO.** Pop-1 records are
   immutable, so sharing them by slot-id across instantiations is safe. (The "mutation blocker"
   was a STRUCK conflation — see §1f.)
5. **Arena cell model?** → **variable-size bump** (each token exactly `sizeof(T)`, 16-aligned),
   chunks never relocate, **no per-token free**. NOT fixed cells, NOT a freelist.
6. **Phase-1 -O0 speedup?** → **~0%, EXPECTED.** Phase 1 is the FOUNDATION, not a perf win.
   **Never perf-gate** any phase; gate = fulltest + byte-identical output only.
7. **Are `type_id`/`file_id` a live-rep change now?** → **NO.** Serialize-time derivations
   (Phase 3). The live rep keeps `_datatype`/`file`; the Phase-3 pre-dump pass writes the indices.
8. **Is whole-parser de-polymorphization in scope?** → **NO.** That is **P2, FENCED.** Only the
   pop-1 lexer stream flattens; pop-2 AST stays objects.
9. **The three `uint32` index spaces?** → **token slot-id** (TokenRec arena) · **cir_node index**
   (cir_node arena — a SEPARATE P3 track) · **type-id** (segmented type table).
10. **What is DONE and what is NEXT?** → DONE: Phase 0 audit `ca898e1`, Phase 1 bump arena
    `e8861ac`, `TokenRec`+`spelling_id` `8bb60a2`, indexable arena/slot registry `45d2b88`,
    `cir_node::origin`→slot-id `617a5fa`, plan corrected `2a7868c`. **NEXT = Phase 2 step 2**:
    lexer emits pop-1 `TokenRec`s + the stream becomes a `vector<uint32_t>` id-stream +
    materialize shells on demand (reuse `pch.cpp`'s `TokenID→new TokenX`).

## GLOSSARY (load-bearing terms — fixed definitions, do not re-interpret)
- **pop-1 / lexer token** — a lexed token; a linear-stream element; target = flat POD `TokenRec`;
  immutable once lexed.
- **pop-2 / AST node** — a parser-built node (`TokenOperator`, `Ternary`, `Case`, `Assign`,
  `Var`…); carries tree pointers (`parent`/`left`/`right`); stays a polymorphic object (P2 fenced).
- **slot-id** — a `uint32` index into the token (`TokenRec`) arena; a token's stable identity for
  id-streams/id-vectors and for `cir_node::origin_id`.
- **identity** — `kind` (the `TokenID` enum) + `spelling_id` (StringPool). Shared, interned,
  immutable. (== the "prototype" layer.)
- **occurrence** — the per-lex `TokenRec` data (`line`/`column`/`value`). Not shared.
- **`TokenRec`** — the flat POD record for ONE pop-1 token. **No children.**
- **id-stream / id-vector** — a `vector<uint32_t>` of slot-ids: the lexer token stream, or a
  substituted macro/template body.

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
- Consequence: collapsing the WHOLE token hierarchy to a flat POD + switch-dispatch (incl.
  the pop-2 AST nodes' `parse()`/`dynamic_cast`) would be a **parser rearchitecture** (115
  parse methods + 326 casts) = **P2 polymorphism collapse, FENCED.** This plan does NOT do
  that. The flat `TokenRec` arena is for **pop-1 LEXER tokens only** (a linear stream); pop-2
  AST nodes stay objects. See §1 (the two-population model — the thing that disambiguates this).

---

## 1. MENTAL MODEL — read this WHOLE section before touching token code

**This plan is SUBORDINATE to three authoritative designs. Do not re-derive; build against them:**
- `docs/plans/2026-06-13-embedded-ast-frontend-design.md` — the architecture (§2: cir_node =
  arena + u32 index handles; the interned string/identifier table = the `TokenRec.spelling_id`
  backing store, index-linked).
- `docs/plans/2026-06-09-frontend-representation-refactor.md` — the phased plan **P0–P5**
  (P0 value/intern pools; **P1 flat token scan buffer**; **P2 polymorphism collapse = FENCED**;
  P3 uid side-arrays + serializable cir_node; P4 forest serialize; P5 modules).
- `docs/plans/2026-06-23-token-arena-flattening-plan.md` — the execution notes + the **two-
  population** finding.

### 1a. TWO TOKEN POPULATIONS — never conflate them
- **Pop-1 = LEXER tokens** (`Ident`, `Int`, `Real`, `Str`, operators, punctuation — the ~324K
  bulk). A **LINEAR STREAM**, not a tree. **They have NO children.** Flatten to a flat **POD
  `TokenRec`** in an arena; the stream is a `vector<uint32_t>` of slot-ids; backtrack = cursor
  rewind. **Pop-1 records are IMMUTABLE once lexed.**
- **Pop-2 = PARSER-built AST nodes** (`TokenOperator` with `left`/`right`, `TokenTernary`,
  `TokenCase`, `TokenTry`, `TokenAssign`, `TokenVar`, …). Constructed DURING parse, carry the
  tree pointers (`parent`/`left`/`right`), and **STAY OBJECTS** (P2 collapse is FENCED). These
  are NOT lexed and do NOT go in the TokenRec arena.

### 1b. THREE index spaces (all `uint32`, all segmented append-only — same pattern)
- **token slot-id** → index into the pop-1 `TokenRec` arena. Used by the lexer id-stream AND by
  `cir_node` to point at its originating token (DONE: `cir_node::origin_id`).
- **cir_node index** → index into the cir_node arena (P3/P4 — a SEPARATE track; cir_node children
  become these, NOT token slot-ids).
- **type-id** → into the segmented type table (`docs/plans/2026-06-12-type-table-value-abi-design.md`).

### 1c. IDENTITY vs OCCURRENCE (the prototype/instance split — design owner's framing)
- **Identity** (shared, immutable, interned): `kind` (the `TokenID` enum value) + `spelling_id`
  (StringPool). This is the "prototype" space — fixed tokens are identified by their `TokenID`;
  identifiers by their interned spelling. Shared across every use; never mutated.
- **Occurrence** (per-lex, not shared): the `TokenRec` itself — `file_id`/`line`/`column` and the
  literal `value`. Today `clone()` conflates identity and occurrence; the flat model separates them.

### 1d. The flat pop-1 record — NO children (linear stream)
```cpp
struct TokenRec {                 // POD, trivially copyable, NO pointers — dumpable/mmappable
    uint16_t kind;                // TokenID — the identity (drives shell rebuild on load)
    uint16_t flags;               // tokflag_t
    uint32_t spelling_id;         // -> StringPool (identity)        DONE
    uint32_t slot_id;             // this record's own arena id      DONE
    uint32_t type_id;             // -> segmented type table (Phase 3 dump-time materialization)
    uint32_t file_id;             // -> interned files       (Phase 3 dump-time materialization)
    int32_t  line, column;        // occurrence/provenance — PER record, not re-stamped
    int64_t  value;               // _token / ival / char code; >64-bit -> value-pool handle (P0)
    // NO first_child/child_count — pop-1 is a LINEAR STREAM. Trees are pop-2 (objects) and
    // cir_node (its own arena/index space). A wide-value/double side-table is a P0 concern.
};
```

### 1e. Transitional mechanism vs end state
- **End state (per the authoritative docs):** the lexer emits POD `TokenRec`s into the arena and
  the stream is a `vector<uint32_t>`; the parser **materializes a `TokenBase` shell on demand**
  (reuse `src/pch.cpp`'s `TokenID → new TokenX` factory) only where it needs an object (a pop-2
  AST node, or a `_datatype` annotation). Pop-2 AST stays objects. cir_node flattens separately.
- **Transitional (where we are now):** the shell `TokenBase` stays polymorphic and carries the
  `TokenRec` as a member (`rec`). Fields migrate onto `rec` additively; allocations route through
  the bump arena (Phase 1). This is a valid path toward the end state, but it is NOT the end state —
  do not mistake "shell + rec member" for the final POD-records-with-materialize-on-demand model.

**Rejected alternatives (do not revisit):**
- *Pure flat POD + switch dispatch for the WHOLE parser* — that is P2 polymorphism collapse
  (1577 `->id()`, 574 `dynamic_cast`, etc.). **FENCED.** Pop-2 AST stays objects.
- *Freelist `operator new` pool on TokenBase* — built and REVERTED (no -O0 win). The Phase-1 arena
  is a **bump** (stable chunks, bulk free), NOT a freelist, and is FOUNDATION not a perf play.

### 1f. CONFLATIONS TO AVOID (caught 2026-06-23 — do not repeat)
1. **`parent`/`left`/`right` are POP-2 fields**, written during AST construction on freshly-built
   objects (e.g. parser.cpp:14354-14361 `new TokenAssign` then `lhs->parent = assign`). They are
   NOT mutations of shared pop-1 lexed tokens. ⇒ id-vector substitution sharing pop-1 records is
   SAFE; there is **no** occurrence/identity-surgery blocker (the earlier "mutation blocker" was this
   conflation — STRUCK).
2. **`TokenRec` has NO children.** Giving a linear-stream record `first_child`/`child_count` (an
   earlier draft did) conflates pop-1 with pop-2/cir_node. STRUCK.
3. **"children→slot-ids" is NOT a token-arena step.** cir_node children → cir_node-arena indices is
   P3 (a separate track). Pop-2 AST children stay pointers (FENCED). Pop-1 has no children.

**Hard invariant: POINTER STABILITY (transitional).** While shells are live objects, `cir_node`,
`TokenBase::parent`, and pop-2 `vector<TokenBase*>` hold raw pointers. The bump arena MUST NOT
relocate a live cell — chunked storage, never a reallocating `vector<cell>`.

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

**Phase 2 — flatten the POP-1 LEXER STREAM (per §1; this is representation-refactor P1).**
The goal is NOT "flatten every token field" — it is: the lexer emits POD `TokenRec`s into the
arena and the stream becomes a `vector<uint32_t>` of slot-ids; pop-2 AST stays objects.
Each step its own commit, fulltest + `--emit=c11` byte-identical between.

`type_id`/`file_id` are **serialize-time derivations, NOT live-rep changes** (RELOCATED to
Phase 3). The type table is already bidirectional — `DataDef::type_id` (plain field),
`Program::type_id_for(dd)`, `Program::type_from_id(id)` (parser.cpp:8995-9067) — so a token's
type/file are already recoverable as indices. Keep `_datatype`/`file` as the shell's live
resolved values; a Phase-3 pre-dump pass writes `rec.type_id`/`rec.file_id` with `Program` in
scope. Routing live reads through `type_from_id` would add hot-path indirection + force
`Program`-threading into `datadef()`/`setDataType()` for ZERO benefit before serialization.

Step list:
  1. `spelling_id` — DONE @8bb60a2 (TokenRec introduced as a shell member; identity field migrated).
  1b. indexable arena (slot-id ↔ token bridge) — DONE @45d2b88 (`TokenRec.slot_id`,
     `TokenArena` slot registry, `madc_slot_id_for`/`madc_token_for_slot`, test_token_arena).
  1c. `cir_node::origin` → `origin_id` (slot-id) — DONE @617a5fa. First "pointer class → index"
     conversion; READ-ONLY provenance ⇒ safe. Proved the registry under load (669 compiles).
  2. **Lexer emits pop-1 `TokenRec`s + the stream becomes `vector<uint32_t>` (the P1 win).**
     Per the flattening plan's two-step seam: route lexer construction through a factory while
     behavior is identical, validate, THEN have the factory append a `TokenRec` and let the
     parser **materialize a `TokenBase` shell on demand** (reuse `src/pch.cpp`'s `TokenID→new
     TokenX`) where it actually needs an object. Kills the per-token `new` on the lex path.
  3. **No-clone macro/template substitution.** Substitution builds a NEW `vector<uint32_t>`
     with substituted slot-ids; unchanged pop-1 records are reused BY ID (shared). **SAFE — see
     audit below: pop-1 records are immutable.** Retires the clone() on these paths (the ~43%
     instantiate cost). Provenance (line/col) is per pop-1 record (the definition's position).

**MUTATION-SAFETY AUDIT (2026-06-23) — CONCLUSION: no-clone substitution IS safe; no blocker.**
(An earlier version of this section concluded the opposite — that was a CONFLATION of pop-1 and
pop-2, now corrected; see §1f.) The `parent =`/`left =`/`right =` writes the audit found are on
**POP-2 AST objects built fresh during parse** — e.g. parser.cpp:14354-14361 does `new
TokenAssign` / `new TokenVar` then `lhs->parent = assign`, wiring brand-new nodes. They are NOT
mutations of the pop-1 lexed tokens that substitution shares. Pop-1 `TokenRec`s carry only
identity (kind/spelling_id) + occurrence (line/col/value) and are **immutable once lexed**, so
two instantiations referencing the same pop-1 slot-id cannot collide. (`setDataType`'s 37 sites
likewise mostly target parser-synthesized temporaries; `read_count` is a harmless diagnostic.)
So id-vector substitution needs NO identity/occurrence surgery on the AST — it is the
straightforward "build a new id-vector, reuse unchanged ids, swap substituted ids" the
flattening plan describes. Pop-2 AST construction stays exactly as today (objects, FENCED).

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
  (the `type_id` source for the Phase-3 dump-time materialization).
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

## 5. PRIORITY ORDER — parity with g++ FIRST (real efficiency), THEN forest goes BELOW it
GOAL (user, 2026-06-23): madc must **lex+parse+instantiate FASTER than g++**. **EXACT SUCCESS
METRIC:** `madc(lex + parse + cir-build) ≤ g++ -fsyntax-only -O0` (at minimum MEET; ideally
BEAT) — the front-end-only comparison (sum the three `--show-stats` phase lines; EXCLUDES
c2mir-compile + execution, matching `-fsyntax-only`). Measured frame: madc lex 0.46 + parse 1.10
+ cir 0.38 = **1.94 s** vs g++ `-fsyntax-only` **0.50 s** (~3.9× to close). g++'s 0.5 s parses
the stdlib from scratch too — so MEET = same from-scratch work (T1-T6 + lex), BEAT = the forest
(skip stdlib parse). g++'s 0.5 s already includes parsing `<map>`/`<vector>`/
`<string>` every run — so "match g++" means doing the SAME from-scratch work in the same time;
the forest then takes madc BELOW g++ by not re-parsing the stdlib. Ordering of wins:

1. **PRIMARY — stop re-parsing to instantiate (materialize-from-AST). THE parity lever.**
   87% of madc's parse time is template instantiation, and madc **RE-PARSES the body tokens per
   instantiation**: `instantiate_template_use` (parser.cpp:3469) copies `TemplateDef.body`
   (a `vector<TokenBase*>` — TOKENS, not a parsed tree), substitutes into those tokens, splices
   them into the stream, and runs the parser AGAIN; sema is entangled in that re-parse.
   g++/clang parse each body ONCE to an AST and instantiate by **copy + substitute** (`tsubst` /
   `TreeTransform`), never re-parsing. This is the structural 4×. It is **forest-INDEPENDENT**
   (pure in-memory; helps live project templates) and generalizes the landed lazy-body work
   (today *defers* the parse → target *copy the parsed tree, never re-parse*). Design doc:
   `docs/plans/2026-06-23-materialize-from-ast-instantiation-design.md`.
2. **SECONDARY — flat representation (THIS plan: arena / `TokenRec` / interning / no-clone).**
   Constant-factor (~1.5×). Makes lexing fast AND makes "copy + substitute" cheap (a `cir_node`
   arena with `uint32` handles + `type_id`s turns substitution into an index remap, not a
   re-parse). It is the SUBSTRATE for #1 and #3 — necessary, not sufficient alone.
3. **THEN — forest (pre-parsed stdlib, Phase 3 serialization).** Amortizes the stdlib parse
   across runs → BELOW g++. Only worth it AFTER parsing is efficient (caching a slow front end
   still loses on cold runs). NOT "the payoff" — the THIRD win.

Phase 1 (~0% at -O0, `fc59be0`) is FOUNDATION, expected, not a speed claim. Gate = correctness
+ byte-identical; never perf-gate a phase. P2 whole-parser de-polymorphization stays FENCED.

## 6. Rehydrate
`scripts/resume.sh`; **read §1 (MENTAL MODEL) FULLY** — it is the part that has tripped up
past sessions (the two-population conflation). Then `git log --oneline -10` to confirm HEAD.
**DONE:** Phase 0 audit (`ca898e1`), Phase 1 bump arena (`e8861ac`), TokenRec + spelling_id
(`8bb60a2`), indexable arena / slot registry (`45d2b88`), `cir_node::origin`→slot-id
(`617a5fa`). **CURRENT WORK = Phase 2 step 2** — flatten the pop-1 lexer stream (lexer emits
`TokenRec`s + `vector<uint32_t>` id-stream + materialize-on-demand shells via pch.cpp's
factory). Then step 3 (no-clone id-vector substitution — SAFE, §1f). Correctness gate only.
Implement the FULL plan; do NOT re-introduce the struck "children→slot-ids" / "mutation
blocker" framing.

**BIGGER PICTURE (do not lose at compaction):** this token-arena plan is the SECONDARY
(constant-factor + substrate) lever per §5. The PRIMARY parity lever — the one that closes the
~4× vs g++ — is **materialize-from-AST instantiation (stop re-parsing template bodies)**, its
own design at `docs/plans/2026-06-23-materialize-from-ast-instantiation-design.md`. The forest
(pre-parsed stdlib) is the THIRD win (below g++). Priority order: §5.
