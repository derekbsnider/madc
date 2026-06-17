# Plan — generalize `operator()` call dispatch to any class-typed receiver node

**Status:** ready to implement (next round). **Branch:** `feature/retire-embedded-shims-claude`.
**Parent goal:** clear all set/map walls (audit row 7d → `__invoke_result` chain).
**Companion:** `docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` §19i;
`docs/parity/feature-drops-audit.md` row 7d.

---

## 1. Why this is the next wall

`set<int>`/`map<int,int>` need `__is_invocable<_Compare&, const _Key&, const _Key&>`
to evaluate **true** (madc currently folds it to 0; g++=1). That trait derives from
`__is_invocable_impl<__invoke_result<F, Args...>, void>::type`, which needs
`__invoke_result<F,Args...>::type` to EXIST. That type is, in `__result_of_other_impl`:

```cpp
static __result_of_success<
    decltype( std::declval<_Fn>()(std::declval<_ArgTypes>()...) ),  // <-- THIS
    __invoke_other> _S_test(int);
```

i.e. **the return type of calling the functor** `declval<F>()` with the args. madc
cannot compute that decltype → `__invoke_result::type` is absent → the `__void_t`
detection (which DOES work here — `tmp/vt1b`, `tmp/vt3b`) selects `false_type` →
`__is_invocable::value == 0`. So the blocker is: **madc can't dispatch `operator()`
on the result of `std::declval<F>()`** (a call-result, class-typed, rvalue receiver).

Verified prerequisites already landed this session (so the path is clear to here):
- `6ecb723` qualified static member-fn-template explicit-targ call.
- `e28c2c3` body-less member-fn-template return type (clang SubstDecl, return-type-only).
- `07a3b23` unqualified static member-fn-template call in expression context.

---

## 2. Exact current behavior (3-oracle, `--std=c++17 --no-embedded-headers`)

| reducer | expr | g++ | madc | note |
|---|---|---|---|---|
| `tmp/op1.cpp` | `c(1,2)` (named object) | 1 | **1** ✓ | the ONLY working receiver form |
| `tmp/op2.cpp` | `r(1,2)` (`Cmp& r`) | 1 | **2** ✗ | ref receiver — `(1,2)` parsed as comma-expr |
| `tmp/op3.cpp` | `b.c(1,2)` (member) | 1 | **2** ✗ | member receiver |
| `tmp/cc1.cpp` | `getcmp()(1,2)` (call-result, runtime) | 1 | err "lvalue required" | call-result receiver |
| `tmp/dc7.cpp` | `decltype(declval<Cmp&>()(1,2))` | ok | "incomplete type" | the decltype form |
| `tmp/dc5.cpp` | real `std::declval<less<int>&>()(...)` | ok | "incomplete type" | the real shape |
| `tmp/mc1.cpp` | `getcmp().run(1,2)` (NAMED method on call-result) | 3 | **3** ✓ | node-receiver method machinery EXISTS |
| `tmp/mc2.cpp` | `b.c.run(1,2)` (named method on member) | 3 | **3** ✓ | "" |

**Conclusion:** named-method calls on arbitrary receiver NODES already work
(`mc1`/`mc2`); only the implicit `operator()` dispatch (`obj(args)`) is stuck on a
**named-object** receiver. The fix is to make the `operator()` dispatch reuse the
already-working node-receiver method machinery.

---

## 3. Root cause (one code block)

`Program::parseExpr_operatorArm`, the functor-call branch
(**`src/parser.cpp` ~17305–17339**, "P2.1b gap 1 — functor call `obj(args)`"):

```cpp
if ( !exStack.empty()
  && paren_follows_object                         // prev_for_member is a ttIdentifier
  && !member_is_assign_lhs
  && exStack.top()->type() == TokenType::ttVariable     // <-- only a plain variable
  && (obj_call_base = dynamic_cast<TokenVar *>(exStack.top())) != NULL
  && obj_call_base->var.type
  && obj_call_base->var.type->is_object()               // <-- only a direct object (not ref)
  && (fcls = dynamic_cast<DataDefCLASS *>(obj_call_base->var.type)) != NULL
  && (fmethod = fcls->findMethod("operator()")) != NULL )
{
    TokenCallMethod *tc = new TokenCallMethod(obj_call_base->var, *fmethod);  // Variable& recv, no parent_expr
    exStack.pop(); ... parseCallMethod(tc); opStack.push(tc); ...
}
```

Three coupled limitations: (a) `ttVariable` only — excludes `ttMember`,
`ttCallFunc`, `ttCallMethod`, `ttOperator`, `ttSubscript`; (b) `var.type->is_object()`
— a reference is `DataDefPTR`+`vfREFERENCE`, not `is_object()`; (c) the receiver is a
`Variable&` with no `parent_expr`, so there is nowhere to attach a value-node receiver.
When the guard fails, the `(` falls through and `(1,2)` is parsed as a parenthesized
comma-expression (→ value 2 / "lvalue required").

---

## 4. The machinery to reuse (proven, makes `mc1`/`mc2` work)

Named-method call on a receiver NODE — `src/parser.cpp` ~15285–15442:
- Resolve the receiver class from the node: `struct_type = lhs_dot->datadef()`
  (or `TokenObjTemp::obj_class`); for an operator-result/temp it's `(a+b).m()`.
- Synthesize a receiver Variable of that class:
  `tv_var = new Variable("__op_expr", *struct_type, 1, NULL, false)` (line 15306).
- Build the method call and attach the node as the real receiver:
  `TokenCallMethod *tc = new TokenCallMethod(*tv_var, *var); tc->parent_expr = recv_parent;`
  where `recv_parent` is the node for `ttSubscript|ttOperator|tkObjTemp|ttMember|
  ttCallFunc|ttCallMethod|tkTypeid` (lines 15395–15413).
- `reselect_method_overload(tc, *tv_var, method_cls, id)` re-binds the overload once
  arg types are known (line 15427).
- CIR: `TokenMember::parent_expr` is materialized at codegen via `class_this_arg` →
  `translate_expr(parent_expr)` → address of the temp. **No new CIR work needed.**

Reference unwrap helper: **`Program::operand_object_class(TokenBase*)`**
(`src/parser.cpp:7966`) returns the `DataDefCLASS*` for a node, unwrapping a reference
(`is_reference()` / `vfREFERENCE` → `DataDefPTR::base_type`) and a direct class. This is
the one call that resolves the receiver class uniformly for object / ref / member /
call-result / operator-result. Returns NULL for non-class → guard falls through.

---

## 5. clang reference (the canonical model)

`Sema::BuildCallToObjectOfClassType` (clang `SemaOverload.cpp:15528`): a postfix
`E(args)` where `E` has class type builds an overload set from the object's
`operator()`s (+ surrogate call functions) over the **expression** `E` — never a named
declaration. Our `parent_expr` node IS that expression `E`; `operand_object_class`
selects the class; `findMethod("operator()")` + `reselect_method_overload` is the
(single-`operator()`, no-surrogate) subset sufficient for the std functors
(`less`/`equal_to`/lambdas). Surrogate conversion-to-fn-pointer is out of scope (none
of the set/map functors use it); note it as a future item, do not build it now.

---

## 6. Implementation (concrete)

Edit the functor-call branch (`src/parser.cpp` ~17305–17339). Replace the
`ttVariable`+`is_object()` guard with a node-general one:

```cpp
{
    TokenBase     *recv_node = exStack.empty() ? NULL : exStack.top();
    DataDefCLASS  *fcls      = recv_node ? operand_object_class(recv_node) : NULL;
    Variable      *fmethod   = fcls ? fcls->findMethod(std::string("operator()")) : NULL;
    if ( fmethod
      && paren_binds_to_receiver          // see §7 — the careful discriminator
      && !member_is_assign_lhs )
    {
        // Named OBJECT variable keeps today's direct-receiver path (no parent_expr);
        // every other value node uses a synthetic receiver var + parent_expr = node
        // (mirrors §4 / line 15306+15411), so refs / members / call-results work.
        TokenVar *tv = dynamic_cast<TokenVar *>(recv_node);
        Variable *recv_var; TokenBase *recv_parent = NULL;
        if ( tv && recv_node->type() == TokenType::ttVariable
          && tv->var.type && tv->var.type->is_object() ) {
            recv_var = &tv->var;                       // op1 path, unchanged
        } else {
            recv_var    = new Variable("__functor_expr", *fcls, 1, NULL, false);
            recv_parent = recv_node;                   // op2/op3/cc1/dc7 path
        }
        TokenCallMethod *tc = new TokenCallMethod(*recv_var, *fmethod);
        if ( recv_parent ) tc->parent_expr = recv_parent;
        exStack.pop();
        tc->file = tb->file; tc->line = tb->line; tc->column = tb->column;
        tb = parseCallMethod(tc);
        tc = reselect_method_overload(tc, *recv_var, fcls, std::string("operator()"));
        opStack.push(tc);
        if ( tb && tb->id() == TokenID::tkSemi ) done = true;
        return done ? ExprStep::Done : ExprStep::Break;
    }
}
```

Notes:
- `reselect_method_overload` signature: confirm it (used at 15427) and pass the
  synthetic `recv_var`; it re-binds by arg types and PRESERVES `parent_expr`
  (see `src/parser.cpp:8666–8668`). For a const `operator()` (the `less` case) verify
  the const-method overload binds (the functors' `operator()` is `const`).
- Keep the existing member-fptr branch (17340+) intact and AFTER this one.

---

## 7. THE careful part — the `(`-binds-to-receiver discriminator

Today's guard uses `paren_follows_object = prev_for_member->type()==ttIdentifier`
(the object was the immediately-preceding identifier). A node receiver's preceding
token is `)` / `]`, not an identifier, so a new predicate is needed. C++ has no
ambiguity — a `(` immediately after a *value* primary is always a call — but madc's
shunting-yard reaches this branch in several contexts, so gate precisely:

- **Fire only when** `recv_node` is a value already on `exStack` AND the current `(`
  immediately follows that value's last token. Use `prev_for_member` /`prevToken()`:
  accept `ttIdentifier` (named obj — today), `tkClBrk` `)` (call-result / paren),
  `tkClSqr` `]` (subscript). Reject when a higher-binding pending operator should
  consume first — BUT note the existing named path deliberately does NOT gate on
  `opstack_has_pending_op` (comment at ~17311: `cout << m(7)` must bind `(7)` to `m`).
  Mirror that: rely on prevToken-immediacy, not opstack emptiness.
- **Regression test matrix (must all stay correct — add as reducers/tests):**
  `(T)(x)` C-cast (handled earlier at ~16762, must still win); `(a)(b)` where `a` is
  not a functor (grouping then call — should error or stay as today); `f() * (x)`
  (binary, `(x)` is grouping, NOT a call on `f()`); `cout << f()` (no trailing `(`);
  `arr[i](args)` (subscript-result functor); `(a+b)(args)` (operator-result functor);
  a functor returned then called `make_cmp()(1,2)`; nested `f()(g())(h())`.
  The `f() * (x)` case is the one most likely to break — `f()` is a value on exStack,
  `*` is pending on opStack, then `(x)`. Here the `(` must NOT bind to `f()`. The
  discriminator: prevToken is `*` (an operator), not `)`/`]`/ident → do not fire.
  So the precise rule: **fire iff prevToken is the receiver node's own closing token**
  (`)`/`]`/identifier of the just-completed primary), i.e. no operator intervened.

---

## 8. Test plan / gate

1. Reducers to green (`--std=c++17 --no-embedded-headers`, 3-oracle vs g++):
   `op1`(stay 1) `op2`→1 `op3`→1 `cc1`→1 `dc7`→bool `dc5`→ok `mc1`/`mc2`(stay 3).
2. Add a couple as permanent integration tests (`tests/testfunctorcall*.mad`) with
   `.expect` fixtures — the functor-on-node forms are otherwise uncovered.
3. **`make -C src fulltest` MUST be 627/7** (same baseline) before committing — the
   blanket member-template-retention regression (621/13, reverted) proved that hot-path
   changes need the full gate. Watch especially: `testforeach*`, `teststringref`,
   `testsubscript*`, `testvector*`, operator-heavy tests.
4. Then re-probe the REAL chain: `tmp/dc5` (should compile/run), `tmp/ir3.cpp`
   (`__invoke_result<...>::type` resolves), `tmp/ir2.cpp` (`__is_invocable::value`
   → **1**), `tmp/seti.cpp` (set<int> advances past `_S_key`).

---

## 9. After this wall (sequencing to set/map)

Once `operator()`-on-node works and `__invoke_result::type` resolves:
- `__is_invocable_impl`'s `__void_t<typename _Result::type>` spec selects → `true_type`
  → `__is_invocable::value == 1` (row 7d done).
- `_Rb_tree::_S_key`'s `if constexpr (__is_invocable<...>{})` now folds to a CORRECT
  constant (row 7c) — the inner `is_invocable_v<>` static_assert is deferred
  (parser.cpp:6294) — and `_S_key` compiles. Re-run `tmp/seti.cpp`; expect a NEW,
  deeper `_Rb_tree` wall (this is a long instantiation — keep a fresh reducer at each).
- Residuals already mapped: `tmp/mt1` empty-result-struct sizeof 0-vs-1 (minor, not
  SFINAE); C++20 `.contains()` for testmap/testset (`--std=c++20` + a `contains`);
  row 6b `vector<T*>` element threading; `testcontainerdtor` invalid `.put()` test.

---

## 10. Process reminders (this codebase)

- NO SHIMS; fix at the deepest layer; 3-oracle (g++ AND clang) before changing.
- One reasoned pass; do not chain speculative micro-fixes.
- Single-command Bash calls (no `&&` chains). Reducers in `tmp/` with their real flags.
- `bash scripts/resume.sh` first on resume; read this plan + handoff §19i before editing.
