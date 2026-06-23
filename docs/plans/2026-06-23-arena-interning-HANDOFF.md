# Arena + interning — CONTINUATION CONTRACT (post-compaction: follow exactly)

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude`
**Read this BEFORE acting. It is imperative, not advisory. Do not re-scope, do not
re-derive, do not re-litigate. The session that wrote it kept mis-modeling the goal
and the corrections below are hard-won — honor them.**

---

## 0. SETTLED — do NOT re-open (each cost real time this session)

1. **The arena is governed by PRE-EXISTING authoritative designs. Do not write a
   competing plan; build against these:**
   - `docs/plans/2026-06-13-embedded-ast-frontend-design.md` — ARCHITECTURE:
     append-only token array + rewind cursor; **cir_node = arena + `u32` index
     handles, no internal pointers**; interned type/identifier IDs by index;
     side-car tables keyed by node index; segmented mmap / zero-copy on-disk.
   - `docs/plans/2026-06-09-frontend-representation-refactor.md` — PHASED P0..P5
     (critical path P0→P3→P4→P5; P1 independent; **P2 FENCED**).
   - `docs/plans/2026-06-12-type-table-value-abi-design.md` — segmented `u32`
     type-id table (the type sibling of the string table).
   - `docs/plans/2026-06-23-token-arena-flattening-plan.md` — execution NOTES only
     (subordinate; holds this session's findings).
2. **The flat arena is for LEXER TOKENS ONLY.** The parser still builds the
   `cir_node` AST; each cir_node carries a **`uint32_t[]` of arena slot-ids** for
   its originating tokens. The cir_node AST ALSO becomes a contiguous arena (P3,
   index-linked) so the whole IR serializes. Three pointer classes → indices:
   children→node-index, tokens→slot-ids, types→type-id.
3. **P2 polymorphism-collapse (1577 `->id()`, 388 `->type()`, 574 `dynamic_cast`)
   is FENCED. DO NOT do it.** My "must de-polymorphize ~480+thousands of sites"
   estimate was wrong (conflated lexer tokens with the parser AST) and is STRUCK.
   You do NOT need to rewrite `->id()` call sites to flatten the lexer tokens.
4. **DO NOT rebuild the token-pool allocator.** A `TokenBase::operator new/delete`
   pool was built, measured (NO -O0 win — the 47% malloc is an -O2 figure), and
   REVERTED. It optimizes the current pointer-object model, not the flat arena.
   (Reason recorded in the flattening-plan finding (3).)
5. **`-O0` is the dev default (optimization is last-lap). Test cap is 10s.** Do not
   flip `-O2` to chase numbers; fix algorithms. (`src/Makefile`, `scripts/run_tests.sh`.)
6. **String dedup = a purpose-built arena-model hashstr table, NOT
   `std::unordered_map<std::string,…>`** (a std::map re-allocates each key string,
   defeating the dedup-malloc win). It must be index-linked (not pointer-chained)
   so it mmaps zero-copy. Built: `include/stringpool.h` (algorithm = tinycc
   `TokenSym` / SMAUG `hashstr`, indices not pointers).

---

## 1. State at handoff (verify with `git log --oneline -15` + `git status`)

**Committed on `feature/front-end-performance-claude` this session (newest first):**
- `1b059bf` subordinate arena notes to the pre-existing designs
- `b2c1c75` **fix(parser): logical_snapshot → consumed_since — per-template O(n²),
  testsubscript parse 7.59s→1.06s (7.1×)**
- `cc30385`, `34ac39d`, `8cf72c8`, `fc59be0`, `d25bde5` arena plan + Phase-0 findings
- `f4c5f75` perf_vs_gcc.sh stats-parse fix
- `77d551a` -O0 default + 10s cap + perf-vs-gcc tooling + recorded baseline
- `be3f800` **fix(parser): bound + gate template-id lookahead (BUG B) — O(n²)→O(n),
  memcpy-a1 27s→0.6s (45×); memcpy-a* now pass torture**

**UNCOMMITTED at handoff (commit these next — see §2 step 1):**
- `include/stringpool.h` — the StringPool (sanity-tested: dedup + 5000-entry
  growth/distinctness pass via a scratch test; NOT yet a doctest, NOT yet wired).
- `docs/plans/2026-06-13-embedded-ast-frontend-design.md` — added the arena-model
  intern-table structure to §2.
- `docs/plans/2026-06-23-arena-interning-HANDOFF.md` — this file.

**Validation baseline to preserve at every step:** fulltest **669 passed / 0 failed
/ 0 timed out / 18 skipped**; gcc-torture failset == the recorded baseline;
`--emit=c11` byte-identical.

---

## 2. P0 string-interning — the imperative sequence (each step its own commit, each
fulltest-green before the next)

**Step 1 — land StringPool (DO FIRST).**
- Add a doctest unit test `tests/unit/test_stringpool.cpp` (dedup → same id;
  distinct → distinct ids; `str()` roundtrip; empty→0; 5000-entry growth/rehash
  integrity; `count()`). Wire it into `src/Makefile`'s unit-test list (copy an
  existing `test_*` entry pattern; it needs no `dd*` globals — header-only).
- Commit `include/stringpool.h` + the test + the design-doc edit + this contract.
- Gate: `make -C src test` green.

**Step 2 — intern identifiers at the lexer (additive, keep `.str`).**
- Add a `StringPool` member to `Program` (one per compile). Absorb `intern_file`'s
  role into it OR leave files as-is for now (note which).
- In the lexer, where `TokenIdent` is created, also `intern()` the spelling and
  store a `uint32_t spelling_id` on `TokenIdent` (ADD the field; keep `str` for
  now so no reader breaks). `clone()` copies `spelling_id`.
- Gate: fulltest 669/0/0/18 (behavior-neutral; additive).
- NOTE: no perf win yet — that lands in step 4 when `str` is dropped. This step is
  the foundation; do NOT skip to step 4.

**Step 3 — re-key the hot string maps to `uint32` spelling-id.**
- Convert the callgrind-flagged maps from `std::map<std::string,…>` to id-keyed
  (`std::unordered_map<uint32_t,…>` or a flat structure): `datatype_map`,
  `template_map`, `partial_spec_map`, `template_alias_map`, `var_template_map`,
  `fn_template_map`, `define_map`, `macro_map` (do them one at a time, fulltest
  between). Key insert/lookup by `intern(name)`.
- Gate per map: fulltest green + re-run `scripts/perf_vs_gcc.sh` on a header-heavy
  test to confirm the `Rb_tree<string>` cost is dropping.

**Step 4 — drop the per-token `std::string` (the malloc win).**
- Replace `TokenIdent::str` reads (~480 sites) with `pgm.spelling(spelling_id)` /
  an accessor; remove the `std::string str` member. This is the big surface — do
  it in tranches, compiler-guided, fulltest between tranches.
- Gate: fulltest 669/0/0/18; torture failset == baseline; perf_vs_gcc shows the
  per-identifier `std::string` allocation gone.

**THEN (separate, later — not P0):** P1 token value-records (flat `TokenRec` arena,
stream = slot-ids), P3 cir_node `uid` side-arrays + serialization, P4 serialize/mmap
the forest. Follow the authoritative docs in §0.1.

---

## 3. The OTHER live track — keep hunting O(n²) (highest ROI this session)

The perf-parity rule found TWO big wins (BUG B 45×, consumed_since 7.1×) via
**callgrind whatever is slower than gcc**. Keep doing it — it out-delivers the
arena refactor per unit effort:
- `scripts/perf_vs_gcc.sh <file> [--std=STD]` — times madc vs recorded gcc baseline
  (`docs/parity/perf-baseline.tsv`), auto-callgrinds when slower, prints top
  self-cost madc functions. Frame of reference RECORDED ONCE (reused; `--refresh`).
- Log new finds in `docs/plans/2026-06-23-parser-lookahead-audit.md` (the bug-class
  log + the "C++ checks must be context-gated AND C-mode-disabled" principle +
  the ~40-site forward-scan triage worklist).
- Candidates not yet callgrinded: testmap, testtuple, other template-heavy tests.

---

## 4. Process rules (non-negotiable)

- Commit via `git commit -F -` heredoc (NOT `-m` with backticks). Stage files
  EXPLICITLY — never `git add -A`. Leave `mir-debug-support.md` UNTRACKED (not ours).
- Commit messages end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_015eGuYph7nFzyq2e8vc9B1Y`.
- One heavy job at a time; cap every run `( ulimit -t N; timeout M … )`. No
  poll-loop sleeps. Background long builds/tests; you are re-invoked on completion.
- `make -C src fulltest` after every change. `-O0` default. Do not push to GitHub
  without a `/release`.
- Memory: `project_frontend_performance.md` is the active-track file; keep it synced.

## 5. Open design points (resolve WITH the authoritative docs, do not freelance)

- Intern-table segmentation/rehash for the embedded snapshot: frozen embedded
  segment owns an id range; TU appends + TU-private bucket extension (keeps the
  embedded block zero-copy). Buckets are derived (rebuild-on-load for compressed
  segments; baked for `codec=none`).
- `cir_node` derives from c2mir `node_t` (pointer DLIST): the contiguous index form
  is the *serialized* rep; relocate indices → `node_t` pointers on load.
- `StringPool::c_str(id)` is valid until the next growing `intern()` — hold the id,
  not the pointer; `reserve()` up front. (Documented in the header.)
