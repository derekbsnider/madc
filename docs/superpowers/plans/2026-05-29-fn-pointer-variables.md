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
