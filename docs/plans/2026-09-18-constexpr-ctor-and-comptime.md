# constexpr constructors — next step, and where compile-time EXECUTION fits

Written 2026-09-18 after the owner asked (a) what the next step is for the
115-test constexpr family, and (b) whether madc should leverage its own JIT to
execute compile-time code — noting Zig/Jai support compile-time execution as a
LANGUAGE feature, which madc could inherit beyond C++ compiling.

Two separate questions with two different answers. They do not block each other.

## PART 1 — the 115-test family needs constexpr CONSTRUCTOR evaluation

Measured 2026-09-18 at 1369/1950 (70.2%):

    constexpr S s = {42,7};                 static_assert(s.i==42)  madc OK
    struct S { int i; constexpr S(int v) : i(v) {} };
    constexpr S s(42);                      static_assert(s.i==42)  madc FAILS

Aggregates fold (stage 1 carrier + array elements + flat/nested members all
landed). Objects built by a constexpr CONSTRUCTOR do not:
`initialize_static_struct_data` refuses any class with `has_user_ctor`.
Canonical lane case is cpp0x/constexpr-delegating.C:

    struct S { int i; constexpr S(int i) : i(i) {}
                      constexpr S() : S(j) {} };    // DELEGATING
    constexpr S s{};
    SA(s.i == 42);

### GCC's approach — reduce the ctor to an AGGREGATE initializer

    static tree massage_constexpr_body (tree fun, tree body)
    {
      if (DECL_CONSTRUCTOR_P (fun))
        body = build_constexpr_constructor_member_initializers
          (DECL_CONTEXT (fun), body);
                                            -- gcc/cp/constexpr.cc:797

    /* Build compile-time evalable representations of member-initializer list
       for a constexpr constructor.  */          -- constexpr.cc:625

GCC turns a constexpr ctor into a CONSTRUCTOR node — the same representation as
an aggregate initializer. **That is exactly the form madc already materializes.**

### The madc slice — adopt, do not invent

Everything needed exists:
  - `FuncDef::is_constexpr`, `constexpr_return_tokens`, `constexpr_call_bindings`,
    depth limit 512 (47e07b916)
  - `evaluate_constexpr_function_call` — the bind-params-and-re-evaluate pattern
  - the 15-rung evaluator returning `ConstValue`
  - `initialize_static_struct_data` / `initialize_static_subobject_data` and
    `store_static_integer_array_value` — the materialization writers
  - `vfCONSTBAKED` — the guard that keeps ALLOCATED storage from reading as an
    initialized value

Shape: save the ctor's member-initializer expressions (the twin of
`constexpr_return_tokens`); at a `constexpr S s(args)` declaration bind the
parameters through `constexpr_call_bindings`, evaluate each member-init through
the existing rungs, write the results with the existing writers, set
`vfCONSTBAKED`. Delegation (`S() : S(j) {}`) recurses into the delegated ctor
over the same storage first.

⚠️ C++11 constexpr ctors are NARROW — empty body, ctor-initializer list only.
That is why this is bounded. C++14 relaxed constexpr (loops, mutation) is a
different and much larger problem; do not design for it here.

## PART 2 — compile-time EXECUTION: not for C++ constexpr; yes as a madc feature

### Why C++ constexpr must NOT be native JIT execution

1. **EXECUTION CANNOT ANSWER THE QUESTION.** ⚠️ I first wrote "native execution
   yields a SIGSEGV"; that is WRONG, and wrong in the direction that
   UNDERSTATES this. Crashes are the RARE case (null deref, div-by-zero
   SIGFPE, unbounded recursion). The common case is NO FAULT AT ALL:

       constexpr int f() { int a[3] = {1,2,3}; return a[5]; }
       static_assert(f() == 0, "");
       g++ -std=c++14: error: non-constant condition for static assertion
       executing it:   reads adjacent stack memory, returns a plausible number

   Out-of-bounds reads, uninitialized reads and signed overflow all just
   produce a value. Execution computes something either way; it has no notion
   of "is this a constant expression?". gcc and clang answer that by evaluating
   SYMBOLICALLY over modeled objects (CONSTRUCTOR trees / APValue) — that is
   what makes the diagnostic possible, not the speed of the engine.
2. **SFINAE needs RECOVERABLE failure.** Constant evaluation runs during
   overload resolution and deduction; a crashed JIT cannot be recovered from.
   (11 of the lane's 24 `static assertion failed` tests are SFINAE.)
3. **UB must be REJECTED** — signed overflow, out-of-bounds, uninitialized
   reads. Native execution silently produces garbage: a SILENT WRONG ANSWER,
   the worst outcome by this project's own rules.
4. **It runs on dependent/incomplete code** during deduction, which cannot be
   codegen'd at all.
5. **Neither canon compiler does it.** GCC interprets trees
   (gcc/cp/constexpr.cc, 13272 lines). Clang interprets
   (ExprConstant.cpp, 16724 lines) and its PERFORMANCE answer is a
   stack-based, strongly-typed BYTECODE VM (clang/lib/AST/Interp/), still
   behind `-fexperimental-new-constant-interpreter`:

   > "The constexpr interpreter aims to replace the existing tree evaluator in
   > clang, improving performance on constructs which are executed
   > inefficiently by the evaluator."   -- clang/docs/ConstantInterpreter.rst

   Even the fast path is a typed VM, because it must still diagnose.
6. **Cross-compilation — a REAL constraint, but NOT the everyday one.**
   ⚠️ OWNER CORRECTION 2026-09-18: madc is NOT a cross-compiler in standard
   operation (host lexer -> parser -> CIR -> c2mir -> MIR execute); the
   `cross-aarch64-linux` / `cross-x86-64-macos` / `cross-arm64-macos` /
   `hosted-arm64-macos` targets are opt-in MODE= builds. I originally led this
   list with cross-compilation, which OVERSTATED it: on the host target,
   executing constexpr natively WOULD compute correct sizes. It bites only when
   madc is deliberately used as a cross compiler — which it supports, so the
   evaluator must still be target-parameterized, but it is not the argument
   that decides this. Items 1-4 decide it, and none depends on the target.

### ⚠️ MIR INTERPRETED MODE DOES NOT CHANGE THIS (owner raised it, 2026-09-18)

MIR supports an interpreter, and it correctly removes every JIT-SPECIFIC
objection (codegen cost, W^X, portability). But it does not change the
conclusion, because the ENGINE was never the issue. Verified in
third_party/mir/mir-interp.c — the interpreter does a RAW dereference:

    #define LD(op, val_type, mem_type)          \
        int64_t a = get_mem_addr (bp, ops + 1); \
        *r = *((mem_type *) a);                 \

No bounds checks, no sanitization anywhere in that file. Identical memory
semantics to the JIT. `a[5]` on a 3-element array reads adjacent memory under
both. **Execution engine (JIT vs interpreter) is ORTHOGONAL to semantic
checking**; constexpr needs the checking.

### Where compile-time execution DOES belong

As a **madc dialect feature** (Zig `comptime`, Jai `#run`), it is attractive and
differentiating, and `.claude/rules/backend-strategy.md` already sanctions
direct-MIR for "runtime internals … and the interpreter / REPL / debug tier".
A MIR-INTERPRETER tier is the right engine: it can bound execution, refuse UB
and be stopped — none of which native code can do.

⚠️ UNVERIFIED (no Zig/Jai sources locally): my understanding is that Zig
evaluates `comptime` inside the compiler rather than by executing host code,
precisely because cross-compilation is first-class for it, and that Jai's `#run`
goes through a bytecode VM. Verify before citing either as precedent.

### They do not block each other

Part 1 is a bounded slice on the existing evaluator and should not wait for a
comptime engine. If a comptime VM later lands, C++ constexpr can migrate onto
it — provided it preserves diagnosis and TARGET (not host) semantics. That is
exactly clang's own migration path from `ExprConstant.cpp` to `Interp/`.

---

# PART 3 — EXECUTION PLAN (constexpr ctor evaluation)

Target: the 115-test `Expecting integer constant expression` family, the
biggest single block toward the owner's 75% goal (1463/1950, +94 from 1369).

## ⚠️ STEP 0 — VERIFY BEFORE BUILDING (do not skip; this is where today's
## wrong turns came from)

`FuncDef` ALREADY carries what this needs:

    bool is_constexpr;                                    // madc.h:290
    std::vector<TokenBase *> constexpr_return_tokens;     // madc.h:291
    std::vector<TokenBase *> forest_ctor_init_tokens;     // madc.h:296
      // "A ctor's MEM-INITIALIZER-LIST tokens, captured beside the body span
      //  when the body came through parse_deferred_function_body ...
      //  Rides DK_DEFBODY run slot 3 (ctor_init_tokens)."

**PROVE, with a DBG print or a debugger, that `forest_ctor_init_tokens` is
populated for `struct S { int i; constexpr S(int v) : i(v) {} };` before
designing anything around it.** The comment scopes the capture to bodies that
came through `parse_deferred_function_body` (in-class inline bodies, which a
constexpr ctor normally is) — verify, do not assume. If it is NOT populated,
the first slice is to capture it, twinning
`retain_constexpr_return_expression` (src/parser.cpp:67863), which is the
existing model: clone with `clone_origin()`, bound the span with `DelimDepth`.

## THE OWNERS TO ADOPT — all exist, none to be invented

| need | existing owner |
|---|---|
| is this fn constexpr | `FuncDef::is_constexpr` |
| the ctor's init-list tokens | `FuncDef::forest_ctor_init_tokens` |
| bind ctor parameters | `constexpr_call_bindings` (depth limit 512) |
| evaluate one init expression | the 15-rung `ConstValue` evaluator |
| the call-evaluation pattern | `evaluate_constexpr_function_call` (:17931) |
| write bytes into storage | `initialize_static_subobject_data` / `store_static_integer_array_value` |
| refuse uninitialized storage | `vfCONSTBAKED` |
| where aggregates materialize | the decl site, src/parser.cpp ~:72967 |

GCC's shape, which this mirrors: reduce the ctor to the AGGREGATE initializer
form (`build_constexpr_constructor_member_initializers`, cp/constexpr.cc:797) —
i.e. produce exactly the bytes `initialize_static_struct_data` already writes,
then set `vfCONSTBAKED` so `read_constant_subobject` will read them.

## SLICES — commit at each green, own reducer + dual oracle each

**S1 — non-delegating constexpr ctor, integer members.**

    struct S { int i; constexpr S(int v) : i(v) {} };
    constexpr S s(42);
    static_assert(s.i == 42, "");          // g++/clang++: accept

  At the decl site, when `gotconstexpr` and the class has a constexpr ctor
  selected: bind the ctor's parameters into `constexpr_call_bindings`, evaluate
  each mem-initializer through `parse_constant_integer_expression`, write via
  the existing writers, set `vfCONSTBAKED`. Body must be EMPTY (C++11).
  Gate: the reducer + `testconstexpr*` + `testenum*` + `teststatic*`.

**S2 — delegation.** `constexpr S() : S(j) {}` — recurse into the delegated
  ctor over the SAME storage first, then apply this ctor's own inits.
  Canonical lane test: cpp0x/constexpr-delegating.C. Guard the recursion with
  the existing depth limit; a cycle must be a LOUD refusal.

**S3 — widen members.** Nested class members, NSDMI on a ctorless member, and
  a constexpr ctor whose argument is itself a constexpr call.

**S4 — measure.** Run the lane LOCALLY (~6 min; `MADC_GXX_ROOT=/workspace/gcc/gcc/testsuite`).
  Diff the failure LIST against `docs/parity/host-suite-failing.txt` for the
  host suite. Report the family delta, not just the total.

## ⚠️ RULES THAT BIND THIS SLICE

- **When a correct fix moves the metric by ZERO, DIAGNOSE before concluding.**
  That error cost a wrong recommendation today (see the CORRECTION in
  scratchpad/measure-constexpr-was-not-the-lever.md).
- A reducer must be narrowed to what the fix GOVERNS — a reducer containing an
  unrelated failing shape fails on every branch and proves nothing.
- The integer path must stay byte-identical: these rungs are on the path of
  EVERY array bound, enum initializer and non-type template argument.
- Never compare failure COUNTS; diff the LIST.
- Rule trailers on every src/ commit; `Layer:` must say why yours is deepest.
