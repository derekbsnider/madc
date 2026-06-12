# HANDOFF — Retire embedded shims: real headers serve every mode

**Read this FIRST on resume/post-compaction.** Cold-start brief; assume you
remember nothing. Run `bash scripts/resume.sh` first (live git/build truth),
then read this top-down. The governing process document is
**`madc-header-partition-handoff.md` (repo root)** — the user has had to
re-point at it repeatedly; every decision here must trace to it. Its
companion memory: `project_retire_embedded_shims` +
`project_header_partition_architecture`.

---

## 0. TL;DR

Branch **`feature/retire-embedded-shims-claude`** off develop @ `2832fc0`
(develop untouched). ALL bucket-3 shims are DELETED (23k lines):
`include/madc/` holds ONLY bucket-1/2 compiler headers
(float/limits/stdarg/stdbool/stddef/stdint) + madc-owned `ns_*`. Real
glibc/libstdc++ serve every mode **including default STD_MADC** (which now
presents as g++ — `presents_as_cpp()`). Real `<iostream>/<string>/<cmath>`
compile AND run g++-identically in default mode. Integration was 546/36
before the latest (uncommitted-at-writing) stream-boundary fix; expect
~548+/34− after. The work remaining is (a) the wall list in §4, (b) the
PROCESS conformance items in §5 that institutionalize the partition doc.

**User rulings (binding):**
- K&R-era recovery (old-style params, implicit-int defs) ONLY under
  explicit `--std=c78..c17`. Never STD_MADC, never C++ (`knr_supported()`).
- No shims, no per-case hacks, fix at the deepest layer — categorical.
- All bucket-3 hand-rolled headers stay deleted; never re-author them.

## 1. The partition model (from madc-header-partition-handoff.md)

A header is madc's ONLY if its correctness requires codegen-private facts
(size_t identity, va_list layout, limits, intrinsics). Bucket 1 = pure
compiler headers (madc supplies fully). Bucket 2 = layering shims that
`#include_next` to the system copy (stdint/limits/float). Bucket 3 =
EVERYTHING else — all glibc + all libstdc++ — consumed REAL and unmodified.
The authority for bucket 1/2 membership is `gcc -print-file-name=include`
(the `$OWN` dir), NOT the standard's freestanding list.

## 2. What landed (commit chronology on this branch)

- `fa25e7f` **K&R gate**: `Program::knr_supported()`; harness `--std=c17`;
  9 K&R-era tests got `.flags`. GATED GREEN (fulltest 582, torture 52-name
  baseline ZERO regr +1 fixed → `docs/parity/torture-failset-current.txt`
  now 52 names, SMAUG soak green).
- `13383b7` **presents_as_cpp()**: STD_MADC seeds `__cplusplus` (201703L
  floor) + `__GNUG__` like explicit C++ modes; C modes stay plain gcc.
  Pin tests: testpredefmacros (defined) / testpredefmacros_c17 (absent).
  GATED GREEN (same three gates).
- `2d61556` **the sweep**: all bucket-3 shims deleted; `#include_next`
  made positional (never consults named PCH/embedded caches); baked PCH
  table EMPTIED (stale single-mode `gcc -E` captures that shadowed real
  headers; `gen_precompiled_headers.sh` HEADERS=() with rationale; lookup
  machinery kept for the proper PCH track). Plus 3 root-cause fixes:
  typedef_emit_name chokepoint for extern-proto RETURN types
  (cir_builder ~11289); shim text-ctor requires `required_param_count()<=2`
  (cir_builder `class_text_ctor`); template DEFAULT-arg declarator
  suffixes `_Tp*`/`_Tp&`/`_Tp&&` fold into the arg type (parser ~2660,
  mirrors the explicit-arg star fold; the suffix used to LEAK into the
  live token stream).
- `bb8083b` **preprocessor root causes**: #if expands function-like
  macros WITH arguments (expandIfMacros); gcc's guard-aware
  multiple-include optimization for SYSTEM headers (guard-less
  bits/mathcalls.h re-tokenizes per `_Mdouble_` pass — float/ldouble math
  decls were silently lost) while user `"..."` includes keep require-once
  (testincludeonce); generic `__builtin_X -> X` libc-twin dlsym fallback
  (emit_symbol = twin; kills the grow-forever hand list); FP-classify
  builtin family as sizeof-dispatched statement-expr macros onto REAL
  glibc exports (`__fpclassify*`/`__isnan*`/`__isinf*`/`__finite*`).
- `1b91e9f` **SFINAE pre-check** ([temp.deduct]):
  instantiate_fn_template_binding resolves a substituted
  `typename Q::X<args>::member` RETURN type in a sandboxed token push
  BEFORE the body parse; unresolvable → silent candidate discard. Real
  <cmath>'s integer-only `__gnu_cxx::__enable_if` overloads no longer
  hard-error float calls.
- (latest) **instantiation stream-boundary fix** (parser
  instantiate_fn_template_binding tail): the injected token run is
  restored to `base_depth` UNCONDITIONALLY after the parse — an "ok"
  `__hypot3<float>` instantiation left 2 trailing inj tokens that the
  resumed outer parse consumed, shifting every later declaration
  ("__z undeclared" two functions later). Cleared testmathh +
  testieeehugeval. `#if MADC_DEBUG_FNTPL` now also reports any
  imbalance (the diagnostic that found this).

## 3. Diagnostics arsenal (all gated, compile with -D<flag>)

- `MADC_DEBUG_FNTPL=1` — fn-template instantiation outcomes + STREAM
  IMBALANCE reports (parser.cpp).
- `MADC_DEBUG_NS_RESOLVE` — unknown-namespace throws with instantiation
  depth (parser.cpp ~13755).
- `MADC_DEBUG_TYPEDEF_EMIT` — typedef_emit_name alias→tag decisions
  (cir_builder.cpp).
- `MADC_DEBUG_BASE_CLAUSE` — base-clause first-lookup resolutions
  (parser.cpp ~19545).
- `madc -E` — preprocessed token stream (the bisect substrate;
  tmp/m4_pp.txt is `#include <math.h>` in default mode).
- Reducers in tmp/ (gitignored), ALL default-mode no-flags unless noted:
  realios*.mad (iostream), p2.mad, c9/c11.mad (extern-proto string),
  d1-d3.mad (string by-value), v1-v6.mad (vector/iterator bases),
  m1-m4.mad (math.h), h1-h4.mad (hypot shape), pfx1/pfx2.mad
  (m4_pp.txt prefixes), bisect.sh (prefix bisector).

## 4. REMAINING WALLS (attack order; per-fix METHOD in §6)

1. ~~typename dependent return types~~ CLEARED @1b91e9f.
   ~~math param-scope leak~~ CLEARED @ stream-boundary fix.
2. **Class-scope alias in hidden-friend bodies** — `typedef _Bit_iterator
   iterator;` then `friend iterator operator+(const iterator&, ...)` in
   real bits/stl_bvector.h → "use of undeclared identifier 'iterator'".
   Blocks real `<vector>` and with it map/set/sstream clusters
   (testvector/map/set/containerdtor/templatecontainer/templatestring/
   subscript*/sstream — the largest cluster, ~15 tests). Reducer: v1.mad
   (`#include <vector>`). Related: the claude_status known-gap about
   char_type/iter_type alias resolution — same family; fix generically
   (class-scope alias visibility during member/friend body parse), no
   per-alias hacks.
3. **testmadceval\*** (6 tests) — emitted eval code references `_ISupper`
   etc.: glibc ctype.h's anonymous enum constants don't reach the child
   eval TU. Likely the eval-TU synthesis (`<ns_madc>` path) needs the
   same real-header include context as the parent.
4. **teststat/teststatret/testservent** — parse error in real
   sys/stat.h chain under default mode ("unexpected token type 10" near
   EOF = stream desync; instrument like wall 1 — possibly another
   boundary/recovery leak).
5. **testmultiret/testrust** — bogus mangled import `_ZNSolsESo`
   (ostream<<ostream by value — overload resolution mis-pick on the
   real-header operator<< set; reduce `cout << <multi-ret-call>`).
6. **--emit=c11 hygiene** (non-blocking): `operatornew[]__o5`,
   `operator""s` leak as raw C identifiers in emitted text (JIT tree
   unaffected). safe_ident()-class fix at emission.
7. Stderr NOISE from caught/discarded instantiation attempts
   (throwbuf::sync prints unconditionally): wrap candidate-scoring
   instantiation in a diagnostics-suppressed mode so SFINAE discards are
   silent (currently they print scary-but-harmless errors, e.g. m1's
   "cannot dereference non-pointer type"). Principle: a DISCARDED
   candidate prints nothing; the CHOSEN candidate's errors are real.

## 5. PROCESS CONFORMANCE (institutionalize the partition doc — overdue)

These make the model self-enforcing instead of memory-dependent:

- **P1. Step-1 discovery gate**: new `scripts/check-header-partition.sh`
  — enumerate `gcc -print-file-name=include`, record GCC version +
  listing checksum in `docs/parity/header-partition-baseline.txt`;
  verify `include/madc/` ⊆ {bucket-1/2 names from $OWN} ∪ {ns_*}; FAIL
  on any bucket-3 reappearance. Wire into `make -C src fulltest` next to
  check-no-std-hardcoding.sh. THIS is the unfakeable "shims stay dead"
  contract.
- **P2. Step-4 macro parity**: madc has NO `-dM` yet (gap). Add
  `--dump-macros` (trivial: dump define_map/macro_map after init), then
  diff against `gcc -dM -E -x c /dev/null` and `g++ -dM -E -x c++` for
  the macros real headers branch on; record the accepted-diff baseline
  in docs/parity/. (gen_predefined_macros.sh captures build-time values;
  the diff verifies nothing load-bearing is missing.)
- **P3. Acceptance oracle (partition doc "Acceptance tests")**: freeze
  the C smoke (stdio/stdlib/string/stdarg/stddef/limits) and C++ smoke
  (type_traits/utility/tuple/vector/string/memory) as permanent
  tests/*.mad fixtures in BOTH default and --std=c++17 modes, once wall
  2 falls.
- **P4. Bucket-2 conformance**: current stdint.h/limits.h/float.h are
  FULL shims; the doc prescribes thin `#include_next` chaining shims.
  Convert + verify madc's #include_next semantics against each.
- **P5. Step-5 builtins checklist**: enumerate the `__is_*`/`__has_*`
  intrinsics the installed libstdc++ calls (command in the doc) and
  track implemented-vs-missing in docs/parity/ (drives <type_traits>
  conformance work).

## 6. METHOD (mandatory — unchanged)

Per fix: reduce (tmp/, NO flags = default mode is the point) → attribute
(gcc + clang + stock `/workspace/mir/c2m FILE -ei`; for madc-path bugs use
`--dump-cir`, NOT emit-C-as-truth) → DEEPEST-layer fix, no shims, no
per-name special cases → rebuild (`touch` the .cpp first — NAS mtime trap;
clean-rebuild if results look impossible) → re-probe reducers →
`make -C src fulltest` (cap: `( ulimit -t 3600; timeout 3000 ... )`, ONE
heavy job at a time) → full torture ALONE, failset-name diff vs
`docs/parity/torture-failset-current.txt` (52 names) → SMAUG soak
(`cd /workspace/MadSMAUG/runtime/area && timeout 50 /workspace/madc/bin/madc
--project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000`; exit 124
+ "Realms of Despair ready at" = good) → commit on THIS branch → update the
STATUS block in docs/plans/2026-06-12-retire-embedded-shims-plan.md.
Background long runs (`run_in_background`), capture FULL logs to tmp/
(never `| tail` into the log — it truncates the failset!).

## 7. MERGE GATE (do not merge to develop before ALL of)

fulltest 100% green (582+ incl. re-greened tests) + both check gates +
P1 partition gate · torture zero regressions vs 52-name baseline · SMAUG
soak · `bash scripts/run_tests.sh --exe` (shared-codegen surfaces moved)
· mirrors synced (claude_status.json head, CHANGELOG, ROADMAP, KG via
scripts/kg_query.sh, agent memory) · user approval (develop is the
shared stable branch).

## 8. Why the failures were "new" (user question, answered 2026-06-12)

Only 18 tests ever ran `--no-embedded-headers` (iostream/fstream/string/
compare families, under --std=c++17). vector/map/set/sstream and the
whole madc-dialect surface (eval, php arrays, foreach) had ONLY ever run
against the embedded shims. The sweep put all 582 tests through real
headers in default mode for the first time; every failure is a latent
real-header bug, not a regression of proven coverage.
