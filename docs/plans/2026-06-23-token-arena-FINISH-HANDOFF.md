# TOKEN-ARENA — FINISH-TO-DONE HANDOFF + EXECUTION PLAN

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **HEAD at write:** `c117e41`

> ⛔⛔ **SUPERSEDED IN PART (design owner, 2026-06-23). READ THIS FIRST.**
> Token-arena **2.2a / 2.2b / 2.3-step1(rec-completion) are DONE + gated + committed**
> (`341dc5a`, `03ba2a8`, `c76e59a`). **Step 2.3's scratch-token re-parse isolation is
> CANCELLED** — recon of g++ + c2mir (see below) showed it optimizes the re-parse g++
> does NOT do. The work PIVOTED to the **g++ model at the cir_node TREE level**:
> **GOVERNING PLAN = `docs/plans/2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md`**
> (+ verified cross-checks in `...-two-tree-cir-architecture-NOTES.md`). Two cir_node
> trees — immutable Tree-1 (parsed patterns + header corpus = the forest, the copy
> source) and mutable Tree-2 (per-TU, →c2mir, built by `tsubst` = copy+substitute,
> never re-parse). VERIFIED: g++ does exactly this (tsubst over DECL_SAVED_TREE,
> copy_node); c2mir mutates only the node_t base it understands, so cir_node extension
> fields (incl. a Tree-1 back-ref) are invisible/safe.
> **PHASE 0 AUDIT DONE (2026-06-23)** — results in the PLAN doc's "Phase 0 — RESULTS".
> Crux finding: madc has NO `T`-as-placeholder DataDef (it substitutes at the token
> level pre-parse) → a NEW Phase 1.5 (template-param placeholder DataDef) is the deep
> prerequisite. **NEXT (code, next session) = PLAN Phase 1 (`copy_cir_subtree`), the
> safe first slice.** The token-arena steps below (2.2/2.3) are HISTORY now; do not
> resume them — follow the two-tree PLAN.

> ⛔ **DISTRUST THE /compact AUTO-SUMMARY.** It has been WRONG about this work before
> ("slab"→**variable-size bump**; "~3-5%"→**~0% at -O0, by design**). If anything you "remember"
> conflicts with THIS doc or the two docs it subordinates to, the docs win. Answer the READ-CHECK
> before editing.

> 🎯 **THE ONE JOB (user directive, firm, 2026-06-23): GET ALL OF TOKEN-ARENA DONE.** Then, and
> only then, the cir_node arena; then #1 materialize-from-AST. **NO reordering. NO incompletion.
> NO side quests.** This doc is the imperative plan to drive token-arena Phase 2 step 2 → step 3 →
> Phase 3 to completion. Do them IN ORDER. Each is its own commit, gated.

---

## READ-CHECK — answer ALL out loud in your first message, BEFORE any edit
1. **What is the ONE job?** → Finish ALL of token-arena (Phase 2 step 2, step 3, Phase 3). In order.
   Not cir_node arena, not #1, not the rbtree warning, not the pushback span-scan. Those are AFTER /
   SEPARATE.
2. **Two token populations?** → **pop-1** = LEXER tokens (Ident/Int/Real/Str/ops/punct), a LINEAR
   STREAM, flatten to POD `TokenRec`, **immutable once lexed**, NO children. **pop-2** = PARSER-built
   AST nodes (`TokenOperator` left/right, Assign, Var, Ternary…), the tree, **stay polymorphic
   objects** (P2 collapse FENCED). Token-arena flattens **pop-1 ONLY**.
3. **Gate?** → CORRECTNESS ONLY: `make -C src fulltest` = **669/0/0/18** + gcc.c-torture failset
   **byte-identical** to the 51-name baseline + `--emit=c11` byte-identical. **NEVER perf-gate.**
   ~0% at -O0 is EXPECTED (foundation), not failure.
4. **No-clone (step 3) safe?** → YES. pop-1 records are immutable, so sharing them by slot-id across
   instantiations is safe. The old "mutation blocker" was a struck conflation (pop-1 vs pop-2).
5. **Arena model?** → variable-size **bump**, 16-aligned, chunked, **never relocates** a live cell,
   **no per-token free**, bulk free at reset. NOT a freelist, NOT fixed cells.
6. **Where does the materialize-from-`kind` factory already exist?** → `src/pch.cpp:139`
   `token_from_id(TokenID)` (used by PCH deserialize). REUSE/extend it — do not write a parallel one.

## SUBORDINATE TO (build against these; do not write a competing plan)
- `docs/plans/2026-06-23-p1-token-arena-implementation-plan.md` — the phased plan + mental model (§1).
- `docs/plans/2026-06-23-token-arena-flattening-plan.md` — the two-step seam + the two-population
  finding + the `TokenRec` layout (§ lines 167-208).
- `docs/plans/2026-06-13-embedded-ast-frontend-design.md` — the arena + u32-index architecture.

---

## SETTLED — DO NOT RE-LITIGATE
- **Execution order (user, firm):** ALL of token-arena (2.2 → 2.3 → Phase 3) **FIRST**, THEN the
  cir_node arena, THEN #1 materialize-from-AST. #1 *sits on* this substrate (its "substitute = clone
  + remap" needs the flat arena); that is WHY token-arena goes first. Do not reorder.
- **Correctness gate ONLY.** No perf-gate; do not re-measure to "decide" whether to proceed; do not
  callgrind-then-pivot. A slice that passes fulltest + torture-byte-identical + emit-byte-identical
  is DONE — commit and move to the next.
- **No side quests.** The `_Rb_tree` warning (root-caused @`4ee820a`, two type bugs) is a SEPARATE
  deferred slice. The pushback/macro span-scan is part of perf-lever #1, NOT token-arena. #1
  materialize-from-AST is AFTER token-arena. Do not start any of them here.
- **pop-1 flattens, pop-2 stays objects** (P2 polymorphism collapse FENCED). The whole-parser
  de-polymorphization is NOT in scope, ever, in this track.
- **`type_id`/`file_id` are Phase-3 serialize-time derivations**, NOT live-rep changes. The live
  shell keeps `_datatype`/`file`; the Phase-3 pre-dump pass writes the index fields.

## DONE (verify with `git log --oneline`; do not re-discover)
- `ca898e1` Phase 0 audit · `e8861ac` Phase 1 bump arena (`operator new`→arena, no-op delete) ·
  `8bb60a2` `TokenRec` struct + `spelling_id` migrated · `45d2b88` indexable arena + slot registry
  (`madc_slot_id_for`/`madc_token_for_slot`, [0]=NULL sentinel) · `617a5fa` `cir_node::origin`→
  `origin_id` (slot-id; first id-vector consumer, read-only provenance).
- SEPARATE perf levers landed this session (NOT token-arena phases): `be21229` lexer `_buf`
  identifier span-scan (lever #1); `c117e41` `findVariable` sid-keyed (lever #2 / C2). Mentioned only
  so you don't confuse them with the phases below.

## CURRENT STATE (facts, audited)
- `TokenStream::_buf` (madc.h ~1077) = `std::vector<TokenBase*>` + cursor — a flat array of POINTERS
  to individually-`new`'d polymorphic objects. P1 changed the CONTAINER (deque→vector); the CONTENT
  is still scattered heap objects. `vector<TokenRec>` of inline POD records does NOT exist yet.
- `TokenRec` (tokens.h) is a member on `TokenBase` (`rec`); carries `spelling_id` + `slot_id` today.
  Phase-3 fields (`type_id`/`file_id`) are reserved, written at dump time.
- Factory to reuse: `src/pch.cpp:139 token_from_id(TokenID)` — payload-free kinds already covered;
  payload kinds (Ident/Int/Real/Str/Char/DataType/REM) need payload-carrying construction.
- Lexer `new TokenX` sites (~60 in `src/lexer.cpp`): 33 `TokenDataType`, 7 `TokenIdent`, 7
  `TokenReal`, 5 `TokenInt`, 3 `TokenREM`, 2 each `TokenStr`/`TokenChar`/operators…, rest single
  payload-free operators/punctuation.
- Type table is bidirectional already: `DataDef::type_id`, `Program::type_id_for(dd)`,
  `Program::type_from_id(id)` (parser.cpp ~8995-9067) — the `type_id` source for Phase 3.

---

## THE PLAN — three steps, in order, each its own gated commit

### STEP 2.2 — lexer emits `TokenRec` + the stream becomes `vector<uint32_t>` (two-step seam)
Goal: kill the per-token `new` on the lex path; the parser materializes a `TokenBase` shell on
demand. Do it as the flattening plan's **two-step seam** (old path replaced, NOT kept alongside):

- **2.2a — route construction through ONE factory (behavior-IDENTICAL).** Replace every lexer
  `new TokenX(...)` with a single `Program` factory call (e.g. `make_token(kind, payload…)`).
  Payload-free kinds delegate to `pch.cpp`'s `token_from_id(kind)`; payload kinds (Ident→spelling,
  Int→ival, Real→dval, Str→bytes, Char→code, DataType→type, REM→text) get payload-carrying
  construction. The factory still `new`s an object and returns `TokenBase*` — NO representation
  change yet. **Gate: fulltest 669/0/0/18 + torture byte-identical + emit byte-identical. Commit.**
- **2.2b — factory appends a `TokenRec`; stream becomes `vector<uint32_t>` of slot-ids; parser
  materializes shells on demand.** The factory writes a POD `TokenRec` (kind + payload→spelling_id/
  value + line/col) into the arena and records its slot-id in the stream. `TokenStream` exposes the
  current token by materializing (or caching) a `TokenBase` shell from the `TokenRec` via the factory
  (reuse `token_from_id` + payload from the rec). Replace `TokenStream::_buf`'s `vector<TokenBase*>`
  with `vector<uint32_t>` (slot-ids); backtrack = cursor rewind (unchanged). **Gate as above.
  Commit.** Expect ~0% -O0 — fine.

### STEP 2.3 — no-clone macro/template substitution (id-vector)
Substitution (`#define` expansion, template body splice) builds a NEW `vector<uint32_t>` reusing the
UNCHANGED pop-1 slot-ids (shared, immutable) and substituting only the replaced ids. Retires
`clone()` on those paths. **SAFE — pop-1 records are immutable (READ-CHECK Q4); no identity/
occurrence surgery.** Provenance (line/col) is the definition-site record's, per pop-1 semantics.
**Gate: fulltest + torture byte-identical + emit byte-identical. Commit.**

### STEP 3 — serialization (the forest/PCH keystone)
- **Pre-dump materialization pass:** walk the tree with `Program` in scope, write each `TokenRec`'s
  index fields from the live shell — `rec.type_id = type_id_for(tok->_datatype)`,
  `rec.file_id = intern(tok->file)`. Live rep unchanged; only the on-disk record is fully indexed.
- **Dump + load:** dump `TokenRec[]` + `StringPool` + type table + file table; on load, mmap + rebuild
  shells by `kind` (the same `token_from_id` factory). This is the forest keystone
  (`docs/plans/2026-06-22-embedded-header-forest-execution-plan.md`). **Gate as above. Commit.**

### THEN token-arena is DONE → next track is the cir_node arena (NOT in this doc).

---

## PROCESS RULES (non-negotiable)
- Commit via `git commit -F -` heredoc (NOT `-m` with backticks). Stage files EXPLICITLY — never
  `git add -A`. Leave `mir-debug-support.md` UNTRACKED (not ours).
- Commit trailers:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_015eGuYph7nFzyq2e8vc9B1Y`.
- `-O0` is the dev default. Cap every test run `( ulimit -t N; timeout M … )`. ONE heavy job at a
  time. **No poll-loop / `while pgrep` waits** (that self-matches and hangs — bug hit 2026-06-23);
  background a long job and wait for the completion notification.
- `make -C src fulltest` after every change. Callgrind/timing is INFORMATIONAL — record it, never
  gate on it, never let it trigger a reorder or a pivot.
- Keep `project_frontend_performance.md` (memory) synced at each step commit.

## VERIFICATION (each step)
- `make -C src clean && make -C src` builds with no new warnings.
- `make -C src fulltest` → 669/0/0/18; drift gates green.
- gcc.c-torture failset byte-identical to the 51-name baseline:
  `python3 scripts/run_gcc_testsuite.py` → extract `FAIL(...)`/`TIMEOUT` basenames, sort -u, `diff`
  against `docs/parity/torture-failset-current.txt` (must be empty).
- `bin/madc --emit=c11` on a representative TU byte-identical to pre-change (representation changed,
  emitted C must not).
