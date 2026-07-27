# HANDOFF — class static data members: create on odr-use, alias when exported

**Status:** open, NEXT. **Branch:** cut a fresh one off `develop`
(`feature/class-static-alias-claude`); develop is at v0.54.0 @`52b05376`.
**KG node:** `Gap{class_static_itanium_alias}`.

This is an imperative handoff. Where it says DO, do that; where it says DO NOT,
that path has already been measured and rejected.

---

## Read this first — BOTH halves are needed, and the two stdlibs need opposite things

This section was rewritten after source recon into `/workspace/gcc`,
`/workspace/llvm-clang-src` and the container's libc++. The first draft was
based on libstdc++ evidence alone and reached a conclusion that is only half
right.

### The model is confirmed by BOTH canons

- **gcc:** `finish_static_data_member_decl` (`gcc/cp/decl2.cc:1167`) creates the
  VAR_DECL while parsing the class body, marks it `DECL_IN_AGGR_P`, pushes it to
  `pending_statics`; the out-of-class definition completes THAT decl.
- **clang:** `Sema::ActOnCXXMemberDeclarator` (`clang/lib/Sema/SemaDeclCXX.cpp:3440`)
  computes `isInstField` from the storage class (:3535) and routes a static
  through `HandleDeclarator` (:3650), producing a `VarDecl` at declaration time.

Same model, reached differently. Creating the decl at the declaration is right.

### But libstdc++ and libc++ differ in the load-bearing way

**libstdc++ — hidden, private, never odr-used by user code:**

```
libstdc++-v3/include/bits/locale_classes.h:364   static __gthread_once_t _S_once;   // private: at :335
libstdc++-v3/include/bits/locale_classes.h:420   static __gthread_once_t _S_once;   // private: at :407
libstdc++-v3/src/c++98/locale.cc:75              __gthread_once_t locale::_S_once = ...
$ nm -D libstdc++.so.6 | grep _ZNSt6locale7_S_onceE      # EMPTY — not exported
```

Private, so no user code can odr-use it; defined inside the library; not
exported. Nothing should ever reference it, and madc referencing it was the
whole bug.

**libc++ — public, exported, genuinely odr-used:**

```
$ grep -n "static locale::id id;" <libc++>/__locale     # 8+ facet classes
$ nm -D libc++.so.1 | grep -cE "2idE"                   # 32 exported
00000000000fc988 V _ZNSt3__110moneypunctIcLb0EE2idE
```

`std::use_facet<F>(loc)` reads `F::id`. These are PUBLIC, exported as weak
object symbols, and user code odr-uses them for real. **Under `-stdlib=libc++`
the alias is not optional — `use_facet` cannot work without it.**

Note these are TEMPLATE-class statics (`moneypunct<char,false>::id`). I twice
called that the "harder subset" needing new template-argument mangling. It is
not, and it is not an edge case either — it is the common case here. See
SETTLED #2.

### So both steps are required, and they are complementary

1. **Create on odr-use** — kills the spurious references to hidden statics like
   `_S_once`, and is what deletes the placeholder guard.
2. **Alias when the symbol is exported** — makes an odr-used system-header
   static resolve against the library, which libc++'s facet `id`s need.

Neither alone is sufficient. Step 1 without step 2 leaves `use_facet` broken
under libc++; step 2 without step 1 keeps emitting references to hidden
symbols.

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

3. **Then** set `storage_alias_name` via `itanium_mangle_nested_var` for a
   static whose defining class comes from a system header. Separate commit;
   steps 1–2 stand alone and are what delete the guard. This step is what makes
   `std::use_facet` work under `-stdlib=libc++`, so it is REQUIRED for the
   flavor track, not a nicety. Do NOT gate it on "is the symbol exported" by
   probing the `.so` — emit the alias and let the link fail loudly if the
   library genuinely lacks it; a probe of the host's `.so` is exactly the
   platform-means-library conflation this track exists to undo.

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
- For step 3, the gate is `std::use_facet` under BOTH flavors: it exercises an
  exported, odr-used, template-class static (`_ZNSt3__110moneypunctIcLb0EE2idE`
  and its libstdc++ equivalent) — precisely the shape steps 1 and 2 have to
  agree on. `libcxx_gate.sh` is where the libc++ leg belongs.

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
