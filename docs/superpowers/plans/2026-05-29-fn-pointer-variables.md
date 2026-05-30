# Plan: Function-Pointer Variables (parenthesized declarators)

> Next step after the 2026-05-29 fidelity-suite + SMAUG push. This is the **#1
> SMAUG c2mir blocker**: SMAUG's command/spell tables (`tables.c`) are
> function-pointer arrays, and the builder erases fn-pointer variables to
> `long`, producing ~1300 of c2mir's pointer/integer type errors.

**Goal:** represent function-pointer variables faithfully so `int (*fp)(char)`,
`void (*table[])(CHAR_DATA*)`, etc. compile through `cir_node → c2mir` instead
of degrading to `long`.

## Root cause (verified)

- A fn-pointer type is `class DataDefFPTR : public DataDef` (`include/datadef.h:1001`):
  `DataDefFPTR(FuncDef *fd)` — `target` is the `FuncDef` (return type + params),
  `rawtype()==dtINT64`, `is_function()==true`.
- `CirBuilder::append_type_specs` switches on `rawtype()`; `dtINT64 → N_LONG`.
  So a fn-pointer variable emits type spec `long` and **no declarator nesting** —
  the pointer + `N_FUNC(params)` signature are gone. c2mir then sees `long table[]`
  assigned/compared with real function addresses → "returning pointer without cast
  for integer", "incompatible pointer types" (tables.c: 653 + 650).

## The two-part fix

### Part A — Builder: construct the fn-pointer declarator (`src/cir_builder.cpp`)
A C `ret (*name)(params)` declarator in c2m's tree is `N_DECL[id, suffixes]`
where the suffix list, **in c2m order**, carries the pointer and the function.
(Recall the array-of-pointers fix `894c3f8`: c2m appends pointer ops AFTER the
direct-declarator's array/func ops; pointer-to-function is the same family —
the `N_POINTER` is the *inner* binding, the `N_FUNC` the *outer*. Verify the
exact order against `c2mir.c`'s `declarator`/`direct_declarator` and `--dump-cir`
vs `c2m -d` on a tiny `int (*fp)(char);`.)

1. Detect `DataDefFPTR` (via `dynamic_cast` / a new `is_funcptr()` predicate)
   in the type-spec/declarator paths: `var_decl`, `param_decl`, `type_list`,
   member emission, and the extern-proto path — anywhere a variable's type is
   turned into specs+declarator.
2. Add a helper `node_t fnptr_declarator(DataDefFPTR *fp, node_t inner_id)`:
   - type specs (for the enclosing decl) = the `target` FuncDef's **return type**
     (reuse `append_type_specs` / `type_list` on `fd->returns`).
   - declarator suffixes = build `N_FUNC(param_list)` from the target's params
     (reuse `param_decl` per param), plus the `pointer()` for the `(*...)`, in
     the c2m order that yields pointer-to-function (NOT function-returning-pointer).
3. Route fn-pointer var/param/member decls through this helper instead of the
   `dtINT64 → long` path.

### Part B — Renderer: parenthesized declarators (`src/cir_emit_c.cpp`)
`emit_declarator` today is flat: all `*` as prefix, then id, then `[]`/`()` as
postfix. That produces `*fp(args)` (= function returning pointer) for a
pointer-to-function, the WRONG type. Rework it to the C "spiral rule":
- Walk the suffix list tracking nesting. When a pointer binds *inside* a
  function/array suffix (pointer-to-function / pointer-to-array), emit
  `(* ... )` around the inner declarator: `ret (*name)(args)`, `T (*name)[N]`.
- Keep the existing flat behavior for the simple cases (plain `*`, `[]`, `()`
  that don't need regrouping) so the 20 reducers + fidelity stay green.
- This is the same masking issue noted in the suite memory: the flat emitter
  hides POINTER-vs-FUNC order in emitted text while c2mir's checker catches it.

## Gates (each fix)
- **SMAUG c2mir errors:** `cd /workspace/MadSMAUG/src; timeout 60 /workspace/madc/bin/madc SMAUG.mad 2>err` → the `159 check errors` count must DROP (watch `tables.c` "returning pointer without cast" / "incompatible pointer types" vanish). Also confirm still **0 untranslatable**.
- **Regression:** `make -C src fulltest` integration **>= 302**; 20 fidelity reducers `FIDELITY-OK`.
- Add a plain-C reducer `tests/fidelity/fnptr.c` (`int (*fp)(int); int g(int x){return x;} ... fp=g; fp(3);` + a fn-ptr array) and drive it to `FIDELITY-OK` first — that proves the parenthesized-declarator emission before hitting SMAUG.
- Commit per coherent change (builder helper; renderer parenthesization), each fulltest-gated.

## After this
Re-run SMAUG; the `tables.c` cluster should collapse. Remaining c2mir errors:
the "assigning integer without cast to pointer" theme (NULL/`0`→pointer typing in
act_move/ibuild/const/db) — likely the next cluster. Then `c2mir` clean →
**MIR-gen → JIT**, then link + boot against `MadSMAUG/runtime/` (login → the
old serpent-combat benchmark). See `claude_status.json.smaug_compile` and
agent memory `project_cir_fidelity_suite` for full context.

---

## 2026-05-29 progress + course-correction

**Landed (committed `2ec34ff`, integration 302→303, fnptr.c FIDELITY-OK):**
- **Part B renderer (`cir_emit_c.cpp`):** `emit_declarator` rewritten flat→spiral.
  The suffix list is c2m innermost-first; a pointer binding inside a func/array
  suffix is parenthesized — `int (*fp)(int)`, `char *(*fp)(void)`. Verified
  against `c2m -d`. All 20 prior reducers render byte-identically (only fn-ptr
  declarators, where a POINTER precedes a FUNC/ARR, change).
- **Part A builder (scalar) (`cir_builder.cpp`):** `DataDefFPTR` no longer emits
  `long`. `fnptr_func_node` + `fnptr_decl_pieces` build `ret (*name)(params)`
  with order `[lead-dims…, POINTER, FUNC, ret-stars…]`. Wired into `var_decl`
  (scalar fn-ptr vars) and `param_decl` (fn-ptr params).

**Two premises in this plan were WRONG — corrected by measurement:**
1. The 159 SMAUG c2mir **fatal** errors are **NOT** fn-ptr-dominated. They are
   dominated by **96 "excess elements in array/struct/union initializer"** (2D
   brace init, e.g. `act_move.c:64` `char * const sect_names[SECT_MAX][2]`).
   The fn-ptr "returning pointer without cast" / "incompatible pointer types"
   were among the **2421 warnings**, not the fatal 159. ⇒ The highest-value
   SMAUG target now is the **2D-array init bug**, not more fn-ptr work.
2. Fixing fn-ptr **typedefs + struct members** at the *emitter* layer **cannot**
   be net-positive on its own — it **regresses** SMAUG (159→163 or →176).

**Why the typedef/member emitter fix regresses (root cause = parser layer):**
SMAUG's `typedef void DO_FUN(args)` is a **function** typedef (no `*`). It is
used two ways: `DECLARE_DO_FUN(do_north)` ⇒ `DO_FUN do_north` (a *function*
declaration) and `DO_FUN *do_fun` (a struct-member *pointer*). The parser
**collapses both `DO_FUN *p` and `DO_FUN g` into the same bare `DataDefFPTR`**
(typedef alias set, **0 recoverable stars** — confirmed: both emit `DO_FUN p`).
So the emitter cannot tell pointer-use from function-decl-use:
- emit `typedef void DO_FUN(args)` (function form) ⇒ `DO_FUN *p`/member lose the
  `*` → function-typed lvalues → "lvalue required as left operand" (→176).
- emit `typedef void (*DO_FUN)(args)` (pointer form) ⇒ the `DECLARE_DO_FUN`
  `DO_FUN do_north` becomes a pointer **variable** conflicting with the function
  definition → "repeated declaration" (+4 → 163).

**Prerequisite (deepest layer, Task #5):** the parser must record the source
`*` on fn-ptr-typedef variable/param declarations (pointer var → 1 star;
`DECLARE_DO_FUN` function decl → 0 stars / function type). Only then can the
typedef emit as a function typedef while pointer uses keep their `*`.

**Stashed** (recoverable; `git stash list` → "WIP fnptr typedef+member emitter"):
`typedef_decl` fn-ptr branch, `member_node` fn-ptr branch, `fnptr_alias_stars`,
the `fnptr_decl_pieces(FuncDef*, bool emit_pointer, …)` generalization, and a
`DataDefFPTR::ptr_syntax` flag (Form-1 typedef=false / Form-2=true, set in
`parser.cpp`). Unstash after Task #5 and re-gate (fnptr.c FIDELITY-OK,
fulltest ≥303, SMAUG fatal-error count must DROP, 0 untranslatable).
