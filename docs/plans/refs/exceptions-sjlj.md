# Curated reference: SJLJ exceptions (for roadmap P1.1)

**Reference-only, cruft-free extraction.** This distills the *recyclable* essence
of the old transpiler's try/catch/throw so the P1.1 implementer never has to open
the dead 6,138-line `src/madc_emit_c.cpp` and risk carrying forward Gecko /
text-emission cruft. Build P1.1 as `cir_node` lowering of `TokenTRY` in
`CirBuilder`, calling the LIVE runtime below. (Memories:
`project_recycle_old_transpiler_carefully`, `project_north_star_c23_cpp23`.)

## ✅ The runtime is ALREADY LIVE — recycle nothing, just call it
`src/rt/rt_except.c` (was `src/exception_runtime.cpp` before forest-carriers S5
made it a dual-build C11 source — the host build compiles it into libmadc AND
madc compiles it into an AOT ledger module for `-static-libmadc`) is in the
Makefile via `RT_OFILES`. It is complete and backend-agnostic. The contract:

Structs (defined in src/rt/rt_except.c):
- `MadcTryContext { jmp_buf jbuf; MadcTryContext *prev; MadcCleanupEntry *cleanup_mark; }`
- `MadcCleanupEntry { void **fn_indirect; void *obj_ptr; uint8_t *guard; uint8_t is_chain_tail; MadcCleanupEntry *prev; }`
- exception type tags: `NONE=0 INT=1 DOUBLE=2 CSTR=3 ANY=99`

`extern "C"` functions to call from lowered code:
- `void *__madc_try_push(MadcTryContext *ctx)` — pushes ctx, **returns `(void*)ctx->jbuf`** (setjmp on this).
- `void  __madc_try_pop()` — normal try-block exit.
- `void  __madc_throw_int(int64_t)` / `__madc_throw_double(double)` / `__madc_throw_cstr(const char*)` — set exception, pop one try ctx, unwind cleanup to `ctx->cleanup_mark`, `longjmp(jbuf,1)`. With no active try → unwind all + abort.
- `void  __madc_rethrow()` — bare `throw;` (re-longjmp to next try, exception state intact).
- `int   __madc_exception_type()`, `int64_t __madc_exception_int()`, `double __madc_exception_double()`, `const char *__madc_exception_cstr()` — catch dispatch + value bind.
- `void  __madc_exception_clear()` — after a catch handler completes.
- Cleanup stack: `__madc_cleanup_push(entry, fn_indirect, obj, guard, is_chain_tail)`, `__madc_cleanup_pop()`, `__madc_cleanup_unwind_to(mark)` (calls dtors), `__madc_cleanup_discard_to(mark)`.

## The lowering shape (distilled from old `emit_try_catch` — as a SPEC, not code)
For `try { BODY } catch(T e) { H } [catch(...) { H2 }]`, build cir_node equivalent to:
```
MadcTryContext __try_ctx_N;                       // local
if ( setjmp( *(jmp_buf*)__madc_try_push(&__try_ctx_N) ) == 0 ) {
    BODY                                          // lowered normally
    __madc_try_pop();                             // normal exit (no catch ran)
} else {
    // exception path — dispatch on type, bind value, run the matching handler:
    int __t = __madc_exception_type();
    if      (__t == 1 /*INT*/    && catch matches int)    { T e = __madc_exception_int();    H; __madc_exception_clear(); }
    else if (__t == 2 /*DOUBLE*/ && ...)                  { ... }
    else if (__t == 3 /*CSTR*/   && ...)                  { ... }
    else if (catch(...) present)                          { H2; __madc_exception_clear(); }
    else                                                  { __madc_rethrow(); }  // no handler → propagate
}
```
- **throw lowering:** `throw <int-expr>` → `__madc_throw_int(expr)`; double/cstr likewise (pick by the throw operand's type — the old `throw_func_for_type` map); bare `throw;` → `__madc_rethrow()`.
- **nesting:** unique `ctx_id` per try; nested try works because `__madc_try_push`/`pop` is a stack. Use distinct synthesized names per depth.
- **declarations in the try body** must be visible to the catch path: hoist them before the setjmp (the old code did this) OR scope them so dtor-unwind can see them — see the design decision below.

## ⚠️ THE ONE REAL DESIGN DECISION (make it deliberately — this is where drift hides)
**How do try-body object destructors run on the exception (longjmp) path?**
The current CIR backend does RAII via the **c2mir `__attribute__((cleanup))`** attribute. **`longjmp` does NOT fire cleanup-attribute destructors** — so a naive lowering LEAKS/skips dtors for objects constructed in the try body when an exception propagates. Three options:
1. **Runtime cleanup stack (recommended, aligns with the live runtime):** at each try-body object construction, also `__madc_cleanup_push` a cleanup entry; `__madc_throw_*` already calls `__madc_cleanup_unwind_to(ctx->cleanup_mark)`, so dtors run correctly on throw. On normal exit, `__madc_cleanup_discard_to` (the cleanup-attribute still handles normal-scope teardown). This is what `MadcCleanupEntry`/`cleanup_mark` exist for.
2. The old transpiler's **static per-object guard vars** (`__try_alive_N_M`, reset at entry, set after each ctor, checked in a catch-side dtor pass). Works but is a hack tied to text emission — **do NOT carry this forward** unless option 1 proves infeasible.
3. Ignore unwind (leak on exception path) — **unacceptable** for compliance.
Decide between 1 and the cleanup-attribute interaction **before implementing**; verify with a `try { Obj o; throw 1; } catch(...) {}` test that `o`'s dtor runs exactly once. gcc/clang behavior is canon.

## ⚠️ c2mir gotcha that DIRECTLY affects this work (found 2026-05-31, P0.5)
A `__attribute__((cleanup))` variable declared INSIDE a statement-expression `({ ... })`
does **not** get its destructor scoped correctly by c2mir — the dtor fires at the wrong
time and can corrupt unrelated locals (observed: a stmt-expr-local cleanup temp clobbered
a sibling object). **Implication for P1.1:** emit the try-context, any cleanup-tagged
temps, and the catch handler as **block-scoped statements** (e.g. via `m_pending_stmts`
/ a real `{ }` block), NOT inside a `({...})` statement-expression. This is also why the
SJLJ unwind should lean on the runtime cleanup stack rather than relying on
cleanup-attribute timing across the setjmp/longjmp boundary.

## ❌ Do NOT carry forward (this is the cruft / drift)
- `gp_tree_node`, `child(node,i)`, `an_code`, `AN_CATCH`/`AN_CATCH_LIST`/`AN_BLOCK` node kinds — dead Gecko AST.
- `O(...)` / `emit_indent()` / `indent_level` **string/text emission** — the CIR backend builds nodes, not text.
- `_setjmp(((char*)&ctx))` form — use the live API: `setjmp(*(jmp_buf*)__madc_try_push(&ctx))`.
- the static-guard dtor hack (prefer the runtime cleanup stack, option 1).

## Reference anchors (read ONLY to confirm structure — never copy)
- LIVE runtime + contract: `src/rt/rt_except.c` (current tree).
- Old lowering shape: `42e9b6e~1:src/madc_emit_c.cpp` — `emit_try_catch` (~2826), `emit_try_body_dtors_guarded` (~2658), `throw_func_for_type` (~513), `exception_type_for_catch` (~523), `collect_catch_clauses` (~494).
- SJLJ design rule: `.claude/rules/c11-transpiler.md` (Exceptions section).
- Parser side: `TokenTRY`/catch/throw already tokenize+parse (the gap is purely CIR lowering — `cir error: unhandled expression: TokenTRY`).
