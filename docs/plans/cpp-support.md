# madc C++23 / C23 Compliance Roadmap

> **GOVERNED BY** `docs/plans/madc-vision-and-invariants.md` — the north-star vision
> (madc as a polyglot transpiler; cir_node as the universal C/C++ IR) + the **invariants
> I1–I8** every change must satisfy. Run its "does this block the vision?" checklist
> before any change. This roadmap is how we get there without blocking it.

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
6. **Dialect gating via the `--std=` enum — TWO axes, one enum.** `LanguageStd`
   (madc.h:1005; default `STD_MADC`, the permissive superset) drives both:
   (a) **source-input dialect** — gatekeeps which keywords/features are active so C
   and C++ don't clash (`class`/`new`/`operator` are C identifiers; `auto` =
   storage-class in old C but deduction in C++/C23); the lexer already gates many
   C++ keywords behind `!is_c_mode()` (lexer.cpp:1309-1339). **Every C++ feature
   added MUST declare its `--std=` floor** (keyword AND feature dispatch).
   (b) **c2mir backend-target std** — the C standard the lowering EMITS TO; currently
   hardcoded C11 (`emit_lang` celC11, madc.cpp:448). Should be a `LanguageStd` value
   (`c2mir_target_std`, default `STD_C11`) referenced by lowering + emit, BUMPABLE as
   c2mir's C support advances — and feeding lowering-vs-raising (a C23 target can pass
   through `typeof`/`_BitInt` instead of lowering). See `project_std_enum_gatekeeping`.

## Execution sequence (user-agreed 2026-05-31)
1. **Core C++ features FIRST** — P0 (done) → P1 → P2, until the class model is
   genuinely usable as intended.
2. **THEN a C-stability pass** — pivot to ensuring plain C still compiles/runs
   correctly: the ~56 CIR integration failures (ROADMAP Track 1.3) + a full
   C-regression sweep. Rationale: several P0 fixes touched SHARED lowering
   (`member_node` pointer rendering, assignment, member access, `new`/struct
   paths) that C also exercises — the suite stayed green throughout, but a
   deliberate C sweep before promotion is the right hygiene.
   (Memory: `project_sequencing_cpp_then_c`.)

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
| **`<=>`** (C++20 three-way comparison) | gated + loud error (slice 1, `90e5f5c`); faithful `<compare>` lowering pending — `std::strong_ordering` doesn't register from the real header yet | **P2.15** |

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

> **P1.1 status (2026-05-31, commit `2444f85`):** Phase A DONE — single-level
> `try/catch/throw` for scalar types (int/double/cstr) works via the SJLJ runtime
> (`testexcept`, `testexcept_dtor_nothrow`, `testtrycatch` green). Remaining
> exception follow-ups (the hard tail):
> - **P1.1b — MIR JIT `setjmp`/`longjmp` frame bug (fork-level; C-stability/MIR pass)**.
>   ROOT CAUSE (investigated 2026-05-31, intel below): the MIR JIT does NOT model
>   `setjmp` as a **returns-twice** function, so locals live across the `setjmp`
>   aren't forced to memory; after `longjmp` re-enters the frame, register-allocated
>   locals are stale/clobbered → faults. Two surface symptoms, same root: (a) rethrow
>   to an outer try infinite-loops (outer `setjmp` re-returns 0 on re-entry); (b)
>   exception unwind SIGBUSes (null deref) in JIT'd `main` at a post-`longjmp` offset.
>   EVIDENCE: scales with FRAME SIZE not object count — 1 `std::string` (≈32B) OR ≥3
>   simple objects in a throwing try crash; 1–2 small objects work; plain scope-dtors
>   (no `setjmp`) always work; crash is in JIT'd `main` (never a `__madc_*` runtime
>   frame); identical at `-O0` and `-O1`. Lowering is gcc-correct (emit-c11 path).
>   So it's NOT runtime-fixable. **FIX IT CORRECTLY = FORK FIX** (`/workspace/mir`):
>   make c2mir treat `setjmp`/`__builtin_setjmp` as returns-twice and spill locals live
>   across it (what gcc/clang do); design for upstream. The madc-side `volatile`-the-
>   try-locals trick is a SHIM (avoid — violates fix-at-deepest-layer; last resort only).
>   Confirm via a minimal C `setjmp`-reducer before fixing. `.mir_skip`'d: `testrethrow`, `testexcept_dtor_rethrow`,
>   `testexcept_dtor_{string,order,nested}`.
> - **P1.1c — Phase B: RAII dtor-unwind on the throw path** — DONE at the FRONT END
>   (commit `b9c7829`, gcc-verified): cleanup-stack + discard-on-normal; objects in a
>   try body register on the runtime cleanup stack, `__madc_throw_*` unwinds to
>   `cleanup_mark`, normal exit discards (cleanup-attribute handles normal teardown);
>   dtor runs exactly once per path. 3 exception tests flipped green (incl. a
>   cross-function throw). REMAINING is a MIR-backend bug (folded into P1.1b below):
>   `testexcept_dtor_{string,order,nested}` crash in the MIR JIT on RE-ENTRANT dtor
>   calls during unwind + longjmp (3+ objects or a string object) — lowering is
>   gcc-correct (emit-c11 → gcc runs clean), `.mir_skip`'d. Front-end edge: try-body
>   objects declared with NO ctor args (`Foo f;`, the no-arg vars-loop path) don't yet
>   get the cleanup-push — follow-up (no in-scope test exercises it).
> - **P1.1d — catch-clause parser gaps**: `catch(char* m)` (pointer type) doesn't
>   parse; typed cstr/string catch binding needs parser+binding work (used
>   `catch(...)` as the interim).

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
- **P1.3 — user-class methods/operators using the SELF type**. Root-caused
  2026-05-31: the class registers as a type (`struct_map`/`datatype_map`) at
  parser.cpp **~11280-81, AFTER** the member/method-parse loop (`parseFunction` at
  11094/11120/11216). So a method whose param/return is the class being defined
  can't resolve the self-type → "Failed to find type 'C' when parsing function
  parameters" / "Expecting type in class definition". Blocks `int operator+(const
  Counter& o)`, `V operator+(const V&)` returning V, `C add(C o)`.
  - **P1.3a (high value):** register the class in `datatype_map`/`struct_map`
    EARLY — right after `ddc` is created (~11001), before the body loop — so
    self-references resolve (incomplete-type forward-self-reference; finalize size
    at the existing ~11281 point). Then user operators ride the existing dispatch
    with NO new operator code (principle 1). RISK: early registration affects name
    resolution + by-value self-param sizing — gate carefully.
  - **P1.3b (lower):** bind `this` in method bodies — `this.member`/`this->member`
    → "use of undeclared identifier 'this'" (bare member access already works via
    `__this`).

### P2 — completeness & polish
- **P2.1 — operator dispatch completeness**. Extend `binop_overload_symbol`
  (compound assign / bitwise / shift / logical) + add unary / `operator()` /
  `operator->` dispatch + scope the `tkAdd` string-concat guard to the string class
  (so user `operator+` isn't blocked). Becomes testable once P0.1/P0.2/P1.3 land.
- **P2.1b — operator parser gaps** (P2.1 delivered binary-set + unary + prefix
  inc/dec CIR dispatch; these need PARSER work, found 2026-05-31): `operator()`
  (`obj(args)` isn't routed to `operator()` — the call site mis-parses; the method
  body emits fine); `operator->` (`obj->m` errors "must be a pointer" — rewrite as
  `(*obj.operator->()).m`); postfix `++/--` (parser encodes no dummy-int param, so
  prefix/postfix collapse to one nullary `operator++` = prefix only); unary+binary
  same-name on one class (`operator-()` AND `operator-(const C&)` — the binary param
  fails to register, a same-name-overload parser gap).
- **P2.2 — `enum class`** (scoped enums). Parse `enum class`, namespace the values.
- **P2.3 — general `auto`** type deduction from an initializer expression.
- **P2.4 — `const` enforcement**. Error on assignment to a const lvalue.
- **P2.5 — access control** enforcement (verify current state first).
- **P2.5b — `class` defaults members to PRIVATE (correct the legacy artifact).**
  madc currently defaults unlabeled `class` members to PUBLIC — a LEGACY ARTIFACT,
  not a constraint. C++'s model (which we are building toward) is `class`→private,
  `struct`→public; `class Q { int m; }; q.m` from outside must be rejected. FIX:
  initial `access_flags` = `vfPRIVATE` for `class`, `0` (public) for `struct`, in
  `TokenCLASS::parse` (parser.cpp ~11104). The container template headers
  (`include/madc/vector|map|set`) declare unlabeled members — add explicit `public:`
  where external/class-model access needs them (most member access is internal via
  methods, so verify what actually breaks). NOT deferred — part of finishing P2
  correctly. (Invariants I6/I8; `feedback_dont_cling_to_legacy`.)
- **P2.5c — access control on METHODS**. P2.5/P2.5b enforce access on DATA members
  only; a private method called externally is not yet rejected (methods resolve via
  `findMethod`, not the `member_access`-checked path). Extend the access check to
  methods. (Found during P2.5b; consistent gap with P2.5's data-only scope.)
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

- **P2.10 — fn-ptr to an object-returning function (string-returning lambda)**.
  A lambda `return "hi";` deduces a `std::string` (object) return, which routes the
  hoisted fn through the `__retbuf` ABI — but `DataDefFPTR` can't render a fn-ptr to
  a retbuf-returning function (emits `char (*)(int)`, segfaults). Scalar/pointer
  lambda returns are fixed (P1.2); object returns need the fn-ptr-to-retbuf ABI.
  Intersects P1.1c/the __retbuf machinery. (Found 2026-05-31 during P1.2.)
- **P2.8b — `string&` references not wired through operator dispatch** (pre-existing,
  surfaced during P2.7). A `std::string&` reference (parameter OR `for (string& s : c)`
  loop var) can't use string operators — `s += "x"` on a string-ref → "invalid operand
  types of +". String references aren't routed through the class-operator dispatch the
  way string values/members are. Wire the vfREFERENCE-string case into
  `class_operator_call`/`string_obj_arg`. Scalar `T&` refs work; only string refs gap.
- **P2.9 — `DataDefPTR` double-pointer vs reference type-code overlap (latent
  hazard)**. `DataDefPTR` stacks `rtPtr` twice for `T**`, so a double-pointer's raw
  `type()` code lands in the `rtRef` (20000+) range — i.e. `is_pointer()` returns
  FALSE on the raw code of an `A**`. Anything that classifies by raw `type()`
  instead of the `DataDefPTR` dynamic type (`dd_ptr_depth` / `dynamic_cast`) can
  misread a double-pointer as a reference. The P0.7 fix is robust (counts via the
  override), but this overlap is a soundness bug waiting to bite. Fix the type-code
  scheme so `T**` ≠ a reference code. (Found 2026-05-31 during P0.7.)

- **P2.11 — keyword/feature → standard REGISTRY (then table-driven gating)**. THE
  keystone of the std-dialect subsystem. Build a DECLARATIVE table mapping each
  keyword + language feature to `{introduced_in, removed/deprecated_in, dialect
  family}`; then gating, conversion (P2.13), and target selection (P2.12) are all one
  lookup. This replaces the current AD-HOC scattered `if (!is_c_mode())` checks
  (lexer.cpp:1309-1339 — a binary C-vs-not-C split; the "hard-coded specifics"
  anti-pattern). Then close the gaps it exposes: `operator` (`tkOPEROVER`,
  lexer.cpp:1322) registered UNCONDITIONALLY (a C identifier); audit lambda `[...]`,
  `auto` (C23 added deduction — not pure C++), `enum class`, and FEATURE dispatch;
  `--std=c11/c17` must reject/ignore C++-only constructs. Core part of the
  C-stability pivot. (Principle #6; `project_std_enum_gatekeeping`.)

- **P2.12 — c2mir backend-target std as an enum (de-hardcode C11)**. Introduce a
  `LanguageStd c2mir_target_std` (default `STD_C11`); replace hardcoded C11 / the
  `celC11` emit assumption (madc.cpp:448) and any "c2mir is C11" lowering decisions
  with references to it; unify the render-target language selection with the shared
  `LanguageStd` enum (per `.claude/rules/mc11-ir.md`). Wire lowering-vs-raising to
  query it (target ≥ C23 → pass `typeof`/`_BitInt` through instead of lowering). So a
  future c2mir C-standard upgrade is a one-constant bump. (Principle #6b; forward-
  looking; do alongside / after the C-stability pass.)

- **P2.13 — standards CONVERSION (input-std ≠ output-std)**. A direct payoff of the
  one-IR + dialect-enum-on-both-ends design: madc as a dialect transpiler — C++23→C11/
  C89 (Cfront downlevel, partly the existing C++→C lowering), C23→C11 (downlevel),
  C89→C23 (modernize), madc→portable C11 (`--emit=c11`, already first-class). "Convert"
  = parse at one `--std=` level, emit at another. Depends on P2.11 (input gating) +
  P2.12 (enum-driven output target). Forward-looking capability; `project_std_enum_gatekeeping`.

- **P2.14 — RETIRE `dtSTRING`/`dtSTRINGref` tags; std::string fully generic class
  (complete the string-as-real-class refactor).** The std::-types refactor removed the
  dtSTRING *lowering* but std::string still carries the special `dtSTRING` DataType tag
  (and `string&` = `dtSTRINGref`), and ~130 sites (50 cir_builder + 83 parser) key off
  `dtSTRING` to RECOGNIZE the string class. INCONSISTENT: vector/map/set were fully
  de-special-cased (their dt-tags removed in the hygiene sweep — generic DataDefCLASS),
  but string kept its tag. This residual special-casing is the root that spawned the
  type-code workarounds: `canonical_string_class` (P2.8b) and `dd_ptr_depth` (P0.7), and
  it's most of P2.9 (the dtSTRINGref→dtSTRING rtReference-offset overlap). FIX (correct
  completion, per `feedback_correct_over_shortcuts`): retire `dtSTRING`/`dtSTRINGref`,
  make std::string a generic class type recognized by CLASS IDENTITY (`== ddSTRING` /
  `as_class_instance`==the string class) like vector/map/set; this dissolves P2.9 + both
  shims. SUBSTANTIAL (~130 sites → class-identity checks) — a deliberate refactor, not a
  quick fix; supersedes/absorbs P2.9. Found 2026-05-31 (user: "I thought we got rid of
  dtSTRING?").

- **P2.14 — DONE (2026-05-31, grep-VERIFIED).** `grep dtSTRING\|dtSTRINGref src/ include/`
  = **0** (coordinator-verified, not just claimed); `DataDefSTRING` carries the generic
  `dtRESERVED` tag; std::string recognized purely by class identity like vector/map/set;
  `canonical_string_class` shim removed; **P2.9 dissolved** (no more dtSTRINGref→dtSTRING
  +20000 overlap). Commits ddcaa80→4c049cc (typespec_t DataDef* registration · MadValueKind
  · Variable ctor/dtor by identity · enum deletion + repoint + shim removal). 409 green
  throughout, 0 regressions. Running count 219→0. The resilient legacy crutch is eliminated.
  Historical (the journey): Done (commits `bbaddce`/`66c91f1`/`9930af7`/
  `c951efa`, 409 green): added `is_std_string*`/`is_string_class()` class-identity
  recognizers and migrated ALL `rawtype()==dtSTRING` recognition to them — **0**
  recognition sites remain in core files. But **219 `dtSTRING`/`dtSTRINGref`
  occurrences remain** because the tag is also the **type-naming vocabulary of the
  builtin-registration ABI** — THIS is why it's been resilient. Remaining work to reach
  grep=0 (each independently committable, gated):
  1. **Registration ABI (~160 sites: ns_php/perl/python/ruby/js/rust + parser string
     method/operator/ctor regs):** `datatype_vec_t` (= `vector<DataType>`) names string
     by the `dtSTRING` enum value (resolved to `&ddSTRING` by `DataType_to_dd`). Add a
     `DataDef*`-based param spec so string is named by `&ddSTRING`, migrate the sites.
  2. **MadValue union discriminator** (`datadef.h` ~837-926, `datatokens.h:142`): the
     PHP mixed-array string variant uses `type==dtSTRING` — give it a non-dtSTRING
     discriminator.
  3. **char*/string routing paths** that key off `ddSTRING.rawtype()==dtSTRING` — the 3
     that segfaulted on the trial repoint (testcaststringtolong / testternarystrcharptr
     / testvariadicterstrtwice / testsystemcharbuf; likely cir_emit_c.cpp / char*-literal
     coercion) — trace + fix.
  4. **Then** repoint `DataDefSTRING`/`ref` → `dtRESERVED`/generic class tag, DELETE the
     `dtSTRING`/`dtSTRINGref` enum values, remove the `canonical_string_class` shim.
  5. **grep=0 verify** (`grep dtSTRING\|dtSTRINGref src/ include/` → 0) + 409+ green.
  Absorbs P2.9. DEFINITION OF DONE = grep returns zero; recognition-migration alone is
  NOT done (the tag still exists = drift risk).

- **P2.15 — `<=>` three-way comparison (C++20).** Slice 1 LANDED
  (2026-06-11, `90e5f5c`): the token is gated at the C++20 std floor
  (STD_MADC + `--std=c++20`+; below the floor it lexes `<=` then `>` and is
  rejected at parse — test `test3waygate`), and the CIR builder's
  unhandled-binary-operator default is a LOUD error_node (it silently
  lowered any unmapped operator as N_ADD — `a <=> b` compiled as `a + b`
  in the madc dialect since the token existed). `a <=> b` now errors
  cleanly until the faithful lowering lands. PREREQUISITE found by probe:
  `std::strong_ordering` does NOT register when parsing the real
  `<compare>` (`use of undeclared identifier` — the category classes, their
  constexpr class-typed statics `less`/`equal`/`greater`, and their
  hidden-friend `operator@(ordering, __cmp_cat::__unspec)` need real-header
  parsing work first; g++ -O0 canon: `<=>` itself is an INLINE byte-select
  into `_M_value`, `r < 0` CALLS the TU-local friend
  `_ZStltSt15strong_orderingNSt9__cmp_cat8__unspecE`). **Slice 2a LANDED**
  (`c8fdb48`): the category types register and carry correct values from
  the REAL `<compare>` (test `testcompare_realhdr`, g++-verified -1 0 1 2)
  via five general fixes — `__cplusplus` tracks `--std=` +
  `__cpp_impl_three_way_comparison` at the C++20 floor (feature-test
  macros describe THIS compiler, not the capture host; `__cpp_concepts`
  deliberately undefined), scope-relative/nested
  `resolve_namespaced_type_token`, namespace-qualified scoped-enum
  pseudo-namespaces (+ `parsePostfixChain` walk), file-scope ctor-syntax
  declarations record top_decls (out-of-class static member definitions
  kept their ctor args), and CLASS-typed static members resolve to their
  storage instead of the silent-0 integral fold. **Slice 2a′ LANDED**
  (`fcceefb`): standalone `#include <compare>` works — g++'s chain
  duplicated by defining `__cpp_concepts=202002L` at the C++20 floor
  (constraints CONSUMED, never evaluated — constrained declarations parse
  as if unconstrained; `<concepts>`' active body carries `<type_traits>` →
  `bits/c++config.h` in, exactly like g++) + requires-clause /
  trailing-requires / concept-definition consumption in the template
  skippers + `using NAME = type;` alias names may shadow registered type
  names. Remaining plan:
  1. Builtin scalars: parse `a <=> b` as a binary operator. Lowering
     semantics RULED (user, 2026-06-11): FAITHFUL `std::strong_ordering` /
     `partial_ordering` category objects from the real `<compare>` header
     (g++/clang canon; required for `--std=c++20` conformance) — no
     pragmatic-int shape. GATE the token at the C++20 std floor via the
     LanguageStd enum (`--std=c++17` must reject it once parsing exists;
     today it lexes ungated — [[project_std_enum_gatekeeping]] pattern).
  2. Class `operator<=>` overloads — ride the existing member/free
     operator machinery (operand_object_class-aware) + mangled-direct /
     body instantiation for std types.
  3. Rewritten candidates (`a < b` → `(a <=> b) < 0`, reversed `==`) and
     `= default` generation for `operator<=>` — parser overload-resolution
     work, last.

### P3 — broader standards surface (later)
- **Polyglot transpiler (far-future direction).** The endgame generalizes the
  std-dialect subsystem: madc supports features AND syntax from other languages, so
  the input axis becomes "source LANGUAGE + version" and the P2.11 registry becomes
  keyword/feature/syntax→{language, version}. madc then transpiles language X → target
  Y through the one IR (standards-conversion P2.13 = the C/C++ instance of this). Keep
  the std-dialect architecture general enough not to preclude it. See
  `project_madc_vision`.
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
