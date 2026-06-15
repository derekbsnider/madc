# Plan — forward prototypes for late-drained free-fn-template instantiations

**Status:** recon done, ready to implement next session. Branch
`feature/retire-embedded-shims-claude` (LOCAL-ONLY). Companion: handoff
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` §1b (SESSION 14).

## 1. Problem

The `<set>`/`<map>` reducers (`tmp/sii.mad`/`tmp/mii.mad`,
`--std=c++17 --no-embedded-headers`) now PARSE the full closure (5 walls cleared
this session) and fail with a SINGLE c2mir check error. Localized with the
**gcc-on-emitted-C** method (`madc --emit=c11 r > r.c; gcc -std=gnu11
-fsyntax-only -w r.c`):

```
sii.c:909:104: invalid type argument of unary '*' (have 'int')
sii.c:938:47:  conflicting types for '__ns_std_move__o2'
```

`__ns_std_move__o2` (an overload-disambiguated `std::move` instantiation) is
**defined** at line 938 and **used** at line 909 (inside the lazily-materialized
`_Optional_alloc::release` body) with **no forward prototype** before the use →
c2mir applies the C89 implicit-int rule (`int __ns_std_move__o2()`), so the use
derefs an `int` (`*...`) and the real definition then "conflicts."

This is the SAME class as testforeach2's `__ns_std_for_each` MIR-link-undefined
failure — instantiated free fn-templates not declared/emitted for
definition-before-use. Likely a **2-test fix** (testset/testmap path + testforeach2),
though further `_Rb_tree` walls may sit behind it.

## 2. madc recon — the exact gap (cir_builder.cpp `translate_module`)

Prototypes are emitted in two referenced-driven passes; definitions in Pass 2:

- **Pass 0.75 (~12031):** iterates `prog->funcdef_map`, emits an extern proto for
  each entry whose emit symbol ∈ `referenced_funcs`. Keyed by the funcdef_map
  entry's symbol.
- **Fixpoint `materialize_and_lower` (~11964-12027):** to a fixpoint, (a) parses
  ODR-used `deferred_lazy_bodies` → pushes the TokenFunc to **`materialized_funcs`**
  (11978) AND `lib_funcs`; (b) drains `prog->pending_funcs` (functions newly
  instantiated *during* a late re-parse) into **`lib_funcs` only** (11994-12004) —
  **NOT** `materialized_funcs`; (c) emits a `func_def` for any `lib_funcs` entry
  that is referenced (12005-12013).
- **Pass 1.95(a) (~12361):** emits forward protos for **`materialized_funcs`** only.
- **Pass 2 (~12407):** emits all `func_def_nodes` (definitions).

**THE GAP:** a free-fn-template instantiation reached only *during* the fixpoint's
late body translation (`release` → `std::move` → `__ns_std_move__o2`) is appended
to `prog->pending_funcs`, drained into `lib_funcs` (11994-12004), and gets a
**definition** (12012) — but it is **never added to `materialized_funcs`**, so
Pass 1.95(a) never emits its forward proto. Pass 0.75 also misses it: the
instantiation's per-overload symbol (`__ns_std_move__o2`, via `local_emit_name`)
is not the key Pass 0.75 computes from the `funcdef_map` template entry
(`__ns_std_move`). Net: definition emitted, no prototype, use precedes def.

Contrast: `allocator_..._o2` (also a late def) DOES get a real proto — it reaches
`materialized_funcs` via the `deferred_lazy_bodies` path (a), so Pass 1.95 covers
it. Only the **pending_funcs-drained** instantiations fall through.

**Verify-first step (cheap, do at implementation start):** add a one-line gated
diag in the 11994-12004 drain loop printing `key`; confirm `__ns_std_move__o2`
passes through it and is absent from `materialized_funcs`. (High confidence from
structure, but confirm before editing.)

## 3. Clang research — why clang never hits this, and the principle it implies

Clang (`Sema::PerformPendingInstantiations`,
`clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:6417`) drains its
`PendingInstantiations` deque at end-of-TU, calling `InstantiateFunctionDefinition`
for each — producing AST `FunctionDecl`s. CodeGen then lowers those to LLVM IR,
where a call references the callee `llvm::Function*` **by object/symbol**; IR has
**no textual declare-before-use constraint** (a `declare`/`define` may appear in
any order; the verifier only requires the symbol to exist). So clang's late
instantiations are simply more decls in the AST — ordering is a non-issue.

**The lesson:** madc's bug is **intrinsic to emitting ordered textual C11** for
c2mir (which honors C's K&R implicit-int on use-before-decl). Clang's model is
"every callee has a declaration; emission order is irrelevant." The robust,
clang-aligned invariant for madc's C-emission layer is therefore:

> **Every function madc emits a DEFINITION for must have a forward declaration
> emitted ahead of all definitions — unconditionally, not filtered by
> `referenced_funcs`/`funcdef_map` membership.**

The current `referenced_funcs`-filtered proto passes are an *optimization* (match
c2mir's "declare only what `#include` pulled in", keep emitted C lean). That
optimization is correct for EXTERN decls (libc fns madc doesn't define) but is the
source of this bug for madc-DEFINED functions: anything in `func_def_nodes` is
defined by us and can be used before its definition by an earlier-emitted body.

## 4. Approaches

**A — Targeted (minimal, low risk).** In the pending_funcs drain (11994-12004),
also push the drained TokenFunc into `materialized_funcs` (it is exactly the
"late, not in the Pass-1 snapshot" category Pass 1.95(a) exists for). One/two
lines. Pass 1.95(a) then protos it. Dedup against double-proto (identical C
prototypes are legal, but avoid noise). Scope: only the late instantiations;
nothing else changes.

**B — Principled / clang-aligned (broader, higher value, higher risk).** Drive
the forward-proto pass off `func_def_nodes` (every definition madc emits) instead
of (only) `referenced_funcs` ∩ `funcdef_map`. Guarantees the invariant in §3 for
ALL madc-defined functions, killing this whole bug class (incl. testforeach2 and
any future late-instantiation). Risk: must NOT add protos for extern-only / libc
funcs (those have no madc def — they stay referenced-only), and must not produce
a proto whose signature conflicts with an existing one (member-template
placeholders, retbuf-shape mismatches — see the Pass 0.75 `is_member_template`
skip at 12045 and the retbuf notes). Needs careful signature-source selection.

**Recommendation:** land **A** first (clears the immediate 1-2 tests, low risk,
fully validated), then evaluate **B** as a follow-up hardening once A confirms the
mechanism — B is the real fix for the *class* but deserves its own validation
pass. Do NOT do B blind.

## 5. Validation

- **Reduce + 3-oracle FIRST** (g++ AND clang++ + madc; values via cout, exit code
  = RUN-SUCCESS not main()): a class whose **lazily-materialized** method body
  calls a `std::move`-like free fn template returning a non-trivial class by
  value, used before its definition in emission order. Start from `tmp/sii.mad`;
  derive a headers-free reducer if possible (a free `template<class T> T mv(T&)`
  + a class method that calls it from a deferred/late context).
- **gcc-on-emitted-C** after the fix: `madc --emit=c11 tmp/sii.mad > x.c;
  gcc -std=gnu11 -fsyntax-only -w x.c` must be clean for the `__ns_std_move__o2`
  errors (expect the NEXT `_Rb_tree` wall to surface — record it).
- **`make -C src fulltest`** — zero regression (the proto passes touch EVERY
  emitted module; a bad change here can break definition-before-use broadly →
  watch for new c2mir "implicit declaration"/"conflicting types" or MIR
  "import of undefined" failures across the whole suite, not just the container
  tests). This is why the risk is higher than this session's parse fixes.
- Check **testforeach2** specifically (same class — may flip or advance).
- Add a committed regression test once a clean reducer exists.

## 6. Open questions to resolve at implementation
- Confirm (diag) `__ns_std_move__o2` flows through the pending_funcs drain and is
  absent from `materialized_funcs` (§2 verify-first).
- For approach A: does pushing to `materialized_funcs` risk a DUPLICATE proto for
  any entry that also reaches Pass 0.75? (Dedup by emitted symbol if so.)
- Does `func_proto(tf)` emit the correct retbuf-shaped signature for a by-value
  class-returning instantiation (it must match the def at 938)? Verify the
  emitted proto's signature equals the definition's.
