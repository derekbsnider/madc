# HANDOFF — class static data members: create on odr-use, alias when exported

**Status:** open, NEXT. **Branch:** cut a fresh one off `develop`
(`feature/class-static-alias-claude`); develop is at v0.54.0 @`52b05376`.
**KG node:** `Gap{class_static_itanium_alias}`.

This is an imperative handoff. Where it says DO, do that; where it says DO NOT,
that path has already been measured and rejected.

---

## Read this first — the obvious plan is WRONG, and one command proves it

The plan this task started from was: "mangle class statics to their Itanium
symbol so a reference resolves against libstdc++." That is **not sufficient,
and for the motivating symbols it is impossible.**

```
$ nm -D /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep _ZNSt6locale7_S_onceE
$                                  # empty — not exported
$ nm -D /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep _ZSt4cout
000000000027a580 B _ZSt4cout@@GLIBCXX_3.4
```

libstdc++ exports 620 data symbols, and `std::cout` is one of them — so the
alias mechanism itself is sound. But `std::locale::_S_once`,
`std::locale::id::_S_refcount` and `std::ios_base::Init::_S_synced_with_stdio`
are **hidden**. There is nothing to link against, for madc or for anyone.

**And nothing should be trying to.** g++ compiling `#include <locale>` emits no
reference to `_S_once`: only libstdc++'s own internals odr-use it. A
declaration alone emits nothing. madc's error was never a missing alias — it
was **emitting a reference to a static member that nothing in the program
uses.**

---

## The actual defect

`cff57761` made every static data member DECLARATION create a storage
`Variable`. That is right for a member the program uses and wrong for the
hundreds a system header merely declares — those then reached emission and
produced `undeclared identifier locale___S_once`.

Two placeholder guards were added to contain that, and this task removes the
second:

- `has_inclass_init` — **KEEP.** g++'s own `DECL_INITIAL` distinction
  (`decl_constant_value`, `gcc/cp/init.cc:2582`). A member with an in-class
  initializer folds.
- `from_system_header` — **DELETE.** Labelled a placeholder in the code, the
  commit, the changelog and the release notes. Removing it is the acceptance
  test.

---

## DO — create the storage on odr-use, not at declaration

Move the creation from the declaration site into
`resolve_class_static_member_value` (`src/parser.cpp:2567`), which is already
THE owner of the constant-vs-storage choice and is reached exactly when a
member is actually *referenced*:

1. It already prefers an existing storage `Variable` and folds a real in-class
   constant. Add one branch: when there is no in-class constant **and** no
   storage Variable exists, CREATE it there (with `vfEXTERN`, via
   `class_static_member_storage_name`) and return a `TokenVar` to it.
2. **Remove the creation from `TokenCLASS::parse` entirely**, along with the
   `from_system_header` guard. The declaration goes back to recording only
   `static_member_types` and any constant.

Why this is the right layer, not a relocation of the same hack: a reference is
precisely an odr-use, so nothing is created for a member nobody touches —
system-header or not, with no origin-keyed test anywhere. A user class's member
is created when its body reads it, and the out-of-class definition still
completes that same Variable through `addVariable`'s existing-symbol branch
(adopt type/count, clear `vfEXTERN`). If a program genuinely odr-uses a hidden
library static, it gets a link error — which is what g++ gives too.

3. **Then** set `storage_alias_name` for the case where it IS correct: a static
   whose defining class comes from a system header AND whose mangled symbol is
   exported. Use `itanium_mangle_nested_var` — see SETTLED below. Treat this as
   a second, separate commit; step 1–2 stands on its own and is what deletes
   the guard.

---

## SETTLED — verified by reading the source on 2026-07-27, do not re-derive

1. **The mangler exists and is general.**
   `itanium_mangle_nested_var(const std::vector<std::string> &qualifiers,
   const std::string &name)` — `include/madc_mangle.h:130`, body
   `src/madc_mangle.cpp:952` → `ItaniumMangler::mangle_nested_variable` (:602).
   An arbitrary qualifier chain; a class name is one more qualifier than a
   namespace.

2. **Template-class statics are NOT a harder subset.** I claimed twice that they
   would need template-argument mangling. They do not: `mangle_nested_variable`
   runs each qualifier through `parse_component` (`src/madc_mangle.cpp:317`),
   which detects a template-id with `split_template_id_parts` and parses its
   arguments via `parse_type`.

3. **Extern emission already exists.** `cir_builder.cpp:6582` emits `N_EXTERN`
   for a `vfEXTERN` global; `cir_builder.cpp:270` is the single place mapping a
   `Variable` to its emitted symbol and already reads `storage_alias_name`.

4. **`addVariable` already implements g++'s completion semantics** — the
   existing-symbol branch (`src/parser.cpp:18575`) adopts the definition's type
   and count, clears `vfEXTERN`, and returns the SAME Variable. `vfEXTERN` is
   madc's `DECL_IN_AGGR_P`.

5. **Not libc++-specific.** libstdc++ and libc++ are both Itanium; libc++ only
   adds the `__1` inline namespace to the chain, which `namespace_qualifiers()`
   already handles.

6. **Precedent to copy:** `namespace_cpp_variable_symbol`
   (`src/parser.cpp:2112`), wired in `addVariable` at `:18587` / `:18601`.

7. **Splitting a qualified spelling** is owned by `split_scope_spelling()` in
   `include/spelling_delim.h`. DO NOT hand-roll it — that family was
   consolidated this release and is ratchet-gated.

---

## Gate

- The four tests that exposed the guard must pass with it gone:
  `tests/testmadceval.mad`, `testmadcevalexpr.mad`,
  `testmadcevalexprtyped.mad`, `testmadcevalscope.mad`.
- `tests/teststaticmemberstorage.mad` must stay byte-identical to g++.
- Add a case that reads an **exported** system-header static (`std::cout` is
  the proven-exported example) if step 3 is done.

⚠️ **A reducer cannot catch the failure this task is about.** Two of the three
batteries on `cff57761` went red on exactly it while all 45 reducers in
`tmp/udecl/` stayed green — a user class's static always has its definition in
the same file, so the shape is structurally unreachable from a reducer. Run
`bash scripts/remote_build.sh battery` and expect it to be what finds the
problem.

---

## Method note worth keeping

The premise of this handoff's first draft was killed by a single `nm -D`. Run
the cheap existence check on a symbol you intend to link against **before**
designing around it — a plan built on an assumed symbol is a plan that compiles
in your head and nowhere else.

---

## Also open, unrelated

`Gap{nested_tag_not_scoped_to_struct_body}` — two scopes each declaring a
`struct Inner` collide. Plan recorded in `claude_status.json` handoff #29:
valid C cannot produce the collision, so no `--std=` gate is needed, and
`parser.cpp:31285` already qualifies a nested tag's store key — it just
requires `class_scope_stack` non-empty, and only `TokenCLASS::parse` pushes it.
