# CODEX HANDOFF — eliminate ALL c2mir compile warnings + add a ratchet gate

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-29.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `9605221e` · tree clean.
**Why this exists:** c2mir compile warnings accumulated across the suite over ~a week with **no gate
to catch them**. That is the real failure. Warnings are NOT to be ignored. This handoff drives them to
**zero** AND installs a ratchet so they can never silently accumulate again.

---

## ⚡ MISSION (two parts, both required)
1. **Install a warnings ratchet gate** (do this FIRST — it stops the bleeding and measures progress).
2. **Drive all compile warnings to ZERO**, deepest-layer, gcc/clang-canon, no shims/suppression.

## SCOPE (measured 2026-06-29, flag-off / PRODUCTION path, `bin/madc <test>`)
**97 warnings across 25 of 688 tests.** Census tool: `tmp/warn_census.sh` (per-test counts +
category histogram → `tmp/warn_census_out/`). Top tests: testsubscript 18, testmadc_ns 13,
testcontainerdtor 13, testmap 10, teststdmapint 9, testset 5, testvectorptr 3, teststructinterop 3.

Categories (normalized):
```
 73  incompatible argument type for pointer type parameter
  6  using pointer without cast for integer type parameter
  6  using integer without cast for pointer type parameter
  6  incompatible types in assignment to a pointer
  2  range cases are not a part of C standard
  2  assigning integer without cast to pointer
  1  returning pointer without cast for integer result
  1  assigning pointer without cast to integer
```
All emitted by c2mir at `/workspace/mir/c2mir/c2mir.c:8509` family when madc's generated C passes /
assigns / returns a pointer-or-integer whose type mismatches the target without an explicit cast.

## THREE ROOT-CAUSE CLASSES (fix at the deepest layer; one class — ideally one cause — per commit)

### Class 1 — pointer-type fidelity (79: the 73 "incompatible argument" + 6 "incompatible assignment")
The dominant class. Sub-groups:
- **`_Rb_tree` (most of it): testsubscript/testmap/testset/teststdmapint/testcontainerdtor/testmadc_ns.**
  **READ `docs/plans/2026-06-29-rbtree-pointer-warnings-FINDINGS.md` FIRST** — it has the confirmed
  instrumentation and **two DISPROVEN hypotheses (do NOT repeat them)**: (1) overload-resolution
  downcast viability — ruled out, the arg is typed `_Link_type` (derived) at selection so selection is
  correct; the saved patches `tmp/b-part1-score_arg_to_param.patch` / `tmp/b-part2-reselect-static.patch`
  **must NOT be applied** (don't fix it; Part-2 regressed a binding); (2) generic `static_cast<Derived*>`
  downcast emission — ruled out (`tmp/static_cast_down.mad` is clean). **Confirmed leading cause:** the
  `_S_key` arg has static type **derived** (`_Link_type`) but is **emitted as a base pointer** — the bug
  is inside an instantiated `_Rb_tree` accessor (likely `_M_begin`/`_M_end`, declared returning
  `_Link_type` via `static_cast<_Link_type>(_M_impl._M_header._M_parent)` — a `static_cast` of a
  **member-access** pointer — or iterator `_M_node` typing / accessor return-type fidelity). NEXT: trace
  `_M_begin` in the emitted C (`bin/madc --emit=c11 tmp/mapmin.mad > x.c`), build a
  member-`static_cast`-return reducer, verify whether the accessor returns base where its declared
  return type is derived.
- **`ns_ruby` / `ns_perl` namespace calls (testlang, testperl, testruby*):** e.g. `ns_ruby:21:80`,
  `ns_perl:44:112` — a borrowed-language namespace function is called with a mismatched pointer (or a
  pointer where an int is expected — see Class 2). Trace the specific `ns_*.cpp` signature vs the
  call-site arg lowering.
- **struct/vector pointer assignment:** `teststructinterop.mad:38/55:114`, `testvectorptr.mad:32-34:19`
  ("incompatible types in assignment to a pointer") — a struct/vector element pointer assigned a
  related-but-different pointer type without the cast. Likely the same upcast/derived-base family as
  (a), but on an ASSIGNMENT (`N_ASSIGN`) rather than a call arg.

**Precedent already landed (`08642457`):** the derived→base **ctor-arg** upcast — the general call path
(`cir_builder.cpp:3608`) and now both ctor-arg loops apply `upcast_class_ptr`. The assignment family
likely needs the analogous explicit cast at the `N_ASSIGN` lowering (find where a class-pointer is
assigned and emit `upcast_class_ptr` / the proper cast, mirroring the call-arg fix).

### Class 2 — int↔pointer mismatch (16) — a SEPARATE type-lowering fidelity bug
NOT the inheritance family. madc passes/returns/assigns a pointer where an **integer** is expected (or
vice versa) without a cast — meaning a value's lowered type is wrong (a pointer modeled as a 64-bit
int, or an undeclared/implicit-return function typed `long` instead of its real pointer return — cf.
`.claude/rules/embedded-headers.md` "declare real return types"). Sites:
- `testfnptrvarargs`, `testkrfnptrvarargs`: "using integer without cast for pointer type parameter" —
  a function-pointer **vararg** argument is lowered as an integer. Trace the fn-ptr vararg arg typing.
- `testreturnfndecay`: "using pointer without cast for integer type parameter" — a function/array
  decays to a pointer passed where an int is expected (or a real-return-type gap).
- `teststruct.mad:9` ("assigning pointer without cast to integer") + `:13` ("using integer without
  cast for pointer") — a struct member's declared type (pointer vs int) mismatches the assigned/passed
  value. Fix where the member type or the value type is determined.
- `ns_perl:44` (3×) "using pointer without cast for integer type parameter" — a perl namespace fn arg.
These are the classic "real return type / value lowered at the wrong width" bugs — fix at the type
origin, never with a cast-shim at the use site.

### Class 3 — range-case note (2): `testcaserange.mad:5,6`
c2mir's pedantic note "range cases are not a part of C standard" on `case lo ... hi:` (a GNU/madc
extension c2mir supports but flags). madc OWNS the lowering: **expand `case lo ... hi:` to individual
`case lo: case lo+1: ... case hi:` labels** in the CIR (Tier-1 lowering — cf. the c11-transpiler rule
"Range designators must be expanded to individual designators"), so the emitted C is strict C11 and
c2mir emits nothing. Bound the expansion (reject absurd ranges) and keep the runtime behavior identical.

## DELIVERABLE 1 (FIRST) — the ratchet gate (so this NEVER recurs)
Mirror the existing torture-failset / drift-gate pattern:
1. Promote `tmp/warn_census.sh` → `scripts/warn_census.sh` (clean it; it compiles every `tests/*.mad`
   flag-off with its `.flags`, counts `warning --` lines). Make it print a single suite-wide total and,
   with `--check`, compare against a committed baseline.
2. Commit a baseline `docs/parity/warning-baseline.txt` = the current per-test counts (97 total). The
   gate FAILS if any test's count **exceeds** its baseline (new warnings) — a strict ratchet.
3. Wire `--check` into `make -C src fulltest` (alongside the existing drift gates) so CI fails on any
   regression. As you fix warnings, **lower the baseline in the same commit**; the end state is an
   all-zero baseline and the gate forbids any new warning.
This is the keystone: it makes "warnings accumulated silently" impossible going forward.

## GATE (every commit — correctness, never perf-gate, never suppress)
1. `make -j4 -C src` clean, **no new compiler warnings** either.
2. `make -C src fulltest` 670/0/0/18 + drift gates GREEN + **`scripts/warn_census.sh --check` GREEN**
   (warning baseline drops or flat, NEVER rises).
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 670/0/0/18.
4. gcc.c-torture failset byte-identical to `docs/parity/torture-failset-current.txt`.
5. Fix at the DEEPEST layer (`gcc -S -fverbose-asm -O0` / `clang -S` canon; compare emitted C with
   `--emit=c11` + `gcc -fsyntax-only -Wincompatible-pointer-types`). **No cast-shims at use sites when
   the type origin is wrong; no `#pragma`/suppression; no per-callee/class-NAME hardcoding (Rule #7).**

## TOOLS / METHOD
- **Categorizer:** `bin/madc --emit=c11 <test> > x.c` then
  `gcc -std=gnu11 -fsyntax-only -Wincompatible-pointer-types -Wno-implicit-function-declaration -Wno-builtin-declaration-mismatch x.c`
  — gives EXACT emitted-call line + expected-vs-got types (the c2mir position collapses to the class
  line; gcc on the emitted C pinpoints it). `tmp/mapmin.mad` is the minimal map repro.
- **Reducers present:** `tmp/mapmin.mad`, `tmp/static_cast_down.mad` (clean). `tmp/overload_*.mad` are
  SYNTHETIC and MISLEADING for Class 1 (they reproduce a different manifestation) — trust the emitted-C
  categorizer + the FINDINGS doc, not those.
- **gdb abort-backtrace** for emission-path questions: getenv-guarded `abort()` + `gdb -batch`.

## /goal (falsifiable)
`scripts/warn_census.sh` reports **0 total warnings** across `tests/*.mad` (flag-off); the committed
`docs/parity/warning-baseline.txt` is all-zero and `--check` is wired into `make -C src fulltest` and
GREEN; flag-off fulltest 670/0/0/18 + flag-on run_tests 670/0/0/18; gcc.c-torture failset byte-identical;
no warning suppression, no cast-shims masking a wrong type origin, no class/callee-NAME hardcoding.

## HANDBACK
Per class fixed: before/after warning count (from the census), the deepest-layer cause, gate results.
If a class walls, the emitted-C evidence + which layer declined. Claude verifies.

## SETTLED — do not re-litigate / do not repeat
- The (a) ctor-arg upcast (`08642457`) stays. The FINDINGS doc's two disproven Class-1 hypotheses are
  dead ends — do not re-run them; the saved patches are NOT to be applied.
- Range-cases: expand to individual labels (lowering), do NOT silence with a pragma.
- The ratchet gate is non-negotiable and comes first.
