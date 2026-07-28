# HANDOFF — Itanium symbols for class statics; ONE OPEN REGRESSION blocks merge

**Branch:** `feature/class-static-alias-claude` @`ddf93b82` (off `develop` v0.54.0
@`52b05376`). Tree clean, pushed: NO — nothing on this branch is pushed yet.
**Container:** built at branch HEAD, idle.
**Supersedes** `docs/plans/2026-07-27-class-static-itanium-alias-HANDOFF.md`
(still accurate for the SETTLED mangler facts; read this one first).

This is an imperative handoff. Where it says DO, do that. Where it says DO NOT,
that path has been measured and rejected — the measurement is given so you do
not have to repeat it.

---

## 0. ~~READ THIS FIRST — the branch does not merge~~ FIXED @`a81f86e1`

**RESOLVED 2026-07-28.** Root cause was NEITHER of the two leads below:
`class_ctor_call` (and `global_ctor_call`'s built-in arm) emitted the ctor
RECEIVER by raw `v->name`, bypassing `var_emit_name` — so an aliased global
(a system-header class static completed by its out-of-class definition) was
DEFINED under the Itanium name but CONSTRUCTED under the invented one. The
emitted C showed it directly: `struct strong_ordering
_ZNSt15strong_ordering4lessE;` (definition, aliased) vs
`strong_ordering__strong_ordering((&strong_ordering__less), -1);` (ctor,
raw). All seven receiver sites now route through `var_emit_name` — the
sixth instance of this session's "one owner, half its consumers" shape.
Lead 1 was wrong (the alias IS applied; `canonical_cpp_spelling` is fine
for `strong_ordering`); lead 2 was moot (storage is defined in-module, no
extern needed). The follow-up warning-ratchet item it surfaced (dlfcn
builtin typing) is fixed in `1ad16f46`; the DEEPER layer that fix exposed
is recorded as `Gap{builtin_redecl_half_adopt}` — see §3.

Original blocker text kept for the record:

`34b2f245` introduced a regression in **four** tests. They are red at branch
HEAD. Everything else is green. **Fix this before anything else, and before any
merge or release.**

```
testcompare_realhdr   testcompareops_realhdr
testdefaultedcmp_realhdr   testrewritten_realhdr
```

Bisected with the harness (see §4 — do NOT run these bare):

| commit | result |
|---|---|
| `681275c2` (code == `91afcd3d`, last green battery) | **4 passed** |
| `34b2f245` (class statics → real Itanium symbol) | **0 passed, 4 failed** |

### The real diagnostic

```
/usr/include/c++/13/compare:162:35: undeclared identifier partial_ordering__unordered
/usr/include/c++/13/compare:342:34: undeclared identifier strong_ordering__less
/usr/include/c++/13/compare:345:34: undeclared identifier strong_ordering__equal
/usr/include/c++/13/compare:351:34: undeclared identifier strong_ordering__greater
   ... each followed by "lvalue required as unary & operand"
```

The name is the **invented** `strong_ordering__less`, NOT a mangled
`_ZNSt14strong_ordering4lessE`.

**DO NOT** pursue "the alias is wrongly applied to these" — that was my first
hypothesis and the name in the error refutes it. The alias is NOT applied. These
system-header class statics end up with storage but **neither an alias nor an
extern declaration**, and the reference is an address-of (`&`) inside `<compare>`
itself, at its own out-of-class definition site.

### Check in this order

1. **Why `class_static_member_itanium_symbol()` declines them.**
   `src/parser.cpp` (helper defined just below
   `class_static_member_storage_name`). `from_system_header` is true for
   `<compare>`, so the likely answer is an empty `canonical_cpp_spelling()` on
   `strong_ordering` / `partial_ordering`. If so, decide whether the correct
   behaviour is (a) give them a spelling, or (b) do not create storage for a
   system-header static we cannot name — and say which in the commit.
2. **Why no extern decl is emitted.** `note_global_reference()`
   (`src/cir_builder.cpp`, declared in `src/cir_builder.h`) did NOT exist at
   `34b2f245` — it arrived one commit later in `88a6ab7b`, and it is exactly the
   thing that makes an address-of record a global reference.
   **⚠️ RE-RUN THE FOUR TESTS AT BRANCH HEAD FIRST.** The failure's shape may
   already differ from the `34b2f245` diagnosis above.

```
TESTS="testcompare_realhdr testcompareops_realhdr testdefaultedcmp_realhdr testrewritten_realhdr" \
    bash scripts/remote_build.sh sync build tests
```

---

## 1. What landed (15 commits, all green except §0)

| commit | what |
|---|---|
| `7f5f8524` | **tooling** — self-logging, per-stage rc summary, `TESTS=` subset runs |
| `2b50b0ab` | eval scope capture; deletes the `from_system_header` placeholder guard |
| `91afcd3d` | instantiation products are not a lookup surface (`vfINSTPRODUCT`) |
| `34b2f245` | class statics → real Itanium symbol; non-type template args → literals ⚠️ **carries the regression** |
| `88a6ab7b` | `&Class<T>::static` resolves and binds to the real symbol |
| `1c44c10d` | explicit template args are part of the postfix head |
| `c3d7fbe1` | function templates at global scope |
| others | docs / status / mirrors |

Baseline moved 769/753/753 → **770/754/754** (green battery at `91afcd3d`),
then the four regressions above appeared at `34b2f245`.

**New gates:** `testevalexterncapture`, `testclassstaticitanium`,
`testglobalfntemplate`, plus `.expect_quiet` on the four `testmadceval*` tests
(they had none, so diagnostics on stderr with exit 0 passed on stdout alone).

---

## 2. The through-line: one rule, half its domain

**Five** defects this session had the same shape — a rule written once and
applied to only part of what it governs. Every fix was "adopt the existing
owner", never new machinery:

1. `is_runtime_eval_scope_supported_variable` stated "no declaration in the
   emitted module ⇒ not capturable by name" and covered only constants.
2. `dump_registered_names` excluded `Class__member` for METHODS but not for
   static DATA members.
3. `TokenVar` recorded a global reference; `TokenAddrOf` emitted the name and
   recorded nothing → extracted `note_global_reference()` as one owner.
4. The postfix chain handled `(` `[` `.` `->` but not the template-argument list.
5. `namespace_function_symbol` invented `__ns__name` for the GLOBAL namespace,
   which nothing looks up.

**DO run `/dupaudit` scoped to the expression-parsing paths before merging.**
Five instances in one session is the signal that command exists for.

Related, still true and still unfixed: `namespace_function_symbol` also carries
a **divergent copy of the Itanium operator table** (`_lt`, `_pl`, `_ix`, `_cl`)
duplicating `operator_code()` in `src/madc_mangle.cpp:150`. One rule, two
implementations, one non-canon by construction.

---

## 3. Open work, in priority order

1. ~~**THE REGRESSION** (§0). Blocks merge.~~ FIXED @`a81f86e1` (+`1ad16f46`).
1b. **`Gap{builtin_redecl_half_adopt}`** — a real-header re-declaration of a
   builtin-registered function HALF-applies: parseFunction's return-type
   refresh (parser.cpp:47541) keeps the OLD parameter list (param-push gated
   on `!func_already_declared`, 48175). Live failure: `getenv("HOME")` under
   `--no-embedded-headers` errors "expected 2 got 1" (`__madc_getenv`'s
   2-param result-buffer convention shadows the header's 1-param prototype);
   g++ compiles it. Reducer: `tmp/getenv_realhdr.mad`. The fix is TWO layers
   and needs its own battery: adopt the header prototype wholesale (gcc
   canon: an explicit prototype replaces a builtin) AND bind the call to the
   real libc symbol, not the madc wrapper. SMAUG calls getenv — this blocks
   real C89 code under real headers.
2. **GAP B — `std::use_facet` binds to the invented `__ns_std_use_facet`.**
   Characterised by a clean 2×2 (all four measured):

   | | deducible from args | non-deducible |
   |---|---|---|
   | user template | works | **works** (`make<int>(9)`) |
   | system header | works (`std::max<int>`, `std::forward<int>`) | **FAILS** (`use_facet`, `has_facet`, `__try_use_facet`) |

   So: *a free function template declared in a system header whose template
   parameter is not deducible from its arguments falls back to the bodyless
   `__ns_` placeholder instead of instantiating its retained body.*
   Mechanism: system-header templates go through
   `capture_free_function_overload` into `free_function_overloads` for
   mangled-direct selection, which ranks by ARGUMENT TYPES; with the parameter
   absent from the parameter list there is nothing to rank on.
   `itanium_mangle_std_free_template` already TAKES a template-args vector.
   The 2×2 is the gate: the failing cell must go green, the other three stay.
   **DO NOT** bind `use_facet` mangled-direct — libstdc++ has `extern template`
   for the facet CLASSES (`locale_facets.tcc:1322`) but **none** for
   `use_facet`, so the library expects its body instantiated.
   Estimate 1–2 commits; see §5 before trusting that.
3. **`Gap{nested_tag_not_scoped_to_struct_body}`** — two scopes each declaring
   `struct Inner` collide. Plan in `claude_status.json` handoff #29.
4. **`include/madc/ns_php`** declares extern-C `__php_*` in the SCRIPT header
   while a real `namespace php {}` exists in `src/ns_php.cpp:476`. Against
   `.claude/rules/cpp-first-api.md`. Owner's direction: `libmadc.so` exposes
   BOTH the C++ mangled names AND C-friendly wrappers, but the wrappers are an
   export convenience for C hosts — never the script-side resolution path.
5. `/promote` — master is at v0.48.0, develop at v0.54.0.

---

## 4. ⚠️ METHOD — two failures that cost real time

**A tests/ file is NOT a reducer.** The four `_realhdr` tests carry
`tests/*.flags` = `--std=c++20 --no-embedded-headers`. I ran them as bare
`bin/madc tests/x.mad` for ~6 builds; every run failed for the wrong reason, the
bisect measured nothing, it wrongly implicated the global-template fix, and I
narrowed that guard three times against meaningless evidence. Run tests through
`run_tests.sh` / `TESTS='<glob>' remote_build.sh tests`, which applies
`.flags` / `.input` / `.expect`. Reserve bare `bin/madc` for `tmp/` reducers.

> The tell was there early: **the same code both passed a battery and failed a
> bare run.** When that happens the HARNESS is wrong, not the code.

**Never stack code commits without a green battery between them.** The battery
for `1c44c10d` was aborted (correctly — I was about to change the tree under
it), and I then committed twice more without re-running. That is why a
regression from `34b2f245` was not caught until four commits later.

---

## 5. SETTLED — verified, do not re-derive

1. **Estimates ran conservative all session.** I called GAP C "not small, budget
   >2 layers, needs fresh context"; it was ~40 lines. I stopped twice at
   "near my context ceiling" with 37–44% remaining. Weight my sizing accordingly.
2. **The mangler is general.** `itanium_mangle_nested_var(qualifiers, name)`
   (`include/madc_mangle.h:130`) takes an arbitrary chain; a class name is one
   more qualifier than a namespace. Template-class statics are NOT a harder
   subset — `parse_component` handles template-ids.
3. **Non-type template args**: `nontype_literal_code()` handles `true`/`false`
   (`Lb1E`/`Lb0E`). **Integers deliberately fall through** — Itanium takes the
   type from the PARAMETER's declaration, so `8` is `Li8E` for `int` but `Lm8E`
   for `size_t` (`std::array`, `std::bitset`), and the mangler receives only a
   spelling. Fixing it means carrying the parameter's declared type into the
   spelling callers pass.
4. **Both stdlibs export facet `id` statics** — libstdc++ 13 exports **51**,
   libc++ 18 exports **32**. The alias is a BOTH-flavors requirement, not
   libc++-only. The `_S_*` trio (`locale::_S_once` etc.) is private, unexported
   and never odr-used — correctly referenced by nothing.
5. **Probe UNANCHORED.** `nm -D … | grep -E "2idE$"` returns 0 because these
   symbols carry `@@GLIBCXX_3.4` suffixes. A zero result is the answer most
   likely to be an artifact; use a positive control.
6. **Global-namespace variables are NOT mangled.** g++ emits `D global_var` and
   `D _ZN2ns6ns_varE`. A recon sweep reported the `!current_namespace().empty()`
   gate as unjustified — it is a **FALSE POSITIVE**; "fixing" it would create a
   wrong symbol.
7. **`vfINSTPRODUCT`** (fresh bit 1048576; 65536 is retired) marks anything
   minted at `_inst_depth > 0`, keeping it off the lookup surface. Same rule
   `pack_tap_name` already applied to the decl index.
8. Global-scope function templates: registration is gated to **non-operator,
   non-system-header** declarations. Both restrictions were established by
   bisect — removing either breaks `<compare>`.

---

## 6. Tooling changed under you — `scripts/remote_build.sh` @`7f5f8524`

- **Always** writes `tmp/logs/rb-<stamp>.log` and prints the path. **Never pipe
  it through `tail`** — that cost a full ~30-minute battery on 2026-07-28.
  `grep` the log.
- Prints a **stage summary** naming every stage's rc. `total rc=1` can no longer
  be anonymous — that is what made the `forest_index_oracle` failure findable
  while all 770 tests passed.
- `TESTS='<glob> …' remote_build.sh sync build tests` (JIT) or `tests-all`
  (JIT + exe + obj) for the inner loop. **Full battery is the pre-merge gate,
  not the default.**
- A filtered run prints `SUBSET RUN — … NOT a suite baseline`. Never quote one
  as a baseline.
- `forest_index_oracle` and `libcxx_gate` are NOT in the targeted lanes. Run the
  oracle explicitly after anything touching symbols, registration or the forest:
  `ssh -p 2299 dev@localhost 'cd /workspace/madc; bash scripts/forest_index_oracle.sh'`
