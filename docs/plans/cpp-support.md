# madc C++23 / C23 Compliance Roadmap

**Authoritative plan for madc's language-standard compliance.** Every C/C++
language task traces to a line in this document. Controller and subagents both
anchor here: pick the next unchecked item in priority order; do not invent work
that isn't on this roadmap, and do not re-solve something the Status table marks
DONE.

> Supersedes the 2026-05-24 "C++ Support Plan" (asmjit era), which declared
> *"full C++ compliance is not the goal."* That premise is **retired**. With the
> CIR → c2mir → MIR backend ("C all the way down", portable `--emit=c11`), the
> governing goal is now:

## North star

**Make madc as C23- and C++23-compliant as practical on the CIR backend.**
Anything that takes us off that path — no matter how locally tempting or easily
testable — is **drift** and is rejected. (Memory: `project_north_star_c23_cpp23`.)

## Non-negotiable principles (read before any task)

1. **One unified class machinery.** User classes and libstdc++ classes use the
   SAME path. The only difference: *precompiled* (a method's `FuncDef::emit_symbol`
   names a real libstdc++ mangled symbol / runtime wrapper, no body emitted) vs
   *needs-compiling* (madc emits `ClassName__method`). Never special-case a std::
   type. (Memories: `project_cpp_mangled_direct`, `project_string_as_class`.)
2. **No special-casing, no shortcuts, no shimming a symptom.** Fix at the deepest
   layer. The legacy `dtSTRING`/`tkSTRING`/`ns_stl` shortcuts were retired for a
   reason — do not reintroduce that pattern. (Memory: `feedback_dont_cling_to_legacy`.)
3. **gcc/clang is canon.** `g++ -S -fverbose-asm -O0` (annotated) / `clang -S` is
   the oracle for "what should this compile to." Reduce, compare, then fix.
   Verify emitted C with `--emit=c11` against stock `c2m`/`gcc`.
4. **Lowering vs raising.** Default Tier 1: lower/resolve in madc to ordinary C11.
   Raise c2mir (Tier 2) only for genuine semantic primitives; raise MIR (Tier 3)
   only for floor gaps (SIMD). See `.claude/rules/lowering-vs-raising.md`.
5. **Gate every change:** `make -C src fulltest`. Never drop the passing count.
   One concept per commit. Parser-grammar changes are load-bearing for the whole
   suite — highest regression risk; go incrementally.

## How to use this doc

- **Controller:** keep the Status table honest (re-probe before asserting), drive
  the Prioritized roadmap top-down, brief subagents with the relevant section +
  anchors + the principles above.
- **Subagent:** you were given a specific task from the Prioritized roadmap. The
  Status table tells you what already works (don't rebuild it) and what's broken
  (don't assume it works). Build only your task; report DONE_WITH_CONCERNS if it
  cascades.

---

## Status (CIR backend — re-verified 2026-05-31, empirical probes)

### C++ — working
| Feature | Notes |
|---|---|
| Classes: members, methods | bare unqualified member access in methods (`x = 42`) via `__this` |
| Constructors (default + args), destructors | `testctor*`; RAII via c2mir `cleanup` attribute |
| Single inheritance | base members at offset 0; `class D : public B` |
| `new` / `delete` | calloc + ctor / dtor + free |
| **std::string — full real class** | decl/ctor/dtor/methods/members/pointers/streams; operators `[] + == != < > <= >= = +=`; **return-by-value** via `__retbuf`. DONE this session. |
| **std::vector / map / set** | real `#include`-defined templates, monomorphized; element methods `v[i].method()` and `keys[i]==k` (keystone, DONE this session) |
| Template instantiation | Borland monomorphize; scalar / pointer / string type args |
| References — numeric `T&` | params lower to `T*` |
| Streams | `cout <<` chains, `cin >>`, manipulators via wrappers |
| `nullptr`, range-for over containers, user `namespace { }` blocks | namespace-block parsing added this session |

### C++ — broken or missing (the work)
| Feature | Symptom (evidence) | Roadmap item |
|---|---|---|
| Class by-value **return** | compiles, returns GARBAGE (`makeA(9)`→312984464); `__retbuf` is string-only | **P0.1** (in progress) |
| Class-**reference** param member access | `A& a; a.v` → "member reference is not a structure or union" | **P0.2** |
| **Virtual dispatch / vtables** | `p->who()` via base ptr → SIGSEGV | **P0.3** |
| Range-for over **raw array** | SIGSEGV (works for containers) | **P0.4** |
| **try / catch / throw** | CIR builder: "unhandled expression: TokenTRY" | **P1.1** |
| **Lambdas** | 2 c2mir check errors (regressed from old backend) | **P1.2** |
| User-class operator overloading | class-typed/self-type params don't parse; `this.member` qualified access unsupported (bare works) | **P1.3** |
| Operator dispatch completeness | `binop_overload_symbol` lacks compound/bitwise/shift/logical; no unary/`()`/`->` dispatch | **P2.1** |
| **`enum class`** (scoped enums) | parse: "Expecting '{' after enum" | **P2.2** |
| General `auto` deduction | "'auto' type deduction" unsupported (only fn-ptr/lambda) | **P2.3** |
| `const` **enforcement** | silently allows `const int x; x = 6;` | **P2.4** |
| Access control enforcement | uncertain — `private:`+accessor probe errored; verify | **P2.5** |

### C side (C11 → C23)
The CIR is a C11 AST consumed by c2mir. Broad C parity (toward `master`'s ~C89 +
GCC-torture ~97.9%) is tracked as **ROADMAP Track 1.3** (the develop→master gate)
— that worklist (~56 integration failures: vla / complex / struct-init / fnptr /
bitfield) is the C-side priority and is **not duplicated here**. C23-specific
surface (`typeof` [now standard], `_BitInt(N)`, `#embed`, `constexpr` objects,
`nullptr`/`static_assert`/binary literals, attributes) is mostly future; route
each per the lowering-vs-raising tiers. c2mir hard limits (no SIMD, no inline asm,
no wide chars) per `.claude/rules/c11-transpiler.md`.

---

## Prioritized roadmap

### P0 — correctness bugs (silent miscompiles / crashes) — FIRST
These produce wrong answers or crashes on valid C++. Highest priority.

- **P0.1 — class by-value return** *(in progress)*. Generalize the `__retbuf`
  struct-return ABI from std::string-only to any by-value class/struct (callee
  hidden `T* __retbuf`, copy-ctor/bit-copy into `*__retbuf`; caller temp). Anchors:
  `is_string_returning_call`, `string_call_temp_addr`/`string_temp_decl`,
  `retbuf_param`, func_def/func_proto, `translate_return` (cir_builder.cpp).
- **P0.2 — class-reference param member access**. `A& a; a.v` must resolve through
  the ref→pointer model carrying the class type. Reuse the numeric `T&`→`T*`
  machinery + class-reference handling.
- **P0.3 — virtual dispatch / vtables**. `p->who()` via a base pointer SIGSEGVs —
  the per-class vtable / `__vptr` indirect call (merged in `da4e1c5`) is broken on
  CIR. Diagnose with `--emit=c11` + gcc; fix the vptr layout / indirect-call lowering.
- **P0.4 — range-for over raw arrays**. SIGSEGV; `translate_foreach` must handle a
  C array (iterate `&a[0]..&a[n]` by element size) as well as the container path.

- **P0.5 — copy-assignment / `operator=` synthesis for classes with object
  members**. `B z2; z2 = makeB();` (assign a class-with-string-member into an
  EXISTING object) bit-copies and **double-frees** the shared string buffer. The
  P0.1 by-value-return fix handled decl-init/temp-read forms; this is the distinct
  copy-*assignment* path. Synthesize a memberwise `operator=` (destroy-old +
  member copy-construct, or copy-and-swap) for non-trivial classes; gcc canon.
  Found 2026-05-31 (DONE_WITH_CONCERNS on P0.1).

- **P0.6 — `arr[i]->method()` arrow-chain after subscript**. The dot-subscript
  keystone (commit `be5b063`) handled `v[i].method()`; the `->` variant
  (`ptrs[i]->who()`) still hits "chained arrow method call not yet supported".
  Blocks the classic `vector<Base*>` polymorphism pattern. Same parse location as
  the dot keystone — extend it to `->`. (Found 2026-05-31 during P0.3.)

- **P0.7 — `vector<T*>` multi-element read SIGSEGV**. Reading 2+ POINTER elements
  from a `vector<Base*>` crashes (`A* e=w[0]; A* f=w[1];` → SIGSEGV); a single
  pointer element works, and `vector<int>` (2+ elems) works. With initial `cap=4`
  no realloc occurs, so it's not the grow path — it's **pointer-element
  monomorphization**: `vector<T*>` likely mis-sizes the element / strides
  `operator[]`/`data+i` by the wrong width when `T` is itself a pointer, so
  element[1] reads at a bad offset → garbage pointer → crash on deref. Diagnose via
  `--emit=c11` of the instantiated `vector<A*>` (check `sizeof(T)`, the
  `new(slot)T(v)` placement-construct, and `T& operator[]` stride). `vector<Base*>`
  is the canonical polymorphism container, so this is high-value. (Found 2026-05-31
  during P0.6.)

### P1 — core C++ features the language is incomplete without
- **P1.1 — exceptions (try/catch/throw)**. Lower `TokenTRY` to SJLJ as `cir_node`
  in `CirBuilder` (CIR builder currently errors `unhandled expression: TokenTRY`;
  parser already tokenizes/parses try/catch/throw). **The SJLJ runtime is ALREADY
  LIVE** — `src/exception_runtime.cpp` survived the transpiler removal and is in the
  Makefile (`__madc_try_push/pop`, `__madc_throw_int/double/cstr`, `__madc_rethrow`,
  `__madc_exception_type/int/double/cstr`, `__madc_exception_clear`, cleanup stack).
  So P1.1 recycles NO runtime — only writes the lowering that calls it.
  **Read the curated, cruft-free reference: `docs/plans/refs/exceptions-sjlj.md`**
  (the runtime contract + the distilled lowering shape + the ONE real design
  decision: try-body dtor unwind on the longjmp path, since c2mir `cleanup`
  attributes do NOT fire on longjmp — use the runtime cleanup stack). That doc
  exists so the implementer NEVER opens the dead `madc_emit_c.cpp` and carries
  forward `gp_tree_node`/text-emission cruft. Old code is reference-ONLY.
- **P1.2 — lambdas**. Hoist to free functions + capture struct; fix the 2 c2mir
  errors. Was working on the old backend — re-establish on CIR.
- **P1.3 — user-class operator definitions**. Parser gaps block them: class-typed
  params (esp. the operator's own class / `const T&`), and `this.member` qualified
  access (bare member access already works). Once these parse, user-class operators
  ride the existing dispatch with NO new operator code (principle 1).

### P2 — completeness & polish
- **P2.1 — operator dispatch completeness**. Extend `binop_overload_symbol`
  (compound assign / bitwise / shift / logical) + add unary / `operator()` /
  `operator->` dispatch + scope the `tkAdd` string-concat guard to the string class
  (so user `operator+` isn't blocked). Becomes testable once P0.1/P0.2/P1.3 land.
- **P2.2 — `enum class`** (scoped enums). Parse `enum class`, namespace the values.
- **P2.3 — general `auto`** type deduction from an initializer expression.
- **P2.4 — `const` enforcement**. Error on assignment to a const lvalue.
- **P2.5 — access control** enforcement (verify current state first).
- **P2.6 — implicit derived→base pointer conversion**. `A* p = new B();` emits a
  cosmetic "incompatible types in assignment to a pointer" warning (layout is
  compatible — `__vptr` at offset 0; program runs correctly). Make derived→base
  pointer conversion implicit. (Found 2026-05-31 during P0.3.)
- **P2.7 — range-for by reference** (`for (int& v : a) v *= 2;`). The loop var
  currently COPIES each element (`x = a[i]`) in BOTH the array and class paths, so
  a `T&` loop var can't mutate the source. Make the reference form alias the
  element. Broad (both range-for paths), pre-existing. (Found 2026-05-31 during P0.4.)
- **P2.8 — tighten `translate_foreach` dispatch (kill the MadArray catch-all)**.
  range-for dispatches class→`size()/op[]`, fixed-array→indexed (P0.4), **else
  silently ASSUMES MadArray** (`madarray_size` on raw bytes). MadArray itself is a
  live feature (php::/perl:: dynamic arrays, `dtARRAY`) — but the catch-all is a
  fragile default: it's why a raw array crashed (P0.4), and a **VLA range-for STILL
  hits it and crashes**. Fix: gate the MadArray path on the actual `dtARRAY` type
  and make any unrecognized container a clear compile ERROR, not a silent MadArray
  cast. (Found 2026-05-31 reviewing P0.4.)

### P3 — broader standards surface (later)
- C-side parity worklist → **ROADMAP Track 1.3** (the develop→master gate).
- C23 surface (`_BitInt`, `#embed`, `constexpr` objects, attributes) per the
  lowering-vs-raising tiers; design Tier-3 (SIMD) for upstream MIR.
- Deferred-indefinitely (cost ≫ value on this backend): multiple inheritance,
  move semantics / rvalue refs, template metaprogramming (SFINAE/variadics),
  coroutines/concepts. Revisit only if a target codebase demands them.

---

## Cross-references
- `docs/plans/ROADMAP.md` — Track 1.3 (CIR coverage → master parity gate).
- `docs/adr/0001-cir-c2mir-backend.md` — backend decision (settled).
- `.claude/rules/lowering-vs-raising.md`, `.claude/rules/c11-transpiler.md`,
  `.claude/rules/gcc-methodology.md` — the how.
- `docs/superpowers/plans/2026-05-31-RESTART-HANDOFF.md` — session rehydration.
