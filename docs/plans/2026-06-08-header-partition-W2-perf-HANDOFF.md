# HANDOFF — Header-partition campaign: W2 done, real `<iostream>` RUNS, parser O(n²) fixed

**Read this FIRST on resume/post-compaction.** Self-contained cold-start brief.
Assume you remember nothing. Run `bash scripts/resume.sh` first (live git/build
truth), then read this, then the **governing design corpus** in §1.

> **PROCESS LESSON THAT GOVERNS THIS WORK (do not skip §1).** The biggest failure
> this campaign keeps hitting: a fresh session rehydrates on the *routed handoff
> only*, re-derives designs/building-blocks that already exist, and that
> re-derivation *is* the duplicate code this campaign exists to delete. Before
> planning ANYTHING, read the design corpus (§1) and `grep` for the capability
> (including work done in prior sessions). Treat "I think X is missing" as a
> search task, not a fact. See `[[feedback_rule4_check_own_prior_work]]`.

---

## 0. TL;DR

Branch **`feature/header-partition-claude`**, HEAD **`10fcef1`**, working tree
clean, **17 commits ahead of `develop`, local only (unpushed — user's call)**.
Gates green: **fulltest 543/4** (known reds: testdefer/testfstream/
testlargesizeofquery/testloop), **gcc.c-torture 1566/31/57/1** (run ALONE),
21 MI/RTTI/vdtor tests + `test_extern_polymorphic` pass.

**This session delivered two milestones:**
1. **Real libstdc++ `<iostream>` compiles and RUNS end-to-end.** `bin/madc
   --std=c++17 --no-embedded-headers tests/testcout.mad` prints
   `This is a test, x = -1` (byte-identical to g++), via mangled-direct calls to
   libstdc++ — from a **2858-error compile wall to running**. (W2 non-member
   operator resolution + std::endl manipulator + facet vtable ownership +
   reachability DCE + external-dtor gating.)
2. **Parser O(n²) symbol lookup fixed** — real `<iostream>` compile **7.6s →
   1.33s** (5.7×; g++ is 0.48s). `TokenCpnd::findVariable` was a linear vector
   scan (~40% of all instructions on a real-header compile); now an O(1) index.

---

## 1. GOVERNING DESIGN CORPUS (READ THESE — do not re-derive)

The header-partition campaign = **madc ships only compiler-freestanding + its own
`ns_*` headers, and CONSUMES the real glibc/libstdc++ headers** (eventually as one
pre-lexed compressed embedded package), retiring the hand-tooled stdlib shims.

- **`~/.claude/plans/clever-scribbling-dove.md`** — the approved campaign plan.
  Defines the **build half (B1–B6)**, **runtime half (R1–R5)**, **M** (shim
  retirement), **A** (acceptance oracle). §7 below tracks each.
- **`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`** — the
  std-hardcoding design. Defines **W1–W5** (W1 mangler, W2 non-member operator
  resolution, W3 ABI-from-declaration, W4 extern header globals, W5 auto-include
  map). This is the design **W2 (this session) executed** — do NOT reinvent it.
- **`docs/superpowers/plans/2026-06-02-mangler-nonmember-template-ops.md`** —
  the W1 mangler-completeness plan (`itanium_mangle_std_free_template`), the
  building block W2 consumes.
- `docs/plans/madc-vision-and-invariants.md` — invariants I1–I8.
- Memory: `[[project_header_partition]]` (the live campaign status),
  `[[project_north_star_c23_cpp23]]`, `[[project_cpp_mangled_direct]]`,
  `[[project_template_instantiation]]`.

Branch facts (from git, not assumption): `feature/header-partition-claude` is the
**canonical tip** (newest, 17 ahead of develop). `retire-std-hardcoding` is
**already merged into develop** — not a competing branch. No fork to reconcile.

---

## 2. CURRENT STATE

- Branch `feature/header-partition-claude`, HEAD `10fcef1`, clean, 17 ahead of
  `develop`, **unpushed**.
- fulltest **543/4** (reds: testdefer, testfstream, testlargesizeofquery,
  testloop — all predate this work; testfstream/testloop/testdefer go green when
  the real-`<fstream>` retirement lands, campaign M).
- gcc.c-torture **1566/31/57/1** (run ALONE).
- MIR fork `/workspace/mir` @ `2ffebff` (develop), pinned by `MIR_COMMIT`.

---

## 3. THIS SESSION'S COMMITS (9, since the prior handoff `4a46e6f`)

All gated green. In order:

| Commit | What |
|---|---|
| `32484e1` | **Reachability DCE** for library inline-method bodies. Pass 2 emits a system-header fn body only if reachable from user-code roots (g++'s ODR-use model). New `Program::is_system_header_path()` (prefix-match vs `madc_sys_include_paths[]`, data-driven, never `namespace==std`). Real `<iostream>` 2858 → 100 errors. |
| `12398f2` | **Facet vtable ownership.** `DataDefCLASS::from_system_header` — a system-header polymorphic class is libstdc++-owned regardless of inline virtual-default bodies (fixes `ctype`/`num_put`/`num_get`/`basic_streambuf` parallel-vtable flood). 100 → 1. |
| `acd9fdc` | **W2 step 1** — capture non-member operator overloads from declarations into `Program::free_operator_overloads` (`capture_free_operator_overload` in parser.cpp; the skipped-template path couldn't see an operator declarator). 141 captured. |
| `0d442f6` | docs(resume): anchor branch on W2 from the design. |
| `61d9264` | **W2 step 2** — `CirBuilder::try_free_operator_call`: at `class_operator_call`, consider free candidates whose param[0] deduce-matches the lhs class + param[1] exactly matches rhs (free exact `const char*` beats member `const void*`); most-specialized wins (char partial spec); bind mangled-direct via `itanium_mangle_std_free_template`. `cout << "lit"` → exact g++ symbol. 1 → 0 c2mir errors. |
| `e9a2792` | **External-dtor gating** — dtor-synthesis Passes 1.6/1.7/1.8 skip `is_externally_defined()` classes (killed the dangling `_ZNSt12__cow_stringD1Ev` from synthesizing `ios_base::failure`'s dtor). |
| `f38c005` | **std::endl manipulator** mangled-direct — `cout << endl` lowers to `endl(&cout)` (manipulator gets the stream), bound to `_ZSt4endl…`. Real `<iostream>` RUNS. New test `tests/testcout_realhdr.mad` (+.flags/.expect). New generic runner convention `tests/foo.timeout`. |
| `960505c` | docs(resume): W2 + external-dtor done. |
| `10fcef1` | **perf: `findVariable` O(n²) → O(1)** — incremental `unordered_map` index on `TokenCpnd`. Real `<iostream>` 7.6s → 1.33s. |

---

## 4. REAL `<iostream>` RUNS — the W2 mechanism (committed, done)

`cout << "lit" << -x << endl` now lowers to (`--emit=c11`):
```c
basic_ostream..._operator<<( &(*_ZNSolsEi( (void*)&(*_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(&_ZSt4cout, "lit")), -x )), __ns_std_endl? NO -> ... )
```
i.e. const char* binds the FREE `std::operator<<(ostream&,const char*)`
(`_ZStls…`), int binds the member `_ZNSolsEi`, endl binds free `std::endl`
(`_ZSt4endl…`) called with the stream. All mangled-direct to libstdc++ (R2
auto-load links it). **Key principle (do not violate):** madc emits *definitions*
only for entities it defines; everything libstdc++ owns (vtables, typeinfo,
out-of-line/explicitly-instantiated members, free operators/manipulators) is
referenced by its real Itanium symbol — discriminated DATA-DRIVEN
(`is_system_header_path` / `is_externally_defined` / `from_system_header`), never a
`namespace=="std"` test (Rule #7).

### W2 anchors (live @ `10fcef1`)
- Capture: `capture_free_operator_overload` + `capture_free_manipulator_overload`
  + shared `extract_free_signature` + `serialize_token_range` — `src/parser.cpp`
  (just above `register_skipped_namespace_template_function`). Store:
  `Program::free_operator_overloads` (`include/madc.h`, struct `FreeOperatorOverload`).
- Consume: `CirBuilder::try_free_operator_call` — `src/cir_builder.cpp` (just above
  `class_operator_call`, called from it after `select_operator_overload`). Shared
  deduction `deduce_free_stream_call` (file-local). Manipulator path is the first
  block in `try_free_operator_call`.
- Mangler (W1, in develop): `itanium_mangle_std_free_template` (`src/madc_mangle.cpp`,
  tests in `tests/unit/test_mangle.cpp:378`). Produces the exact `_ZStls…`/`_ZSt4endl…`.

### W2 — what REMAINS (the broader operator surface)
- `operator<<(ostream&, const std::string&)`, `operator>>`, `std::getline` — same
  machinery; the mangler already has tests for all of them
  (`test_mangle.cpp` lines ~388–410). Needs: a std::string rhs match in
  `try_free_operator_call` (rhs spelling for the `std::string` class), and the
  istream `>>`/getline analogues. Build GENERIC; never a stream-specific picker.
- **Friend operators + full ADL** (task #19): friend FUNCTION definitions are
  currently SKIPPED (parser.cpp ~17176; only friend *type names* recorded for
  access). Hoisting inline friend `operator<<` to callable free functions +
  widening lookup to associated namespaces/classes is the path to full C++
  operator compliance. `try_free_operator_call`'s `collect`/deduce chokepoint is
  built to extend additively.

---

## 5. PERFORMANCE — findVariable fixed; remaining levers (profiled with callgrind)

**Diagnosis (callgrind on a real-`<iostream>` compile; perf unavailable, callgrind
is — it gets SIGXCPU'd by the bg CPU cap but dumps a usable partial profile):**
- It was NOT lexing (`-E` 0.49s), NOT c2mir (0.15s on the emitted 5942-line C),
  NOT emission (`--emit=c11` minus parse = 0.53s). It was **parse 6.84s**, and
  **~40% of all instructions were in `TokenCpnd::findVariable`** — a linear scan
  over the scope's `std::vector<Variable*>` doing string compares, per lookup,
  recursing to parent → O(n²) with thousands of symbols in the closure. (g++ uses
  hash-table name lookup; same idea.)

**Fixed (`10fcef1`):** incremental `unordered_map<string,Variable*>` index on
`TokenCpnd`, built lazily (first-wins via emplace, matching the old scan).
Subtlety that bit once: a Variable's `name` is MUTATED after insertion (operator
arity-disambiguation renames an overload), so cached keys go stale — handled by
validate-on-hit + rebuild-on-stale (+ reset at the one erase site). Verified zero
lookup divergence vs a fresh scan across the C++ test set. **7.6s → 1.33s.**

**Remaining levers (post-fix profile is now FLAT, top ~3%, no single hotspot):**
1. **Don't eagerly instantiate templates inside system headers** (user's explicit
   directive). An empty `<iostream>` program does **796 template instantiations**
   it never uses; each re-lexes a template body (the `Source::get`/`std::istream`
   per-char I/O at ~10–15%) and churns the maps/allocator. Biggest remaining lever
   but delicate: must keep genuinely-used + explicit instantiations. **This is
   also what B3–B6 (the pre-lexed PCH package) solves wholesale** — cache the
   parsed+instantiated closure once instead of every compile.
2. **Buffer the lexer** — `Source::get/peek` go through `std::istream` per char
   (`sentry` overhead ~3%+); a buffered reader removes ~10–15%.
3. **`unordered_map` the other symbol tables** — `datatype_map`/`struct_map`/
   `namespace_map` are `std::map<string>` (O(log n), shows as `std::less<string>`/
   `operator<` ~1.3%). Same fix as findVariable, smaller scale.

Commands: `( ulimit -t 600; timeout 900 valgrind --tool=callgrind
--callgrind-out-file=tmp/cg.out --dump-instr=no bin/madc --emit=c11 --std=c++17
--no-embedded-headers tmp/empty_io.mad )` then `callgrind_annotate --threshold=85
tmp/cg.out`. Phase timing: there is no flag; `--emit-pch`≈tokenize, `--emit=c11`
≈parse+CIR+emit, `c2m -c <emitted.c>`≈c2mir.

---

## 6. NEXT STEP (the one remaining iostream-adjacent item is DONE; pick a lever)

Real `<iostream>` runs. The immediate forks, in the user's expressed priority:
- **(perf) eager-instantiation reduction** (§5.1) — the user flagged it twice;
  biggest remaining perf lever; delicate. OR a quick lexer-buffer / map pass first.
- **(W2 breadth)** std::string `operator<<`, `operator>>`, `getline` (§4).
- **(compliance)** friend operators + full ADL (task #19).

---

## 7. BROADER CAMPAIGN STATUS (every workstream the user asked about)

### From `retire-std-hardcoding-design.md` (W1–W5)
- **W1 mangler — DONE** (in develop; member ops + non-member template ops incl.
  `_ZStls…`/`_ZSt4endl…`; round-trip unit tests).
- **W2 non-member operator resolution — DONE this session** (`acd9fdc`/`61d9264`/
  `f38c005`); breadth (string/`>>`/getline/friend/ADL) remains (§4).
- **W3 ABI-from-declaration plumbing — PARTIAL.** sret/`__retbuf` + vbase layout
  exist (used by MI). Generic base-subobject offset for inherited libstdc++ calls
  + by-value struct returns through MIR not yet exercised on real streams.
- **W4 extern globals in headers — PARTIAL.** `cout`/`cin`/`cerr` resolve as
  externs (mangled `_ZSt4cout` etc.) on the real-header path; the legacy
  `make_hidden_std_global`/`SK_*` still exist for the embedded path (deleted in M).
- **W5 auto-include map — PARTIAL.** The auto-include table exists; the std::
  symbol→header trigger set (cout→`<iostream>`, etc.) is the only permitted
  hardcoding; audit/complete it during M.

### From `clever-scribbling-dove.md` — runtime half (R1–R5)
- **R1 per-`--std` macros + `__has_include`/`__has_builtin` — NOT DONE.**
  `__cplusplus` is frozen at `201703L` (`set_language_standard`, parser.cpp ~6527);
  the `__has_*` operators are unimplemented in `expandIfMacros`/`evaluateIfCondition`
  (lexer.cpp ~4028/4084). Generate per-std tables via `gen_predefined_macros.sh`.
- **R2 runtime libstdc++ auto-load — DONE** (task #7, in develop; it's why
  mangled-direct `_ZSt…` calls link at MIR time, `madc_import_resolver`).
- **R3 retire the `array` keyword — NOT DONE.** `array` lexes as `TokenARRAY`
  (datatokens.h:48), shadowing `std::array`; move madc's PHP-style dynamic `array`
  into a namespace to free the slot. Unblocks `<tuple>`/`<array>`/`<bitset>`.
- **R4 trait-builtin breadth — PARTIAL.** Several `__is_*`/`__are_same` etc. landed
  in prior arcs (table-driven fold, parser.cpp ~4290/4340). Correctness-first;
  must match g++ on a reducer matrix; leave-unrecognized rather than guess.
- **R5 real-header sema completeness — SUBSTANTIALLY DONE this session.** C1
  (class-scope alias `char_type`, bc5e6cd) done earlier; the W2/facet/DCE work
  made real `<iostream>` parse+run. Remaining R5 gaps are for OTHER headers:
  the `<string_view>`/`<memory>` blocker (namespace-qualified template-id in a
  static-const captured during instantiation, `std::__are_same<float,float>::value`
  — `capture_constant_initializer_value` drops the `::Name` segment) and
  `_S_categories_size` class-scoped enum visibility. (Per the older
  realhdr handoffs — verify against current state before assuming still-broken.)

### From `clever-scribbling-dove.md` — build half (B1–B6) — NONE DONE
This is the **pre-lexed embedded header package** = the wholesale fix for the
§5.1 perf problem (cache the parsed/instantiated closure once, not per compile).
- **B1 partition manifest** (`gen_partition_manifest.sh`, bucket-1 vs bucket-2
  via `#include_next`), **B2 system closure** (`gen_system_closure.sh`, `g++ -M`),
  **B3–B6** (pre-LEX raw tokens — NOT `gcc -E` — compress into one `MADP` package
  in libmadc.a; `include/madc-freestanding/`; `#include_next` within the package).
  Reuse `src/pch.cpp`/`include/madc_pch.h`; extend `compiler_hash()` with the
  toolchain version. Note: madc already has `--emit-pch` (tokenize+serialize, 0.7s)
  as the seed.
- **NOT started.** Sequenced after R1/R2 per the plan; but the perf finding (§5)
  makes B3–B6 newly attractive — it directly removes the re-parse/re-instantiate
  cost.

### M — incremental shim retirement (NOT DONE)
Once each std header parses+links+RUNS green against the real header in
`--std=c++NN`, delete its `include/madc/` shim and the per-type machinery
(`ddSTRING`, `dt*STREAM`, `string_*`/`streamout_*` wrappers, `SK_*`, the `_ZSt…`
literals at cir_builder.cpp ~1628, `make_hidden_std_global`). Order: `<string>` →
parse-only headers → `<iostream>`/`<ostream>`/`<istream>` → `<fstream>` (fixes the
3 fstream reds) → `<sstream>` → containers (last). `scripts/check-no-std-hardcoding.sh`
is the grep-gate (wired into fulltest). **`<iostream>` now RUNS against the real
header, so it is a candidate to begin M with** (carefully — keep the embedded path
working until the real path covers every iostream test).

### A — acceptance oracle (NOT DONE)
Add `madc -dM` (dump `define_map`/`macro_map`); wire C-smoke + C++-smoke + macro
parity (`madc -dM` vs `gcc -dM`) + end-to-end RUN into fulltest. `testcout_realhdr`
is the first end-to-end real-header regression; add `<fstream>` etc. as they land.

---

## 8. METHOD + COMMANDS + GATES (mandatory)

- Read the design corpus (§1) before planning; grep before assuming-missing.
- Per change: reduce → compare g++ AND clang (`-S`/`-fsyntax-only`) → DEEPEST-layer
  fix → rebuild → re-probe → fulltest (known reds only) → torture failset **ALONE**
  (1566/31/57/1) → commit. Validate by RUNNING (or `--dump-cir`), not
  `--emit=c11`-as-truth (it skips c2mir checking).
- Real-header probes: `--std=c++17 --no-embedded-headers`.
- NAS mtime trap: `touch src/<f>.cpp` before `make`; clean-rebuild if results look
  impossible. Cap every run `( ulimit -t 120; timeout 180 … )`, ONE heavy job at a
  time. Background `-v` to a FILE then grep (interactive `-v | grep` truncates and
  has lied about capture counts repeatedly this session).
- Commands:
  ```bash
  bash scripts/resume.sh
  make -C src 2>&1 | grep -iE 'error:|warning:'
  make -C src fulltest 2>&1 | grep -E 'passed,|FAIL:'            # 543/4
  python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc | tail -1  # 1566/31/57/1 ALONE
  bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad   # "This is a test, x = -1"
  bin/madc --std=c++17 --no-embedded-headers tests/test_extern_polymorphic.mad  # must stay PASS
  ```

## 9. OPEN ITEMS
- **Unpushed:** `feature/header-partition-claude` 17 commits ahead of `develop`,
  local only. `develop` also ahead, local. Pending the user's push decision —
  do NOT push without asking.
- `tmp/` has scratch (empty_io.mad, cg.out, reducers) — gitignored.
- The 4 known fulltest reds predate this work (see §2).

## 10. WHY THIS IS NOT A SHIM (the user's standing constraint)
Every discriminator is DATA-DRIVEN (`is_system_header_path`/`is_externally_defined`/
`from_system_header`/path-based), never `namespace=="std"`/name (Rule #7). W2 is
g++'s real model (free operators as ADL candidates, manipulators forwarding the
stream); the mangler GENERATES the libstdc++ symbol (never a hardcoded `_ZSt…`
literal — that drift cost days before). The perf fix is an algorithm change
(O(n²)→O(1)), not a shortcut. Dead code is not emitted (correct), never stubbed.

See `[[project_header_partition]]`, `[[feedback_rule4_check_own_prior_work]]`,
`[[feedback_correct_over_shortcuts]]`, `[[project_template_instantiation]]`.
