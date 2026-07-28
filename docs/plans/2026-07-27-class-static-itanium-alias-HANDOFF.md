# HANDOFF — class statics: storage DONE, the Itanium alias still open

**Status:** step 1 **DONE** @`2b50b0ab` (branch `feature/class-static-alias-claude`,
off develop v0.54.0 @`52b05376`). Step 2 (the alias) **open**.
**KG node:** `Gap{class_static_itanium_alias}`.

---

## Read this first — this handoff's own plan was wrong, and how it was caught

The first two drafts of this document planned to fix the `from_system_header`
placeholder guard by **creating a class static's storage on odr-use** instead of
at its declaration. That plan was wrong, and following it would have shipped a
second shim at the same wrong layer.

It was caught by refusing to build on the handoff's premise. The guard was
deleted **alone**, with nothing else changed, purely to read the diagnostic it
had been hiding:

```
tests/testmadceval.mad:12:38: undeclared identifier locale___S_once
tests/testmadceval.mad:12:38: incompatible argument type for arithmetic type parameter
```

— once per eval call site, on stderr, **while still exiting 0**.

That is not a mangling failure and not a class-static failure. It is
`madc::eval`'s scope capture: the CIR lowering reads every captured variable as
a bare `id()`, bypassing the path that records `referenced_globals` and gets an
`extern` declaration emitted, so capturing anything with **no definition in this
TU** emits a name c2mir never saw. A plain `extern int g;` in scope reproduces
it with no class in the picture.

The rule was already written in `is_runtime_eval_scope_supported_variable`, for
parse-time constants: *"has no declaration in the emitted module — reads of it
FOLD to its value, so the by-name capture would emit an undeclared identifier."*
It simply never covered the other half of its own domain.

**Lesson worth keeping:** the handoff asserted a root cause it had not
instrumented. One bisect build — delete the suspect guard, change nothing else,
read the error — replaced three paragraphs of plan with the actual answer. Do
that before designing, every time, no matter how confident the previous session
sounded.

Second-order note: the test was first placed in the parse-time collector and
then **moved** to the CIR lowering. "Did a definition ever arrive" is a whole-TU
property, and the collector runs at the eval call site with the rest of the file
unparsed, so it would have dropped `extern int g;` … `int g = 5;`. The lowering
runs after the whole parse, where `vfEXTERN` is final.

### What that means for the storage model

madc now creates the decl for **every** static data member with no in-class
initializer, with no origin-keyed exception — matching both canons:

- **gcc:** `finish_static_data_member_decl` (`gcc/cp/decl2.cc:1167`) creates the
  VAR_DECL while parsing the class body, marks it `DECL_IN_AGGR_P`; the
  out-of-class definition completes THAT decl.
- **clang:** `Sema::ActOnCXXMemberDeclarator`
  (`clang/lib/Sema/SemaDeclCXX.cpp:3440`) computes `isInstField` from the
  storage class (:3535) and routes a static through `HandleDeclarator` (:3650).

The `has_inclass_init` guard **stays** — it is g++'s own `DECL_INITIAL`
distinction, and it is what keeps `basic_string<...>::npos` and
`memory_resource::_S_max_align` folding.

**Do NOT reintroduce odr-use creation.** It is unnecessary now, and it would
cost the declared array extent (`static int a[3];` records `member_dims` at the
declaration; an odr-use site only has the element type).

---

## STEP 2 LANDED @`34b2f245` — what it fixed, and what it did NOT

The alias is IN. `class_static_member_itanium_symbol()` sets
`storage_alias_name` for a static of a system-header class, reusing
`itanium_mangle_nested_var` + `split_scope_spelling`. Verified against
`nm -D libstdc++.so.6` (UNANCHORED):

| madc | libstdc++ | |
|---|---|---|
| `_ZNSt8numpunctIcE2idE` | same | ✓ |
| `_ZNSt7__cxx118numpunctIcE2idE` | same | ✓ |
| `_ZNSt10moneypunctIcLb0EE2idE` | same | ✓ (was `…Ic5falseE…`) |

That third one was a SECOND defect found by fixing the first: a NON-TYPE
template argument was mangled as a TYPE. `nontype_literal_code()` now sits
beside `builtin_code()` in the same mangler. **Integers deliberately still fall
through** — Itanium takes the type from the PARAMETER's declaration, so `8` is
`Li8E` for `int` but `Lm8E` for `size_t` (`std::array`, `std::bitset`), and the
mangler receives only a spelling. Fixing that means carrying the parameter's
declared type into the spelling callers pass. Guessing = an invisibly-wrong
symbol instead of a visibly-wrong one.

### The alias is NECESSARY BUT NOT SUFFICIENT — two gaps sit in front of it

Both were found by reducers, after the plan had already assumed they didn't
exist. Neither is exercised by the suite today.

**GAP A — address-of a namespace-qualified template-id static.**
`&std::numpunct<char>::id` → parser error "use of undeclared identifier 'id'".
DISCRIMINATED by a control: `Box<int>::value` (user template, value context)
resolves fine and reaches MIR, failing only with `import of undefined item
Box_int32_t__value` — correct, since the reducer never defines it. So template-id
statics work on the VALUE path; the gap is the ADDRESS-OF path
(`parseAddressOfExpression`, `src/parser.cpp:20655` region), whose qualifier
resolver takes `std` and then expects a plain identifier — it never considers a
template-id. `resolve_template_id_static_member` already does the analogous job
for the value side; the address side needs the same, minus the constant fold
(an address needs real storage). **This is the next concrete fix.**

EXACT MECHANISM, traced 2026-07-28 — start here, do not re-derive:
`Program::parseAddressOfExpression` (`src/parser.cpp:20603`), qualifier arm at
`:20702`. For `&std::numpunct<char>::id` it does:

1. `aname` = `std` -> found in `namespace_map`, so `aclass` stays NULL;
2. consumes `::`;
3. takes the NEXT identifier as the member name — so `member_name` becomes
   `numpunct`, and `<char>::id` is never looked at;
4. `numpunct` is not a variable in `std`, so it falls through to the dlopen
   fallback and dies.

THE FIX: after step 3, if `peekToken()` is `tkLT` and `member_name` names a
class TEMPLATE in that namespace, instantiate the template-id to a
`DataDefCLASS *`, then fall into the EXISTING `aclass` branch (`:20707-20721`)
unchanged — it already resolves a static data member's storage by
`class_static_member_storage_name` and already throws the right diagnostic when
the member is declared but never defined. Do NOT write a second static-member
lookup beside it.

THE ONE PIECE STILL TO LOCATE: the entry point that parses + instantiates a
template-id from an expression-operand position and yields the instantiated
`DataDefCLASS *`. The value side reaches it before calling
`resolve_template_id_static_member(pgm, inst)`, which takes an ALREADY-instantiated
`TokenDataType *inst` — so find that caller and reuse it. Grep the callers of
`resolve_template_id_static_member` first; the instantiation happens immediately
above them.

**GAP B — `std::use_facet` binds to the invented `__ns_std_use_facet`.**
It is declared ONLY as a friend inside `class locale`
(`locale_classes.h:81`); the namespace-scope definition arrives too late at
`locale_classes.tcc:192`. `--dump-registered` shows BOTH `template std::use_facet`
AND a bodyless `function __ns_std_use_facet`, and the call site picks the
placeholder, which has no body → "undeclared identifier __ns_std_use_facet".

⚠️ Do NOT "fix" this by binding `use_facet` mangled-direct on the
`free_function_overloads` path. Checked: libstdc++ has `extern template` for the
facet CLASSES (`locale_facets.tcc:1322`) but **none for `use_facet` itself**, so
the library expects the compiler to instantiate its small inline body — which
references `_Facet::id`, i.e. GAP A. Fix A first; B likely follows.

### VERIFIED FALSE POSITIVE — do not "fix" this

A recon sweep reported that namespace-scope C++ variables in the GLOBAL
namespace never get `storage_alias_name`, because the gate requires
`!current_namespace().empty()` on top of `parsing_extern_decl`, and called that
extra condition unjustified.

It is justified. The Itanium ABI does not mangle a variable at global namespace
scope. Measured against g++ 13:

```
int global_var = 5;
namespace ns { int ns_var = 6; }
    ->   D global_var          (UNMANGLED)
         D _ZN2ns6ns_varE
```

Emitting `_Z…` for a global-scope variable would CREATE the wrong-symbol bug
this track exists to remove. The gate stays.

Kept here because the sweep was right about everything else, which is exactly
what makes a single confident wrong item dangerous.

## SUPERSEDED — step 2 as originally written

Required on EVERY stdlib flavor (see the correction below), and NOT deleted
by step 1.

**⚠️ CORRECTED 2026-07-28 — this is a BOTH-flavors requirement, not a libc++
one.** Earlier drafts said libstdc++'s class statics are private and unexported
while libc++'s are public, and framed step 2 as a libc++ need. That was drawn
from three `_S_*` symbols and does not generalize. The probe that produced it
was also wrong: `nm -D … | grep -E "2idE$"` returns 0 because these symbols
carry `@@GLIBCXX_3.4` version suffixes, so the `$` anchor never matches. Always
probe UNANCHORED, and suspect the probe before the conclusion.

Measured, both unanchored:

| symbol class | libstdc++ 13 | libc++ 18 |
|---|---|---|
| facet `id` objects, exported | **51** (+40 `_ZGV` guards = 91 lines) | **32** |
| `locale::_S_once` | **0** — private (`locale_classes.h:335`), defined in `src/c++98/locale.cc:75` | n/a |

So the split is not libstdc++-vs-libc++, it is **`_S_*` internals vs facet
`id`s**:

- The `_S_*` trio is private, unexported, and never odr-used from any inline
  body (they appear in the headers as declarations only). Nothing should
  reference them; after step 1 nothing does.
- Facet `id`s are **public and exported in BOTH stdlibs**, and
  `std::use_facet<F>(loc)` odr-uses `F::id` for real. This is what step 2 is
  for, on every flavor. It reinforces SETTLED item 5 rather than contradicting
  it.

Live evidence that these are already reachable on a plain libstdc++ build: the
10 names `forest_index_oracle` flagged after step 1 (`numpunct_char__id`,
`codecvt_..._id`, `num_get_..._id`, `num_put_..._id`) are exactly these facet
`id` statics, now instantiated and given storage.

**DO:** set `storage_alias_name` via `itanium_mangle_nested_var` for a static
whose defining class comes from a system header. Do NOT gate it on "is the
symbol exported" by probing the host's `.so` — emit the alias and let the link
fail loudly if the library genuinely lacks it. Probing the host `.so` is exactly
the platform-means-library conflation this track exists to undo.

### SETTLED — verified by reading the source, do not re-derive

1. **The mangler exists and is general.** `itanium_mangle_nested_var(const
   std::vector<std::string> &qualifiers, const std::string &name)` —
   `include/madc_mangle.h:130`, body `src/madc_mangle.cpp:952` →
   `ItaniumMangler::mangle_nested_variable` (:602). An arbitrary qualifier
   chain; a class name is one more qualifier than a namespace.
2. **Template-class statics are NOT a harder subset.** They were called that
   twice and it is wrong: `mangle_nested_variable` runs each qualifier through
   `parse_component` (`src/madc_mangle.cpp:317`), which detects a template-id via
   `split_template_id_parts` and parses its arguments with `parse_type`. The
   exported `moneypunct<char,false>::id` — present in BOTH stdlibs — is the
   COMMON case here, not an edge.
3. **Extern emission already exists.** `cir_builder.cpp:6582` emits `N_EXTERN`
   for a `vfEXTERN` global; `var_emit_name` (`cir_builder.cpp:260`) is the single
   place mapping a `Variable` to its emitted symbol and already reads
   `storage_alias_name`.
4. **`addVariable` already implements g++'s completion semantics** — the
   existing-symbol branch (`src/parser.cpp:18575`) adopts the definition's type
   and count, clears `vfEXTERN`, returns the SAME Variable. `vfEXTERN` is madc's
   `DECL_IN_AGGR_P`.
5. **Not libc++-specific.** Both stdlibs are Itanium; libc++ only adds the `__1`
   inline namespace, which `namespace_qualifiers()` already handles.
6. **Precedent to copy:** `namespace_cpp_variable_symbol`
   (`src/parser.cpp:2112`), wired in `addVariable` at `:18587` / `:18601` — note
   it is gated on `parsing_extern_decl`, which is false at a class-body
   declaration, so the class-static wiring is a new call site, not a condition
   to widen.
7. **Splitting a qualified spelling** is owned by `split_scope_spelling()` in
   `include/spelling_delim.h`. DO NOT hand-roll it — ratchet-gated.

### Gate for step 2

`std::use_facet` under BOTH flavors — it exercises an exported, odr-used,
template-class static (`_ZNSt3__110moneypunctIcLb0EE2idE` and libstdc++'s
`_ZNSt10moneypunctIcLb0EE2idE`). `libcxx_gate.sh` is where the libc++ leg
belongs; the libstdc++ leg is an ordinary integration test, because those 51
exports make it reachable without any flavor flag.

Note also `forest_index_oracle`: once these statics carry an alias they are
still vfINSTPRODUCT, so they stay off the lookup surface (see `91afcd3d`).
Re-run that gate for step 2 — it is not in the targeted lanes.

---

## Tooling changed under you — read this before running anything

`scripts/remote_build.sh` @`7f5f8524`:

- It **always** writes `tmp/logs/rb-<stamp>.log`. Do not pipe it through `tail`;
  that is what cost a full battery on 2026-07-28. `grep` the log.
- It prints a **stage summary** naming every stage's rc. `total rc=1` on its own
  is no longer possible.
- `TESTS='<glob> …' remote_build.sh sync build tests` runs a SUBSET (JIT);
  `tests-all` runs the subset across JIT + exe + obj. **Scope the inner loop to
  the blast radius; the full battery is the pre-merge gate, not the default.**
- A filtered run prints `SUBSET RUN — … NOT a suite baseline`. Never quote one
  as a baseline.

⚠️ **A reducer could not have caught the failure step 1 fixed.** Two of three
batteries on `cff57761` went red on it while all 45 reducers in `tmp/udecl/`
stayed green — a user class's static always has its definition in the same file,
so the shape is unreachable from a reducer. What made it *detectable* was
`.expect_quiet`: those four `testmadceval*` tests had none, so diagnostics on
stderr with exit 0 passed on stdout alone. All five carry one now.

---

## GAP C — free function templates at GLOBAL scope (NEW, open, sized as NOT small)

`template<typename T> T ident(T x) { return x; }` at file scope, called as
`ident(7)`, reports **"use of undeclared identifier 'ident'"**. The IDENTICAL
template inside `namespace nn`, called as `nn::ident(7)`, works. Deduced and
explicit forms both fail. The suite does not cover this shape at all.

TRACED, and the first two hypotheses were WRONG — do not repeat them:

1. `register_skipped_namespace_template_function` (`src/parser.cpp:40697`) opens
   with `if ( pgm.current_namespace().empty() ) return;`.
2. The instantiation lookup (`src/parser.cpp:43436`) bails on
   `fd->namespace_name.empty()`.
3. The map key is `ns + "::" + name`, i.e. `"::ident"` at global scope.

I relaxed (1) and (2) together, rebuilt, and **NOTHING CHANGED** — all five
reducers failed identically. So `register_skipped_namespace_template_function`
is not even REACHED for a global-scope template; the decision is further
upstream, in whatever routes a template declaration to the "skipped" path
(its only call site is `src/parser.cpp:46418`, guarded merely by
`!pgm.deferred_function_body_sink`). Those edits were REVERTED rather than
shipped — they are unverified and had no effect. Start by finding what decides
a global-scope template declaration is skipped/deferred in the first place.

SIZING: this is NOT the "adopt an existing owner" shape the other fixes in this
release had. `namespace_name.empty()` / `current_namespace().empty()` appears
**53 times** in parser.cpp; "a function template lives in a namespace" is a
design assumption threaded through the front end, not one missing case. Budget
accordingly, and expect the layer count to be >2.

NOT a blocker for `use_facet` (GAP B) — that one is namespace-scoped already.

## Also open, unrelated

`Gap{nested_tag_not_scoped_to_struct_body}` — two scopes each declaring a
`struct Inner` collide. Plan in `claude_status.json` handoff #29: valid C cannot
produce the collision, so no `--std=` gate is needed, and `parser.cpp:31285`
already qualifies a nested tag's store key — it just requires
`class_scope_stack` non-empty, and only `TokenCLASS::parse` pushes it.
