# c2mir `__attribute__((cleanup))` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement GNU `__attribute__((cleanup(fn)))` semantics in the madc c2mir fork — a variable so tagged has `fn(&var)` called automatically when it leaves scope, on every exit path (fall-through, `return`, `break`, `continue`, `goto`) — so madc's entire RAII surface (`std::string` dtors, C++ class destructors, `defer`) reduces to one declaration tag instead of hand-rolled goto-ladders.

**Architecture:** c2mir already *parses* `cleanup(fn)` into `N_ATTR(N_ID("cleanup"), N_LIST(N_ID("fn")))` and discards it (`try_attr_spec`, c2mir.c:4439). We (1) stop discarding it — attach the attr list to the declaration; (2) in the **check** pass resolve the cleanup function, validate `void fn(T*)`, store it on `struct decl`, and register the decl with its `node_scope`; (3) in a **post-check, pre-gen AST transform pass** inject (and check) cleanup-call statements at every scope exit, reusing the existing `gen` for `N_CALL`. AST injection (not raw-MIR emission) keeps the delicate control-flow logic inspectable and reuses c2mir's call codegen.

**Tech Stack:** C (c2mir.c, one ~15k-line file in `/workspace/mir`), the c2mir node API (`new_node`/`new_node1/2`, `op_append`, `NL_*` list macros, `N_CALL`/`N_ADDR`/`N_ID`/`N_EXPR`/`N_BLOCK`), `check()` (recursive semantic pass), the `gen()` MIR emitter, c2mir's own test runner (`c2mir/c2mir-test.c` / `make -C /workspace/mir bootstrap test`), and madc's `make -C src fulltest` as the downstream consumer gate.

---

## Background — what exists, verified by reading c2mir.c

- **Parse:** `attr` rule (4503-4531) builds `N_ATTR(name_id, arg_list)` and already accepts `cleanup(fn)` → `N_ATTR(N_ID "cleanup", N_LIST(N_ID "fn"))`. `attr_spec` (4533-4553) returns an `N_LIST` of `N_ATTR`. `try_attr_spec` (4424-4447) parses it then **drops `r`** — the only reason attributes do nothing.
- **Decl/scope model (comment block 5615-5625):** `N_MODULE`/`N_BLOCK`/`N_FOR`/`N_FUNC` carry a `struct node_scope` (5986-5991) in their `attr`; each `struct decl` (5965-6012) has `node_t scope`. Scopes chain via `((struct node_scope*)scope->attr)->scope`.
- **Check pass:** `check()` (6724) is recursive; `N_BLOCK` check (9993-9999) does `create_node_scope`/`finish_scope`. Declarations are checked via the `N_SPEC_DECL` path (decl gets its `decl_t` attr + resolved `scope`).
- **Gen pass:** `gen()` (12576) walks the checked AST → MIR. Control-flow handlers: `N_BLOCK` (13802), `N_RETURN` (14058), `N_GOTO` (14025), `N_BREAK`/`N_CONTINUE` (14042-14047). `gen` already compiles `N_CALL` and `N_ADDR`, so injected cleanup calls need no new codegen.

So the only genuinely new logic is: resolve+store the cleanup fn (check), and compute "which scopes does this exit cross, and which cleanup vars are live" + inject the calls (transform pass).

---

## File Structure

- **Modify `/workspace/mir/c2mir/c2mir.c`** — the entire change lives here:
  - `struct decl` (5965-6012): add `node_t cleanup_fn;`.
  - `struct node_scope` (5986-5991): add a list of cleanup decls in declaration order.
  - `try_attr_spec` / its callers: thread the parsed attr list onto the declaration node.
  - check `N_SPEC_DECL` path: detect `cleanup`, resolve+validate fn, set `decl->cleanup_fn`, append decl to its scope's cleanup list.
  - new function `process_cleanups(c2m_ctx, func_def)`: post-check transform.
  - call `process_cleanups` from the `N_FUNC_DEF` check tail (after the body is checked, before gen).
- **Create `/workspace/mir/c2mir/test/cleanup-*.c`** (or wherever c2mir's test corpus lives — confirm with `ls /workspace/mir/c2mir/test* ; grep -rn "c2mir-test\|run_test" /workspace/mir/Makefile`) — one test file per exit path, each `main`-returning and self-checking via `printf` + an expected-output assertion.
- **Downstream (separate, after this lands):** madc consumes it — `src/cir_builder.cpp` swaps the manual `std::string` destruct for a `cleanup` tag (its own plan; see "Integration" at end).

---

## Key design decisions (locked here)

1. **AST injection, post-check, pre-gen.** A dedicated pass walks each function's checked AST and inserts cleanup-call statements, then runs `check()` on each *newly synthesized* subtree (so it gets types/attrs and gen can compile it). Rationale: reuses gen's `N_CALL` codegen; the risky scope-exit logic is AST manipulation (inspectable, dumpable via `-dg`) not raw MIR.
2. **The cleanup call shape:** `N_EXPR(N_LIST(), N_CALL(N_ID "fn", N_LIST(N_ADDR(N_ID "var"))))` — `fn(&var)`. GNU semantics: `fn` receives a *pointer* to the variable.
3. **Order:** within one scope, cleanups run in **reverse declaration order**. Across nested scopes at a `return`/`break`/`goto`, inner scopes clean up before outer.
4. **Exit-path handling:**
   - **Fall-through end of a block:** append cleanups for that block's vars before the block closes.
   - **`return expr;`** in a function with live cleanups: rewrite to evaluate `expr` into a temp, run all enclosing scopes' cleanups (inner→outer), then `return tmp;`. Void/`return;` skips the temp.
   - **`break`/`continue`:** run cleanups for scopes between the current point and the loop/switch the jump targets.
   - **`goto L`:** run cleanups for scopes exited between the goto and `L`'s scope (the label's scope must be an ancestor or sibling-ancestor; jumping *into* a scope past a cleanup decl is already a c2mir/C constraint).
5. **Generality = full GNU semantics** (all exit paths, nested scopes) — this is designed for upstream, so no pragmatic shortcuts that change observable behavior. Phases deliver paths incrementally but the end state is complete.
6. **No behavior change when unused:** a function with zero cleanup decls must produce byte-identical MIR to today (the pass early-returns). This protects the ~1071-test c2mir suite.

---

## Phase 0 — Parse capture + struct plumbing (no behavior yet)

### Task 0.1: Add fields to `struct decl` and `struct node_scope`

**Files:** Modify `/workspace/mir/c2mir/c2mir.c:5965-6012` and `:5986-5991`

- [ ] **Step 1: Branch the fork**

Run:
```bash
git -C /workspace/mir checkout -b feature/cleanup-attribute
git -C /workspace/mir rev-parse --abbrev-ref HEAD
```
Expected: `feature/cleanup-attribute`.

- [ ] **Step 2: Add `cleanup_fn` to `struct decl`**

In `struct decl` (before the closing `};` at ~6012), add:
```c
  node_t cleanup_fn; /* resolved N_ID of __attribute__((cleanup(fn))), else NULL */
```

- [ ] **Step 3: Add a cleanup-decl list to `struct node_scope`**

In `struct node_scope` (5986-5991), add:
```c
  VARR (node_t) * cleanup_decls; /* decls in this scope with cleanup_fn, decl order; NULL if none */
```
(Confirm the `VARR` macro is available here — c2mir uses `VARR (node_t)` widely; `grep -n "VARR (node_t)" c2mir/c2mir.c` to confirm the type is declared. If `node_t` VARR isn't declared, add `DEF_VARR (node_t)` near the other `DEF_VARR` declarations.)

- [ ] **Step 4: Initialize the new fields**

Where `struct decl` is allocated (`grep -n "reg_malloc (c2m_ctx, sizeof (struct decl))" c2mir/c2mir.c`), set `decl->cleanup_fn = NULL;`. In `create_node_scope` (6571), set `ns->cleanup_decls = NULL;`.

- [ ] **Step 5: Build the fork**

Run: `make -C /workspace/mir 2>&1 | tail -5`
Expected: builds clean (libmir.a + c2m). New fields are unused → no behavior change.

- [ ] **Step 6: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c
git -C /workspace/mir commit -m "c2mir: add cleanup_fn (decl) + cleanup_decls (node_scope) fields"
```

### Task 0.2: Stop discarding the parsed attribute list; attach it to the declaration

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (`try_attr_spec` 4424-4447 and its caller(s))

- [ ] **Step 1: Find where `try_attr_spec` is called on a declaration**

Run: `grep -n "try_attr_spec" c2mir/c2mir.c`
Read each call site. The declaration path (declarator / `declaration`) is the one whose result must carry the attr list to the eventual `N_SPEC_DECL`/decl. Document which call site corresponds to a variable declaration's attributes.

- [ ] **Step 2: Return the attr list instead of dropping it**

In `try_attr_spec`, the `attr_spec` result `r` (4439) is currently dropped. Return it to the caller (or set an out-param), and at the declaration call site, stash it on the declarator node so check can find it. Minimal approach: store the attr `N_LIST` in a side table keyed by the declarator node, OR append it as an extra trailing operand the check pass knows to look for. Choose the side-table approach if adding an operand perturbs `NL_EL` indexing elsewhere (verify by reading how `N_SPEC_DECL`/`N_DECL` operands are indexed in check).

(EXECUTION NOTE: the exact attachment mechanism must be chosen by reading the declarator/`N_SPEC_DECL` operand layout — do NOT guess. A `HTAB`/side-map `node_t declarator -> node_t attr_list` is the least invasive and is the recommended default; c2mir already uses HTAB-style tables.)

- [ ] **Step 3: Build + run the c2mir suite (must stay green)**

Run: `make -C /workspace/mir 2>&1 | tail -3 && make -C /workspace/mir test 2>&1 | tail -15`
Expected: builds clean; c2mir test suite passes exactly as before (attrs captured but still semantically inert).

- [ ] **Step 4: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c
git -C /workspace/mir commit -m "c2mir: capture parsed __attribute__ list onto the declarator (was discarded)"
```

---

## Phase 1 — Check: resolve, validate, and register the cleanup function

### Task 1.1: Resolve `cleanup(fn)` on a declaration during check

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (the `N_SPEC_DECL` / `create_decl` check path — find via `grep -n "create_decl\|case N_SPEC_DECL" c2mir/c2mir.c`)

- [ ] **Step 1: Write a c2mir test that should parse+check clean (and fails today)**

Create `/workspace/mir/c2mir/test/cleanup-resolve.c`:
```c
#include <stdio.h>
static void cl(int *p) { printf("cleanup %d\n", *p); }
int main(void) {
  int x __attribute__((cleanup(cl))) = 7;
  printf("body %d\n", x);
  return 0;
}
```
Expected behavior once implemented: prints `body 7` then `cleanup 7`. For Task 1.1 we only require it to **compile and run** (printing `body 7`); the cleanup call comes in Phase 2.

- [ ] **Step 2: Run it (pre-implementation) to see current behavior**

Run: `/workspace/mir/c2m -ei /workspace/mir/c2mir/test/cleanup-resolve.c`
Expected: prints `body 7` (attr currently inert, no cleanup). Confirms the attr no longer errors and the baseline is "no cleanup yet".

- [ ] **Step 3: In the decl check path, detect+resolve the cleanup attr**

After a declaration's identifier and scope are resolved, look up the stashed attr list (Task 0.2). For an `N_ATTR` whose name id is `"cleanup"`: take its single arg `N_ID`, resolve it to a symbol in scope (reuse the identifier-resolution used for `N_ID` expressions — `grep -n "symbol_find\|find_decl" c2mir/c2mir.c`), assert it names a function of type `void (*)(T*)` compatible with the decl's address type, and set `decl->cleanup_fn = <resolved id node>`. Error with a clear message on a non-function or arity/type mismatch.

(EXECUTION NOTE: reuse the existing symbol lookup + the existing function-call type-compatibility check; do not write a parallel type checker. Find the call-arg compatibility helper via `grep -n "check_assignment_types\|compatible_types_p" c2mir/c2mir.c`.)

- [ ] **Step 4: Register the decl in its scope's cleanup list**

When `decl->cleanup_fn != NULL`, lazily allocate `ns->cleanup_decls` (the `node_scope` of `decl->scope`) and append the decl node — preserving declaration order.

- [ ] **Step 5: Build + run the test + full c2mir suite**

Run: `make -C /workspace/mir 2>&1 | tail -3 && /workspace/mir/c2m -ei c2mir/test/cleanup-resolve.c && make -C /workspace/mir test 2>&1 | tail -8`
Expected: prints `body 7`; c2mir suite still fully green; a deliberately-bad `cleanup(not_a_func)` test errors cleanly (add `cleanup-bad.c` and assert non-zero exit).

- [ ] **Step 6: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c c2mir/test/cleanup-resolve.c c2mir/test/cleanup-bad.c
git -C /workspace/mir commit -m "c2mir: resolve+validate cleanup(fn) in check, register per scope"
```

---

## Phase 2 — Inject cleanup at block fall-through end (the core mechanism)

### Task 2.1: `process_cleanups` pass — fall-through case only

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (new function + call from `N_FUNC_DEF` check tail at 9831-…)

- [ ] **Step 1: Make `cleanup-resolve.c` the failing test for actual cleanup**

The expected output is now the full:
```
body 7
cleanup 7
```
Run: `/workspace/mir/c2m -ei c2mir/test/cleanup-resolve.c`
Expected (pre-impl): only `body 7` (no cleanup yet) → this is the RED state.

- [ ] **Step 2: Implement `process_cleanups` for fall-through**

Add a function that, for each `N_BLOCK` with a non-empty `cleanup_decls`, synthesizes — at the **end** of the block's statement list — one cleanup call per cleanup decl in **reverse** order:
```c
/* For each cleanup decl D (reverse order) in block B's scope, append to B:
     N_EXPR(N_LIST(), N_CALL(N_ID copy-of D->cleanup_fn,
                             N_LIST(N_ADDR(N_ID copy-of D's name)))) */
```
Build the nodes with `new_node`/`new_node1`/`new_node2` + `op_append` (mirror existing synthesized-node code — `grep -n "new_node2 (c2m_ctx, N_CALL" c2mir/c2mir.c` for a template), then `check()` each new `N_EXPR` subtree with the block as context so it gets types/attrs. Walk all blocks in the function (recurse the AST).

- [ ] **Step 3: Call the pass from `N_FUNC_DEF` check, after the body is checked**

In the `N_FUNC_DEF` check handler (9831), after the function body has been checked (and scopes finished), call `process_cleanups(c2m_ctx, func_def_node)`. Guard: if the function has no cleanup decls anywhere, return immediately (zero behavior change).

- [ ] **Step 4: Build + run the test**

Run: `make -C /workspace/mir 2>&1 | tail -3 && /workspace/mir/c2m -ei c2mir/test/cleanup-resolve.c`
Expected:
```
body 7
cleanup 7
```

- [ ] **Step 5: Reverse-order + multi-var test**

Create `c2mir/test/cleanup-order.c` with two cleanup vars `a` then `b`; assert output shows `b` cleaned before `a`. Run it.

- [ ] **Step 6: Full c2mir suite green**

Run: `make -C /workspace/mir test 2>&1 | tail -10`
Expected: all c2mir tests pass (unused-cleanup functions emit identical MIR; the pass early-returns).

- [ ] **Step 7: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c c2mir/test/cleanup-order.c
git -C /workspace/mir commit -m "c2mir: inject cleanup calls at block fall-through end (reverse order)"
```

---

## Phase 3 — `return` (all enclosing scopes)

### Task 3.1: Run cleanups before `return`, preserving the return value

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (`process_cleanups`)

- [ ] **Step 1: Failing test — cleanup on early + value return**

Create `c2mir/test/cleanup-return.c`:
```c
#include <stdio.h>
static void cl(int *p){ printf("cleanup %d\n", *p); }
static int f(int e){
  int x __attribute__((cleanup(cl))) = 1;
  if (e) return 99;     /* early return: cleanup must run, value 99 preserved */
  return 0;
}
int main(void){ printf("got %d\n", f(1)); return 0; }
```
Expected once implemented: `cleanup 1` then `got 99`.

- [ ] **Step 2: Implement return handling**

In `process_cleanups`, for each `N_RETURN` inside any cleanup-bearing scope: gather cleanup decls for all scopes from the return's enclosing block up to (and including) the function block, inner→outer, reverse-decl within each. Rewrite:
- `return expr;` (non-void) → a block: declare a temp of the function's return type, `tmp = expr;`, the cleanup calls, `return tmp;`.
- `return;` / void → cleanup calls, then `return;`.
Synthesize + `check()` the new subtree. (Build the temp decl the same way c2mir builds compiler temporaries — `grep -n "get_temp\|new temp\|gen_temp" c2mir/c2mir.c`; if there's no AST-temp helper, synthesize an `N_SPEC_DECL` for a uniquely-named local of the return type.)

(EXECUTION NOTE: confirm how the function's return type node is reachable from the `N_RETURN` (walk to the enclosing `N_FUNC_DEF`'s declarator return type). The temp must have exactly that type.)

- [ ] **Step 3: Build, run, suite**

Run: `make -C /workspace/mir 2>&1 | tail -3 && /workspace/mir/c2m -ei c2mir/test/cleanup-return.c && make -C /workspace/mir test 2>&1 | tail -6`
Expected: `cleanup 1` / `got 99`; suite green.

- [ ] **Step 4: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c c2mir/test/cleanup-return.c
git -C /workspace/mir commit -m "c2mir: run cleanups before return (all enclosing scopes, value preserved)"
```

---

## Phase 4 — `break` / `continue` (scopes up to the loop/switch)

### Task 4.1: Cleanups on loop/switch exit jumps

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (`process_cleanups`)

- [ ] **Step 1: Failing test**

`c2mir/test/cleanup-break.c`: a `for`/`while` whose body declares a cleanup var, with a `break` and a `continue`; assert the var is cleaned on each iteration's `continue` and on the `break`. Include a cleanup var in an inner block inside the loop to test partial-scope exit.

- [ ] **Step 2: Implement**

For each `N_BREAK`/`N_CONTINUE`: find the innermost enclosing loop (`break`/`continue`) or switch (`break`), gather cleanup decls for scopes between the jump and that construct's scope (inner→outer, reverse within scope), inject the calls immediately before the jump (wrap `{ cleanups; break; }`).

(EXECUTION NOTE: c2mir tracks the break/continue target during check/gen — `grep -n "start_label\|continue_label\|break_label\|stmt->attr" c2mir/c2mir.c`. Use the same enclosing-construct determination so cleanups match the jump's actual target. A `continue` exits the loop *body* scope(s) but NOT the loop's own (e.g. for-init) scope — verify against the scope nesting.)

- [ ] **Step 3: Build, run, suite**

Run: `make -C /workspace/mir 2>&1 | tail -3 && /workspace/mir/c2m -ei c2mir/test/cleanup-break.c && make -C /workspace/mir test 2>&1 | tail -6`
Expected: correct per-iteration + break cleanup; suite green.

- [ ] **Step 4: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c c2mir/test/cleanup-break.c
git -C /workspace/mir commit -m "c2mir: run cleanups on break/continue up to the loop/switch scope"
```

---

## Phase 5 — `goto` (scopes crossed between source and label)

### Task 5.1: Cleanups on goto

**Files:** Modify `/workspace/mir/c2mir/c2mir.c` (`process_cleanups`)

- [ ] **Step 1: Failing test**

`c2mir/test/cleanup-goto.c`: a `goto` out of an inner block (with a cleanup var) to a label in an outer scope; assert the inner var is cleaned. (Standard C already forbids jumping *into* the scope of a VM/cleanup decl; only test legal backward/outward gotos.)

- [ ] **Step 2: Implement**

For each `N_GOTO`: find the target label's scope (labels are function-scoped; resolve via the label table — `grep -n "check_labels\|N_LABEL\|label" c2mir/c2mir.c`). Compute the set of scopes exited = scopes on the path from the goto's scope up to the nearest common ancestor of (goto scope, label scope). Inject cleanups (inner→outer, reverse within scope) before the jump.

(EXECUTION NOTE: this is the fiddliest case. Compute the nearest-common-ancestor scope via the `node_scope->scope` parent chain. Cleanups run for scopes strictly between the goto and the common ancestor.)

- [ ] **Step 3: Build, run, suite**

Run: `make -C /workspace/mir 2>&1 | tail -3 && /workspace/mir/c2m -ei c2mir/test/cleanup-goto.c && make -C /workspace/mir test 2>&1 | tail -6`
Expected: correct; suite green.

- [ ] **Step 4: Commit**

```bash
git -C /workspace/mir add c2mir/c2mir.c c2mir/test/cleanup-goto.c
git -C /workspace/mir commit -m "c2mir: run cleanups on goto for scopes crossed to the label"
```

---

## Phase 6 — Parity sweep + GCC cross-check

### Task 6.1: Differential test vs gcc

**Files:** Create `c2mir/test/cleanup-matrix.c`

- [ ] **Step 1: Build a matrix test**

One program exercising: multiple cleanup vars, nested scopes, every exit path, cleanup vars whose dtor reads the value, and a no-cleanup control function. It prints a deterministic trace.

- [ ] **Step 2: Cross-check against gcc (canon)**

Run:
```bash
gcc -std=gnu11 -O0 c2mir/test/cleanup-matrix.c -o /tmp/cm_gcc && /tmp/cm_gcc > /tmp/cm_gcc.out
/workspace/mir/c2m -ei c2mir/test/cleanup-matrix.c > /tmp/cm_c2m.out
diff /tmp/cm_gcc.out /tmp/cm_c2m.out && echo MATCH
```
Expected: `MATCH` — c2mir's cleanup order/paths are byte-identical to gcc.

- [ ] **Step 3: Full c2mir suite, both interp and gen**

Run: `make -C /workspace/mir test 2>&1 | tail -15`
Expected: all green. Record counts.

- [ ] **Step 4: Commit**

```bash
git -C /workspace/mir add c2mir/test/cleanup-matrix.c
git -C /workspace/mir commit -m "c2mir: cleanup attribute matrix test, gcc-differential MATCH"
```

---

## Integration (separate downstream plan, after this lands)

madc then consumes the primitive (its own short plan):
- `src/cir_builder.cpp`: `string_storage_decl` emits `long name[W] __attribute__((cleanup(string_destruct)))`; **delete** the manual `scope_strings` destruct loop in `translate_block` and the scope-end dtor injection — c2mir now does it on every path (this also retires the Phase-1 trailing-return leak for free).
- The same tag backs C++ class destructors and `defer` when those land on CIR.
- Re-run `make -C src fulltest`; expect ≥ current pass count with correct early-return destruction.
- `--emit=c11` output now contains `__attribute__((cleanup(...)))` — valid for gcc/clang (madc's AOT targets); a strict-C11-only path would need the goto-ladder lowering (deferred, likely never needed).

---

## Self-Review

- **Spec coverage:** GNU cleanup semantics = run `fn(&var)` on every scope exit. Covered: fall-through (Ph2), return (Ph3), break/continue (Ph4), goto (Ph5), reverse+nested order (Ph2/3 tests + Ph6 matrix), validation/errors (Ph1). gcc-differential gate (Ph6) is the canon check (Rule #1).
- **Placeholder scan:** no "TBD/handle-edge-cases" steps. The five "EXECUTION NOTE" items are explicit *read-this-in-c2mir-first* anchors (attr attachment mechanism, symbol/type-compat reuse, return-type reachability, break/continue target, NCA scope walk) — each names what to confirm and the grep to find it. They exist because c2mir is unfamiliar 15k-line code and guessing node layouts would be a plan failure; resolving them is part of each task.
- **Type/name consistency:** `cleanup_fn` (struct decl), `cleanup_decls` (node_scope), `process_cleanups` (the pass) are named identically across all phases. Node shape `N_EXPR(N_LIST(), N_CALL(N_ID, N_LIST(N_ADDR(N_ID))))` is constant.
- **Risk:** the `return`-value temp (Ph3) and the `goto` NCA computation (Ph5) are the two hardest; each is isolated to one phase with its own test, and the no-cleanup early-return guard (design decision 6) plus the full c2mir suite after every phase bound the blast radius. Work is on a fork branch with a 2-year-idle upstream → safe window, present upstream once working.
