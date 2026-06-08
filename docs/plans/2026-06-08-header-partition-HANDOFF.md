# madc HEADER-PARTITION HANDOFF (2026-06-08, read-first)

Cold-start rehydration for branch **`feature/header-partition-claude`**. Assume
you remember nothing. Read this, then the linked artifacts.

## 0. ONE-SCREEN STATE
- **Repo** `/workspace/madc`, **branch `feature/header-partition-claude`**
  (off `develop`), **HEAD `bc5e6cd`**, working tree clean. NOT pushed.
- **Gates (green):** fulltest **540 / 4** (pre-existing reds only: testdefer,
  testfstream, testlargesizeofquery, testloop); gcc.c-torture **1566/31/57/1**;
  MIR pin `2ffebff`. Both this-session commits hold these.
- **Approved plan:** `~/.claude/plans/clever-scribbling-dove.md` (the FULL
  header-partition plan — milestones B1-B6, R1-R5, M, A). READ IT.
- **Governing design doc:** `madc-header-partition-handoff.md` (repo root).
- **Project memory:** `project_header_partition` (in ~/.claude memory) — the
  living campaign note; has the per-fix detail + remaining-work anchors.
- **develop** = `4ed8fea` (123 ahead of origin/develop, NOT pushed): carries the
  whole realhdr trait/template arc + this session's `.json`-default,
  manifest-relative-paths, and `--no-auto-load`. develop/master NOT promoted.
- **MadSMAUG** (`/workspace/MadSMAUG`, symlinked): NEW **`develop`** branch
  `0d49fd7` boots SMAUG via a checked-in `compile_commands.json` + thin
  `MadSMAUG.sh` (no umbrella, needs only a madc binary). master untouched
  (ahead 2 of origin, pre-existing). NOT pushed.

## 1. THE MISSION (do not re-derive — these are settled user decisions)
Make madc consume the **REAL** glibc/libstdc++ headers instead of its hand-tooled
stdlib shims. The "correct mix", baked into `libmadc.a` as ONE pre-LEXED +
compressed package:
- **KEEP (madc-owned, in repo):** compiler freestanding headers (stddef/stdarg/
  stdbool/stdalign/iso646/intrinsics + bucket-2 `#include_next` shims) AND madc's
  OWN libs (`ns_php`/`ns_perl`/`ns_python`/`ns_ruby`/`ns_js`/`ns_rust` + `.h`,
  `<algorithm>` helpers). These ALWAYS win over GCC's.
- **RETIRE (legacy cruft):** `include/madc/{iostream,sstream,fstream,string,
  vector,map,set,typeinfo}` — the hand-tooled stdlib reimplementations (printf-
  bridge iostream, 4 KB sstream, fstream stub).
- **ADD (build-host, unmodified, embedded):** the C + C++ stdlib transitive
  closure (glibc + libstdc++ + bits/).

**LOAD-BEARING USER STEERS (violating these = redoing work):**
1. **NO method wiring / NO madc-authored class bodies.** Every C++ class comes
   from the real `#include`. Do NOT "finish" the hand-tooled shims.
2. **Pre-LEX ALL headers uniformly** (raw token stream, directives preserved,
   macros NOT expanded), NOT `gcc -E`'d. Preprocessing happens at include time
   against the real context. (Today's `.madh` PCH wrongly `gcc -E`'s first; that's
   why it can't be used generally.)
3. **Data-driven discovery only (Rule #7).** Partition via
   `gcc -print-file-name=include`; closure via `g++ -M`; macros via `gcc -dM`.
   No hardcoded header/macro lists, no per-class special-casing.
4. **Embed BOTH sets** (madc-owned + system) as one compressed package; runtime
   uses ONLY the package (never reads GCC's compiler headers).

**REALITY CHECK (the user corrected me on this — internalize it):** the prior
"massive round of C++ header-parsing support" got us FAR. 11 real libstdc++
headers PARSE at `--std=c++17 --no-embedded-headers`. Do NOT get confused by the
legacy `include/madc/` shims — they are NOT the real-header state. We are CLOSE.

## 2. WHAT LANDED THIS SESSION
On `feature/header-partition-claude` (the 2 R5 fixes — both deepest-layer, both
C-inert, both gated fulltest 540/4 + torture 1566/31/57/1):
- **`4fa746e` — R2 was MIS-FRAMED; the real fix is mangling.** Real-header
  programs were NOT failing because libstdc++ wasn't loaded — `bin/madc` already
  links `libstdc++.so.6`. The blocker: `std::` free functions mangled as
  `_ZNSt9terminateEv` (non-canonical `N St … E`) instead of `_ZSt9terminatev`
  (Itanium `St` unscoped-name abbreviation), so `dlsym` missed a symbol already
  in the process. Fix in `src/madc_mangle.cpp` `mangle_nested_function`:
  single-`std`-qualifier → `_ZSt<name>…`. Real `<type_traits>` now
  compiles+links+RUNS end-to-end (no `-l`, no auto-load). Lesson: VERIFY before
  assuming (the handoff's "libstdc++ not loaded" diagnosis was wrong).
- **`bc5e6cd` — R5/C1: class-scope alias leak.** `typedef _CharT char_type;` was
  added to the GLOBAL `user_typedef_names` set (role: "emit verbatim as a
  type-spec") but is never emitted as a top-level C typedef → param/return/member
  emission leaked bare `char_type` → c2mir "unknown type". Fix: keep class-scope
  aliases (typedef @`parser.cpp:18573`, using @`14608`) OUT of
  `user_typedef_names` (guard `class_scope_stack.empty()`); they resolve via
  `type_aliases`; use-sites emit the resolved underlying type. Fixes all leak
  sites at once. Reducer: `tmp/ct_reducer.mad`.

Earlier this session (now on develop, see §0): `.json`→`--project` default
(`2f49eeb`), relative-manifest-path resolution (`aae428f`), `--no-auto-load`
(`4ed8fea`); MadSMAUG develop branch with committed `compile_commands.json`.

## 3. THE NEXT WORK (precise, with anchors — from read-only Explore recon)
The CURRENT first wall for a real `<iostream>`/`<fstream>` program is c2mir
**"repeated declaration"** (char_type is the NEXT wall, only reachable after).
Repro: `bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad 2>&1 |
grep -i "repeated declaration" | head`. TWO independent emission-layer causes:

**Cause A — typedef alias name collision** (`string`/`u16string`/`u32string`/
`wstring`): `std::string` (bits/stringfwd.h) AND `std::pmr::string` (string:66)
both register the BARE alias `string` (`record_typedef` @`parser.cpp:~14990`,
`td.name = alias` @`~15001` — unqualified; namespace only in
`namespace_datatype_map`). cir_builder emits `typedef_decl(td.name)` @
`cir_builder.cpp:~8126` with no dedup → two `typedef X string;` → c2mir repeated.
FIX: namespace-qualify the emitted typedef NAME so std vs std::pmr are distinct C
identifiers. **FIRST verify** how the typedef name vs the underlying (already
distinctly-mangled) struct tag is USED in emitted C — uses may already resolve to
the struct tag, making the bare typedef vestigial/collision-only (then the fix
can dedup/qualify at emission safely). Use `--dump-cir` / `--emit=c11`.

**Cause B — diamond/virtual-base member duplication** (`_M_width`/`_M_precision`/
… emitted twice within one struct): the base-member flatten loop @
`parser.cpp:16937-16951` copies each base's members once PER inheritance path.
`basic_iostream<char> : basic_istream<char>, basic_ostream<char>` — both already
carry the virtual-base `basic_ios`/`ios_base` members → shared vbase members land
twice in `ddc->members`. Offsets ARE deduped (`compute_layout`/`collect_vbases`
@`~6129`), but the members VECTOR is not. cir_builder `struct_def` @
`cir_builder.cpp:2356` emits each → c2mir repeated. FIX: dedup the flattened
members by VIRTUAL-BASE CLOSURE (mirror `collect_vbases`), NOT naive name dedup
(distinct non-virtual bases may legitimately share member names). **HIGH BLAST
RADIUS** — the MI feature is "complete" with tests; verify MI tests
(test_rtti_*, test_vdtor_*, MI tests) + g++ layout. Do this carefully/fresh.

**After Wall 1 clears:** char_type's downstream — istream `:732`/`:1040` c2mir
errors (incompatible pointer types, "lvalue required as unary & operand", "too
few arguments"). Then **§7** (string_view/memory): namespace-qualified template-id
in a static-const captured DURING instantiation (`std::__are_same<float,float>::
__value`) — the `::Name` segment is dropped, **silently folds to 0** (a "parses
but WRONG VALUE" hazard); fix in `capture_constant_initializer_value`
@`parser.cpp:~5315`, upstream of the proven-correct `fold_constant_qualified_member`
@`~4938`. Then **§8** (`_S_categories_size` class-scoped enum visibility in
ostream/istream). Sequence + deeper §7/§8 detail: `docs/plans/2026-06-08-FULL-
rehydration-handoff.md` §6/§7/§8/§10/§11.

Then the rest of the plan: R1 (per-std `__cplusplus` + `__has_include`/
`__has_builtin` operators; `__cplusplus` is FROZEN at 201703L), R3 (retire the
`array` keyword @`datatokens.h:48` so `std::array` parses), R4 (trait builtins,
correctness-first), the B-half packaging (the actual partition), and incremental
shim retirement (M, gated on `is_cpp_mode()`; fix the 3 fstream reds).

## 4. METHOD (mandatory)
- **Hybrid that worked:** read-only Explore subagents (opus) for RECON/localizing
  gaps; do the actual FIXES yourself (correctness-critical, iterative, hottest
  paths — esp. the §7 silent-0 hazard). Do NOT delegate edits to one-shot agents.
- Per fix: reduce → compare gcc/clang/`c2m FILE -ei/-eg` (gcc-pass & clang-pass &
  c2m-fail = c2mir bug; else madc bug) → DEEPEST-layer fix → rebuild → re-probe →
  fulltest (only the 4 known reds) → torture failset **run ALONE** (1566/31/57/1)
  → commit. For madc-path bugs use `--emit=c11`/`--dump-cir`, NOT emit-C-as-truth.
- **`--emit=c11` parses but skips c2mir checking** — c2mir "repeated declaration"
  / "unknown type" errors ONLY appear on the RUN path. So validate fixes by
  RUNNING (or `--dump-cir`), not just `--emit=c11`.
- Real-header probes: `--std=c++17 --no-embedded-headers` (default STD_MADC does
  NOT define `__cplusplus` → real headers fail to parse, `_GLIBCXX_*` unexpanded).
- Reducers go in `tmp/` (gitignored). Cap every run `( ulimit -t 120; timeout 180
  … )`, ONE heavy job at a time. NAS mtime trap: `touch src/<f>.cpp` before make;
  clean-rebuild if results look impossible.

## 5. EXACT COMMANDS
```bash
cd /workspace/madc
git rev-parse --short HEAD                                   # bc5e6cd
make -C src fulltest 2>&1 | grep "passed,"                   # 540/4
python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc | tail -1  # 1566/31/57/1 (ALONE)
# Real-header walls (current first wall = repeated declaration):
bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad 2>&1 | grep -i "repeated declaration" | head
# Proof the symbol wall is gone (runs, exit 0):
printf '#include <type_traits>\nint main(){return 0;}\n' > tmp/tt.mad
bin/madc --std=c++17 --no-embedded-headers tmp/tt.mad ; echo $?
# char_type fix reducer (runs, exit 0):
bin/madc tmp/ct_reducer.mad ; echo $?
```

## 6. OPEN ITEMS
- Push `feature/header-partition-claude` (2 commits) / `develop` (123 ahead) /
  MadSMAUG `develop` — all local only; PENDING user decision.
- Next increment: Cause A → Cause B (clears repeated-decl) → istream downstream.

## 7. KEY ANCHORS
- `src/madc_mangle.cpp` `mangle_nested_function` (~514, std `St` fix landed).
- `src/parser.cpp`: `user_typedef_names` inserts (14608 using, ~14990 typedef,
  18573 lambda — class-scope guard landed); base-member flatten (16937-16951,
  Cause B); `record_typedef` td.name (~15001, Cause A); `capture_constant_
  initializer_value` (~5315, §7); `fold_constant_qualified_member` (~4938);
  `set_language_standard` (~6527, R1); trait table (~4290/4340, R4); `array`
  (~7638, R3).
- `src/cir_builder.cpp`: typedef emit (~8126, Cause A); `struct_def` (~2356,
  Cause B); type-spec emit `type_list`/`append_lit_type_spec` (~1034/1590).
- `src/madc_cir.cpp`: `cir_import_resolver` (~74) — bare dlsym(RTLD_DEFAULT);
  auto-load of libs madc does NOT already link may be wanted later (NOT the
  real-header blocker; that was mangling).

END OF HANDOFF.
