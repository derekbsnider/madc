# madc Roadmap

Master plan linking all workstreams. Updated 2026-06-22 (v0.30.0 — set wall
cleared: real-libstdc++ `std::set`/`std::map` working on the default C++17 path).

**Backend reality:** `madc parser → cir_node (MC11-IR) → c2mir → MIR → JIT` is
the **sole** backend — asmjit and the Gecko parser/MIR-transpiler are gone. The
**central near-term goal is feature parity with `master`** (which still carries
the removed asmjit backend at full C89 coverage): `develop` is not promoted to
`master` until the CIR path's test coverage matches or exceeds it. Track 1.3
(CIR coverage) is therefore the gating workstream — most other tracks are
re-established or unblocked behind it. Where a track below was completed on the
old backend, it is marked "proven on old backend, re-establishing on CIR."

## The Intermediate Representation — MC11-IR (SET IN STONE, 2026-05-29)

The primary in-memory representation is the **Mad-enhanced-C11 IR (MC11-IR)** —
the `cir_node` AST tree. `cir_node` **derives from c2mir's `node_t`** (so c2mir
consumes the lowered C11 view directly) AND each node **carries its originating
lexed tokens + parse subtree + file/line/column** (so madc retains the original
high-level structure without reconstruction). It is deliberately **both**:
lowered for c2mir, high-level for madc. The `.mc11` text is the on-disk
serialization of the extra info; render targets (C11/MC11/C++/madc) share the
`--std=` language enum to pick which view to emit. See
[`docs/rules/mc11-ir.md`](../rules/mc11-ir.md). **Do not re-pose "lowered vs
high-level" — the answer is both.**

## Current State

- **Version:** `0.30.0` (per `VERSION`) — released on `develop` (CIR backend);
  the **set-wall** release: real-libstdc++ `std::set`/`std::map` (incl.
  `std::map<std::string,std::string>`) compile and run on the default C++17
  real-header path, eight root-cause container bugs cleared, fulltest
  **669 passed / 0 failed / 0 timed out / 18 skipped**, gcc.c-torture failset
  byte-identical to the 51-name baseline. Also lands real 16-byte `__int128`
  (P0 wide-integer track) and the measurement-gated embedded-header-forest plan.
  v0.29.0 was the backend-correctness release (MIR-gen `-O2` = `-O1` torture
  parity, fork pinned @ `9ab36fb`).
  `master` still holds the v0.24.0 asmjit/Gecko backend at full C89 coverage;
  develop is **not** promoted to master until the CIR path reaches feature
  parity.
- **Two-tree front-end performance work (local branch, 2026-06-26):** Phase 3
  tsubst consumption and Phase 4 scalar widening are env-gated behind
  `MADC_XTEST_DEP_PARSE`. The local branch now records direct parser-resolved
  TYPE template args on concrete member-template `FuncDef`s (`tsubst_type_args`)
  and CIR consumes those args directly, retiring signature-based binding recovery
  and covering body-only type params. It also records direct TYPE parameter-pack
  elements in `tsubst_type_arg_packs`, and the first CIR fan-out slices now handle
  direct value-pack call arguments such as `sink(args...)` plus direct
  reference-pack call arguments such as `Args&... args` / `sink(args...)` and
  direct expression-pattern packs such as `sink((args + 1)...)` by expanding
  marked list children during `tsubst_cir`. Follow-up pack slices handle the
  simple forwarding-call pattern `sink(std::forward<Args>(args)...)`, re-resolve
  copied callee ids from concrete explicit template args, link covered
  member-template constructors to the same Tree-1 tsubst path, and admit covered
  system-header placement-new bodies including scalar `_Up` construction and
  simple class `_Up` construction with scalar/pointer constructor pack elements
  in allocator-style `new ((void*)p) _Up(std::forward<Args>(args)...)`. The
  current slice also covers direct `__destroy(T*)` helper bodies by deferring
  pointee-class inspection until after substitution, and fixes multi-buffer
  builtin/intrinsic lookup so split system-header/user parses can capture those
  helpers as Tree-1 recipes. Recent slices cover local non-pack nested namespace
  function-template calls such as `sink(nn::ident(v))` by re-resolving copied
  callee ids from substituted argument types, by-value class-object constructor
  packs inside allocator-style placement-new bodies, including singleton and
  multi-element packs, value-returning class-reference constructor-pack shapes,
  and local reference-returning identity-forwarding class-reference constructor
  packs. The 2026-06-25 local keystone extends copied dependent-call handling
  from re-resolution-only to local instantiation-on-miss: the tsubst/copy path
  now instantiates missing nested namespace function templates with
  concrete-typed synthetic params, rebuilds the copied call against the concrete
  callee, and rewrites copied reference-slot arguments back to the concrete pack
  pointer slots. The first non-pack system-header dependent-call slice now admits
  simple scalar/pointer calls, including reserved `__*` helper names, only when
  the substituted args/return are concrete non-class shapes and the resolved
  callee has a materializable body or external symbol; copied calls mark the
  resolved callee reachable so lazy system-header body emission follows the
  ordinary call path. Real system-header reference-forwarding/object-address
  packs now have the first direct placement-new aperture: each expanded nested
  call must resolve through `resolve_copied_dependent_call` and return the
  same/derived class expected by the constructor reference parameter. Broader
  system-header forwarding/destructor/object-address packs still fall back.
  Validation is green both
  flag-off and flag-on at **669 passed / 0 failed / 0 timed out / 18 skipped**;
  `test_cir` is **81 test cases / 1004 assertions / 4 skipped**. Phase 4 is
  roughly **73% implemented** by coverage weight, not session count.
  **2026-06-25:** the generic `is_type_dependent` predicate (the
  `type_dependent_expression_p` / pt.cc:30357 analogue) landed and is COMMITTED
  (`62409d08`, green both gates) — the step-C spine the per-construct
  `tsubst_eligible` catalog retires onto; the trajectory now pivots from growing
  that catalog to generic resolver-reentry. **Keystone update:** the local
  tsubst/copy path now instantiates nested function templates, not just existing
  overloads (g++'s `tsubst → finish_call_expr` model), and the local
  `std::forward`/`std::move` callee-name match is removed. Next work before
  deleting the re-parse fallback is to retire the remaining system-header
  dependent-call bail and catalog entries one construct at a time: real
  system-header forwarding/destructor packs, class-valued placement-new argument
  packs beyond the direct returned-class aperture, broader
  dependent-call packs, and template-id body surfaces.
- **Backend:** `madc parser → cir_node (MC11-IR) → c2mir → MIR` is the **sole**
  backend. The asmjit JIT and the Gecko parser/MIR-transpiler were both removed
  (commits `42e9b6e`, `64f44b3`). There is no `--backend=jit`; `--backend=mir`
  aliases to cir. Builds against the **madc MIR fork**
  (github.com/derekbsnider/mir, branch `develop`, pinned by `MIR_COMMIT`).
- **★ SMAUG 1.8 boots, runs, and is playable** through the CIR path: it boots to
  a live server (`Realms of Despair ready … port 4000`) and a client can create
  a character, navigate the world, and fight (the Newgate serpent fight runs).
  The project's north-star goal — running a real C89 codebase end-to-end —
  is now demonstrated on CIR.
- **CIR baseline (2026-06-11, `feature/template-instantiation-claude` @ `01528ed`):** **572 integration/unit pass / 0 fail / 0 timeout / 18 skip** (`<=>` compliance track COMPLETE incl. = default comparison synthesis ([class.compare.default]) — defaulted member operator<=> yields all six comparisons; ordering-vs-ordering ==/!= work. (`<=>` track functionally complete: token + <compare> types + hidden-friend bodies + REWRITTEN candidates ([over.match.oper] — r!=0, reversed ==, relationals via (x<=>y)@0; the only-<=>/== class idiom gives all six comparisons); remaining polish: = default comparison generation. (`<=>` slice 3a: the token lowering itself works — builtin byte-select into a category temp + class-operand operator<=> dispatch, testspaceship_realhdr 8 g++-verified shapes; remaining: rewritten candidates + = default comparisons, cpp-support.md P2.15. (`<=>` slices 1–2b landed: the C++20 three-way-comparison token is std-gated, the `<compare>` category types live from the REAL header, and hidden-friend operator bodies hoist/compile/dispatch — `r < 0` on `std::strong_ordering` calls the TU-local friend; free-operator dispatch + literal-0 null-pointer-constant scoring + friend-function access grants are general mechanisms, g++-verified by testfreeop/testhiddenfriend/testcompareops_realhdr; remaining: the `<=>` token lowering + rewritten candidates, cpp-support.md P2.15. Template-instantiation batch COMPLETE: 2a fn-template empty-pack elision — std::stof/stod/stold; 2b-i `"pre" + s` exported mangled-direct; 2b-ii `a + "literal"` free-operator **BODY instantiation** + the char_traits explicit-spec instantiation-key fix — `std::char_traits<char>::length` silently folded to 0 before; 2c loud no-ctor-match error — a class declaration whose initializer matches no ctor is now a compile error instead of a silent construction drop, plus the reference-arg ctor-scoring fix it surfaced and the generic `.expect_err` compile-error test convention; 2d reference operands resolve as the referenced class in all operator-resolution surfaces — `cout << s` with a `const string&` parameter and `a + b` on reference params now work; previous 2026-06-11 develop baseline was 557/0/0/18 — eval leftovers B+A0+A: DSL string VALUE compares, MadValue/MadArray → one `madc::value`, mangled-direct `<ns_madc>` + user-call-site scope capture; libmadc in-process eval runs on CirJitSession);
  **gcc.c-torture 1567 of 1652 in-scope (95.0%)** with 26 compile-failed,
  29 runtime-failed, 0 timed out under the post-audit baseline; 33 class-(c)
  tests (gcc-internal / torture-only / UB) formally skipped per
  `docs/parity/failset-classification.md` (user-signed 2026-06-11) via
  `docs/parity/torture-skip-manifest.txt`. The promote gate is **all 41
  class-(a) standard-C failures fixed (≥1608 of 1652 in-scope)**; the old
  "match asmjit 1645" wording is retired (capability sets diverged: asmjit
  passes 82 of CIR's 88 failures; CIR passes 34 of asmjit's 40 failures).
  Class-(b) GNU extensions (14 tests) are roadmap items, not gate blockers.
  The clean `develop` rebuild emits no compiler warnings. The class-(a)
  failures are the active CIR coverage worklist — see Track 1.3.
- **C++17 real-header `std::map` / tuple salvage branch (2026-06-22,
  `wip/map-cxx17-salvage-codex` @ `3534b44` plus local fixes):** the previous
  dirty session is preserved at `failed/2026-06-22-map-cxx17-attempt-codex`
  commit `3534b44`; live work continues on the salvage branch. Map bring-up is
  deliberately C++17-first: `testmap` uses `find/end` rather than C++20
  `contains`, and map canary flags are `--std=c++17 --no-embedded-headers`.
  Focused `testmap` and `teststdmapint` pass. The interrupted handoff
  regressions are recovered: `testforeach2`, `testtuple`, `testfstream`,
  `testloop`, `testmadcevalexpr`, `testmadcevalexprctx`, and
  `testmadcevalexprtyped` are green. C++20 comparison canaries remain green
  under per-test C++20 flags. The local fixes keep body-bearing `void` std
  function templates (`std::_Destroy`, `std::destroy_at`) on the real-header
  body-instantiation path and disambiguate duplicate flattened member C field
  names. The const/reference template-argument spelling and derived-to-base
  nested-template deduction fixes are now enabled by default after external
  method declarations gained typed pointer returns and scalar-reference argument
  addressing. Latest local handoff fixes generic explicit-template overload
  selection for namespace calls such as `std::forward<T>` and const-qualified
  class/struct visibility in CIR emission. The previous `std::get` scoped-alias
  wall is now fixed generically with same-DataDef typedef-alias preservation and
  concrete partial-specialization completion from the opaque template path. The
  later undefined local `basic_string...__o15` wrapper was moved forward by
  generic CIR reference-return/constructor-argument handling. Focused
  `testmap` now passes after a generic anonymous aggregate layout fix:
  `DataDefSTRUCT` records anonymous struct/union groups and CIR emits unnamed
  anonymous aggregate members, so libstdc++ `std::basic_string` keeps its
  `_M_local_buf`/`_M_allocated_capacity` anonymous union instead of flattening
  them as sequential fields. Latest salvage fixes recover C++20 `<compare>`
  constructor binding by preserving scoped enum constant DataDefENUM storage,
  and recover eval render overloads by letting exact object identity bind
  `madc::array`/`madc::value` to `array&`. Fulltest was rerun twice but is noisy
  under the runner's default 5-second per-test cap on this host: one run reported
  **657 passed, 4 failed, 2 timed out, 18 skipped** and another reported
  **650 passed, 3 failed, 10 timed out, 18 skipped** with shifting unrelated
  timeouts; isolated timeout candidates pass sequentially under the default cap.
  Stable functional reds are now `testcontainerdtor`, `testmadc_ns`, `testset`,
  and `testsubscript`. Remaining work: retire the broader map/set/container
  branch reds, fix `tmp/te_direct.mad`'s direct `std::get`
  declaration-initializer call loss, and clean up non-fatal libstdc++
  pointer-type diagnostics in the map path.
- **C++ model — proven on the old backend, being re-established on CIR:**
  ctors/dtors, operator overloading, references, `new`/`delete`, single
  inheritance, vtables, SJLJ exceptions + unwinding, access control, const
  enforcement. These all worked on the asmjit backend; CIR parity is the
  current push.
- **C++ library object model cleanup (merged to `develop`, 2026-06-05):**
  the retire-std-hardcoding gate is at **0 offending lines** on `develop`. The
  intended model is the g++/clang++ one: library classes are declared in
  standard/embedded headers and compiled through the same object, overload,
  mangling, ctor/dtor, and retbuf machinery as user classes. Core compiler and
  runtime code must not special-case concrete C++ library classes or objects
  such as `std::string`, iostream/istream/ostream, sstream, containers, or user
  classes; the exceptions are the mangler, header text, tests, and the
  auto-include trigger map. Auto-includes are now a default `--std=madc`
  convenience only; explicit `--std=c` and `--std=c++` modes require the
  appropriate headers. The cleanup also recovered `std::cin >>` via real
  libstdc++ declarations/operators and advanced generic real-header parsing far
  enough to handle the iostream/istream/ostream class machinery. The remaining
  release-prep work is warning-clean validation and driving the final fulltest
  reds/timeouts to green. Follow-ups stay generic: preserve include-guard/macro
  state before broad real system-header PCH regeneration, finish class/member
  alias resolution for real iostream aliases (`char_type`, `iter_type`,
  `iostate`, `__streambuf_type`, `__ostream_type`, `__ios_type`, and friends),
  and continue closing the real-header `cout`/stream operand walls.
  `std::string` construction (`std::string s("hello")` → `hello len=5`),
  mutation (`s += …`, `s = …`, `a + b`, `s[i]`), `size()`, real `<iostream>`
  output including `cout << unsigned long` / `s.size()`, `std::getline`, and the
  `tmp/fs_out.mad` ofstream write canary now run through real headers. Call-symbol
  derivation is now unified onto a single `CirBuilder::call_emit_symbol` resolver
  (precedence `emit_symbol ?: local_emit_name ?: var_emit_name`) guarded by
  `scripts/check-call-emit-symbol.sh` — no more per-site re-implementations; this
  unblocked binding inline extern-template members (`cout << unsigned long` →
  `_ZNSolsEm`). Remaining real-header walls: `cout << std::string` (the free
  `operator<<` must take the class rhs as a const reference, not a pointer), the
  free-`std::`-fn `emit_symbol` migration (retire the call-site re-mangle + the
  `__ns_` shim gate), and the per-red ingredients for `testfstream` / `testloop` /
  `testdefer`, which remain open.
- **libmadc:** C++ embedding API (security policy, structured diagnostics,
  engine-owned IO). In-process compile/exec/`eval` **runs on CIR→c2mir→MIR**
  via `CirJitSession` (2026-06-10), the script-level `madc::eval_*` surface is
  declaration-only mangled-direct through `<ns_madc>`, the script `array` IS
  the public `madc::value` (A0 unification, 2026-06-11), and runtime-eval
  scope capture fires at the user call site (int/real/array/string locals,
  per-family engine gates). Remaining: package C — `register_function`,
  `get/set_global`, string call marshalling, fork/limits, the policy tail
  (the 38 `test_libmadc_program` skips; see
  `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`).
- **AOT (native object/executable):** deferred, low priority. Near-term native
  builds come from emit-`.c` + an external compiler; `save_object` /
  `save_executable` are stubbed (signatures kept) for a later MIR-based revisit.
- **Legacy reference (asmjit backend, pre-removal):** GCC-torture parity reached
  ~97.9% and ~475 integration tests passed. Retained only as the parity target
  the CIR path is climbing back to — NOT the current state.

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** (97.9% on the old backend; the CIR target) | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | **DONE** (v0.20.1) | [code-cleanup.md](code-cleanup.md) |
| 1.3 | **CIR coverage — drive `cir_node` (MC11-IR) → c2mir → MIR to the re-defined promote gate** | ongoing | **Active — the parity-to-master gate, RE-DEFINED 2026-06-11** (user-signed failset audit, [failset-classification.md](../parity/failset-classification.md)): gate = all 41 class-(a) standard-C failures fixed (≥1608 of 1652 in-scope; currently 1567), 33 class-(c) gcc-only/torture-only tests formally skipped, 14 class-(b) real-world GNU extensions as roadmap items. "Match asmjit 1645" is retired (capability sets diverged: CIR passes 34 tests asmjit fails). Class-(a) collapses to ~12 root causes, headlined by K&R old-style definition parsing (23 tests, std-gated < C23, never C++) and implicit-decl forward-call binding (5 tests). Integration baseline 557/0/0/18 (2026-06-11). | — |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.6 | **SIMD — add a minimal generic-vector extension to MIR (types + insns + per-target codegen) and a c2mir `vector_size` front-end** | large | **In progress (raise the floor)** — MIR branch `feature/simd-vector-support-codex` at `2ffebff` now has a partial MIR `v128` floor plus c2mir `vector_size` support, expression-valued `vector_size` arguments, leading GNU vector attributes in declarations and compound-literal type names, C2MIR `__builtin_abort`, `__builtin_memcmp`, `__builtin_copysignf`, and `__builtin_nan` lowering to libc/libm calls, all 37 GCC c-torture execute vector-construct files checked in and passing under C2MIR `-ei`/`-eg`, including exact `pr92618`, `pr94524-{1,2}`, `pr53645`, `pr53645-2`, `pr109040`, `simd-{1,2,4,5,6}`, `pr65427`, `pr60960`, `pr70903`, `pr85169`, `pr108292`, `scal-to-vec{1,2,3}`, `20050316-{1,2,3}`, `pr105613`, `pr72824-2`, and `fp-cmp-cond-1` runtime coverage plus empty GNU asm barrier parsing, narrow address-taken register rvalue extension, union-alias preservation through array subscripts, and one-lane unsigned `__int128` vector equality broadened to one-lane `__int128` vector arithmetic, bitwise, unary, comparison, shift, compound, inc/dec, and div/mod helper-call lowering, Clang `ext_vector_type` support including non-power-of-two logical lane counts, same-element-count `__builtin_convertvector` across supported vector widths, non-`v128` integer vector operation lowering through scalar lanes, non-`v128` same-size vector casts through memory-backed block copies, same-size integer scalar/vector reinterpret bitcasts, GNU declaration-spec vector attributes, mixed-signedness vector shift-count type compatibility, mixed-source-width `__builtin_shufflevector` support, packed `v128` f32/f64 arithmetic/comparison opcodes, packed `v128` i8/i16/i32 add/sub and comparison opcodes, packed `v128` i64 add/sub opcodes, packed `v128` i8/i16/i32 multiply plus i8/i16/i32 and i64 scalar-count and lane-count shifts, qword vector comparison scalar-fallback masks, packed `v128` i64 equality/order, scalar-condition vector conditionals, GCC vector inc/dec lowering, and x86-64 `v128`/`v64`/`v32`/`v16`/`v8` integer-vector ABI support, plus `MIR_T_V128` text/binary data I/O support, and one-lane `__int128` vector div/mod helper-call lowering; no known <=16-byte SIMD gap remains, but Track 1.6 is still partial; design for **upstream** to vnmakarov/mir | — |

**Track 1.6 (SIMD) raises the *floor*, not just c2mir.** MIR today has no vector
type/insns (locals are `i64/f/d/ld` only), so real SIMD-in-JIT requires adding
vectors to MIR itself + per-target codegen (x86-64 SSE/AVX, aarch64 NEON, …) +
interpreter support + ABI/serialization, plus a c2mir front-end for GNU
`vector_size` / generic vector ops. **Design it for upstream** — it benefits MIR
directly (WASM→MIR, a stated MIR future goal, *requires* SIMD since WASM has
fixed-width SIMD; every MIR target has a vector ISA; it lifts ~11 deferred SIMD
torture tests). Keep it a **minimal generic-vector core** to fit MIR's
lightweight ethos. Interim until it lands: madc **scalarizes** for the JIT and
**emit-C → gcc/clang** for real SIMD (AOT). Feeds Track 6.2 (macOS NEON). See
the lowering-vs-raising rule (`.claude/rules/`) and ADR 0001.

2026-06-05/06 checkpoints: `/workspace/mir` branch
`feature/simd-vector-support-codex` is at `2ffebff`, not yet pinned by madc's
`MIR_COMMIT`. `6257780` adds the first c2mir front-end slice with distinct
memory-backed GNU `vector_size` types, brace initialization, scalar
indexing/lvalue writes, block copy, and memory-shaped param/return plumbing.
`2194f8c` adds MIR `v128`, `vmov`, `vaddi32`/`vsubi32`, vector bitwise ops,
signed `v4i32` comparisons, interpreter/x86-64 codegen, `mir-tests/test17.mir`,
and C2MIR lowering for signed `v4i32` arithmetic/bitwise/unary/scalar
splats/comparisons. `eceffd0` adds unsigned `v4u32` equality/inequality.
`516db72` adds same-size `v128` vector-to-vector casts. `3a7bbbd` adds
unsigned `v4u32` ordering comparisons by biasing operands and reusing the
signed compare. `240f838` adds `v4i32`/`v4u32` vector shifts via lane-wise
scalar MIR lowering for vector/scalar counts and compound shifts.
`99c19a6` adds `v4i32`/`v4u32` vector multiply, divide, and modulo via
lane-wise scalar MIR lowering, including compound assignment coverage.
`52940dd` adds C2MIR builtin recognition and lane-wise lowering for
`__builtin_convertvector` and `__builtin_shufflevector` on `v128` vectors with
4- and 8-byte arithmetic lanes. `0305f1d` adds C2MIR builtin recognition and
lane-wise lowering for GCC `__builtin_shuffle` on same-type `v128` sources
with signed/unsigned runtime mask modulo handling. `2982f38` extends C2MIR
lowering to `v128` integer vectors with 1-, 2-, 4-, and 8-byte lanes, covering
signed/unsigned small-lane arithmetic, bitwise, unary, comparisons, shifts,
compound assignment, same-type shuffles, `uint16x8_t`, and Clang-style
`__builtin_vectorelements`. `40661db` adds C2MIR support for width-changing
`__builtin_shufflevector` results from `v128` sources. `62f8f31` adds
`__builtin_shufflevector` support for non-`v128` source vector widths through
memory-backed scalar lane copies. `84377c2` adds generic non-`v128` GCC
`__builtin_shuffle` support for same-type vectors through memory-backed scalar
lane copies and runtime mask modulo handling. `665dbbb` adds C2MIR `v128`
floating-lane arithmetic and comparisons through memory-backed scalar lane
lowering, covering `v4sf` add/mul/unary/scalar splats/compound ops and `v2df`
division/comparison masks. `1deb2be` extends that floating-lane lowering to
non-`v128` float/double vectors, covering `v2sf`, `v8sf`, and `v4df`
arithmetic/scalar splats/unary/comparison masks. `56e331b` adds x86-64 `v128`
vector parameter/return ABI support across C2MIR prototypes, MIR generated
calls/prologues/returns, interpreter FFI, and interpreter shims. `6de64b4`
adds x86-64 `v64` vector parameter/return ABI support by classifying top-level
8-byte vectors as one SysV SSE eightbyte through the existing `blk2:8` /
`MIR_T_D` path, with `v2si`, `v2sf`, and `v4hi` coverage. `8a16ed6` adds
x86-64 `v32` integer-vector parameter/return ABI support by classifying
top-level 4-byte integer-element vectors as one SysV integer eightbyte through
the existing `blk1:4` path; coverage includes `v1si`, `v2hi`, `v4qi`, and the
GCC-memory-ABI `v1sf` control. `fe49fde` adds MIR packed `v128` f32 arithmetic
opcodes (`vaddf32`, `vsubf32`, `vmulf32`, `vdivf32`) with interpreter support
and x86-64 SSE `addps` / `subps` / `mulps` / `divps` codegen; C2MIR selects
them for `vector_size(16)` float arithmetic while preserving scalar-lane
fallback for other floating vector widths and double lanes.
`ffa02b6` adds MIR packed `v128` f32 comparison opcodes (`veqf32`, `vnef32`,
`vltf32`, `vlef32`) with interpreter support and x86-64 SSE `cmpps` codegen;
C2MIR selects them for `vector_size(16)` float comparisons and lowers `>` /
`>=` by swapping operands into ordered `<` / `<=`. `52a75bb` adds MIR packed
`v128` f64 arithmetic opcodes (`vaddf64`, `vsubf64`, `vmulf64`, `vdivf64`) and
comparison opcodes (`veqf64`, `vnef64`, `vltf64`, `vlef64`) with interpreter
support and x86-64 SSE2 `addpd` / `subpd` / `mulpd` / `divpd` / `cmppd`
codegen; C2MIR selects them for `vector_size(16)` double
arithmetic/comparisons while preserving scalar-lane fallback for non-`v128`
double vectors.
`798f18d` adds MIR packed `v128` i8/i16 add/sub opcodes (`vaddi8`, `vaddi16`,
`vsubi8`, `vsubi16`) with interpreter support and x86-64 SSE2 `paddb` /
`paddw` / `psubb` / `psubw` codegen; C2MIR selects them for
`vector_size(16)` byte/word add/sub while preserving scalar-lane fallback for
8-byte integer lanes.
`3f287d0` adds MIR packed `v128` i8/i16 comparison opcodes (`veqi8`, `veqi16`,
`vgti8`, `vgti16`) with interpreter support and x86-64 SSE2 `pcmpeqb` /
`pcmpeqw` / `pcmpgtb` / `pcmpgtw` codegen; C2MIR selects them for
`vector_size(16)` byte/word equality and ordering comparisons, including
unsigned ordering through lane sign-bit biasing, while preserving scalar-lane
fallback for 8-byte integer lanes.
`730b50d` adds MIR packed `v128` i16 multiply opcode (`vmuli16`) with
interpreter support and x86-64 SSE2 `pmullw` codegen; C2MIR selects it for
`vector_size(16)` signed/unsigned short multiplication and compound
multiplication while preserving scalar-lane fallback for byte, dword, and qword
integer multiply/div/mod.
`9fb836d` adds MIR packed `v128` i16 scalar-count shift opcodes (`vlshi16`,
`vrshi16`, `vurshi16`) with interpreter support and x86-64 SSE2 `psllw` /
`psraw` / `psrlw` codegen; C2MIR selects them for `vector_size(16)`
signed/unsigned short scalar-count shifts and compound shifts while preserving
scalar-lane fallback for vector-count shifts and other lane widths.
`2ec7b5d` adds MIR packed `v128` i32 scalar-count shift opcodes (`vlshi32`,
`vrshi32`, `vurshi32`) with interpreter support and x86-64 SSE2 `pslld` /
`psrad` / `psrld` codegen; C2MIR selects them for `vector_size(16)`
signed/unsigned int scalar-count shifts and compound shifts through the
generalized 16-/32-bit integer-lane shift path while preserving scalar-lane
fallback for vector-count shifts and unsupported lane widths.
`4dcf378` adds C2MIR support for scalar-condition vector conditional
expressions with matching vector true/false arms. Vector conditional results
now lower through the memory-shaped aggregate path, covering assignment and
vector-conditional rvalue indexing. GCC and clang C reject vector-condition
ternary/logical forms; GCC also rejects scalar/vector mixed ternary arms, so
this checkpoint follows the GCC-compatible matching-vector-arm subset.
`b84da0d` generalizes C2MIR `__builtin_convertvector` beyond the earlier
`v128`-only gate. Same-element-count conversions across supported arithmetic
vector lane widths now lower through generic memory-backed vector lanes,
covering `v64` conversions and `v128`-to-`v256` same-element-count widening.
`3146f66` widens C2MIR integer vector operation lowering beyond the `v128`
gate. Arithmetic, bitwise, shifts, unary operations, and comparisons for
supported non-`v128` integer vector widths now lower through scalar lanes, with
`v32`, `v64`, and `v256` fixture coverage.
`09b79af` generalizes C2MIR same-size vector reinterpret casts beyond the
earlier `v128`-only gate. Non-`v128` vector casts now lower through
memory-backed block copies while `v128` keeps the register move path, with
`v64` and `v256` bitcast fixture coverage.
`9f6132e` adds C2MIR same-size integer scalar/vector reinterpret bitcasts.
Integer-to-vector casts write the integer representation into vector storage,
vector-to-integer casts load from materialized vector storage, same-size
float/pointer scalar casts remain rejected to match GCC/clang, and
`c-tests/new/vector-size.c` now covers `v32` and `v64` scalar integer/vector
bitcasts in both directions.
`3b25f0a` extends C2MIR `__builtin_shufflevector` to same-element-type sources
with different vector widths. It validates source indexes against the combined
lane count and copies lanes from materialized sources using independent source
element counts. GCC accepts this mixed-source-width form; Clang rejects it, so
the checkpoint is recorded as GCC extension coverage.
`9de5b22` adds MIR packed `v128` i32 multiply opcode (`vmuli32`) with
interpreter support and x86-64 SSE4.1 `pmulld` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned int multiplication and compound
multiplication while preserving scalar-lane fallback for other integer lane
widths and div/mod.
`3bdf0e4` adds MIR packed `v128` i64 add/sub opcodes (`vaddi64`, `vsubi64`)
with interpreter support and x86-64 SSE2 `paddq` / `psubq` codegen. C2MIR
selects them for `vector_size(16)` signed/unsigned long-long add/sub and
compound add/sub while preserving scalar-lane fallback for other unsupported
64-bit integer operations.
`c96ac07` adds MIR packed `v128` i64 scalar-count shift opcodes (`vlshi64`,
`vurshi64`) with interpreter support and x86-64 SSE2 `psllq` / `psrlq`
codegen. C2MIR selects them for `vector_size(16)` signed/unsigned long-long
left shifts and unsigned long-long right shifts while signed long-long right
shifts remain on scalar-lane fallback because SSE2 has no arithmetic qword
right-shift instruction.
`3f33ff6` fixes the C2MIR scalar-lane fallback for qword vector comparisons:
8-byte comparison lanes now form all-ones masks in 64-bit temporaries with
64-bit subtract-from-zero, so `v2i64` / `v2u64` equality, inequality, and
ordering comparisons produce full-lane masks in generated mode.
`92accb7` adds MIR packed `v128` i64 equality opcode (`veqi64`) with
interpreter support and x86-64 SSE4.1 `pcmpeqq` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned long-long equality and inequality while qword
ordering comparisons stay on the scalar-lane fallback.
`7c169e7` adds MIR packed `v128` i64 ordering opcode (`vgti64`) with
interpreter support and x86-64 SSE4.2 `pcmpgtq` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned long-long ordering comparisons, including
unsigned ordering through lane sign-bit XOR biasing.
`dad14bc` synthesizes packed signed `v128` i64 scalar-count right shifts in
C2MIR with the existing `vurshi64`, `vxor`, and `vsubi64` floor. The permanent
`vector-size.c` fixture now covers the count-zero edge for `v2i64 >> 0`.
`2ed2c4a` extends x86-64 ABI classification for top-level v8/v16 integer
vectors, passing `vector_size(1)` / `vector_size(2)` integer vectors through
the existing integer-register `blk1` path instead of `blk0` / `rblk` memory
ABI; `vector-size.c` now covers `v1qi`, `v2qi` / `v2uqi`, and `v1hi`
parameter/return cases.
`e4e096b` synthesizes packed `v128` i8 scalar-count shifts in C2MIR with the
existing word-shift, mask, XOR, and byte-subtract vector floor; `vector-size.c`
now covers signed and unsigned `v16qi` / `v16uqi` left/right scalar-count
shifts plus compound signed right shift.
`29775cd` synthesizes packed `v128` i8 multiplication in C2MIR with the
existing word multiply, byte mask, word shift, and OR vector floor;
`vector-size.c` now covers signed and unsigned `v16qi` / `v16uqi` multiply and
compound multiply.
`0bf8e7c` adds GCC vector prefix/postfix inc/dec support for integer and
floating vector types in C2MIR. Prefix/postfix lowering reuses the existing
integer/float vector arithmetic paths with a splatted one, and postfix old
values are preserved when vector block-move assignments provide the result
destination. `vector-size.c` now covers `v4si`, `v16uqi`, `v4sf`, `v8si`, and
`v8sf` inc/dec cases.
`3a63473` adds C2MIR recognition for Clang `ext_vector_type` and
`__ext_vector_type__` attributes for power-of-two element counts. C2MIR maps
the element count to the existing byte-size vector representation, preserves
qualifiers, and reuses the current vector lowering paths. `vector-size.c` now
covers `clang_i4`, `clang_uh8`, and `clang_f4` extended-vector cases.
`5966c1d` splits C2MIR vector storage size from logical element count, allowing
Clang non-power-of-two extended vectors such as `ext_vector_type(3)` to keep
their logical lane count while matching Clang's rounded storage/alignment. The
same logical lane count feeds `__builtin_vectorelements`,
`__builtin_convertvector`, and `__builtin_shufflevector` result construction.
`vector-size.c` now covers `clang_i3` and `clang_uc3` size/alignment, logical
element count, and lane arithmetic.
`3d9b8af` adds GCC/clang parity for mixed-signedness vector shift-count
operands. C2MIR now accepts vector shift counts whose storage size, logical
lane count, and lane width match the shifted vector even when signedness
differs, while preserving lane-wise scalar lowering for non-uniform vector
counts. `vector-size.c` now covers `v4si` by `v4ui`, `v4ui` by `v4si`,
`v8hi` by `uint16x8_t`, and compound mixed-signedness vector-count left shift
cases.
`fc493fd` preserves GNU declaration-spec attributes and applies `vector_size`
and `ext_vector_type` after base type resolution. C2MIR now accepts GCC/clang
spellings such as `typedef signed char __attribute__((__vector_size__(16))) V;`
instead of dropping the attribute before the typedef declarator. `vector-size.c`
now covers that typedef spelling with signed-byte scalar compound modulo in the
pr94524-style shape.
`07dd396` parses attribute arguments as constant expressions and evaluates
`vector_size` / `ext_vector_type` arguments through the existing
constant-expression checker. C2MIR now accepts GCC/clang spellings such as
`__attribute__((__vector_size__(2 * sizeof (long long)), __may_alias__))`, and
`vector-size.c` now covers that spelling plus pr92618-style casted
vector-pointer stores that write all 16 bytes.
`fbb47f3` lowers C2MIR `__builtin_abort` to a void zero-argument call to libc
`abort`, matching GCC/clang lowering and avoiding the previous unresolved
`__builtin_abort` import. `c-tests/new/builtin-abort.c` covers the generic
builtin, and exact GCC torture cases `c-tests/gcc/pr92618.c`,
`pr94524-1.c`, and `pr94524-2.c` now pass under C2MIR `-ei` and `-eg`.
`48cd7be` accepts GNU empty asm statement barriers. C2MIR now parses
`asm` / `__asm` / `__asm__` statement syntax with qualifiers, operands, and
clobbers, rejects non-empty templates/output/goto asm, evaluates input
operands, and emits no MIR instruction for empty templates. Exact GCC torture
cases `c-tests/gcc/pr53645.c` and `pr53645-2.c`, plus focused
`c-tests/new/empty-asm.c`, now pass under C2MIR `-ei` and `-eg`.
`ff01f80` extends C2MIR `force_val` handling for narrow address-taken
register-backed scalar lvalues; `char` and `short` rvalues are now sign- or
zero-extended after byte/word pointer writes, fixing exact GCC `pr109040.c`.
Coverage adds `c-tests/gcc/pr109040.c` and focused
`c-tests/new/narrow-reg-address.c`.
`fbe5efb` lowers C2MIR `__builtin_memcmp` to an imported libc `memcmp` call
returning `int`, validates pointer/pointer/integer argument types, and avoids
the previous unresolved `__builtin_memcmp` symbol. Coverage adds focused
`c-tests/new/builtin-memcmp.c` plus exact GCC SIMD cases `c-tests/gcc/simd-5.c`,
`pr65427.c`, and `pr60960.c`.
`033732f` preserves leading GNU attributes in declaration specifiers and
type-name specifier/qualifier lists, covering macro-expanded vector forms in
ordinary declarations and compound literal type names. Coverage adds exact GCC
SIMD cases `c-tests/gcc/scal-to-vec1.c`, `scal-to-vec2.c`, and
`scal-to-vec3.c`.
`95e52f9` preserves union aliases through array subscripts so MIR O2 DSE keeps
union-width stores that are later read through array members. Coverage adds
exact GCC SIMD `c-tests/gcc/20050316-2.c`, which passes C2MIR `-ei` and `-eg`.
`626f75e` adds C2MIR `__int128` / `unsigned __int128` spelling and narrow
memory-shaped scalar handling, then lowers one-lane unsigned `__int128` vector
equality/inequality by comparing and storing the low/high 64-bit halves.
Coverage adds exact GCC `c-tests/gcc/pr105613.c`, which passes C2MIR `-ei` and
`-eg`.
`59117d8` lowers C2MIR `__builtin_copysignf` and `__builtin_nan` to checked
libm imports, clearing the IEEE vector-search blockers from exact GCC
`c-tests/gcc/pr72824-2.c` and `c-tests/gcc/fp-cmp-cond-1.c` under C2MIR `-ei`
and `-eg`; coverage also adds focused `c-tests/new/builtin-fp.c`.
`c69f4da` imports the remaining 21 exact GCC c-torture vector fixtures found by
the vector-construct scan, so all 37 GCC execute tests that mention vector
constructs are now checked in under `c-tests/gcc` and pass C2MIR `-ei` / `-eg`.
`55c65ee` adds MIR text and binary I/O round-trip support for `MIR_T_V128`
data items by serializing each vector element as 16 byte values. Focused
coverage now exercises textual scan/output in `mir-tests/scan-test.c` and
binary write/read in `mir-tests/io.c`.
`e4a8945` adds MIR `v128` lane-count shift opcodes for i8/i16/i32/i64 lanes:
`vlshvi*`, `vrshvi*`, and `vurshvi*`. C2MIR selects these opcodes for matching
vector-count operands, the interpreter executes them directly, and x86-64
generated mode lowers them through scalar lane loads/shifts/stores. Coverage
adds direct MIR scan/execute checks in `c-tests/mir/vector-shift-count.mir` and
C frontend checks in `c-tests/new/vector-shift-count.c`.
`360fdb5` adds C2MIR one-lane `__int128` and `unsigned __int128` vector
lowering for add/sub/mul, bitwise ops, unary ops, equality/ordering
comparisons, scalar-count and vector-count shifts, compound assignment, and
GCC vector inc/dec through low/high 64-bit halves. Coverage extends
`c-tests/new/vector-size.c`.
`2ffebff` adds C2MIR one-lane signed and unsigned `__int128` vector division
and modulo through `__divti3`, `__udivti3`, `__modti3`, and `__umodti3`
helper-call imports. C2MIR and the MIR binary runners now resolve those helpers
for saved MIR/BMIR execution, and `c-tests/new/vector-size.c` covers exact
small results plus high-half identity checks.
`/workspace/mir` `timeout 900 make test` passed at `2ffebff` with
interpreter/O0 `Tests 1121, Success tests 2242` and generated-mode
`Tests 1125, Success tests 2250`, focused `make scan-test` and `make io-test` passed
for the new `v128` data I/O coverage, the new checked-in exact vector copies
passed GCC native plus C2MIR `-ei` / `-eg` at `c69f4da`, and focused
builtin-fp and one-lane unsigned `__int128`
vector reducers passed GCC/clang assembly and native validation plus C2MIR
`-ei` / `-eg`,
focused
union-array alias reducers and adjusted array parameter probes passed C2MIR
validation, focused prefix vector-attribute cases passed
GCC/clang assembly and native validation plus C2MIR `-ei` / `-eg`, exact
`scal-to-vec1.c`,
`scal-to-vec2.c`, and `scal-to-vec3.c` passed C2MIR `-ei` / `-eg`, focused
`__builtin_memcmp` reducers passed GCC/clang native and
assembly validation plus C2MIR `-ei` / `-eg`, exact `simd-5.c`, `pr65427.c`,
and `pr60960.c` passed GCC/clang native validation plus C2MIR `-ei` / `-eg`,
generated MIR showed `import memcmp`, `memcmp_p`, and `call memcmp_p` calls,
focused empty-asm barrier reducers passed GCC/clang native
validation and C2MIR `-ei` / `-eg`, generated MIR for the focused fixture has
the input-operand call with no asm marker, exact GCC `pr109040.c` and focused
narrow-register reducers passed GCC/clang assembly/native validation plus
C2MIR `-ei` / `-eg`, focused `interp-test17` and
`gen-test17` passed, generated MIR showed `vmuli32`, focused v4i32 multiply
reducers passed GCC/clang assembly/native validation and C2MIR interp/gen
validation, and GCC/clang `-msse4.1` assembly showed `pmulld`. Focused v2i64
add/sub reducers passed GCC/clang assembly/native validation and C2MIR
interp/gen validation; GCC/clang assembly showed `paddq` / `psubq`, and
generated MIR showed `vaddi64` / `vsubi64` in both the focused reducer and
`c-tests/new/vector-size.c`. Focused v2i64 scalar-shift reducers passed
GCC/clang assembly/native validation and C2MIR interp/gen validation;
GCC/clang assembly showed `psllq` / `psrlq`, generated MIR showed `vlshi64` /
`vurshi64` for left and unsigned-right shifts. Focused signed v2i64
scalar-right-shift reducers passed GCC/clang `-msse4.2` assembly/native
validation and C2MIR interp/gen validation; GCC used a `pcmpgtq` / `psrlq` /
`psllq` / `por` sign-fill sequence, clang used `psrlq` / `pxor` / `psubq`, and
generated MIR showed the C2MIR `vurshi64` / `vxor` / `vsubi64` synthesis
including `v2i64 >> 0`.
Focused v2i64/v2u64 comparison reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR now uses 64-bit
`sub` mask formation for qword comparison lanes instead of 32-bit `subs`.
Focused v2i64/v2u64 equality reducers passed GCC/clang `-msse4.1`
assembly/native validation and C2MIR interp/gen validation; GCC/clang assembly
showed `pcmpeqq`, generated MIR showed `veqi64` for equality plus `vxor` for
inequality. Focused v2i64/v2u64 ordering reducers passed GCC/clang `-msse4.2`
assembly/native validation and C2MIR interp/gen validation; GCC/clang assembly
showed `pcmpgtq`, generated MIR showed `vgti64` for signed ordering and `vxor`
bias plus `vgti64` for unsigned ordering.
Focused v8/v16 integer-vector ABI reducers passed GCC/clang assembly/native
validation, C2MIR interp/gen validation against GCC-built and clang-built
shared libraries, and generated MIR now uses `blk1:1` / `blk1:2` arguments and
integer return registers instead of `blk0` / `rblk` memory ABI.
Focused v16qi/v16uqi scalar-shift reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR showed `vlshi16`,
`vurshi16`, and `vsubi8` selection for byte-shift synthesis.
Focused v16qi/v16uqi multiply reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR showed `vand`,
`vurshi16`, `vmuli16`, `vlshi16`, and `vor` selection for byte-multiply
synthesis.
Focused vector inc/dec reducers passed GCC native validation and C2MIR
interp/gen validation for integer and float vectors. Clang rejects vector
inc/dec forms, so this checkpoint is recorded as GCC extension coverage.
Focused Clang `ext_vector_type` reducers passed Clang native/assembly
validation and C2MIR interp/gen validation. GCC ignores this Clang-only
attribute as expected. C2MIR interp/gen validation also passed for the full
`vector-size.c` fixture after adding the extended-vector cases. Focused Clang
odd-lane reducers for `ext_vector_type(3)` passed native/assembly validation
and C2MIR interp/gen validation. An odd-lane `__builtin_shufflevector` /
`__builtin_convertvector` reducer also passed Clang native/assembly and C2MIR
interp/gen validation. The full MIR `timeout 900 make test` passed after the
logical-lane change.
Focused mixed-signedness vector shift-count reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. Generated MIR for
the reducer showed lane-wise scalar `lshs` / `urshs` operations for the
non-uniform vector-count cases, not the low-64-bit scalar-count packed shift
opcodes. The full `vector-size.c` fixture passed GCC native validation and
C2MIR interp/gen validation with the mixed-signedness vector-count cases.
Focused declaration-spec vector-attribute reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. The exact GCC
`pr94524-1.c` and `pr94524-2.c` torture sources now pass exact runtime
validation after C2MIR lowers `__builtin_abort` to libc `abort`. The full
`vector-size.c` fixture passed GCC native validation and C2MIR interp/gen
validation with the declaration-spec vector-attribute case. The full MIR
`timeout 900 make test` passed after the memcmp checkpoint with `Tests 1090,
Success tests 2180` plus bootstrap checks.
Focused expression-valued `vector_size` reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. The exact GCC
`pr92618.c` torture source now passes exact runtime validation after
`__builtin_abort` lowering. GCC and clang assembly showed full 128-bit vector
stores for the casted vector-pointer store shape, and the full `vector-size.c`
fixture passed GCC native validation and C2MIR interp/gen validation with the
constant-expression attribute case. The full MIR `timeout 900 make test`
passed after the memcmp checkpoint with `Tests 1090, Success tests 2180` plus
bootstrap checks.
`/workspace/madc` fulltest hit the known failing set. The aggregate harness
reported 486 passed / 4 failed / 1 timed out / 55 skipped with
`testfortypedcomma` classified as `TIMEOUT` in this run. GCC/clang
packed-small-integer multiply and scalar-shift reducers matched the packed
word/dword shapes, and GCC/clang scalar-condition vector ternary reducers plus
C2MIR interp/gen reducers passed. GCC/clang
same-element-count convertvector reducers and C2MIR interp/gen reducers also
passed; focused GCC/clang non-`v128` integer-vector reducers and C2MIR
interp/gen reducers passed; focused GCC/clang non-`v128` vector-cast reducers
and C2MIR interp/gen reducers passed, with MIR dumps showing scalar lane
conversion, integer-operation lowering, and non-`v128` cast block copies;
focused GCC/clang scalar integer/vector bitcast reducers and C2MIR interp/gen
reducers passed, negative same-size float/pointer scalar-vector controls
rejected, and MIR dumps showed direct integer scalar/vector reinterpret stores
and loads; focused GCC mixed-source-width shufflevector reducers and C2MIR
interp/gen reducers passed, and the Clang rejection control still rejects that
mixed-source form.
Focused lane-count shift validation passed native GCC, C2MIR `-ei`, C2MIR
`-eg`, direct MIR `-ei`, direct MIR `-eg`, `make scan-test`, and
`make io-test`; generated MIR from the C fixture showed all twelve
`vlshvi*` / `vrshvi*` / `vurshvi*` opcodes.
Focused one-lane `__int128` vector reducers passed GCC/clang native validation
where the frontend accepts the operators, C2MIR `-ei`, and C2MIR `-eg`; the
full `vector-size.c` fixture passed GCC native validation plus C2MIR
interp/gen validation with the full one-lane `__int128` vector operator set.
Saved MIR `-ei` / `-eg` and saved BMIR interp/gen validation also passed for
the helper-call div/mod imports.
`git diff --check` is clean. Vector-condition ternary/logical semantics remain outside current
C2MIR C coverage because GCC and clang C reject those forms.
No known <=16-byte SIMD gap remains after the one-lane `__int128` vector
div/mod helper-call checkpoint. Later gaps include 32-byte-and-larger vector ABI support
beyond the covered stack-passed `pr109040` case requiring the broader AVX/YMM
or generic-vector MIR floor, broader MIR vector opcodes, registers,
interpreter support, codegen, and further optional per-target packed lowering.

**Track 1.3 is the central workstream.** It is the sole backend, so its
coverage *is* the bar for promoting `develop → master`. SMAUG 1.8 now boots,
runs, and is playable on this path (a real-world end-to-end proof); the ~95
remaining integration failures are the worklist between here and parity. Build
the `.mc11`/`.c` renderer + the gcc-`-fverbose-asm` fidelity gate + the
`cir_node`-vs-`c2m -d` differential to make those failures mechanical and
localizable, then reimplement eval/exec + REPL on MIR.

**Dependencies:** 1.2 before 1.3. **1.3 (full CIR parity) gates promotion to
master and unblocks Tracks 3, 5, 6, and AOT.**

---

## Track 2: C++ Support

*Extend from C scripting convenience to practical C++ OOP.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 2.1 | Constructors & destructors (RAII foundation) | 3-5 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.2 | Operator overloading completion | 2-3 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.3 | References `T&`, const enforcement | 1 wk | **Mostly done** | [cpp-support.md](cpp-support.md) |
| 2.4 | `new` / `delete` | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.5 | Single inheritance | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.6 | Virtual functions / vtables | 2-3 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.7 | Exception handling (SJLJ) | 3-4 wk | **Mostly done** (v0.21.0) — Phase A + B (scalar throw/catch + RAII unwind); **object/class-typed catch OPEN** | [cpp-support.md](cpp-support.md) |
| 2.8 | Quality of life | Ongoing | **Started** — access control, auto token position | [cpp-support.md](cpp-support.md) |
| 2.9 | Generic extern class ctor/dtor | — | **DONE** (v0.21.0) — replaces per-type switch boilerplate | — |
| 2.10 | **Single-name local instantiations (flattened→Itanium-mangled)** | 1-2 wk | **Planned** | — |

**2.3 remaining:** pointer-to-const enforcement (`*p` writes), const methods.
**2.7 remaining:** exceptions are SCALAR-ONLY (int/double/cstr/`catch(...)`).
Throwing/catching user-class or `std::` exception objects, and inheritance-aware
`catch (const Base &)` of a derived throw, are unsupported — the SJLJ runtime
carries no thrown object + catch dispatch is an integer-tag chain, not an RTTI
type match. Tracked as **P1.1e** in [cpp-support.md](cpp-support.md).
**2.8 remaining:** enum class, auto type deduction, broader real-iostream
output replacement, scope-level destruction.

**2.10 — name every madc-local template instantiation by its Itanium mangled
name, retiring the flattened-key scheme.** Today madc carries TWO naming schemes
for the same entity: libstdc++-exported symbols are referenced mangled-direct
(`_ZNSt6vectorIiSaIiEE…`, via `madc_mangle`), while madc-monomorphized local
bodies (class-template instances like `vector<int>` + nested types, free-fn-
template instances `__ns_std__Destroy`/`__addressof`, member-template instances)
get flattened keys (`vector_int32_t_std__allocator_int32_t_…`). Carrying two
names for one entity is a standing source of confusion and drift (the mangler
should be the single name source). Unifying on the mangled name everywhere
(symbol table, emitted C, call sites, struct tags) gives: (a) `--emit=c11`
diffability against g++; (b) **free linker dedup** — a local instantiation whose
mangled name coincides with a libstdc++ weak export ODR-merges automatically, so
the "exported vs inline-only" decision disappears (always mangle; emit a body
only when nothing else defines it). **Cost/risk:** mangler completeness —
correct Itanium for nested types, member templates, and substitution compression
(`S_`/`S0_`); a wrong name becomes a link error or a silent wrong-symbol bind, so
migrate one category at a time behind the full gate. The member-template
convergence (Phase 2.10's first consumer — emit a local body under the mangled
name `itanium_mangle_member_template_sub` already computes when the owner is
local/not-exported) establishes the pattern.

**Dependencies:** All met. 2.1-2.7 complete.

---

## Track 3: Build Infrastructure

*Pre-compiled headers, modules, and portable builds.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 3.1 | PCH Phase 1 — post-lexer token serialization | 2 wk | **Partial** | [precompiled-headers.md](precompiled-headers.md) |
| 3.2 | PCH transition — replace text-embedded stubs | 2-3 wk | Blocked on parser | [precompiled-headers.md](precompiled-headers.md) |
| 3.3 | PCH Phase 2 — AST serialization | 4-6 wk | Future | [precompiled-headers.md](precompiled-headers.md) |
| 3.4 | PCH Phase 3 — C++20-style modules (.madm) | 6-8 wk | Future | [precompiled-headers.md](precompiled-headers.md) |
| 3.5 | Project build driver v1 (`--project`) | — | **DONE** | [madc-project-build-driver](../superpowers/plans/2026-06-08-madc-project-build-driver.md) |

**Dependencies:** 1.4 (parser cleanup) unblocks 3.2. 1.5 (token cleanup) before 3.3.

- **Unified front-end representation refactor (FULL DESIGN, later-stage optimization):**
  [2026-06-09-frontend-representation-refactor.md](2026-06-09-frontend-representation-refactor.md)
  is the comprehensive design that **details and reframes 3.1/3.3/3.4** (Phase-2 is now
  pre-PARSE `cir_node` AST, not just pre-lex; Phase-3 modules = the embedded forest) **and adds
  the Track-1 front-end prerequisites they depend on**: flat value-record token buffer + index
  cursor + source-stack (PoC: 3.6–4.5× over `deque<TokenBase*>`); an arbitrary-precision
  out-of-line value pool (fixes `__int128`/`_BitInt`, currently a 64-bit alias); `uid` as the
  universal handle with madc metadata in `uid`-keyed side-arrays (the c2mir-blind superset) +
  `uid`→MIR for debug info; static-immutable/project-volatile forest with materialize-on-resolve;
  and an optional, fenced c2mir hook seam (HIR/LIR; OSR/deopt + polyglot dynamic execution are
  flagged research-grade). Subsumes [2026-06-09-embedded-header-forest-design.md](2026-06-09-embedded-header-forest-design.md)
  and [2026-06-09-lazy-member-body-instantiation-plan.md](2026-06-09-lazy-member-body-instantiation-plan.md)
  (LANDED). **After** the current real-header correctness work, not before.

- Project build driver v1 landed (`--project compile_commands.json`, multi-TU compile+link+JIT-run of `main`). Replaces the need for a hand-written umbrella translation unit (like SMAUG's `SMAUG.mad`) for multi-file C programs. Deferred follow-ons: Makefile-subset reader + a link-description section (compile_commands.json carries no link rule); native `.madproj`; other-ecosystem readers; honoring `ProjectTU.working_dir` for include resolution (SMAUG will need it); `--project` + `--emit=c11` (project mode currently ignores `--emit`); real object-code-to-disk (parity-recovery item — asmjit on master had it); parallel/incremental build + manifest auto-detection. **Next concrete step: SMAUG bring-up via a generated `compile_commands.json` (separate follow-on plan).**

---

## Track 4: Embedding & Library

*Make madc usable as a library in other programs.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 4.1 | libmadc C++ API | — | **DONE** | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.2 | C ABI shim (`extern "C"`) | 2-3 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.3 | Fork-based isolation / worker mode | 3-4 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.4 | Node.js integration | 4-6 wk | Future | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.5 | Rust bindings (madc-sys + madc crate) | 2-3 wk | Future | [perry-rust-integration.md](perry-rust-integration.md) |

**Dependencies:** 4.2 before 4.4 and 4.5.

- **Type table + value ABI (DESIGN AGREED 2026-06-12):**
  [2026-06-12-type-table-value-abi-design.md](2026-06-12-type-table-value-abi-design.md)
  — one segmented uint32-typeid table (primitives / system-forest / project
  segments) as the canonical type identity, plus a 32-byte `madc_value`
  interchange struct (16-byte payload inlines every madc primitive incl.
  `__int128`/`_Complex`/`v128`; SSO; refcounted cells; gradual-typing flags
  LOCKED/COERCE/NULLABLE; re-tag unrestricted by default). **Eval package C is
  the first consumer**; the cir_node `datadef` side-array (refactor P3), forest
  type-ref serialization (P4), and the tag-arithmetic retirement are later
  campaigns on top. NaN-boxing (5A.5) stays internal-madcdis-only.

---

## Track 5: Data Substrate & Storage

*Three-tier data architecture: core substrate (madcdis) + external
drivers (madcdat) + language-conventional interfaces.*

### Track 5A: madcdis — Core Data Substrate (`libmadcdis`)

*Typed in-memory data substrate. Ships as optional `libmadcdis.so`.
Pools, values, interning, datasets, relations, query IR, planner.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5A.1 | Library restructure — split madcdis from madcdat | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.2 | DataSet/Relation/Query/Schema/Mapper → `include/madcdis/` | 1 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.3 | DataSource moves from libmadc to madcdis | 1 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.4 | Memory pools (arena, slab, size-class, intern) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.5 | Value system — NaN-boxing, refcounting, interning | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.6 | Multiplicity dedup for collections | 2 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.7 | Column encoding catalog (dict, RLE, FoR, delta, prefix, GCD) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.8 | mem:// and shm:// pool-backed drivers | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.9 | Federated query planner (core, capability-aware) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.10 | GQL as canonical query language + SQL/Cypher lowering | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.11 | Derivation relations (keyframe aggregation, retention) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.12 | COW snapshots (fork-based, page-level) | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |

### Track 5B: madcdat — External Storage Drivers (`libmadcdat`)

*Optional companion library. Depends on libmadcdis. External backends.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5B.1 | Library restructure — libmadcdat depends on libmadcdis | 1-2 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.2 | File-format drivers (CSV/DSV, FLR, VLR, snapshot) | Ongoing | **Partial** (DSV, FLR, VLR exist) | [madcdat-plan.md](madcdat-plan.md) |
| 5B.3 | Keyed local DB drivers (BDB, GDBM, QDBM) | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.4 | SQLite driver | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.5 | Network DB drivers (MySQL, PostgreSQL) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.6 | Graph DB drivers (FalkorDB, Neo4j) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.7 | Service drivers (HTTP/REST, MCP, S3) | 4-6 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.8 | Structured text adapters (SMAUG areas, mbox, TOML) | 2-3 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |

### Track 5C: Language-Conventional Interfaces

*Multiple syntactic surfaces over the same data substrate.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5C.1 | C-native core API (DataSet, Cursor, Query builder) | 2-3 wk | **Partial** | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.2 | C++23 ranges integration (madc::linq::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.3 | Ruby-style trailing blocks (madc::ruby::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.4 | Python comprehensions (madc::python::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.5 | Objective-C brackets (madc::objc::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.6 | ORM-style records (madc::orm::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.7 | Native query sub-grammars (sql::, cypher::, gql::) | 4-6 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |

**Library structure:**
```
libmadc          (core: compiler, runtime, embedding API)
  ↑
libmadcdis       (optional: data substrate — pools, values, datasets, query, planner)
  ↑
libmadcdat       (optional: external drivers — BDB, GDBM, SQLite, MySQL, etc.)
```

**Dependencies:**
- **Track 1.3 (CIR coverage) must reach full parity before any
  Track 5 work begins.** The data substrate needs a stable compiler
  foundation — templates, full C++ class support, and AOT output must
  work before DataSet<T>/Cursor<T>/Relation<A,B> can compile through
  the CIR → c2mir → MIR pipeline.
- 5A.1-5A.3 (restructure) first — moves existing code to new library boundary
- 5B.1 follows 5A.1 — madcdat depends on madcdis
- 5A.4-5A.5 (pools, values) before 5A.7-5A.12 (column encoding, COW, derivation)
- 5C.1-5C.2 (library-only surfaces) independent of compiler work
- 5C.3-5C.7 (compiler-integrated surfaces) require Track 9 (multi-syntax)

**Research:** [madcdis-memory-research.md](madcdis-memory-research.md) — design lineage from SMAUG, Lucene, modern arenas, refcounting

---

## Track 6: Platform Support

*Run madc on more than just x86-64 Linux.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 6.1 | macOS/ARM64 MVP (via MIR — c2mir + MIR are already cross-platform) | 10-15 wk | Planned | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.2 | macOS SIMD (NEON) | 2-3 wk | Blocked on 6.1 | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.3 | macOS AOT (Mach-O writer) | 4-6 wk | Future | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.4 | Windows port | TBD | Not started | — |

**Dependencies:** 1.3 (IR) dramatically reduces 6.1 effort.

---

## Track 7: Rendering Abstraction (`ui::`)

*Universal semantic rendering: teletype to Unreal Engine. WCAG by design.
Hardware × user preference × accessibility three-way negotiation.
JIT-time capability resolution for zero runtime overhead.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 7.1 | Semantic IR + Level 0 (text stream) | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.2 | Level 1 — curses/terminal backend | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.3 | Reactivity + compiler-tracked state deps | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.4 | Level 2 — 2D graphics (Skia/Cairo) | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.5 | Level 3 — Web backend (WebSocket + thin JS) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.6 | Level 3 — Native GUI (SDL2/GTK) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.7 | Level 4 — GPU/3D (WebGPU/Metal) | Future | Future | [rendering-abstraction.md](rendering-abstraction.md) |

**Dependencies:** 2.1 (constructors) for widget lifecycle. 7.1-7.2 can
start after 1.2 (cleanup) makes the parser ready for `render` blocks.

---

## Track 8: Tooling (madcide + libmadcedit)

*A Turbo-C style IDE and reusable editor library, built in madc.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 8.1 | libmadcedit core — piece table, cursor, undo, CUA keys | 3-4 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.2 | libmadcedit curses rendering | 2-3 wk | Blocked on 7.2 | [madc-ide.md](madc-ide.md) |
| 8.3 | Syntax highlighting + keybinding profiles (Vim, Emacs, Turbo-C) | 2-3 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.4 | madcide shell — file tree, tabs, build, errors | 3-4 wk | Blocked on 8.2 | [madc-ide.md](madc-ide.md) |
| 8.5 | Advanced — find/replace, split views, go-to-def | Ongoing | Future | [madc-ide.md](madc-ide.md) |

**Dependencies:** 7.1-7.2 (rendering Level 0-1). Config via TOML + madc scripts.

---

## Track 9: Multi-Syntax Support

*Write madc programs in Python, Ruby, or Rust syntax. Controlled via
`#pragma syntax python`. Syntax is skin-deep — AST, compiler, IR unchanged.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 9.1 | Syntax profile infrastructure + lexer config | 2-3 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.2 | Python-style indentation mode | 3-4 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.3 | Type annotation variants (suffix syntax) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.4 | Ruby/Rust profiles | 2-3 wk ea | Future | [multi-syntax.md](multi-syntax.md) |
| 9.5 | Mixed-syntax files (`#pragma syntax`) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |

**Dependencies:** 1.2 + 1.4 (parser cleanup). Editor highlighting reuses profiles.

---

## Track 10: Future Language Evolution

*Safety, modern features, and long-term direction.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 10.1 | Optional bounds checking (`--check-bounds`) | 1-2 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.2 | Ownership annotations (RAII-based) | TBD | Future | [future-considerations.md](future-considerations.md) |
| 10.3 | Go-style error returns (multi-return convention) | 1 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.4 | `-O` optimization levels (`-O0..-O3` flag landed; MIR-gen level) | 2-3 wk | **Partial** | — |
| 10.5 | TOML parser (for config files) | 1-2 wk | Future | — |

**Dependencies:** 2.1 (RAII) before 10.2. Existing multi-return enables 10.3.

---

## Ideal Execution Order

Each step builds on the previous. Items at the same indent level can
run in parallel.

```
 COMPLETED:
 ──────────
 1.  Track 1.2  Code cleanup Phase A                    [DONE v0.20.1]
 2.  Track 2.1  Constructors & destructors              [DONE v0.21.0]
 3.  Track 2.2  Operator overloading                    [DONE v0.21.0]
 4.  Track 2.3  References T& + const enforcement       [MOSTLY DONE]
 5.  Track 2.4  new / delete                            [DONE v0.21.0]
 6.  Track 2.5  Single inheritance                      [DONE v0.21.0]
 7.  Track 2.6  Virtual functions / vtables             [DONE v0.21.0]
 8.  Track 2.7  Exception handling (SJLJ + unwinding)   [DONE v0.21.0]
 9.  Track 2.8  Access control + auto token position    [DONE]
10.  Track 2.9  Generic extern class ctor/dtor          [DONE v0.21.0]

 NEXT UP (recommended order):
 ────────────────────────────
11.  Track 1.3  CIR coverage — cir_node (MC11-IR) → c2mir → MIR  [ongoing]
     ├── ★ SMAUG 1.8 boots, runs, and is playable on this path (v0.25.0)
     ├── Burn down the ~95 CIR integration failures toward 0
     ├── Build the .mc11/.c renderer + gcc -fverbose-asm fidelity
     │   gate + cir_node-vs-`c2m -d` differential
     └── Then reimplement eval/exec + REPL on MIR (deferred)
     ** THE PARITY-TO-MASTER GATE — sole backend; nothing downstream
        ships, and develop is not promoted to master, until this reaches
        full parity with master's C89 coverage **

 ║── Track 7.1  Rendering: Semantic IR + Level 0         [2-3 wk]
 ║   └── render { } blocks, UINode, text output
 ║       Can start in parallel with Track 1.3

12.  Track 1.4  Code cleanup Phase B                    [3 wk]
     └── Parser dereference & subscript unification
         Unblocks: PCH transition, parser resilience

 ║── Track 7.2  Rendering: Level 1 curses backend        [3-4 wk]

13.  Track 8.1  libmadcedit core                         [3-4 wk]
     ├── Piece table, cursor, undo/redo, CUA keybindings
     └── Requires: Level 0 rendering (step 11b)

14.  Track 8.2  libmadcedit curses + syntax highlight    [4-6 wk]
     └── Requires: Level 1 rendering (step 12b)

15.  Track 8.4  madcide shell                            [3-4 wk]
     └── Self-hosting milestone: edit madc in madcide

16.  Track 3.2  PCH transition                           [2-3 wk]
     └── Replace text-embedded stubs with pre-compiled

17.  Track 6.1  macOS/ARM64 MVP (via MIR)               [10-15 wk]
     └── c2mir + MIR are already cross-platform; CIR coverage (step 11) first

18.  Track 4.2  C ABI shim                               [2-3 wk]

19.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

20.  Track 7.3-7.6  Rendering Levels 2-3                [4-6 wk each]

21.  Track 4.3  Fork-based worker isolation              [3-4 wk]

22.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]

     ── CIR PARITY GATE ─────────────────────────────────────
     Track 1.3 (CIR coverage) must reach full parity before
     data work begins.

23.  Track 5A.1-3  madcdis library restructure            [3-4 wk]
     Track 5B.1    madcdat depends on madcdis             [1-2 wk]

24.  Track 5A.4-5  Pools + value system                  [6-8 wk]

25.  Track 5A.9   Federated query planner                [4-6 wk]

26.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]

27.  Track 4.4  Node.js integration                      [4-6 wk]
     Track 4.5  Rust bindings                            [2-3 wk]

28.  Track 3.4  Modules (.madm)                          [6-8 wk]

29.  Track 6.3  macOS AOT (Mach-O writer)               [4-6 wk]

30.  Track 7.7  Rendering: Level 4 GPU/3D               [future]

31.  Track 9    Multi-syntax (Python/Ruby/Rust modes)     [ongoing]

32.  Track 10   Safety, optimization levels              [ongoing]
```

**Recommended next:** Track 1.3 (CIR coverage) is the highest-leverage item and
the gate for promoting `develop → master` — it is the sole backend, so nothing
downstream (data substrate, ARM64 port, AOT) proceeds, and master is not
updated, until `cir_node → c2mir → MIR` reaches full C89 parity. SMAUG 1.8
already boots/runs/plays on this path; the ~95 remaining integration failures
are the worklist. Build the `.mc11`/`.c` renderer and the gcc-`-fverbose-asm`
fidelity gate to make them mechanical and localizable, then reimplement
eval/exec + REPL on MIR. Latent items surfaced during the SMAUG bring-up:
other signed `int`-returning libc fns on the `long` fallback (declare them
`int`), and the flaky `testfortypedcomma` (uninitialized 2nd declarator in a
multi-declarator for-init).

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling **and running** SMAUG
1.8 (~158K LOC C89) end-to-end. ★ **Achieved on the CIR path (v0.25.0,
2026-05-30):** SMAUG compiles through `cir_node → c2mir → MIR`, links, boots to
a live server (`Realms of Despair ready … port 4000`), and is playable — a
connected client creates a character, navigates the world, and fights (the
Newgate room-109 serpent fight runs). This matches and now exceeds the old
asmjit backend's startup → login → serpent-combat reach, on the sole supported
backend. Remaining: broader gameplay coverage and driving the CIR integration
worklist to parity. The port itself lives in the external
[MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo.

SMAUG does NOT need C++ features (Tracks 2, 8) — it's pure C. But the
C++ features make madc useful as a general-purpose scripting language
beyond the SMAUG port. The rendering abstraction (Track 7) would let
SMAUG target terminal, web, and GUI from the same game code.

## Plan Index

| Plan | File |
|------|------|
| **ADR 0001 — CIR/c2mir backend (why c2mir, not direct-MIR)** | [../adr/0001-cir-c2mir-backend.md](../adr/0001-cir-c2mir-backend.md) |
| Code Cleanup | [code-cleanup.md](code-cleanup.md) |
| C++ Support | [cpp-support.md](cpp-support.md) |
| Cross-Cutting Insights | [cross-cutting-insights.md](cross-cutting-insights.md) |
| Data Storage & Federation (legacy) | [data-storage-federation.md](data-storage-federation.md) |
| madcdis Core Substrate | [madcdis-plan.md](madcdis-plan.md) |
| madcdis Memory Research | [madcdis-memory-research.md](madcdis-memory-research.md) |
| madcdat External Drivers | [madcdat-plan.md](madcdat-plan.md) |
| Language Interfaces | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| Future Considerations | [future-considerations.md](future-considerations.md) |
| libmadc Phase 4 | [libmadc-phase4.md](libmadc-phase4.md) |
| macOS/ARM64 Port | [macos-arm64-port.md](macos-arm64-port.md) |
| Pre-Compiled Headers | [precompiled-headers.md](precompiled-headers.md) |
| Perry/Rust Integration | [perry-rust-integration.md](perry-rust-integration.md) |
| Rendering Abstraction | [rendering-abstraction.md](rendering-abstraction.md) |
| madc IDE & Editor | [madc-ide.md](madc-ide.md) |
| Multi-Syntax Support | [multi-syntax.md](multi-syntax.md) |
| Typed-Register IR (archived — asmjit-era) | [archived/typed-register-ir.md](archived/typed-register-ir.md) |
| Gecko+MIR Transpiler (archived — superseded by CIR) | [archived/transpiler-backend.md](archived/transpiler-backend.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
