# Parser unbounded-lookahead audit — O(n²) gotcha log

**Date opened:** 2026-06-23
**Branch:** `feature/front-end-performance-claude`
**Status:** LIVING LOG. Append as cases are found/fixed.

## The bug class

A parse-time helper that **scans forward over the token stream** and, on a common
or failure path, **runs to the end of the stream** instead of bailing early. When
such a helper is invoked **once per token / per declaration / per `<`** (an O(n)
frequency), the unbounded scan makes parsing **O(n²)**.

The tell in a profile: one `peek_*` / `*_index` / `*_from` helper with a huge
**self** cost, and the per-token accessors it calls (`TokenX::id()`) clustered
right under it.

### Why this hides

- It is invisible on small inputs and ordinary tests — only macro-/comparison-
  heavy or very large bodies expose it (e.g. gcc.c-torture `memcpy-a*`, `memclr`).
- `tokens` may be the **whole program stream** OR a **small spliced-in local
  stream** (`swap_in`). The same `for (i…; i < tokens.size(); ++i)` is harmless on
  a 30-token local stream and catastrophic on the 324K-token program stream. **You
  must know which `tokens` is at each site.**

## Trigger (standing rule)

**Whenever a test compiles slower than gcc, callgrind it.** GCC is the
performance baseline, not just the codegen baseline (`.claude/rules/gcc-parity.md`).
`scripts/perf_vs_gcc.sh <file> [--std=STD]` times madc's front-end vs `gcc -O0 -c`,
prints the ratio, and **auto-callgrinds** when madc is slower than the threshold,
emitting the top self-cost madc functions — the culprit list. New entries in this
log start there.

The **frame of reference is recorded once**: gcc (and tinycc, the floor) are timed
the first time a `(file, std)` is seen and stored in `docs/parity/perf-baseline.tsv`;
later runs reuse the recorded numbers and only re-time **madc** (the thing that
changes). Re-measure the reference with `--refresh` after a host/toolchain change.
This keeps the loop fast and the ratio stable across runs.

## Detection methodology (reuse this)

1. Reproduce with `--show-stats`; confirm the cost is **parse** (not lex / c2mir /
   instantiate) and note `tokens re-read` (rules in/out the clone path).
2. Build a **macro-free reducer** that mirrors the body shape and scale it
   (×256/512/1024). Confirm parse time ~4× per doubling = O(n²) (tok/s halves).
3. `valgrind --tool=callgrind` at small N; `callgrind_annotate --inclusive=no`.
   The top **self**-cost madc function is the culprit.
4. Fix at the **deepest layer**: bound the scan to where the construct can
   legitimately end (statement/block/paren/bracket delimiters), or gate the scan
   behind a cheap O(1) predicate so it never runs on the common path.
5. Gate: fulltest green + gcc-torture failset == baseline + (if emit touched)
   `--emit=c11` byte-identical + re-time the reducer (must be linear).

## Cases

### #1 — `peek_after_balanced_template_id_from` (FIXED 2026-06-23)
- **File:** `src/parser.cpp` (~16819). Call sites: `template_id_is_type_expression_context`
  (expr parse, per identifier-before-`<`) and the cast-detection path (~19128).
- **Cause:** invoked on every `ident <` in expression context to skip a *template-id*
  `Name<…>`. For a less-than comparison (`i < n`) there is **no matching `>`**, so
  `for (i=lt_index; i<tokens.size(); ++i)` scanned to **end of the whole program**,
  returned NULL after O(n) work. Once per `<` across the file → **O(n²)**.
- **Evidence:** callgrind 37.5% self in this one function (10× the next). memcpy-a1
  parse 27.18 s; reducers 0.28/1.20/4.98 s at N=256/512/1024 (4.2× per doubling).
- **Fix (deepest layer):** a balanced template-id can never span `;` / `{` / `}`.
  Bail (return NULL) on those — bounds each scan to the current statement → O(n).
- **Result:** memcpy-a1 parse **27.18 s → 0.598 s** (45×); reducers linear
  (~500K tok/s flat across N). tinycc compiles the same in 0.05 s; gcc -O0 3.58 s.

### #2 — `logical_snapshot()` whole-stream copy per `template<>` (FIXED 2026-06-23)
- **File:** `include/madc.h` `TokenStream::logical_snapshot()`; sole caller
  `src/parser.cpp` (template-declaration parse, ~34547).
- **Cause:** to capture the requires-clause prefix that `skip_requires_clause()`
  pops, the code snapshotted the **entire** logical token stream (a
  `vector<TokenBase*>` of every remaining token) and diffed sizes — **once per
  `template<>` declaration**. libstdc++ headers have hundreds/thousands of
  templates × a large stream ⇒ O(templates × tokens) = O(n²). (Pre-existing: the
  pre-P1 deque code copied the whole stream the same way; P1 preserved it as
  `logical_snapshot`. Not a P1 regression.)
- **Found by:** the perf-parity rule — testsubscript was 8.5s vs g++ ~0.9s;
  callgrind showed ~50% self+inclusive in `logical_snapshot` + the
  `vector<TokenBase*>` push_back/size/[]/grow it drove.
- **Fix (deepest layer):** `TokenStream::consumed_since(Pos)` reconstructs ONLY the
  popped front prefix (popped pushback entries + the `_buf` range the cursor
  advanced over) — O(consumed), not O(stream). `savepos()` is O(pushback)≈O(1).
  `logical_snapshot()` deleted (no other caller).
- **Result:** testsubscript parse **7.59s → 1.06s (7.1×)**, total 8.49s → 2.17s.
  tok/s 53K → 377K. fulltest unchanged.

## Audit worklist — forward scans over the token stream (triage pending)

Seed list = every `for (… < tokens.size(); …)` in `src/parser.cpp` (2026-06-23).
**Each needs the same two questions:** (a) is `tokens` here the live program stream
or a small spliced-in local stream? (b) how often is this site invoked — O(1)
per-construct, or O(n) per-token/per-decl? Only (live stream) × (O(n) frequency) ×
(scans to end on a common path) = a BUG-B-class O(n²). Triage; don't mass-rewrite.

Sites (line numbers at 2026-06-23 HEAD; will drift — re-grep):
`2828, 2893, 2975, 4683, 4755, 8375, 8413, 13278, 16826 (#1 FIXED), 17966 (capped
ui<10 — safe), 18040, 21721, 21765, 21821, 21893, 23502, 24030, 24065, 26496,
29347, 29392, 29475, 29566, 29594, 30087, 30253, 30311 (i<end — bounded), 30356,
30394, 30480, 30537, 30580, 33283, 33839, 33846, 34008, 34032 (i+k<…, k<n —
bounded), 35109, 37394, 39296`.

Obviously-bounded ones (small explicit cap, or `i < end` local range) are noted
inline and can be skipped. The rest are unknown until the two questions are
answered. Prefer to confirm with a reducer + callgrind before "fixing" — a scan on
a small local stream is not a bug, and changing it adds risk for no gain.

## Principle — C++-only checks must be context-gated AND C-mode-disabled

A C++ disambiguation (template-id peek, most-vexing-parse probe, `<` =
less-than-vs-angle-bracket, `>>` split, etc.) must satisfy BOTH before it runs:

1. **Context-gate — only fire where the construct is possible.** Gate the
   expensive work behind the cheap O(1) precondition that the construct could even
   apply (e.g. *the identifier names a template* → `find_template` /
   `find_template_alias` before the template-id scan). A bare `<` after an
   ordinary identifier is less-than; do not scan to prove it.
2. **C-mode disable — no C++ checks in C.** Gate behind the `--std=` floor via
   `Program::cpp_keyword_active(min_std)` (true for `STD_MADC` or
   `is_cpp_mode() && std >= min_std`). Templates are C++98, so
   `cpp_keyword_active(STD_CPP98)`. In `--std=c*` the check is a no-op.
   - Note: a **standalone `.c`** file defaults to **STD_MADC** (C++ active), so the
     context-gate (#1) is the load-bearing one there; the C-mode gate covers the
     explicit `--std=c*` paths (e.g. gcc.c-torture runs under `--std=c17`).

Put the C-mode gate at the **deepest shared helper** (e.g. inside
`template_id_is_type_expression_context`) so no caller can forget it; put the
context-gate at the **call site** (it needs the identifier). This is the
`--std=` gatekeeping rule (`docs/plans/...std-enum...`, KG
`std_enum_gatekeeping`) applied to *parse-time disambiguation*, not just keywords.

When auditing the worklist below, add these as a third and fourth triage question:
(c) is this check C++-only? If so, is it C-mode-disabled? (d) is it gated by the
cheap precondition, or does it do the expensive work first?

## Related
- Macro token blow-up (324K tokens) is a *different* axis — see P5 in
  `docs/plans/2026-06-22-front-end-performance-plan.md` (macros as high-level
  nodes). Interning (P3) makes the cheap O(1) gates in step 4 even cheaper.

## Post-O(n²) profile — 2026-06-23 (container tests are no longer pathological)

After BUG B (template-id scan) and the `consumed_since` fix, `testmap` /
`testtuple` / `testtemplatecontainer` have **no remaining O(n²)** — the largest
single self-cost is 3.86% (callgrind of `testmap`, real-header `--std=c++17
--no-embedded-headers`). The two big algorithmic wins (45×, 7.1×) are banked;
what's left is **broad constant-factor**, ranked:

| Bucket | ~self-cost | What | Lever |
|---|---|---|---|
| istream lexer | ~20–25% | `istream::sentry/get/peek` + `Source::get/peek/good/eof` + `string += char` (token text built one char at a time) | **P2 buffered char lexer** |
| string-keyed maps | ~7–8% | per-token `keyword_map`/`define_map`/`macro_map`/`datatype_map` lookups; `std::less<string>` is 6.7% inclusive / 4.2M calls; `_getToken` drives ~2M of them | **P0 interning** (intern-once → integer keys) |
| malloc | ~5–6% | token / per-token `std::string` alloc | P1 token arena |

**Decision (user, 2026-06-23): take P2 (buffered lexer) next** — biggest measured
lever, self-contained, independent of interning, and the canonical tinycc lesson.
Note on interning: converting one map in isolation is ~net-neutral (a string-tree
walk becomes a hash+dedup). The win only lands when the lexer computes the
`spelling_id` **once per word** and *every* per-token lookup reuses it — so P0
step 3/4 must be done as a coordinated intern-once-reuse change, not one map at a
time. The macro maps also carry 100+ predefined insert sites to re-key.

### P2 implementation (in progress)
`Source` backed by `std::stringstream _ss` → flat `std::string _buf` + `size_t
_gpos` cursor (the whole input is already in memory; the istream sentry/locale
overhead was pure waste). `get/peek/good/eof/getline/showerror` become index ops;
public contract (pushback frames, line-splice, line/column, move-assignment for
the per-`#include` fresh-Source swap) preserved. Gate: fulltest 669/0/0/18 +
torture byte-identical + re-callgrind to confirm the istream bucket collapsed.
