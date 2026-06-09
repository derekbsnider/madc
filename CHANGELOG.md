# Changelog

## [Unreleased]

## [v0.26.0] — 2026-06-09

Real-header C++ track: real libstdc++ `<string>`/`<iostream>`/`std::getline` via
mangled-direct binding, call-symbol derivation unified onto one gated resolver,
the `--project` multi-TU build driver (SMAUG boots end-to-end through it),
≤16-byte SIMD/`vector_size` through the MIR fork, VLAs, and multi-return
reimplemented on CIR.

### Call-symbol derivation unified onto one resolver + drift gate (2026-06-09)

- **One source of truth for a call's emitted C symbol.** Added
  `CirBuilder::call_emit_symbol` with the precedence
  `emit_symbol ?: local_emit_name ?: var_emit_name` and routed **every**
  call-symbol site through it. Previously this precedence was re-implemented inline
  at ~8 sites, each with a different partial slice (notably `func_emit_name`, which
  ignored `emit_symbol` entirely) — the drift that shipped wrong symbols.
- Merged the two synonymous `FuncDef` fields `class_emit_name` + `nested_emit_name`
  into one `local_emit_name` (a madc-emitted body's non-default symbol — a hoisted
  nested fn or an arity-disambiguated method/operator); `emit_symbol` (external ABI
  bind, no body) stays distinct. Two fields, not three.
- **Drift-prevention gate** `scripts/check-call-emit-symbol.sh`, wired into
  `make fulltest`: fails if `local_emit_name`'s value is read as a symbol anywhere
  outside `call_emit_symbol`. Verified it catches an injected violation.
- **`cout << unsigned long` / `s.size()` now work.** The post-parse CIR bind pass
  now binds non-template methods/operators of explicitly-instantiated libstdc++
  classes (e.g. `basic_ostream<char>`) to their external symbol even when the header
  marks them `inline` — so `operator<<(unsigned long)` binds the real member
  `_ZNSolsEm` (g++ match) instead of emitting a body that forwards into the
  non-exported `_M_insert<T>` template. Mirrors the existing ctor/dtor binding.
- All steps behavior-preserving (parser invariant: `var.name == local_emit_name`
  when set; `emit_symbol` mutually exclusive with `local_emit_name`). Validation:
  `make -C src fulltest` unchanged at `543 passed, 4 failed, 0 timed out, 26 skipped`;
  gcc.c-torture unchanged at `1566 passed, 31 compile-failed, 57 runtime-failed,
  1 timed out, 30 skipped`. Remaining: `cout << std::string` (free-operator class rhs
  as a const reference), the free-`std::`-fn `emit_symbol` migration, then the
  per-red ingredients. See `docs/plans/2026-06-09-emit-symbol-unification-HANDOFF.md`.

### Real `<fstream>` ofstream canary advanced (2026-06-09)

- `tmp/fs_out.mad` now compiles and runs through real libstdc++ `<fstream>` /
  `<ofstream>`, writes `hello42`, and exits cleanly. This is a canary advancement;
  full `tests/testfstream.mad` and `tests/testloop.mad` remain known reds.
- Fixed the path with generic parser/sema behavior: constructor parameter scope now
  survives trailing `throw()` / `noexcept` before constructor initializer lists,
  unqualified namespace lookup searches inline namespace children, non-expression-head
  C++ identifiers get the same namespace fallback after lexical lookup fails, and
  `&ref_returning_function()` parses as address-of an addressable call expression.
- Validation: `make -C src fulltest` remains at the known baseline
  `543 passed, 4 failed, 0 timed out, 26 skipped`; gcc.c-torture remains
  `1566 passed, 31 compile-failed, 57 runtime-failed, 1 timed out, 30 skipped`;
  string reducers, real-header `testcout_realhdr`, `test_extern_polymorphic`, and
  `tmp/fs_out.mad` canaries pass.

### Real `<string>` richer mutation path advanced (2026-06-09)

- Fixed the c2mir failure reached by real libstdc++ `std::string` mutators such
  as `s += "x"` and `s = "hi"` by treating C++ reference returns as address
  returns in the CIR/external-call boundary and applying the same derived-to-base
  adjustment used for pointer returns. Focused reducers for default construction,
  assignment, and append now compile/run quiet.
- Real libstdc++ `std::string` construction and mutation now compile and run
  end-to-end (verified at `c9fd222`): `std::string s("hello")` (`hello len=5`),
  `s += "..."`, `s = "..."`, `a + b`, and `.size()` all work. This supersedes the
  earlier same-day diagnosis that `std::string s("hello")` "still crashes" / needed
  member-template C-string-ctor instantiation — that wall is cleared. The next
  functional wall is `std::getline` (`'getline' is not a member of namespace 'std'`
  — unbound) and then `<fstream>` (`ofstream`/`ifstream` typedefs + `open()`/
  `operator<<` + the `ios_base`/`basic_ostream` hierarchy, inc-5/inc-6).
- Validation: `make -C src fulltest` remains at the known baseline
  `543 passed, 4 failed, 0 timed out, 26 skipped`; gcc.c-torture remains
  `1566 passed, 31 compile-failed, 57 runtime-failed, 1 timed out, 30 skipped`;
  real-header `testcout_realhdr`, `test_extern_polymorphic`, and `tmp/fs_out.mad`
  canaries pass.

### `--no-auto-load`: make `#load` linking explicit (2026-06-08)

- New CLI flag `--no-auto-load`: madc does **not** act on `#load` directives
  (e.g. an embedded header auto-loading libm/libcrypt). The named library is
  not `dlopen`ed; the namespace is bound to the global symbol scope, so its
  symbols must be provided explicitly — via `madc -l<lib>` or the host — and
  linking is fully under the user's control. It does *not* error (that is the
  stricter `enable_dlfcn_functions=false` sandbox knob). Backed by a new,
  dedicated `RegistrationPolicy::enable_auto_library_loading` (default on),
  set on the engine so `--project` translation units inherit it. New fixture
  `tests/testnoautoload` (embedded `<math.h>` `#load`s libm; with the flag,
  `sqrt` still resolves through madc's own libm link). fulltest 540/4.

### `--project`: relative manifest paths resolve against the manifest's own directory (2026-06-08)

- When a `compile_commands.json` entry omits the `directory` field, madc now
  resolves that entry's `file` and `-I` paths against **the manifest file's own
  directory** instead of the process working directory. This lets a manifest
  with relative paths be checked into a repository and stay portable — it
  resolves the same regardless of the cwd, which a program may need to set
  elsewhere (e.g. SMAUG runs from its data directory). An explicit `directory`
  is honored unchanged. New fixture `tests/testprojectreldir`. fulltest 539/4.

### A `.json` source defaults to `--project` mode (2026-06-08)

- **`madc compile_commands.json [args...]`** now behaves as `madc --project
  compile_commands.json [args...]`: a positional source file with a `.json`
  extension is treated as a project manifest implicitly, mirroring how gcc/clang
  select a tool by file extension. Options (e.g. `-lcrypt`) still precede the
  source positional, as for any source file. New fixture `tests/testprojectjson`
  exercises the implicit path through the production runner. fulltest 538/4.

### SMAUG boots via `--project` — the intended path (2026-06-08)

- **`madc --project <compile_commands.json> -lcrypt`** compiles all 51 non-IMC
  SMAUG `.c` files as separate translation units, links the MIR modules, and runs
  `comm.c`'s real `main` → "Realms of Despair ready … on port N" with a live
  game loop (per-pulse area resets). No umbrella, no injected `main`.
- **`fix(parser)` (`da4145c`) — record the explicit `*` count for function-type
  typedef declarators.** madc collapsed `DO_FUN g` (a C function declaration) and
  `DO_FUN *g` (a fn-ptr variable) into one bare `DataDefFPTR` (the parser consumed
  the declarator `*` but never recorded it), so `DO_FUN do_look;` emitted as a NULL
  fn-ptr global → multi-TU SIGSEGV / MIR repeated-decl. Now: count the stars, emit
  exactly that many (recorded on `Variable::fnptr_explicit_stars`; the type stays
  `DataDefFPTR` so fn-ptr call detection is unaffected). New shared
  `Program::consume_declarator_stars` helper replaces the duplicated star-eating
  loops. fulltest 537/4, torture 1566/31/57/1 unchanged.
- **`feat(cli)` (`e4005e0`) — `-l<lib>` + `--help`/`-?`/`-h`.** `-l<name>` dlopens
  `lib<name>.so` (`RTLD_GLOBAL`) so the import resolver finds its symbols at link —
  general, works with or without `--project` (SMAUG needs `-lcrypt`). `--help`
  finally prints a usage screen.

### Fixed — SMAUG bring-up: C-source language mode + empty TUs (2026-06-08)

- **Compile `.c` TUs in C mode under `--project`** (`2887740`): a `.c` source is C,
  not C++ — gcc/clang select language by extension, so the build driver now defaults
  a `.c` TU with no explicit `-std` to `--std=c89` (C mode). In `STD_MADC` the C++
  keywords (`class`/`new`/…) are reserved, so SMAUG's `class` variables hit "Expecting
  type name after elaborated type specifier" in `TokenIF`'s C++17 if-init
  declaration-probe. Scoped to `--project`; torture/fulltest unchanged. Fixture
  `tests/testproject_ckw`.
- **Accept an empty translation unit** (`1851d88`): `Program::parse()` errored on an
  empty token queue; a TU that is only comments or wholly `#ifdef`'d out is valid
  C/C++ (gcc emits an empty object). Now a successful no-op parse. Unblocked SMAUG's
  `services.c` (entirely `#ifdef WIN32`). Fixture `tests/testproject_empty`.
- **Milestone:** SMAUG boots end-to-end from a fresh madc compile and stays up serving
  ("Realms of Despair ready … on port N") via the umbrella + `--std=c` + `comm.c`'s real
  `main` (MadSMAUG `master` `2140d3f`). Global `.bss` zero-init verified (matches gcc).

### Added — project build driver (v1) (2026-06-08)

- **Project build driver (v1):** `madc --project <compile_commands.json>` compiles each translation unit independently (each with its own fresh `Program` applying that TU's `-D`/`-I`/`-std`), links the resulting MIR modules in one `MIR_context`, and JIT-runs `main` — multi-TU compile+link+run without a hand-written umbrella translation unit. First manifest reader = `compile_commands.json` (vendored nlohmann/json at `include/json.hpp`). See `docs/superpowers/specs/2026-06-08-madc-project-build-driver-design.md`.

### Added — C++11 opaque-enum declarations (`enum class E : T;`) (2026-06-06)

`enum class E : T { ... }` (with body) parsed, but the forward/opaque declaration
`enum class E : T;` (and unscoped `enum E : T;`) failed "Expecting identifier after
type": the no-`{` branch assumed a *variable* declaration and pushed `int`, leaving
`int ;`. Now a `;` after the (optional) underlying type is recognized as an
opaque-enum-declaration — the tag is registered as a type and the statement
finishes; a later full definition re-registers with its enumerators. Second
real-header parser gap; unblocks `std::byte` (`enum class byte : unsigned char;`)
in <vector>/<map>/<set>/<memory>. `tests/testopaqueenum.mad` covers scoped/unscoped
opaque-then-full-def; fulltest 520/4/0/26, zero regressions.

### Added — C++ brace-initialization of variables (`T x{...}`) (2026-06-06)

madc parsed `T x = {...}` but not the brace-init form `T x{...}` (failed
"unexpected token type 7"). Now a post-declarator `{` is handled in
`parseDeclaration`, reusing the existing balanced brace-list parser: aggregates
/arrays/structs/classes route through `= {...}`; scalars are unwrapped
(`int x{7}` -> `= 7`, empty `int x{}` -> value-init `= 0`) to avoid a separate
scalar `= {N}` defect. First parser gap from the real-header measurement
(unblocks `in_place_t in_place{}` in <utility>/<memory>). `tests/testbraceinit.mad`
covers scalar/empty/array/struct/namespace forms; fulltest 519/4/0/26, zero regr.

### Added — predefined compiler macros from the configured compiler (2026-06-06)

`scripts/gen_predefined_macros.sh` captures the build compiler's predefined macros
(`<CXX> -dM -E -std=c++17`) at build time into `src/predefined_macros.cpp` (460:
450 object-like + 10 function-like such as `__INT8_C(c)`), exposed via accessor
functions (PLT-resolved — avoids PIE text relocations). `_tokenizer_init` seeds
them into `define_map`/`macro_map` AFTER the hand-set builtins and BEFORE `-D`, so
real-toolchain values win and `-D` can still override. Real system headers branch
heavily on these (`__STDC_HOSTED__`, `__SIZEOF_*`, `__*_TYPE__`, feature macros).

Mode-gated: `__cplusplus`/`__GNUG__` (C++-only) are seeded **only** in an explicit
C++ std — never in C mode or the `STD_MADC` default — so the bulk of tests and C
code's `#ifdef __cplusplus` are unaffected. Host/std-specific → gitignored,
regenerated each build (`make clean` refreshes). Third preprocessor-environment
piece for real-header parsing. `tests/testpredefmacros.mad` covers it; fulltest
518/4/0/26, zero regressions.

### Added — system include paths from the configured compiler (2026-06-06)

The lexer's `#include <...>` system search list was hardcoded C-only
(`/usr/local/include`, `/usr/include`, `/usr/include/x86_64-linux-gnu`) with a
`// TODO: should come from ./configure`. Now `scripts/gen_sys_includes.sh` runs
the build compiler's `<CXX> -x c++ -E -v` at build time and emits
`src/sys_include_paths.cpp` with the real search list — including the **C++ paths**
(`/usr/include/c++/NN`, …) the old list lacked. The lexer uses it (falling back to
the hardcoded C list if detection is empty). Host-specific, so gitignored and
regenerated each build (`make clean` refreshes).

Effect: madc now finds the real C++ header closure without manual `-I`. A real,
non-embedded system header parses end-to-end — e.g. `#include <cstddef>` emits the
real `typedef … ptrdiff_t/size_t`. Second piece of the preprocessor environment
for real-header parsing. fulltest 517/4/0/26, zero regressions.

### Added — `-D` command-line macro defines (2026-06-06)

madc now accepts `-DNAME`, `-DNAME=VALUE`, and `-D NAME` (repeatable, gcc-style: a
bare name defines to `1`). Object-like; applied after the builtin defines so a
`-D` can override one. First piece of the preprocessor *environment* needed to
parse real system headers (which branch on predefined macros). New
`tests/testdefineflag.mad` (+`.flags`/`.expect`) covers single/multiple/`=value`/
bare forms; matches gcc. Note: `-D` one-at-a-time won't scale to the ~450
predefined macros — a configure-captured predefined-macro builtin set is the
follow-up (see `docs/plans/2026-06-06-real-header-pch-pipeline.md`).

### Added — `--emit=c11` SIMD rendering (`cir_emit_c.cpp`) (2026-06-06)

The transpile renderer now renders vector types and compound literals, so the
emit-C-vs-g++ oracle holds for vectors:

- `N_SPEC_DECL` now renders its attribute operand (op 2) — a SIMD typedef emits
  `typedef int v4 __attribute__((vector_size(16)));` instead of `typedef int v4`.
  (Also un-drops the `cleanup` attribute, previously silently lost.)
- New `N_ATTR` case → `__attribute__((name(args)))` (renders the vector attr in a
  cast/type-name spec list).
- New `N_COMPOUND_LITERAL` case → `(T){ ... }` (was `/*<unhandled COMPOUND_LITERAL>*/`).

10 of 11 SIMD tests are now emit-C-vs-gcc parity-OK; the 11th
(`testgccwidevectorshift`) renders its 64-byte vector correctly but fails to link
under plain gcc because `__builtin_sub_overflow` lowers to the madc runtime symbol
`__madc_sub_overflow_u32` — a pre-existing, SIMD-unrelated emit-c portability gap
(overflow builtins assume the madc runtime). Full suite unchanged at 515/4/1/26.

### Added — madc SIMD frontend: lower `DataDefSIMD` to a c2mir vector type (2026-06-06)

madc already parsed `__attribute__((vector_size(N)))` / `__vector_size__` into a
`DataDefSIMD`, but `cir_builder` never lowered it — the vector-ness was dropped and
c2mir saw a scalar (`typedef int v4hi`), so vector code failed its check pass. With
the fork now providing ≤16B vectors, the bridge is built (Tier-1 lowering — reuse
c2mir's own attribute→vector machinery rather than duplicating it):

- `typedef_decl`: a `DataDefSIMD` typedef emits the **element** type's specs and a
  `vector_size` `N_ATTR` on the `N_SPEC_DECL` attrs operand → c2mir's
  `apply_vector_attrs` rewrites the typedef into a real vector type, so every use
  inherits it (subscript, arithmetic, literals handled by c2mir natively).
- `append_type_specs`: a `DataDefSIMD` emits element specs + a `vector_size`
  `N_ATTR` **directly in the spec-qual list** → covers alias-less sites uniformly
  (casts `(V)x`, abstract type-names, params), where c2mir scans the same list.

**All 11 previously-skipped SIMD tests now pass** (un-skipped), including the
one-lane `__int128` vector and the 32-byte/64-byte wide vectors (via c2mir's
scalar-lane fallback). Full suite **515 passed / 4 failed / 1 timed out / 26
skipped**, zero regressions vs the `504/4/1/37` post-pin-bump baseline.

Follow-up (done, see the entry above): the `--emit=c11` renderer needed matching
SIMD rendering for emit-C-vs-g++ parity.

### Changed — consume MIR fork ≤16-byte SIMD; bump `MIR_COMMIT` → `2ffebff` (2026-06-06)

The MIR fork's SIMD/vector work (`feature/simd-vector-support-codex`, 61 commits)
was fast-forward-merged to the fork's `develop` and pushed. `develop` now carries
≤16-byte (`v128` and smaller) `vector_size`/`ext_vector_type` support across
c2mir + MIR (frontend, interpreter, x86-64 codegen, ABI), validated by the fork's
own `make test` and the 37 GCC c-torture vector files.

`MIR_COMMIT` is bumped `8864a73` → `2ffebff`. Rebuilding madc against it is a
clean superset: full suite **504 passed / 4 failed / 1 timed out / 37 skipped**,
zero regressions vs. the prior `486 / 4 / 1 / 55` baseline.

**18 integration tests un-skipped** (deleted `.mir_skip` fixtures): they were
skipped only because the older pinned MIR lacked the relevant c2mir features, all
of which `2ffebff` now provides — `_Alignas`/`alignof`, compound literals
(global-ptr, GNU designators, array), inline-asm decl/output/rw-operand + nested
asm barriers, `__builtin_strcmp` macro-cycle, `__builtin_abs` (unsigned), global
aliases (array + scalar), K&R fn-ptr varargs, `_Decimal64` zero, wide strings,
`prefer`, and `argv` deref.

Note: at this step the madc-side SIMD frontend was not yet wired, so the 11
`testgccvector*/testsimd*` tests stayed skipped. That frontend bridge landed in
the immediately-following change (above), un-skipping all 11.

### Changed — retire-std-hardcoding cleanup merged to `develop` (2026-06-05)

The intended std-class hardcoding retirement is now on `develop`.
`scripts/check-no-std-hardcoding.sh` reports **0 offending lines**. Core madc
should rely on standard/embedded C++ headers, generic object/overload/mangling
machinery, and real libstdc++ declarations/operators for classes such as
`std::string`, iostream/istream/ostream, sstream, containers, and user classes,
instead of compiler/runtime per-class branches.

Release prep now treats compiler warnings as blockers. A clean `make -C src`
rebuild on `develop` emitted no compiler warnings, `make -C src test` passed,
and the latest capped `make -C src fulltest` against the SIMD branch hit the
known failing set. The aggregate harness reported
**486 passed, 4 failed, 1 timed out, 55 skipped** with
`testfortypedcomma` classified as `TIMEOUT`. The next validation goal is
driving the remaining fulltest reds/timeouts to green; the largest
longer-running parity bucket
remains the documented SIMD/vector_size work in c2mir and the `/workspace/mir`
fork.

### Added — MIR fork SIMD/vector_size checkpoints (2026-06-05/06)

The `/workspace/mir` branch `feature/simd-vector-support-codex` is now at
`2ffebff`, still intentionally unpinned by madc's `MIR_COMMIT`. Checkpoint
`6257780` added the first c2mir GNU `vector_size` slice with distinct
memory-backed vector types, size/alignment, brace initialization, scalar
subscript reads/writes, block copy/assignment, and memory-shaped
parameter/return plumbing.

Follow-up checkpoint `2194f8c` adds MIR `v128`, `vmov`, `vaddi32`/`vsubi32`,
vector bitwise ops, signed `v4i32` comparisons, interpreter/x86-64 codegen,
`mir-tests/test17.mir`, and C2MIR lowering for signed `v4i32`
arithmetic/bitwise/unary/scalar splats/comparisons. `eceffd0` adds unsigned
`v4u32` equality/inequality. `516db72` adds same-size `v128` vector-to-vector
casts. `3a7bbbd` adds unsigned `v4u32` ordering comparisons by biasing operands
and reusing the signed compare. `240f838` adds `v4i32`/`v4u32` vector shifts
via lane-wise scalar MIR lowering for vector/scalar counts and compound shifts.
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
`__builtin_vectorelements`. `c-tests/new/vector-size.c` covers the expanded
and width-changing shuffle C2MIR paths. `40661db` adds C2MIR support for
`__builtin_shufflevector` result widths determined by the index count, covering
smaller and larger result vectors from `v128` sources. `62f8f31` adds
`__builtin_shufflevector` support for non-`v128` source vector widths by
materializing supported vector sources to memory and copying selected scalar
lanes. `84377c2` adds generic non-`v128` GCC `__builtin_shuffle` support for
same-type vectors by materializing supported vector widths to memory and
copying lanes under runtime mask modulo rules. `665dbbb` adds C2MIR `v128`
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
and x86-64 SSE `addps` / `subps` / `mulps` / `divps` codegen. C2MIR now selects
those opcodes for `vector_size(16)` float arithmetic while keeping the existing
scalar-lane fallback for other floating vector widths and double lanes.
`ffa02b6` adds MIR packed `v128` f32 comparison opcodes (`veqf32`, `vnef32`,
`vltf32`, `vlef32`) with interpreter support and x86-64 SSE `cmpps` codegen.
C2MIR selects those opcodes for `vector_size(16)` float comparisons and lowers
`>` / `>=` by swapping operands into ordered `<` / `<=`, preserving C comparison
semantics. `52a75bb` adds MIR packed `v128` f64 arithmetic opcodes
(`vaddf64`, `vsubf64`, `vmulf64`, `vdivf64`) and comparison opcodes
(`veqf64`, `vnef64`, `vltf64`, `vlef64`) with interpreter support and x86-64
SSE2 `addpd` / `subpd` / `mulpd` / `divpd` / `cmppd` codegen. C2MIR selects
those opcodes for `vector_size(16)` double arithmetic/comparisons while keeping
the scalar-lane fallback for non-`v128` double vectors. `798f18d` adds MIR
packed `v128` i8/i16 add/sub opcodes (`vaddi8`, `vaddi16`, `vsubi8`,
`vsubi16`) with interpreter support and x86-64 SSE2 `paddb` / `paddw` /
`psubb` / `psubw` codegen. C2MIR selects those opcodes for `vector_size(16)`
byte/word add/sub while keeping the scalar-lane fallback for 8-byte integer
lanes. `3f287d0` adds MIR packed `v128` i8/i16 comparison opcodes (`veqi8`,
`veqi16`, `vgti8`, `vgti16`) with interpreter support and x86-64 SSE2
`pcmpeqb` / `pcmpeqw` / `pcmpgtb` / `pcmpgtw` codegen. C2MIR selects those
opcodes for `vector_size(16)` byte/word equality and ordering comparisons,
including unsigned ordering through lane sign-bit biasing, while keeping the
scalar-lane fallback for 8-byte integer lanes.
`730b50d` adds MIR packed `v128` i16 multiply opcode (`vmuli16`) with
interpreter support and x86-64 SSE2 `pmullw` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned short multiplication and compound
multiplication while preserving scalar-lane fallback for byte, dword, and qword
integer multiply/div/mod.
`9fb836d` adds MIR packed `v128` i16 scalar-count shift opcodes (`vlshi16`,
`vrshi16`, `vurshi16`) with interpreter support and x86-64 SSE2 `psllw` /
`psraw` / `psrlw` codegen. C2MIR selects them for `vector_size(16)`
signed/unsigned short scalar-count shifts and compound shifts while preserving
scalar-lane fallback for vector-count shifts and other lane widths.
`2ec7b5d` adds MIR packed `v128` i32 scalar-count shift opcodes (`vlshi32`,
`vrshi32`, `vurshi32`) with interpreter support and x86-64 SSE2 `pslld` /
`psrad` / `psrld` codegen. C2MIR selects them for `vector_size(16)`
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
`v32`, `v64`, and `v256` coverage in `c-tests/new/vector-size.c`.
`09b79af` generalizes same-size C2MIR vector reinterpret casts beyond the
earlier `v128`-only gate. Non-`v128` vector casts now lower through
memory-backed block copies while `v128` keeps the register move path, with
`v64` and `v256` bitcast coverage in `c-tests/new/vector-size.c`.
`9f6132e` adds C2MIR same-size integer scalar/vector reinterpret bitcasts.
Integer-to-vector casts write the integer representation into vector storage,
vector-to-integer casts load from materialized vector storage, and same-size
float/pointer scalar casts remain rejected to match GCC/clang. The checker also
keeps pointer-sized vector casts out of the constant-address path, avoiding
bogus scalar-constant lowering for 8-byte vectors. `c-tests/new/vector-size.c`
now covers `v32` and `v64` scalar integer/vector bitcasts in both directions.
`3b25f0a` extends C2MIR `__builtin_shufflevector` to same-element-type sources
with different vector widths. Validation now checks source indexes against the
combined lane count, and codegen copies lanes from materialized sources using
independent source element counts. This is GCC extension coverage: GCC accepts
the mixed-source-width form, while Clang rejects it.
`9de5b22` adds MIR packed `v128` i32 multiply opcode (`vmuli32`) with
interpreter support and x86-64 SSE4.1 `pmulld` codegen. C2MIR now selects it
for `vector_size(16)` signed/unsigned int multiplication and compound
multiplication while preserving scalar-lane fallback for other integer lane
widths and div/mod.
`3bdf0e4` adds MIR packed `v128` i64 add/sub opcodes (`vaddi64`, `vsubi64`)
with interpreter support and x86-64 SSE2 `paddq` / `psubq` codegen. C2MIR now
selects them for `vector_size(16)` signed/unsigned long-long add/sub and
compound add/sub while preserving scalar-lane fallback for other unsupported
64-bit integer operations.
`c96ac07` adds MIR packed `v128` i64 scalar-count shift opcodes (`vlshi64`,
`vurshi64`) with interpreter support and x86-64 SSE2 `psllq` / `psrlq`
codegen. C2MIR selects them for `vector_size(16)` signed/unsigned long-long
left shifts and unsigned long-long right shifts while signed long-long right
shifts remain on the scalar-lane fallback because SSE2 has no arithmetic qword
right-shift instruction.
`3f33ff6` fixes the C2MIR scalar-lane fallback for qword vector comparisons:
8-byte comparison lanes now form all-ones masks in 64-bit temporaries with
64-bit subtract-from-zero, so `v2i64` / `v2u64` equality, inequality, and
ordering comparisons produce full-lane masks in generated mode.
`92accb7` adds MIR packed `v128` i64 equality opcode (`veqi64`) with
interpreter support and x86-64 SSE4.1 `pcmpeqq` codegen. C2MIR now selects it
for `vector_size(16)` signed/unsigned long-long equality and inequality, while
qword ordering comparisons stay on the scalar-lane fallback.
`7c169e7` adds MIR packed `v128` i64 ordering opcode (`vgti64`) with
interpreter support and x86-64 SSE4.2 `pcmpgtq` codegen. C2MIR now selects it
for `vector_size(16)` signed/unsigned long-long ordering comparisons,
including unsigned ordering through lane sign-bit XOR biasing.
`dad14bc` synthesizes packed signed `v128` i64 scalar-count right shifts in
C2MIR with the existing `vurshi64`, `vxor`, and `vsubi64` floor. The permanent
`vector-size.c` fixture now covers the count-zero edge for `v2i64 >> 0`.
`2ed2c4a` extends x86-64 ABI classification for top-level v8/v16 integer
vectors, passing `vector_size(1)` / `vector_size(2)` integer vectors through
the existing integer-register `blk1` path instead of `blk0` / `rblk` memory
ABI. The permanent `vector-size.c` fixture now covers `v1qi`, `v2qi` /
`v2uqi`, and `v1hi` parameter/return cases.
`e4e096b` synthesizes packed `v128` i8 scalar-count shifts in C2MIR with the
existing word-shift, mask, XOR, and byte-subtract vector floor. The permanent
`vector-size.c` fixture now covers signed and unsigned `v16qi` / `v16uqi`
left/right scalar-count shifts plus compound signed right shift.
`29775cd` synthesizes packed `v128` i8 multiplication in C2MIR with the
existing word multiply, byte mask, word shift, and OR vector floor. The
permanent `vector-size.c` fixture now covers signed and unsigned `v16qi` /
`v16uqi` multiply and compound multiply.
`0bf8e7c` adds GCC vector prefix/postfix inc/dec support for integer and
floating vector types in C2MIR. Prefix/postfix lowering reuses the existing
integer/float vector arithmetic paths with a splatted one, and postfix old
values are preserved when vector block-move assignments provide the result
destination. The permanent `vector-size.c` fixture now covers `v4si`,
`v16uqi`, `v4sf`, `v8si`, and `v8sf` inc/dec cases.
`3a63473` adds C2MIR recognition for Clang `ext_vector_type` and
`__ext_vector_type__` attributes for power-of-two element counts. The lowering
converts the attribute's element count to a byte vector size, preserves
qualifiers, and reuses the existing vector type and operation paths. The
permanent `vector-size.c` fixture now covers `clang_i4`, `clang_uh8`, and
`clang_f4` extended-vector types.
`5966c1d` splits C2MIR vector storage size from logical element count, allowing
Clang non-power-of-two extended vectors such as `ext_vector_type(3)` to keep
their logical lane count while matching Clang's rounded storage/alignment. The
same logical lane count is used by `__builtin_vectorelements`,
`__builtin_convertvector`, and `__builtin_shufflevector` result construction.
The permanent `vector-size.c` fixture now covers `clang_i3` and `clang_uc3`
size/alignment, logical element count, and lane arithmetic.
`3d9b8af` adds GCC/clang parity for mixed-signedness vector shift-count
operands. C2MIR now accepts vector shift counts whose storage size, logical
lane count, and lane width match the shifted vector even when signedness
differs, while preserving the lane-wise scalar lowering required for
non-uniform vector counts. The permanent `vector-size.c` fixture now covers
`v4si` by `v4ui`, `v4ui` by `v4si`, `v8hi` by `uint16x8_t`, and compound
mixed-signedness vector-count left shift cases.
`fc493fd` preserves GNU declaration-spec attributes and applies
`vector_size` / `ext_vector_type` after base type resolution. C2MIR now accepts
GCC/clang spellings such as `typedef signed char
__attribute__((__vector_size__(16))) V;` instead of dropping the attribute
before the typedef declarator. The permanent `vector-size.c` fixture now covers
that typedef spelling with signed-byte scalar compound modulo in the
pr94524-style shape.
`07dd396` parses attribute arguments as constant expressions and evaluates
`vector_size` / `ext_vector_type` arguments through the existing
constant-expression checker. This accepts GCC/clang spellings such as
`__attribute__((__vector_size__(2 * sizeof (long long)), __may_alias__))`.
The permanent `vector-size.c` fixture now covers that spelling plus
pr92618-style casted vector-pointer stores that write all 16 bytes.
`fbb47f3` lowers C2MIR `__builtin_abort` to a void zero-argument call to libc
`abort`, matching GCC/clang lowering and avoiding the previous unresolved
`__builtin_abort` import. `c-tests/new/builtin-abort.c` covers the generic
builtin, and exact GCC torture cases `c-tests/gcc/pr92618.c`,
`pr94524-1.c`, and `pr94524-2.c` now pass under C2MIR `-ei` and `-eg`.
`48cd7be` accepts GNU empty asm statement barriers. C2MIR now parses
`asm` / `__asm` / `__asm__` statement syntax with qualifiers, operands, and
clobbers, rejects non-empty templates/output/goto asm, evaluates input
operands, and emits no MIR instruction for empty templates. The exact GCC
torture cases `c-tests/gcc/pr53645.c` and `pr53645-2.c`, plus focused
`c-tests/new/empty-asm.c`, now pass under C2MIR `-ei` and `-eg`.
`ff01f80` extends C2MIR `force_val` handling for narrow address-taken
register-backed scalar lvalues. `char` and `short` rvalues are now sign- or
zero-extended after byte/word pointer writes, fixing exact GCC `pr109040.c`.
Coverage adds `c-tests/gcc/pr109040.c` and focused
`c-tests/new/narrow-reg-address.c`.
`fbe5efb` lowers C2MIR `__builtin_memcmp` to an imported libc `memcmp` call
returning `int`, validates pointer/pointer/integer argument types, and avoids
the previous unresolved `__builtin_memcmp` symbol. Coverage adds focused
`c-tests/new/builtin-memcmp.c` plus exact GCC SIMD cases `c-tests/gcc/simd-5.c`,
`pr65427.c`, and `pr60960.c`.
`033732f` preserves leading GNU attributes in declaration specifiers and
type-name specifier/qualifier lists instead of discarding them before the base
type is known. This accepts macro-expanded forms such as
`__attribute__((vector_size(N * sizeof(T)))) T` in ordinary declarations and
compound literal type names. Coverage adds exact GCC SIMD cases
`c-tests/gcc/scal-to-vec1.c`, `scal-to-vec2.c`, and `scal-to-vec3.c`.
`95e52f9` preserves union aliases through array subscripts by keeping array
storage operands in lvalue/storage context when possible and carrying an
existing union alias onto the indexed memory load/store. This prevents MIR O2
DSE from deleting union-width stores that are later read through array members,
fixing the exact GCC SIMD case `c-tests/gcc/20050316-2.c` under C2MIR `-ei`
and `-eg`.
`626f75e` adds C2MIR `__int128` / `unsigned __int128` spelling and narrow
memory-shaped scalar handling, then lowers one-lane unsigned `__int128` vector
equality/inequality by comparing and storing the low/high 64-bit halves. This
closes exact GCC `pr105613.c` under C2MIR `-ei` and `-eg`, with new coverage in
`c-tests/gcc/pr105613.c`.
`59117d8` recognizes C2MIR `__builtin_copysignf` and `__builtin_nan` as checked
builtins and lowers them to imported libm `copysignf` / `nan` calls. This clears
the remaining IEEE vector-search blockers discovered in GCC torture triage:
exact GCC `c-tests/gcc/pr72824-2.c` and `c-tests/gcc/fp-cmp-cond-1.c` now pass
C2MIR `-ei` and `-eg`. Coverage also adds focused
`c-tests/new/builtin-fp.c`.
`c69f4da` adds the remaining 21 exact GCC c-torture vector fixtures found by
the vector-construct scan:
`20050316-{1,3}.c`, `20050604-1.c`, `20050607-1.c`, `20060420-1.c`,
`pr108292.c`, `pr110817-{1,2,3}.c`, `pr123753.c`, `pr23135.c`,
`pr70903.c`, `pr71626-1.c`, `pr85169.c`, `pr85331.c`, `pr94412.c`,
`pr94591.c`, and `simd-{1,2,4,6}.c`. All 37 GCC execute tests found by
searching for vector constructs are now checked in under `c-tests/gcc` and pass
C2MIR `-ei` and `-eg`.
`55c65ee` adds text and binary MIR I/O round-trip support for `MIR_T_V128`
data items by representing each vector element as 16 byte values. Coverage now
exercises textual scan/output in `mir-tests/scan-test.c` and binary
write/read in `mir-tests/io.c`.
`e4a8945` adds MIR `v128` lane-count shift opcodes for i8/i16/i32/i64 lanes:
`vlshvi*`, `vrshvi*`, and `vurshvi*`. C2MIR selects these opcodes for
matching vector-count operands, the interpreter executes them directly, and
x86-64 generated mode lowers them through scalar lane loads/shifts/stores so
the path does not require AVX2. Coverage adds direct MIR scan/execute checks in
`c-tests/mir/vector-shift-count.mir` and C frontend checks in
`c-tests/new/vector-shift-count.c`.
`360fdb5` adds C2MIR one-lane `__int128` and `unsigned __int128` vector
lowering for add/sub/mul, bitwise ops, unary ops, equality/ordering
comparisons, scalar-count and vector-count shifts, compound assignment, and
GCC vector inc/dec by operating on low/high 64-bit halves. Coverage extends
`c-tests/new/vector-size.c`.
`2ffebff` adds C2MIR one-lane signed and unsigned `__int128` vector division
and modulo through `__divti3`, `__udivti3`, `__modti3`, and `__umodti3`
helper-call imports. C2MIR and the MIR binary runners now resolve those helpers
for saved MIR/BMIR execution, and `c-tests/new/vector-size.c` covers exact
small results plus high-half identity checks.

This is still **not** the completed Track 1.6 SIMD raise. Remaining gaps
are now beyond the 16-byte-and-smaller slice: AVX/YMM register ABI for
32-byte-and-larger external vector boundaries, broader MIR vector
opcodes/registers/interpreter/codegen, and further optional per-target packed
lowering.
Vector-condition ternary/logical semantics remain outside current C2MIR C
coverage because GCC and clang C reject those forms.
madc's `MIR_COMMIT` remains pinned to fork `develop` at `8864a73` until the MIR
branch is ready to merge and consume from madc.

Validation in `/workspace/mir`: `timeout 900 make test` passed at `2ffebff`
with interpreter/O0 `Tests 1121, Success tests 2242`, generated-mode
`Tests 1125, Success tests 2250`, plus bootstrap checks. Focused
`make scan-test` and `make io-test` passed for the new `v128` data I/O
coverage; the 21 newly checked-in exact GCC vector torture copies passed GCC
native and C2MIR `-ei` / `-eg` at `c69f4da`. Clang native passed
where it accepts the GCC forms; it rejects four GCC-only vector-element address,
vector increment/decrement, or `__builtin_shuffle` forms. Exact
`pr72824-2.c`, `fp-cmp-cond-1.c`, `pr105613.c`, and focused builtin-fp /
one-lane unsigned `__int128` vector reducers passed GCC/clang native and
assembly validation plus C2MIR `-ei` / `-eg`. Exact `20050316-2.c` and focused
union-array alias reducers passed C2MIR `-ei` / `-eg`, and adjusted array
parameter plus multidimensional array parameter probes stayed green. Focused
one-lane `__int128` vector div/mod reducers passed GCC/clang native and
assembly validation, C2MIR `-ei` / `-eg`, saved MIR `-ei` / `-eg`, and saved
BMIR interp/gen validation. Focused
prefix vector-attribute cases passed GCC/clang assembly/native validation plus
C2MIR `-ei` / `-eg`; exact `scal-to-vec1.c`, `scal-to-vec2.c`, and
`scal-to-vec3.c` passed C2MIR `-ei` and `-eg`. Focused `__builtin_memcmp`
reducers passed GCC/clang
native and assembly validation plus C2MIR `-ei` / `-eg`; exact `simd-5.c`,
`pr65427.c`, and `pr60960.c` passed GCC/clang native validation plus C2MIR
`-ei` / `-eg`; generated MIR showed `import memcmp`, `memcmp_p`, and
`call memcmp_p` calls. Focused empty-asm barrier reducers passed GCC/clang
native validation and C2MIR `-ei` / `-eg`, and generated MIR for the focused
fixture contains the input-operand call with no asm marker. Exact GCC
`pr109040.c` and focused narrow-register reducers passed GCC/clang
assembly/native validation plus C2MIR `-ei` / `-eg`. Focused
`interp-test17` and `gen-test17` passed;
generated MIR showed `vmuli32` selection for the full vector fixture; focused
v4i32 multiply reducers passed GCC/clang assembly/native validation and C2MIR
interp/gen validation; GCC/clang `-msse4.1` assembly showed `pmulld`. Focused
v2i64 add/sub reducers passed GCC/clang assembly/native validation and C2MIR
interp/gen validation; GCC/clang assembly showed `paddq` / `psubq`; generated
MIR showed `vaddi64` / `vsubi64` in both the focused reducer and
`c-tests/new/vector-size.c`. Focused v2i64 scalar-shift reducers passed
GCC/clang assembly/native validation and C2MIR interp/gen validation;
GCC/clang assembly showed `psllq` / `psrlq`, generated MIR showed `vlshi64` /
`vurshi64` for left and unsigned-right shifts. Focused signed v2i64
scalar-right-shift reducers passed GCC/clang `-msse4.2` assembly/native
validation and C2MIR interp/gen validation; GCC used a `pcmpgtq` / `psrlq` /
`psllq` / `por` sign-fill sequence, clang used `psrlq` / `pxor` / `psubq`, and
generated MIR showed the C2MIR `vurshi64` / `vxor` / `vsubi64` synthesis
including `v2i64 >> 0`.
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
validation and C2MIR interp/gen validation; GCC ignores this Clang-only
attribute as expected. C2MIR interp/gen validation also passed for the full
`vector-size.c` fixture after adding the extended-vector cases. Focused Clang
odd-lane reducers for `ext_vector_type(3)` passed native/assembly validation
and C2MIR interp/gen validation; an odd-lane
`__builtin_shufflevector` / `__builtin_convertvector` reducer also passed
Clang native/assembly and C2MIR interp/gen validation. The full MIR
`timeout 900 make test` passed after the logical-lane change.
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
Focused c2m
interp/gen reducers passed, GCC/clang packed-f32 arithmetic and comparison
assembly reducers matched the packed-single shape, GCC/clang packed-f64
reducers matched the packed-double shape, GCC/clang packed-small-integer
add/sub reducers matched the packed byte/word shape, GCC/clang
packed-small-integer comparison reducers matched the packed byte/word compare
shape, GCC/clang packed-small-integer multiply reducers matched the packed word
multiply shape, GCC/clang packed-small-integer scalar-shift reducers matched the
packed word/dword shift shapes, GCC/clang scalar-condition vector ternary
reducers plus C2MIR interp/gen reducers passed, focused ABI reducers and
mixed-compiler ABI controls established the v32/v256 compiler-divergence
boundaries, GCC/clang same-element-count convertvector reducers and C2MIR
interp/gen reducers passed, focused GCC/clang non-`v128` integer-vector
reducers and C2MIR interp/gen reducers passed, focused GCC/clang non-`v128`
vector-cast reducers and C2MIR interp/gen reducers passed, focused GCC/clang
scalar integer/vector bitcast reducers and C2MIR interp/gen reducers passed,
negative same-size float/pointer scalar-vector controls rejected, focused GCC
mixed-source-width shufflevector reducers and C2MIR interp/gen reducers passed,
the Clang rejection control for that mixed-source form still rejects, MIR dumps
showed scalar lane conversion, integer-operation lowering, non-`v128` cast
block copies, and direct integer scalar/vector reinterpret stores and loads,
focused lane-count shift validation passed native GCC, C2MIR `-ei`, C2MIR
`-eg`, direct MIR `-ei`, direct MIR `-eg`, `make scan-test`, and
`make io-test`, generated MIR from the C fixture showed all twelve `vlshvi*` /
`vrshvi*` / `vurshvi*` opcodes, and `git diff --check` is clean.
Downstream `/workspace/madc`
fulltest hit the known failing set; the aggregate harness reported
**486 passed, 4 failed, 1 timed out, 55 skipped** with `testfortypedcomma`
classified as `TIMEOUT` in this aggregate run.

### Fixed — generic real-header parser/PCH checkpoint (2026-06-05)

Real-header parsing now handles class-scope aliases/static member types,
nested/template aliases, explicit specialization constructor/destructor source
names, class-qualified expressions, method-result receiver chains,
base-qualified calls on `this`, arity-aware unqualified member lookup, and
ordinary variable/function shadowing of contextual type names. C K&R recovery
now requires a real old-style declaration suffix, so unresolved C++ unnamed
parameter types do not get misread as K&R parameter names.

PCH deserialization reconstructs keywords and builtin datatype tokens, and the
compiler hash rejects stale generated `.madh` blobs so text embedded headers are
used until real system-header PCH can preserve include-guard/macro state.

Validation: `make -C src`, `make -C src test`, focused parser regressions,
`scripts/check-no-std-hardcoding.sh`, the inline-asm scan, and fulltest
**486 passed, 4 failed, 1 timed out, 55 skipped**. Remaining real-`iostream`
work must be solved through generic class/member alias resolution from the real
headers and real libstdc++ declarations/operators, not through `stdio.h` /
`FILE*` stream bridges or per-`std` compiler shims.

### Fixed — `std::cin >>` works on the CIR path (2026-06-04)

Embedded `<iostream>` now declares `std::istream` and `extern std::cin`, and
namespace-scope C++ variables can mangle to their real Itanium symbols such as
`_ZSt3cin`. Numeric extraction binds to the real libstdc++
`basic_istream::operator>>` member, while string extraction uses a small runtime
bridge that calls the real C++ iostream extraction on `std::istream` /
`std::string`. External class operators returning references now dereference the
returned address before participating in chained expressions, so
`cin >> a >> b` keeps the stream lvalue shape. `testcin.mad` passes and fulltest
is now **486 passed, 4 failed, 1 timed out, 55 skipped**.

### Changed — PHP string helpers prefer real C++ namespace linkage (2026-06-04)

Declaration-only namespace functions now keep C++ linkage by default and mangle
to their real namespace symbols, while `extern "C"` declarations stay on the C
ABI path. The PHP string helpers now make `php::trim(...)`,
`php::number_format(...)`, and the related string functions the foundational
C++ definitions; the `__php_*` symbols are convenience wrappers that call those
namespace functions. `test_mangle` now covers GCC-backed nested namespace
symbols, and `testphp.mad` plus `--emit=c11 testphp.mad` validate that emitted C
imports `_ZN3php...` symbols for string helpers. PHP array helpers remain on the
generated wrapper path pending the `MadArray` / `MadValue` C++ API naming slice.

### Changed — auto-includes are madc-mode only (2026-06-04)

`Program::set_language_standard_option()` now centralizes `--std=` parsing for
the CLI and madc shebangs, including C++ modes. Embedded standard-header
auto-includes are gated to `STD_MADC`, so `--std=c` and `--std=c++` require
explicit includes like a standard compiler. Added `teststdcppinclude.mad` and
unit coverage for the standard-mode boundary.

### Fixed — external bool returns use `_Bool` in CIR prototypes (2026-06-04)

CIR external prototypes now emit `N_BOOL` for `dtBOOL` scalar and return specs
instead of reading bool-returning external symbols as `long`/`int`. This fixes
generic bool-returning functions and methods without class-specific handling;
`teststringmethods.mad` again reads `empty()` correctly, and `test_cir` covers
an `extern "C"` bool-returning host symbol.

### Fixed — range-for locals in included/header function bodies (2026-06-04)

`translate_block` now skips parameters by explicit `vfPARAM` flags instead of
skipping the first `N` variables by function parameter count. That positional
assumption dropped compiler-generated range-for element locals in functions with
parameters, leaving header/helper function bodies to emit uses of an undeclared
loop variable. `tests/testforeachheaderbody.mad` covers a range-for over `array`
inside an included helper body.

### Added — `extern "C"` / `extern "C++"` linkage specs parse (2026-06-04)

The parser now accepts C++ linkage specifications for single declarations and
declaration blocks while preserving existing `extern` declaration semantics.
This is a prerequisite for moving embedded polyglot namespace headers toward
ordinary C++ namespace wrappers over explicit C ABI symbols. The same slice also
keeps reference-returning class functions out of the non-trivial object retbuf
call path, avoiding a bogus hidden return-slot argument. `testexternclinkage.mad`
covers both paths.

### Fixed — typedef-preserved function signatures in CIR (2026-06-04)

Function signatures now retain source typedef aliases on `FuncDef` for returns
and parameters, so CIR prototypes and definitions can render declaration-only
extern functions with header spelling instead of falling back through raw
`DataDef` type specs. This fixes `extern "C"` string-pointer declarations such
as `string *__php_trim(string *)` without adding class-specific handling, and
keeps ordinary namespace wrappers compatible with the C ABI boundary.
`testexterncstringptr.mad` covers the path.

### Changed — embedded `<ns_php>` uses ordinary namespace wrappers (2026-06-04)

Embedded `<ns_php>` now declares the `__php_*` runtime symbols as explicit
`extern "C"` ABI functions and implements `php::` calls as normal namespace
function bodies over that boundary. Emitted C now contains generated
`__ns_php_*` namespace wrappers instead of direct `asm("__php_*")` aliases for
the C++ surface. `testphp.mad` and its `--emit=c11` path cover the split.

### Changed — embedded `<ns_perl>` uses ordinary namespace wrappers (2026-06-04)

Embedded `<ns_perl>` now follows the same model as the public C++ header:
`__perl_*` functions are explicit `extern "C"` ABI declarations, and `perl::`
functions are ordinary namespace wrapper bodies. `testperl.mad`,
`testregex.mad`, `testprefer.mad`, and `--emit=c11 testperl.mad` cover the
split.

### Changed — embedded Python/Ruby/JS namespaces use ordinary wrappers (2026-06-04)

Embedded `<ns_python>`, `<ns_ruby>`, and `<ns_js>` now declare their runtime
symbols as explicit `extern "C"` ABI functions and define normal namespace
wrapper bodies over that boundary. `testlang.mad`, `testrubycharsshadow.mad`,
and `--emit=c11 testlang.mad` cover the generated `__ns_python_*`,
`__ns_ruby_*`, and `__ns_js_*` surfaces.

### Changed — embedded `<ns_rust>` uses ordinary namespace wrappers (2026-06-04)

Embedded `<ns_rust>` now declares `__rust_*` runtime symbols as explicit
`extern "C"` ABI functions and defines ordinary `rust::` wrapper bodies over
that boundary. `rust::match` remains parser syntax and is unaffected.
`testrust.mad`, `testrustmatch.mad`, `testprefer.mad`, and
`--emit=c11 testrust.mad` cover the split.

### Changed — embedded `<algorithm>` helpers use explicit ABI wrappers (2026-06-04)

Embedded `<algorithm>` no longer maps its array helpers with direct `asm`
aliases. It now declares `madarray_size` and `__php_array_get_cstr` as explicit
`extern "C"` ABI functions, then uses ordinary helper bodies inside the header.
`testforeach.mad`, `testforeach2.mad`, `testforeachheaderbody.mad`, and
`--emit=c11 testforeach2.mad` cover the path.

### Changed — C++ library objects now stay on the generic object path (2026-06-04)

The retire-std-hardcoding branch now reaches the finish-line gate:
`scripts/check-no-std-hardcoding.sh` reports **0 offending lines**. The cleanup
removes the compiler/runtime's per-type std hooks and keeps `std::string`,
streams, containers, and user classes on the same parsed-header object model:
generic overload resolution, mangling, ctor/dtor handling, and retbuf return
paths.

As a drift cleanup, embedded `<algorithm>` now implements `std::for_each(array,
void (*)(string))` as an ordinary header function body over the existing array
helpers and a normal local `string`. The old runtime shim in `ns_php.cpp` copied
a fake libstdc++ `basic_string` layout before invoking a callback; that
class-layout copy is gone. `testforeach`, `testforeach2`, and the
no-std-hardcoding gate pass after this change.

### Fixed — a `struct` with an object member is promoted to a class (`teststruct2`) (2026-06-02)

A `std::string` **member of a `struct`** was never constructed, so `bob.name = "…"`
ran `basic_string::operator=` on an unconstructed string → crash. In C++ a `struct`
*is* a class (same record; they differ only in default access), and the **contents**
decide whether it needs object semantics — not the keyword. So a `struct` now stays a
plain `DataDefSTRUCT` and is **promoted to a `DataDefCLASS`** (which is-a `DataDefSTRUCT`)
at the closing `}` when it contains an **object member by value** (`is_object()` — a
`std::string` is an object). It then flows through the existing class machinery (member
ctors at declaration, RAII dtor at scope exit) exactly like a user class with such a
member. A struct with no object members is unchanged. Because only object-having structs
promote, trivial C structs (the whole gcc.c-torture suite + SMAUG) are untouched.
Integration **456 → 457** (`teststruct2`), full gcc.c-torture **1565** with **zero
regressions**, SMAUG boots clean. ~28 lines in the parser; no `cir_builder`/fork change.

### Fixed — multi-return (`return a, b` / `x, y := f()`) reimplemented on the CIR backend (2026-06-02)

Integration **455 → 456** (`testmultiret` recovered), full O1 gcc.c-torture **1565** with
zero regressions, SMAUG boots. Go-style multi-return was an asmjit-era feature; its
compile-side was removed with asmjit in `64f44b3` and **never reimplemented on CIR**, so it
had been silently broken since the backend switch (`return q, r` → `return 0`; `q, r := f()`
→ `q = f(); r` uninitialized). Ported the `__retbuf` ABI to `cir_builder`: a multi-return
function gets a `void` C return + a hidden `long *__retbuf` first param (`func_def`/`func_proto`
in lock-step), `return a, b` → `__retbuf[i] = …; return;`, and the call site `a, b := f(args)`
allocates `long __mret[N]`, calls `f(__mret, args)`, then `a = __mret[0]; b = __mret[1]`.
Integer multi-return (matching the parser's `int64` typing).

### Adopted — 5 proven bug fixes from community MIR forks (2026-06-02, fork `8864a73`)

Upstream MIR is frozen ~2 years; adopted genuine fixes from maintained community forks
(each attributed inline as `ADOPTED-FROM: <fork> @ <sha>`): a spill-reload `op_nums[]`
buffer overflow (`MAX_INSN_RELOAD_MEM_OPS` 2→4), `addr_regs`-stale-after-SSA (O2-gated),
`jump_opt` deleting `lref`/computed-goto labels, a vararg-RET error-path use-after-NULL,
and NULL teardown guards. O1-safe (torture 1565, zero regressions). `MIR_COMMIT` →
`8864a73`. A 4-fork survey + an O2-viability experiment are recorded in
`docs/parity/mir-fork-community-patches.md` (the GVN-`≥O3` shim was deliberately *not* adopted).

### Fixed — `const`-bound VLA: a `const` var with a runtime initializer is a VLA bound (2026-06-02, madc-only)

**gcc.c-torture 1564 → 1565 (92.9%)**, recovers `20221006-1`, zero regressions, SMAUG boots.
OTHER-38 group A (VLA follow-ons).

`int M1[len][len]` where `const int len = atoi(argv[1]);` was rejected with "Expecting integer
constant expression": the array-dimension classifier (`bracket_dim_uses_runtime_value`) treated
*any* `const`-qualified variable as a compile-time-constant dimension. A `const` qualifier does
not make a value a compile-time constant (C11 6.7.6.2) — `const int len = atoi(...)` has no folded
value. Fix: a `const` var folds to a constant dimension **only** when `read_constant_integer`
succeeds (the compile-time-known-scalar oracle already used by `resolve_integer_constant`);
otherwise it is a runtime VLA bound and lowers through the existing multidim VLA machinery. Enum
constants and folded `const` globals still fold; local `const`s with a runtime initializer that
previously *threw* now compile.

### Added — VLAs (core): multidim, runtime `sizeof`, param-bound side effects (2026-06-02, madc-only)

Core VLA support — **all 6 VLA integration tests pass** (advanced torture forms — VLA-in-struct, `const`-bound, goto-dealloc, multidim-via-pointer — still remain). **gcc.c-torture 1559 → 1564
(92.8%)** across the VLA work, +4 integration (+5 torture), zero regressions, SMAUG boots.

- **Param-bound side effects** (`int a[i++]`): the bound expression is evaluated on function entry
  (C11 6.9.1p10). Recovers `testparamvlaruntimeexpr`, torture `pr77767`.
- **Runtime `sizeof(vla)`** (`typedef int c[i+2]; sizeof(c)`): computed as
  `(dim0*…*dimk)*sizeof(element)` (C11 6.5.3.4p2) rather than a constant. Recovers
  `testtypedefvlasizeof`, torture `970217-1`, `20040411-1`.
- **Multidim** (`int M1[m][n]`): the flat malloc'd pointer's nested subscript chain is linearized
  to a single row-major index `M1[i*n + j]` (c2mir has no VLA types). Recovers `testmultidimvla`.

VLA is therefore confirmed **not** a c2mir/MIR floor gap — it is entirely a madc front-end lowering.

### Added — function-local variable-length arrays (VLAs) (2026-06-02, madc-only)

**gcc.c-torture 1559 → 1561 (92.6%)**, +1 integration (`testvla`), zero regressions, SMAUG boots.

VLA was mis-documented as a c2mir/MIR floor gap. In fact the parser already records the runtime
bound (`Variable::vla_size_expr`) and emits the VLA as a pointer — but `cir_builder` never consumed
it, so a local `int a[n]` became an **uninitialized** `int *a` → SIGSEGV. (VLA *parameters* are
plain pointers and already worked; only local *definitions* were broken — this worked on the old
asmjit backend and was never ported to CIR.) Now a function-local VLA lowers to
`a = (T *)malloc(n * sizeof(T))` with `__attribute__((cleanup(__madc_vla_free)))` freeing it at
scope exit — the same RAII mechanism class destructors use. `malloc`+cleanup (not
`__builtin_alloca`) is required so a VLA reached by a backward `goto`/loop is reclaimed rather than
growing the stack frame (the goto-loop torture `20040811-1`). Recovers torture `920929-1`, `pr43220`.
Follow-ups: multidim with 2+ runtime dims, runtime `sizeof(vla)`, param-VLA side-effecting bound.

### Fixed — static-initializer compound-literal ordering (`20050929-1`) (2026-06-02, MIR fork `4aa628b`)

Seventh c2mir-grind fix. **gcc.c-torture 1558 → 1559 (92.5%)**, zero regressions, SMAUG boots,
fulltest 451.

A file-scope object initialized with **multiple compound-literal addresses**
(`struct B e = { &(struct A){1,2}, &(struct A){3,4} }`) was miscompiled: each element's value is
produced by `val_gen`, which emits a compound literal's storage data into the module. The first
element's data happened to land before the object's own data items (clean), but the 2nd+ element's
storage was spliced into the **middle** of the object's data stream, and the element's trailing
`ref` became an anonymous data item attached to the sub-object — truncating the object (`e` ended up
8 bytes; `e.b` read into the spliced `{3,4}` data → SIGSEGV). gcc passes; stock c2m crashed. Fix
(fork `4aa628b`): `gen_initializer`'s file-scope branch pre-computes all element values **before**
emitting the object's own data items, so out-of-line compound-literal storage lands before the
parent. Constants gen position-independently → non-compound-literal static inits are byte-identical.
`MIR_COMMIT` `838b116` → `4aa628b`. PR middle-end/24109.

### Fixed — `__builtin_*_overflow` u64 helper selection (`pr85095`) (2026-06-02, madc-only)

Sixth c2mir-grind fix (madc-only, no fork change). **gcc.c-torture 1557 → 1558 (92.5%)**, zero
regressions, SMAUG boots, fulltest 451.

`__builtin_add/sub/mul_overflow` with **both operands unsigned 64-bit** miscompiled: the operands
reach the runtime helper as `long long`, which sign-extends a large unsigned value (`2^64-18 → -18`),
and `overflow_helper_name` selected the generic `_u64` helper that signed-widens them → wrong
overflow flag and result (`pr85095`: `f1(16,-16)` gave 0, want 1). The correct `_uu64` helper (which
reinterprets the inputs as unsigned) already existed but was never selected. Fix: key the `_uu64`
selection on the **operands'** signedness, not the destination — so `pr85095` (unsigned operands →
`_uu64`) and `pr91450-1` (`__builtin_mul_overflow(int,int,&u64)`: signed operands → signed-widening
`_u64`) both pass.

### Fixed — `__builtin_conjf` + `_Complex` width conversion (`20020411-1`, two-part) (2026-06-02, MIR fork `838b116`)

Fifth c2mir-grind fix (two-part). **gcc.c-torture 1556 → 1557 (92.4%)**, zero regressions,
SMAUG boots, fulltest 451.

- **madc (`cir_builder`):** lower `__builtin_conj{,f,l}(z)` → `~z` (a `~` on a complex operand is
  the conjugate in c2mir). Tier-1 (madc owns the front end) — stock c2mir has no `__builtin_conj*`
  and fell through to a nonexistent dlsym symbol.
- **c2mir (fork `838b116`):** convert `_Complex` values component-wise on a width change. A
  `_Complex float` ↔ `_Complex double` conversion was mishandled at three sites — `N_CAST`
  (scalar-cast of the aggregate), `N_ASSIGN` (block-move of LHS-size bytes from the narrower
  source), and the local initializer (same block-move flaw) — all producing garbage. Added
  `complex_to_complex()` (load each component at source width, cast to the destination component
  type, store into a fresh temp), called from all three. `MIR_COMMIT` `8f97e4f` → `838b116`.

### Fixed — static-local initializers (`pr53084`, two-part) (2026-06-02, MIR fork `8f97e4f`)

Fourth c2mir-bug-grind fix (two-part). **gcc.c-torture 1552 → 1556 (92.3%)** — `+4`
(`pr53084`, `20071029-1`, `pr124358`, `string-opt-17`), zero regressions, SMAUG boots,
fulltest 451 (`+1` `teststaticlocalinit`).

- **madc parser (the real prize):** a dead asmjit-era hoist. For a `static` local with an
  initializer, the parser pushed the initializer into the file-scope statement stream
  (`tkProgram->statements`) and cleared it from the declaration, relying on the removed
  JIT's `TokenDecl::compile()` to run it once at startup. The CIR/c2mir pipeline never
  emits those hoisted statements, so it silently dropped **every scalar static-local
  initializer**: `static int x=7` read 0, `static double d=2.5` read 0, `static const char
  *p="foo"` was left null → SIGSEGV on `p[0]`. (Masked because madc doesn't propagate
  `main`'s return to the exit code — rc-based tests read green; only printing exposed it.)
  Fix: keep the initializer on the declaration so it emits as the `SPEC_DECL`'s constant
  initializer — c2mir initializes a `static` once at load (gcc semantics).
- **c2mir (fork `8f97e4f`):** `check_const_addr_p` gated an `N_STR` base on
  `curr_scope == top_scope`, rejecting a *computed* string-literal address (`"foo"+1`) for a
  block-scope `static`. A string literal has static storage at any scope (C11 6.4.5p6) →
  `return TRUE`. `MIR_COMMIT` `772efeb` → `8f97e4f`.

### Fixed — zero-length array members (`T x[0]`) (2026-06-02, MIR fork `772efeb`)

Third c2mir bug, a two-part fix. **gcc.c-torture 1551 → 1552 (92.1%)**, zero regressions,
SMAUG boots, fulltest 450.

- **madc parser:** the nested/anonymous-struct member path didn't record per-dimension
  shape and dropped a trailing `[0]`/`[]` member to a *scalar* (`char name[0]` →
  `char name`), so `name[0]` read the wrong storage. Now it records `inner_dims` and passes
  them to `addMember`, exactly like the top-level member path — the member becomes a proper
  (flexible/zero-length) array.
- **c2mir (fork `772efeb`):** `set_type_layout` skipped any zero-size member (`continue`),
  leaving a GNU zero-length array at offset 0 instead of its running offset (it aliased the
  first member). Now a zero-size member gets the current aligned offset without growing the
  type. C99 flexible-array members (`[]`) are unchanged. (Confirmed independently via stock
  `c2m`; an upstream candidate.)

Recovers `gcc.c-torture/execute/zerolen-1.c`. `MIR_COMMIT` `74adb6a` → `772efeb`.

### Fixed — c2mir: statement-expression ending in post-increment/decrement (2026-06-02, MIR fork `74adb6a`)

Second "bugs before features" c2mir fix. **gcc.c-torture 1550 → 1551 (92.0%)**, zero
regressions, SMAUG boots, fulltest 450.

A statement-expression's value is its block's last expression, but c2mir gens
expression-statements value-discarded (`top_gen` passes `val_p=FALSE`). Most expressions
materialize their result regardless, but post-`++`/`--` only do so when used — so a
stmt-expr ending in `x++`/`x--` (`({ x--; })` as a value, `return`, or `while`/`if`
condition) left an `undef` operand and crashed MIR codegen. Fix (madc MIR fork `74adb6a`):
gen the marked last expression in value context (`val_p=TRUE`). Confirmed a **c2mir** bug
via stock `c2m`; recovers `gcc.c-torture/execute/950906-1.c`. `MIR_COMMIT` `caa6ff9` →
`74adb6a`.

### Fixed — c2mir: struct-valued statement-expression miscompile (2026-06-02, MIR fork `caa6ff9`)

First of the "bugs before features" c2mir fixes. **gcc.c-torture 1549 → 1550 (92.0%)**,
zero regressions, SMAUG boots, fulltest 450.

A GNU statement-expression whose value is a struct/union (`({ …; obj; })`) returns the
lvalue of an in-block local, but c2mir reuses that local's stack slot across
non-overlapping sibling scopes. With two in one expression — `({..A..}).x - ({..B..}).x`
— both member loads are deferred to the enclosing operator and read whichever block ran
last, yielding a wrong result (`20020320-1`). Confirmed a **c2mir** bug (not the MIR
machine, not madc): stock `c2m -ei`/`-eg` both miscompile a minimal reducer. Fix (in the
[madc MIR fork](https://github.com/derekbsnider/mir), `caa6ff9`): copy a struct/union
stmt-expr value into a fresh `ALLOCA` temporary so each has independent storage, matching
GCC. Pre-existing upstream since statement-expression support landed (`c8e3c4f`) — a clean
upstream candidate. `MIR_COMMIT` bumped `1fdf44d` → `caa6ff9`.

### Fixed — aggregate-init cluster: array compound literals + array-of-pointer declarators (2026-06-02, develop @ ec97689)

The last cheap front-end parity cluster. **gcc.c-torture 1547 → 1549 (91.9%)**, zero
regressions, SMAUG boots, and **all compiler warnings cleaned to zero**.

- **Array compound literal with a struct element** (`(S[]){{.b=3,…}}`, `pr98366`): the
  array path rendered the element type via `append_type_specs`, which emits `N_INT` for any
  struct, so c2mir saw `int[]` and rejected the brace-init as "excess elements in scalar
  initializer". Extracted the scalar path's struct/typedef/anonymous spec-builder into
  `CirBuilder::append_lit_type_spec()` and reuse it for the array path; the parser now
  propagates the element's typedef alias to the literal.
- **Array of pointer-to-typedef'd-array** (`A3_28 *paa[]`, `strlen-4` family): `var_decl`'s
  `skip_tail` correction (which compensates for the parser flattening a typedef's dims into
  `v->dims`) only applies in the non-pointer path. With a pointer prefix the parser leaves
  those dims in the pointee, so `v->dims` holds only the variable's own dims — gated
  `skip_tail` on `!is_ptr` so the trailing `[]` is no longer dropped. (`strlen-4` itself
  still hits a deeper runtime bug; the declarator family is now correct.)
- The other two tests the worklist had grouped here (`pr109938`/`pr109986`) are **SIMD floor
  gaps** (`v4si` vector initializers), not aggregate-init — re-classified.

### Changed — zero compiler warnings (2026-06-02)

- `FuncDef` constructor initializer order matched to member declaration order (`-Wreorder`);
  explicit `(T)` casts in the `MADC_COMPLEX_OPS` macro for `unsigned short` complex arithmetic
  (`-Wnarrowing` ×11 — no-op for wide types, makes the defined modular truncation explicit);
  removed two unused locals in `parser.cpp`; enlarged the `"p%zu"` `snprintf` buffer in
  `translate_module` (`-Wformat-truncation`).

### Fixed — CIR↔asmjit gcc-torture parity campaign 82.0%→91.8% (2026-06-01, branch feature/cir-stdstring-claude)

Recovered the regression the CIR backend carried vs the old asmjit backend on the
gcc.c-torture/execute suite (same runner): **CIR 1382 (82.0%) → 1547/1685 (91.8%)**,
integration 432 → **450**, zero regressions throughout, SMAUG boots + playable. Each
cluster gcc-compared, failset-diffed for zero regressions, and SMAUG-soaked.

- ~20 root-cause clusters landed: bitfield load/store, struct/aggregate member-type
  resolution, varargs (+ MIR-fork SysV ABI fixes), integer promotion, builtin/libm
  return types, `_Complex` pass/return ABI, GNU nested functions, `__attribute__`
  alias/aligned, statement-expressions, pointer-to-array, FAM, self-ref typedef, K&R
  unprototyped, function-scoped labels + block-scope `extern`.
- **Union support fixed** (`14fc16c`/`187b135`/`f9d7566`, +11): emit `N_UNION` at every
  site (definition, type-spec reference, typedef'd-anonymous, function-return, var-decl,
  extern) — previously all hardcoded `N_STRUCT`, which broke member aliasing/type-punning
  and union-by-value parameter passing.
- **Nested statement-expression last-value** `({ ({...}); })` (`069fb8b`, +3).
- **Function used as a value** (address-of / fn-ptr decay) now emits a prototype (`9ac7a1b`, +3).
- The MIR dependency is the **madc fork** (`feature/cleanup-attribute` @ `1fdf44d`), carrying
  native `_Complex`, `__attribute__((cleanup))`, the scope-depth auto-local layout fix, and
  the SysV varargs / `_Complex` / `_Alignas` ABI fixes.

### Changed — dead-code removal + test/recovery hardening (2026-06-01)

- **Removed the legacy `cir_translate` path** (`9af4e29`, `src/madc_cir.cpp` ~1800→275 lines).
  It was reachable only via `MADC_CIR_OLD=1` and had **drifted** from the live `CirBuilder`
  (the sole backend) — and a stale `test_cir` was exercising it, which hung `MIR_interp` and
  pegged the host. `test_cir` now tests the live `CirBuilder` (`7d927d5`). One backend, no A/B drift.
- **`make test` runs each unit binary under `ulimit -t` + `timeout`** (`bd1f5da`) — a hung test
  fails the suite instead of pegging the machine.
- New rule [`no-parallel-implementations.md`](.claude/rules/no-parallel-implementations.md):
  one implementation per concern; tests use the production path; cap every test run.
- New [`scripts/resume.sh`](scripts/resume.sh) rehydration preflight: live git/reflog state,
  runaway-process detection (`--kill`), and a tiered ~120k-token rehydration corpus manifest
  (self-checks the ≥100k floor) — recovers full context after an aggressive compaction.

### Changed — std:: types are real classes/templates; legacy C++ shortcuts retired (2026-05-31, branch feature/cir-stdstring-claude)

The temporary shortcuts that faked C++ niceties (a special `dtSTRING` type with bespoke
string-object lowering, the `tkSTRING` token, the `tkVECTOR`/`tkMAP`/`tkSET`/`tkLIST`
keywords, and `ns_stl.cpp` wrappers) are **retired** in favor of madc's real C++ framework:

- **`std::string` is a real C++ class** — declaration/construction/destruction, methods,
  operators (`=`/`+=`/`==`/`!=`), `cout <<`, struct members, `string*` pointers, and
  **return-by-value** all flow through the class model. Methods/ctors/dtor/operators bind
  to mangled libstdc++ symbols via a new `FuncDef::emit_symbol`; storage is a real
  `struct string` sized from `sizeof(std::string)`. `std::string` is now `std::`-only,
  defined by `#include <string>` (the `tkSTRING` builtin token is gone).
- **`std::vector`/`map`/`set` are real `#include`-defined `std::` templates**
  (`include/madc/vector|map|set`), instantiated per use through the class model + template
  engine. The `vector`/`map`/`set`/`list` keywords and `ns_stl.cpp` are removed. `vector<string>`
  works; container element destructors run (general `__destroy` intrinsic, no element leak).
  Added `namespace { }` block parsing.
- 368 → **376** integration tests, zero regressions. New: `teststringclass`,
  `teststringreturn`, `teststringeq`, `testtemplatestring`, `testtemplatecontainer`,
  `testplacementnew`, `testrefreturn`, `testcontainerdtor`. Plan + restart guide:
  `docs/superpowers/plans/2026-05-31-stdtypes-as-real-classes.md`,
  `docs/superpowers/plans/2026-05-31-RESTART-HANDOFF.md`.
- Remaining (step 4): complete operator-overloading coverage (bind `std::string operator[]`/`+`;
  extend the dispatch table — overload only when a class declares the operator) + three parser gaps.

## [v0.25.0] — 2026-05-30

CIR is now the sole backend, and SMAUG 1.8 boots, runs, and is playable.

> **Build dependency:** madc now builds against the **madc MIR fork**
> ([github.com/derekbsnider/mir](https://github.com/derekbsnider/mir), branch
> `feature/complex-support`) at `/workspace/mir` — not upstream MIR. It carries
> native C99 `_Complex` support and the c2mir fixes the CIR backend depends on.

### CIR is now the sole backend; asmjit and Gecko removed (feature/cir-node)

- **Removed the Gecko parser + MIR-transpiler entirely** (`42e9b6e`).
- **Removed the asmjit x86-64 JIT + original codegen entirely** (`64f44b3`).
  `madc parser → cir_node (MC11-IR) → c2mir → MIR` is now the **sole** backend.
  There is no `--backend=jit`; `--backend=mir` aliases to cir.
- **MC11-IR set in stone:** the `cir_node` tree derives from c2mir `node_t`
  (c2mir consumes the lowered C11 view) AND carries each node's originating
  tokens + parse subtree + file/line/col (madc retains the high-level view).
  See `docs/rules/mc11-ir.md`.
- **Honest CIR baseline: 227 pass / 193 fail / 56 skip** (integration). The 193
  are the active coverage worklist. (The earlier "419 pass / 0 fail" was the
  now-removed backend; full C89 coverage is the *target* the CIR path is
  climbing back to, not a current property.)
- **Parser-fix port + empty-body CIR fix** landed (SMAUG now parses end-to-end
  through CIR): `fd4d510`, `a429323`, `28d5e65`, `8b627ec`.
- **Deferred (stubbed):** libmadc in-process compile/exec/`eval` + the REPL
  (~100 unit tests skipped as the spec); native AOT object/executable
  (`save_object`/`save_executable` stubbed, signatures kept).
- **`--backend=cir` and `--dump-cir` CLI flags;** extern function prototypes
  with variadic support; SMAUG mini test (malloc/free/printf/structs/pointers)
  runs through the CIR path.

### ★ SMAUG 1.8 boots, runs, and is playable through CIR (2026-05-30)

The full SMAUG 1.8 codebase (~158k LOC C89) now compiles and **runs**
end-to-end through `cir_node → c2mir → MIR → JIT`: it boots to a live
server (`Realms of Despair ready … port 4000`), and a connected client can
create a character, navigate the world, and **fight** — the Newgate room-109
serpent fight runs without crashing. Integration **316 → 325**. The fixes,
in order:

- **fn-ptr-typedef rendering** (`49b79a1`, call-site `7924e42`): function
  typedefs / members / vars / params now render from the retained signature
  (`DataDefFPTR->target`) instead of erasing to `long`; the implicit `*` for a
  Form-1 fn-ptr-typedef use is added at emit time in `explicit_star_count` so
  `var.type` stays `DataDefFPTR` and the expression parser's fn-ptr-call
  detection keeps working. Cleared the SMAUG `spec_fun` blocker.
- **extern set only at variable creation** (`62577a8`): a file-scope
  definition plus a redundant `extern` of the same global share one Variable;
  `vfEXTERN` is now set only when the symbol is created, never re-set on an
  existing one, so a defined global can't be demoted to a declaration
  (MIR-link `import of undefined item help_greeting`).
- **varargs lowered to the c2mir intrinsic** (`2fbe5f0`): `va_start(ap,last)`
  → `__builtin_va_start(ap)` directly, instead of a master-`__va_args` +
  struct-copy that mis-set `reg_save_area` in large frames (SMAUG's `bug()`
  segfaulted in `vsprintf`).
- **switch `default` case emitted** (`4011d95`): `translate_switch` dropped
  every `default:` (it only emitted the separately-stored default when
  `default_index < 0`, which the parser never sets) — values matching no case
  fell through executing nothing. Was the last boot blocker.
- **strcmp family declared `int`, not the `long` fallback** (`ac16d07`):
  undeclared comparison libc fns defaulted to a `long` return; libc returns
  `int` in `eax`, so a negative result read as 64-bit became huge and SMAUG's
  `bsearch_skill_exact` (`strcmp(name,x) < 1`) always took the wrong branch —
  `skill_lookup` failed for 79 skills, every combat gsn stayed unassigned, and
  the serpent fight dereferenced `skill_table[bad]` (SIGSEGV). ASSIGN_GSN
  failures 79 → 0.
- **JIT-symbolizing crash handler** (`25c731a`): madc's SIGSEGV backtrace now
  resolves MIR-generated frames to `func+0xoff [JIT]` (maps the faulting
  address to a function's `machine_code` range). First brick of a madc-native
  debugger; pinpointed the serpent crash to `learn_from_failure`.
- Plus the array/pointer type-erasure family, 2D-array init, switch pre-case
  decls, abstract-param `N_TYPE`, and the real System V `va_list` ABI that
  drove SMAUG from **159 → 0** c2mir check errors earlier in the session.

## [v0.24.0] — 2026-05-28

Native C99 `_Complex` support in c2mir, transpiler cleanup, 410→419.

- **Native `_Complex` support in c2mir.** 13 commits to our MIR fork
  implementing C99 `_Complex` types directly in c2mir — no struct
  workaround needed. Supports `_Complex double/float/long double`,
  arithmetic (`+`, `-`, `*`, `/`, `+=`, etc.), `__real__`/`__imag__`
  operators, imaginary literals (`1.0i`), conjugate (`~`), equality
  (`==`, `!=`), boolean context, function params/return, casts.
  Zero regressions across c2mir's 1,071 test suite.

- **Complex constant folding in c2mir.** Compile-time evaluation of
  `_Complex` expressions for global initializers (`_Complex v = 3.0 +
  1.0iF;`). Complex-to-scalar cast extracts real part per C99 6.3.1.7.

- **Transpiler `_Complex` cleanup.** Removed `struct __madc_c*`
  workaround infrastructure (-302 lines). Transpiler now emits raw
  `_Complex` types and imaginary literals directly.

- **`__real__`/`__imag__` as first-class grammar operators.** New
  `AN_REALPART`/`AN_IMAGPART` AST nodes, `GT_REALPART`/`GT_IMAGPART`
  grammar terminals. Gecko parses them as unary prefix operators.

- **Test parity: 410→419.** All 12 `_Complex` tests now pass.
  Transpiler parity at 88.2% (419/475, 56 skipped).

## [v0.23.0] — 2026-05-27

MIR default backend, clang++ compiler, transpiler parity push (400→410).

- **MIR is now the default backend.** `bin/madc` uses the Gecko+c2mir
  transpiler pipeline by default. Legacy asmjit JIT available via
  `--backend=jit`.

- **clang++ replaces g++ as the default compiler.** Builds clean with
  identical results. Prepares for macOS port.

- **String literals as `const char *`.** Literal-initialized `string`
  variables emit as `const char *` instead of managed char arrays.
  Temporary `std::string` constructed at namespace call sites via
  `__builtin_alloca`. `MADC_STRING_SIZE` renamed to `STDSTRING_SIZE`.

- **Transpiler parity: 400→410 (+10 tests).** Header prototypes for
  dirent/time/netdb/select, emitter extern declarations, attribute
  mode lowering, std::vector/map/set/list tokenizer collapse,
  madc::array support, extern "C" regex/argv wrappers, builtin
  wrappers (object_size, strcpy_chk), char** pointer depth fix.

- **`MADC_EXTERN_C` macros.** `MADC_EXTERN_C0` through `MADC_EXTERN_C4`
  for generating thin extern "C" wrappers by argument count.

- **c2mir built-in headers.** Emitter preamble now uses `#include
  <stdint.h>`, `<stddef.h>`, etc. from c2mir's embedded C11 headers
  instead of hand-written typedefs.

- **_Complex type mapping (WIP).** Compound _Complex keywords lowered
  to `struct __madc_cX` types. Runtime helpers for all complex
  arithmetic compiled by clang++. Operation lowering still in progress.

- **STL container `_cstr` variants.** Map/set get, contains, put
  operations accept `const char *` keys directly.

- **Transpiler triage.** All 65 skipped tests categorized with specific
  root causes in `.mir_skip` files. `docs/transpiler-triage.md` tracks
  the full breakdown.

## [v0.22.0] — 2026-05-26

Gecko+MIR transpiler pipeline: Phase 2 semantic pre-pass, Phase 4 string
runtime, O(1) AST dispatch, iostream wrappers, and namespace function bridging.

- **Phase 2 semantic pre-pass.** `madc_sema.cpp/h` — SemaInfo struct
  collects variable types, function signatures, class info, and typedef
  names in a single AST walk before emission. TypeClass enum for
  compile-time-checked type classification.

- **Phase 4 string runtime.** `string` variables are managed `std::string`
  objects via placement-new in stack buffers. Runtime wrappers:
  `__madc_string_construct/destruct/assign_cstr/cstr/length/append`.
  Namespace functions (php::trim etc.) receive `std::string*` directly.

- **O(1) AST dispatch via `gp_set_anode_code`.** 156 anode names registered
  as integer codes at grammar build time. Eliminates ~200 `strcmp` calls per
  AST traversal. Uses Makarov's built-in Gecko API (`node->aux` field).

- **Hash map tokenizer.** Replaces ~60 sequential string comparisons per
  identifier token with a single `unordered_map` lookup for keyword
  recognition.

- **iostream wrappers.** `cout <<` / `cerr <<` / `cin >>` emit calls to
  generic `__madc_ostream_*` / `__madc_istream_*` wrappers that take a
  stream pointer. Works with any `std::ostream` / `std::istream`.
  Full manipulator support (hex, oct, setw, setprecision, etc.).

- **Extern "C" namespace wrappers.** 106 thin C-linkage functions across
  6 namespace files (php, perl, python, ruby, js, rust). Enables
  `dlsym`-based import resolution and provides a clean C API for libmadc.

- **Class inheritance.** Base class fields copied into derived struct.
  Method calls resolve through the base chain via sema. Access specifiers
  (public/private/protected) tokenized correctly.

- **`__attribute__` skipping.** Tokenizer consumes `__attribute__((...))`,
  `__extension__` constructs. Unlocks 9 GCC-extension-heavy tests.

- **`typeof` type specifier.** Grammar rules for `typeof(expr)` and
  `typeof(type)`, emitted as `__typeof__()` for c2mir.

- **Expression type inference for cout.** `infer_expr_cout_type()` walks
  AST expressions to determine correct output type — deref, subscript,
  pointer arithmetic, cast, function call return types.

- **New rule: `enum-over-strings.md`.** Never use strings or chars as
  type discriminators when an enum will do.

- Transpiler test results: 283/473 (59.8%) match legacy output, up from
  274/473 (57.9%) at session start. 475/475 legacy JIT tests pass.

## [v0.21.1] — 2026-05-25

Const enforcement, access control, token position, and JIT IR architecture research.

- **Top-level `const` enforcement.** `const int x = 5; x = 10;` is now a
  compile error. All 12 mutating operators (`=`, `++`, `--`, `+=`, `-=`,
  `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=`, `^=`) are checked.
  Correctly distinguishes top-level const (`const int x`) from low-level
  const (`const char *p` — pointer can still change).

- **`const T&` parameter enforcement.** `void f(const int &x) { x = 5; }`
  is now a compile error.

- **`public:`/`private:`/`protected:` access control.** Class members and
  methods respect access specifiers. Private/protected members are rejected
  at compile time when accessed from outside the class.

- **Automatic token position inheritance.** Every token created during
  parsing automatically inherits file/line/column from the most recently
  consumed source token. Eliminates 0:0 positions in error messages and
  prepares for IDE features (hover, go-to-definition, syntax highlighting).

- **JIT IR architecture research and MIR backend plan.** Cross-referenced
  V8, HotSpot, LuaJIT, MIR, dstogov/ir, RyuJIT, PyPy, Julia, Cranelift,
  TPDE, GCC, and LLVM. Decision: adopt MIR (MIT, ~16K lines C) as
  optimizing backend, replacing asmjit for codegen. 12 optimization passes,
  91% of GCC -O2 quality, 5 architectures, 66% smaller binary. Plan at
  `docs/plans/typed-register-ir.md`, research at
  `docs/research/jit-ir-design-2026.md`.

## [v0.21.0] — 2026-05-25

C++ class model: constructors, destructors, operators, references, new/delete,
inheritance, vtables, exceptions with destructor unwinding, and generic extern
class infrastructure.

- **User-defined constructors and destructors.** `ClassName()` / `~ClassName()`
  with LIFO destruction ordering, constructor arguments (`Foo f(1, 2)`),
  and early-return destructor cleanup.

- **Operator overloading.** `operator+`, `-`, `*`, `/`, `==`, `!=`, `<`, `>`,
  `<=`, `>=` in class bodies. Generic `try_class_operator_dispatch()` handles
  all 10 operators.

- **References (`T&`).** `vfREFERENCE` flag (varflag_t widened to uint32_t),
  auto-deref in `TokenVar::compile()` and `operand()`, LEA at call site for
  pass-by-reference.

- **`new` / `delete` operators.** `TokenNEW` (malloc + ctor + vtable) and
  `TokenDELETE` (dtor + free). Arrow method calls (`ptr->method()`) for
  heap-allocated class objects.

- **Single inheritance.** `class Derived : public Base` with member layout
  copy, method inheritance via `findMethod()` chain, automatic base ctor/dtor
  chaining.

- **Virtual functions and vtables.** `vtable_slots` vector, `virtual_methods`
  map, 8-byte `__vptr` at offset 0, vtable filled after `cc.finalize()`,
  indirect call dispatch in `TokenCallMethod::compile()`.

- **Exception handling (SJLJ).** `try` / `catch(...)` / `catch(int e)` /
  `throw` / `throw;` via setjmp/longjmp. Thread-local `MadcTryContext` linked
  list and `MadcException` state. Typed catch value binding and rethrow.

- **Destructor unwinding during exceptions (Phase B).** Runtime cleanup stack
  (`MadcCleanupEntry` linked list) tracks destructible objects inside try
  blocks. `__madc_throw_*` walks the cleanup stack calling destructors in LIFO
  order before `longjmp`. Guard bytes prevent double-destruction. Inheritance
  chains push base dtor entries so LIFO unwind calls derived first, then bases.

- **Built-in type exception cleanup.** Strings, stringstreams, file streams,
  MadArrays, vectors, maps, sets, and lists inside try blocks get cleanup
  entries. All 11 built-in types are exception-safe.

- **Generic extern class ctor/dtor infrastructure.** `DataDefCLASS` gains
  `extern_ctor` / `extern_dtor` / `_dtor_ptr` fields and
  `register_extern_ctor_dtor()`. Single generic code paths in `voperand()`
  and `cleanup()` replace ~260 lines of per-type switch cases. Adding a new
  libc C++ type requires only one registration call. Struct members with
  registered types are auto-constructed/destructed.

- **20 new integration tests.** Constructors, destructor order, constructor
  args, early return, operator overloading, references, new/delete,
  inheritance, virtual dispatch, basic exceptions, rethrow, and 8 exception
  + destructor unwinding tests (basic, LIFO order, partial construction,
  no-throw path, inheritance chain, nested try, rethrow, string cleanup).

## [v0.20.1] — 2026-05-25

Code cleanup Phase A: compiler restructuring and tooling.

- **Compiler file split.** `compiler.cpp` (15,835 lines) split into 4 files:
  `compiler_builtins.cpp` (1,296L), `compiler_control_flow.cpp` (930L),
  `compiler_operators.cpp` (5,662L), plus shared `compiler_internal.h` (283L).
  Core compiler.cpp reduced to 8,330 lines (47% of original).

- **Builtin dispatch table.** 45 `if (var.name == "__builtin_*")` checks in
  `TokenCallFunc::compile()` replaced by a 43-entry data-driven dispatch table.
  New builtins are one table entry + one handler function.

- **AST walker template.** Generic `walk_ast()` template replaces hand-written
  traversal code.  `contains_label()` reduced from 33 lines to 4.

- **`--emit-function` CLI tool.** `bin/madc --emit-function <name> <file>`
  extracts a complete function definition verbatim.  Parser path for `.mad`
  files (uses `TokenCpnd::end_line`); text fallback for C/C++ source.

- **`TokenCpnd::end_line` tracking.** Parser now records the closing `}`
  line for every compound statement.  IDE infrastructure for code folding
  and structural views.

- **`--no-includes` flag.** Disables `#include` processing during
  tokenization for processing non-madc source files.

- **Build fixes.** Makefile `test` target sets `LD_LIBRARY_PATH` for AOT
  unit tests.  `fulltest` target explicitly depends on `libmadc.so`.

- **`_chk` family consolidation.** ~200 lines of duplicated `__builtin___*_chk`
  argument remapping consolidated into two shared helpers.

- **`__builtin_shuffle` for SIMD vectors.** Element-wise lane permutation via
  runtime mask indices. Supports 1-source and 2-source forms, all element
  types (int8–int64, float, double), register-backed and memory-backed
  vectors. Honors caller destination register to avoid stale-Xmm bug.
  Closes pr85331, pr94591.

- **Anonymous struct init in union.** `{{"1234", "567"}}` for unions with
  flattened anonymous struct members now unwraps the inner TokenStructLit
  correctly. Added `has_anon_aggregate` flag to DataDefSTRUCT. Closes pr87053.

- **Self-referencing struct array init.** `struct E e[2] = { {0, &e[1]}, ... }`
  now sets array dims and vfFIXEDARRAY before parsing init expressions, so
  `&e[1]` resolves the correct element stride. Closes pr39100.

- **Wide SIMD vectors (>128-bit).** Global vectors >16 bytes now use
  Mem-backed operands. TokenAssign uses qword-by-qword memory copy for
  large SIMD assignment. Closes pr65427.

- **Large struct return (>16 bytes).** Per System V AMD64 ABI, structs
  larger than 16 bytes are now returned via a hidden `__retbuf` first
  parameter. Caller allocates buffer, callee copies via
  `emit_raw_aggregate_copy`. Works for both direct calls and function
  pointer indirect calls. Closes pr43784, struct-ret-1.

- **SIMD-to-SIMD reinterpret cast.** `(__m128i)(__m128d)v` for >8-byte
  vectors now preserves all 128 bits instead of losing the upper lane via
  movq.  General same-size SIMD reinterpret cast for >8-byte types added
  as early path in TokenCast.  Closes pr92618.

- **Unsigned overflow dispatch + arithmetic width.** `__builtin_add_overflow`
  with unsigned 64-bit inputs uses dedicated `_uu64` helpers that preserve
  unsigned semantics through `__int128` widening.  Unsigned narrower-than-
  target arithmetic (`0U - 1U` returned as `unsigned long`) now wraps at
  operand width.  Unary minus on unsigned literals (`-2U`) negates at
  32-bit.  Closes pr85095.

- **Wide SIMD (>16-byte) subscript write, pointer dereference, cast chain,
  and function return.** `v[63] = 1` on 64-byte vectors now emits a direct
  memory store.  `*p` on wide SIMD pointers copies all bytes via aggregate
  copy.  SIMD-to-SIMD casts of any element type combination use aggregate
  copy for >16 bytes.  `is_large_struct_return` recognizes SIMD >16 bytes
  for hidden `__retbuf` return.  `compile_token_normalized` keeps >16-byte
  SIMD results Mem-backed.  Closes pr85169, pr70903.

- **Inline asm clobber list.** The `=r`/`"0"` identity pattern (empty asm
  body) now recognizes clobber clauses (`: "memory"`) instead of falling
  through to the no-op path.  Closes pr49279.

- **va_arg through indirect pointer.** `va_arg(*pap, T)` where `pap` is a
  global `va_list*` no longer overwrites the pointer variable with the
  buffer value on write-back.  Closes stdarg-1.

- **Static multi-variable declarations.** `static int a = 1, b = 2;` now
  preserves the `static` attribute for the second variable.  Previously
  the comma-continuation pushed back only the type token, losing static
  storage for subsequent variables.  Closes va-arg-22.

- **va_end semantics.** Changed from `ap = 0` to `((void)(ap))` to match
  GCC's no-op-with-side-effect-evaluation behavior.

- **va_arg on struct member / array element.** `va_arg(a.g, long)` where
  `a.g` is a struct member now writes back to the member's Mem location
  instead of a dead register.  Parser guards `TokenMember`/`TokenSubscriptExpr`
  from `ap_var` extraction.  Closes stdarg-2.

- **goto into dead `if(0)` conditional.** Labels inside `if(0)` blocks are
  now detected via `contains_label()` and the block is emitted as unreachable
  code so labels get bound.  Closes pr17078-1, vla-dealloc-1.

- **Swapped subscript `N[ptr]`.** `N[(char*)x]` where N is an integer and
  the index is a pointer is now detected and swapped to `ptr[N]`.
  Closes pr22061-1.

- **`**pp++` postfix increment.** Double-deref with postfix increment now
  correctly increments the innermost pointer variable via `TokenDerefStep`.
  Closes va-arg-21.

- **Stack bump-pool `__builtin_alloca`.** Replaced `alloca→malloc` mapping
  with a real stack-based bump allocator using `cc.newStack()`.  64KB pool
  per function, cursor in a stack slot (survives `setjmp`/`longjmp`).
  Closes pr64242.

- **setjmp buffer copy.** `__builtin_longjmp` from a `memcpy`'d buffer copy
  now works.  Magic sentinel in `buf[0..1]` validates the jmp_slot pointer.

- **Pre-compiled header infrastructure (.madh format).** Post-lexer token
  serialization with zlib compression.  `madc --emit-pch` mode pre-lexes
  headers, `scripts/gen_precompiled_headers.sh` batch-processes system
  headers via `gcc -E -P` preprocessing.  38 system headers embedded.
  Lookup chain: text-embedded → pre-compiled → filesystem.

- GCC parity: 1631 → 1649/1685 (96.8% → 97.9%). Integration tests: 452,
  all passing.

- **Scalar-to-vector SIMD arithmetic.** Inline `__attribute__((vector_size(N)))`
  now parses in declarations and compound literals. Float/double scalars
  splat correctly (was silently reinterpreting Xmm as Gp). Packed mul/div
  for all element sizes: byte via unpack-mul-repack, 64-bit via lane-by-lane
  imul, lane-by-lane div/idiv with remainder. Byte add/sub via paddb/psubb.
  Packed negation via psubd/subps. 8-byte SIMD uses movq instead of movaps.
  Global SIMD vectors init at parse time and use Mem-backed voperand.
  Mixed int/float mul guard now allows SIMD through emit_plain_binop3.
  Shift operators detect SIMD from either operand. Scalar-left bitwise
  ops compile as scalar with per-lane copy. Closes scal-to-vec{1,2,3},
  simd-{1,2,5}, pr23135.

- **Ternary function-pointer call.** `(c ? foo : bar)()` now dispatches
  via a generic expression-as-function-pointer path. The postfix `(`
  check restricts to function types to avoid breaking braceless-if bodies.
  Ternary void branches (e.g. `abort()`) skip the merge-slot store.
  Closes pr34768-{1,2}, pr46309.

- **Compound assignment evaluation order.** `x[0] |= foo()` now evaluates
  RHS before reading LHS value, matching GCC behavior. Closes pr58943.

- **Identity-cast destination store.** `(int)-4` no longer silently
  produces 0 — the operator_may_be_wider path now stores to the caller's
  destination when regdp.first is set. Cast expressions also evaluate
  in `literal_integer_value` for global constant initializers.
  Closes pr39240.

- **K&R function pointer calls.** `long (*f)()` with empty `()` (K&R
  unspecified params) now accepts any number of arguments. Closes pr67037.

- **String literal truncation.** `char a[2][3] = {"1234", "xyz"}` no
  longer overflows the first element into the second. Closes pr86714.

- GCC parity: 1598 → 1631/1685 (94.8% → 96.8%). Integration tests: 451,
  all passing.

- **SIMD array init + subscript + signed lane divmod.**
  Global arrays of SIMD vectors (`UV u[] = { ((UV){...}) }`) now write
  initializer data at parse time. Double-subscript on fixed-array-of-SIMD
  (`v[0][0]`) now resolves the lane element type correctly. Signed byte/
  short SIMD division and modulo now use `movsx`+`idiv` instead of
  `movzx`+`div`. Closes pr53645, pr53645-2, pr94524-1, pr94524-2.

- **Anonymous struct/union array member flag.** Members declared with
  `[]` inside anonymous structs/unions now correctly set the array-decl
  flag, fixing `&p->u.vec[16]` scale-factor computation from 8 to the
  actual element size. Closes pr41395-2.

- **`__attribute__((aligned(N)))` on struct members.** The alignment
  attribute on struct members is now extracted and applied via
  `apply_member_alignment()`, fixing struct size, alignment, and nested
  member offsets. Both `aligned` and `__aligned__` (dunder) forms are
  recognized. Closes pr23467, stkalign.

- **Bitwise AND with real destination.** `double d = i & 7` now computes
  the AND in integer and coerces the result to double, instead of
  performing a floating-point AND on raw bit patterns. Closes pr59643.

- **SIMD cast subscript parsing.** `((V8)x)[0]` no longer misparsed
  as a lambda — the subscript-on-expression check now matches SIMD
  cast types.

- **va_arg fixes.** Global `va_list` variables now write back the
  advanced pointer to backing storage. `va_arg(*pap, T)` through a
  pointer-to-va_list now correctly dereferences the pointer before
  reading/advancing. Closes pr64979.

- **`__builtin_*_overflow` unsigned result check.** Overflow detection
  for unsigned result types now correctly identifies negative infinite-
  precision results as overflow.

- **GCC integer/SIMD conversion parity advanced.**
  Division/modulo now use the natural C arithmetic type even when an
  outer destination requests a narrower signed result, unary `~` uses
  inferred operand types for nested operator expressions, narrow
  bitwise assignment stores through the LHS correctly, and explicit
  scalar-to-SIMD casts bitcast low scalar bytes instead of taking the
  arithmetic scalar-splat path. Small integer SIMD relational compares
  now route through lane-wise compare lowering, and SIMD `++` / `--`
  uses integer vector add/sub where appropriate. Closes `pr110817-1.c`,
  `pr110817-3.c`, `pr120630.c`, `pr19606.c`, `pr64682.c`,
  `pr123753.c`, `conversion.c`, and `20050316-1.c`, while preserving
  `pr109986.c`.

- **GNU SIMD/vector parity advanced.**
  Wide vector storage now stays memory-backed when it exceeds XMM width,
  including `__builtin_*_overflow` stores through vector-element
  pointers, lane-wise comparisons, bitwise ops, shifts, and by-value
  vector call arguments. `vector_size(...)` attributes now evaluate
  constant expressions such as `4 * sizeof(int)`, and small integer
  vectors use the same lane-wise `^`, `|`, `&`, and `~` handling.
  Closes `pr108292.c`, `pr109040.c`, `pr109938.c`, and `pr109986.c`
  plus related SIMD cases.

- **Array compound literals now parse and compile.**
  `(int []){0, 1, 2}` and `(int [3]){...}` now build a synthetic struct
  with N uniform elements, decay to a pointer in expression context, and
  support postfix subscripting. `&(type []){...}[i]` (address-of on a
  subscripted array compound literal) wraps the derived expression in
  `TokenAddrExpr`. Closes pr22098-{1,2,3}.c from the GCC torture suite.

- **`*func(args) = value` now assigns through dereferenced call return.**
  The unary-`*` dereference handler now explicitly parses function calls
  so trailing `=` stays in the outer expression. Closes pr60072.c.

- **C23 `[[...]]` attribute skip fixed.**
  The lexer now waits for a real `]]` pair instead of decrementing on
  each single `]`, and the parser skips any `[[...]]` before
  declarations. Unlocks 4 GCC torture tests.

- **GNU case range extension: `case LOW ... HIGH:`.**
  Switch cases now support range matching. The compiler emits two
  unsigned comparisons for the range check.

- **Inline asm fallback for unrecognized constraint patterns.**
  The asm handler previously only recognized `"+r"`, `"+m"`, `"=r"`,
  `"=m"`, and `"0"` constraints. Other patterns (`"+g"`, `"=m"` with
  trailing colons, multi-operand forms) consumed tokens past the asm
  statement, breaking subsequent code. The parser now uses paren-depth
  tracking to consume all remaining tokens when the constraint doesn't
  match a known shape. Closes pr40657.c, pr49390.c, pr65053-1.c,
  pr65053-2.c, pr88904.c.

- **GCC predefined macros expanded.**
  Added `__LDBL_MAX__`, `__LDBL_MIN__`, `__LDBL_EPSILON__`,
  `__FLT_MANT_DIG__`, `__DBL_MANT_DIG__`, `__LDBL_MANT_DIG__`,
  `__FLT_DIG__`, `__DBL_DIG__`, `__LDBL_DIG__`, `__ORDER_BIG_ENDIAN__`.
  `__GNUC__`, `__GNUC_MINOR__`, `__GNUC_PATCHLEVEL__`, and byte-order
  macros now reflect the actual build compiler via C preprocessor defines.

- **sizeof(expr) paren fix.** `sizeof(expr)` in the expression-fallback
  path no longer double-consumes the closing paren, fixing
  `if (sizeof(0LL) == sizeof(0U))` and similar comparisons.

- **Pointer-to-array declarations now parse.** `int (*a)[N]` is no
  longer misinterpreted as a function pointer. After `(*name)`, if `[`
  follows instead of `(`, the parser treats it as a pointer-to-array.

- **Switch case values widen to 64-bit.** Case constants exceeding
  32-bit range (e.g. `case 1000000000000000000ULL:`) now get the correct
  64-bit type instead of being truncated to 32-bit. Closes pr34154.c.

- **Postfix `++`/`--` now treated as value-producing for operator
  context.** `w++ - 3` was previously misparsed because `-` after `++`
  stayed as unary negation. `isPostfixPosition()` now recognizes `++`
  and `--` as value-producing. Closes pr93744-3.c.

- **Wide character support.** `L'x'` wide character literals, `wchar_t`
  typedef, `__WCHAR_MAX__` macro, `L"..."` wide string literal token
  metadata. Closes widechar-1.c, 20010325-1.c.

- **strlen family fixed.** Char-array pointer dereference chains,
  substring assignment through array-element pointers, multi-level
  string length computations. Closes strlen-2 through strlen-6.

- **Struct/compound literal fixes.** Zero-sized struct members, struct
  compound literal designator field lookup. Closes struct-ini-4.c,
  zero-struct-1.c, zero-struct-2.c.

- **Cast+call+shift.** `(unsigned long long)foo() << 32` correctly
  saves the first call result across the second call and applies the
  shift in 64-bit. `__builtin_choose_expr` implemented.

- **Multi-level dereference store.** `***f = 42` now correctly follows
  the pointer chain instead of treating the value as an address. The
  parser builds the dereference chain iteratively instead of recursively
  (which consumed the assignment operator). Closes pr97421-2.c.

- **Unsigned compound /= and %=.** `safediv` now receives operand types
  for compound assignments, selecting `div` vs `idiv` correctly.
  Closes pr69447.c.

- **Unsigned arithmetic operator type inference.** Arithmetic operators
  with unsigned natural type now infer their own type instead of
  accepting the caller's signed target. Bitwise operators always produce
  integer results even when the enclosing expression wants a double.
  Closes pr48197.c.

- **IEEE -0.0 signbit.** `__builtin_signbit` correctly detects -0.0.
  Closes pr35456.c.

- **va_arg accepts general expressions.** `va_arg(aps[4], long)` now
  parses the first argument as an expression rather than requiring a
  bare identifier. TokenVaArg carries the expression through to compile.

- GCC parity: 1543 → 1598/1685 (91.6% → 94.8%). Integration tests:
  421 → 451, all passing.

## [v0.20.0] - 2026-05-21

GCC parity crosses 91%: 1505 → 1536/1685 (89.3% → 91.2%). C++ std surface now namespace-owned, std::vector support, __builtin_*_overflow_p, inline asm, triple dereference, volatile token-paste.

- **GCC front-edge closures continued.**
  `__builtin_*_overflow_p` now dispatches through typed fixed-signature
  helpers instead of the unprototyped dynamic-symbol path, so the
  type-indicator argument once again selects the intended C width for
  `pr105777.c`. Variable declarations also now preserve the real token
  after post-declarator `__attribute__((...))` blocks, which restores
  GCC-style array declarations like `short a[4] __attribute__((aligned
  (16))) = { ... };` from `pr108064.c`. Added
  `tests/testbuiltinmuloverflowp.mad` and
  `tests/testalignedarrayattrinit.mad`. Full validation is green at
  `418/418` JIT and `418/418` EXE, and the current GCC floor is at
  least `1536/1685` (91.2%).

- **C++ std surface stays namespace-owned.**
  Embedded `<string>` now exposes `std::string` as the canonical type,
  and `<iostream>` keeps `std::cout`, `std::cin`, `std::cerr`, and
  `std::endl` under `std`. Bare `string` / stream names now require an
  explicit `using namespace std;` or `using std::<name>;`. Parser
  namespace-owned type handling now covers declarations, class members,
  lambda params / returns, container template arguments, and range-for
  declarations. Added embedded `<string>` plus
  `tests/teststdstringconv.mad`.
- **GCC front-edge closures continued.**
  `volatile` now remains a real qualifier token through macro
  token-paste, nested packed anonymous aggregate members preserve their
  layout attributes, nested variadic calls count only visible fixed
  parameters when packing `__va_args`, chained unary dereference handles
  `***p` forms, and output-only inline asm operands such as `"+m"` stop
  cleanly at the statement boundary. Focused GCC validation is green for
  `minmaxcmp-1.c`, `misalign.c`, `nest-stdar-1.c`, `pr103209.c`, and
  `pr103376.c`. Full validation is green at `410/410` JIT and
  `410/410` EXE; a live full GCC sweep reports `1514/1685` (89.9%) with
  one 5s timeout.

## [v0.19.0] - 2026-05-21

GCC parity push: 1496 → 1505/1685 (88.8% → 89.3%), `__builtin_frame_address`, stdio/string builtin aliases, pointer dereference typing fixes.

- **GCC builtins stdio and string-alias lane moved forward.**
  Fixed fixed-array pointer dereference typing for `const char *arr[]`
  shapes, added the `vprintf` packed-varargs bridge, and expanded
  GCC builtin aliases for unlocked stdio, `fputc` / `fwrite`,
  `mempcpy`, `index`, `strcspn`, `strspn`, and `rindex`. Fixed
  pointer arithmetic scaling for fixed arrays whose element type is
  itself a pointer, and kept postfix `++` / `--` from making a
  following `&` parse as unary address-of. Added
  `tests/testconstptrarrayderef.mad` and
  `tests/teststdiobuiltinredirects.mad`, plus
  `tests/teststrpbrklocal.mad`, `tests/testpostincbitand.mad`, and
  `tests/testgcclimitmacros.mad`.
  Focused GCC validation is
  green for `builtins/fprintf.c`, `printf.c`, `fputs.c`,
  `mempcpy.c`, `mempcpy-2.c`, `strchr.c`, `strcspn.c`, and
  `strspn.c`, plus `builtins/strpbrk.c`, `strrchr.c`, `strlen.c`,
  and `strlen-3.c`; full validation is green at `379/379` JIT and
  `379/379` EXE. A live full GCC sweep reports `1505/1685` (89.3%).

## [v0.18.0] - 2026-05-21

GCC parity push: 1327 → 1496/1685 (78.8% → 88.8%), _Complex arithmetic, IEEE floating-point, bitfield promotions, auto-include headers.

- **GCC torture `ieee/mzero2.c` now passes.**
  Floating-point unary negation now flips the sign bit directly
  instead of computing `0 - x`, which preserves `-0.0` during
  file-scope/static initialization and downstream IEEE divide/multiply
  semantics. Added `tests/testnegzerostatic.mad` as the local
  regression. Full validation is green at `370/370` JIT and
  `370/370` EXE, and the conservative GCC parity floor rises to at
  least `1504/1685` (89.3%).

- **GCC torture `921013-1.c` and `frame-address.c` now pass.**
  `*ptr++` now preserves real pointee types instead of forcing float
  dereferences through integer Gp temporaries, fixing float-equality
  stores like `*d++ = *x++ == *y++;`. `__builtin_frame_address(0)` is
  now registered as a builtin and lowered to the current stack frame
  address. Added `tests/testderefstepptrrealcmp.mad` and
  `tests/testbuiltinframeaddress.mad` as local regressions. Full
  validation is green at `372/372` JIT and `372/372` EXE, and the
  conservative GCC parity floor rises to at least `1506/1685` (89.4%).

- **GCC torture `ieee/fp-cmp-8f.c` now passes.**
  Contextual identifiers like `struct try` now parse correctly in
  struct-tag and typedef-alias positions, and indirect function-pointer
  calls now reset the expression type to the callee's real return type
  before outer comparisons/casts are lowered. Added
  `tests/teststructtrytag.mad` and `tests/testfnptrfloatretcmp.mad` as
  local regressions. Full validation is green at `369/369` JIT and
  `369/369` EXE, and the conservative GCC parity floor rises to at
  least `1503/1685` (89.2%).

- **GCC torture `ieee/hugeval.c`, `ieee/inf-1.c`, `ieee/inf-3.c`, and `ieee/inf-4.c` now pass.**
  The IEEE builtin macro surface now covers `__builtin_huge_val*`,
  `__builtin_isfinite*`, and `__builtin_isnan*`, and the embedded
  `math.h` surface now defines `HUGE_VAL` / `INFINITY` in terms of
  real infinity builtins instead of large finite literals. Added
  `tests/testieeehugeval.mad` as the local regression. Focused
  validation is green, and the conservative GCC parity floor rises to
  at least `1502/1685` (89.1%).

- **GCC torture `ieee/compare-fp-1.c` and `ieee/fp-cmp-1.c` now pass.**
  Real floating-point comparisons now honor IEEE unordered semantics:
  `==`, `!=`, `<`, and `<=` no longer treat NaN cases as ordered truths
  just because `ucomis*` set `ZF`/`CF`, and the IEEE builtin predicate
  family now has macro coverage for `__builtin_isunordered`,
  `__builtin_islessgreater`, and the matching `inf` / `nan` helpers
  those tests expect. Added `tests/testieeefpcompare.mad` as the local
  regression. Focused validation is green, and the conservative GCC
  parity floor rises to at least `1498/1685` (88.9%).

- **GCC torture `const-addr-expr-1.c` and `conversion.c` now pass.**
  Parser-side `->` validation now accepts pointer-shaped expressions
  produced by fixed-array decay and pointer arithmetic such as
  `&((array + 1)->field)`, and constant-folded integer operators now
  preserve their real unsigned result type instead of defaulting back
  to signed `int`. Real-to-`unsigned int` casts also now use an
  explicit unsigned-32 conversion path for values above `INT_MAX`,
  which restores GCC-matching results for cases like `(unsigned)(double)~0U`.
  Added `tests/testconstaddrexprarrow.mad` and extended
  `tests/testuint32realcoerce.mad` as local regressions. Full
  validation is green at `365/365` JIT and `365/365` EXE, and focused
  GCC reruns bring parity to at least `1496/1685` (88.8%).

- **GCC torture `builtin-prefetch-4.c`, `builtin-types-compatible-p.c`, and `compndlit-1.c` now pass.**
  GNU compound-literal designators now accept both `.field = value` and
  GNU `field: value` spellings, `__builtin_types_compatible_p(...)` now
  parses and compares real type signatures instead of hardwiring `0`,
  `__builtin_prefetch(...)` now preserves side effects in its address
  operand while remaining a no-op hint, and unsigned 32-bit to real
  coercions now zero-extend before `cvtsi2s{sd,ss}` so values above
  `INT_MAX` keep their correct magnitude. Added
  `tests/testcompoundlitgnudesignator.mad`,
  `tests/testbuiltintypescompatible.mad`,
  `tests/testbuiltinprefetcheffects.mad`, and
  `tests/testuint32realcoerce.mad` as local regressions. Full
  validation is green at `364/364` JIT and `364/364` EXE, and the
  latest full GCC sweep plus focused reruns bring parity to at least
  `1494/1685` (88.7%).

- **GCC torture `alias-1.c`, `bswap-3.c`, `built-in-setjmp.c`, and `builtin-bitops-1.c` now pass.**
  GNU attribute preservation now matches exact attribute identifiers
  instead of substring-matching words inside string arguments, so
  `optimize("-fno-strict-aliasing")` is skipped instead of being
  mistaken for `alias`. GCC byte-swap builtins now resolve through
  exported `__madc_bswap*` helpers; `__builtin_setjmp` emits a real
  JIT-side `_setjmp` with helper-owned `jmp_buf` storage; and the
  integer bit-operation builtins now cover the `int`, `long`, and
  `long long` lanes. Shift expressions now compute in the promoted left
  operand type before converting to the caller's target, which preserves
  unsigned 64-bit right shifts in int assignment/return contexts. Added
  `tests/testattributeoptimize.mad`, `tests/testbuiltinbswap.mad`,
  `tests/testbuiltinsetjmp.mad`, and `tests/testbuiltinbitops.mad` as
  local regressions. Full validation is green at `360/360` JIT and
  `360/360` EXE, and a full GCC sweep raises parity to `1491/1685`
  (88.5%).

- **GCC torture `bitfld-3.c` now passes.**
  Wide unsigned bitfield arithmetic now reduces results to the effective
  bitfield precision that GCC uses for operations on 33/40/41-bit fields,
  so mixed-width multiply/add/subtract expressions wrap at the wider
  participating bitfield width instead of leaking full `uint64_t`
  results. Added `tests/testbitfieldwidearith.mad` as the local
  regression. Full validation is green at `356/356` JIT and `356/356`
  EXE, and focused GCC reruns bring the conservative parity floor to at
  least `1432/1685` (85.0%).

- **GCC torture `bitfld-1.c` now passes.**
  Bitfield expressions now follow C's integer-promotion rules before
  arithmetic: narrow unsigned bitfields like `unsigned int u:7` promote
  to `int` until an explicit cast forces `unsigned int`, which restores
  the correct signed-vs-unsigned remainder behavior in mixed bitfield
  expressions. Added `tests/testbitfieldpromote.mad` as the local
  regression. Full validation is green at `355/355` JIT and `355/355`
  EXE, and focused GCC reruns bring the conservative parity floor to at
  least `1431/1685` (84.9%).

- **GCC torture `arith-rand-ll.c` now passes.**
  The parser's ternary-type fallback no longer treats real `int64_t` /
  `long long` branches as if they were the generic default `int` case,
  so expressions like `(unsigned long long)(yy >= 0 ? yy : -yy)` keep
  their full 64-bit width instead of collapsing to 32 bits through the
  false branch. Added `tests/testternaryllcast.mad` as the local
  regression. Full validation is green at `354/354` JIT and `354/354`
  EXE, and focused GCC reruns bring the conservative parity floor to at
  least `1430/1685` (84.9%).

- **GCC torture `align-3.c` and `align-nest.c` now pass.**
  Function declarations now preserve GNU `__attribute__((aligned(N)))`
  as an explicit function alignment override, so `__alignof__(func)`
  reports the declared function alignment instead of the generic
  function-type fallback. Cleanup for stack-backed runtime-sized
  aggregates also now skips fixed arrays of those aggregates, which
  avoids freeing stack storage in cases like packed/aligned local arrays
  of VLA-sized structs. Added `tests/testfunctionalignof.mad` as the
  local regression. Full validation is green at `353/353` JIT and
  `353/353` EXE, and focused GCC reruns bring the conservative parity
  floor to at least `1429/1685` (84.8%).

- **GCC torture `alias-3.c` now passes.**
  File-scope `extern` aliases that resolve to real global storage now
  clear the temporary stack-backed flag inherited from the non-allocating
  declaration path, so scalar alias writes like `b++` lower through the
  global load/store path instead of a bogus local stack slot. Added
  `tests/testglobalaliasscalar.mad` as the local regression. Full
  validation is green at `352/352` JIT and `352/352` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1427/1685` (84.7%).

- **GCC torture `alias-2.c` now passes.**
  GNU `__attribute__((alias("target")))` now survives lexing on
  declarators, the parser records global storage aliases, and both JIT
  and AOT global-address resolution now follow that alias to the
  canonical backing storage instead of creating a distinct global slot.
  Added `tests/testglobalaliasarray.mad` as the local regression. Full
  validation is green at `351/351` JIT and `351/351` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1426/1685` (84.6%).

- **GCC torture `931004-11.c` and `931004-12.c` now pass.**
  Local fixed-size arrays now allocate stack slots using the element
  type's actual alignment instead of the raw element size, which fixes
  odd-sized struct arrays like `struct tiny x[3]` where `newStack(..., 3)`
  was producing invalid stack slots before direct and varargs struct-by-
  value calls. Added `tests/testsmallstructarraycall.mad` as the local
  regression. Full validation is green at `342/342` JIT and `342/342`
  EXE, and focused GCC reruns bring the conservative parity floor to at
  least `1417/1685` (84.1%).

- **GCC torture `921007-1.c`, `921016-1.c`, `921019-1.c`, and `930628-1.c` now pass.**
  The lexer now breaks the `strcmp` / `__builtin_strcmp` macro-expansion
  cycle at the builtin alias boundary, typedef-enum / alias bitfield
  extraction no longer wrongly forces signed fixed-width aliases like
  `int32_t` through the unsigned path, fixed-array struct members now
  preserve multidimensional shape for decay/subscript lowering, and
  native EXE global pointer initializers can now materialize constant
  string-subscript addresses like `(void *)&("X"[0])` directly in copied
  `.data` instead of relying on runtime `string_cstr` setup. Added
  `tests/testbuiltinstrcmpmacrocycle.mad`,
  `tests/testsignedbitfieldassignexpr.mad`,
  `tests/teststrlitaddrsubscriptglobal.mad`, and
  `tests/teststructmembermultidimdecay.mad` as regressions. Full
  validation is green at `341/341` JIT and `341/341` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1415/1685` (84.0%).

- **GCC torture `20230630-2.c` and `20230630-4.c` now pass.**
  `__attribute__((scalar_storage_order(...)))` now survives lexing as a
  layout-affecting attribute, struct parsing records reversed scalar
  storage order for bit-field aggregates, and bit-field load/store
  lowering now byte-swaps multi-byte storage units when the requested
  scalar storage order differs from the host endianness. Full
  validation is green at `337/337` JIT and `337/337` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1411/1685` (83.7%).

- **GCC torture `eeprof-1.c` now passes.**
  `scripts/run_gcc_testsuite.py` now respects the testsuite's
  `dg-options "-finstrument-functions"` lane by forwarding
  `--finstrument-functions` into madc, the CLI/compiler now support
  `--finstrument-functions` directly, and GNU
  `__attribute__((no_instrument_function))` now survives lexing/parsing
  so instrumentation hooks skip the expected profiling helpers. Added a
  generic `tests/foo.flags` fixture convention to `scripts/run_tests.sh`
  and `tests/testfinstrumentfunctions.mad` as the local regression. Full
  validation is green at `330/330` JIT and `330/330` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1409/1685` (83.6%).

- **GCC torture `pr93434.c` now passes.**
  Fixed-array struct assignment now scales write-side indices with the
  same full element stride as the read path, so copies like
  `t2[i] = t2[k]` land on the correct element for 16-byte struct array
  members instead of writing at raw byte offsets. Added
  `tests/testfixedarraystructcopy.mad` as the regression. Full
  validation is green at `329/329` JIT and `329/329` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1408/1685` (83.6%).

- **Embedded standard headers now auto-include on first use of common names.**
  Unresolved identifiers such as `size_t`, `intptr_t`, `DBL_MIN`, and
  `FLT_RADIX` now trigger tokenization of the owning embedded header in
  the lexer, so typedefs and macros come from the embedded header
  surface instead of a parallel ambient-builtin path. Added
  `tests/testautoincludestdheaders.mad` as the regression. Full
  validation is green at `328/328` JIT and `328/328` EXE.

- **GCC torture `960512-1.c` now passes.**
  The lexer now keeps compound type-specifier accumulation alive across
  line breaks, so split declarations like `__complex__` newline
  `double f(void)` parse as a single `_Complex double` type instead of
  prematurely defaulting to plain `double _Complex`. Complex conditions
  now also lower to a real boolean by testing whether either real or
  imaginary lane is non-zero, which fixes `if (c = f())` truthiness for
  complex assignment expressions. Added
  `tests/testcomplexsplitdeclcond.mad` as the regression. Full
  validation is green at `323/323` JIT and `323/323` EXE, and focused
  GCC reruns bring the conservative parity floor to at least
  `1407/1685` (83.5%).

- **GCC torture `pr49644.c` and `pr104604.c` now pass.**
  Complex `*`, `/`, `*=`, and `/=` now lower component-wise through the
  compiler's internal complex pair representation, and pointer-aware
  normalization no longer strips fixed-array decay off complex-typed
  expressions before compare/cast lowering. That closes the
  `_Complex double a[12], *c = a; if (c != a + 6)` pointer-arithmetic
  lane and the `_Complex unsigned t = 42; t /= c; return v + t;`
  unsigned complex division lane. Added
  `tests/testcomplexptrcmpdecay.mad` and
  `tests/testcomplexunsigneddiveq.mad` as regressions. Full validation
  is green at `322/322` JIT and `322/322` EXE, and focused GCC reruns
  bring the conservative parity floor to at least `1406/1685` (83.4%).

- **GCC torture `20020227-1.c` now passes.**
  Scalar assignment into packed complex struct members now zeroes the
  imaginary lane via an immediate store instead of routing the zero
  through a register-typed float value, which had been duplicating the
  real lane in packed layouts like `struct { char c; _Complex float f; }`.
  Full validation remains green at `320/320` JIT and `320/320` EXE, and
  focused GCC reruns bring the conservative parity floor to at least
  `1404/1685` (83.3%).

- **GCC torture `pr56837.c` now passes.**
  Complex subscript assignment no longer forces scalar RHS values
  through a bogus `regdp.second = complex_type` path before the
  dedicated complex-element store logic runs, so `_Complex int a[i];
  a[i] = -1;` now lowers cleanly. Full validation remains green at
  `320/320` JIT and `320/320` EXE, and focused GCC reruns bring the
  conservative parity floor to at least `1403/1685` (83.3%).

- **The `_Complex` arithmetic / builtin lane moved forward again.**
  Pure-imaginary literals like `1.0i`, `2.2if`, and `10iL` now
  materialize as real complex values instead of collapsing to scalars,
  scalar-to-complex casts now build the internal complex pair form,
  `~x` on complex values lowers as conjugation, and complex `+`, `-`,
  `+=`, and `-=` now run component-wise with width coercion instead of
  raw-copying mismatched layouts. `__builtin_conj[f|l]` / `conj[f|l]`
  also route through a builtin conjugate path, old-style forward
  declarations like `void foo(), bar(), baz();` now rebuild cleanly
  when the complex-typed definitions arrive later, and
  `tests/testcomplexkw.expect` now matches GCC's result instead of the
  old broken behavior. Added `tests/testcompleximagadd.mad`,
  `tests/testcomplexconjop.mad`, `tests/testcomplexaddeq.mad`,
  `tests/testbuiltinconjf.mad`, and `tests/testcomplexfwddeclparams.mad`
  as regressions. Validation is green at `320/320` JIT and `320/320`
  EXE, and focused GCC reruns confirm at least `1402/1685` (83.2%)
  parity pending a fresh full-suite pass.

- **GCC torture `20070614-1.c` now passes.**
  Complex-by-value call lowering now repacks scalar expressions passed
  to `_Complex` parameters into the compiler's internal complex pair
  storage before argument validation and call setup, which closes the
  pure-imaginary multiply call lane behind `bar (1.0iF * i)`. Full
  validation remains green at `315/315` JIT, `315/315` EXE, and GCC
  parity is now `1397/1685` (82.9%).

- **GCC torture `20050121-1.c` now passes.**
  `_Complex` / `__complex__` declarations now preserve a real internal
  complex pair type instead of collapsing immediately to scalar storage,
  GNU `__real__` / `__imag__` now parse as component expressions,
  complex-to-scalar casts route through the real lane, equality / `!=`
  compare complex values component-wise without double-evaluating their
  operands, and `&(__real expr)` now lowers as an addressable lvalue.
  The branch-local complex regressions `tests/testcomplexkw.mad`,
  `tests/testcomplexushort.mad`, and `tests/testcomplexrealaddr.mad`
  are green, integration / EXE remain `315/315`, and GCC parity is now
  `1344/1685` (79.8%).

- **GCC torture `20030222-1.c`, `20030714-1.c`, `20030928-1.c`, `20061220-1.c`, `20080424-1.c`, and `20080519-1.c` now pass.**
  Typedef enum aliases now preserve their alias spelling well enough for
  narrow unsigned bitfield extraction heuristics, nested local function
  definitions now mangle their internal symbol names so repeated
  `nested` / `nested2` bodies do not collide across outer functions, the
  inline-asm parser now safely treats GCC's empty-template `"=m"` /
  `"m"` barrier form as a no-op, and struct-by-value call-argument
  copying is constrained back to plain C structs instead of raw-copying
  runtime class objects like `string` and `array`. Added
  `tests/testenumbitfieldalias.mad` and `tests/testnestedasmbarrier.mad`
  as regressions, and refreshed integration / EXE counts to `315/315`
  plus GCC parity to `1343/1685` (79.7%).

- **GCC torture `20040520-1.c` and `930406-1.c` now pass.**
  Ordinary nested function definitions now get an explicit environment
  parameter/capture plumbing path instead of only the lambda-only
  closure lane, and parser statement handling now accepts local
  `__label__` declarations as a GNU extension. The remaining `_Complex`
  work is still intentionally out of this diff.

- **GCC torture `20040411-1.c`, `20040423-1.c`, and `20041218-2.c` now pass.**
  Typedef'd arrays now preserve their full array shape instead of
  collapsing immediately to pointers, `sizeof(type)` can now materialize
  runtime VLA-backed typedef and aggregate extents, packed runtime-sized
  struct layout no longer double-counts dynamic members in its fixed
  base size, and stack-backed SIMD parameters now spill through the SIMD
  store helper instead of the scalar IR store path so upper lanes survive
  function entry intact. Added `tests/testtypedefvlasizeof.mad` and
  `tests/testgccvectorcasts.mad` as regressions.

- **GCC torture `20050604-1.c` and `20050607-1.c` now pass.**
  GNU `vector_size(...)` attributes now survive lexing and attach to
  typedef aliases as real SIMD datadefs, SIMD compound literals now
  materialize stack/global backing storage and preserve XMM values for
  vector arithmetic, and integer/float vector `+=` now lower through
  `paddw` / `addps` instead of falling back into scalar conversion
  paths. Casting a vector literal to `long long` still preserves the
  low-qword shape needed by `20050607-1.c`. Added
  `tests/testgccvectorlit.mad` as the regression.

- **GCC torture `20050502-1.c` now passes.**
  Unary `*(`...`)` parsing now honors trailing postfix `++/--`, so
  `*(*x)++` lowers as dereference-of-old-pointer instead of collapsing
  into `(*x)++`. Pointer-valued dereferences also now materialize as
  memory loads instead of treating the pointer slot address as the
  pointee value, and the postfix deref increment fast path is narrowed
  back to the real `*(*p++)++` shape. Added
  `tests/testderefpostincread.mad` as the regression.

- **GCC torture `20041124-1.c` now passes.**
  `_Complex` / `__complex__` now participate in the lexer's normal
  type-specifier accumulator instead of being force-rewritten to
  `double`, so declarations like `_Complex unsigned short` keep the base
  type's own width. The imaginary-literal compatibility lane now also
  accepts bare integer suffixes like `200i` in addition to float forms
  like `1.0iF`. Added `tests/testcomplexushort.mad` as the regression.

- **GCC torture `930513-1.c` now passes.**
  K&R old-style parameter declarations now accept function-pointer
  parameters with explicit prototypes and varargs, `&name` can late-bind
  allowed extern function symbols at parse time, indirect varargs calls
  now preserve their fixed-argument boundary and variadic ABI setup, and
  native EXE emission now records external function-address `movabs`
  loads as real `R_X86_64_64` relocations instead of freezing host
  process addresses into the binary. Added
  `tests/testkrfnptrvarargs.mad` as the regression.

- **GCC torture `20010122-1.c` now passes.**
  Function-pointer array elements now stay callable after subscript
  parsing, so `funcs[i]()` and similar shapes lower through the existing
  indirect-call path instead of collapsing to the raw function pointer
  value. Pointer-returning indirect calls now also treat `void *` as a
  real return value instead of dropping the `RAX` bind because its raw
  pointee type is `void`. Added `tests/testfnptrarraycall.mad` as the
  regression.

- **GCC torture `20070919-1.c` now passes.**
  Local block-scope struct tags can now shadow earlier tags in the GCC
  parity lane, struct members can carry runtime-sized array counts, and
  the compiler now uses runtime aggregate sizes for local struct backing
  storage, pointer subscript scaling, and aggregate copies. The struct
  initializer path also now accepts array-of-struct string shorthand like
  `{ "abcdefg", ... }` when the element is a single char-array member.
  Added `tests/testvlastructmember.mad` as the regression.

- **GCC torture `20070614-1.c` now passes.**
  The lexer now lowers `_Complex` / `__complex__` declarations to a
  temporary `double` compatibility path and accepts `i`/`j` imaginary
  literal suffixes like `1.0iF` on the GCC parity lane. This is still
  not full complex-number semantics, but it closes the compile/runtime
  path for the current torture case and adds `tests/testcomplexkw.mad`
  as a regression.

- **GCC torture `20060420-1.c` now passes.**
  Expression-base subscript assignment now stores through IR instead of
  assuming a Gp RHS, fixed-array subscript reads/writes keep stack-backed
  arrays as addresses via `lea`, and pointer/fixed-array casts bypass the
  float-conversion path so `(char *)buffer` / `(long)buffer` preserve real
  address bits. Added `tests/testglobalarrayptrcastloop.mad` as the
  regression.

- **Native EXE file-scope compound literals now relocate nested pointers correctly.**
  AOT/native executable emission now routes file-scope compound-literal
  storage through the discovered-data relocation path and also patches
  pointer-valued fields inside copied `.data` payloads. This fixes
  `tests/testcompoundlitglobalptr.mad` in EXE mode instead of only in JIT.

- **Function-pointer array declarations now parse as plain C.**
  Declarations like `void *(*funcs[3])(void)` now flow through the
  normal declaration/initializer path instead of failing at the `)`
  after the name.

- **Aggregate attribute parser now accepts repeated `__attribute`.**
  `struct ... } __attribute((packed)) __attribute((aligned));` now stays
  attached to the aggregate definition instead of leaking into later
  parser stages. GCC torture `packed-aligned.c` now passes.

- **Validation/docs sync for GCC-first mode.**
  Added `tests/testfnptrarray.mad` as a regression for function-pointer
  array declarations, `tests/teststmtexprmember.mad` for GNU statement-
  expression member access, and `tests/testnestedstructflatinit.mad` for
  flat nested-struct initializers. Added
  `tests/testcompoundlitglobalptr.mad` for native file-scope
  compound-literal relocation and `tests/testglobalarrayptrcastloop.mad`
  for the `20060420-1.c` address-cast loop shape, plus
  `tests/testcomplexkw.mad` for the `_Complex` / `iF` compatibility lane,
  `tests/testfnptrarraycall.mad` for indirect calls through
  function-pointer array elements, `tests/testkrfnptrvarargs.mad`
  for K&R-declared varargs function pointers, and
  `tests/testcomplexushort.mad` for `_Complex unsigned short` plus
  integer imaginary-suffix compatibility, plus
  `tests/testcomputedgoto.mad` for GNU computed goto and
  `tests/testderefpostincread.mad` for the `20050502-1.c` deref-postinc
  read shape.
  Removed the `std::list` expectation from `tests/testmadc_ns.mad`,
  refreshed the current integration / EXE pass counts to `306/306`, and
  refreshed GCC torture parity to `1337/1685` (79.3%).

## [v0.17.0] - 2026-05-19

GCC parity push: 1305 → 1316/1685 (78.1%), cast chain fixes, struct init, preprocessor features.

- **Local variable zero-init at function entry.**
  Stack-slot zero-init now emitted at function prologue via `prologue_cursor`
  instead of at first-use. Fixes variables modified in one loop branch
  being re-zeroed on the next iteration when the first reference was
  inside a conditional.

- **Empty struct/union brace-init (`struct X x = {}`).**
  Direct qword zero-fill instead of the assignment path which self-copied
  (source == destination pointed to same stack slot).

- **Constant-fold register width matches semantic type.**
  `optimize()` now uses `regdp.second->newreg()` instead of hardcoded
  `newGpq()`. Fixes `~0U` (32-bit) comparisons failing against 32-bit
  variables due to 64-bit vs 32-bit register mismatch.

- **`#pragma push_macro` / `pop_macro` support.**
  Save and restore individual `#define` macro definitions. Per-macro-name
  stack with sentinel for "macro was undefined".

- **`f().member` — dot access on struct-returning function calls.**
  `TokenCallFunc` is now an allowed LHS for dot-member access in
  `parseExpression`. The function call becomes `parent_expr` of the
  resulting `TokenMember`.

- **`list` removed from keyword map.**
  `list` no longer shadows C identifiers. `char *list; *list` now
  parses correctly. Use `std::list<T>` for the container type.

- **Nested integer cast chains: `(long long)(int)x`.**
  The generic cast fallback now compiles inner expressions with a fresh
  `regdp` so nested narrowing casts produce independent results. Widening
  step added when inner produces a narrower register than the outer target.

- **TokenCast: proper real↔integer and narrowing cast codegen.**
  Float→int, int→float, and integer-narrowing casts now emit correct
  conversion instructions instead of falling through to the
  "reinterpret" path. Fixes double `cvtsi2ss` when `(int)flt_expr`
  appeared inside float arithmetic, and `(unsigned char)-10` keeping
  the full 64-bit value.

- **Narrow-integer promotion for assignments and compound ops.**
  `x = x / -5` where `x` is `unsigned char` now computes the division
  in `int` (C integer promotion), not in unsigned char where `-5`
  wraps to 251. Compound ops (`/=`, `+=`, etc.) promote similarly.

- **L/LL integer literal suffix selects 64-bit type.**
  `1ULL` now has type `uint64_t` (was `uint32_t`). `1L`/`1LL` →
  `int64_t`. Fixes `(int)(-1ULL >> 15)` producing 131071 instead of -1.

- **`sizeof("string literal")` returns char-array size.**
  Parenthesized form now returns `strlen(s)+1` (was `sizeof(std::string)=32`).

- **Static array init with negative values.**
  `literal_integer_value` now handles unary minus (`-N`), plus (`+N`),
  and bitwise NOT (`~N`). Fixes `static int arr[] = {1, -1}` leaving
  negative elements as zero.

- **Forward-declaration return type refresh.**
  When a function is forward-declared with one return type and later
  defined with another, the FuncDef is replaced with the correct type.

- **Preprocessor: `#if`/`#elif` macro expansion.**
  `expandIfMacros()` iteratively replaces define names in `#if`
  conditions before evaluation. Fixes OpenSSL `macros.h` checks.

- **Preprocessor: `#ifdef`/`#else`/`#endif` inside macro arguments.**
  Conditional directives inside function-like macro arg lists are now
  processed (GCC extension). Fixes RoD `act_obj.c` `ch_printf` call.

- **Preprocessor: backslash-newline line splicing.**
  `\` + optional trailing whitespace + newline joins physical lines
  at the `Source::get()`/`peek()` level.

- **Preprocessor: multi-line function-like macro calls.**
  `(` on the next line after a macro name is now recognized.

- **`#include <>` searches system paths + `-I` flag.**
  Angle-bracket includes search `-I` paths, then `/usr/local/include`,
  `/usr/include`, `/usr/include/x86_64-linux-gnu`. Quoted includes
  search source dir then `-I` paths. Adds `-I`/`-Ipath` CLI flag.

- **Duplicate typedef redeclarations accepted.**
  `typedef struct foo FOO;` repeated is now silently accepted (C std).

- **Embedded headers: `strings.h`, `malloc.h`, `sys/vfs.h`, `resolv.h`.**

- **Lexer: EOF without trailing newline.**
  Files ending without `\n` no longer produce a spurious TokenChar(-1).

- **Lexer: angle-bracket include fallback to source directory.**
  `#include <file.h>` now searches the current source directory as last
  resort, fixing local header copies like libpq-fe.h → postgres_ext.h.

- **Lexer: octal integer literal support.**
  `010` now correctly parses as 8 (octal), not 10 (decimal).

- **Preprocessor: nested function-like macro expansion.**
  Macro arguments are now pre-expanded before substitution (C standard
  behavior). Fixes `UMIN(x, UMIN(y, z))` and similar nested calls.

- **Parser: subscript on generic pointer expressions.**
  `NAME(ch)[0]` where NAME expands to a ternary now subscripts correctly.

- **Parser: `const`/`restrict` in cast expressions.**
  `(const OBJ_DATA * const *)expr` and `(char)CONST` in case labels.

- **Parser: cast-expression tightness.**
  `(unsigned char)~0 * ' '` now parses as `((unsigned char)(~0)) * ' '`,
  not `(unsigned char)(~0 * ' ')`.

- **Parser: unary `+` after assignment.**
  `x = +20` no longer fails with "Missing operand".

- **Parser: `sizeof` / `parsePostfixChain` for keyword identifiers.**
  `sizeof(class->member)` works when `class` is used as a C identifier.

- **Compiler: `*(*p)++ = value` assignment.**
  Post-incremented dereference as assignment LHS now captures old pointer,
  stores RHS, then increments. Matches GCC codegen.

- **Compiler: asmjit 32-arg limit.**
  FuncSignature/InvokeNode capped at 32 args. Varargs calls with many
  format arguments no longer abort.

- **Compiler: static struct/array brace-init.**
  Local `static` variables with brace initializers are now initialized
  at first entry, not left zeroed.

- **Compiler: unsigned int64 → double/float (GCC pattern).**
  `(double)UINT64_MAX` now gives ~1.84e19, not -1.0. Uses the
  test/jns/shr/or/cvtsi2sd/addsd pattern matching GCC.

- **Compiler: 32-bit unsigned comparison.**
  `int` vs `unsigned int` comparisons now truncate to 32 bits before
  the unsigned compare. UINT_MAX in limits.h now has the U suffix.

- **Typesafe: SAR for signed right-shift.**
  `safeshr` now uses SAR (arithmetic shift) for signed types instead
  of SHR (logical shift). Fixes `-2147483648 >> 1` producing a
  positive number.

- **Embedded headers: `struct passwd` in `pwd.h`, `intptr_t`/`uintptr_t`,
  `size_t` in `stdio.h`.**

- **GNU named variadic macro parameter expansion.**
  `#define test(ret, args...) fprintf(stdout, args)` now joins all
  trailing call-site arguments into the named variadic parameter.

- **Float-precision negation and comparison.**
  `safeneg()` uses `subss`/`xorps` for float (was `subsd`/`xorpd`).
  `safecmp()` uses `ucomiss` for float comparisons. Fixes `-1.0f`
  producing `1.0f` and float comparison wrong answers.

- **C usual-arithmetic float promotion.**
  `infer_numeric_type()` returns the actual floating type (float or
  double) instead of always `ddDOUBLE`. Matches GCC's usual arithmetic
  conversions for int-vs-float expressions.

- **Function return type always set in regdp.**
  `TokenCallFunc::compile()` now unconditionally sets `regdp.second`
  to the function's actual return type, fixing float→double coercion
  when comparing float function returns directly.

- **`ddINT = ddINT32`: LP64 ABI completion at the type-system level.**
  `dtINT`, `dtINTptr`, `dtINTref` now alias 32-bit variants. Unsuffixed
  integer literals, unqualified `int` variables, and `TokenOperator`
  default types are all 4 bytes. `dtFLOAT` explicitly pinned to avoid
  enum collision.

- **Integer cast compiles inner at natural width.**
  `TokenCast` for sub-64-bit integer targets compiles the inner
  expression with NULL type when it detects an operator whose datadef
  may underreport width. Fixes `(int)(LL_expr >> n)` truncating the
  LL literal before the shift.

- **Signed→unsigned same-size cast truncation.**
  `(unsigned int)(signed int)x` now masks the upper 32 bits via
  `canonicalize_narrow_integer_reg`. IR coerce handles signed→unsigned
  same-size conversions.

- **`unsigned char *` treated as charptr for string coercion.**
  `(unsigned char *)"str"` now produces a valid c_str pointer instead
  of garbage.

- **Integer literal source text preserved through macro arg pre-expansion.**
  `TokenInt` now carries `source_text` so hex representation and L/U/LL
  suffixes survive the tokenize→reserialize round-trip during
  function-like macro argument pre-expansion. Fixes `sizeof()` on
  macro-substituted literals like `0x12345678LU` returning 4 instead of 8.

- **Unsigned char/short use signed comparison after integer promotion.**
  C usual arithmetic conversions: `unsigned char` and `unsigned short`
  fit in `int`, so relational comparisons use signed `setl`/`setg`
  instead of unsigned `setb`/`seta`. Only types >= `sizeof(int)` force
  unsigned comparison.

- **Float arithmetic in integer-destination context.**
  `add`/`sub`/`mul`/`div` now skip the plain-binop fast path when
  the destination type is integer but operands are float, preventing
  float literals from being truncated to int before the operation.

- **Integer→real cast compiles inner expression at natural type.**
  The `(float)expr` / `(double)expr` cast no longer forces the inner
  expression's `regdp.second` to the pre-inferred signed type. Unsigned
  operators like `>>` now correctly use SHR instead of SAR when the
  source operand is unsigned.

- **32-bit registers (Gpd) for int/unsigned int types.**
  `DataDef::newreg()` and `IRBuilder::newReg()` now return `newGpd()`
  for 32-bit integer types. On x86-64, 32-bit ops automatically
  zero-extend to 64 bits, giving correct wrapping at 2^32 for free.
  All typesafe arithmetic helpers (`safemul`, `safeor`, `safeand`,
  `safexor`, `safeshr`, `safediv`) dispatch between r32/r64 forms.
  Matches GCC's code shape (`addl`, `imull`, `shll` instead of `addq`).

- **Cast operand binds tightly over simple literals.**
  `(double)5 < 3.0` now parses as `((double)5) < 3.0` instead of
  `(double)(5 < 3.0)`.

- **C integer literal type rules for hex/octal without suffix.**
  Hex constant `0x80000081` now typed as `unsigned int` per C §6.4.4.1.

- **Unsigned `div` for unsigned integer division/modulo.**
  `safediv()` now uses the x86 `div` instruction (unsigned) instead of
  `idiv` (signed) for unsigned operands.

- **Real→integer assignment for narrow int types.**
  `unsigned short s = double_expr` now converts via `cvttsd2si` instead
  of storing raw double bits.

## [v0.16.0] - 2026-05-18

sizeof(int) = 4: LP64 ABI alignment and GCC torture suite 75% milestone.

- **`sizeof(int)` = 4 bytes, matching GCC and the LP64 ABI.**
  The bitmap type-specifier accumulator now maps `int` to `ddINT32`
  (4 bytes) and `unsigned int` to `ddUINT32`. `long` remains 8 bytes.
  All type sizes now match GCC on x86-64 (except `long double`, which
  is 8 bytes in madc vs 16 in GCC).

- **Codegen fixes for 4-byte int.**
  Integer stack slots allocate at least 8 bytes for safe 64-bit register
  writes. IR narrow-integer canonicalization extended in-place (reusing
  the same vreg) to avoid confusing asmjit's register allocator. 32-bit
  shift operations (`safeshl`) now use `shl r32` so results wrap at
  the correct width.

- **Float/double brace initializers in arrays and struct members.**
  `TokenReal::compile` now emits `newFloatConst` when the target type
  is float-sized, and init-store paths handle Xmm (Vec) registers via
  `movss`/`movsd` instead of assuming all values are in Gp registers.

- **Removed scanf `%d` → `%ld` format-rewriting shim.**
  With `sizeof(int)` = 4, libc's `%d` writes the correct 4 bytes into
  a standard int slot. The `__madc_sscanf`/`__madc_fscanf` wrappers
  now pass through to the real libc functions.

- **`__builtin_add/sub/mul_overflow` and `_overflow_p` predicates.**
  Overflow-checking arithmetic builtins implemented via `__int128`
  helper functions in `va_helpers.cpp`.

- **Ternary operator in constant expressions.**
  `sizeof(int) >= 4 ? 0x4000 : 4` now works in array dimensions and
  other compile-time contexts.

- **C23 `[[attribute]]` consumption.**
  Double-bracket attributes (`[[gnu::noipa]]`, `[[nodiscard]]`, etc.)
  are now consumed and skipped at the lexer level.

- **Unary `+` operator support.**
  `f(+1)` and `+42` are now parsed as no-op unary `+`.

- **Zero-length arrays: `sizeof(arr[0])` = 0.**
  `int arr[0]` now has sizeof 0, matching GCC.

- **Embedded headers: `stddef.h` and `assert.h` now registered.**

- **`__builtin_bswap64` mapped to helper function.**

- **Build: `make -C src` now builds `lib/libmadc.so` alongside `bin/madc`.**

- **Expression sandbox: comma-operator rejection for `eval_expression`.**
  `(1, 2)` is now rejected by the pre-lex text scanner, while commas
  inside function-call parentheses are still allowed.

## [v0.15.0] - 2026-05-17

GCC torture test suite parity initiative: pass rate 627 → 1245 (37% → 74%).

- **GCC torture test suite runner and 96 compile-failure fixes.**
  New `scripts/run_gcc_testsuite.py` drives GCC's `gcc.c-torture/execute`
  tests through madc. 24 commits across parser, lexer, compiler, and
  typesafe took the pass rate from 627/1685 to 1245/1685, closing 96
  compile failures. Compile failures dropped from 320 to 221; 22 newly
  compilable tests now hit runtime issues instead.

- **C comma operator in parenthesized expressions.**
  `(expr1, expr2)` now evaluates `expr1` for side effects, discards it,
  and returns `expr2`. Works inside `if()` conditions, assignments, and
  nested expressions.

- **Full C operator precedence in the `#if` preprocessor evaluator.**
  The `#if` condition evaluator now uses integer arithmetic (was boolean)
  with the full C precedence chain: unary → multiplicative → additive →
  shift → relational → equality → bitwise AND/XOR/OR → logical AND/OR.
  Also adds `#error` and `#warning` directives.

- **Scientific notation in float literals.**
  Both `1.5e-3` and `1e5` (no decimal point) now parse correctly.
  Exponent signs (+/-) and float suffixes (f/F/l/L) are handled.

- **Mixed int*double arithmetic promotion (C99 6.3.1.8).**
  When one operand of `*` is integer and the other is floating-point,
  the integer is promoted to double for the computation and the result
  is truncated back to int if the destination requires it. Re-implements
  the lost RoD mixed-arithmetic fix.

- **K&R empty-parens functions: `f()` vs `f(void)`.**
  Added `is_void_params` flag to `FuncDef`. Functions declared with
  empty parens accept any number of arguments; `f(void)` means exactly
  zero. Applies to both regular functions and function pointers.

- **Zero-length arrays and flexible array members.**
  `int arr[0]` and `int arr[]` in struct members are now accepted.
  `int arr[0]` in variable declarations is treated as a 1-element
  placeholder.

- **Bitfields in anonymous struct/union members.**
  Named bitfields (`int x : 4;`), unnamed bitfields (`int : 4;`), and
  comma-separated members (`int f1, f2, f3;`) now work inside anonymous
  struct/union bodies.

- **Forward enum references.**
  `enum X var;` where X was previously defined is now treated as `int`.

- **Embedded headers: `stddef.h`, `assert.h`.**
  `stddef.h` provides `size_t`, `ptrdiff_t`, `wchar_t`, `NULL`,
  `offsetof`. `assert.h` provides the `assert()` macro. `NULL` also
  added to `stdlib.h` and `string.h`.

- **50+ GCC `__builtin_*` aliases and type macros.**
  Covers math functions (sqrt, sin, cos, pow, fma, etc.), string
  functions (memchr, strchr, strdup, strnlen), `__builtin_va_arg/start/end`,
  `__builtin_signbit`, `__builtin_classify_type`, bswap16/32, and
  GCC predefined type macros (`__UINT8_TYPE__`, `__WCHAR_TYPE__`,
  `__INT_LEAST*_TYPE__`, `__INT_FAST*_TYPE__`, etc.).

- **Comprehensive `__attribute__` consumption.**
  `__attribute__((...))` is now consumed in variable declarations,
  typedef aliases, struct member types, function definitions (after
  param list), and the single-underscore `__attribute` variant.
  `_Alignas` maps to `__attribute__` for C11 alignment specifiers.

- **`typedef const struct`, `const` after type in struct members.**
  `typedef const struct X *alias;` now works via `parsing_typedef_decl`
  flag. `char const *p;` in struct members is accepted.

- **Flat struct initialization.**
  `struct { int f[4]; } s = {1,2,3,4};` distributes values into array
  members instead of requiring nested braces.

- **`asm`/`volatile` as if-body, `va_arg(*ptr, type)`.**
  `if (...) asm(...)` no longer fails. `va_arg(*ap, type)` for pointer-
  to-va_list parameters is accepted.

- **`static` implicit-int for C89 K&R functions.**
  `static funcname(...)` is treated as returning `int`.

- **GCC extension aliases: `__volatile__`, `__signed__`.**

## [v0.14.1] - 2026-05-10

- **SMAUG native executable runtime now survives the first real combat path.**
  `smaug.exe` now boots, accepts telnet, completes character creation,
  reaches Newgate room 109, enters combat with the serpent, survives
  repeated damage rounds, and can kill the serpent cleanly in the
  standalone native executable lane.

- **Small 1..16 byte struct returns now follow the SysV x86-64 ABI in both JIT and native EXE mode.**
  `return some_struct;` no longer leaks a dead stack address or treats
  the first machine word of a local struct as a pointer. The compiler
  now marshals the low 8 bytes into `rax` and the high 8 bytes into
  `rdx`, matching GCC and fixing SMAUG's `EXT_BV multimeb(...)` login
  path plus the broader small-aggregate return-by-value lane.

- **Release baseline now includes the first proven native SMAUG combat run.**
  `make -C src fulltest` is green at 271 integration and 261 unit, and
  the `smaug.exe` runtime probe now advances from startup/login through
  a full serpent fight in room 109.

## [v0.14.0] - 2026-05-08

- **Native `save_executable()` SMAUG path now survives real startup and login.**
  The standalone ELF path now preserves rematerializable global
  struct/class and fixed-array operands across statements without
  reusing stale cached bases, so native SMAUG no longer crashes in
  `bug()` on malformed `vault.lst` EOF handling.

- **`char *` assignments from string literals now emit real C-string pointers.**
  Post-declaration assignments like `char *p; p = "hello";` no longer
  route the literal through `string_cstr(void*)` as if it were a
  `std::string` object. This fixes SMAUG's `alarm_section =
  "new_descriptor::accept";` login-path crash and restores the telnet
  greeting / name-prompt flow in native executables.

- **Native executable coverage expanded around AOT parity regressions.**
  `tests/unit/test_libmadc_program.cpp` now locks in:
  top-level init before `main`, `stderr` support, preserved global array
  layout across function-scope `extern` redeclarations, `char *` returns
  from string literals, and `char *` assignments from string literals.

- **Standalone native executables from madc scripts.**
  `bin/madc -o binary script.mad` generates a self-contained ELF x86-64
  executable with no madc runtime dependency (only libc). The `_start`
  stub calls `__libc_start_main` for proper libc initialization (stdio,
  malloc, atexit). ELF symbol versioning (.gnu.version / .gnu.version_r)
  uses `dlvsym`-based detection — no hardcoded glibc versions. Function
  symbols emitted in `.symtab` for gdb debugging.

- **`madc::engine` public class for shared program configuration.**
  Engine owns registry, policy defaults, and logging. Programs created
  from an engine share its state. C API: `madc_engine_create/destroy`.

- **C API at near-parity with C++ embedding surface.**
  `expression_policy`, allowlist vectors, expression bindings/context,
  `register_function`, `has_function`, `get/set_global`, `eval_body`,
  `has_error`, helper functions. 354-line header, 60+ functions.

- **Process globals cleaned up for multi-instance embedding.**
  `madc_verbose` is now `thread_local`. Dead `throwit` global removed.
  All Phase 4.1 global state blockers closed.

- **Compile-once-run-many and .o cache.**
  `compile_string()` / `compile_file()` + `call()` reuse JIT code.
  `.o` cache via `save_object()` / `load_object()`: SMAUG loads in
  9.5ms from cache vs 26s compile (2,700x speedup).

- **ELF .o writer and loader.**
  `save_object()` emits standard ELF x86-64 relocatable objects with
  function symbols and external symbol relocations. `load_object()`
  reads them back with dlsym resolution and mmap(PROT_EXEC).

- **pkg-config and SONAME versioning.**
  `libmadc.pc` template, `libmadc.so.0` SONAME, versioned install
  with symlinks. `pkg-config --cflags --libs libmadc` works.

- **Embedding examples.**
  `examples/embed_hello.cpp` (C++) and `examples/embed_hello.c` (C).

- **`libmadc.so` install/use validation is now a first-class build path.**
  `src/Makefile` now installs `libmadc.so` with executable/shared-library
  mode through `INSTALL_PROGRAM`, carries a library-owned weak default
  `madc_verbose` definition in `src/madc_globals.cpp` so external
  consumers do not depend on the CLI binary for that symbol, and exposes
  `make -C src libmadc-smoke` to stage-install the library and compile
  plus run both `tests/libmadc_cpp_smoke.cpp` and
  `tests/libmadc_c_smoke.c` against the staged headers and shared
  library.

- **The C shim now covers scalar policy mirrors, invoke limits, and diagnostics enumeration.**
  `include/madc_api.h` and `src/madc_c_api.cpp` now expose C-facing
  mirrors for the scalar portions of `compile_options`,
  `security_policy`, `runtime_eval_policy`, and `invoke_limits`, plus
  diagnostics counting and copy-out through `madc_error`. This keeps the
  C ABI thin, but it is now useful for policy-controlled hosts that need
  more than fire-and-forget compile/call entrypoints. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in policy roundtrip
  and diagnostics enumeration.

- **Phase 4.3 now has a first `libmadc.so` target and thin C ABI.**
  `src/Makefile` now builds `lib/libmadc.so` from a dedicated PIC object
  set and exposes `install-libmadc` for the shared library plus public
  headers. A first C-facing wrapper now also ships in
  `include/madc_api.h` and `src/madc_c_api.cpp`, centered on opaque
  `madc_program` handles and scalar/string `madc_value` exchange for
  compile/exec/eval/call plus last-error retrieval. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in basic C-API
  compile/call behavior and error-text retrieval.

- **Host-side `register_function(...)` can now deduce normal C++ callback signatures, including `std::string`.**
  `madc::program` now has a typed callback-registration helper for
  ordinary host function pointers, so embedders can register callbacks
  like `int64_t(const std::string &)` or `std::string(std::string)`
  without spelling out the low-level `native_signature` or manually
  handling the compiler's `std::string*` callback ABI. Internally this
  lowers through a generated trampoline onto the existing explicit
  signature path, keeping the ABI stable while making the public C++
  surface less error-prone. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in deduced
  `std::string` parameter and return callbacks.

- **Host-side `madc::program::call(...)` and `register_function(...)` now support script `string` object signatures on the real `std::string*` ABI.**
  The host call bridge now recognizes compiled script `string`
  parameters and returns, passing `std::string` object pointers through
  the existing dtSTRING pointer ABI and copying returned string objects
  back into host `madc::value` strings. `register_function(...)` now
  accepts `native_type::string_object` signatures on that same ABI, so
  host callbacks can participate in script string-object calls without a
  separate shim type. Coverage in `tests/unit/test_libmadc_program.cpp`
  now locks in host-to-script string parameters, script string returns,
  and string-object host callbacks.

- **`madc::program::call(...)` now supports up to four arguments.**
  The host-side call dispatcher no longer stops at arity 2 for the
  existing supported native subset. `program::call(...)` now supports up
  to four arguments for `void`, `bool`, `int64_t`, `double`, and
  `const char *` signatures, and the unit coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in both a four-arg
  compiled-script call and a four-arg registered host callback.

- **In-language `madc::eval_unit(...)` now mirrors the host-side full-source alias.**
  The madc script namespace now exposes `eval_unit(...)` as the explicit
  full-source alias alongside the older `eval(...)` name, so script-side
  and host-side runtime-eval examples can use the same terminology for
  the full translation-unit lane. `tests/testmadceval.mad` now uses the
  explicit alias directly.

- **Host-side C++ full-source eval now has an explicit `eval_unit(...)` alias.**
  `madc::program` and the top-level `madc::` convenience wrappers now
  expose `eval_unit(...)` as the explicit name for the existing
  full-translation-unit runtime-eval contract. This keeps `eval(...)`
  working as a compatibility alias while making the public surface read
  cleanly as `eval_expression(...)`, `eval_body(...)`, and
  `eval_unit(...)`. Coverage in `tests/unit/test_libmadc_program.cpp`
  now locks in both the stateful and convenience-wrapper `eval_unit(...)`
  path.

- **Host-side C++ runtime eval now has an explicit `eval_body(...)` lane.**
  `madc::program` and the top-level `madc::` convenience wrappers now
  expose `eval_body(...)` for the common case where the caller wants to
  supply function-body text instead of writing a full
  `__madc_eval(...)` wrapper manually. Typed overloads for `bool`,
  `int64_t`, `double`, and `std::string` auto-wrap the body when no
  explicit `__madc_eval(...)` definition is present, while the generic
  `madc::value` path stays explicit by requiring a declared
  `program::native_type` return contract. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in wrapped-body
  success, explicit-entry passthrough, generic typed-contract use, and
  void-contract rejection.

- **Host-side C++ eval helpers now have typed overloads.**
  `madc::program::eval(...)` and `madc::program::eval_expression(...)`
  now have overloads that write directly into `bool`, `int64_t`,
  `double`, and `std::string`, and the top-level `madc::eval(...)` /
  `madc::eval_expression(...)` convenience wrappers mirror the same
  surface. This keeps the explicit `madc::value` path available while
  removing boilerplate for the common scalar/string embedding cases.
  Coverage in `tests/unit/test_libmadc_program.cpp` now locks in typed
  program wrappers, top-level convenience wrappers, and incompatible
  result-kind rejection.

- **Typed script-side `madc::eval_*` helpers now auto-wrap body text by default.**
  `madc::eval_bool(...)`, `madc::eval_int(...)`,
  `madc::eval_double(...)`, and `madc::eval_string(...)` now treat
  their source input as `__madc_eval` body text when no explicit
  `__madc_eval(...)` definition is present, generating the wrapper
  automatically from the known typed return contract. This removes the
  explicit reserved-entry requirement from the common typed script-side
  path while preserving compatibility for callers that still provide the
  full entry function explicitly. Coverage in `tests/testmadceval.mad`,
  `tests/testmadcevalscope.mad`, and
  `tests/unit/test_libmadc_program.cpp` now exercises the body-mode
  path directly.

- **Full script-side `madc::eval(...)` now has its own child-program sandbox policy.**
  `madc::program` now exposes `runtime_eval_policy` as a separate public
  control surface for full in-language source eval child programs. Hosts
  can independently restrict child `madc::eval(...)` builtin,
  namespace, header, and dynamic-symbol capability without changing the
  parent program's main compile surface or the narrower
  `eval_expression(...)` policy lane. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in policy
  roundtrip plus a restricted-child case where parent compilation stays
  permissive but child full eval cannot resolve `puti(...)`.

- **Script-side runtime eval can now capture current scope under its own sandbox gate.**
  In-language `madc::eval*` and `madc::eval_expression*` calls can now
  see the current visible madc scope through a compiler-synthesized
  `MadArray` context object when runtime-eval scope access is enabled.
  This is controlled independently from broader full-program sandboxing
  through dedicated source-vs-expression gates:
  `compile_options.enable_runtime_eval_source_scope_access`,
  `compile_options.enable_runtime_eval_expression_scope_access`,
  `security_policy.allow_runtime_eval_source_scope_access`, and
  `security_policy.allow_runtime_eval_expression_scope_access`, and
  `authority_mode::system_locked` clamps it off. Coverage in
  `tests/testmadcevalscope.mad` plus
  `tests/unit/test_libmadc_program.cpp` now locks in both the allowed
  path, the full disable path, and independent source-vs-expression
  disable behavior.

- **Full in-memory runtime eval now normalizes trailing-newline-sensitive source and installs scope globals at parse time.**
  The internal full-source eval path now normalizes in-memory source
  buffers so a missing trailing newline no longer breaks
  `madc::eval_int("int __madc_eval() { ... }")`, and the child
  translation-unit path now injects primitive/string scope fields after
  tokenization and before parse so scope-backed full `eval(...)` sees
  the expected globals. This same slice also fixed an off-by-one filter
  in runtime-scope capture so hidden `__literal__*` backing variables no
  longer leak into generated context objects.

- **Madc script code can now call full in-language `madc::eval(...)`.**
  The `madc::` namespace now exposes `madc::eval(out, source)` for
  entry-function-based runtime evaluation of full in-memory madc source
  strings, plus typed helpers `madc::eval_int(source)`,
  `madc::eval_bool(source)`, `madc::eval_double(source)`, and exact
  string helper `madc::eval_string(out, source)`. These layer on the
  same host `program::eval(...)` path used by the public embedding API,
  including normal lexer/parser/compiler flow and reserved
  `__madc_eval` entrypoint semantics. Coverage in
  `tests/testmadceval.mad` now locks in integer, boolean, double, and
  string runtime evaluation from script code itself.

- **In-language runtime eval bridges now hang off `Program` internals.**
  `Program` now owns internal `runtime_eval_source(...)` and
  `runtime_eval_expression(...)` helpers, and the parser/runtime
  `madc::eval*` plus `madc::eval_expression*` bridges are now thin
  callers into that seam instead of directly orchestrating temporary
  wrapper programs themselves. The remaining internal wrapper hop is
  now gone too: those helpers compile and invoke through `Program`
  child instances plus the same internal expression/source validation
  and zero-arg call marshaling rules, without bouncing back through the
  public `madc::program` facade. This is still an ownership cleanup;
  the validated behavior and test baseline are unchanged.

- **In-language `madc::eval_expression(...)` now supports `MadArray` context objects.**
  Madc script code can now build associative expression context objects
  through `madc::context_set_int(...)`, `madc::context_set_real(...)`,
  `madc::context_set_string(...)`, and `madc::context_set_array(...)`,
  then evaluate runtime expressions against that context through
  `madc::eval_expression_ctx(...)` plus typed `_ctx` helpers for
  `bool`, `int`, `double`, and exact-string results. Coverage in
  `tests/testmadcevalexprctx.mad` now locks in nested numeric and
  string context traversal from script code itself.

- **Parser-owned expression context now lowers string leaves correctly.**
  Context-resolved string fields were previously injected as raw
  `TokenStr` nodes after parse-time identifier/member resolution, which
  bypassed the usual string-literal lowering path in `parseExpression()`
  and left top-level/nested string context results empty. String
  context leaves now lower through `Program::addLiteral(...)` the same
  way lexer-produced string tokens do. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves both top-level and
  nested host-side string context evaluation.

- **Phase 4.2 / libmadc public C++ API: first `madc::program` slice.**
  New header `include/libmadc/program.h` and implementation
  `src/madc_program.cpp` add a C++-first pimpl facade over the internal
  `Program` class with `compile_file`, `exec_file`, `exec_string`,
  `diagnostics()`, `last_error()`, `has_error()`, and
  `clear_diagnostics()`. `exec_string` writes the in-memory source to a
  temp file for the current lexer/parser pipeline, while rewriting
  public diagnostics back to the caller's virtual filename. Coverage in
  `tests/unit/test_libmadc_program.cpp` locks in successful compile/run,
  parser diagnostic filename rewriting, diagnostic reset across runs,
  and stream cleanup on destruction.

- **`madc::program` now has a first host callback registration API.**
  `program::register_function(name, callback, signature)` now feeds the
  existing builtin-registration path through a public C++ surface,
  letting hosts expose global native functions to scripts before
  compile/execute. The initial public signature model is intentionally
  narrow: `void`, `bool`, `int64`, `double`, and `const char*`.
  Coverage proves both integer callbacks and script-string to
  `const char*` coercion through `tests/unit/test_libmadc_program.cpp`.

- **`madc::program` now has a first script-call API too.**
  `program::call(name, args, result)` can now invoke compiled global
  script functions from host C++ after `compile_file(...)`, initializing
  the compiled runtime once via `root_fn()` before the first host-side
  call. The initial surface is intentionally narrow and explicit:
  it supports only the scalar / C-string subset (`void`, `bool`,
  `int64`, `double`, `const char*`), currently limits arity to 2, and
  rejects unsupported script-side shapes like `string` object params or
  returns, multi-return functions, and varargs with a structured public
  runtime error instead of miscalling them. Coverage in
  `tests/unit/test_libmadc_program.cpp` proves integer args/return,
  host string to script `char *` args, and the current unsupported
  `string` object rejection path.

- **`madc::program` now exposes first global get/set surfaces.**
  `program::get_global(name, result)` and
  `program::set_global(name, value)` now let hosts read and write
  compiled global variables after runtime initialization. The first
  public slice is intentionally narrow and explicit: scalar globals map
  to `madc::value` booleans / integers / reals, script `string` globals
  map to host strings, and unsupported shapes like arrays fail with a
  structured public runtime error instead of pretending to work.
  Coverage in `tests/unit/test_libmadc_program.cpp` now proves integer
  and string global round-trips, unsupported array rejection, and a
  64-bit integer regression where host-side writes above 32-bit range
  must not truncate through the older `Variable::set(int)` helper path.

- **`madc::program` now has a first in-memory `eval(...)` surface.**
  `program::eval(source, result, virtual_filename)` now compiles an
  in-memory translation unit through the same temp-file lexer/parser
  path as `exec_string(...)`, then invokes a reserved zero-arg
  `__madc_eval` entrypoint through the existing host-side `call(...)`
  path. This keeps `eval(...)` on the same compile/runtime seam instead
  of inventing a separate execution model. The first slice is
  intentionally narrow and explicit: it is entry-function based rather
  than free-form expression evaluation, and it inherits the same narrow
  result marshaling as `call(...)`. Coverage in
  `tests/unit/test_libmadc_program.cpp` proves integer and string
  results, virtual-filename diagnostic rewriting, and the current
  missing-entry failure path.

- **`madc::` now has a first convenience wrapper tier over `madc::program`.**
  New header `include/libmadc/api.h` now exposes free-function
  `madc::eval(...)`, `madc::exec_string(...)`, and `madc::exec_file(...)`
  as thin wrappers over a temporary `madc::program`. This gives simple
  embedders a script-like entry convention without introducing a second
  execution model or changing where policy/runtime state really lives.
  Coverage in `tests/unit/test_libmadc_program.cpp` now proves the
  wrapper layer for eval, string exec, and file exec.

- **`madc::program` now has a first expression-only evaluation surface.**
  `program::eval_expression(expression, result, virtual_filename)` now
  evaluates a single expression through a dedicated expression parse /
  compile seam rather than exposing the full general-purpose `eval(...)`
  surface. The first slice is intentionally narrow and explicit: it
  routes through the existing policy seam, builds a synthetic hidden
  function around the parsed expression instead of widening the old
  `__madc_eval` path, and introduces the first public allowlist
  surfaces for embedded headers and dynamic symbols. `math.h` is the
  first real header-group case, enabling libm-backed expressions
  without reopening unrestricted `#load` / fallback symbol access. The
  result-type inference for operator expressions now also follows the
  expression AST instead of trusting `TokenOperator`'s default
  `_datatype`, so real-valued libm expressions like
  `sqrt(9.0) + cos(0.0)` stay on the dedicated expression path instead
  of falling back to the older translation-unit route.
  Coverage in `tests/unit/test_libmadc_program.cpp` now proves plain
  arithmetic, statement-shaped rejection, direct symbol allowlists,
  string-literal return marshaling, forked scalar expression results,
  `math.h` header-group use, and the top-level
  `madc::eval_expression(...)` wrapper.

- **Expression authority is now explicit on `madc::program`.**
  `include/libmadc/options.h` now adds `expression_policy`, and
  `madc::program` now exposes `set_expression_policy(...)` /
  `get_expression_policy()`. `eval_expression(...)` no longer depends on
  the broader `security_policy` symbol/header fields for expression
  calls: function calls are denied by default, allowed calls can be
  granted either by explicit function name or by header-group expansion
  such as `math.h`, and out-of-policy calls now fail with a public
  runtime error before compilation. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves default call denial,
  explicit allowlist acceptance, allowlist rejection, and policy
  roundtrip.

- **`eval_expression(...)` now rejects breakout-oriented source forms
  before compilation.** The generated-expression path now validates the
  supplied text up front and fails explicitly on `;`, block braces,
  preprocessor directives, assignment operators, increment/decrement,
  and reserved `__madc_*` identifiers instead of relying on accidental
  parser/compiler rejection after code generation. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves breakout-token and
  mutation-operator rejection.

- **`eval_expression(...)` now validates parsed AST shape too.**
  The dedicated expression seam no longer relies only on source-text
  guards before building its synthetic hidden function. Parsed
  expressions are now walked through an explicit whitelist of allowed
  node families, while mutation/sequencing forms remain rejected.
  Coverage in `tests/unit/test_libmadc_program.cpp` now adds ternary
  acceptance and explicit rejection for comma sequencing in expression
  mode.

- **`expression_policy` now gates pointer/lvalue-style expression forms explicitly.**
  The dedicated expression path no longer treats member access,
  subscript access, and pointer operations as accidental byproducts of
  the AST whitelist. `expression_policy` now has separate booleans for
  function calls, member access, subscript access, and pointer
  operations, with the non-call lvalue/pointer-style forms disabled by
  default. Coverage in `tests/unit/test_libmadc_program.cpp` now proves
  default rejection and explicit opt-in for subscript and pointer
  expressions, plus roundtrip of the richer policy surface.

- **`eval_expression(...)` now supports first host-supplied bindings.**
  `madc::program` now exposes `set_expression_bindings(...)`,
  `get_expression_bindings()`, and `clear_expression_bindings()` for a
  narrow first binding model. The dedicated expression path installs
  explicit host-provided scalar/string bindings into the temporary
  expression program before parsing, so hosts can evaluate expressions
  against scoped input data without routing through globals. This first
  slice is intentionally narrow: only boolean, integer, real, and
  string bindings are accepted, while arrays/objects/bytes fail
  explicitly. Coverage in `tests/unit/test_libmadc_program.cpp` now
  proves integer bindings, string binding roundtrip, unsupported-kind
  rejection, and program-state roundtrip for the binding map.

- **`eval_expression(...)` now supports first object-backed host context.**
  `madc::program` now also exposes `set_expression_context(...)`,
  `get_expression_context()`, and `clear_expression_context()`. The
  first slice is intentionally conservative: a host `madc::value`
  object becomes the source of top-level expression bindings, with
  explicit rejection for non-object contexts and explicit collision
  rejection when a context field name overlaps a direct binding name.
  This gives expression mode a scoped object/struct-style host surface
  without committing yet to nested member reflection. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves integer field use,
  non-object rejection, collision rejection, and state roundtrip.

- **Nested object-context primitive traversal now works in `eval_expression(...)`.**
  Safe expression mode still does not allow pointer dereference or
  general host-member semantics, but it now supports static nested
  primitive leaf traversal from object context values such as
  `user.stats.level`. This is implemented conservatively by rewriting
  matching dotted context paths to synthetic scoped bindings before
  parse/compile, keeping the existing no-pointer boundary intact.
  Coverage in `tests/unit/test_libmadc_program.cpp` now proves nested
  object traversal through `user.stats.level + 1`.

- **Nested expression-context lookup now fails path-by-path with explicit diagnostics.**
  The object-backed `eval_expression(...)` context seam no longer
  rejects a whole context just because it contains an unrelated
  unsupported leaf. Instead, context validation now follows the
  expression's referenced identifier paths: missing nested fields,
  descent through non-object leaves, and terminal object/unsupported
  values now fail with path-specific runtime errors, while unrelated
  array/bytes/object leaves are ignored unless the expression actually
  touches them. Coverage in `tests/unit/test_libmadc_program.cpp` now
  proves explicit missing-field and bad-descent diagnostics plus the
  non-referenced unsupported-leaf case.

- **madc script code can now call a first in-language `madc::eval_expression(...)`.**
  The `madc::` namespace now exposes `madc::eval_expression(out, expr)`
  for use inside madc programs themselves. This first slice is narrow
  on purpose: it evaluates the runtime expression string through the
  existing libmadc expression seam, stringifies scalar/string results
  into a caller-supplied `string`, and currently allows libm-backed
  calls through `math.h` when the active program's dlfcn policy allows
  them. Coverage in `tests/testmadcevalexpr.mad` now locks in integer,
  string, and libm-backed runtime expression results from inside madc
  itself.

- **In-language `madc::eval_expression(...)` now has typed helpers.**
  Madc script code can now evaluate runtime expressions directly into
  scalar types via `madc::eval_expression_int(expr)`,
  `madc::eval_expression_bool(expr)`, and
  `madc::eval_expression_double(expr)`, plus an exact-string helper
  `madc::eval_expression_string(out, expr)` that keeps the existing
  string-out calling convention instead of stringifying non-string
  results. Coverage in `tests/testmadcevalexprtyped.mad` locks in the
  typed runtime path for integer, boolean, double, and exact-string
  expression evaluation inside madc itself.

- **`eval_expression(...)` now leans more on madc's own token/parser pipeline.**
  Expression call restrictions are now checked from lexer tokens before
  parse instead of by rescanning raw source text, which keeps blocked
  calls on the public runtime-error path even when the callee would
  otherwise be undeclared at parse time. The remaining nested
  object-context seam is also closer to the parser now: dotted context
  paths tolerate parser-legal whitespace and comments around `.`
  separators. Coverage in `tests/unit/test_libmadc_program.cpp` now
  proves nested context traversal through spaced/commented dotted
  paths.

- **`eval_expression(...)` object context now resolves through a parser-owned named-root model.**
  Object-backed expression context is no longer implemented by
  pre-rewriting nested dotted paths onto synthetic bindings before
  parse. Unresolved identifiers and subsequent `.` chains can now
  resolve directly against a parser-visible context root, so
  `user.stats.level` is modeled inside madc's own parse pipeline while
  still staying limited to static primitive leaves. Coverage in
  `tests/unit/test_libmadc_program.cpp` continues to prove top-level
  context fields, nested primitive traversal, path-specific missing-
  field/bad-descent diagnostics, and parser-legal trivia around dotted
  paths.

- **Synthetic expression-function wrapping now lives on `Program`.**
  The AST surgery that turns a parsed expression into a compilable
  hidden function is no longer embedded only inside the `libmadc`
  wrapper flow. `Program` now owns a `build_expression_function(...)`
  helper that attaches the parsed expression to the normal AST /
  pending-function pipeline, leaving `madc::program::eval_expression(...)`
  focused on policy, bindings/context setup, and result marshaling.

- **Full in-memory eval/exec no longer require temporary source files.**
  `Program` now has an in-memory translation-unit tokenization path, so
  `madc::program::eval(...)` and `madc::program::exec_string(...)`
  compile source buffers directly through the normal lexer/parser/
  compiler pipeline instead of writing temp files first. This keeps the
  full-source eval path aligned with the same core compiler machinery
  that the expression lane has been moving toward.

- **`madc::program` now exposes first options/policy surfaces.**
  `include/libmadc/options.h` now ships `compile_options`,
  `security_policy`, `invoke_limits`, and `authority_mode`, and
  `madc::program` now exposes setters/getters for all three. The first
  implementation is intentionally honest about what it enforces today:
  `compile_options` mirrors the real `Program::RegistrationPolicy`
  booleans for builtin registration and built-in namespace registration,
  `security_policy` is a higher-level wrapper over those same effective
  gates, and `invoke_limits` currently round-trips as stored
  configuration only. Coverage in `tests/unit/test_libmadc_program.cpp`
  proves disabled core builtin registration, disabled namespace
  registration, and invoke-limit round-tripping.

- **`madc::program` policy now gates raw `#load` and fallback `dlsym`.**
  The same `enable_dlfcn_functions` policy seam now reaches the real
  authority escape hatches too: `#load` directives, `#load`-backed
  namespace `dlsym`, parse-time RTLD-default symbol fallback, and
  compiler-side extern late-bind `dlsym` now all fail explicitly when
  that policy gate is disabled. Coverage in
  `tests/unit/test_libmadc_program.cpp` now locks in the lexer, parser,
  and compiler denial paths. The remaining policy gap is deeper
  runtime/in-process resource enforcement rather than raw symbol-loading
  access.

- **`authority_mode::system_locked` now clamps dangerous capability back on the public API seam.**
  `system_locked` is no longer descriptive-only metadata. On the
  effective `madc::program` policy surface it now forces process
  builtins and dynamic-loading paths off, keeps the derived
  `compile_options` / `security_policy` views in sync, and prevents
  later `set_compile_options(...)` calls from re-enabling `#load`,
  dlsym fallback, or process builtins. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves both the clamped
  getters and the failed re-enable path.

- **Public policy now carries an explicit execution-mode seam.**
  `include/libmadc/options.h` now defines `execution_mode` with
  `in_process` and `fork_per_invocation`, and `security_policy` now
  stores that desired execution mode. This slice does not yet implement
  child-process execution, but it does make the contract explicit:
  under `authority_mode::system_locked`, the effective public policy now
  clamps execution to `fork_per_invocation` so the next worker/fork
  runtime work has a stable surface to attach to. Coverage in
  `tests/unit/test_libmadc_program.cpp` locks in round-trip behavior for
  unlocked mode and the locked-mode clamp.

- **`fork_per_invocation` now reaches real child-process execution on the public runtime seam.**
  When the effective public policy execution mode is
  `fork_per_invocation`, `exec_file(...)` / `exec_string(...)` now
  compile in the parent and fork a child for `Program::execute()`,
  while `call(...)` and entry-function-based `eval(...)` now also run
  their actual invocation inside a child. The host side captures child
  stdout/stderr into temp files, serializes public diagnostics back to
  the parent, replays output/error, and now also marshals the existing
  narrow scalar / C-string result subset back for forked `call(...)` /
  `eval(...)`. This is still an intentionally narrow worker slice:
  globals stay in-process, result marshaling does not widen beyond the
  existing scalar / C-string contract, and limit enforcement remains
  honest post-invocation accounting rather than preemption. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves fork-mode exec
  success/error propagation, child stdout/stderr contribution to
  output-limit enforcement, plus forked scalar/string `call(...)` and
  `eval(...)` results.

- **`madc::program` now enforces `invoke_limits` on public invocation paths.**
  `exec_file(...)`, `exec_string(...)`, `eval(...)`, `call(...)`, and
  runtime-init paths reached through `get_global(...)` / `set_global(...)`
  now snapshot process CPU time, resident size, and madc-managed
  output/error buffer sizes before invocation and reject the operation
  afterward if the configured `cpu_ms`, `memory_bytes`, or
  `output_bytes` budget was exceeded. This is explicit post-invocation
  accounting rather than in-process preemption. A follow-up now also
  captures raw libc `stdout` / `stderr` writes during host API
  invocation so `output_bytes` reflects both `MadcEngine`-managed
  buffers and direct fd-level output. Coverage in
  `tests/unit/test_libmadc_program.cpp` now proves raw-output-byte,
  CPU-time, and resident-growth rejection paths.

- **MadcEngine now restores redirected standard streams on teardown.**
  `MadcEngine` now resets `std::cin` / `std::cout` / `std::cerr` plus
  built-in log sinks in its destructor, fixing the `madc::program`
  teardown crash where `std::cerr` still pointed at a freed
  `ostringstream` during process exit. The regression was reproduced by
  repeated standalone runs and confirmed under both `gdb` and
  `valgrind`.

- **`make -C src test` now fails on the first crashing unit binary.**
  The unit-test loop used to return the status of only the final test
  binary, which could hide an earlier segfault behind a false-green
  run. The Makefile now exits immediately when any unit binary fails, so
  `make -C src test` and `make -C src fulltest` surface the real cause.

- **`madcdat` now has its own archive and install target.**
  `make libmadcdat` now builds `lib/libmadcdat.a` from the gated
  storage/federation object set, and `make install-madcdat` now stages
  that archive plus the canonical `include/madcdat/` headers and the
  dependent `include/libmadc/` public headers needed to compile against
  it. When `madcdat` is disabled, both targets now fail explicitly
  instead of pretending the artifact exists.

- **`madcdat` now has a real top-level configure gate.**
  `./configure --enable-madcdat=no` now excludes the storage/federation
  object set from the build, drops the `madcdat`-dependent unit test
  binaries from `make -C src test`, and still leaves the core compiler,
  runtime, and integration suite buildable. Backend toggles like
  `--with-bdb` / `--with-gdbm` / `--with-qdbm` / `--with-sqlite3` now
  require `madcdat` to be enabled instead of silently pretending the
  subsystem is present. The default enabled path remains unchanged and
  fully green.

- **Prepared the physical `madcdat` tree split without changing the
  library boundary yet.** The storage/federation headers now have a
  canonical public root at `include/madcdat/`, the storage/query driver
  translation units now live under the `src/madcdat_*.cpp` naming
  pattern, and the old `include/libmadc/*.h` data-layer headers are now
  compatibility forwarders into that new header root. This settled the
  public header boundary before the later archive/install split and also
  locked in `./configure --enable-madcdat` as the top-level subsystem
  gate rather than `--with-madcdat`.

- **QueryBuilder now has a first logical-composition seam.** `Query`
  now carries predicate match-mode metadata (`all` vs `any`), and
  `QueryBuilder` exposes `match_all()` / `match_any()` so the public
  surface can start expressing future AND-vs-OR intent without jumping
  straight to a planner rewrite. `DataSet<T>::query(...)` and
  `query_raw(...)` explicitly reject non-default composition for now
  instead of silently ignoring it. Coverage in
  `tests/unit/test_libmadc_storage_contract.cpp` locks in the builder
  metadata shape.

- **Builder queries now support `where_not_in(...)`.** `Query` /
  `QueryBuilder` can now carry one explicit negative-membership
  predicate in addition to equality, inequality, positive membership,
  string patterns, and ordered bounds. `DataSet<T>` applies `NOT IN`
  through local fallback, `sqlite://` pushes it natively, and the keyed
  local stores explicitly reject that shape for pushdown so they fall
  back locally instead of pretending to support it. Coverage in
  `tests/unit/test_libmadc_storage_contract.cpp`,
  `tests/unit/test_libmadc_sqlite.cpp`, and
  `tests/unit/test_libmadc_qdbm.cpp` locks in builder metadata,
  SQLite pushdown, and keyed-store fallback behavior.

- **Builder queries now support `where_like(...)`.** `Query` /
  `QueryBuilder` can now carry one explicit SQL-style string pattern
  predicate in addition to equality, inequality, membership, and
  ordered bounds. `DataSet<T>` applies `LIKE` through a string-only
  local fallback matcher supporting `%` and `_`, `sqlite://` pushes it
  natively, and the keyed local stores explicitly reject that shape for
  pushdown so they fall back locally instead of pretending to support
  it. Coverage in `tests/unit/test_libmadc_storage_contract.cpp`,
  `tests/unit/test_libmadc_sqlite.cpp`, and
  `tests/unit/test_libmadc_qdbm.cpp` locks in builder metadata,
  SQLite pushdown, and keyed-store fallback behavior.

- **Builder queries now support `where_in(...)`.** `Query` /
  `QueryBuilder` can now carry one explicit membership predicate in
  addition to equality, inequality, and ordered bounds. `DataSet<T>`
  applies `IN` through local fallback, `sqlite://` pushes it natively,
  and the keyed local stores explicitly reject that shape for pushdown
  so they fall back locally instead of pretending to support it.
  Coverage in `tests/unit/test_libmadc_storage_contract.cpp`,
  `tests/unit/test_libmadc_sqlite.cpp`, and
  `tests/unit/test_libmadc_qdbm.cpp` locks in builder metadata,
  SQLite pushdown, and keyed-store fallback behavior.

- **Builder queries now support `where_ne(...)`.** `Query` /
  `QueryBuilder` can now carry one explicit not-equal predicate in
  addition to equality and ordered bounds. `DataSet<T>` applies `!=`
  through local fallback, `sqlite://` pushes it natively, and the keyed
  local stores explicitly reject that shape for pushdown so they fall
  back locally instead of pretending to support it. Coverage in
  `tests/unit/test_libmadc_storage_contract.cpp`,
  `tests/unit/test_libmadc_sqlite.cpp`, and
  `tests/unit/test_libmadc_qdbm.cpp` locks in builder metadata,
  SQLite pushdown, and keyed-store fallback behavior.

- **Typed relation traversal now accepts target-side builder filters
  and limits.** `Relation<A,B>::query_related(...)` now has a target-
  query overload mirroring the raw relation path, so callers can filter
  and cap resolved target rows without dropping to `value`-shaped
  output. Typed relation traversal still rejects `select(...)`, keeping
  the no-partial-decode rule intact while making target-side filtering
  semantics consistent across the typed and raw surfaces.

- **Relation raw traversal now accepts real target-side builder
  composition.** `Relation<A,B>::query_related_raw(...)` no longer
  restricts the target query to just dataset name plus selected fields.
  Target-side builder filters and limits are now honored against the
  resolved related rows, and an empty `select(...)` now means “return
  the full logical object” instead of hard-failing. This keeps relation
  traversal aligned with the existing `DataSet<T>::query_raw(...)`
  surface without inventing a separate relation-only query language.

- **Strict builder bounds now have a public API.** `QueryBuilder` now
  exposes `where_gt(...)` and `where_lt(...)` in addition to the
  existing inclusive bound helpers, so callers can express exclusive
  ordered scans without constructing `Query` objects manually. The
  runtime and ordered backends already carried the inclusive/exclusive
  flags internally; this slice makes that capability part of the
  public builder surface and locks it in with coverage across SQLite,
  QDBM, BDB, and the storage-contract builder tests.

- **Storage federation plan tightened around planning boundaries.** The
  canonical federation plan now explicitly calls out the logical-vs-
  physical query IR boundary, a coarse driver capability model for
  planning, and a deliberately narrow V1 federation scope. This keeps
  the newer pushdown/relation/projection work aligned with a clear
  planner seam without replacing the more detailed
  source/mapping/index/reindex design already in the plan.

- **Core `DataSource` classification now distinguishes storage,
  service, and IPC families.** `madc::DataSource` now exposes both a
  coarse domain classification layer (`storage`, `service`, `ipc`) and
  a finer source-family layer (`record_file`, `relational_database`,
  `keyed_database`, `graph_database`, `service_api`, `unix_socket`,
  etc.), with helpers such as `is_storage()`, `is_database()`,
  `is_graph_database()`, `is_service_api()`, and `is_unix_socket()`.
  This makes the core API match the current design direction:
  `DataSource` is a general external-conduit abstraction for storage,
  IPC, and embedding interconnect work, not just database plumbing.
  Coverage in `tests/unit/test_libmadc_storage_contract.cpp` now proves
  record-file storage, remote graph storage, relational vs keyed DB
  classification, HTTPS service, and Unix-socket IPC classification.

- **Storage planning note: `madcdat` is now the official subsystem
  name.** The storage/federation/indexing design is now explicitly
  named `madcdat`, while the physical library split remains future
  work. Current implementation still lives in the existing `libmadc`
  tree; the later `libmadcdat` boundary stays a planned optional build
  direction rather than an in-progress extraction. `madc::DataSource`
  stays on the core side of that line as a general external-conduit
  abstraction, not something owned exclusively by the data subsystem.

- **Raw projected builder queries.** `DataSet<T>` now has a separate
  `query_raw(...)` surface for builder queries that return projected
  `madc::value` objects instead of pretending partial rows still decode
  into full host `T`. `QueryBuilder::select(...)` now flows end-to-end
  through the raw path, local fallback can project logical records, and
  the current pushdown backends (`sqlite://`, `qdbm://`, `bdb://`,
  `gdbm://`) now accept selected-field builder shapes and return
  projected objects on that raw surface. Coverage in
  `tests/unit/test_libmadc_qdbm.cpp`,
  `tests/unit/test_libmadc_sqlite.cpp`, and
  `tests/unit/test_libmadc_storage_contract.cpp` proves the new
  projection path.

- **Bounded key-range query support for builder queries.** `Query` /
  `QueryBuilder` can now carry upper-bound metadata in addition to
  equality and lower-bound filters, so callers can express bounded
  `>= ... <= ...` key scans instead of only point lookups or open-ended
  lower ranges. `DataSet<T>` applies those bounds in local fallback,
  `sqlite://` now pushes the full bounded predicate directly, and the
  ordered keyed `qdbm://` / `bdb://` backends now honor upper bounds by
  stopping native cursor scans once the key range is exhausted.
  Coverage in `tests/unit/test_libmadc_sqlite.cpp`,
  `tests/unit/test_libmadc_qdbm.cpp`,
  `tests/unit/test_libmadc_bdb.cpp`, and
  `tests/unit/test_libmadc_storage_contract.cpp` locks in the new
  bounded-range builder shape.

- **First relation-aware traversal over pushed dataset queries.**
  `Relation<A,B>` can now do more than resolve one source key at a
  time: `query_related(...)` walks a filtered source `DataSet<A>` query
  and materializes related `B` rows for `key_match` and `offset`
  bindings. The initial coverage proves both shapes: FLR index rows can
  query through VLR offset locators, and a filtered `sqlite://` source
  can traverse into a keyed `qdbm://` side dataset by shared primary
  key. This is the first real relation-traversal layer on top of
  `DataSet<T>::query(...)`, without pretending to be a full join
  planner yet.

- **Ordered/lower-bound query pushdown for keyed datasets.** The query
  builder path now goes beyond equality-only filters: `Query` /
  `QueryBuilder` can describe lower-bound scans, `DataSet<T>` can apply
  them through either backend pushdown or local fallback, `sqlite://`
  now executes key-ordered `>=` plus `LIMIT` builders directly, and the
  ordered keyed `qdbm://` / `bdb://` backends now use native cursor
  positioning for the same lower-bound key scans. Coverage in
  `tests/unit/test_libmadc_sqlite.cpp`,
  `tests/unit/test_libmadc_qdbm.cpp`,
  `tests/unit/test_libmadc_bdb.cpp`, and
  `tests/unit/test_libmadc_storage_contract.cpp` locks in the new
  builder metadata and range behavior.

- **Phase 4 planning note: reserve the `libmadcdat` seam and treat
  `madc::eval(...)` as policy-bound execution.** The Phase 4 and storage
  plans now explicitly reserve an optional future `libmadcdat`
  sublibrary for the `madcdat` storage/federation/indexing subsystem:
  `DataSource`, drivers, mappings, relations, source adapters, indexes,
  and federation/query planning. That boundary is intentional planning,
  not an extraction already underway, and it keeps that complexity from
  bleeding into the core `libmadc` embedding surface. The same planning
  pass also records `madc::eval(...)` as a future core API that must
  honor the same security policy, parser registration, and invoke
  limits as file-based program execution.

- **Storage federation design note: keep the layers separate.** The
  storage plan in `docs/plans/data-storage-federation.md` now makes the
  anti-monolith structure explicit: `DataSource` stays first-class,
  `SourceAdapter` handles source segmentation/classification,
  `FormatAdapter<T>` stays per-record, `ExtractedRecordType` models
  multiple record families per source, `Relation<A,B>` stays distinct
  from dataset-local mapping, and `IndexDefinition` plus reindex
  workflows own derived indexes for CSV/TOML/tagged-text/mailbox-style
  sources. This locks in a high-cohesion/low-coupling direction before
  more storage backends and text-format parsers land.

- **First real query pushdown path for typed datasets.** `Query` /
  `QueryBuilder` now carry structured builder metadata instead of just
  display text, `DataSet<T>` grows `query(...)`, and the runtime can now
  push simple equality filters into backends that actually support them
  while falling back to local scan/filter elsewhere. `sqlite://` now
  executes scalar `WHERE field = value` plus `LIMIT` directly, and the
  keyed `qdbm://`, `gdbm://`, and `bdb://` backends now execute primary-
  key equality filters through the same path. New coverage in
  `tests/unit/test_libmadc_sqlite.cpp`,
  `tests/unit/test_libmadc_qdbm.cpp`,
  `tests/unit/test_libmadc_gdbm.cpp`,
  `tests/unit/test_libmadc_bdb.cpp`, and
  `tests/unit/test_libmadc_storage_contract.cpp` locks in the builder
  metadata and pushed-query behavior.

- **Stable append-only VLR locator contract.** `vlr://` record locators
  are now an explicit opt-in contract rather than a best-effort offset
  detail. When a VLR dataset is configured with a tombstone sidecar,
  locator-aware writes become append-only: inserts append new payloads,
  updates append a replacement row and tombstone the old version,
  deletes only tombstone rows, restores clear tombstones for truly
  deleted rows, and `get_by_locator(...)` now fails explicitly for
  tombstoned/stale payload offsets. This keeps live locators stable
  across reopen and non-compacting rewrites while making stale-link
  failures visible. New coverage in `tests/unit/test_libmadc_vlr.cpp`
  proves reopen, update, erase, restore, and no-tombstone failure
  behavior, and `tests/unit/test_libmadc_relation.cpp` now opts the
  payload file into the stable-locator contract.

- **First concrete FLR -> VLR offset relation slice.** The storage
  layer now has its first real cross-dataset binding, not just relation
  metadata. `RecordLocator` is now part of the driver/runtime surface,
  `DataSet<T>` can `insert_with_locator(...)`, `get_by_locator(...)`,
  and `get_field(...)`, `Relation<A,B>::resolve(...)` can follow offset
  and key-match bindings, and `vlr://` now tracks byte offsets for
  variable-record payload rows so `flr://` index rows can point into a
  VLR payload file by stored offset. New coverage in
  `tests/unit/test_libmadc_relation.cpp` proves ordered FLR index rows
  resolving VLR payload rows end-to-end.

- **FLR post-reap restore by dead-archive reinsertion.** `flr://`
  restore is no longer limited to pre-compaction tombstone clearing.
  If a tombstoned record has already been reaped into the dead archive,
  `DataSet<T>::restore(key)` now loads that archived row, reinserts it
  into the live FLR, and removes it from the archive. Ordered fixed-
  record datasets now reinsert restored rows by key order rather than
  blindly appending. New coverage in `tests/unit/test_libmadc_flr.cpp`
  proves restore-after-reap plus sorted reinsertion.

- **FLR reap/compaction into dead archive files.** The `flr://`
  tombstone-sidecar path now has its first cleanup workflow.
  `MappingSpec<T>` / `SchemaInfo` can carry a dead-record archive file,
  `DataSet<T>` now exposes `compact()`, and `FlrDriver` can reap
  tombstoned fixed-length records into a parallel archive FLR while
  rewriting the live FLR to contain only surviving rows and resetting
  the tombstone bitvector to match the compacted live record count.
  New coverage in `tests/unit/test_libmadc_flr.cpp` proves live-file
  shrink plus dead-archive persistence through reopen.

- **Fourth real keyed local DB backend: `sqlite://`.** The storage
  layer now has a SQLite-backed keyed backend alongside `qdbm://`,
  `gdbm://`, and `bdb://`. `src/madc_storage_sqlite.cpp` adds a
  `SqliteDriver` that creates a schema table on first open, maps
  registered host fields to typed SQLite columns, enforces one primary
  key field, and supports duplicate-rejecting insert, update, erase,
  point lookup, reopen persistence, and deterministic key-ordered scan.
  New doctest binary `tests/unit/test_libmadc_sqlite.cpp` covers typed
  `DataSet<T>` round-trip through an actual SQLite database file.

- **FLR tombstone sidecars + pre-reap restore.** `flr://` can now bind a
  packed-bit tombstone sidecar so deletes become soft positional marks
  instead of immediate physical removal. `MappingSpec<T>` and
  `SchemaInfo` now carry tombstone sidecar metadata, `DataSet<T>` grows
  `restore(key)`, and `FlrDriver` skips tombstoned rows in normal
  lookup/scan paths while preserving the underlying fixed-record file
  intact until a later reap/compaction phase. New coverage in
  `tests/unit/test_libmadc_flr.cpp` proves delete/reopen/restore
  behavior, and `tests/unit/test_libmadc_storage_contract.cpp` now
  locks in the contract for tombstone sidecars, dataset roles, ordered
  fixed-record metadata, and positional/offset/key/graph relation kinds.

- **Third real keyed local DB backend: `bdb://`.** The storage layer
  now has a Berkeley DB Btree-backed keyed backend alongside
  `qdbm://` and `gdbm://`. `src/madc_storage_bdb.cpp` adds a
  `BdbDriver` with one primary key field, typed record payload
  storage, point lookup, insert/update/erase, and ordered scan through
  Berkeley DB cursors. Like the Villa path, keys are canonically
  encoded so signed and unsigned integer keys sort correctly under the
  backend's lexical Btree ordering. New doctest binary
  `tests/unit/test_libmadc_bdb.cpp` covers keyed round-trip, duplicate
  rejection on insert, update, erase, and ordered iteration.

- **Second real keyed local DB backend: `gdbm://`.** The storage layer
  now has a second optional key/value backend alongside `qdbm://`.
  `src/madc_storage_gdbm.cpp` adds a `GdbmDriver` over GNU GDBM with
  one primary key field, typed record payload storage, point lookup,
  insert/update/erase, and full database scan through the native key
  iteration API. Unlike Villa, GDBM is hash-backed, so the public
  contract deliberately does not promise ordered scans. New doctest
  binary `tests/unit/test_libmadc_gdbm.cpp` covers keyed round-trip,
  duplicate rejection on insert, update, erase, and unordered scan
  membership.

- **First real keyed local DB backend: `qdbm://` via Villa.** The
  exploratory storage layer now has its first ordered key/value store
  instead of file-shaped record stores only. `src/madc_storage_qdbm.cpp`
  adds a `QdbmDriver` over QDBM's Villa B+ tree API with one primary key
  field, point lookup, ordered cursor scan, insert/update/erase, and
  typed binary record payloads stored independently from the in-memory
  host layout. Keys are canonically encoded so integer keys sort
  correctly under Villa's lexical comparator, and scans now come back in
  key order rather than insertion order. New doctest binary
  `tests/unit/test_libmadc_qdbm.cpp` covers keyed round-trip, duplicate
  rejection on insert, update, erase, and ordered iteration.

- **Registration-based `infer_mapper()` for host C++ types.** The
  exploratory storage layer no longer requires a handwritten
  `DataMapper<T>` class for every happy-path host type. `mapper.h` now
  ships `MapperRegistration<T>`, `MapperBuilder<T>`, and
  `AutoDataMapper<T>`, so a type can register its fields once and let
  `DataSet<T>` infer a working mapper automatically at `open()`. The
  builder derives normalized schema facts (kind, width, signedness) for
  integers, enums, reals, bools, chars, fixed `char[N]`, and
  `std::string`, and FLR-specific bindings can override storage offsets
  and fixed text widths so file layout stays separate from host-memory
  layout. The `dsv://`, `flr://`, and `vlr://` round-trip tests now use
  this inference path instead of bespoke mapper classes.

- **Autotools/configure scaffolding for optional storage backends.**
  Added `configure.ac`, `config.mk.in`, and a top-level `Makefile.in`
  so `madc` can grow optional backend detection without hard-linking
  every database dependency into the core build. `src/Makefile` now
  consumes generated feature flags and optional libs from `config.mk`
  when present while preserving the existing `make -C src` workflow.
  Initial `./configure` switches are wired for `--with-bdb`,
  `--with-gdbm`, `--with-qdbm` (Villa-oriented), `--with-xqdbm`, and
  `--with-sqlite3`, with optional translation units in place so feature
  detection can be added ahead of or alongside backend implementation.

- **Third working `libmadc` storage backend: `vlr://`.** The first
  local storage family is now complete: `dsv://` for logical text
  rows, `flr://` for fixed-size binary records, and `vlr://` for
  variable-length binary records. `src/madc_storage.cpp` now includes
  `VlrDriver`, which persists generic object-shaped `madc::value`
  records as length-prefixed binary payloads with native scalar widths
  and full string preservation, avoiding the fixed-size pressure that
  drives FLR truncation/error policy. New doctest binary
  `tests/unit/test_libmadc_vlr.cpp` covers variable-sized record
  round-trip, long-string preservation, update/erase, and key-based
  retrieval through the same typed `DataSet<T>` facade ordinary host
  C++ code uses for `dsv://` and `flr://`.

- **Second working `libmadc` storage backend: `flr://`.** The storage
  runtime now has a fixed-record binary backend alongside `dsv://`.
  `SchemaInfo` and `SchemaField` carry record-layout metadata needed to
  lower `MappingSpec<T>` into driver-visible storage policy: logical vs
  fixed-record layout, record size, overflow policy, and per-field text
  truncation behavior. `src/madc_storage.cpp` now includes `FlrDriver`,
  which persists object-shaped `madc::value` records into fixed-size
  binary rows, enforces strict overflow by default, supports explicit
  truncation when configured, and normalizes cached rows through the
  persisted fixed-record form so `get()` reflects what actually hit the
  file. New doctest binary `tests/unit/test_libmadc_flr.cpp` covers
  successful fixed-record round-trip plus the strict oversized-string
  failure path.

- **First working `libmadc` storage backend: `dsv://`.** The
  exploratory storage/federation API now has a real end-to-end C++
  vertical slice instead of sketches only. `src/madc_storage.cpp`
  adds the first runtime pieces for this subsystem: a concrete
  `DsvDriver`, a built-in `DataDriverRegistry`, a minimal
  `Query`/`QueryBuilder` implementation, and generic record-oriented
  driver operations over `madc::value` objects. `DataSet<T>` is now a
  usable typed C++ facade that composes `DataSource`, `DataMapper<T>`,
  `MappingSpec<T>`, and the driver registry. The first round-trip test
  in `tests/unit/test_libmadc_dsv.cpp` proves that ordinary host C++
  can persist and read back a mixed struct through `dsv://`, including
  key-based `get` / `update` / `erase`, mapped field renames, filtered
  scans, and CSV-style quoting for textual fields.

- **Exploratory `libmadc` storage/federation API sketches + contract
  tests.** Added the first public header sketches for a future typed
  storage layer under `include/libmadc/`: `datasource.h`, `schema.h`,
  `driver.h`, `mapper.h`, `dataset.h`, `relation.h`, and `query.h`.
  The model is C++-first and keeps direct/programmatic access primary:
  `madc::DataSource` is location-only, `SchemaInfo` describes inferred
  type layout, `DataMapper<T>` / `MappingSpec<T>` handle automatic
  mapping plus targeted overrides, `FormatAdapter<T>` covers irregular
  legacy formats such as SMAUG-style tagged text files, and SQL/GQL are
  planned as optional peer front-ends over a shared query layer. New
  doctest binary `tests/unit/test_libmadc_storage_contract.cpp`
  (now 10 cases) locks in the initial contract for `dsv://`, `flr://`, and
  `vlr://` planning: location-only URI parsing, mixed-type schema
  description, logical vs fixed vs variable record layouts, strict FLR
  overflow-by-default, truncation only by explicit opt-in, tombstone
  sidecars, and positional/offset/key/graph relation kinds.

- **Phase 4.2 / libmadc public C++ API: `madc::error`.** New header
  `include/libmadc/error.h` (and `src/madc_error.cpp`) ships the next
  public libmadc type as a structured diagnostic container mirroring the
  existing internal `Program::Diagnostic` shape: severity, phase, message,
  file, line, and column. It includes enum-name helpers, equality, a
  formatted `to_string()`, and a bridge helper
  `make_errors_from_program_diagnostics(const Program&)` so the public API
  can lift compiler/runtime diagnostics without exposing `Program`
  internals. New doctest binary `tests/unit/test_libmadc_error.cpp`
  (5 cases, 24 assertions) covers default construction, field storage,
  formatting, equality, and conversion from real `Program` diagnostics.

- **Phase 4/libmadc logging lifecycle fixes.** Built-in syslog/file/JSON
  sinks are now engine-owned helpers instead of anonymous lambdas appended
  into the generic `log_sinks` fanout. That fixes the re-enable / re-apply
  regression where `disable_*(); enable_*();` or repeated
  `apply_log_config()` calls on the same engine could accumulate stale sink
  callbacks and duplicate every later log line. Syslog configuration now
  also rebinds cleanly when `apply_log_config()` changes ident/option/
  facility. Covered by new doctest cases in `tests/unit/test_datadef.cpp`
  for file/json sink re-enable, config re-apply, and syslog reconfiguration
  state.

- **Phase 4.2 / libmadc public C++ API: `madc::value`.** New header
  `include/libmadc/value.h` (and `src/madc_value.cpp`) ship the first
  public type of the libmadc embedding API: a tagged value carrying
  the eight host↔script kinds (`null`, `boolean`, `integer`, `real`,
  `string`, `bytes`, `array`, `object`). `bytes` is `std::vector<uint8_t>`,
  `array` is `std::vector<value>` (heap-owned via `unique_ptr`), and
  `object` is `std::map<std::string, value>`. Copy is deep, move
  leaves the source in null state, equality compares structurally,
  and accessor mismatches throw `std::runtime_error`. `MadValue` (in
  `datadef.h`) is unchanged; it remains the internal php:: array
  helper. The two are deliberately separate. The umbrella public-API
  directory lives at `include/libmadc/` (mirroring the `libmadc.so`
  artifact name), so the existing `include/madc/` embedded-scripting
  header tree is untouched. New doctest binary
  `tests/unit/test_libmadc_value.cpp` (19 cases, 67 assertions)
  covers every kind, deep-copy / move-out, nested arrays of objects,
  equality across kinds, and accessor-mismatch throws.

- **Phase 4/libmadc log sinks: rotation, JSON, declarative config.**
  The file sink grew optional size-based rotation
  (`enable_file_sink(path, max_bytes, max_files)`) — when the next
  formatted line would exceed `max_bytes`, the existing file rotates
  through `path.1` … `path.<max_files>` and the oldest is dropped. A
  new `reopen_log_file()` lets a host integrate with logrotate-style
  external rotation by reopening the same path after an out-of-band
  rename. A new structured-fields sink
  (`enable_json_sink` / `disable_json_sink`) emits one JSON object per
  `write_log()` call with optional `ts`, `level`, and `message` fields,
  with a public static `json_escape()` and `format_json_log_line()` so
  hosts can build their own variant sinks. Topping it all off, a
  `MadcEngine::Config` struct + `apply_log_config()` give embedding
  hosts a one-call declarative surface to set threshold, timestamps,
  level prefixes, error-stream toggle, and any combination of file /
  syslog / JSON sinks. Twelve new doctest cases cover internal
  rotation + max_files cap, external rotate + reopen, json_escape
  edge cases, JSON line shape with and without timestamps, dual file +
  JSON routing through the `madc::<level>` facades, declarative apply
  + disable + unwritable-path handling.

- **Phase 4/libmadc log sinks.** `MadcEngine` now grows a runtime
  `log_threshold` (default `debug`, i.e. log everything) that gates
  both `write_log()` and the line buffering inside `MadcLogStreambuf`,
  so a filtered `madc::debug << ... << x` does not even accumulate
  per-character work. Added a sink-registry layer
  (`add_log_sink` / `clear_log_sinks` / `log_to_error_stream` toggle)
  so `write_log()` fans out to a list of `(LogLevel, message)`
  callbacks in addition to (or instead of) the formatted error-stream
  output. Built two concrete backends on the registry: a syslog sink
  (`enable_syslog_sink` / `disable_syslog_sink`, with a public static
  `syslog_priority_for(LogLevel)` mapping every level to the matching
  POSIX `LOG_*` priority) and a file sink (`enable_file_sink` /
  `disable_file_sink`, append-mode, formatted text matching the error
  stream). New doctest coverage exercises threshold filtering across
  `write_log` and the level streams, sink fanout / clearing, the
  syslog priority mapping for all eight levels, and the file sink for
  cross-engine append, error handling on unwritable paths, and routing
  through the `madc::<level>` facade streams.

- **Phase 4/libmadc level-stream facade.** Added `madc::emerg`,
  `madc::alert`, `madc::crit`, `madc::err`, `madc::warn`, `madc::notice`,
  `madc::info`, and `madc::debug` — eight `std::ostream`-shaped global
  level streams under namespace `madc`. They are line-buffered
  `std::streambuf` instances that flush each complete line through the
  bound engine's `write_log()` (so timestamps, level prefixes, and the
  active error sink — buffer / tee / future syslog — all apply
  uniformly). One call to `engine.bind_log_streams()` wires all eight
  to that engine; `MadcEngine::unbind_log_streams()` detaches them.
  Without an engine bound, the streams fall back to formatting through
  `std::cerr` so unconfigured embedding hosts still see output. Covered
  by five new doctest cases in `tests/unit/test_datadef.cpp`.

- **Phase 4/libmadc console-manager groundwork.** `MadcEngine` now
  provides standard-stream helper APIs for binding input/output/error,
  capturing output/error to owned buffers, teeing output/error to a
  secondary sink, and formatting levelled log messages with optional
  timestamps and level prefixes. This is the first caller-facing
  convenience layer for embedding hosts; it still uses standard C++
  stream machinery underneath. Covered by new doctest coverage in
  `tests/unit/test_datadef.cpp`.

- **First-wave C23 compatibility landed.** Added compile-time
  `_Static_assert` / `static_assert`, `alignof` / `_Alignof`,
  `typeof` / `typeof_unqual`, a typed `nullptr` literal, and C23
  digit separators in binary/hex/decimal/floating literals. The
  parser now evaluates richer integer constant expressions for static
  assertions (comparisons and logical `&&` / `||` in addition to the
  existing arithmetic/bitwise chain), `alignof` shares the existing
  type-query surface with `sizeof`, and `typeof(expr)` can drive
  ordinary declarations. Covered by `tests/teststaticassert.mad`,
  `tests/testalignof.mad`, `tests/testtypeof.mad`,
  `tests/testnullptr.mad`, and `tests/testdigitsep.mad`.

- **`rust::match` statement.** New namespaced statement form modeled
  after Rust's `match`. v1 surface: integer constant patterns,
  multi-pattern arms (`1 | 2 | 3 => ...`), `_` wildcard with free
  source-order placement, single-statement or block bodies, and no
  fall-through (every arm ends with an implicit jump out of the
  match, `break` still exits early). The lexer now recognizes `=>`,
  the parser dispatches `rust::match` at statement head only (so
  `match` remains a usable identifier in user code), and codegen
  emits a flat compare-and-jump chain over the patterns. Covered by
  `tests/testrustmatch.mad`. See `docs/language/rust-match.md`.

- **`ns_common` extracted from the four user-facing namespaces.** The
  `php::`, `python::`, `ruby::`, and `rust::` implementations were
  carrying duplicate copies of trim/replace-all/repeat/contains/
  starts_with/ends_with, the substring split/join loops, and a
  MadValue→string helper. They now all forward to a single set of
  helpers in `include/ns_common.h` / `src/ns_common.cpp`. The
  user-facing namespace surfaces are unchanged; `php_str_repeat`
  inherits the rust-side `count<=0` clear that was missing before.

- **Added a new `rust::` namespace.** v1 is intentionally small-surface
  and runtime-native: string helpers (`contains`, `starts_with`,
  `ends_with`, `trim*`, `replace`, `repeat`, `len`, `is_empty`) plus
  array helpers (`split`, `split_whitespace`, `join`, `first`, `last`,
  `get`, `push`, `pop`). This is Rust-flavored namespace sugar over
  existing madc `string` / `array` semantics, not an ownership or
  borrowing model. `tests/testrust.mad` covers the initial surface.

- **Added `prefer ...;` and `#pragma prefer ...` for namespace
  precedence.** Both forms now feed the same parser behavior and can
  reorder unqualified identifier lookup between namespaced helpers and
  normal C/madc lexical-global resolution. `c` is the fallback lane for
  ordinary identifier lookup, so `prefer rust, c;` lets bare `len(...)`
  resolve to `rust::len` before a same-named user function. Covered by
  `tests/testprefer.mad`.

- **Multi-return runtime path now executes correctly.** The original
  `testmultiret.mad` failure was split across both sides of the hidden
  `__retbuf` ABI: the callee wrote return slots through the stack-slot
  parameter as if it were already a Gp, and the caller never prepended
  the hidden retbuf pointer when invoking a multi-return function.
  Fixed by loading `__retbuf` into a real Gp inside `TokenRETURN::compile()`
  and prepending the caller retbuf in `TokenCallFunc::compile()`.
  `tests/testmultiret.expect` now locks in the runtime output `3 / 2 / 7 / 42`.

- **Namespace-call arguments can shadow same-named namespace members.**
  `ruby::chars(chars, s)` previously re-resolved the first argument back
  to `__rb_chars` because `current_namespace` leaked into `parseCallFunc()`
  argument parsing. Namespace members now resolve first only at
  expression head, and `parseCallFunc()` / `parseCallMethod()` suspend
  `current_namespace` while parsing argument expressions.
  `tests/testrubycharsshadow.mad` covers the regression, and
  `tests/testlang.mad` now exercises `ruby::chars` again.

- **php::array_column() and nested-array values.** `MadValue` can now
  deep-copy and destroy nested `array` values, `php::array_push_array()`
  appends nested arrays by value, and `php::array_column()` extracts an
  integer-indexed column from an array of nested arrays.
  `tests/testphp.mad` covers `array_column`, with `tests/testphp.expect`
  asserting the output.

- **TokenAssign: subscript-assign value is LHS-typed, honors caller
  dest** (a59adbb).  Two issues collapsed in the TokenSubscript /
  TokenSubscriptExpr branches: (a) the expression value of
  `arr[i] = x` for narrow-integer element types was the unbound RHS
  register's full int instead of the byte / short that was actually
  stored, sign-/zero-extended back to int64; (b) the branch
  unconditionally overwrote `regdp.first`, so outer
  `r = (buf[i] = x)` had its mirror-to-caller logic confused and r
  never got written.  Visible victim: SMAUG
  `while ((BUFF[num]=fgetc(fp)) != EOF) num++;` in `send_*_title()`
  and `show_file()` walked off the buffer because the EOF compare
  saw the unbound int.  Worked around for v0.13.0 by
  `MadSMAUG/patches/madc-fgetc-loop.patch` substituting an int
  intermediate; that patch is now retired.
  `tests/testsubscriptassign.mad` covers small-positive, EOF marker,
  high-bits-truncate, and the SMAUG fgetc-loop pattern.

- **switch: emit `default:` body in source-order position** (396c147).
  When `default:` appeared before the case labels in source order
  (the SMAUG colorize idiom), the compiler emitted all case bodies
  first and then the default body at the end — so a case with no
  break would fall through into the default body, including the
  synthetic fall-through from the unlabeled tail code that follows
  the last explicit case.  Visible victim: SMAUG `make_color_sequence`
  returned -1 for every &X colour code, so all `&Y/&G/&C/&w` in
  prompts and help text came through with the `&` stripped and the
  letter passed verbatim.  Fix tracks `default_index` — the
  source-order position among the cases — and emits the default body
  there.  `tests/testswitchdefaultorder.mad` covers default-first,
  default-middle, default-last with intentional fall-through.
  Visible result: SMAUG room titles, prompts, status-line, hint
  banners, and help entries all render in correct ANSI colour.

## [v0.13.0] — 2026-04-30 — SMAUG 1.8 plays end-to-end on madc

- **SMAUG 1.8 is a fully playable network MUD on madc.** The
  158k-line C89 codebase JIT-compiles in-process, accepts telnet
  connections, walks the full character-creation flow (name →
  confirm → password → retype → color → sex → class → race →
  stats roll), serves the MOTD, drops the player into "Ominous
  Tapestries" with a newbie burlap sack, and responds to in-game
  commands: `look`, `inventory`, movement (`n`/`s`/`e`/`w`/`u`/`d`),
  `say`, `who`, `quit`. Returning-player `Reconnecting.` flow also
  works. NPCs greet, bow, and offer advice.

- **Four real codegen / lexer fixes this session.** Each
  collapsed multiple SMAUG runtime symptoms into one root cause:

  - **Lexer: octal (`\NNN`) and hex (`\xHH`) escape sequences.**
    The lexer previously parsed `\033` as `\0` (NUL terminator)
    followed by literal `"33"`, silently truncating the string.
    SMAUG's `make_color_sequence()` then `sprintf`'d into a
    zero-length format and the next line `buf[ln-1] = 'm'`
    wrote 'm' (0x6D) to `buf[-1]` — out of bounds into a
    caller's stack frame. The corruption clobbered byte 7 of
    `nanny()`'s `DESCRIPTOR_DATA *d` slot, turning `0x55…` into
    `0x6d0055…`, and the next `write_to_buffer` SIGSEGV'd
    dereferencing the bogus pointer (the long-standing
    "comm.c:1381 second-connection NULL deref"). Full C escape
    syntax now supported: `\NNN` (1-3 octal digits), `\xHH`
    (1-2 hex digits), plus the existing single-letter escapes.
    `tests/testoctalescape.mad`.

  - **`scanf`-family rewrites `%d` → `%ld`.** madc's `int` is
    64-bit by design, but libc's `%d` writes only 4 bytes into
    a destination slot — leaving the high 4 bytes of an
    `int x = 0; sscanf("%d", &x);` round-trip stale. SMAUG
    `db.c`'s `sscanf("%d %d ...", &x1, &x2, ...)` produced
    `0x00000000FFFFFFFF` for negative inputs read from `.are`
    files; that broke `slot_lookup`'s `if (slot <= 0) return -1;`
    guard for the first negative slot during `gods.are` object
    loading and `abort()`'d boot. Three new wrappers
    (`__madc_sscanf` / `__madc_fscanf` / `__madc_scanf`) parse
    the format string and prepend `l` to `%d/%i/%u/%o/%x/%X/%n`
    that have no explicit length modifier, then forward to
    `vsscanf`/`vfscanf`/`vscanf`. Embedded `<stdio.h>` `#define`s
    `sscanf`/`fscanf`/`scanf` to redirect transparently — no
    source changes needed. `tests/testsscanfwide.mad`.

  - **`stat` family added to the int32-return whitelist.**
    `stat` / `fstat` / `lstat` etc. return `int` (-1 on failure)
    but were missing from the dlsym-int-returner whitelist that
    triggers `movsxd` of the 32-bit result into the 64-bit RAX
    madc reads. Without sign extension, a -1 return arrived as
    `0x00000000FFFFFFFF` and `if (stat(p, &sb) == -1)` silently
    fell through to the success branch. SMAUG `save.c:891`
    logged spurious `Preloading player data ... (-15803487K)`
    on every connection because the stat-failed path never
    short-circuited. Added `stat`, `fstat`, `lstat`, `fstatat`,
    `statfs`, `fstatfs`, `utime`, `utimes`, `futimes`.
    `tests/teststatret.mad`.

  - **`safemov` narrow→64 must sign-extend across the full 64
    bits.** The signed-sub-int → 64-bit-dest path used
    `movsx r32, r/m8` (and similar for `r/m16`). x86 `movsx` to
    a 32-bit register sign-extends to the dest's 32 bits and
    then implicitly zero-extends to 64 — leaving the high 32
    bits at zero. So a signed -1 char loaded into a Gpq came out
    as `0x00000000FFFFFFFF`. Visible victim: the chained EOF
    idiom `int x = (c = fgetc(fp)); if (x == EOF) break;` —
    the assignment-expression value extended through this
    `safemov` didn't match an int64 sentinel. Fixed by using
    `movsx r64, r/m8` (and `r64, r/m16`) directly when the dest
    is gpq. Unsigned narrows continue to use the shorter
    `movzx r32` form whose implicit zero-extend to 64 is
    correct. `tests/testsignextend.mad`.

- **MadSMAUG: all bootstrap shims removed.** `slot_lookup`,
  `act`, `to_channel`, `boot_log` all run upstream definitions.
  The `_bootstrap_comm_shim.c` is now just a doc comment.

- **MadSMAUG: upstream `fgetc`-into-char-array idiom patched.**
  `act_comm.c`'s `send_*_title` (4 sites) and `db.c`'s
  `show_file` / `show_file_vnum` (2 sites) used the chained
  `(buf[i] = fgetc(fp)) != EOF` idiom; this returns the unbound
  RHS register's full int instead of the truncated-and-extended
  char value the C standard requires, so the EOF compare never
  matched and the loop walked off the buffer. The patch
  (`patches/madc-fgetc-loop.patch`) substitutes an `int`
  intermediate at the six call sites. The TokenVar variant of
  the same pattern (`(c = fgetc(fp))`) works correctly under
  this release; the TokenSubscript variant is queued for a
  follow-up codegen fix and the workaround patch comes out then.

## Previous unreleased work (now part of v0.13.0)

- **SMAUG runtime is fully interactive end-to-end.** boot_db
  completes through every load_*() phase, game_loop ticks at
  4 Hz, area_update fires every 30-90 s with `Resetting:` log
  lines, telnet returns the greeting, and the character-creation
  dialog progresses through Name → "Did I get that right? (Y/N)"
  → password prompt (with telnet IAC `ff fb 01` echo-suppression)
  → password retype → color preference. Clean client disconnect
  handled. First time SMAUG under madc has been genuinely
  playable as a network-protocol MUD.

- **Five real codegen / parser bugs fixed this session.** Each
  one was a real divergence from gcc's behavior on the exact same
  upstream source, and each had a one-line repro:

  - **TokenVar enum const-fold**: `TokenOperator::optimize` calls
    `ival()` / `dval()` on each leaf when both sides report
    `is_constant()`. `TokenVar` reported is_constant correctly for
    vfCONSTANT vars (enum members, `static const int`) but
    inherited the default returning 0. So `enum { BASE=1024, ...,
    TOP }; (TOP - BASE)` folded to **0** at runtime even though
    parse-time array sizing read the same expression correctly via
    `read_constant_integer`. Visible victim was SMAUG colorize.c's
    `for (at=0; at < AT_MAXCOLOR; ++at)` running zero iterations
    and the file-read loop's `DISPOSE` macro spamming ~57
    `DISPOSEing NULL in colorize.c, line 40` lines per boot. Fix:
    9-line override of `ival()` / `dval()` on `TokenVar`.
    `tests/testenumconstfold.mad`.

  - **Brace-less comma-expression statements**: `parseExpression`
    treats `,` as a hard stop because every other caller (function
    args, for-init/incr separators) needs it that way. So a body
    like `while ( (*p = *i) != '\0' ) ++p, ++i;` parsed as
    `++p;` and the `, ++i;` became a sibling statement AFTER the
    loop. `*i` stayed at the first char forever, `p` walked off
    the buffer, SIGSEGV — found via SMAUG mud_prog.c:2437. Fix:
    new `parseExpression(push_back_comma=true)` flag, new
    `parseExprStmt` helper called from parseStatement's
    expression-statement branch, new `TokenComma::compile` that
    evaluates left for side effects and returns right's value.
    `tests/testcommastmt.mad`.

  - **`setrlimit` resource guards**: codegen bugs that compile to
    spinning loops (the comma-stmt bug above was one) used to pin
    the host at 99% CPU until killed by hand. Add
    `MADC_CPU_LIMIT=<seconds>` (default 60, RLIMIT_CPU →
    SIGXCPU/SIGKILL) and `MADC_MEM_LIMIT=<MB>` (default 2048,
    RLIMIT_AS — covers JIT mappings + dlopen libs). Both
    overridable, both have a `=0` disable knob.

  - **Function-scope static initializers**: TokenDecl::compile
    skipped the inline initialize-on-every-call code for
    vfSTATIC vars (correct C semantics — static init must fire
    exactly once, before main), but nothing was feeding the
    initializer into program-startup code instead. So
    `static char const *p = "literal";` left p NULL. SMAUG
    mud_comm.c's `get_color` SIGSEGV'd inside
    `strstr(color_list, color)` — `color_list` was the static
    pointer that never got its literal address. Fix: in
    parseDeclaration, push the wrapped TokenAssign onto
    `tkProgram->statements` when the decl is a function-scope
    static with `=`-init. Counter-pattern statics
    (`static int n = 5; n++;`) keep working — the init still
    fires only once. `tests/teststaticlocalinit.mad`.

  - **Real-typed global Xmm load/store via reg-base addressing**:
    three independent gaps, all in the same TokenCpnd::movreg path.
    (1) Missing `else` between the kVec and kGp arms: an Xmm
    operand fell through to the Gp branch's bare `else` clause
    which threw "unsupported operand". (2) `movsd xmm, [abs64]`
    has no encoding — only `mov rax, [moffs64]` for the GP analog
    (which is why integer globals worked: asmjit's reloc system
    patches the moffs64 form, but movsd has nothing to patch). At
    cc.finalize() asmjit aborted with `Reloc entry contains
    address that is out of range (unencodable)` for any heap
    pointer above the 32-bit signed range. Spill the address into
    a Gp first and use `[gp]` addressing — always encodes. (3)
    `movxval2mptr` (the Xmm-to-mem write-back) didn't exist, and
    `TokenCpnd::putreg` only had a Gp branch — even when reads
    were fixed, writes silently no-op'd and globals stayed at
    zero. Add the helper and the kVec branch.

- **MadSMAUG `act()` un-stubbed.** All four bootstrap-shim
  function bodies that earlier sessions thought were "variadic
  pipeline corrupts the heap" turn out to be layout-shift
  symptoms of the comma-stmt and real-global codegen bugs above.
  With those fixed, the upstream `act()`, `to_channel`,
  `boot_log` definitions stand on their own. Only `slot_lookup`
  remains stubbed (next-session blocker, see below).

- **Open issue blocking slot_lookup un-stub**: a real madc bug
  that only manifests in the SMAUG umbrella context (5800+
  functions). Inside slot_lookup, `if (slot <= 0) return -1;`
  fails to early-return for slot=-1. Probes confirm:
  `sizeof(slot)=8` (madc int is 64-bit by design), low 32 bits
  read as -1 correctly, high 32 bits are zero (NOT
  sign-extended), the 64-bit cmp sees positive 4B and the guard
  fails. Cannot reproduce in a small repro — SMAUG-umbrella
  specific. Three hypotheses documented in
  MadSMAUG/src/_bootstrap_comm_shim.c. Workaround keeps an
  unconditional `return -1` stub.

- **MadSMAUG accepts telnet connections and sends the login
  greeting** — `Welcome to MadSMAUG. By what name do you wish to
  be known?` — the first interactive frame from a JIT-compiled
  SMAUG. The path from `boot_db()` to a responsive `game_loop()`
  surfaced one madc bug:

  - **Function-local `extern T name;` resolves to the file-scope
    global, not a fresh uninitialized local.** addVariable was
    checking only the local scope before creating a new
    Variable; comm.c new_descriptor()'s
    `extern char *help_greeting; if (help_greeting[0] == '.')`
    crashed on every incoming connection because the local
    extern shadowed db.c's actual global with an uninitialized
    NULL pointer. Fix: when `parsing_extern_decl`, fall through
    to `tkProgram->findVariable(id)` and reuse the existing
    global. Regression test
    `tests/testfunclocalextern.mad` covers the read-and-write
    case across two calls.

  Plus bootstrap-shim peeling on the MadSMAUG side: the
  send_to_char / write_to_buffer / send_to_pager /
  send_to_char_color / set_char_color stubs were removed so the
  upstream comm.c definitions win the funcnode-dedupe race;
  slot_lookup gained a stub (the upstream version aborts during
  boot when skill_table is empty, which exposed itself only
  after the extern fix wired fBootDb visibility correctly inside
  it); main() in SMAUG.mad now drives `boot_db() →
  init_socket(port) → game_loop()` instead of `exit(0)`.

- **SMAUG `boot_db()` runs end-to-end** — five compounding fixes in
  one session unstuck the post-area-loading runtime path. SMAUG now
  loads all 25 areas, fires `area_update`, finishes board / vault /
  clan / member-list / council / deity / watch / ban / corpse /
  immortal-host / hint / project / morph / login-message / color
  loading, and reaches `[probe] after boot_db` in the bootstrap
  shim. Runtime estimate ~75% → ~95%+ (boot complete; the only
  remaining surface is the game loop, which the bootstrap doesn't
  invoke). Five fixes, in order found:

  1. **Mixed string-literal / char-pointer ternary type
     unification** — `feof(fp) ? "End" : fread_word(fp)` had its
     parser-side datadef set to dtSTRING (true branch wins), so the
     downstream `char *` consumer ran `string_cstr` on the false
     branch's raw `char *` return, dereferencing it as if it were a
     `std::string` and crashing inside libstdc++. The parser now
     unifies pointer-flavored ternary branches: real pointers,
     dtSTRING string literals, fixed-array variables, and
     fixed-array struct members are all "char-pointer-like"; when
     they disagree the result type is a real pointer (or
     `char *` / `ddLPSTR` if both are decay/literal). Closes
     boards.c:1615, fight.c:4298 `IS_NPC(victim) ? buf2 : ""` (buf2
     is `char[N]`), player.c:1883 `(x == lvl) ? buf : (x == lvl+1)
     ? buf2 : " exp"`, and the dozen `obj ? obj->field : "(none)"`
     calls in act_wiz.c do_mstat. **(closes the boards-loading
     SIGSEGV.)**
  2. **TokenTerQ::compile merge slot rewrite + IRBuilder coerce
     extensions** — the existing merge_slot path called
     `compile_token_normalized` whose tmp_rdp.second was
     pre-seeded to the *target* type, not the branch's actual
     type, so a dtSTRING literal on a char* merge surface got
     relabeled char* without ever calling `string_cstr`. New
     emit_branch lambda compiles each branch with a clean
     regdefp_t, then routes the produced operand through
     `IRBuilder::coerce(raw_type → merge_type)`. Two new coerce
     pairs: dtSTRING → pointer-to-char (emits `string_cstr` to
     yield the c_str() char *) and 8-byte integer → dtSTRING
     relabel (covers dlsym-fallback functions like `ctime` whose
     return type is `char *` but parses as int64).
  3. **Local C fixed-size array LEA re-emit on every reuse** —
     `voperand` cached the Gp holding a stack-array's base pointer
     but only re-emitted the LEA for *global* fixed-arrays. SMAUG
     `bug()` declares `char buf[MAX_STRING_LENGTH]`, first uses it
     inside `if (fpArea != NULL) sprintf(buf, ...)`, then
     unconditionally `strcpy(buf, "[*****] BUG: ")` after the if.
     With fpArea NULL (the load_vaults phase), the LEA inside the
     not-taken branch never executes; the cached vreg is
     uninitialized and the strcpy lands on NULL inside libc
     memcpy. New `fixed_array_stack` map on TokenCpnd remembers
     the stack Mem so reuse can re-LEA into the cached Gp,
     mirroring the existing global-fixed-array re-emit pattern.
  4. **Crash-handler stack walk for non-JIT faulting RIP** — when
     a JIT'd function calls into libc and the fault happens inside
     glibc (e.g. memcpy on NULL dst), the existing handler reads
     RIP from `ucontext_t::uc_mcontext.gregs[REG_RIP]`, finds it
     outside the JIT region, and prints no source-line context.
     Walk `backtrace()` looking for the first frame whose address
     falls in the JIT'd region; that's the call-site that pushed
     the return address into libc. Surfaced db.c:4225 strcpy as
     the actual fault site behind the load_vaults segfault — a
     diagnostic that paid for itself within minutes.

- **TokenLand / TokenLor: actually short-circuit && and ||
  evaluation** — both operators compiled left AND right
  unconditionally before testing either, so `p && p->next` evaluated
  p->next even when p was NULL. Move right-operand compilation
  behind the short-circuit branch. Found within minutes of having
  the JIT-crash → source-line tooling working — handler.c:132 was
  the last anchor, the && expression was the obvious culprit.

- **JIT crash → source-line traceback** — sorted byte-offset →
  (file, line, col, kind) source map built at `cc.finalize()` from
  per-statement / per-function-entry label anchors; SIGSEGV /
  SIGBUS handler reads RIP from `ucontext_t::uc_mcontext.gregs[REG_RIP]`,
  binary-searches the map, prints both the last anchor and the next
  anchor so the user can see the bracket of source that emitted the
  crash. Toggle off with `MADC_NO_SOURCE_MAP=1`. Dump the full map
  with `MADC_DUMP_SOURCEMAP=path`. Three companion fixes were
  needed for the file path to be useful: (1) parseFunction now
  copies file/line from the first body statement so TokenFunc
  fn-entry anchors point at the function body, (2) the lexer was
  storing `c_str()` of stack-local `std::string` into
  `TokenBase::file` at #include time — pointer dangled the moment
  the include scope ended; intern via the existing `included_files`
  map (whose std::string keys are stable since we never erase),
  (3) crash handler print formatting cleaned up.

- **TokenSubscriptExpr: override operand() to return Mem lvalue
  without loading value** — the default `operand()` called
  `compile()` which emits `emit_ir_value` to LOAD the element via
  the computed Mem and yield a Gp holding the value. Callers that
  expected an lvalue (TokenAssign LHS for nested subscripts, outer
  TokenSubscriptExpr for chained 2D indexing, TokenMember for
  `s[i].field`) treated that Gp as an *address* and indexed off the
  value — for 2D-array struct member writes
  `m->map[0][0] = 7` the inner subscript loaded the int value at
  &map[0][0] (zero in calloc'd memory) and the outer subscript
  wrote to NULL. Override operand() to mirror compile()'s address
  calculation but return the Mem operand directly. SMAUG `load_rooms`
  initializes `map_index->map_of_vnums[i][j] = -1` over a 49×79 int
  grid inside MAP_INDEX_DATA — every element write went through the
  value-as-address path before the fix. After the fix limbo.are AND
  the rest of the area list (25 files total) load end-to-end.

- **TokenCallFunc: spill (rax, rdx) to stack for small-struct
  return-by-value** — SysV x86-64 returns aggregates of 1..16 bytes
  in (rax, rdx). Madc's FuncSignature only carries a single TypeId
  for the return, so cc.invoke captured rax alone — downstream
  struct copies treated that 8-byte value as a *pointer* and
  segfaulted dereferencing arbitrary bytes. Concrete failure (SMAUG
  mobile loading): `pMobIndex->act = fread_bitvector(fp);` where
  EXT_BV is 4 ints. Mobile act_flags 1073741825 ended up packed
  into rax with the next int; struct memcpy crashed at
  `movups (%rsi=0x40000001),%xmm0`. Fix: for struct returns of size
  1..16, allocate a 16-byte stack slot, capture rax via setRet,
  emit `mov [slot+0], rax_vreg` and (for >8-byte returns) the rdx
  spill `mov rdx_vreg, x86::rdx; mov [slot+8], rdx_vreg` as the
  first instruction after the InvokeNode (before asmjit's allocator
  reuses rdx). Operand becomes the slot's LEA'd address, which is
  what struct-aware consumers expect. SMAUG now loads gods.are
  end-to-end: "gods.are : Rooms: 1200-1201 Objs: 1200-1200 Mobs:
  1200-1200".

- **parseDeclaration: allocate storage when promoting extern to
  definition** — when a global was first seen as `extern T name;`
  and later defined without `extern`, addVariable returned the
  existing Variable* without running the allocate-storage path —
  var->data stayed NULL with vfSTACK still set, and every function
  that referenced the global created a fresh stack-local instead.
  Concrete failure (uncovered with the SMAUG umbrella's `last_area`
  pointer): mud.h declared extern, db.c defined it, load_area set
  it; load_author saw NULL because both lived in their own stack
  copies. Fix calloc's storage when transitioning out of extern at
  file scope (alloc=true, scalar with non-zero size, not a function
  type) and clears vfSTACK / sets vfALLOC.

- **static-local struct: allocate persistent storage; thread `static`
  flag through `static struct X x;` path** — `static struct A x;`
  inside a function was stack-allocated. TokenSTATIC::parse handed
  off to parseKeyword for the `struct` token, which routed through
  TokenSTRUCT::parse → parseDeclaration *without* its is_static
  parameter set. Voperand allocated via cc.newStack instead of
  addressing the calloc'd heap backing store; `&x` returned a stack
  address and persistence across calls was lost. Two-part fix:
  (1) voperand path that loads `mov base_reg, imm(var->data)` and
  returns Mem indexed off it for global structs, mirroring the
  existing global fixed-array path; (2) new
  `Program::parsing_static_decl` flag (analogous to parsing_extern_decl)
  that TokenSTATIC sets before parseKeyword and parseDeclaration
  ORs into `gotstatic` then immediately clears so nested locals in
  the function body don't inherit static storage.

- **TokenAddrExpr: compute &arr[i] without going through value-load
  path** — `&arr[i]` returned the *value* at arr[i] instead of its
  address. The fallthrough case called `expr->operand(pgm)` which
  for TokenSubscript inlines through to `compile()` — which loads
  the element via emit_ir_value. SMAUG's `init_mm` hits this with
  `int *piState = &rgiState[2];` and the negative-subscript
  centered-indexing trick wrote to garbage. New TokenSubscript
  branch in TokenAddrExpr mirrors the fixed-array address calc
  (load base, widen index, LEA [base + idx<<shift]).

- **TokenCpnd::voperand: re-emit global fixed-array base on every
  reuse** — global fixed-arrays (`char buf[256];` at file scope)
  cached the first `mov reg, imm(addr)` and skipped the reuse-emit
  path other globals took. The populating mov is itself an
  instruction, and asmjit only sees it on the first control-flow
  path. Subsequent uses on a divergent branch read an uninitialized
  vreg → stack/garbage address. Concrete failure: a function that
  wrote a global fixed-array on one if-branch and returned it on
  both; the else-branch return was a stack address.

- **TokenMember::operand: LEA fixed-array struct members instead of
  loading first byte** — a struct member declared as a fixed array
  (`char d_name[256]`) lives in-place. As an rvalue it decays to a
  pointer to its first element. Madc was returning a Mem operand of
  size sizeof(element) at the array's start, so any value-context
  use loaded the first byte. Concrete failure: `printf("%s",
  dentry->d_name)` segfaulted in libc sprintf — we'd passed
  `(uint8_t)'.'` instead of the address of d_name. All six branches
  of TokenMember::operand updated to LEA when
  `is_fixed_array_member()` is true.

- **compiler: dedupe pending_funcs by FuncDef so duplicate definitions
  don't poison asmjit's funcnode binding** — the MadSMAUG umbrella has
  ~125 functions defined twice (once in upstream files, once stubbed in
  `_bootstrap_comm_shim.c`). Both definitions shared one FuncDef +
  FuncNode in `funcdef_map`. Each TokenFunc::compile called
  `cc.addFunc(funcnode)` for the same FuncNode — asmjit's Compiler v1.14
  silently dropped the labels of every funcnode added between the
  duplicate addFunc calls. Out of 1878 SMAUG user functions, only 168
  ended up with bound labels at finalize; the other 1710 calls emitted
  as `call $+5` (zero-displacement), pushing extra return addresses
  that corrupted wrapper-frame ret pops. `boot_db` SIGSEGV'd at
  0xfffffffffffffff0 before `show_hash` could even run. Fix walks
  `pending_funcs` in reverse, marks earlier TokenFuncs sharing a
  FuncDef as `is_overridden`, and short-circuits both `prepareFuncNode`
  and `TokenFunc::compile` for them so asmjit sees exactly one addFunc
  per FuncNode (the LAST source definition wins — matches the user's
  expectation that shim stubs override upstream defs). Result: 0/1878
  unbound. SMAUG `boot_db` now runs through 12+ init phases (Loading
  commands → sysdata → socials → skill table → classes → races → news →
  stances → herbs → tongues → make_wizlist) before hitting a missing-
  data-file `readdir(NULL)` crash. Runtime coverage 5% → ~30%+. Also
  adds `MADC_DUMP_FINAL=path` env knob — post-finalize per-function
  machine-byte dump (unlike emit-time `MADC_DUMP_ASM`, which truncates
  after the register allocator pass).

- **parser: move istream getline into std:: namespace** — `getline(istream&,
  string&)` was registered globally via `addFunction("getline", ...)`,
  which collided with user-defined `getline` (e.g. SMAUG IMC's
  `static const char *getline(char *buffer)`) — call sites resolved to
  the istream form with the wrong arity and the parser errored
  "Incorrect number of parameters: expected 2 got 1". Moved
  registration to `__std_getline` and aliased into
  `namespace_map["std"]["getline"]` so the unqualified spelling is
  reachable only via `std::getline(...)` or `using namespace std;`.
  Fixed `using namespace` to register an alias `Variable` when the
  namespace key doesn't match the underlying name — without it,
  `__std_X` imported as `X` wouldn't resolve under
  `findVariable("X")`. `tests/testfstream.mad` and `tests/testloop.mad`
  updated to add `using namespace std;`.

- **IRBuilder::coerce: dst=void fast path for statement-discard sites**
  — Compile path that synthesizes an unnamed function-pointer or
  struct-typed value and immediately discards it as a statement
  expression hit the type-check ladder with `src.type->name` empty
  and dst=void, throwing "unsupported type conversion (src= ->
  dst=void)" with no useful position info. Surfaced while ingesting
  MadSMAUG IMC sources (imc-mail.c `imc_recv_mail` body declares
  `imc_packet out;`, whose typedef chain has unnamed inner
  DataDefs). Fast path matches what the ladder would do anyway —
  pass through.

- **lexer: skip GCC `__attribute__((...))` decorations** — Treat
  `__attribute__` as a no-op at the lexer level: when the keyword
  is seen, consume the matching outer parenthesised payload and
  re-enter the tokenizer. Required for IMC's `iced.h`
  (`__attribute__((format(printf,1,2)))`) and any C codebase that
  decorates declarations with `noreturn` / `aligned` / `unused` /
  `visibility` / `const`. Without this the parser bailed mid-
  declaration with "Expecting brace after function declaration"
  because it tried to interpret `__attribute__((...))` as the start
  of a new function declarator. Regression: `tests/testattribute.mad`.

- **lexer: implement C token-paste operator (##) in function-like macros**
  — After parameter substitution, scan the expanded body for `##`
  and strip it (along with surrounding whitespace) so the lexer
  fuses adjacent identifiers when it re-tokenizes. Required for
  IMC's color-code pattern `#define COL(x) C_##x` — without `##`
  support, expanding `COL(b)` produced literal `C_##b` which the
  parser saw as undeclared identifier `C_`. Also adds a minimal
  `<sys/ioctl.h>` embedded header (FIONREAD / TIOCINQ / TIOCOUTQ).
  Regression: `tests/testtokenpaste.mad`.

- **compiler/IR: drop spurious finalize ret + clean up codegen mismatches**
  — Five fixes that drove the SMAUG umbrella's `MADC_VALIDATE` error
  count from ~50 to 2. (1) `_compiler_finalize` no longer emits a
  trailing `cc.ret()` outside any active function — it had been
  producing a noise-storm of `InvalidInstruction` errors that masked
  real codegen bugs. (2) `bind_call_return` narrow_int_ret path
  emitted `mov gpw/gpd, gpq` when a libc dlsym int return fed a
  narrower destination; asmjit's intermediate validator silently
  rejected that and left the destination uninitialized. Fix routes
  through `dest.r64()` so the encoder sees `mov gpq, gpq`; the
  destination vreg's natural sub-word width still truncates
  downstream consumers. (3) `safemov(Gp,Gp)` in the
  same-or-narrower-dest branch hits the same root cause for direct
  callers — force both ends to r64() matching the safemul/shl/shr/
  or/and/xor pattern. (4) `IRBuilder::load` clamps aggregate Mem
  sizes (>8) down to 8 and explicitly `setSize()`s the local Mem
  copy so the encoder doesn't reject unsized operands with
  `InvalidOperandSize`. (5) `IRBuilder::store` clamps the Mem dest
  the same way and avoids the widening branch when both ends are
  already gpq. Also: `TokenAssign` subscript-write path now routes
  the index through `load_idx_to_gpq` so a sub-word index (postinc
  on sh_int / char) gets sign/zero-extended before being added to
  the base. New regression: `tests/testnarrowdlret.mad` covers the
  bind_call_return fix end-to-end.

- **compiler: track current function name + widen extra-index Gp adds**
  — `Program::cur_func_name` is set at TokenFunc::compile entry. The
  MADC_VALIDATE error handler now prints `in function: <name>` for
  every asmjit error, converting the previous noise-storm of
  `InvalidArgument` errors at end-of-file into a per-function trail
  pointing at the actual offender (e.g. `do_mstat` (7 errors),
  `pull_type_name` (5), `do_showrace` (3) in the SMAUG umbrella).
  Separately, the multi-dim fixed-array subscript path's
  `cc.add(idx_reg, ex_op.as<x86::Gp>())` widens `ex_op` through
  `load_idx_to_gpq` so a sub-word extra-index (sh_int / char) doesn't
  produce `add gpq, gpw`.

- **MADC_DUMP_ASM env knob** — env-gated asmjit FileLogger captures the
  complete instruction stream (mnemonics + machine bytes + immediate
  explanations + register-cast annotations) to a file. Used to localize
  InvalidInstruction / InvalidArgument errors in the SMAUG umbrella
  where curToken-based localization runs out of signal once asmjit's
  pass-2 register allocator fires after token compile. Off by default.

- **TokenOperator::settype: propagate pointer / fixed-array type
  through arithmetic** — `buf + strlen(buf)` where `buf` is a fixed
  `char[N]` was being typed as plain `char` (because settype only
  checked is_real / is_integer). For variadic dlsym call paths the
  packing then inserted `addArgT<char>()` (1 byte), asmjit truncated
  the 64-bit pointer to a single byte at the call site, and the callee
  received e.g. 0xb0 (low byte of stack address) instead of the real
  pointer. SMAUG's `boot_log` call `vsprintf(buf+strlen(buf), str,
  param)` SIGSEGV'd at 0x36-ish addresses. settype now also propagates
  pointer-typed operands and synthesizes a pointer type from a
  fixed-array TokenVar via getPointerType. After this the first
  SMAUG `boot_log` line prints correctly:
  `Thu Jan  1 00:00:00 1970 :: [*****] BOOT: ---[ Boot Log ]---`.

- **Fix asmjit instruction-size mismatches: subscript indices and IR
  stores** — asmjit's encoder rejects mixed-width Gp/Gp instructions
  (`mov gpq, gpw`, `imul gpw, gpq`, `and gpb, gpq`). The SMAUG
  umbrella's density of sub-word integer types (`sh_int`, `char`,
  `unsigned char`) lit them up — main never actually ran because the
  JIT machine code was incomplete. Three fixes:
  - New `load_idx_to_gpq()` helper widens sub-word index Gps via
    movsxd / movsx in all six subscript call sites
    (sub / cmpd_sub / cmpd_subptr / subexpr).
  - `IRBuilder::store()` widens a sub-word source Gp before storing
    into a wider Mem (sh_int call return into qword stack slot).
  - `safemul` / `safeshl` / `safeshr` / `safeor` / `safeand` /
    `safexor` now force both Gp operands to r64() before emitting
    the op. The destination vreg's natural width is preserved
    (asmjit Compiler treats r64() of a gpw as the same vreg via the
    64-bit view).
  - Plus `MADC_VALIDATE=1` env knob installs an asmjit ErrorHandler +
    `kValidateIntermediate` so each ill-formed instruction is flagged
    at emit with mnemonic and source token.

- **compiler: always print cc.finalize() errors** — was DBG-only,
  silent in normal builds. Symptom: a program with a finalize error
  compiles, exits 0, but main never actually runs (the JIT machine
  code is incomplete). For non-trivial programs this looked like a
  successful no-op. Promoted to unconditional stderr output with
  `asmjit::DebugUtils::errorAsString()` for the kind name.

- **TokenRETURN: handle `return void_call();` in void-returning fn**
  — C allows `return some_void_call();` in a void-returning function;
  the inner expression runs for side effects, no value to ret.
  Compiler was sending the empty-Operand result of the void call
  straight to saferet, which threw "operand is not register,
  immediate, or memory". Detect ret_type as bare void (rawtype
  dtVOID && !is_pointer — the pointer guard keeps `void *` returning
  functions on the regular pointer path), compile expression with a
  discarded regdp, emit a bare cc.ret().

- **C99 variable-length array (VLA) support** — `T name[expr]` where
  `expr` references a runtime value now compiles. The variable is
  retyped as `T *` internally and laid out as a stack-resident
  pointer slot. At scope entry voperand emits
  `name = malloc(expr * sizeof(T))`; the matching free fires from
  TokenCpnd::cleanup before any object destructors. Subscript and
  pointer-arith reuse existing pointer paths because the variable
  type is now DataDefPTR.
  - Detection (parser): scan ahead from each `[` for any
    ttIdentifier that resolves via findVariable to a non-vfCONSTANT
    variable. Constants (#defines were already lex-expanded; enum
    values and `const int N` are vfCONSTANT) keep the parse_constant
    path. No-match identifiers also keep parse_constant so typedef'd
    integer constants and other lookup-retry idioms aren't mis-
    classified.
  - TokenDecl::compile forces voperand for VLAs at the decl point —
    otherwise lazy emission could put the malloc inside a loop body
    and re-execute every iteration, wiping prior writes.
  - TokenSubscript::compile / compile_set load the pointer through
    the Mem slot before indexing (was assuming Gp).
  - Surfaced by SMAUG `build.c:6010` `char temp_buf[MAX_STRING_LENGTH
    + max_buf_lines]`. Regression: `tests/testvla.mad`.

- **safeadd: handle Xmm-lhs/Gp-rhs and Gp-lhs/Mem-rhs** — mirrors the
  earlier safediv mixed-operand widening. Xmm op1 + Gp op2 — convert
  op2 to Xmm via cvtsi2sd / cvtsi2ss before delegating to the
  Xmm/Xmm safeadd. Gp op1 + Mem op2 — load Mem into a fresh Gp via
  safemov, then delegate to the Gp/Gp path. Surfaced by SMAUG
  `track.c:hunt_victim`.

- **parser: only stop after cast push when initial_brackets == 0** —
  a previous fix's early return in the cast-detection branch fired
  for any caller with stop_on_closing_paren=true, including
  parse_parenthesized_expression (used by `if (...)` / `while
  (...)`). That caller passes initial_brackets=1, so a cast inside
  the condition stopped parsing right after the cast pushed —
  leaving trailing operators unparsed and popOperator hitting an
  empty exStack with "Missing operand". Restrict the early return
  to initial_brackets == 0 — the deref-of-cast caller (`*(CAST)X`)
  passes initial_brackets=0; if/while pass 1. Closes SMAUG
  `save.c:668` through QUICKMATCH macro:
    #define QUICKMATCH(p1, p2)  (int) (p1) == (int) (p2)
    if ( QUICKMATCH(a, b) == 0 )  →  if ( (int)(a) == (int)(b) == 0 )
  Regression: `tests/testchainedeq.mad`.

- **parser: stop after cast push in stop_on_closing_paren mode** —
  `*(TYPE *)expr = rhs;` was being parsed with the inner cast
  detection consuming past the matching `)` of the cast group and
  through the following `=` and RHS, returning a TokenAssign with
  the cast as its left side. The outer `*` wrapper then held that
  TokenAssign instead of the bare TokenCast, so when the assignment
  dispatch fired it saw a TokenCast LHS — which TokenAssign doesn't
  handle — and threw "Assignment on a non-variable lval". Surfaced
  by SMAUG `variables.c`:
    *(EXT_BV *)pvd->data = fread_bitvector(fp);
  Regression: `tests/testderefcastassign.mad`.

- **resolveCompoundLHS: TokenDerefExpr (deref of expr) lvalue** —
  `*(expr) |= rhs;` where `expr` is a pointer-yielding subexpression
  lands as TokenDerefExpr. That class reports `type() == ttMember`
  but isn't a TokenMember or TokenDeref, so resolveCompoundLHS's
  ttMember branch threw "compound assignment <op> on unsupported
  member type". Mirror TokenAssign::compile()'s TokenDerefExpr
  handling: pull the deref_type and operand() Mem from
  TokenDerefExpr the same way TokenDeref already does. Regression:
  `tests/testcompoundderefexpr.mad`.

- **saferet: handle Mem operand by loading into a Gp before ret** —
  saferet was strict on Reg|Imm; a Mem operand falling through from
  the IR pipeline tripped a raw throw. Now loads Mem into a fresh
  Gp via `cc.mov` and rets through that. Surfaced by SMAUG
  `fread_bitvector` returning an EXT_BV struct.

- **parser: function-to-pointer decay on `return func;`** — the
  parser's "follower decides decay vs call" heuristic missed the
  `;` follower at top of an expression. `return do_aassign;` (where
  DO_FUN is `typedef void (...)`) was being built as a TokenCallFunc
  for a void-returning function — its compile() returned an empty
  asmjit Operand and TokenRETURN's compile_token_normalized →
  IRBuilder::coerce threw "invalid src" with no useful context.
  Add `(peek_id == tkSemi && opStack.empty())` to the
  followed_by_value_end set. The opStack-empty guard preserves
  operator-consuming patterns like `cout << endl;` where BSL on
  opStack wants the no-arg ostream call form. Regression:
  `tests/testreturnfndecay.mad`.

- **TokenCast: don't short-circuit (void *) through the (void)-discard
  path** — `DataDef::rawtype()` strips the pointer ref bias — `void
  *` and bare `void` both report `rawtype() == dtVOID` because
  pointer-ness lives in the high bits of `_type` (10000-step
  encoding) not the rawtype. TokenCast's `(void)expr` discard
  short-circuit only checked rawtype, so `(void *)expr` was taking
  the same path: it set `regdp.second = expr->datadef()` (often the
  bare value type, losing the pointer-ness) and didn't propagate
  the cast type forward, so downstream `emit_ir_value` ended up
  calling `IRBuilder::coerce(int* -> void)` and threw. Add
  `!cast_type->is_pointer()` to the guard. Surfaced by SMAUG
  `comm.c:init_socket`:
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &x, sizeof(x))
  Regression: `tests/testvoidptrcast.mad`.

- **IRBuilder::coerce: include src/dst type names in error message** —
  bare `throw "unsupported type conversion"` gave no clue which types
  caused the failure; every gap looked the same. Promoted to
  `snprintf`-into-static-buffer so the actual `src=...` /  `dst=...`
  type names land in the error stream. Surfaced while probing past
  act_wiz.c, where an `int* -> void` coerce was firing from inside a
  TokenIF → TokenLT → TokenCallFunc → TokenCast → TokenAddrOf chain;
  investigation deferred to next session now that the message is
  actionable.

- **resolveCompoundLHS: raw-pointer subscript lvalues** — `int *p;
  p[i] += N;` (and the rest of the compound-op family — -=, *=, /=,
  %=, &=, |=, ^=, <<=, >>=) used to throw `"<op> on unsupported
  subscript lval"` because the ttSubscript branch only covered
  TokenSubscript with a fixed-array base and TokenSubscriptExpr with
  an expression base. A TokenSubscript whose `object.type` is a
  pointer fell between the two. New branch mirrors
  TokenSubscript::compile()'s pointer-subscript read path — MOV the
  pointer into a Gp, fold the index by element stride (SIB scale for
  power-of-2, imul otherwise), build the writeback Mem, load through
  it for the LHS. Closes SMAUG `act_info.c` `prgnShow[iShow] +=
  obj->count`. Two remaining raw `throw` sites in resolveCompoundLHS
  upgraded to `pgm.Throw(left)` for better diagnostics on the next
  gap. Regression: `tests/testcompoundptrsub.mad`.

- **safediv: Gp/Xmm mixed dividend/divisor operands** — `op2` (the
  dividend) is the destination register that receives the quotient
  for both x86 idiv and SSE divsd/divss. Its register family now
  selects the result family — Xmm op2 → real division, Gp op2 →
  integer division — and op3 is coerced into the chosen family
  before the hardware op (`cvtsi2sd` / `cvtsi2ss` for Gp→Xmm,
  `cvttsd2si` / `cvttss2si` for Xmm→Gp). Closes the SMAUG
  mud_prog.c blocker reached after the Gp-vs-Xmm safecmp closure.

- **safecmp: Gp-vs-Mem and Gp-vs-Xmm mixed comparisons** — `if
  (chances != 0 && victim->morph)` and similar SMAUG idioms compare a
  computed Gp value against a stack-resident Mem; now safecmp loads
  the Mem into a fresh Gp via safemov and delegates to the Gp/Gp
  path. Mixed Gp-vs-Xmm (`stances[i] > GRAND_MASTER * .75`) converts
  the Gp to double via cvtsi2sd and uses ucomisd. Closes the
  `skills.c:check_parry` front edge.

- **IRBuilder::coerce: char*→string transient relabel** — ternary
  branches that mix a string literal (dtSTRING) with a char*-yielding
  pointer expression set the merged value's `_datatype` to dtSTRING
  but the actual storage is a Gp char*. For printf-style consumers
  (the typical use), the relabel-only coerce keeps the Gp and
  retypes as string. Closes the `fight.c:damage` front edge that
  surfaced once the nested fixed-array struct-init fix advanced
  compile far enough to reach `damage`.

- **Three SMAUG-front-edge fixes in one commit** (`b7d6347`):
  - `emit_struct_init` now handles nested fixed-array members. SMAUG's
    `const struct liq_type liq_table[] = { { "water", "clear", { 0,
    1, 10 } }, ... };` has a third member (`sh_int liq_affect[3]`)
    that's itself a fixed array. Reads the parent struct's
    `m_count(member_name)` and emits per-element stores at
    `[base + addr + j*esize]` with zero-fill for trailing slots.
  - fn-ptr-member-call detector skips when the member is the LHS of
    an assignment. `ch->last_cmd = (aRoom ? do_rreset : do_reset);`
    was mis-parsing the RHS paren as a CALL through last_cmd.
    prevToken-based check distinguishes from `int v = (*flfunc)(args)`
    where the `(`'s prevToken is `)`, not the assignment op.
  - `continue` inside `switch` inside `for` now compiles. TokenCONT
    walks loopstack from top to bottom looking for the first entry
    with a non-NULL continue label; switches push (NULL, exit) so
    `break` targets the switch but `continue` pierces through to
    the enclosing loop.

- **parseFunction param-loop hardening** — when a forward declaration
  registered N parameters and the definition arrived with fewer
  parsed names (real C-side mismatch like `int main(int, char**);`
  → `int main()` OR a parser-side undercount in typedef'd-pointer
  param parsing), the loop walked off the end of `ids[]` and crashed
  inside the std::string copy ctor (NULL+8 deref). Now fills missing
  slots with synthetic names (`__synthetic_pN`); extras past the
  count are ignored.

- **Cross-function xmm-leakage variant of the asmjit float quirk closed
  at the root** — the variadic-dlsym call path was building a
  `FuncSignature` from the actual argument types but never marking it
  variadic. Per SysV x86-64 ABI, calls to variadic functions must set
  `AL` = number of XMM registers used so the callee knows where to
  find float args. Without it, `AL` was left with whatever was in
  `rax` from prior code (often the format string's low byte) and
  printf either skipped xmm0 (`%f` printed `0.000000`) or read past
  valid args. Symptom was binary-layout-dependent because the leftover
  `AL` value depends on what code ran just before — this is what made
  the issue look like a typed-Xmm reg-allocator quirk for weeks.
  Fix: `funcsig.setVaIndex(1)` marks args at index 1+ as variadic
  (correct for the entire printf family — one fixed format-or-target
  arg followed by `...`). asmjit now emits the AL setup and float-arg
  printf works deterministically. Both float-quirk variants the prior
  TODO had filed (multi-arg-printf-reordering and cross-function xmm-
  leakage) are now closed; `tests/testfloat.mad` is no longer a
  layout-shift canary.

- **`fd_set` typedef + FD_* macros take pointer; `struct hostent`** —
  Bare `fd_set` typedef alias added to `<sys/select.h>` /
  `<sys/time.h>`. `FD_ZERO`/`SET`/`CLR`/`ISSET` now expect a
  `fd_set *` (pointer), matching glibc; `FD_CLR(fd, &set)` (the
  standard call style) no longer expands to `&(&set)`. `struct
  hostent` added to embedded `<netdb.h>` with the full glibc layout.
  Required by MadSMAUG `comm.c`. Existing FD_* test
  (`tests/teststructinterop.mad`) updated to the pointer call form.

- **`((char *)expr)[i]` cast-of-pointer subscript** — parser's `[`
  handler now recognizes TokenCast-of-pointer alongside
  TokenMember/Subscript/Deref bases, and `TokenSubscriptExpr::compile`
  routes TokenCast through `compile()` (not `operand()`) so the cast
  emits its conversion before the index calculation. Closes the SMAUG
  `comm.c:3112` `((char *)arg)[0] == '\0'` form. Regression:
  `tests/testcastsubscript.mad`.

- **`sizeof unary-expr` (no parens) + keyword case-labels + multi-decl
  idents** — `sizeof ok_otype`, `sizeof *a` (no parens) now resolved.
  Constant-integer-expression parser accepts contextual-identifier
  keywords (`case class:` for an enum tag named `class`). Multi-
  variable declarations (`sh_int cou, race, class, ...`) accept
  contextual-identifier names. Closes MadSMAUG `grub.c` front edges.
  Regression: `tests/testsizeofnoparens.mad`.

- **Embedded headers `<crypt.h>`, `<netinet/in_systm.h>`,
  `<netinet/ip.h>`, `<arpa/telnet.h>`** — `<crypt.h>` `#load`s
  `libcrypt.so` and types `extern char *crypt(...)` (libcrypt isn't
  in glibc's RTLD_DEFAULT search). `<arpa/telnet.h>` carries the
  TELNET protocol constants (IAC, WILL/WONT/DO/DONT, GA, the
  TELOPT_* set). Required by MadSMAUG `act_info.c` (crypt) and
  `comm.c` (telnet protocol).

- **`try`/`catch`/`throw` as C identifiers; pre-case declarations in
  switch bodies** — `int try; try = saving_throw();` is valid C; the
  parser now treats these C++ keywords as contextual identifiers in
  declaration / variable / member positions, and routes them through
  `parseExpression` at statement position when not followed by `{`
  (would-be try-block) or `(` (would-be throw-arg). Switch parser
  also accepts variable declarations and stray `;` between
  `switch(...) {` and the first `case`/`default`. Regressions:
  `tests/testkeywordsasidents.mad`, `tests/testswitchpredecl.mad`.

- **Function-to-pointer decay before comparison/logical/bitwise
  operators** — `if (t->fn == do_cast && tmp->...)` failed at parse;
  the decay heuristic only fired for value-end tokens. Now
  `==`/`!=`/`<`/`<=`/`>`/`>=`/`&&`/`||`/`&`/`|`/`^` also trigger
  decay. Without this the call-creation path consumed the operator
  token eagerly and silently lost it. Regression:
  `tests/testfnptrcompare.mad`.

- **Struct member offsets after fixed-array members + array-of-pointers
  indexing** — two long-latent bugs sharing a root cause: `DataDefSTRUCT`
  never recorded per-member counts for fixed-array members, so
  `m_offset()` walked `dd.size` per step instead of `dd.size * count`,
  and anywhere parser/compiler asked "in-place aggregate or stored
  pointer?" the answer was just `is_pointer()` on the member's datadef —
  which mis-classifies an array of pointers (`SKILLTYPE *arr[N]`) as a
  stored pointer. Fix: parallel `member_counts` vector on DataDefSTRUCT
  with an `m_count(name)` accessor, plus `TokenMember::is_fixed_array_member()`
  shared by parser and compiler. Three compiler sites updated
  (TokenSubscriptExpr::compile, resolveCompoundLHS's tse path,
  TokenAssign's tse write path) to LEA the member when it's a fixed
  array even if its element is a pointer; parser's subscript-on-
  expression branch skips the pointer-element unwrap for fixed-array
  members so `arr[i]->member` types correctly. The TODO had the natural
  fix blocked on a float-quirk regression — the v0.12.0 typed-Xmm
  IRBuilder sweep already closed that. Closes the MadSMAUG `magic.c:134`
  (`ch->pcdata->special_skills[sn]->name`) front edge. Regression:
  `tests/teststructarrayofptr.mad`.

- **Cast body stops at matching `)` in BSL chains** — `cout << (int)(a
  - b) << endl;` (and any cast wrapping a parenthesized expression
  inside an operator chain) failed at parse time with "Unexpected
  keyword in expression" pointing at the next statement. The recursive
  parseExpression invoked on the cast body parsed past its matching `)`
  and kept consuming the outer `<< endl;` chain. Fix: when the cast
  body starts with `(`, consume it and call parseExpression with
  stop_on_closing_paren and initial_brackets=1. Postfix follow-ups
  like `(MyType*)(p+1)->m` keep parsing — that path is already handled
  in parseExpression's close-paren branch. Surfaced while writing
  tests/teststrextra.mad. Regression: `tests/testcastparenexpr.mad`.

- **Remaining `<string.h>` typed returns landed** — `extern char *strrchr`,
  `strstr`, `strdup`, `strpbrk`, `strtok`, `strndup` all added to embedded
  `<string.h>`. The cross-function xmm-leakage variant of the asmjit-
  Compiler float quirk that previously blocked these (binary-layout
  shifts pushed `testfloat.mad`'s `test_promote()` past a code-cache
  threshold) is closed by the v0.12.0 typed-Xmm IRBuilder fix — the
  allocator's scalar-real type hints no longer interleave with the
  int-vector path. Regression: `tests/teststrextra.mad` exercises each
  through standard SMAUG idioms (pointer arithmetic, NULL comparison
  without explicit cast, deref-and-assign, while-loop tokenization,
  heap-allocated copies).

- **Comprehensive `cc.newXmm()` → typed scalar sweep across
  `compiler.cpp` and `typesafe.cpp`** — followup to the IRBuilder
  fix below: every other generic `cc.newXmm()` call site (call
  return binding, real-typed token operand caching, `safeneg` /
  `testzero` temps, real-cast intermediates, `_fconst` /
  `_fx_tmp` / `_tmp_mm` etc.) was mistyping its Xmm as `int32x4`.
  Each was replaced with the right `cc.newXmmSs` / `cc.newXmmSd`
  variant (or via a new file-scope `newScalarXmm(pgm, dd, name)`
  helper that dispatches on `dd->size`). Variadic-dlsym call path
  also now suppresses `funcsig.setRetT<double>()` for known int-
  returning libc functions (printf family) — telling asmjit the
  return is `double` made its allocator keep an xmm reg live
  across the call, interfering with arg setup. Doesn't fully
  eliminate the cross-function xmm-leakage variant of the float
  quirk; that's still filed as a TODO blocker for the broader
  `<string.h>` typed-return additions.

- **Nested struct-member initializers** — struct initializers with
  a nested brace-list for a struct-typed member —
  `struct Liq v = { "x", "y", { 0, 1, 10 } }` — used to throw
  "TokenStmt::compile() unexpected token type=25" (ttStructLit).
  Two-part fix: (1) parser's nested-brace reader is now recursive
  (a `{` inside a `TokenStructLit`'s inits is itself another
  `TokenStructLit`); (2) `emit_struct_init` detects `mtype is
  btStruct + element is TokenStructLit` and recurses with the
  member offset as the new base. Both global
  (`struct Liq tab[] = {{...}, ...};`) and local forms work.
  Closes the MadSMAUG `const.c:360+` (`liq_table[]`) front edge.
  Regression: `tests/teststructinitnested.mad`.

- **`&((ch)->p->v)` postfix chain after `)`** — the `&(...)`
  parser passes `stop_on_closing_paren=true` to `parseExpression`
  so it can grab a parenthesized lvalue. The inner expression
  `(ch)->p->v` itself opens a paren around `ch`; when that close-
  paren brought parseExpression's brackets count to 0,
  `stop_on_closing_paren` ended parsing — leaving `->p->v`
  unconsumed. Fix: when we'd otherwise end on stop_on_closing_paren,
  peek one ahead — if the next token is a postfix-chain operator
  (`.` / `->` / `[`), keep parsing the chain. Closes the
  MadSMAUG `icec-mercbase.c:318` (`&ICE_LISTEN(ch)`) front edge.
  Regression: `tests/testaddrparenchain.mad`.

- **`(expr)[N]` for pointer-yielding expressions** —
  `(p + n)[i]`, `(q = p + 1)[i]`, `(mud = imc_mudof(arg))[0]`,
  `(buf + N)[i]` (with `buf` a fixed array) used to throw
  "Expecting ] in lambda expression" because the `[` handler only
  recognized TokenMember/Subscript/Deref* as valid subscript
  bases. Two-part fix: (1) parser widens subscript-on-expression
  detection to accept TokenAdd/TokenSub/TokenAssign whose
  `datadef()` reports a pointer (or whose operands include a
  fixed-array TokenVar); the elem_type derivation walks into
  TokenAdd/Sub/Assign operands to recover the fixed-array's
  element type when the operator-level datadef misreports
  scalar; (2) `TokenSubscriptExpr::compile` calls
  `base_expr->compile()` (not `operand()`) for these complex
  bases — they need full arithmetic/assign emission to yield a
  Reg holding the pointer value. Closes the MadSMAUG
  `imc-mail.c:1047` and `imc-config.c:186`
  (`GETSTRING(..., (idetails + strlen(idetails)), ...)`) front
  edges. Regression: `tests/testparenexprsub.mad`.

- **`sizeof(*ptr->member)` and `sizeof(*obj.member)`** — the
  sizeof parser's `*` branch only accepted a bare identifier; for
  postfix chains like `*c->local` it threw "Expecting identifier
  after '*' in sizeof". Fix: when the next token after `*` is
  followed by `.`, `->`, or `[`, parse a full postfix chain and
  use its datadef. For pointer chains return sizeof(pointed-to);
  otherwise sizeof(chain type). Closes the MadSMAUG
  `icec-mercbase.c:130` (`imc_malloc(sizeof(*c->local))`) front
  edge. Regression: `tests/testsizeofderefchain.mad`.

- **Typed scalar Xmm allocation in IR + `extern char *strchr(...)`
  in embedded `<string.h>`** — `IRBuilder::newReg` was using
  `cc.newXmm()` for every Xmm value, which asmjit types as
  `int32x4` (4-element int vector). For scalar floats / doubles
  the allocator's int-vector liveness path interleaved with the
  scalar-real path under register pressure, producing
  filename-length-dependent reordering of varargs `printf`
  arguments. Fix: dispatch on `type->size`, using `cc.newXmmSs`
  for `float` and `cc.newXmmSd` for `double`. The allocator now
  sees scalar-real type hints and varargs xmm setup is
  deterministic. With the allocator behaving, added the long-
  blocked `extern char *strchr(char *s, int c);` to embedded
  `<string.h>` so `*(strchr(s,c)) = 0` and `if (strchr(...) ==
  NULL)` work without explicit user-side `extern`. Closes the
  MadSMAUG `imc.c:340` front edge. Regression:
  `tests/teststrchrtyped.mad`. The remaining string.h additions
  (`strrchr`/`strstr`/`strdup`/etc.) still hit a different
  cross-function xmm-leakage variant of the asmjit-Compiler
  quirk; filed in TODO for the typed-register-IR Stage 4 work.

- **`string` as a function parameter name** — `static void
  parsekeys(char *string) { p1 = string; }` used to throw
  "Expecting identifier". Inside the function body the lexer
  always returns the type-keyword token for `string`, and
  parseExpression's ttDataType branch unconditionally took the
  inline-declaration path (`type ident`), failing when the next
  token wasn't an identifier. Fix: when the ttDataType is a
  contextual identifier (`string` is currently the only flagged
  one), look it up as a variable first; if found AND the next
  token is not an identifier, treat the keyword as a variable
  reference. Inline declarations still work because they have an
  identifier next. Closes the MadSMAUG `imc-version.c:128` front
  edge. Regression: `tests/teststringparam.mad`.

- **Keyword-as-identifier in enum body** — `enum { name, sex, class,
  race, ... }` (using `class` or other contextual-keyword identifiers
  as enum tags) used to throw "Expecting identifier in enum". Earlier
  sessions made `class` work as a variable name, struct member, postfix
  chain, and `&` operand, but the enum body was missed. Fix: route
  enum identifier parsing through `is_contextual_identifier_token` /
  `contextual_identifier_name` like the rest of the parser. Closes the
  MadSMAUG `grub.c:500` front edge. Regression: `tests/testenumclass.mad`.

- **`sizeof(*arr)` / `sizeof(*ptr)`** — sizeof parser handled
  ttDataType, ttIdentifier, and `struct tag` forms but rejected
  unary `*` with "Unknown type in sizeof". The standard C idiom
  `sizeof(arr) / sizeof(*arr)` (count the elements of a fixed
  array) didn't compile. Fix: added a `*identifier` branch — for a
  fixed-array variable, returns the element type's size; for a
  pointer variable, returns the pointed-to type's size. Closes
  MadSMAUG `update.c:2300-2301`. Regression:
  `tests/testsizeofderef.mad`.

- **Classic C `(*flfunc)(args)` fn-pointer call** — two distinct
  gaps fixed together while probing MadSMAUG `reset.c:985`
  (`value = (*flfunc)(arg);`). (1) The unary-`*` parser threw
  "cannot dereference non-pointer type" on a fn-pointer variable;
  C semantics treat `*fp` as the function itself, so the unary-`*`
  identifier branch now pushes the var as a value when its type is
  `is_function() && is_numeric()` (i.e. `DataDefFPTR`), matching
  the existing paren-branch behavior. (2) After the deref, the
  `(args)` call needed a fn-ptr-VAR-call branch in the `(`
  handler, parallel to the existing fn-ptr-MEMBER-call branch. Both
  branches now scan opStack for tighter-than-`=` pending operators
  (precedence < 14): `&&`, `!`, `<`, etc. block the call, but `=`
  (declaration init / assignment) doesn't, so `int v = (*fp)(arg);`
  works while `ch->fn && (other)` still doesn't mis-fire.
  Regression: `tests/testfnptrparenscall.mad`.

## [v0.12.0] — 2026-04-25

SMAUG Phase F front-edge wave: 13 parser/lexer/compiler fixes surfaced while probing MadSMAUG translation units (mud_prog.c, news.c, stances.c, tables.c, act_info.c, act_obj.c, boards.c, misc.c, update.c). Highlights: pointer-typed `*(ptr ± N)` / `*(p = ptr + N)`, single-pass macro substitution, keyword-as-identifier in unary `&`, `vfADDRTAKEN` pointer + `&ptr->member`, struct decl with `*` decorator, interleaved CV-qualifier+star chains, constant-expression shift+bitwise operators, char literals inside macro args, `extern` libc late-bind via dlsym, `*++p` not eating trailing binops, and fn-ptr-member-access not mis-firing through pending operators. Compound-assign / inc-dec error diagnostics swept to `Throw(...)`. 14 new integration tests; 197/197 passing.

- **`ch->fn && expr_with_parens` no longer mis-fires fn-ptr call
  detection** — when a struct member of function-pointer type sits
  on top of exStack, any `(` later in the expression triggered the
  "direct invocation through struct-member function pointer"
  branch — even with `&&`, `!`, or other operators between. Result
  was "Missing operand" because the parser tried to consume the
  `(args)` of a sub-expression as call args of the prior fn-ptr
  member. Fix: scan opStack for any pending operator (anything not
  `(`) — if one's there, the `(` belongs to a sub-expression and
  the fn-ptr-call form is skipped. Closes the MadSMAUG
  `update.c:744-745` (`!xIS_SET(ch->act, ACT_RUNNING) &&
  ch->spec_fun && !IS_AFFECTED(ch, AFF_POSSESS)`) front edge.
  Regression: `tests/testfnptrmember_binop.mad`.

- **`*++p` / `*--p` followed by a binary operator** — the unary-`*`
  parser fell through to `parseExpression(deref_tb=tkInc, true)`
  which then happily consumed any trailing binary op too, so
  `*++p == 'e'` parsed as `*(++p == 'e')` (deref of a bool).
  `*++p;` (no trailing binop) accidentally worked because
  parseExpression stopped on `;`. Fix: explicit `tkInc`/`tkDec`
  branch in the unary-`*` parser that builds a `TokenInc`/`TokenDec`
  with the pointer as `right`, wrapped in `TokenDerefExpr` — one
  node sequence, no recursive parseExpression. Closes the MadSMAUG
  `misc.c:2149` front edge (`*srcptr == '%' && *++srcptr == 's'`).
  Regression: `tests/testderefpreinc.mad`.

- **`extern RET name(args);` typed-calls via dlsym** — explicit
  forward declarations of libc / system functions used to fail at
  compile time with "method has neither FuncNode nor x86code". The
  dlsym fallback fired only for *implicit* calls (an undeclared
  identifier followed by `(`); explicit `extern` declarations
  registered the function symbol but never tried to resolve it.
  Fix: in `TokenCallFunc::compile`, when a declared function has
  neither funcnode nor x86code, try `dlsym(RTLD_DEFAULT, name)` as
  a last resort. If resolved, set `method->x86code` and fall through
  to the typed-call path. Also extracted the int32-sign-extension
  whitelist into a file-scope helper so the typed-call's
  `bind_call_return` applies the same movsxd dance the variadic
  dlsym path does — otherwise `extern int strcmp(...)` returned
  garbage-signed int64 values. Regression: `tests/testexterndlsym.mad`.

- **Char literals inside macro arguments don't break the macro arg
  parser** — the lexer's macro-arg loop tracked `(`, `)`, `,`, and
  `"` for nesting/escapes but not `'`. So an argument containing a
  char literal with a paren/comma/quote inside it (e.g.
  `stub(x, (x > 0) ? ')' : '(')` ) prematurely ended the macro call:
  the `)` inside `')'` was read as a real close-paren. Diagnostic
  was "Unterminated string" once the lexer ran out of input
  expecting more. Fix: handle `'` symmetrically with `"` — copy the
  char literal verbatim through its closing `'`, honouring `\`
  escapes. Surfaced in MadSMAUG `boards.c:599-607` where
  `pager_printf` gets nested-ternary args containing `'V'`/`'B'`/
  `':'`/`')'`. Regression: `tests/testmacrocharlit.mad`.

- **Constant-expression evaluator now supports `<<`, `>>`, `&`, `|`,
  `^`** — `case (1 << 14):` (and the underlying SMAUG idiom
  `case ITEM_HOLD:` where `ITEM_HOLD` expands to `(1 << 14)`) used
  to throw "Expecting ')' in constant expression". The const-expr
  recursive descent only had additive (`+`, `-`) and multiplicative
  (`*`, `/`, `%`) layers — when the additive loop saw `<<` it
  returned to the outer paren handler, which then demanded `)`.
  Added the full C precedence chain (shift → bitwise-and → bitwise-
  xor → bitwise-or). Closes the MadSMAUG `act_obj.c:1735` front
  edge. Regression: `tests/testconstexprshift.mad`.

- **`struct tag { ... } *first, *last;`** — defining a struct and
  immediately declaring pointer-to-the-struct variables in the same
  statement used to throw "Expecting variable name or ';' after
  struct definition". `TokenSTRUCT::parse` only routed through
  `parseDeclaration` when the post-`}` token was an identifier
  (`} name;`); a leading `*` decorator (`} *first, *last;`) fell
  through to the diagnostic. Fix: also enter parseDeclaration when
  the next token is `*`. Closes the MadSMAUG `act_info.c:2721` front
  edge (`whogr_s` linked-list definition). Regression:
  `tests/teststructptrdecl.mad`.

- **`type const *p` and interleaved CV-qualifier-with-stars chains**
  — `parseDeclaration` consumed `const` / `restrict` only after the
  pointer stars, so `char const *p`, `int const *q`, and
  `int const * const *xpp` all threw "Expecting identifier after
  type". Replaced the two separate sweeps with a single qualifier-
  or-star loop so qualifiers and stars can interleave freely.
  Closes the MadSMAUG `act_info.c:3074` front edge
  (`char const *class;`). Regression: `tests/testconstmid.mad`.

- **Compound-assign / inc-dec error diagnostics now carry file:line**
  — `TokenAddEq`/`SubEq`/`MulEq`/`DivEq`/`ModEq`/`BSLEq`/`BSREq`/
  `BandEq`/`BorEq`/`XorEq` and `TokenInc`/`TokenDec` previously
  threw raw C-strings on missing operands or unsupported lvalues, so
  the user saw a bare `: error:` line with no source context.
  Converted to `pgm.Throw(this) << "..." << flush` (or `Throw(left)`
  / `Throw(tse)` for inner-token-precise resolveCompoundLHS sites)
  so diagnostics now anchor to the operator's source location and
  the message includes the operator name.

- **`&ptr->member` now works when `ptr` is address-taken** —
  whenever a pointer-typed local was later referenced via `&ptr`
  (marking it vfADDRTAKEN → stack-backed), earlier `ptr->member` /
  `&ptr->member` uses read through the Mem slot as if it were the
  struct itself. `TokenMember::operand` saw a `Mem` for `_obj` and
  jumped to the "struct on the JIT stack" branch, which `addOffset`-d
  into the pointer's own storage rather than the struct it points at
  — producing a stack-frame-relative address instead of the real
  member address. Fix: when `_obj` is `Mem` and `object.type` is a
  pointer, first load the pointer value from the Mem into a fresh
  Gp, then compute `[gp + offset]` via the same shapes the reg-
  resident pointer branch uses. Regression:
  `tests/testaddrtakenptrmember.mad`.

- **`&class->member` parses when `class` names a C variable** — the
  unary-`&` handler's postfix-chain detector and its simple-ident
  fallthrough both required `addr_tb->type() == ttIdentifier`, so
  `&class->affected` (SMAUG's `tables.c:1861`, `fwrite_class`) threw
  "expecting variable name after '&'". Earlier fixes made `class`
  usable as a C identifier in parseStatement and parsePostfixChain,
  but the `&` path was missed. Fix: the `&` handler now uses
  `is_contextual_identifier_token()` / `contextual_identifier_name()`
  at both spots, matching the rest of the parser's keyword-as-
  identifier handling. Regression: `tests/testaddrclass.mad`.

- **Function-like macro parameter substitution no longer cascades** —
  the lexer ran one full-body pass per parameter. If the value
  substituted for parameter N happened to match the name of a later
  parameter M, the M-pass would rewrite the already-substituted
  value. SMAUG's `CREATE(type, NEWS_TYPE, 1)` — where the user's
  local variable is named `type` (identical to `CREATE`'s second
  parameter) — hit this: first `result→type` rewrote `(result)` to
  `(type)`, then `type→NEWS_TYPE` rewrote that into `(NEWS_TYPE)`,
  leaving `(NEWS_TYPE) = (NEWS_TYPE *) calloc(...)` which does not
  parse. Fix: walk the original macro body once, collecting each
  identifier, and look it up in a single param-name → arg-value
  map. Substituted strings are emitted verbatim and never re-
  scanned. Closes the MadSMAUG `news.c:153` front edge. Regression:
  `tests/testmacrosubst.mad`.

- **`*(ptr + N)` / `*(ptr - N)` / `*(p = ptr + N)` now parse** —
  `TokenAdd`, `TokenSub`, and `TokenAssign` all inherit
  `_datatype = &ddINT` from `TokenOperator`'s constructor and never
  overrode `datadef()`, so expressions like `start - 1` reported
  their type as `int` regardless of operand types. The unary-`*`
  parser consulted `deref_expr->datadef()` and rejected these as
  "cannot dereference non-pointer type". Fix: override `datadef()`
  on `TokenAdd`/`TokenSub` to propagate a pointer operand's type
  through arithmetic (`ptr + int`, `int + ptr`, `ptr - int`; `ptr -
  ptr` still yields the integer default), and override
  `TokenAssign::datadef()` to return the LHS's type (C assignment-
  as-expression evaluates to the assigned value). Closes the
  MadSMAUG mud_prog.c front edges at lines 2552/2553
  (`*(start - 1) == ' '`, `*(end = start + strlen(arglist)) == ' '`).
  Regression: `tests/testderefptrexpr.mad`.

- **Function-like macros no longer eat later declarators** — SMAUG-
  style `#define bug(...) ((void)0)` above a later
  `void bug(const char *, ...) { ... }` definition used to expand at
  the definition head, turning `void bug(const char *, ...)` into
  `void ((void)0)` and killing the parse. The lexer now walks back
  through recently emitted tokens before the function-like expansion
  check; if the preceding non-`*` token is a type keyword
  (`ttDataType`), `struct`/`class`/`enum`, or a storage-class /
  qualifier (`const`/`extern`/`static`/`register`/`typedef`/
  `restrict`), expansion is suppressed so the declarator parses
  normally. Ordinary call sites (preceded by `{` / `;` / `,` / an
  operator) still expand. Covers the three common declarator shapes
  — `void foo(...)`, `char *foo(...)`, and `static int foo(int)`.
  MadSMAUG's `#undef bug` workaround around the db.c include can
  now be removed. Regression: `tests/testmacrodefhead.mad`.

- **`*(TYPE*)expr` with typedef'd TYPE** — the unary-`*`-`(` parser
  branch used to consume the `(` and call `parseExpression` on the
  inner content, bypassing cast detection (which runs on `(`).
  Typedef'd type names like `EXT_BV` then fell into the identifier/
  variable-lookup path and failed with "use of undeclared identifier
  'EXT_BV'". Fix: peek inside the `(` — when the first token is a
  cast signature head (`ttDataType` keyword, `struct`/`class`, or a
  typedef'd identifier in `datatype_map`), delegate the whole
  `(...)` back to `parseExpression` so its existing cast detection
  runs. Plain grouping forms (`*(a + b)`) take the previous path
  unchanged. Regression: `tests/testderefcasttypedef.mad`. Closes
  the MadSMAUG mud_prog.c front edge at line 1276
  (`xIS_SET(*(EXT_BV*)vd->data, flag)`), and the isolated
  `(*(EXT_BV*)p).bits[0]` / `EXT_BV v = *(EXT_BV*)p;` forms.

- **Real `<` / `<=` / `>` / `>=` comparisons were flipping** — x86
  `ucomisd` writes CF/PF/ZF (unsigned-compare semantics), not the
  signed SF/OF flags that `setl` / `setle` / `setg` / `setge` read.
  `emit_compare` was emitting the signed setcc variants after a
  ucomisd, which read unrelated flags and produced wrong 0/1
  results — `1.5 < 2.0` returned `0`, `1.5 > 2.0` returned `1`.
  The fix treats reals like unsigned when choosing the setcc (`setb`
  / `setbe` / `seta` / `setae`, matching the flag semantics x86
  mandates for floating-point compares). Equality (`==` / `!=`) was
  already correct because `setce`/`setne` read ZF, which ucomisd
  does set. Regression: `tests/testrealcmp.mad`.

## [v0.11.0] — 2026-04-24

SMAUG Phase F front-edge resumption: the MadSMAUG umbrella now compiles and runs end-to-end against a stub `main()` after a dozen language gaps filed during whole-program porting were closed. Highlights: `goto` / forward labels, struct-copy init+assign via `memcpy`, `*p++ = rhs` as LHS, `(*p).member`, `expr[i].member`, compound-assign on expression-base subscripts (`xREMOVE_BIT` / `xSET_BIT`), struct-array subscript stride, `class` as a plain C identifier, leading-dot float literals, `char[N]` exact-length init without implicit `'\0'`, unary `-` after `{` / `,` / `;` / `(` / `=`, `#include` realpath canonicalization, better diagnostics for unsupported compound-assign lvals, and `safemov(Operand, double)` no longer truncating for Mem destinations. SMAUG completion tracked in `docs/smaug-progress.md` (~27% parse/compile by line count).

- **`goto label;` + forward labels** — function-scoped labels and
  `goto` resolve through `Program::label_map`, which `TokenFunc::
  compile` clears at each function boundary. Forward references
  work naturally: `TokenGOTO::compile` look-or-creates the
  `asmjit::Label` on first use, and a later `TokenLabel::compile`
  binds it. parseStatement detects `ident:` at statement position
  via a `tkTerC` peek (the single-`:` token; `::` stays as the
  separate tkNS token, so there's no ambiguity).

  The `label_map` lives on `Program` rather than `TokenFunc` by
  design — adding an `std::map<std::string, asmjit::Label>` member
  directly to `TokenFunc` silently shifted its multi-inheritance
  vtable layout and regressed unrelated codegen paths
  (`float f = 1.5;` initialization in particular). Keeping the map
  on Program and clearing it at each function entry avoids the
  layout change while preserving function-scoped semantics.

  Regression: `tests/testgoto.mad` covering backward loop gotos,
  forward skips, the SMAUG `doneargs:` multi-branch exit pattern,
  and per-function label isolation. Unlocks (future) ingest of
  mud_prog.c / magic.c / tables.c / build.c / mud_comm.c / ban.c /
  services.c, though mud_prog.c still stumbles on a separate
  deep-macro `(EXT_BV*)` cast parse issue filed as a new gap.

- **`char[N] = "..."` with matching length skips null terminator** —
  C89 allows `char c[3] = "abc";` (no implicit `'\0'` because the
  array is exactly full). The parser was always pushing a null onto
  the init list, producing `Too many initializers for array
  (expected N)` when the explicit size matched the string length.
  Inferred-size (`char c[] = "abc";`) and oversized
  (`char c[10] = "hi";`) cases still append `'\0'`. Regression:
  `tests/testcharnoterm.mad`.

- **Unary `-` in brace-init lists** — `isPostfixPosition` treated
  any non-operator prev-token as "postfix position", including the
  symbol tokens that actually open expression contexts (`{`, `(`,
  `,`, `;`, `=`). As a result `int arr[] = { -5, -4 };` converted
  the unary `TokenNeg` to a binary `TokenSub` at the position right
  after `{` and `,`, and the expression parser reported `Missing
  operand`. The postfix check now rejects those symbol positions.
  SMAUG's const.c is full of negative-initialized lookup tables
  (`str_app`, `int_app`, `dex_app`, …). Regression:
  `tests/testnegbraceInit.mad`.

- **Compound-assign on expression-base subscripts** —
  `resolveCompoundLHS`'s ttSubscript branch only recognized
  `TokenSubscript` with a fixed-array variable base. Any
  `TokenSubscriptExpr` form — struct-contained array members
  (`obj.bits[i] &= ~mask;`), pointer-deref-then-subscript
  (`p->arr[i] += n;`) — fell through to `<op> on unsupported
  subscript lval`. Added a TokenSubscriptExpr path that mirrors the
  TokenAssign write branch: `base_expr->operand()` (avoid the
  `emit_ir_value` load-first-element trap), LEA for aggregate bases
  / MOV for pointer-typed bases, fold non-power-of-2 element sizes
  via `imul`, compute the element Mem, load through
  `load_mem_to_gpq`, then the existing compound-op path handles the
  arithmetic + writeback. Regression: `tests/testcompoundsubexpr.mad`.

  SMAUG's `xREMOVE_BIT` / `xSET_BIT` macros expand to exactly this
  form. Closes the deepest MadSMAUG compile front edge: after this
  fix `bin/madc SMAUG.mad` compiles + runs end-to-end (umbrella
  `main()` is still a stub, but every ingested translation unit
  compiles and every symbol resolves via the bootstrap shim).

- **`return X;` mis-detected as multi-return when the next statement
  starts with an identifier** — `TokenRETURN::parse`'s
  `looks_like_second_return` peeked at the next token and, because an
  identifier could plausibly be the start of a second return expression
  OR the start of the next statement, treated `return 1; noop(ch); …`
  as multi-return. That silently injected a `__retbuf` parameter at
  compile time and corrupted the function's emission, which
  downstream blew up as `IRBuilder::coerce() invalid src` in
  fight.c / skills.c bodies. The fix uses the consumed stop token
  (`curToken()`) as the signal: multi-return only fires when
  parseExpression actually stopped on `,`. Added a new `curToken()`
  accessor on `Program`. Regression: `tests/testreturnnextident.mad`.

- **Better diagnostics for unsupported compound-assign lval errors**
  — `resolveCompoundLHS` threw `msg.c_str()` from a stack-local
  `std::string`; the catch handler then printed garbage bytes once
  the string went out of scope. Same for `TokenStmt::compile`'s
  `throw this` fallback. Both now throw into static buffers with
  actual diagnostic text, so SMAUG-level errors like `&= on
  unsupported subscript lval` are visible.

- **`class` as a plain C identifier** — madc reserves `class` for OOP
  declarations, but C codebases (notably SMAUG) use it everywhere as
  a struct member name (`ch->class`), a local variable (`int class;`),
  a function parameter, and a subscript index (`tbl[ch->class]`).
  `parseStatement`'s ttKeyword case now routes `class` through
  `parseExpression` when the next token is neither an identifier (real
  class-declaration head) nor `{`. `parsePostfixChain` now accepts
  `class` after `.` / `->` via `is_contextual_identifier_token` (which
  already accepted `tkCLASS` and the STL container keywords).
  Regression: `tests/testclassident.mad`. Advances the MadSMAUG
  umbrella through `skills.c` to the next compile-time front edge
  (an IR coerce mis-wiring in fight/skills function bodies).

- **`safemov(Operand, double, ...)` truncating to int for Mem
  destinations** — `TokenOperator::optimize` constant-folds
  expressions like `double d = 1.0 + 0.5;` and then calls
  `safemov(*regdp.first, foperate(), regdp.second)` with the
  computed double. For Mem destinations the fallback converted
  via `imm((int)d)` — silently dropping the fractional part — so
  the stored bit pattern looked like a denormal double and readers
  got garbage (`2.122e-314` etc.). Added a Mem + `d1->is_real()`
  branch that materializes the double through the local const pool
  into a scratch Xmm and stores Xmm → Mem. Regression:
  `tests/testrealconstfold.mad`. (Printf of the same result still
  hits the separate pre-existing asmjit variadic-doubles quirk;
  the fold itself is now correct, as the `==` / `>` comparisons
  demonstrate.)

- **SMAUG progress tracking** — `docs/smaug-progress.md` holds a running
  parse / compile / link / runtime percentage estimate for the SMAUG
  1.8 umbrella bootstrap. Mirrored in `claude_status.json` under
  `long_term_goal.smaug_completion_estimate`. Current state:
  ~18% parse / ~18% compile / 0% link / 0% runtime (27,821 of 158,537
  upstream lines ingested and parse-clean through the MadSMAUG
  umbrella).

- **Struct-array subscript element stride + base addressing** —
  `arr[i].member` for a fixed array of structs nested inside another
  struct used to produce wrong values / crashes. Two aligned fixes:
  - TokenSubscriptExpr::compile and TokenAssign's ttSubscript /
    TokenSubscriptExpr write branch now fold non-power-of-2 element
    sizes into the index register via `imul` (SIB scale only covers
    1/2/4/8; `sizeof(struct K)` of 16 fell through to scale 1 and
    aliased adjacent elements).
  - Both sites now read `base_expr->operand(pgm)` rather than
    `compile(pgm, rdp)`. For a struct-contained `int bits[N]`, the
    `bits` TokenMember reports `_datatype = int` (numeric), and
    `compile()` routes through `emit_ir_value` — which loads the
    first element's value into a Gp. Subsequent writes would then
    index off that value rather than the array base. Using
    `operand()` keeps the raw Mem/Gp so LEA/MOV can pick the right
    shape downstream.
  Regression: `tests/teststructarrsub.mad` covering both
  `struct { int a; int b; }` elements and `int bits[4]` members.

- **`expr[i].member` now parses and compiles** — the dot handler's
  ttSubscript branch used to `dynamic_cast<TokenSubscript *>`
  unconditionally, but `TokenSubscriptExpr` also reports ttSubscript
  without deriving from `TokenSubscript`; the NULL cast's
  `tsub->object` segfaulted. Added an explicit TokenSubscriptExpr
  fallback that synthesizes a struct-typed object variable and routes
  the result through the existing parent_expr path. Also taught
  `TokenSubscriptExpr::compile` to return the raw element Mem
  directly when the element type is struct/class — `emit_ir_value`'s
  coerce/load path doesn't handle aggregate types and would have
  corrupted the Mem before TokenMember's parent-expr dot-chain
  could add the member offset. Closes the MadSMAUG umbrella front
  edge at `handler.c:4789` in `add_kill`
  (`ch->pcdata->killed[x].vnum`). Regression:
  `tests/testsubscriptexprmember.mad`.

- **Leading-dot float literal `.4`** — lexer now accepts `.4` / `.25f`
  / `.75l` as shorthand for `0.4` / `0.25f` / `0.75l`. The single-`.`
  case in the main tokenizer peeks for a digit and parses the
  fractional expansion (consuming the optional `f/F/l/L` suffix)
  before falling back to TokenDot. Closes the MadSMAUG umbrella
  front edge at `handler.c:4683` (`c += .4*(...)`). Regression:
  `tests/testleadingdotfloat.mad`.

- **`(*p).member` now parses as `p->member`** — the parenthesized-deref-
  then-dot form used to segfault `parseExpression` because the `.`
  handler cast `lhs_dot` to `TokenMember *` unconditionally, but
  `TokenDeref` and `TokenDerefExpr` both report `ttMember` for LHS-
  compat reasons without actually deriving from `TokenMember`. Added
  explicit branches to the dot handler:
  - `TokenDeref` LHS — route the dot through the underlying pointer
    variable so the normal no-`parent_expr` `TokenMember` compiles as
    `[ptr + offset]` via voperand's pointer-in-Gp branch.
  - `TokenDerefExpr` LHS — synthesize a struct-typed object variable
    and pass the `TokenDerefExpr` as `parent_expr`, so
    `TokenMember::operand` calls `TokenDerefExpr::operand` (which
    materializes the pointer value) and accesses `[ptr + offset]`
    through the struct-value "dot chain" branch.
  Exposed by SMAUG's `xIS_SET((var), bit)` macro after `*vector`
  substitution — when `vector` is also a madc keyword (STL container
  reserve), the unary-`*` handler hands the parser a keyword rather
  than an identifier and wraps the expression in `TokenDerefExpr`.
  Closes the MadSMAUG umbrella front edge at `handler.c:2989`
  (`affect_bit_name`). Regression: `tests/testparenderefmember.mad`.

- **Struct-copy initialization and assignment** — C's bytewise struct
  copy (`struct S a = other;` at declaration and plain `a = other;` as
  assignment where both sides are the same user-defined struct type)
  now compiles to a single `memcpy(&dest, &src, sizeof(S))` invocation.
  Two split changes:
  - **parseDeclaration**: when an `=`-initialized struct's RHS is not
    `{` (and not a string literal), push the `=` back and fall through
    to the normal initializer path instead of throwing `Expected '{'
    or string literal for initializer`. That path wraps the init as a
    `TokenAssign` which TokenAssign::compile then lowers via memcpy.
  - **TokenAssign::compile**: added a struct-to-struct branch that
    LEAs both sides' storage (or reuses the Gp when TokenMember already
    returned a LEA'd address, e.g. for `obj->member` struct members)
    and invokes libc `memcpy` with `sizeof(S)`. Struct types must
    match (`ltype == regdp.second`); mismatches raise a compile error
    rather than silently reinterpreting.
  Closes the MadSMAUG umbrella front edge at `handler.c:1284`
  (`EXT_BV extra_flags = obj->extra_flags;`). Regression:
  `tests/teststructcopy.mad`.

- **SMAUG Phase F — `#include` canonicalization** — `should_tokenize_include`
  now canonicalizes each resolved path through `realpath()` before the
  include-once check. Previously the raw `cur_dir + incfile` key was used,
  so `#include "upstream_src/mud.h"` (from the SMAUG.mad umbrella) and
  `#include "mud.h"` (from `ident.c` / `interp.c` / `ibuild.c`) registered
  as two distinct entries even when both paths resolved — via symlinks —
  to the same underlying file. The MadSMAUG umbrella tripped on this at
  mud.h:97 (`AFFECT_DATA` redefined) once the earlier `bug(...)` macro
  front edge was resolved. Quoted includes still fall back to the raw
  path when `realpath()` cannot resolve (e.g. before the file exists);
  embedded-header keys starting with `<` bypass `realpath` entirely.

- **`*p++ = rhs` / `*p-- = rhs` as a write target** — the read side
  (`c = *p++`) already went through `TokenDerefStep`, but the write side
  was missing: `TokenAssign::compile` only dispatched `TokenVar`,
  `TokenDeref`, `TokenDerefExpr`, `TokenMember`, and `TokenSubscript*`
  LHS kinds, so `*arg_first++ = *argument++;` threw `Assignment on a
  non-variable lval`. Added a `TokenDerefStep` LHS branch that mirrors
  the read side: capture `old_ptr = ptr`, step the pointer variable,
  then expose `[old_ptr]` as the Mem lvalue the numeric-assignment path
  writes into. Closes the MadSMAUG umbrella front edge in
  `act_move.c:182` (`grab_word`). Regression:
  `tests/testderefpostincstore.mad`.

## [v0.10.1] — 2026-04-24

- **Typed-register IR — multi-return return-buffer store normalization**
  — `TokenRETURN::compile` no longer open-codes separate Reg/Xmm/Imm/Mem
  cases when writing numeric/pointer multi-return slots into `__retbuf`.
  Eligible slots now compile through `compile_token_normalized(...)` and
  store through `IRBuilder::store`, preserving the existing qword-slot
  contract while moving shape normalization into the shared IR path.
  Unsupported slot types still fall back to the legacy path. Validation:
  23 IR unit + 25 datadef unit + 170 integration tests pass.

- **Typed-register IR — stream I/O shape normalization** —
  `TokenBSL` / `TokenBSR` no longer duplicate their own Reg-vs-Mem-vs-Imm
  argument and writeback logic. ostream string output now materializes
  object addresses through `lea_var_to_gp`, cstr output uses
  `load_var_to_gp`, ostream numeric output normalizes through
  `IRBuilder::coerce` + `load`, and istream integer/real writeback now
  routes through `emit_ir_value` from the temporary stack slot. The
  stream helper invoke selection is still explicit, but the shape
  normalization now lives on the shared path. Validation: 23 IR unit +
  25 datadef unit + 170 integration tests pass.

- **Typed-register IR — direct-call fallback allocation + typesafe prune**
  — the normal direct-call path no longer pre-allocates `operand(pgm)`
  before `bind_call_return`; fallback return storage is now allocated
  at the bind point only when no caller destination was supplied. Follow-on
  Stage 5 cleanup removed dead mixed Gp↔Xmm arithmetic/compare overloads
  and Xmm+Imm arithmetic overloads from `typesafe.cpp` after auditing the
  remaining direct call sites, and tightened the Operand dispatchers to
  reject mixed-group arithmetic/compare immediately. Validation: 23 IR unit
  + 25 datadef unit + 170 integration tests pass.

- **Typed-register IR — dead `safemov(Xmm, Imm)` removal** — a narrower
  follow-up Stage 5 audit confirmed the dedicated vector-immediate mover
  was dead: it had no external callers, and the only path to it was the
  `safemov(Operand&, Operand&, ...)` Xmm+Imm dispatcher arm. Both were
  removed. Real/vector constants still materialize through the existing
  `safemov(op, double/int64_t, ...)` const-pool paths. Validation: 23 IR
  unit + 25 datadef unit + 170 integration tests pass.

- **Typed-register IR — final compiler-site cleanup** — the remaining
  obvious compiler-side Mem stores/loads now route through
  `IRBuilder::store` / `load` / `coerce` where appropriate: stack-local
  zero/init, stack-parameter home-slot stores, compound/member writeback,
  subscript Gp stores, call returns to Mem, cast-to-Mem, ternary
  merge-to-Mem, compound real-member loads, and switch-expression
  Mem-to-int64 normalization. After this pass, the remaining `safemov`
  calls in `compiler.cpp` are adapter internals, pure register shuffles,
  or legitimate leaf/boundary loads rather than unfinished IR cleanup.
  Validation: 23 IR unit + 25 datadef unit + 170 integration tests pass.

## [v0.10.0] — 2026-04-24

Typed-register IR scaffolding + bottom-up migration (Stages 0–3c): fifteen shared compile-site helpers now absorb the per-token shape/coercion boilerplate that used to be copy-pasted across binary ops, comparisons, compound-assigns, inc/dec, lambda-capture, and call return-binding. Three latent bugs fixed as side effects. ~880 net lines removed from `compiler.cpp`, zero behavior change, 48 unit + 170 integration tests green throughout.

### Added

- **Typed-register IR — Stage 3c var-move helpers** — three
  file-scope helpers (`load_var_to_gp`, `lea_var_to_gp`,
  `store_gp_to_var`) absorb the "move a variable's value or
  address in or out of a Gp, independent of whether the variable
  is register- or stack-backed" pattern. Three sites that
  open-coded this pattern now one-line through the helpers:
  lambda-capture pack / reload loops in `TokenCallFunc::compile`
  and the multi-return integer-unpack loop in
  `TokenAssign::compile`. Each Reg/Mem shape-dispatch for these
  three sites now lives in exactly one place.

- **Typed-register IR — Stage 2i/2j + Stage 3b** — three more
  bounded cleanups:
  - **TokenMod general path** now routes through
    `GeneralBinopCascade`. The scratch Reg that
    `begin_general_binop` allocates doubles as the remainder
    register safediv writes into, closing the last binary-op
    hold-out. All eleven binary-op tokens share the same
    cascade scaffolding now.
  - **TokenInc / TokenDec collapsed into `emit_inc_dec`.** The
    two were near-identical; factor the shared lowering through
    a SafeUnaryStep function pointer and each TokenXx::compile
    now one-lines its delegation. Covers all four
    shape+position combinations (plain-var Reg, plain-var Mem,
    member/deref lvalue, each in postfix and prefix).
  - **Fn-pointer call dispatch cleanup.** Remove
    `reinterpret_cast<Operand *>(&gp)` UB in TokenCallFunc's
    fptr-call path by using an Operand local that both branches
    write into. Also add a load-Mem-to-Gp step for stack-backed
    fn-pointer variables so the downstream invoke's
    `ptr_op.as<Gp>()` can never see a Mem (previously a latent
    crash for any fn-pointer variable that got spilled).
  - **Five more IRBuilder::coerce unit tests.** Cover int↔real
    (cvtsi2sd/ss, cvttsd/ss2si) and int64→int32 narrow relabel
    paths that Stage 1/2 introduced but hadn't yet asserted.
  Tests: 23 IR (up from 18) + 25 datadef unit + 170 integration
  all pass.

- **Typed-register IR — Stage 2f/g/h general-fallback collapse +
  TokenNeg dead-code fix** — three more bounded refactors on the
  binary-op general fallback paths:
  - **TokenNeg dead-code branch fixed.** The old
    `is_plain_numeric_expr(left) && is_plain_numeric_expr(right)`
    fast path was unreachable (TokenNeg is unary, `left` is
    always NULL) and its body wrongly emitted `safeshl` instead
    of `safeneg`. Replaced with a real unary-right IR-normalized
    fast path.
  - **Pointer-arithmetic scaling extracted.** The two inline
    blocks in TokenAdd/TokenSub that emitted
    `imul rval, rval, sizeof(*ptr)` for `p ± n` collapse into
    `emit_pointer_arith_scale`. TokenSub's extra "right is not
    pointer" guard is now folded in and applies uniformly.
  - **General-fallback cascade extracted.** TokenAdd / TokenSub /
    TokenMul / TokenXor / TokenBand / TokenBor / TokenBSL /
    TokenBSR / TokenDiv all open-coded the same ~8-line
    `caller_dest + mirror_to_caller` scaffolding around their
    safe op. A `GeneralBinopCascade` struct plus
    `begin_general_binop` / `finish_general_binop` helpers
    replace it. Each general-path body now reads: begin cascade,
    compile left, tmp for right, compile right, (optional per-op
    work), safe op, finish cascade. TokenMod general path stays
    open-coded because its remainder register is woven through
    regdp.first in a way that doesn't factor cleanly.
  Net: ~200 more lines removed from compiler.cpp. 18 IR + 25
  datadef unit + 170 integration tests pass.

- **Typed-register IR — Stage 3a call return-binding unification**
  — `TokenCallFunc` had three divergent call-return paths: the
  function-pointer-call path (used `bind_call_return` — handled
  Reg + Mem + void uniformly), the `dlcall` path (open-coded
  `call->setRet(0, regdp.first->as<Gp>())` — would crash if the
  caller passed a Mem destination), and the variadic dlsym path
  (open-coded with separate double/int branches; the int branch
  carried a movsxd sign-extend whitelist for int32-returning
  libc functions, the double branch used movsd into a Reg-only
  dest). Both open-coded paths are latent Mem-destination bugs
  not exercised by today's tests. Widened `bind_call_return`
  with a `narrow_int_ret` flag that carries the movsxd dance
  the variadic dlsym path needs, then routed all three call
  sites through `bind_call_return`. Mem-destination returns now
  work uniformly across every call shape — the Stage-1 IR-route
  contract (emit_ir_value honors caller dest) now holds
  end-to-end through function calls.

- **Typed-register IR — Stage 2 arithmetic/comparison collapse** —
  the per-operator boilerplate that each binary token duplicated
  (normalize both sides, run the safe* helper, route through
  emit_ir_value) is now in shared helpers:
  - `emit_compare` with a `CmpKind` enum collapses the six
    comparison tokens (`TokenEquals`, `TokenNotEq`, `TokenLT`,
    `TokenLE`, `TokenGT`, `TokenGE`) to 4-line delegations.
  - `emit_plain_binop3` (3-arg safe ops) folds the plain-numeric
    fast paths of `TokenAdd`, `TokenSub`, `TokenMul`.
  - `emit_plain_divmod` (safediv + remainder) folds TokenDiv
    (dividend result) and TokenMod (remainder result).
  - `emit_plain_bitop2` (2-arg safe ops) folds TokenXor /
    TokenBand / TokenBor / TokenBSL / TokenBSR. BSL gains a
    plain-integer shortcut it didn't previously have.
  - `emit_compound_binop3` / `emit_compound_bitop2` /
    `emit_compound_divmod` fold the ten compound-assigns
    (`+=` / `-=` / `*=` / `/=` / `%=` / `<<=` / `>>=` /
    `&=` / `|=` / `^=`) onto the same helper surface.
  Token general-fallback paths (pointer arithmetic, regdp-
  cascade for complex expressions) are intentionally untouched
  — they still handle the operand shapes the fast path rejects.
  Net: ~380 lines removed across compiler.cpp with no behavior
  change. 18 IR + 25 datadef unit + 170 integration tests pass.

- **Typed-register IR — Stage 1 leaf-token sweep** — every leaf
  token that produces a value now routes its final operand
  through `emit_ir_value`, so shape normalization and type
  coercion happen in one place instead of being re-derived at
  each compile site. Sweep coverage: `TokenInt`, `TokenReal`,
  `TokenChar`, `TokenVar` (numeric, pointer, and function-
  reference paths), `TokenAddrOf`, `TokenAddrExpr`,
  `TokenMember`, `TokenDeref`, `TokenDerefExpr`,
  `TokenSubscript` (fixed-array, pointer, and container-call
  paths), `TokenSubscriptExpr`, `TokenVaArg`. The
  `TokenVar::compile` function-reference branch collapsed from
  three asymmetric branches (Reg-dest, Mem-dest, no-dest) to a
  single emit_ir_value call, as did the container-call tail of
  `TokenSubscript::compile`, which previously ignored a caller's
  `regdp.first=Mem` destination. To support function-pointer
  assignments through the IR, grew `IRBuilder::coerce` with a
  function-ref ↔ pointer passthrough (both are 8-byte addresses
  in a Gp; no instruction emitted, just a type relabel). One new
  unit test in `tests/unit/test_ir.cpp` covers the relabel. 18
  IR unit tests + 25 datadef unit tests + 170 integration tests
  all pass.

- **Typed-register IR — Stage 1 call-arg + operand normalization**
  — introduced IR-mediated helpers in `compiler.cpp`
  (`emit_ir_value`, `ir_from_operand`, `compile_token_normalized`,
  `compile_call_arg_normalized`, `add_funcsig_arg`,
  `set_funcsig_ret`, `set_invoke_arg`, `set_invoke_args`) that
  centralize the compile-site normalization patterns previously
  scattered across the `safe*` helpers and ad-hoc call-site code.
  Now that compile sites normalize into (Reg, concrete-type)
  before calling the `safe*` helpers, the Mem-path branches in
  `safeadd` / `safesub` / `safeor` / `safeand` / `safexor` /
  `safecmp` / `safeset{e,g,ge,l,le,ne}` are gone — the caller
  never hands in the un-normalized shape anymore. Grew
  `IRBuilder::coerce` to cover integer/pointer ↔ real
  conversions (`cvtsi2ss/sd`, `cvttss/sd2si`).

- **Typed-register IR — Stage 0 scaffolding** — new `IRBuilder`
  layer (in `include/madc_ir.h` / `src/madc_ir.cpp`) sitting
  between AST-walking compile() methods and asmjit emission.
  Values carry `(operand, DataDef, IRShape)` triples; shapes are
  `Reg`, `Mem`, `Imm`, `Addr`. Stage 0 exposes `load()`,
  `store()`, and `coerce()` — the minimum needed to centralize
  the integer sign/zero widening and real↔real conversion
  decisions currently scattered across `safemov`/`safeadd`/
  compile() sites. Emit-as-you-build: each call emits asmjit
  immediately, no deferred graph. 17 new doctest cases in
  `tests/unit/test_ir.cpp` check the emitted instructions via
  `StringLogger` (mov / movsxd / movzx / movsx / movss / movsd
  / cvtss2sd / cvtsd2ss for the right type+shape combinations).
  No existing tokens are ported yet — 170-integration-test
  baseline is untouched. Migration plan in
  `docs/plans/typed-register-ir.md`; rules in
  `.claude/rules/typed-register-ir.md` and `docs/rules/
  typed-register-ir.md`.

### Fixed

- **Float varargs promotion** — `float a = 1.5f; printf("%f", a);`
  used to print `0.000000`. The dlsym variadic call path in
  `TokenCallFunc::compile` checked `argrdp.second->is_real()`
  and passed the Xmm straight to `addArgT<double>()`, but for
  a float value the Xmm only held the low 32 bits (movss); C
  ABI requires cvtss2sd promotion to double before the varargs
  call. Also fixed for struct-member floats loaded via Mem.
  Companion changes: `TokenMember::compile`'s no-destination
  path now loads real-typed members into an Xmm (via
  safemov(Xmm, Mem)) instead of a Gpq, and `safemov(Xmm, Mem)`
  with no `d2` falls back to the Mem's actual size so a 4-byte
  Mem isn't read as a double via an 8-byte cvtsd2ss. Added
  `tests/testfloatvarargs.mad` covering single-local float
  varargs; struct-member double varargs and mixed-real printf
  still hit a separate asmjit-compiler register-allocator
  quirk filed in TODO.

- **Write through double-dereference (`**pp = v;`)** — used to
  SIGSEGV the compiler at address 0x8 inside
  `TokenAssign::compile`. `TokenDerefExpr::type()` returns
  `ttMember` (matching `TokenDeref`), but `TokenDerefExpr` is not
  derived from `TokenMember`. The LHS handling chain checked
  `dynamic_cast<TokenDeref *>` first (which caught plain `*p =
  v;`), then fell through to the `ttMember` branch and blindly
  accessed `tml->var.type` on a NULL dynamic_cast result —
  dereferencing NULL at field offset 8. Added an explicit
  `TokenDerefExpr` branch that mirrors the `TokenDeref` path,
  using the inner expression's compiled Mem as the write
  target. Read-through (`v = **pp;`) already worked because the
  read path compiles the LHS expression directly. Added
  `tests/testdoubleptrwrite.mad`.

- **Struct-member compound-assign on doubles** — `v.x += 2.5;`,
  `v.y *= 3.0;` and friends where the LHS is a `double` struct
  member used to throw `"safeadd() unable to add xmm to gp"` (or
  asmjit finalize error 25) because `resolveCompoundLHS`'s
  `ttMember` branch always loaded the member Mem via
  `load_mem_to_gpq` into a Gp, regardless of type. The local-
  variable path already handled `is_real()` with an Xmm load;
  the member and `*deref` paths now mirror it. Added
  `tests/teststructdoublecompound.mad` covering `+=` / `-=` /
  `*=` / `/=` on double members, one op per helper function to
  sidestep two other pre-existing bugs (struct-member double
  varargs printf, and multi-float interleaved with printf —
  both filed in TODO).

- **`!=` / `<` / `<=` / `>` / `>=` comparisons with Mem destination** —
  completes the roll-out of the `==` fix from commit `6318e6b`.
  `TokenNotEq` / `TokenLT` / `TokenLE` / `TokenGT` / `TokenGE`
  previously passed the caller's destination verbatim to their
  setcc helper. When `TokenAssign` handed them a Mem (typical
  `int r = *p < 'm';` / `int r = a != b;` pattern), the setcc
  target came back Mem and either the safecmp helper or the
  setcc helper threw. Now each operator allocates its own Gp
  when the caller's dest isn't a Reg, runs compare+setcc there,
  and mirrors the 0/1 result back into the caller's Mem via
  safemov. Added `tests/testderefcmp.mad` covering all five
  operators on a `*char` lvalue with Mem destinations.

### Added

- **C integer and float literal suffixes** — the lexer now consumes
  `u`/`U`, `l`/`L`, up to three in a row (so `ul` / `UL` / `lu` /
  `LU` / `ull` / `ULL` / `lul` etc. all work) after a decimal /
  hex / binary integer literal, and `f`/`F`/`l`/`L` after a
  real literal. madc's `int` is 64-bit, so the size hints are
  informational; signedness-via-suffix isn't propagated yet (tracked
  separately). Previously `1u` lexed as `1` followed by identifier
  `u`, breaking `flags |= 1u << 3` and `9000000000LL` literals.
  Added `tests/testintsuffix.mad`.

- **int64 literals that don't fit in int32 store correctly** —
  `long long big = 9000000000;` was truncating to the low 32 bits
  because `mov qword ptr [mem], imm` in x86 only carries a 32-bit
  sign-extended immediate. `safemov(Operand, Operand)` now bounces
  through a register for out-of-range imm-to-Mem stores (imm fits
  in int32 → direct store; otherwise `mov tmp, imm64` then
  `mov [mem], tmp`).

- **C pointer arithmetic scales by element size** — `p + n` on `T *`
  previously added `n` bytes instead of `n * sizeof(T)`, so
  `int *q = p + 2;` advanced `q` by 2 bytes and `*q` read garbage.
  `TokenAdd::compile()` and `TokenSub::compile()` now scale the
  offset by the pointer's pointed-to element size (or the element
  size of a fixed-array base that's decaying to pointer). `char *`
  and `void *` (1-byte elements) skip scaling. Added
  `tests/testptrarith.mad`.

- **Compound-assign on array subscript lvalues** — `resolveCompoundLHS`
  previously threw "+= on a non-variable lval" (as a raw C-string throw
  that further corrupted the error-location printout) for any
  `arr[i] += n;`-style expression. Now computes the element Mem
  operand directly from `TokenSubscript::object` + `index` and uses it
  as the load/compute/store target. Added `tests/testarrayc.mad`.

- **`sizeof(expr)` now handles postfix chains** — `sizeof(buf[0])`,
  `sizeof(obj.field)`, `sizeof(ptr->field)` parse by routing the
  identifier + `[` / `.` / `->` tail through `parsePostfixChain()`
  and taking the resulting node's datadef size. Previously only
  `sizeof(TYPE)` and `sizeof(var)` (bare variable) worked; the
  subscript form `sizeof(arr) / sizeof(arr[0])` is idiomatic C for
  array-length compile-time constants. Added
  `tests/testsizeofexpr.mad`.

### Fixed

- **`*e == 0` comparisons with Mem operands** — two gaps:
  (a) `safecmp(Operand, Operand)` rejected Mem lval / rval outright
      — for `*e == 0` the deref yields a Mem operand. Now bounces
      Mem operands through a sign-extending Gp temp.
  (b) `TokenEquals::compile` handed the caller's destination
      verbatim to safesete. When TokenAssign passed a Mem (typical
      for `int x = *e == 0;`), safesete had no register to set.
      Now allocates its own Gp for the compare/sete and mirrors the
      0/1 result back into the caller's Mem via safemov.
  `!=`, `<`, `<=`, `>`, `>=` have the same pattern — only `==` is
  fixed here; filed in TODO. Added `tests/testderefeq.mad`.

- **Dereferencing an address-taken pointer (`int **pp = &p; *p`)** —
  taking `&p` of a pointer variable spilled `p` to a stack Mem
  slot. Subsequent `*p` went through `TokenDeref::operand` which
  did `ptr_op.as<x86::Gp>()` on the Mem — a silent reinterpret
  that returned a bogus Gp. The resulting `ptr(gp, 0, 8)` had
  garbage register ids and asmjit's finalize flagged error 26,
  after which the JIT executed illegal instructions (SIGILL /
  SIGSEGV). Now loads the pointer value from the Mem slot into a
  fresh Gp before using it as the base. Added
  `tests/testdoubleptr.mad`.

- **`float` variables and real↔real casts** — two gaps kept
  4-byte floats broken:
  (a) `safemov(Mem, Xmm)` with a float-sized Mem emitted plain
      `movss` on a double-valued Xmm, storing the low 32 bits of
      the double (mantissa) instead of a valid float32. Now
      `cvtsd2ss`s when source is double and dest is float (and
      `cvtss2sd` for the opposite direction).
  (b) `TokenCast::compile` reinterpreted real↔real casts. `(double)
      flt_var` therefore passed the raw 32-bit float bits to a
      variadic printf which reads them as a double (→ 0.0).
      Now emits `cvtss2sd` / `cvtsd2ss` when src and dst sizes
      differ.
  Added `tests/testfloat.mad` — split across functions because a
  separate asmjit register-allocation interaction (multiple floats
  with interleaved printf calls in one function spills to an
  uninitialised slot) is filed in TODO.

- **`s->items[i]` read/write through pointer-typed struct members** —
  two bugs collaborated to SIGSEGV (or return garbage) when a
  subscript operated on a pointer held in a struct member:
  (a) `TokenAssign`'s ttSubscript write path only `dynamic_cast`'d
      to `TokenSubscript` (Variable-based subscript) — for a
      `TokenSubscriptExpr` (expression-based, which the parser builds
      for member / subscript / deref bases), the cast returned NULL
      and the next `tsub->datadef()` crashed at address nil.
  (b) `TokenSubscriptExpr::compile` unconditionally `lea`'d when the
      base operand was a Mem. That's correct for a Mem that IS the
      backing storage (fixed arrays), but wrong for a Mem that HOLDS
      a pointer value (`s->items` where items is `int *`) — reads
      then returned bytes from the member slot itself instead of the
      pointed-to array.
  Fix: `TokenAssign` now emits an inline store path for
  `TokenSubscriptExpr` lvalues (including the dtSTRING → char*
  coercion for char*-element arrays), and `TokenSubscriptExpr::compile`
  picks `mov` vs `lea` based on whether base_expr's datadef is a
  pointer. Added `tests/teststructptrsub.mad` covering struct-of-
  pointer read/write and a heap-allocated stack via push/pop helpers.

- **Signed integer division with negative dividend** — `a / b` where
  `a < 0` used to produce wildly wrong quotients (`-17 / 5` came out
  as 858993455 instead of -3). x86's `idiv` treats rdx:rax as a
  128-bit signed dividend; the caller has to sign-extend rax into
  rdx before the divide. `safediv()` was called with a zeroed
  remainder register (via `safexor`), which is correct for unsigned
  division but produces a huge positive 128-bit dividend when rax is
  negative. Now emits `cqo` inside `safediv` for signed types
  (unsigned types keep the caller's zero-extended path). Affects
  both plain `/` `%` and compound `/=` `%=`. Added
  `tests/testsigneddiv.mad`.

- **`char *arr[] = {"a","b",...};` init stores c_str() pointers** —
  TokenDecl's fixed-array init-list path wrote each init's compile
  result straight into the slot, so for a char*-element array the
  slot received the std::string object's address instead of its
  c_str() pointer. Same coercion the `names[0] = "literal"`
  assignment path already applies. Added `tests/teststrarrinit.mad`.

- **Compound-assign on local double/float variables** — `x += 5.0;`,
  `x *= 2.0;` and friends on a stack-local double used to throw
  `"safeadd() unable to add xmm to gp"` because `resolveCompoundLHS`
  always loaded the Mem lval via `load_mem_to_gpq` into a Gp, so the
  subsequent `safeadd(Gp, Xmm)` had no valid overload. Now the
  variable path checks `r.type->is_real()` and loads into an Xmm via
  `safemov`; the Xmm-vs-Xmm arithmetic then proceeds normally.
  Struct-member compound-assign on doubles still falls through the
  Gpq path — filed in TODO. Added `tests/testdoublecompound.mad`.

- **`safemov(Mem, Xmm)` now stores the xmm to memory** — the overload
  used to unconditionally throw `"safemov() unable to move xmm to
  mem"`, so every `double` / `float` arithmetic expression that had
  to mirror its result back into a Mem destination (local variable,
  struct member) bombed at compile. Now emits `movsd` / `movss`
  based on the Mem's declared size. Added `tests/testdoublestore.mad`.

- **Cast body no longer consumes trailing binary operators** —
  `(long)q - (long)nums` used to parse as `(long)(q - (long)nums)`
  because the cast body used `parseExpression(.., true)` which
  greedily continues past binary operators. Cast now uses
  `parsePostfixChain` when the body is a bare identifier with an
  optional `->`/`.`/`[]` tail, and falls back to `parseExpression`
  only for parenthesized bodies / unary-operator heads / function
  calls inside the cast. Known remaining: `(long)(expr)` with an
  inner parenthesized expression still uses the greedy path.

- **Negative int32 returns from dlsym libc functions sign-extend
  correctly** — `strcmp("abc", "abd")` returns `-1` in EAX on Linux
  x86-64, but the upper 32 bits of RAX are indeterminate (typically
  zero-extended by the compiler). madc's dlsym variadic call path
  stored the raw RAX into an int64 destination, so `r < 0` evaluated
  to false (the value read back as `0x00000000FFFFFFFF`, a large
  positive). Now emits `movsxd ret, eax` after the call for a
  curated whitelist of known int32-returning libc functions
  (strcmp/memcmp family, char I/O, printf/scanf family, process
  syscalls, network/socket, time, etc.). Pointer / int64 returners
  (malloc, strdup, strtol, time, lseek...) stay untouched. Added
  `tests/teststrcmpret.mad`.

- **String literals stored into `char *` array elements go through
  string_cstr** — `names[0] = "alice";` where `names` is `char
  *names[3]` used to write the std::string object's address into the
  slot instead of its c_str() pointer; subsequent `%s` or `strcmp()`
  reads returned garbage. TokenAssign's ttSubscript path now applies
  the same dtSTRING → char* coercion the plain `char *p =
  "literal";` path uses. Added `tests/teststrcharptrarr.mad`.

- **`(char *)` cast of std::string expressions coerces via
  string_cstr** — `TokenCast::compile()` used to just reinterpret the
  inner operand's type without changing its value, which for a
  std::string source meant the caller kept the `std::string` object
  address and treated it as a `char *`. `(char *)(cond ? "a" : "b")`
  and `(char *)str_var` both rendered garbage when `%s`-printed.
  `TokenCast` now detects dtSTRING → char* and routes the inner
  expression through `string_cstr` before returning. Added
  `tests/teststringcast.mad`.

- **Compound-assign on narrow (1/2/4-byte) lvalues no longer
  SIGSEGVs** — `resolveCompoundLHS` widens Mem lvalues into a Gpq via
  `load_mem_to_gpq`, but kept `r.type` at the narrow source type. The
  compound-op handlers then allocated a narrow `tmp` matching that
  type and compiled the RHS into it, producing e.g. `safeor(Gp64,
  Gp8)` which is not a legal encoding. asmjit silently dropped /
  malformed the op, `cc.finalize()` returned an error, and the JIT
  executed invalid code — the resulting crash manifested as a
  dereference of whatever register was left stuck holding the RHS
  value. Fixed by switching `r.type` to `ddINT64` whenever we widen
  (variable / member / deref / subscript paths). The writeback Mem
  keeps its original size so the final `safemov(Mem<1|2|4>, Gp64)`
  truncates correctly via the matching-width register view. Added
  `tests/testcompoundnarrow.mad` covering char/short locals, struct
  char/short members, and char array subscripts.

- **String-typed ternary branches now coerce correctly** — follow-up to
  the ternary-to-Mem fix in v0.9.1. Two additional paths were missing:
  (1) the parser didn't set `TokenTerQ::_datatype` from the branches,
  so `TokenAssign`'s dtSTRING → char* coercion couldn't see the ternary
  as string-typed; (2) `TokenTerQ::compile()` set `regdp.second` to
  `&ddINT64` by default, hiding the branch type from variadic-arg
  coercion (printf's dtSTRING → const char* via string_cstr). Now
  parser propagates the true branch's datadef (falling back to the
  false branch when the true is int/NULL), and compile uses the stored
  `_datatype` for `regdp.second`. `const char *s = cond ? "a" : "b";`
  and `printf("%s", cond ? "T" : "F");` both work. Added
  `tests/testternarystring.mad`.

## [v0.9.1] — 2026-04-24 — Silent codegen bug roll-up: ternary to Mem, shared literals, `int = -N`, fn-ptr casts

### Added

- **C function-pointer cast syntax** — the cast parser now recognizes
  `(RET (*)(PARAMS)) expr` after the return type (and any pointer
  stars) by consuming `(*)` and then reusing `parseFnPtrParams()` to
  build a `DataDefFPTR`. Unblocks `qsort(.., (int(*)(const void *,
  const void *)) cmp_fn);` as used in SMAUG's `db.c sort_exits()`.
  Added `tests/testfnptrcast.mad`.

- **Case values accept constant integer expressions** —
  `TokenSWITCH::parse()` used to store `nextToken()` as the case
  value, which worked only for a single-token literal and broke on
  `case EOF:` (where `EOF` expands to `-1`), `case (FOO+1):`, or
  `case 1+1:`. The parse now uses `parse_constant_integer_expression`
  and wraps the evaluated int64 in a `TokenInt` for compile(). Also
  extended `resolve_integer_constant` to accept `ttChar` so
  `case 'a':` still works through the new path. Added
  `tests/testcaseconstexpr.mad`.

### Fixed

- **`switch` expression now sign-extends narrow signed types** —
  `TokenSWITCH::compile()` loaded the switch expression via plain
  `cc.mov(r64, m32)` / `cc.mov(r64, r32)`, which zero-extends and
  leaves the upper bits clear. A negative `int`/`short`/`char`
  expression would therefore never match a negative case constant
  (`case -2:` missed when `i` held `-2`). Now routes through
  `safemov(..., &ddINT64, expr_type)`, and `safemov(Gp, Gp)` was
  updated to emit `movsxd` / `movsx` for signed widening (it used to
  unconditionally `movzx`).

- **Container-type keywords (`map`, `vector`, `set`, `list`) now usable
  as identifiers at statement position** — `parseStatement()`'s
  `ttKeyword` case used to dispatch `map` / `vector` / `set` / `list`
  straight to the keyword-specific parser, which expects a templated
  use (`map<K,V>` etc.). In plain C code these names legitimately
  appear as local variables, parameters, or struct members —
  `MAP_DATA *map; map->vnum = fread_number(fp);` in SMAUG's `db.c` is
  the motivating case. When the token after one of these keywords is
  not `<`, `parseStatement()` now resets the prior-token context and
  routes through `parseExpression()` instead. `contextual_identifier_name()`
  was also missing tkMAP / tkVECTOR / tkSET / tkLIST — it now returns
  their keyword `str` so downstream code sees `"map"` instead of `""`.
  This advances the external MadSMAUG umbrella past `db.c`'s map
  loader. Added `tests/testmapidentifier.mad`.

- **`->` after a dereference expression now falls through to the
  expression-backed path** — `TokenDeref` / `TokenDerefExpr` both
  report `type() == ttMember` (for assignment-compat purposes) but
  are not `TokenMember` instances. `parseExpression()`'s `->` handler
  used to throw `"expression before '->' must be a pointer to struct"`
  when the `dynamic_cast<TokenMember *>` failed, without trying the
  pointer-datadef fallback that already exists for general
  expression-parent `->` uses. The ttMember branch now falls through
  to the expr-backed path when the cast fails, so the classic
  `(*pp)->field` idiom (qsort comparators etc.) parses correctly.
  This advances the MadSMAUG umbrella past `db.c`'s `exit_comp()`
  sort helper. Added `tests/testderefparenarrow.mad`.

## [v0.9.0] — 2026-04-23 — SMAUG Phase F continues: MadSMAUG bootstrap + compiler fixes

### Added

- **Empty-clause `for` regression coverage** — added
  `tests/testforemptyclause.mad` and `.expect` to cover `for (; cond; inc)`,
  `for (init; ; inc)`, and `for (init; cond; )`.

- **Statement-leading unary dereference regression coverage** — added
  `tests/teststmtleadingunary.mad` and `.expect` to cover statement-start
  `*ptr = ...;` after control-flow blocks, which previously leaked prior
  parse context into the new statement.

- **`register` parameter regression coverage** — added
  `tests/testparamregister.mad` and `.expect` to cover function definitions
  that spell parameters as `register int x` / `register char *s`.

- **Pointer pre-increment dereference regression coverage** — added
  `tests/testderefpreincptr.mad` and `.expect` to cover `c = *++p;`, which
  must parse and type-check the same as `c = *(++p);`.

- **Build-then-run helper script** — added `scripts/build_then.sh` so local
  debugging can serialize `make -C src` and the next command against the
  freshly built `bin/madc`. This avoids stale-binary runs and makes targeted
  repro/test loops (`scripts/build_then.sh bin/madc tests/foo.mad`) safer.

- **Struct-member function-pointer regression coverage** — added
  `tests/testfnptrmemberarrow.mad` and
  `tests/testfnptrmemberarrow.expect` to cover `cmd->fn(args)`, the
  classic C parenthesized form `(*cmd->fn)(args)`, and typed extraction
  from `cmd->fn` into a local function-pointer variable before indirect
  invocation.

- **`struct servent` interop in embedded `<netdb.h>`** — 32-byte
  glibc-matching layout (`char *s_name; char **s_aliases; int s_port;
  char *s_proto;`) so `getservbyname()` / `getservbyport()` return
  values now expose `serv->s_port` / `serv->s_name` / `serv->s_proto`
  directly. The `s_port` field holds the port in network byte order,
  matching glibc — user code calls `ntohs(serv->s_port)` to get a
  host-order integer. This closes the MadSMAUG umbrella bootstrap's
  `sock.sin_port = serv->s_port;` front edge in upstream `ident.c`.
  `tests/testservent.mad` drives a real `getservbyname("ftp","tcp")`
  and `getservbyname("http","tcp")`, verifies host-order port values
  (21, 80) after `ntohs()`, and asserts `sizeof(struct servent) == 32`.

- **Extended `<errno.h>` socket/network constant coverage** —
  `<errno.h>` now defines the Linux x86-64 values for `EWOULDBLOCK`
  (alias of `EAGAIN`), `EINPROGRESS`, `EALREADY`, `ENOTSOCK`,
  `EDESTADDRREQ`, `EMSGSIZE`, `EPROTOTYPE`, `ENOPROTOOPT`,
  `EPROTONOSUPPORT`, `ESOCKTNOSUPPORT`, `EOPNOTSUPP`, `EPFNOSUPPORT`,
  `EAFNOSUPPORT`, `EADDRINUSE`, `EADDRNOTAVAIL`, `ENETDOWN`,
  `ENETUNREACH`, `ENETRESET`, `ECONNABORTED`, `ECONNRESET`, `ENOBUFS`,
  `EISCONN`, `ENOTCONN`, `ESHUTDOWN`, `ETIMEDOUT`, `ECONNREFUSED`,
  `EHOSTDOWN`, `EHOSTUNREACH`, plus the System V / extended POSIX
  errors (`EDEADLK`, `ENAMETOOLONG`, `ENOLCK`, `ENOSYS`, `ENOTEMPTY`,
  `ELOOP`, `EDOM`, `EILSEQ`, `EOVERFLOW`, `ENODATA`, `ETXTBSY`,
  `EUSERS`, `EDQUOT`, `ESTALE`, `ENOMSG`). SMAUG's socket bootstrap
  paths (`errno != EINPROGRESS`, `errno != ECONNREFUSED`) can now
  compile.

- **Extended `<fcntl.h>` constant coverage** — the embedded `<fcntl.h>`
  header now defines the `fcntl()` command constants
  (`F_DUPFD`, `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_GETLK`,
  `F_SETLK`, `F_SETLKW`, `F_SETOWN`, `F_GETOWN`, `F_DUPFD_CLOEXEC`) at
  their Linux x86-64 values, plus the missing open flags `O_NDELAY`
  (alias of `O_NONBLOCK`), `O_ASYNC`, `O_DIRECTORY`, `O_NOFOLLOW`, and
  the `FD_CLOEXEC` file-descriptor flag. This closes the MadSMAUG
  umbrella `F_SETFL` bootstrap front edge in upstream `ident.c`'s
  `fcntl(a->afd, F_SETFL, FNDELAY)` path.
  `tests/testfcntl.mad` drives a real `F_GETFL` / `F_SETFL` round-trip
  on an open file descriptor and verifies that `O_NONBLOCK` is actually
  reflected by a follow-up `F_GETFL`.

### Added

- **Ternary operator now writes its merged result to Mem destinations** —
  `TokenTerQ::compile()` only honoured a Gp-register caller destination
  when returning its merged result; if the caller passed a Mem (typical
  for `int r = cond ? a : b;` where TokenAssign targets `r`'s stack
  slot), the branches wrote to a fresh internal Gp and the caller's Mem
  was never updated — `r` kept its zero-initialised value. Every
  int-valued ternary assigned to a local was silently producing 0. Now
  stores the merged result into the caller's Mem via safemov before
  returning. Added `tests/testternaryvalue.mad`. Note: string-typed
  ternary branches (`const char *s = cond ? "a" : "b";`) still don't
  coerce `std::string` to `char *` correctly — filed in TODO.

- **`DIR` typedef in embedded `<dirent.h>`** — glibc exposes `DIR` as a
  typedef for an opaque struct, but madc's embedded `<dirent.h>` only
  defined `struct dirent` and the `DT_*` constants. C code using
  `DIR *dp;` (SMAUG's `db.c` and others) now parses via the new
  `typedef struct __dir_opaque DIR;`. Added `tests/testdirtype.mad`.

### Fixed

- **Unary `*` on a postfix chain no longer swallows trailing binary
  operators** — the old fallthrough for `*ident->member`,
  `*ident.member`, `*ident[idx]` cases called `parseExpression(...,
  true)` on the postfix chain, which greedily consumed trailing binary
  operators such as `*p->name == '$'`. The inner parse would return a
  `TokenEq` (boolean result), and the outer `!dtype->is_pointer()` check
  then threw `"cannot dereference non-pointer type"`. Added a
  `parsePostfixChain()` helper that manually builds `TokenVar` /
  `TokenMember` / `TokenSubscriptExpr` nodes stopping at the first
  non-postfix token, and routed the `*` handler's identifier+postfix
  fallthrough through it. Added `tests/testderefmember.mad` covering
  `*p->name == 'h'`, `*n.name == 'h'`, and chained `*op->inner->name`.

- **Global / literal variables re-emit their address load on every access** —
  `TokenCpnd::voperand()`'s cache-hit branch re-runs `movreg` for global
  variables each time they are referenced (so the register always holds
  the up-to-date global value), but the check excluded `is_constant()`
  vars. For a string literal (`addLiteral` calls `makeconstant()`), the
  initial `mov reg, imm(var->data)` load was therefore emitted only at
  the first use site. If that site was inside a conditional branch
  (a switch case, an if/else arm) that didn't execute at runtime, the
  asmjit-spilled slot was never initialised, and subsequent uses on
  other branches read garbage — the most visible symptom was identical
  `printf("...")` calls across two switches printing nothing on the
  second switch. The exclusion on `is_constant()` was removed; fixed
  arrays still skip `movreg` to avoid re-loading the element-zero of
  the backing storage. Added `tests/testdupliteral.mad`.

- **Negative-constant initializer `int a = -2;` now stores -2** — two
  separate bugs collaborated to leave `a` at 0:
  1. `TokenNeg::compile()` set `mirror_to_caller` when the caller's
     destination was Mem and allocated a fresh temp register, but
     never actually mirrored the negated result back to the caller's
     Mem — so the stack slot was left untouched. Fixed by emitting
     `safemov(*caller_dest, rval, ...)` after the `safeneg`, matching
     the pattern already used by TokenAdd / TokenSub / TokenMul etc.
  2. `parseExpression()`'s conditional-end-at-`)` short-circuit
     returned `exStack.top()` without flushing the operator stack.
     For `-(2)` this lost the pending unary `-`; for `c = -(2)` it
     also lost the pending `=`. Now flushes the opStack via
     `popOperator` before returning.
  Added `tests/testneginit.mad` covering `int a = -2;`, post-decl
  `d = -7;`, `int e = -(2);`, `int f = -(3+4);`, and the sanity-check
  `0 - 2` form.

- **`->` after a function-call now evaluates the call** — `TokenMember::operand()`'s
  chained-arrow path previously called `parent_expr->operand()` unconditionally,
  which works for chained `TokenMember` parents (they re-materialize their own
  address each call) but silently failed for expression parents such as
  `TokenCallFunc` / `TokenCallMethod` / `TokenSubscript` / `TokenDerefExpr`,
  whose `operand()` returns a fresh uninitialized register without emitting
  the underlying computation. The arrow chain therefore read a garbage
  register as the pointer. `TokenMember::operand()` now invokes
  `parent_expr->compile(pgm, fresh_regdp)` for any non-`ttMember` parent,
  so `get_slot(i)->value`, `cmd->fn(args)->field`, and similar patterns
  emit the producing computation before dereferencing. Added
  `tests/testglobalptrarrayarrow.mad` as the regression.

- **Mem-backed arithmetic expressions now materialize through temps** —
  plain arithmetic and bitwise operators (`+`, `-`, `*`, `/`, `%`, `|`,
  `^`, `&`, `<<`, `>>`) now allocate a temporary register when the caller
  passes a Mem destination, then mirror the result back after the op.
  Compound-assignment LHS resolution now does the same for stack-backed
  variables. This fixes SMAUG patterns like `number = (number * 10) + ...`,
  `number *= (multiplier = 1000)`, and `hash = len % STR_HASH_SIZE` in
  `bet.h` / `hashstr.c`. Added targeted regressions
  `tests/testassignexprmem.mad` and `tests/testcompoundassignmem.mad`.

- **Unary `*` now accepts fixed arrays via C array-to-pointer decay** —
  the parser's direct identifier dereference path now treats fixed arrays
  like `char arg[N]` as dereferenceable element pointers in value context,
  so SMAUG forms such as `if ( !*arg )` parse correctly. Added
  `tests/testderefarray.mad` to cover `!*buf` and plain `*word`.

- **Traditional `for` now accepts empty init/condition/increment clauses** —
  `TokenFOR::parse()` and `TokenFOR::compile()` now handle C forms like
  `for (; cond; inc)`, `for (init; ; inc)`, and `for (init; cond; )` instead
  of treating empty clauses as parse failures. This advances the external
  MadSMAUG umbrella through `interp.c`'s `for ( ; *arg != '\0'; arg++ )`
  loop in `one_argument2()`. Current full-batch status: 133 integration
  tests pass.

- **Statement-leading unary operators now reset expression context** —
  `parseStatement()` now clears prior-token context before handing an
  operator-led statement to `parseExpression()`, so statement-start forms
  like `*arg_first = LOWER(*argument);` are parsed as unary dereference
  instead of as a missing left operand for binary `*`. This advances the
  external umbrella through the first `one_argument2()` dereference-assignment
  path in `interp.c`.

- **`register` is now accepted in function parameter lists** —
  `parseFunction()` now tolerates storage-class hints like
  `register char *argument` alongside existing `const` handling when reading
  parameter types. This advances the external umbrella through
  `char *one_argument2(register char *argument, char *arg_first)`.

- **Prefix/postfix inc/dec now preserve operand type metadata** —
  `TokenInc` and `TokenDec` now report the same `datadef()` as their child
  expression, so pointer expressions such as `*++argument` and `*--p` remain
  dereferenceable. This closes the `ch = *++argument;` front edge in
  `interp.c` and moves the MadSMAUG umbrella to the next dereference gap at
  `/workspace/MadSMAUG/src/SMAUG.mad:1178:12`.

- **Bare unary `&` now accepts postfix lvalue chains** — the parser no
  longer limits the non-parenthesized address-of form to plain identifiers.
  Expressions like `&cmd->userec`, `&op->in.x`, and other member/subscript
  postfix chains now parse through the same addressable-expression path as
  `&(cmd->userec)`, which advances the external MadSMAUG umbrella bootstrap
  past `interp.c`'s `update_userec(&time_used, &cmd->userec);` front edge.
  Added `tests/testaddrmemberparen.mad` / `.expect` coverage for nested dot
  and arrow member-address forms. Current full-batch status: 133 integration
  tests pass.

- **Typed Mem-backed local writeback regressions** — three stack-local paths
  now preserve narrow numeric storage correctly instead of bouncing through
  accidental 64-bit temporaries:
  - function-pointer indirect calls now bind integer returns into Mem
    destinations as well as registers, which fixes `int x = op(10, 20);`
    in `tests/testfnptrtypedef.mad`
  - `cin >>` integer and floating-point extraction now writes back to the
    actual lvalue operand for stack-backed locals instead of only updating a
    transient loaded register, which fixes `tests/testcin.mad`
  - generic `safemov(Mem <- Mem)` now copies through a typed temporary
    instead of an unconditional `Gpq`, which fixes stack-local `uint32_t`
    assignment / print paths (`tests/testassign.mad`, `tests/testint.mad`)

- **Struct-body function-pointer members now preserve `DataDefFPTR`** —
  declarators like `void (*callback)(void *)` inside `struct` bodies no
  longer degrade to a plain `int64_t` placeholder. `TokenSTRUCT::parse()`
  now routes the member parameter list through `parseFnPtrParams()` and
  stores a real `DataDefFPTR`, which keeps the signature available through
  member lookup so `cmd->fn(args)` and `FPTR_TYPE *fp = cmd->fn; fp(args);`
  compile.

- **Parenthesized struct-member function-pointer calls** —
  `TokenMember::datadef()` now reports the member's actual stored type
  instead of inheriting `TokenCallFunc`'s callable-return behavior, so
  `(*cmd->fn)(args)` now sees `cmd->fn` as a `DataDefFPTR` member rather
  than as the function's return type. This closes the remaining SMAUG-style
  direct-dispatch spelling gap for function-pointer struct members.

- **`parseExpression` SIGSEGV when `->` follows a dereference expression** —
  `TokenDeref` and `TokenDerefExpr` both reuse `TokenType::ttMember` as their
  `type()` (for assignment-compat purposes), so when the LHS of `->` was a
  dereference the `dynamic_cast<TokenMember *>` in the `tkDeRef` branch
  returned `NULL` and the subsequent `tm->var` read crashed at offset `0x8`.
  `Program::parseExpression()` now null-guards that cast and throws a proper
  "expression before '->' must be a pointer to struct" error instead. This
  replaces the SIGSEGV that MadSMAUG's umbrella bootstrap was hitting during
  `ident.c` parsing with a clean diagnostic, and the umbrella now advances
  past the crash to the next structural front edge (address-of struct member
  via pointer, `&cmd->userec`).

## [v0.9.0] — 2026-04-19 — SMAUG Phase F continues (session 2)

### Fixed

- **Control-flow condition parsing now uses a reusable parenthesis helper** —
  `if`, `while`, and `do/while` conditions now parse through one helper
  instead of hand-rolled stop behavior at each keyword. This fixed the
  `_Bool` regression where `if (a) stmt; else stmt;` with single expression
  statements bound incorrectly. Added/stabilized regression coverage:
  `tests/testc23_bool.mad`.

- **Assignment-expression call results now persist into Mem-backed locals** —
  stack-backed local numerics exposed a gap where function-call RHS values
  assigned into local lvalues were returned to the enclosing expression but
  not reliably stored back through a Mem destination. Call return binding now
  routes through a helper that handles register and memory destinations
  generically, which restores assignment-in-condition behavior like
  `while ((x = next_val(count)) < 35)`. `tests/testassigninexpr.mad` now
  runs cleanly again.

- **Prefix/postfix inc/dec on Mem-backed locals** — once ordinary local
  scalar numerics became stack-backed for stability, prefix/postfix fast
  paths that assumed register-only variables broke value semantics and the
  final `while (x--)` loop case. `TokenInc::compile()` and
  `TokenDec::compile()` now handle Mem operands explicitly, and
  `tests/testpostfix.mad` now passes with the expected output.

- **Pointer-return typing for dereferenced call results** — generic
  `rtPtr(...)` builtin/external signatures now resolve through a helper in
  `addFunction()` instead of a few hard-coded pointer cases, so
  `__errno_location()` keeps an `int *` return type during parsing. The unary
  dereference path also no longer assumes every identifier after `*` is a
  plain variable; when followed by `(` it parses the full call expression.
  This fixes dereferencing call results such as `*get_msg()`,
  `*(version.c_str())`, and `errno` / `*(__errno_location())`. Added targeted
  regressions: `tests/test_ptr_fn_deref.mad`,
  `tests/test_get_argv_deref.mad`, and `tests/test_errno_deref.mad`.

- **MadSMAUG bootstrap parser/front-end follow-ups** — continued the external
  `MadSMAUG` umbrella bootstrap through `ident.c` / `interp.c` and landed the
  next compiler compatibility fixes:
  - struct-body comma declarators now parse (`struct sockaddr_in us, them;`)
  - grouped RHS expressions after deref assignment no longer crash
    (`*p = (x);`, `UMAX(*p, ...)`)
  - chained unary dereference now parses for pointer chains used in SMAUG
    (`*s`, `**s`)
  - `for (...) *ptr = ...;` style bodies starting with a unary operator now
    parse correctly
  - nested pointer `DataDefPTR` types now report `is_pointer() == true`

### Known Current Front Edge

- **MadSMAUG bootstrap now stops at `timerisset`** — after the Mem-backed
  arithmetic fixes and fixed-array unary-deref parsing, rerunning the full
  umbrella bootstrap advances the front edge to
  `/workspace/MadSMAUG/src/SMAUG.mad:1257:18` complaining about undeclared
  `timerisset` (upstream `interp.c` / `do_timecmd`). The next session should
  add `timerisset` / related timeval helper macro coverage and rerun the
  bootstrap immediately.

### Docs

- **Cross-agent hand-off workflow** — added `docs/agent-handoff.md` as the
  canonical playbook for Codex CLI / Claude Code session transfer. It defines
  the read order, source-of-truth rules, end-of-session update contract,
  default task split, and agent-owned feature-branch convention.

- **Claude rule coverage for hand-offs and KG sync** — added
  `.claude/rules/session-handoff.md` and `.claude/rules/knowledge-graph.md`,
  plus paired reasoning docs under `docs/rules/`. Updated the branching rule
  to support agent-owned WIP branches with `-claude` / `-codex` suffixes.

- **Retired `docs/status_report.md` as a live source** — it now points agents
  at `claude_status.json`, `TODO.md`, `CHANGELOG.md`, `docs/test-status.md`,
  and `docs/agent-handoff.md` instead of acting as a stale parallel snapshot.

### Tests

- **Plain bitwise `>>` integration coverage** — `tests/testbsl.mad` now
  exercises both left and right arithmetic bit shifts, and
  `tests/testbsl.expect` asserts the concrete outputs. This closes the
  remaining backlog item where `>>=` and `cin >>` were covered but plain
  `>>` had no integration assertion.

- **C `_Bool` regression coverage** — added `tests/testc23_bool.mad` and
  `tests/testc23_bool.expect` to cover scalar `_Bool` declarations,
  branching, and fixed-array initialization.

- **Binary literal regression coverage** — added `tests/testbinlit.mad` and
  `tests/testbinlit.expect` to cover `0b...` / `0B...` integer literals in
  assignments, expressions, and conditions.

- **`restrict` regression coverage** — added `tests/testrestrict.mad` and
  `tests/testrestrict.expect` to cover `restrict` in pointer declarations and
  function parameters.

- **`flock()` regression coverage** — added `tests/testflock.mad` and
  `tests/testflock.expect` to cover embedded `<sys/file.h>` plus `flock()`
  and the `LOCK_*` constants through the libc dlsym fallback.

- **Include-once regression coverage** — added `tests/testincludeonce.mad`
  and `tests/testincludeonce.expect` to cover repeated local `#include`
  directives being ignored after the first tokenization pass.

### Maintenance

- **Closed stale `%`-safety follow-up** — audited the remaining
  `cc.newXxx(name)` follow-up noted in the backlog. User-derived register names
  are already routed through `"%s"` call sites or `DataDef::newreg()`;
  leftover named temporaries in `typesafe.cpp` and lambda paths are fixed
  literals, so no further code change was required.

### Fixed

- **Assignment as expression inside declaration initializers** — `int y = (x
  = 42);` and related forms now preserve the outer declaration assignment
  while still allowing nested assignment expressions on the RHS. The parser
  keeps the original assignment-context parse and only wraps the initializer
  when it does not already assign to the declared variable. Brace-init paths
  are unchanged. `tests/testdeclassignexpr.mad` covers nested assignment,
  expression composition, and comma declarations.

- **Typed `for` init with comma-separated declarations** — `for (int i = 0,
  j = 10; ...)` now declares all variables correctly. The parser reuses
  `parseDeclaration()` for the typed `for` initializer and routes any
  synthetic comma-continuation declarations into `TokenFOR::init_extras`,
  matching the existing compile-time execution path. `tests/testfortypedcomma.mad`
  covers scalar, three-variable, and mixed pointer/scalar cases.

- **Compiler raw-string diagnostics now carry source context** — top-level
  `Program::compile()` catches for raw `throw "..."` failures now anchor
  messages to the current statement token (and current pre-pass token during
  FuncNode setup) and show source context, replacing location-less compiler
  error lines.

- **C `_Bool` keyword alias** — `_Bool` is now registered as a datatype token
  alias for `ddBOOL`, so C-style boolean declarations and fixed arrays parse
  and compile the same as `bool`.

- **Binary integer literals** — the lexer now accepts `0b...` and `0B...`
  integer literals and emits them as `TokenInt` values alongside the
  existing decimal and hexadecimal literal paths.

- **`restrict` parsed as a no-op qualifier** — the parser now accepts
  `restrict` in declaration and function-parameter pointer declarators and
  ignores it semantically, matching the current compatibility-only handling.

- **Embedded `<sys/file.h>`** — added `LOCK_SH`, `LOCK_EX`, `LOCK_NB`,
  `LOCK_UN`, and `flock()` availability through the existing dlsym fallback,
  closing the last header gap called out in `docs/SMAUG_requirements.md`.

- **`#include` now behaves include-once by default** — the lexer records
  resolved local paths and embedded-header keys, then skips repeated
  includes within the same compile. This matches madc's single-unit build
  model and reduces duplicate header tokenization for SMAUG bootstrap files.

### Added

- **Function pointer typedefs** — `typedef void DO_FUN(CHAR_DATA *ch, char
  *argument);` and `typedef int (*UNOP)(int);` (SMAUG-style and classic C
  forms). Both produce a `DataDefFPTR` registered in `datatype_map`.
  Declarations like `DO_FUN *cmd;` and `UNOP u;` yield function-pointer
  variables; call through with `cmd(args)`. The `*` decorator on Form 1
  (`DO_FUN *cmd`) is accepted as a no-op since the typedef already names a
  function-pointer storage. New helper `Program::parseFnPtrParams` reads a
  parameter list (types only, optional names discarded) and builds a
  `FuncDef` to wrap in the `DataDefFPTR`. `tests/testfnptrtypedef.mad` covers
  both forms, reassignment, and invocation with `cout <<`.

- **SMAUG command-table pattern** — `struct cmd { char *name; DO_FUN *fn; };`
  followed by `struct cmd c = { "who", do_who };` now compiles and runs.
  Both dispatch forms work: intermediate variable (`DO_FUN *fp = c.fn;
  fp(args);`) and direct invocation (`c.fn(args);`).
  `tests/testfnptrstruct.mad` covers the intermediate pattern;
  `tests/testfnptrmember.mad` covers direct invocation.

- **Direct struct-member function-pointer invocation** — `c.fn(args)` and
  `o.fn(a, b)` now parse and run. The parser detects the `(` following a
  `TokenMember` whose datadef is `DataDefFPTR` and builds a `TokenCallFunc`
  whose new `src_node` field points to the member. At compile time the
  fptr-call path compiles `src_node` to materialise the function-pointer
  value, instead of looking it up from a variable. Also fixes struct-body
  parsing so `DO_FUN *fn;` inside a struct stays a `DataDefFPTR` member
  (previously got wrapped in `DataDefPTR(DataDefFPTR)`, which defeated
  fptr dispatch — the parseDeclaration skip for fnptr-base needed the
  same treatment at the struct-body level).

- **Global function-pointer initialization + SMAUG command tables at file
  scope** — `DO_FUN *g = do_who;` and `struct cmd tab[] = { {"who", do_who},
  ... };` at file scope now compile and run. Two-part fix:
  - `Variable` constructor now allocates storage for `DataDefFPTR` globals
    (size 8). Previously ALL `btFunct` types were skipped, but DataDefFPTR
    represents a pointer SLOT, not a function definition — it needs storage.
  - New pre-pass in `Program::compile` creates a FuncNode label for every
    user function and lambda *before* the globals compile, so LEA at global
    init time resolves correctly. Factored `TokenFunc::prepareFuncNode` out
    of `TokenFunc::compile` to do this idempotently. A new `pending_funcs`
    vector on `Program` lists user functions in source order; parser pushes
    to it alongside the existing `ast.push()`.
  - Excluded `DataDefFPTR` variables from `_compiler_finalize`'s x86code-
    backfill loop so its 8-byte slot isn't mistakenly cast to `Method *`.
  `tests/testfnptrglobal.mad` covers the end-to-end pattern: plain global
  fn-ptr + dispatch loop against a file-scope command table + direct
  indexed invocation.

- **`struct sockaddr_in` + `struct sockaddr` + `struct in_addr` interop** —
  glibc x86-64 layouts for socket programming, embedded in
  `<netinet/in.h>` (sockaddr_in 16 bytes, in_addr 4 bytes) and
  `<sys/socket.h>` (sockaddr 16-byte generic base used for the
  `bind()`/`connect()` cast trick). Plus `sa_family_t`, `in_port_t`,
  `in_addr_t`, `socklen_t` type aliases. All socket functions (socket,
  bind, connect, listen, accept, htons, ntohl, etc.) resolve via dlsym.
  `tests/testsockaddr.mad` binds a TCP loopback socket end-to-end.

- **`struct dirent` interop** — 280-byte glibc layout in `<dirent.h>`.
  `opendir()` / `readdir()` / `closedir()` via dlsym fallback.
  `tests/testdirent.mad` iterates `tests/` and classifies entries.

- **Fixed-size array members in struct bodies** — `char buf[N];`,
  `char sa_data[14];`, `int m[N][M];` inside a struct definition now
  parse. The struct-body loop peeks for `[dim]` after the identifier,
  multiplies dimensions, and passes the product as `count` to
  `DataDefSTRUCT::addMember`. The member reserves `count * sizeof(base)`
  bytes inline; `&obj.member` yields a pointer to the buffer start.
  Needed for `struct dirent::d_name[256]` and broadly for SMAUG's
  many fixed char buffers.

- **Unary `&` / `*` immediately following a cast** — `(struct sockaddr *)
  &addr`, `(int *)*ptr` etc. now parse. Previously the cast's closing
  `)` leaked through `isUnaryPosition` as a value-returning token, so
  the next `&` / `*` mis-parsed as binary AND / multiplication and
  threw "Missing operand". The cast block now nulls `_prv_token`
  between consuming its `)` and calling the nested `parseExpression`,
  so unary operators at the head of the cast body see a unary context.

- **`struct stat` + `struct timespec` interop** — `<sys/stat.h>` now embeds
  the full glibc x86-64 layout (144 bytes) of `struct stat` and the
  supporting `struct timespec` (16 bytes). Includes:
  - All type aliases: `mode_t`, `uid_t`, `gid_t`, `dev_t`, `ino_t`,
    `nlink_t`, `off_t`, `blksize_t`, `blkcnt_t`.
  - File-type predicate macros: `S_ISREG`, `S_ISDIR`, `S_ISLNK`,
    `S_ISBLK`, `S_ISCHR`, `S_ISFIFO`, `S_ISSOCK`.
  - Legacy field aliases via macro: `st_atime` → `st_atim.tv_sec` etc.,
    matching glibc.
  `stat()` / `fstat()` / `lstat()` / `chmod()` / `mkdir()` / `mkfifo()`
  resolve via the existing dlsym fallback. `tests/teststat.mad` drives
  real `stat()` on a regular file, a directory, a missing path, and
  checks `st_mtime > 0`.

- **Reassigning a struct's function-pointer member** — `c.fn = other_fn;`
  after init. `TokenVar::compile` for a function identifier assumed any
  `regdp.first` destination was a Gp register — true for variable
  assignments, but wrong for struct-member LHS where the destination is a
  Mem operand. Now LEAs the function address into a tmp Gp and stores to
  the caller's Mem when `regdp.first->isMem()`. `tests/testfnptrreassign.mad`
  covers member reassignment and fn-ptr-variable reassignment paths.

### Fixed

- **Assignment as an expression in enclosing context** — `while ((entry =
  readdir(d)) != NULL)`, `if ((n = get()) > 0)`, and `y = (x = 42)` now
  evaluate the inner assignment and propagate the assigned value to the
  enclosing expression (C `operator=` semantics). `TokenAssign::compile`'s
  numeric path previously wrote the RHS into the LHS storage, then
  restored the caller's original `regdp.first` and returned it — but
  without ever copying the assigned value there, so the enclosing
  comparison / assignment saw an uninitialised Gp. Now, when the
  caller-provided destination is distinct from the LHS's own `_operand`,
  mirror `_operand` into it via `safemov` before returning. Unlocks
  the standard readdir / accept / recv assign-in-condition SMAUG idioms.
  `tests/testassigninexpr.mad` covers while-condition, if-condition,
  chained `y = (x = ...)`, and paired `if ((p = ...) == (q = ...))`.

  Known limitation: `int y = (x = 42);` at declaration-init time still
  gives `y == 0`. The parser only wires the inner `TokenAssign(x, 42)`
  as the declaration's initializer and drops the outer y-assign wrapper.
  Workaround: use `int y = 0; y = (x = 42);` instead.

- **Function-to-pointer decay for value contexts** — `fptr = func_name;`,
  `struct X x = { "name", func_name };`, `call(a, func_name, b);`, `cond ?
  f1 : f2` — anywhere a bare function identifier appears as a value — now
  pushes the function's address instead of mis-compiling as a no-arg call.
  Decay triggers when the function identifier isn't followed by `(` and
  either (a) top of opStack is an assignment op (`=`, `+=`, `-=`, `*=`,
  `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) or (b) the follower token
  is a value-end (`,`, `}`, `)`, `]`, `:`). `cout << endl;` keeps the
  pre-existing behavior because `endl;` has neither, so BSL's special
  handling of ostream-consuming no-arg functions still applies.

- **`char*` coercion in function-pointer indirect calls** —
  `TokenCallFunc::compile`'s fptr path (the `is_function() && is_numeric()`
  branch) now runs the same `dtSTRING -> dtCHARptr` coercion via
  `string_cstr` that the direct-call path uses. Previously, passing a
  string literal to a typedef-declared `void (*)(char *)` function pointer
  would pass the `std::string*` pointer verbatim, so the callee received
  the string object header instead of the null-terminated bytes.

## [v0.9.0] — 2026-04-17 — SMAUG Phase F continues (session 1)

MadSMAUG's `hashstr.mad` now compiles AND runs correctly end-to-end
(`bin/madc MadSMAUG/src/SMAUG.mad` → expected link-count hash stats with
no runtime errors). Every language gap exposed while running the file
was fixed in madc proper. 81 integration + 25 unit tests pass.

### Added

- **Inc/dec on struct members** (e8c3f0b) — `++ptr->links`, `ptr->field--`
  etc. via `TokenInc` / `TokenDec` now support `ttMember` (via `->` and
  `.`) and `*deref` targets, sharing the load-op-store pattern with the
  compound-assignment operators through `resolveCompoundLHS`. Previously
  threw "Increment on a non-variable rval" on anything other than a
  plain variable.

- **For-loop compound-comma increment with postfix inc** (e8c3f0b) — the
  classic SMAUG `for (ptr = head, c = 0; ptr; ptr = ptr->next, c++)`
  pattern now compiles and runs; the postfix-inc-in-incrementer path
  was the same underlying gap as the member-inc one.

- **Post-declaration `char *p; p = "literal";`** (acfc8b1) — and `ptr->
  name = "alice";` — TokenAssign detects char* ← dtSTRING and routes
  through `string_cstr` to pull the literal's data pointer. Before, the
  RHS's `std::string` operand was written verbatim into p, so reads
  dereferenced the string object header and printed garbage.

- **Unsigned comparison operators** (e8c3f0b) — `TokenLT` / `TokenLE` /
  `TokenGT` / `TokenGE` pick `setb` / `setbe` / `seta` / `setae` when
  either operand is unsigned, vs the signed `setl` / `setle` / `setg` /
  `setge` previously used always. Without this, `if (ptr->links <
  65535) ++ptr->links;` with `unsigned short int` jumped over the
  increment because 65535 sign-interprets as -1. New `safesetb` /
  `safesetbe` / `safeseta` / `safesetae` helpers in `typesafe.cpp`.

### Fixed

- **Global pointer variable read/write** (e8c3f0b) — `DataDefPTR` now
  overrides `movrval2mptr` / `movrval2rptr` / `movint2rptr` /
  `movmptr2rval` with explicit qword semantics. The base-class switch
  on `DataType` fell through to the unhandled default for `rtPtr()`
  values (>= 10000), so `global_ptr = x;` silently dropped the store.
  Every global pointer variable read would return the slot's stale
  initial memory rather than the stored value.

- **Subscript → member assign** (e8c3f0b) — `p->next = arr[i];` now
  writes. `TokenSubscript::compile` respects a caller-supplied `Mem`
  destination (the struct member's Mem) by loading into a temp and
  storing; previously it overwrote `regdp.first` with its own fresh Gp
  and abandoned the member's Mem, making the assignment a no-op.

- **Sub-qword sign/zero-extension in `resolveCompoundLHS`** (e8c3f0b) —
  loading a word-sized member into a Gp64 for arithmetic now uses
  `movzx` (unsigned) / `movsx` (signed) / `movsxd` / `mov r32,m32`.
  Plain `cc.mov(gpq, word_ptr)` is not a valid x86 encoding — asmjit
  silently emitted a truncated op or dropped it, leaving the upper bits
  dirty for subsequent arithmetic.

- **`safemov(Mem, Gp)` size mismatch** (e8c3f0b) — now picks the `r8` /
  `r16` / `r32` / `r64` view based on the Mem's size, not the Gp's.
  Without this, writing a Gp64 (the widened member_lhs register) to a
  word-sized member emitted `mov word ptr, r64` which asmjit rejects,
  silently dropping the store.

- **Parser: comma peek-stop in conditional mode** (e8c3f0b) — a nested
  `parseExpression` called from the cast-body handler used to consume
  the `,` that terminates the outer function-call argument. So
  `strcpy((char *)h + 8, "x")` parsed as a one-arg call with `"x"`
  silently merged into the first arg's expression tree. Fixed by adding
  comma to the peek-stop set alongside `;`.

- **`TokenIF::compile` regdp reset** (e8c3f0b) — now zeros regdp before
  the condition, then branch, and else branch, matching what
  `TokenFOR` / `TokenWHILE` / `TokenDO` already do (per
  `.claude/rules/regdp-reset.md`).

### Tests

- `testincmember.mad` — prefix/postfix inc/dec on struct members
- `testunsignedcmp.mad` — unsigned comparisons inside if
- `testglobalptr.mad` — global pointer var read/assign
- `testsubtomember.mad` — `p->next = arr[i]` for NULL and non-NULL
- `testcastargcomma.mad` — cast+arith as call arg with commas
- `testcommaincrement.mad` — SMAUG's `for (...; ptr = ptr->next, c++)`
- `testpostdeclstr.mad` — `char *p; p = "literal";` and via struct member

## [v0.8.0] — 2026-04-17 — SMAUG Phase E Complete + Phase F Start

SMAUG Phase E finishes (C arrays, brace initializers, struct interop with `struct tm`/`timeval`/`fd_set`, end-to-end `select()`) and Phase F begins with the first `.c → .mad` port (`MadSMAUG/src/hashstr.mad`). Every language gap surfaced during the port has been fixed in madc proper: self-referencing structs, three-word compound types, multi-var decls, global fixed arrays, `stdin`/`stdout`/`stderr`, for-loop comma expressions, forward decl + definition, `__FILE__`/`__LINE__`, raw-pointer `ptr[i]` subscript, and more.

### Added — SMAUG 1.8 Source Port Begins (2026-04-17)

First .c → .mad port: `MadSMAUG/src/hashstr.mad` (copied from SMAUG 1.8's
`hashstr.c`). The bootstrap convention is an app-named top-level file
(`SMAUG.mad`) that `#include`s the ported sources in dependency order with
`main()` last; `bin/madc SMAUG.mad` compiles the whole tree.

Each language gap surfaced during the port was fixed in madc proper:

- **Self-referencing structs** (54087c2) — `struct X { struct X *next; ... };`
  works. `TokenSTRUCT::parse` pre-registers the tag in `struct_map` with an
  incomplete placeholder before entering the body-parsing loop, so field types
  like `struct X *` resolve the in-progress struct.

- **Three-word compound types** (54087c2) — `unsigned short int`, `signed long
  int`, `long long int`, `unsigned long long`, `signed long long` all
  produce the correct DataDef. Lexer reads up to two lookahead words and
  picks the longest match.

- **`void` as sole parameter** (54087c2) — `int f(void)` parses as zero-arg.

- **Global fixed-size arrays** (35c6bf0) — `struct X *arr[N]` at file scope
  now works. parseDeclaration allocates a `calloc`'d buffer when the decl is
  global/static; voperand loads the absolute address into the base-pointer Gp
  instead of stack-allocating. The cache-hit path skips `movreg` for fixed
  arrays (the pointer is a compile-time constant).

- **Multi-variable declarations** (35c6bf0) — `int a, b, c;` / `char *p, *q;` /
  `int x = 1, y = 2;`. After the first decl the parser pushes back a clone of
  the base type token so parseCompound naturally iterates and parseDeclaration
  recurses. Base type is preserved alongside the decorated decl_type so each
  var in `char *p, *q` gets its own independent pointer depth.

- **stdin / stdout / stderr** (35c6bf0) — lazy-registered in `<stdio.h>` as
  int64 globals whose backing slot holds `*dlsym("stderr")` etc. Reading
  `stderr` in madc loads libc's current FILE\* value.

- **`register` before struct / typedef** (35c6bf0) — `register struct X *p;`
  now compiles. Previously only primitive types after register were allowed.

- **Unary minus after a keyword** (35c6bf0) — `return -1;`, `if (-x > 0)` etc.
  `isPostfixPosition` / `isUnaryPosition` now treat keywords as unary-opening
  contexts instead of value-producing ones; the Neg→Sub conversion no longer
  misfires.

- **TokenRETURN multi-return detection tightened** (35c6bf0) — previously any
  non-`;` / non-symbol peek after parseExpression triggered the multi-return
  path, so `return X;` followed by `if (...)` / `<ident>()` / etc. misfired
  as multi. Keywords and type names are now also excluded.

- **For-loop comma expressions** (be6c359) — `for (a=0, b=1; cond; i++, j--)`
  parses and runs. `TokenFOR` gains `init_extras` / `incr_extras` vectors;
  the parser uses conditional `parseExpression` for init/cond/incr so `;`
  stays in the stream to gate the extras loops. Compile runs all extras in
  order after the main init and before the jmp-back-to-top.

- **Forward decl + definition param mismatch** (be6c359) — the definition
  pass was re-pushing every DataDef onto `func->parameters` (because
  `FuncDef::findParameter` compares against the DataDef name, not the param
  name), causing the ids[]-vs-parameters[] binding loop to overshoot. Track
  `func_already_declared` and skip the re-push on the 2nd pass.

- **Compile-time error handler robustness** (be6c359) — the
  `catch(const char*)` block crashed on NULL/dangling pointers in the
  exception value (ostream `<<` of NULL), masking the real compile error.
  Guarded.

### Added — Phase E Finish (2026-04-17)

- **`__FILE__` / `__LINE__`** (9e2d5ad) — lexer injects a quoted filename
  and current `source.line()` as a decimal integer. Works correctly inside
  `#define` bodies — each invocation captures the call site.

- **`cc.newXxx(name)` format-safety sweep** (c50acbe) — 32 direct call
  sites in compiler.cpp that passed user- or literal-derived names
  verbatim to asmjit's variadic register-naming API are now routed through
  a `"%s"` format. Variable names containing `%` (our `__literal__tab[%ld]
  = (%s, %ld)` style) previously crashed on the unmatched format spec.
  `DataDef::newreg` was fixed in an earlier commit; this extends the same
  pattern to the rest.

- **Struct interop for libc types + fd_set / select()** (2f08efd) —
  `struct tm` (56 bytes), `struct timeval` (16 bytes), `struct fd_set`
  (128 bytes) with glibc-x86-64-matching layouts. `FD_ZERO` / `FD_SET` /
  `FD_CLR` / `FD_ISSET` forward to `__madc_fd_*` helpers bundled into the
  madc binary (reachable via dlsym thanks to -rdynamic). `select()` works
  end-to-end with a real pipe.
  - Fixed latent `safemov(Gp, Mem)` bug: it used `cc.mov(r1, r2)` for all
    sizes, and asmjit resolves that by reading a full qword even when the
    Mem is 4/2/1 bytes. Reading an int32 struct member (e.g. `tm->
    tm_hour`) pulled 8 bytes starting at the field's offset, returning the
    adjacent field packed into the upper half. Now picks `movsxd` / `mov
    r32,mem` (implicit zero-extend) / `movsx` / `movzx` based on sizes.
  - Extended unary `&` to accept `&(name)` in addition to `&name`, so the
    macro-expanded `__madc_fd_set(fd, &(set))` parses.

- **Raw-pointer subscript `ptr[i]`** (189f4ae) — for `int *`, `char *`,
  `int32_t *`, etc. Computes `[ptr + i*sizeof(base)]` with SIB scaling by
  the pointed-to type's size. Unblocks C interop like `pipe(pfd); int rfd
  = pfd[0];` / `FD_SET(rfd, rfds);` / `select(rfd+1, &rfds, ...)`. Also
  fixed `safemov(Operand, Operand)` Gp←Mem path to forward to the size-
  aware typed overload instead of calling `cc.mov` directly.
  - Added `scripts/psed.sh` — python-backed literal-text patcher for
    multi-line edits where tab-sensitive Edit calls are brittle.

- **Multi-file project convention** (ac0cf4f) — README documents the
  app-named bootstrap file (e.g. `smaug.mad`) that `#include`s its sources
  in dependency order with `main()` last. No new tooling — the existing
  `#include` already resolves relative paths and handles nested includes.

### Added — C Arrays and Structs (SMAUG Phase E)

- **Chained `->` and `.` member access** (b3a9d9a) — `a->b->c`, `a->b.c`, `a.b.c` all
  compile. TokenMember's parent_expr path resolves intermediate pointers and struct
  addresses at codegen time; the dot handler now accepts TokenMember LHS in addition
  to TokenVar.

- **C fixed-size arrays — 1D** (fd98935) — `int arr[N]` allocates a stack slot and
  stores the LEA'd base pointer as the variable's operand. Subscript emits
  `[base + idx*elem_size]` via SIB. `TokenSubscript::compile` honors `regdp.first`
  when the caller supplies a destination register, so `int x = arr[i]` works as RHS.
  Supports int, int32_t, int16_t, char element types, plus char-array decay to
  `char *` for `printf "%s"`.

- **Multi-dimensional arrays** (26dca0e) — `int m[N][M]`, `int cube[N][M][K]`.
  `TokenSubscript` gains an `extra_indices` vector; chained `[i][j][k]` folds into
  a single linear offset `((i0*d1)+i1)*d2 + i2 + ...`.

- **Brace initializer lists for arrays** (a1774d0) — `int a[N] = { v0, v1, ... };`
  with explicit size, inferred size (`int a[] = {1,2,3}`), partial (rest zero-filled),
  and arbitrary expressions as values. Includes a parseExpression fix: the outer
  loop was consuming a trailing `}` past the final element; now treats `}` like `]`
  and breaks without consuming.

- **String-literal char-array init** (1bae4f4) — `char msg[] = "hello"` expands to
  a per-byte initializer list plus a null terminator (length = strlen + 1 for the
  inferred form; zero-padded for oversized explicit `char buf[20] = "hi"`).

- **`char *msg = "literal"`** (13bbc35) — routed through the same path as
  `char msg[] = "literal"` so the two forms produce identical internal storage
  (`ddCHAR` + `vfFIXEDARRAY` + inferred `dims`).

- **Struct initializer lists** (8df2dca) — `struct Foo x = { ... };` with scalars,
  pointers, char* (string_cstr coerces `std::string` literal → `const char *`), and
  `std::string` members (`string_assign` after the auto-construct in voperand).
  Partial inits zero-fill remaining numeric/pointer members.

- **Array-of-structs initializer** (e62d2e7) — `struct Entry tab[] = { {"a", 1},
  {"b", 2}, ... };`. New `TokenStructLit` AST node carries nested brace lists as
  array elements; `emit_struct_init` factored into a shared helper invoked at
  `base + i*struct_size`. `TokenSubscript` now returns a Gp pointer (LEA) when
  indexing into a struct-element array — no load — so `tab[i].name` reaches the
  member via `TokenMember`'s Gp-base dot-chain path (parser also accepts
  `TokenSubscript` as the LHS of `.`).

### Added — Ergonomics

- **Crash handler with backtrace** (308b622) — `SIGSEGV`, `SIGABRT`, `SIGFPE`,
  `SIGBUS`, `SIGILL` caught at startup. Writes signal name, fault address (for
  SEGV/BUS), and a `backtrace_symbols_fd` trace to stderr, then restores the
  default handler and re-raises so the shell sees the real exit status and core
  dumps still drop.

- **`str.length()` / `str.size()` methods** (f04b7b6) — `std::string` exposes its
  length via instance methods that wrap `madc_string_length`.

### Changed

- **Removed builtin `strlen`** (f04b7b6) — the pre-registered madc wrapper
  expected a `std::string *` argument and misfired on char arrays/pointers.
  `strlen(char *)` now resolves via dlsym fallback to libc, which is the natural
  type fit. A `madc::`-namespaced type-aware alternative could be added later if
  useful.

### Fixed

- **`DataDef::newreg` format-string safety** (e62d2e7) — asmjit's
  `newGpq/newXmm/newIntPtr` are variadic printf-style: they interpret the first
  `const char *` argument as a format. Variable names containing `%` (e.g. our
  `__literal__tab[%ld] = (%s, %ld)` string-literal variable names) crashed on the
  unmatched format spec, dereferencing garbage as a pointer. Fixed by passing
  `"%s"` as the format and the name as the argument. (Other direct callers of
  `cc.newIntPtr(name)` likely share the same latent bug; sweep is on TODO.)

- **`emit_struct_init` aliasing corruption** (e62d2e7) — the `std::string`→
  `const char *` coercion was reassigning through the `Operand &` returned by
  `inits[i]->compile(...)`. For global literal variables that reference aliases
  the cached entry in `operand_map`; subsequent uses of the same literal saw the
  already-coerced `char *` and ran string_cstr over it again, interpreting the
  char pointer as a `std::string *` and reading SSO bytes as a new "pointer"
  (e.g. `"alice"` becoming `0x6563696c61` → SEGV in printf). Fixed by using a
  local `Operand` for the effective value.

## [v0.7.0] — 2026-04-16 — SMAUG Phase D: va_list + For-Loop Fix

### Added — SMAUG Phase D: Variadic Functions

- **`va_list` / `<stdarg.h>` support** — Variadic functions with `...` syntax. Hidden
  `__va_args` parameter carries a packed `int64_t[]` buffer from call site to callee.
  `va_start` macro sets the pointer, `va_arg` is a compiler intrinsic that reads and
  advances, `va_end` is a no-op macro. Design avoids the System V `va_list` struct
  (impossible in asmjit Compiler mode due to virtual registers).

- **`vsprintf`/`vsnprintf`/`vfprintf` helpers** — Format-string-aware C functions
  (`__madc_vsprintf` etc.) compiled into the binary. Parse `%d`/`%s`/`%f`/etc. from
  the format string and call `sprintf` per-specifier with args from the packed buffer.
  Redirected via `#define vsprintf __madc_vsprintf` in embedded `<stdarg.h>`.

- **`-rdynamic` linker flag** — Exports binary symbols for `dlsym(RTLD_DEFAULT)`
  visibility, enabling JIT code to call built-in C helpers like `__madc_vsprintf`.

- **39 embedded headers** — Added `<stdarg.h>` (was 38).

### Fixed

- **For-loop increment parsing bug** — `for ( i = 0; i < N; i++ )` now works. All
  increment forms (`i++`, `i--`, `++i`, `--i`, `--c`) parse correctly. Root cause: the
  conditional peek-stop in `parseExpression` left the `;` separator in the token stream,
  so `TokenFOR::parse()` was passing `;` to `parseStatement` instead of the increment
  expression. Fixed by consuming the `;` separator explicitly before calling `parseStatement`.

## [v0.6.0] — 2026-04-16 — SMAUG Phase A/B/C: C Pointer System + Macros

### Added — C Pointer and Type System (Phase A — all 5 items complete)

- **`char *` pointer declarations** — `DataDefPTR` class tracks pointed-to type. Pointer
  handling in `parseDeclaration`, struct/class members, function parameters, and `sizeof()`.
  Supports `char *`, `int *`, `void *`, `struct *`, `char **` (double pointers).

- **`->` struct pointer member access** — `ptr->member` parsed in `parseExpression`,
  resolved via `DataDefPTR::base_type`. Reuses `TokenMember` Gp codegen path
  (`[gp + offset]`). Chained `->` supported (with temp variable for intermediate).

- **`(TYPE *)` cast expressions** — Detects casts by checking if `(` is followed by a
  type name. `TokenCast::compile()` passes through the value with target type annotation.
  Works with `(CHAR_DATA *)calloc(...)`, `(struct tag *)ptr`, `(int *)raw`.

- **`&` address-of operator** — `TokenAddrOf` emits LEA for stack variables. Unary
  detection via `isUnaryPosition()` helper. Works for struct variables passed to functions.

- **Forward typedef struct declarations** — `typedef struct tag_name ALIAS;` before the
  struct body exists. Creates placeholder `DataDefSTRUCT` (size 0), filled in-place when
  the full definition is encountered. The typedef alias automatically sees the completed type.

### Added — Macros (Phase B — all 3 items complete)

- **Function-like macros** — `#define NAME(params) body` with parameter substitution.
  Whole-word matching prevents substring replacement. Handles nested parens, string literals
  in arguments, and zero-parameter macros.

- **Multi-line `#define` with `\` continuation** — Both function-like and object-like macros
  support backslash line continuation.

- **`do { } while(0)` macro bodies** — Fixed do-while crash when multiple loops in sequence
  (regdp reset + TokenInt with NULL regdp.first). SMAUG's CREATE/DISPOSE macros now work.

### Added — Data Structures and Types (Phase C partial)

- **`unsigned`/`signed`/`long`/`short` compound type keywords** — Lexer handles compound
  specifiers: `unsigned char` → dtUINT8, `unsigned int` → dtUINT32, `long int` → dtINT64,
  `short int` → dtINT16, bare `unsigned` → dtUINT32, etc.

- **`enum` keyword** — `enum { NAME, NAME = val, ... }` with auto-incrementing values and
  explicit `= N` assignments. Each enumerator registered as a global constant variable.

- **`typedef` for primitive types** — `typedef int sh_int;`, `typedef unsigned char bool;`,
  `typedef char *LPSTR;`. Alias names can redefine existing type names.

- **`static` keyword** — Static local variables persist across function calls. Heap-allocated
  with `vfSTATIC` flag. Runtime initialization skipped (pre-initialized via allocation).

- **`const` keyword** — Consumed and passed through to type declaration.

- **`extern` keyword** — Consumed and skipped to semicolon.

- **`*ptr` dereference operator** — Read and write through pointer. `TokenDeref` returns
  Mem operand `[ptr_gp]` for numeric types. Works with heap pointers (calloc/malloc).

### Added — Infrastructure

- **`struct Type` in function parameters** — `parseFunction()` handles `struct Name` and
  typedef'd identifiers as parameter types (was only accepting `ttDataType` tokens).

- **Typedef'd types in struct member definitions** — `ROOM_DATA *in_room;` inside a struct
  body now works (identifier resolved against `datatype_map`).

- **`isUnaryPosition()` / `isPostfixPosition()` helpers** — Replaces duplicated prevToken
  checks for unary `&`, `*`, prefix/postfix `++`/`--`, and Neg→Sub conversion.

- **`resolveCompoundLHS()` helper** — All 10 compound assignment operators (`+=`, `|=`, etc.)
  now work on struct members via `->`, not just plain variables. Load-op-store pattern for
  Mem operands.

- **`make fulltest` target** — Runs unit tests + all integration tests in one command.

### Fixed

- **Ternary inside parentheses** — `int m = (a > b ? a : b)` now works. Was setting
  `done=true` unconditionally after ternary, preventing closing `)` from being consumed.
  Fix: only set `done` when `brackets == 0`.

- **Variadic argument promotion** — Sub-64-bit integer types (char, short, int32) now
  sign/zero-extended to 64-bit before passing to variadic functions (printf, etc.).

- **C string function redirect** — When a registered built-in (e.g. `strlen`) expects
  `std::string` but receives a `char *` pointer argument, redirects the call to the C
  library version via dlsym.

- **dlsym return to Mem operand** — Function returns assigned directly to `->` members
  no longer crash. Uses temp Gp register for `setRet` then writes to Mem.

- **Mem-to-Gp for variadic args** — Struct member access via `->` in variadic function
  arguments (printf) now loads Mem into temp Gp before passing.

- **Do-while regdp reset** — `TokenDO` and `TokenWHILE` compile() now reset regdp before
  body and condition sub-compilations. Also fixed `TokenInt::compile()` with NULL regdp.first.

---

## [Unreleased] — SMAUG Goal + Full POSIX Header Coverage (2026-04-16)

### Added — 31 Additional Embedded POSIX/libc Headers (c971eb1, ea06f5a)

38 embedded headers total (up from 3). All standard POSIX/libc headers madc programs
are likely to use are now covered. Each header provides constants via `#define` and
type aliases via `#define`; functions are available via the existing dlsym fallback.

**Batch 1** (c971eb1) — `<stdlib.h>`, `<string.h>`, `<limits.h>`, `<errno.h>`, `<fcntl.h>`,
`<signal.h>`, `<unistd.h>`, `<time.h>`, `<dirent.h>`, `<sys/wait.h>`, `<sys/stat.h>`

**Batch 2** (ea06f5a) — `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`,
`<sys/un.h>`, `<poll.h>`, `<sys/select.h>`, `<sys/time.h>`, `<sys/mman.h>`, `<sys/ipc.h>`,
`<sys/shm.h>`, `<sys/resource.h>`, `<sys/types.h>`, `<pthread.h>`, `<termios.h>`,
`<syslog.h>`, `<dlfcn.h>`, `<ctype.h>`, `<stdint.h>`, `<locale.h>`, `<glob.h>`,
`<fnmatch.h>`, `<pwd.h>`, `<grp.h>`

- `gen_embedded_headers.sh` updated to use `find` + relative paths for subdirectory support
  (`sys/wait.h`, `sys/stat.h`, `netinet/in.h`, etc. all key correctly)
- Type aliases (`pid_t`, `time_t`, `size_t`, etc.) implemented via `#define` — substituted
  through the existing lexer pushback mechanism; no lazy registration needed

### Added — SMAUG 1.8 Compatibility Goal (bd9844c)

- **Long-term goal established:** run SMAUG 1.8 MUD (~158k LOC, C89) in madc without gcc
- **`smaug.tgz`** checked into repo root (official 1.8 tarball)
- **`docs/SMAUG_requirements.md`** — full gap analysis: system headers checklist (37/38
  embedded; only `<stdarg.h>` missing), language feature gap table with BLOCKER/HIGH/MEDIUM/LOW
  severity, 5-phase implementation roadmap (A: pointer/type system, B: macros, C: data
  structures, D: I/O infrastructure, E: full SMAUG boot)
- Top blockers identified: function-like macros, `char *` declarations, `->` operator,
  `(TYPE *)` casts, `&` address-of — all Phase A/B work

### Fixed — Integer Literal Storage (c971eb1)

- **`int` overflow in lexer decimal parser** — accumulator variable was `int`; values ≥ 2^31
  (e.g. `2147483648` from `#define INT_MIN -2147483648`) silently wrapped to negative.
  Fixed: widened accumulator to `int64_t`.

- **`_token` field overflow in `TokenBase`** — `int _token` truncated stored literal values
  for constants ≥ 2^31. Fixed: widened to `int64_t` throughout `TokenBase`, `TokenInt`,
  `TokenVar`; all `get()`/`set()` signatures updated to `int64_t`.

- **`safeneg` sign-extension truncation** — `cc.neg()` was followed by
  `cc.movsx(op, op.r8())`, which sign-extended from `al` (8 bits) back to 64 bits.
  This corrupted any negated value with absolute magnitude ≥ 128 (e.g. `-INT_MIN` → 1,
  `-200` → 56). Fixed: removed the `movsx` entirely — `cc.neg()` on the full-width GP
  register is correct and sufficient.

---

## [Unreleased] — Phase 4 Prep (2026-04-15 → 2026-04-16)

### Added — Standard C Infrastructure

- **Embedded header system** — `#include <name>` checks headers baked into the binary (via
  `scripts/gen_embedded_headers.sh` at build time) before filesystem. `include/madc/` contains
  the source headers. Three implemented: `<iostream>`, `<math.h>`, `<stdio.h>`.

- **`#include <iostream>`** — `cout`, `cin`, `cerr`, `endl` now require this include (matching
  C++ convention). Uses lazy registration — symbols created on first use, not at parse init.

- **`#include <math.h>`** — Auto-loads libm via `#load "libm.so.6"`. Defines `M_PI`, `M_E`,
  `M_SQRT2`, `M_SQRT1_2`, `INFINITY`, `HUGE_VAL`. Math functions (`sqrt`, `sin`, `cos`, `pow`,
  `floor`, `ceil`, `fabs`, `log`) available via dlsym fallback.

- **`#include <stdio.h>`** — Defines `EOF`, `SEEK_SET`/`CUR`/`END`, `BUFSIZ`, `NULL`. `printf`,
  `sprintf`, `snprintf` available via dlsym fallback.

- **dlsym fallback** — Unresolved function calls followed by `(` try `dlsym(RTLD_DEFAULT, name)`
  before throwing "undeclared identifier". Works for all libc functions: `getpid()`, `sleep()`,
  `abs()`, `strlen()`, etc. No `#include` or `#load` needed for basic libc.

- **Variadic dlsym call path** — Dedicated compile path for dlsym-resolved functions. Builds
  `FuncSignature` from actual argument types (int, double, string→cstr). Infers double return
  type from destination register or argument types. Supports `sqrt(4.0)`, `pow(2.0, 10.0)`.

- **C preprocessor directives** — `#define NAME value` (constant substitution via pushback
  re-tokenization), `#undef NAME`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`,
  `#if defined(X)`, `#if !defined(X)`, `#if 0`/`#if 1`. Nested conditionals handled correctly.

- **`#pragma pack(push, N)` / `#pragma pack(pop)`** — Controls struct field alignment. Maintains
  a stack of pack values in the lexer.

- **C ABI struct alignment** — Structs now use natural x86-64 alignment by default: fields
  placed at `align_up(offset, min(field_size, 8))`. Total size rounded to max member alignment.
  `DataDefSTRUCT.pack`: 0=natural (default), 1=packed, N=max alignment N.

- **`struct __attribute__((packed))`** — Packed structs with no padding between fields.
  Attribute parsed before or after the struct tag name.

- **`sizeof()` operator** — Resolves to integer constant at parse time. Supports `sizeof(int)`,
  `sizeof(struct name)`, `sizeof(int32_t)`. Works in expressions: `sizeof(int) * 10`.

- **Compound assignment operators** — `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`,
  `>>=`. All 10 operators with int and double support.

- **Postfix increment/decrement** — `x++` and `x--` with correct old-value-return semantics.
  Parser uses `prevToken()` for prefix/postfix disambiguation.

- **Hex integer literals** — `0xFF`, `0xDEAD`, `0X1A`, mixed-case digits.

- **Command line arguments** — `int main(int argc, char **argv)`. Script args passed from
  the command line. `get_argv(argv, i)` built-in returns `const char*` for the i-th argument.

- **Lazy symbol registration** — `lazy_map<name, {header, kind}>` defers `addGlobal`/
  `addFunction` until the parser first encounters the symbol. Supports variables, functions,
  types, and structs. Extensible for future `#include` headers.

- **`RTLD_GLOBAL` for `#load`** — Loaded library symbols are globally visible via
  `dlsym(RTLD_DEFAULT)`. No namespace prefix needed after `#include <math.h>`.

### Fixed — Phase 4 Prep

- **For-loop `regdp` clobber** — `TokenFOR::compile()` now resets `regdp` before condition,
  statement, and increment sub-compilations. Prevents comparison results from overwriting
  loop counter variables.

- **`cout << func()` crash** — BSL was injecting the ostream as a hidden first parameter to
  ALL function calls on the right side of `<<`. Fixed: only inject for ostream-consuming
  functions (checked via `has_ostream()` on return type).

- **dlsym function name** — dlsym fallback now registers functions under their original name
  (was `__dl_` prefixed, which broke repeated calls to the same function).

- **`streamout_cstr` null safety** — Added null pointer check for `const char*` output.

---

## [Phase 3.5+] — 2026-04-15

### Added — Post Phase 3.5

- **`switch`/`case`/`default` statement** — C-style switch with fall-through semantics. Case values are literal constants. `break` exits via loopstack. Tests: `testswitch.mad`.

- **`cin` / `>>` input operator** — Read from stdin. `cin >> name >> age;` for string, int, double. Chained input via BSR convergence (mirrors `<<` for cout). `DataDefISTREAM` added. Tests: `testcin.mad`.

- **Class methods** — `class Counter { int count; void inc() { count = count + 1; } };` Methods receive hidden `__this` parameter (void*). Member access resolves through `[__this + offset]`. Method names mangled as `ClassName__methodName`. Tests: `testmethod.mad`.

- **Regex support** — `madc::regex_match()`, `madc::regex_search()`, `madc::regex_replace()` via `std::regex`. `perl::grep` and `perl::split` upgraded to use regex (fallback to substring on invalid patterns). Tests: `testregex.mad`.

- **Multiple return values** — Go-style `return q, r;` and `q, r := divide(17, 5);`. Hidden `__retbuf` parameter injected at compile time. Values written to `[retbuf+i*8]`. Works with conditional returns in braced if/else. Tests: `testmultiret.mad`.

- **Ternary operator** — `condition ? true_expr : false_expr`. Uses stack-slot merge to avoid asmjit register convergence issues. Colon acts as expression stop in non-bracketed context. Tests: `testternary.mad`.

- **`madc::` namespace** — `madc::array` works alongside bare `array` keyword. Also hosts regex functions. Backward compatible.

- **`std::` namespace scoping for containers** — `std::vector<int>`, `std::map<string, int>`, `std::set<string>`, `std::list<int>` all work alongside bare keywords. `std::cin` also available.

- **Register-only foreach iterator** — Numeric element variables in range-for loops use `vfREGISTER` for tighter loops.

- **`pushToken()` / deque-based token queue** — Token queue changed from `std::queue` to `std::deque` for speculative parsing support.

### Fixed — Post Phase 3.5

- **asmjit v1.14 deprecation warnings** — Migrated ~70 call sites:
  - `FuncSignatureT<...>(CallConvId::kCDecl)` → `FuncSignature::build<...>()`
  - `FuncSignatureBuilder` → `FuncSignature`
  - `Operand::size()` → `x86RmSize()` (on asmjit operands only)
  - `cc.setArg()` → `funcnode->setArg()`

- **Mem←Mem safemov** — Added temporary register path for `safemov(Mem, Mem)` operations (needed for class method member access).

- **Multi-return cleanup crash** — Skipping `cleanup()` on multi-return paths prevents double-destruct when multiple return statements exist in if/else branches.

### Added — Phase 3.5 (Modern Language Features)

- **Range-based for loops** — `for (type var : container) { ... }` C++ style iteration over `array` and `vector<T>`. Parser detects `:` in for-header, emits `TokenFOREACH` with index-based loop. Break/continue supported.

- **Function pointers** — `auto fn = my_function; fn(args);` Store function addresses in variables and call through them. `DataDefFPTR` wraps `FuncDef` for typed indirect calls via `cc.invoke(ptr_reg, funcsig)`.

- **Lambda expressions** — `[](params) { body }` and `[type](params) { body }` for typed returns. Anonymous functions hoisted to AST as top-level `TokenFunc` entries (asmjit can't nest addFunc/endFunc). Auto-named `__lambda_0`, `__lambda_1`, etc.

- **`auto` keyword** — Type inference for function pointer and lambda assignments. `auto fn = greet;` or `auto add = [int](int a, int b) { return a + b; };`

- **`defer` statement** — Go-style deferred execution. `defer statement;` registers code to run at scope exit in LIFO order, before destructors. Stored on `TokenCpnd::deferred` vector, compiled in reverse during `cleanup()`.

- **`std::for_each()`** — Iterates a MadArray calling a function pointer per element. Works with named function pointers and inline lambdas.

- **Typed STL containers** — C++ template syntax with lazy DataDef instantiation:
  - `vector<int>`, `vector<string>` — push_back, pop_back, at, size, clear, empty + range-for
  - `map<string, int>`, `map<string, string>` — put, get, contains, erase, size, clear
  - `set<string>`, `set<int>` — insert, contains, erase, size, clear
  - `list<int>`, `list<string>` — push_back, push_front, size, clear

- **New source file** `src/ns_stl.cpp` — C++ helper functions for all STL container operations.

- **Documentation** — `docs/language/modern/` for range-for, function pointers, lambdas, defer. `docs/rules/` for branching and feature guard rationale.

- **Infrastructure** — `make debug` target, `scripts/run_tests.sh` helper, feature branch workflow with `#ifdef` guards.

### Added — Phase 2 (Core Language Features)

- **User-defined structs** — `struct Name { type member; ... };` parsed dynamically, registered in `struct_map`. All forms: named, anonymous, typedef, inline variable declaration.

- **Class definitions** — `class Name { int x; string name; };` with data members. Classes registered in `datatype_map` for prefix-free usage (`Point p;` instead of `class Point p;`). Method parsing infrastructure in place.

- **Namespace resolution (`::`)** — `namespace_map` registry on Program. `parseExpression()` resolves `ns::member` syntax. Unknown namespaces with `#load` handles fall back to `dlsym`.

- **`std::` namespace** — `std::cout`, `std::cerr`, `std::endl` mapped to existing globals.

- **`#include` directive** — `#include "file.mad"` tokenizes included file inline at lexer level. Saves/restores Source via move semantics. Recursive includes work. Relative path resolution.

- **`using` statement** — `using namespace std;` imports all namespace members. `using std::cout;` imports single member.

- **Identifier-as-type resolution** — `parseStatement()` checks `datatype_map` for user-defined types, enabling `ClassName var;` syntax without keywords.

### Added — Phase 3 (Multi-Language Namespaces & Dynamic Loading)

- **`php::` namespace** (36 functions) — String: trim, ltrim, rtrim, chop, ucfirst, lcfirst, str_repeat, str_replace, str_pad, str_word_count, nl2br, str_rot13, chunk_split, number_format, wordwrap. Array: explode, implode, count, array_push, array_push_int, array_pop, array_get, array_get_int, array_shift, array_unshift, array_reverse, array_unique, array_search, array_slice, array_merge, in_array, sort, rsort.

- **`perl::` namespace** (21 functions) — chop, chomp, grep, glob, split, join, push, pop, shift, unshift, scalar, reverse, lc, uc, ucfirst, lcfirst, index, rindex, length, substr.

- **`python::` namespace** (16 functions) — title, swapcase, center, ljust, rjust, zfill, count, startswith, endswith, isdigit, isalpha, isalnum, isspace, replace, format.

- **`ruby::` namespace** (12 functions) — squeeze, tr, chars, capitalize, delete, count, include, gsub, sub, rotate, compact, flatten.

- **`js::` namespace** (6 functions) — btoa, atob (base64), encodeURIComponent, decodeURIComponent, parseInt (with radix), stringify (JSON).

- **MadValue / MadArray** — Tagged union (`int64/double/string`) and container type for PHP-style mixed-type arrays. `array` keyword as a first-class data type with JIT construct/destruct.

- **`#load` directive** — `#load "libfoo.so" as ns;` opens shared libraries via dlopen, creates namespace with lazy dlsym resolution.

- **`dlopen`/`dlsym`/`dlclose`/`dlcall`** — First-class dynamic linking functions. `dlcall(funcptr, args...)` invokes through a function pointer with automatic string-to-cstr coercion.

- **Variadic function calling** — Functions with 0 declared parameters accept any argument count/types at compile time. Used by dlopen-resolved functions and `dlcall`.

- **ifstream/ofstream/fstream** — File I/O as first-class data types. Methods: open, close, eof, good, is_open. `<<` operator for writing, `getline()` for reading.

- **Type conversion functions** — `to_string(result, int)`, `stoi(str)`, `stod(str)`, `strlen(str)`.

- **C library globals** — `system(cmd)`, `getenv(result, name)`, `setenv(name, value)`, `unsetenv(name)`.

### Fixed — Phase 2/3

- **String pass-by-value** — `voperand()` was constructing an empty string for function parameters, losing the caller's pointer. Fixed: parameter variables get a bare Gp register; `cleanup()` skips parameter destruction.

- **`dtSTRING -> dtCHARptr` coercion** — Added `string_cstr()` helper. `TokenCallFunc::compile()` auto-converts string arguments to `const char*` when calling functions like `puts()`.

- **Stream good()/eof() crash** — `ifstream`/`ofstream` inherit `std::ios` via virtual inheritance. Casting `void*` directly to `std::ios*` skipped the vtable pointer adjustment (256 bytes on x86-64). Fixed with type-specific wrapper functions.

- **Struct definition returns** — `TokenSTRUCT::parse()` returned `this` for pure definitions, causing `TokenKeyword::compile()` errors. Fixed to return NULL.

---

## [Phase 1] — 2026-04-14

### Added

- **`-v` / `--verbose` flag** — `DBG()` output gated behind `madc_verbose`.
- **`register` keyword** — `vfREGISTER` flag for register-only variables.
- **doctest unit test framework** — `include/doctest.h`, `tests/unit/`, `make test`.
- **Documentation:** usage.md, testing.md, test-status.md, revival-plan.md, rules.

### Fixed

- Char literal compilation (`putchar('h')` now works).
- Struct member access (`addOffset` vs `setOffset`).
- Struct string member lifecycle (construct/destruct).
- `DBG()` dangling-else (do-while idiom).
- asmjit v1.14 API migration.

---

## Prior to Changelog

- Project originally written ~2019.
- Dormant for ~7 years due to asmjit API changes breaking the build.
- April 2026: asmjit v1.14 migration completed; binary builds and runs.
