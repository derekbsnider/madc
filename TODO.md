# TODO

## High Priority

- **GCC torture test suite: push toward 95%.** Currently 1554/1685 (92.2%).
  v0.18.0 closed 169 new tests via _Complex arithmetic, IEEE FP
  semantics, bitfield promotions, auto-include headers, alias support,
  setjmp, bswap, and dozens of individual fixes.
  Remaining targets: `__builtin_return_address`, struct pass-by-value
  for function args (~8 compile/runtime failures), 32-bit arithmetic wrapping
  in widening cast contexts (`(long)(uint_a + uint_b)`), compound
  assignment evaluation order, deeper inline-asm operand coverage, and
  the remaining parser/runtime fronts after `pr108292.c`, `pr109040.c`,
  `pr109938.c`, and `pr109986.c`.
  The focused `_Complex` execute lane is currently green; the active
  fronts have shifted back to the remaining non-complex GCC gaps.

- **Native SMAUG next step: move beyond the first serpent combat path.**
  `smaug.exe` now survives startup, login, room 109 serpent combat, and
  serpent death on the standalone native executable lane. The next
  practical runtime milestone is broader post-combat gameplay:
  additional newbie-path encounters, spells, mobprogs, and longer
  session stability on the native runtime.

- **Phase 4.2 / libmadc public C++ API — next pieces.** `madc::value`,
  `madc::error`, and the first `madc::program` slice now ship at
  `include/libmadc/value.h`, `include/libmadc/error.h`,
  `include/libmadc/program.h`, and a first convenience wrapper tier in
  `include/libmadc/api.h`. `madc::program` is now a pimpl over the
  internal `Program` class with `compile_file`, `exec_file`,
  `exec_string`, public diagnostics access, a first
  `register_function(name, callback, signature)` slice for global host
  callbacks using explicit native signatures, and a first
  `call(name, args, result)` slice for script-from-C++ invocations plus
  first `get_global(name, result)` / `set_global(name, value)` support
  for scalar and `string` globals, plus a first `eval(source, result,
  virtual_filename)` slice that compiles an in-memory translation unit
  and calls a reserved zero-arg `__madc_eval` entrypoint. The
  convenience layer now exposes top-level `madc::eval(...)`,
  `madc::exec_string(...)`, and `madc::exec_file(...)` as thin wrappers
  over a temporary `madc::program` for default-program use cases, while
  the real stateful surface stays on `madc::program`. A first
  `eval_expression(...)` path is now there too, backed by the same
  stateful API and now with a dedicated `expression_policy` surface for
  expression-only header/function allowlists, with `math.h` as the
  first real header group. The first public `compile_options`, `security_policy`,
  `execution_mode`, and `invoke_limits` surfaces are also in place. The options/policy seam now reaches the first real
  authority escape hatches too: builtin-registration toggles and
  built-in namespace registration toggles flow through
  `Program::RegistrationPolicy`, and `enable_dlfcn_functions` now also
  gates `#load`, `#load`-backed namespace `dlsym`, parse-time
  RTLD-default symbol fallback, and compiler-side extern late-bind
  `dlsym`. `authority_mode::system_locked` is now also a real enforced
  preset on that public seam: the effective policy and compile options
  clamp process builtins and dynamic-loading paths off, and later
  `set_compile_options(...)` calls cannot re-enable them. The next
  worker/isolation seam is also now explicit in public policy:
  `security_policy.execution` carries `in_process` vs
  `fork_per_invocation`, and real child-process runtime is now attached
  to that seam for `exec_file(...)` / `exec_string(...)` plus the
  current narrow scalar/C-string `call(...)` and entry-function-based
  `eval(...)` result path, with success/failure, diagnostics, and
  stdout/stderr propagation. `invoke_limits`
  now also enforce post-invocation budgets on
  the public host API for CPU time, resident-memory growth, and
  output/error bytes across both `MadcEngine`-managed streams and raw
  libc `stdout`/`stderr` writes captured during invocation, including
  fork-mode output captured from the child plus child `wait4()` rusage
  accounting. This is honest
  after-the-fact accounting rather than in-process preemption. These
  public runtime-call/global surfaces are intentionally narrow for now:
  they handle only the scalar / C-string subset (`void`, `bool`,
  `int64`, `double`, `const char *`) plus script `string` globals,
  while `call(...)` now supports up to four arguments on that subset
  and also accepts host `std::string` values plus script `string`
  returns/callbacks on the underlying `std::string*` object ABI. It
  still intentionally rejects multi-return and varargs, and `eval(...)`
  is still entry-function based rather than free-form expression
  evaluation.
  `eval_expression(...)` is now the dedicated expression
  path, and the default execution route now lexes/parses a single
  expression into a synthetic hidden function instead of synthesizing a
  whole translation unit. It is still intentionally narrow: `math.h` is
  the only first-class header group, but libm-backed expressions now run
  through that dedicated AST path without the earlier compatibility
  fallback. Parsed expressions now also pass through a first explicit
  AST whitelist on that dedicated seam, but there is still not yet a
  broader dedicated expression-sandbox grammar. The semantic policy is
  also now more explicit: function calls, member access, subscript
  access, and pointer operations are separate `expression_policy`
  levers instead of being implicitly allowed by AST shape alone. A
  first host-supplied binding seam is now there too: expression
  evaluation can read explicit host-provided scalar/string bindings
  without routing through globals, which gives us the clean base for a
  later object/struct-backed context surface. That next layer is now
  also partially real: `set_expression_context(...)` can take a host
  `madc::value::object` and treat its top-level fields as the source of
  expression bindings, with explicit collision checks against direct
  bindings. The next narrow layer on top of that is now there too:
  nested object fields can now be traversed for static primitive leaves
  (e.g. `user.stats.level`) through a parser-owned named-root/context-
  object model instead of pre-parse dotted-path rewriting, while raw
  pointer dereference/member semantics remain out of bounds. The
  nested-context seam is also now path-aware instead of
  all-or-nothing: missing nested fields and bad descent through a
  primitive leaf fail with explicit runtime diagnostics, while
  unrelated unsupported context leaves no longer poison the whole
  context up front unless the expression actually references them. A
  first in-language bridge is now there too: script code can call
  `madc::eval_expression(out, expr)` inside madc itself for
  stringified scalar/string output, and can now also use typed helpers
  `madc::eval_expression_int(expr)`,
  `madc::eval_expression_bool(expr)`,
  `madc::eval_expression_double(expr)`, plus exact-string
  `madc::eval_expression_string(out, expr)`, all layered on top of the
  same libmadc expression runtime and allowing libm-backed calls
  through `math.h` when dlfcn policy is enabled. Madc script code can
  now also build `MadArray`-backed associative context objects via
  `madc::context_set_int/real/string/array(...)` and evaluate against
  them through `madc::eval_expression_ctx(...)` plus typed `_ctx`
  helpers. Full in-language `madc::eval(out, source)` is now also
  present for entry-function-based runtime evaluation of whole source
  strings, with typed `eval_int/bool/double` and exact-string
  `eval_string(...)` helpers layered on the same host eval seam. The
  parser/runtime bridges now hang off `Program`-owned internal
  `runtime_eval_source(...)` / `runtime_eval_expression(...)` helpers
  instead of orchestrating temporary wrapper programs directly, and
  those helpers now also execute their own internal compile/validate/
  zero-arg-call path instead of routing back through the public
  `madc::program` facade. The synthetic
  hidden-function build step is now also owned by
  `Program::build_expression_function(...)` instead of only by the
  embedding wrapper, and full in-memory `eval(...)` / `exec_string(...)`
  now also compile source buffers directly through a `Program` in-memory
  translation-unit tokenize/parse/compile seam instead of writing temp
  files first. Script-side runtime eval can now also capture the current
  visible madc scope through a compiler-synthesized `MadArray` context
  carrier, and that capability is independently gated by
  separate source-vs-expression policy bits
  (`compile_options.enable_runtime_eval_source_scope_access` /
  `security_policy.allow_runtime_eval_source_scope_access` and
  `compile_options.enable_runtime_eval_expression_scope_access` /
  `security_policy.allow_runtime_eval_expression_scope_access`)
  instead of being tied to the broader full-program sandbox. Full
  script-side `eval(...)` now also has its own child-program sandbox
  surface through `runtime_eval_policy`, so the remaining policy work is
  about refinement rather than first extraction. The next internal-facing
  step is to decide how much richer that scope model should become:
  today it handles current visible variables through a hidden context
  object and child-program global injection for full `eval(...)`, but
  deeper lexical/symbol fidelity, richer non-primitive scope values, and
  any finer-grained runtime-eval child capabilities still need
  follow-up. The common typed script-side `madc::eval_*` path no longer
  requires an explicit `__madc_eval(...)` wrapper either: it auto-wraps
  body text when the helper already knows the return type. The remaining
  explicit-entry awkwardness is now concentrated in the generic
  value-typed eval surfaces, which still lack a reliable wrapper return
  type. Host-side C++ ergonomics are now better too: `madc::program`
  and the top-level `madc::eval(...)` / `madc::eval_expression(...)`
  wrappers both have typed overloads for `bool`, `int64_t`, `double`,
  and `std::string`, so common embedding call sites no longer need to
  unpack `madc::value` manually. The host API now also has an explicit
  `eval_body(...)` lane for body-text runtime eval with typed overloads
  and a generic `program::native_type` contract, so the remaining
  awkwardness is mostly about whether generic full-source `eval(...)`
  should ever grow beyond its current explicit-entry semantics.
  `eval_unit(...)` now names that explicit full-source lane directly,
  leaving the open question more about long-term compatibility and
  whether `eval(...)` should eventually become only a compatibility
  alias in docs/examples. The host-side `call(...)` surface is also a
  bit less narrow now: the existing scalar/C-string subset supports up
  to four arguments instead of stopping at arity 2, so the next call-
  surface questions are more about richer types and result shapes than
  about the very first arity ceiling. Host callback ergonomics are
  better too: `madc::program::register_function(name, fn)` can now
  deduce supported plain C++ function-pointer signatures, including
  `std::string`-backed callback parameters and returns, by lowering
  through the existing low-level `native_signature` / `string_object`
  ABI. The next callback-facing work is therefore no longer the first
  ergonomic layer, but whether to add broader wrapper coverage
  (capturing lambdas / functors, richer diagnostics for unsupported
  signatures, or more result/argument shapes) without muddying the core
  ABI surface.
  Phase 4.3 is no longer just planned: the repo now has a first
  `lib/libmadc.so` target built from a dedicated PIC object set, an
  `install-libmadc` path, and a first thin C ABI in
  `include/madc_api.h` / `src/madc_c_api.cpp` for opaque program
  handles plus scalar/string value exchange. That shim now also covers
  scalar policy / invoke-limit mirrors and diagnostics enumeration, so
  the next work is to widen it carefully without duplicating the C++
  API wholesale: stable allowlist/vector handling, any deliberately
  supported callback conventions, and clearer install/consumer
  documentation should come before larger convenience growth. The
  staged install/use smoke path is now also real through
  `make -C src libmadc-smoke`, so the next library-readiness work is to
  audit the installed public header surface and decide what should stay
  first-class versus remain internal/provisional before the C shim grows
  much further. In
  parallel, finish the remaining
  authority/resource gaps beyond the current first-pass dlsym / `#load`
  gates and post-invocation invoke-limit accounting, especially
  broader fork-mode execution coverage beyond the current narrow
  call/eval subset and any stronger/preemptive execution controls, then
  broaden the call/callback/global/eval surface as needed. Also keep
  the future subsystem split in
  view while finishing the separation: core embedding/runtime stays in
  `libmadc`, including core `madc::DataSource`. The `madcdat`
  storage/federation/indexing track now has a real configure-time gate
  at `./configure --enable-madcdat`, canonical data headers under
  `include/madcdat/`, the `src/madcdat_*.cpp` source seam, a standalone
  `lib/libmadcdat.a` archive target, and `make install-madcdat`. The
  next remaining split work is shared-library packaging plus the fuller
  eventual `libmadc`/`libmadcdat` install story, while the legacy
  `include/libmadc/*.h` data-layer paths remain temporary compatibility
  forwarders. The test harness itself is also a little stricter now:
  `make -C src test` stops on the first crashing/failing unit binary
  instead of falsely returning the last test's exit code.

- **Core-work validation default — keep `madcdat` disabled unless it is relevant.**
  The repo now has a real top-level `./configure --enable-madcdat`
  gate, so core parser/compiler/`libmadc` work should normally run in a
  workspace configured with `--enable-madcdat=no` to reduce rebuild and
  unit-test scope. Re-enable it before final validation when touching
  storage/federation code, shared public headers, build wiring, or any
  surface that may affect `madcdat`.

- **Exploratory storage/federation track — move beyond the first local
  backend family.** The first local backends now work from ordinary host
  C++: `DataSet<T>` + registration-based `infer_mapper()` +
  built-in `DataDriverRegistry` + `DsvDriver` + `FlrDriver` +
  `VlrDriver` + `QdbmDriver` + `GdbmDriver` + `BdbDriver` +
  `SqliteDriver`, covered by
  `tests/unit/test_libmadc_dsv.cpp`,
  `tests/unit/test_libmadc_flr.cpp`, and
  `tests/unit/test_libmadc_vlr.cpp`, plus ordered keyed coverage in
  `tests/unit/test_libmadc_qdbm.cpp` and
  `tests/unit/test_libmadc_bdb.cpp`, plus unordered hash-backed keyed
  coverage in `tests/unit/test_libmadc_gdbm.cpp`, plus SQL-backed keyed
  coverage in `tests/unit/test_libmadc_sqlite.cpp`. FLR now also has an
  optional packed-bit tombstone sidecar plus pre-reap `restore(key)`
  coverage, and `compact()` can now reap dead rows into a parallel dead
  archive FLR while rewriting the live FLR. Restore-after-reap is now
  defined too: `restore(key)` can pull a row back out of the dead
  archive and reinsert it into the live FLR, preserving ordered-key
  sort position when the dataset is configured as ordered. The first
  real FLR index -> VLR payload offset binding also now exists through
  record locators plus `Relation::resolve(...)`, and VLR locator
  stability is now defined too: locator-aware VLR datasets opt into an
  append-only tombstone-sidecar contract so live offsets survive reopen
  and stale/tombstoned locators fail explicitly. A first real query
  pushdown path now exists too: `DataSet<T>::query(...)` executes simple
  builder queries directly on `sqlite://` and keyed `qdbm://` /
  `gdbm://` / `bdb://` backends when they can honor the requested
  equality filter, and falls back locally otherwise. Ordered/lower-
  bound key scans now push too: `sqlite://` executes `>=` + `LIMIT`
  builders in key order, and ordered keyed `qdbm://` / `bdb://`
  backends can honor the same lower-bound scans through native cursor
  positioning. Bounded key ranges now work too via combined `>=` / `<=`
  builder clauses on `sqlite://`, `qdbm://`, and `bdb://`. First
  relation-aware traversal is now there too:
  `Relation<A,B>::query_related(...)` can walk filtered source rows and
  materialize `key_match` and `offset` targets across datasets/backends.
  Raw projected builder queries now exist too through
  `DataSet<T>::query_raw(...)`, so `select(...)` can return projected
  `madc::value` objects without waiting for typed partial decode, and
  strict builder bounds now exist too through `where_gt(...)` /
  `where_lt(...)`. `Relation<A,B>::query_related_raw(...)` now also
  accepts target-side builder filters/limits instead of only
  dataset+projection metadata.
  The canonical storage plan now also explicitly separates logical query
  IR from physical execution IR and treats driver capabilities as a
  coarse planning filter rather than a replacement for per-backend query
  shape validation. `where_ne(...)`, `where_in(...)`,
  `where_not_in(...)`, and `where_like(...)` now exist too: SQLite
  pushes all four, while keyed stores deliberately fall back
  locally. A first logical-composition seam now also exists through
  query match-mode metadata (`all` vs `any`), but runtime execution
  still explicitly rejects non-default composition for now. Next concrete
  steps: federated planning on top of that pushed-query path; decide
  whether projection pushdown should
  expand beyond the current keyed/SQLite drivers; typed
  `Relation::query_related(...)` now also honors target-side filters and
  limits, so the next open relation question is typed partial decode or
  planner-owned projection rather than basic target filtering. Then
  broaden builder support from that match-mode seam into real boolean
  composition beyond simple single-field predicates and
  ordered bounds into richer boolean/query composition; decide whether
  FLR/VLR relations should grow
  helper paths for index maintenance after payload rewrites; then branch
  into the next backend tier (`leveldb://`, `rocksdb://`, or service
  protocols). In parallel,
  keep the layering from `docs/plans/data-storage-federation.md`
  intact: `DataSource` identity separate from source parsing,
  source/extracted-record adapters separate from dataset-local mapping,
  and derived index/reindex policy separate from drivers. In parallel,
  broaden mapper inference beyond explicit field registration
  (ideally toward struct/class metadata reuse and lower-friction
  declarations for common host types); then decide whether an
  `xqdbm`-aware higher-level adapter belongs alongside the generic
  record-driver contract.

- **Optional-backend configure path — finish the new build scaffolding.**
  `configure.ac`, `config.mk.in`, and `Makefile.in` now probe optional
  support for Berkeley DB, GDBM, QDBM/Villa, XQDBM, and SQLite3 and
  feed feature flags into `src/Makefile` without breaking `make -C src`.
  Next work here: generate and validate the Autotools outputs on a
  machine with `autoconf` / `automake` / `libtool` installed, then
  replace any remaining backend stub translation units with real driver
  implementations.

- **Phase 4.1 cleanup — kill residual process globals.** State split
  is largely landed. Remaining process-global mutable state per the
  `docs/plans/libmadc-phase4.md` §B inventory: `madc_verbose`
  (`src/madc.cpp:31`), `throwit` (`src/madc.cpp:33`),
  `g_madc_asmjit_err` (compiler.cpp), `g_madc_jit_map*` JIT crash
  source-map globals (compiler.cpp). Each becomes either an
  engine/program field or a CLI-only concern.

- **Exercise SMAUG spells / mobprogs beyond the first combat path** —
  Character creation through in-room movement is verified, and the
  first live combat path now survives into repeated damage rounds
  without the earlier `damage()` crash. Spells (`cast <name> <target>`,
  mana costs) and mobprogs (NPC scripted reactions) still need focused
  runtime coverage and will likely surface the next batch of latent
  codegen issues.

## Medium Priority

### Performance / startup

- **Phase 4 console/logging manager** — Stream-style level facades
  (`madc::emerg` through `madc::debug`) ship as line-buffered
  `std::ostream`s with a runtime `log_threshold` filter, a sink
  registry (`add_log_sink` / `clear_log_sinks` /
  `log_to_error_stream` toggle), a syslog sink, a file sink with
  size-based rotation + `reopen_log_file()` for logrotate integration,
  a JSON-line sink for aggregators, and a declarative
  `MadcEngine::Config` + `apply_log_config()` covering threshold +
  flags + every sink. Re-enable / re-apply duplication regressions are
  fixed; built-in sinks are engine-owned instead of leaked callbacks in
  the generic sink vector. Next plausible work: date-based file rotation
  (rolling daily/hourly), an in-memory ring-buffer sink for crash-time
  postmortem dumps, and per-sink threshold overrides (so e.g. JSON
  ships at info while file ships at warn).

- **Phase 4.4.a — JIT code cache** — every madc boot pays full
  lex+parse+asmjit-compile, even when the source hasn't changed.
  For SMAUG (~158k LOC across 49 .c files) that's ~43s of pure
  compile vs ~0.2s for actual `boot_db()`. A one-time cache of
  the asmjit-emitted machine code + relocation table, keyed on
  a hash of all input source files, would skip straight to
  executing JIT'd code on cache hit and bring SMAUG boot under
  a second on rebuilds. Design + estimated effort in
  `docs/plans/revival-plan.md` §4.4.a (~1 day, ~300 LOC, depends
  on §4.1–§4.3 landing first).

- **Phase 4.4.b — True AOT to ELF `.so`** — the embedding-in-host
  story (CMS / game engine / sandboxed scripting) wants madc-
  compiled scripts shipped as ordinary shared objects, no JIT
  pause at load time. asmjit's Object/ELF mode supports the
  underlying primitive; design in §4.4.b. Stacks on top of 4.4.a.

### Language Completeness

- **Rust-flavored syntax — extend `rust::match`, plan `rust::if let`** —
  `rust::match` v1 ships integer patterns, `|` OR-arms, and `_`
  wildcard with no fall-through (see `docs/language/rust-match.md`).
  Next plausible v2 work, in rough order of value: string patterns
  (today routed through `if`/`else if`/`strcmp` chains), range
  patterns (`1..=10 =>`), and match-as-expression with a stack-slot
  merge similar to ternary. `rust::if let` should still wait until
  there is a concrete tagged-union / `Option` / `Result` value model
  to destructure.

- **Rust-style "safe memory" types need a real ownership model** —
  `Box` / `Rc` / `Arc` / `RefCell` / borrow-checked references are not
  namespace helpers; they imply allocator policy, destructor rules,
  aliasing rules, move semantics, and likely new type forms. Worth
  exploring only after deciding whether madc wants lightweight runtime
  reference-counted containers, affine move-only values, or something
  simpler and host-oriented.

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Error diagnostics** — parser-side diagnostics already carry source context,
  and compiler top-level raw-string failures now anchor to the current
  statement token. Compound-assign / inc-dec operator diagnostics were
  swept to use `Throw(this/left/tse)`. Remaining work is the deeper
  internal-invariant raw `throw "..."` sites in `compile.cpp` (call-site
  helpers, set_invoke_arg, IRBuilder/coerce internals); those generally
  signal compiler bugs rather than user errors, so the value of token
  context is lower.

- **Operator overloading (user-defined)** — `operator+`, `operator<<`, etc. on user types.
  Currently the compiler has hard-coded special cases for std::string assign, stream `<<`,
  subscript; there's no way for user code to define its own.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **Retire `get_argv()` built-in** — predates raw-pointer subscripting on
  `char **`. `argv[i]` now works directly (Phase E raw-pointer subscript)
  and is the documented form. Registration is kept for backward
  compatibility with older scripts; remove once no in-tree code or
  external scripts depend on it.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

## Known Runtime Bugs (surfaced but pre-existing)

- **(RESOLVED 2026-04-30)** ~~Chained subscript-assign expression value~~
  — `(BUFF[i] = fgetc(fp)) != EOF` returned the unbound RHS register's
  full int instead of the byte that was actually stored,
  sign-/zero-extended back to int64.  Fixed in a59adbb: emit
  `movsx r64, r8/r16` from the RHS register's low bytes after the
  store, and respect `regdp.first` so outer assignments
  (`r = (buf[i] = x)`) get the value written into their destination.
  MadSMAUG patches/madc-fgetc-loop.patch retired; the original
  upstream `while ((BUFF[num]=fgetc(fp)) != EOF) num++;` pattern now
  compiles correctly.  tests/testsubscriptassign.mad covers it.

- **(RESOLVED 2026-04-30)** ~~SMAUG `&X` colour codes stripped, letters
  passed verbatim~~ — Switch-default body was always emitted last, so
  when `default:` was first in source order (the SMAUG colorize idiom),
  the unlabeled tail-code that follows the last case fell through into
  default and returned `ln = -1`.  Visible victim:
  `make_color_sequence` returned -1 for every &Y/&G/&C/&w; prompts,
  room titles, and help entries came through with the `&` stripped and
  the colour letter passed verbatim.  Fixed in 396c147 by tracking
  `default_index` in TokenSWITCH and emitting default body at its
  source-order position.  tests/testswitchdefaultorder.mad covers it.

- **(RESOLVED 2026-04-30)** ~~SMAUG colorize.c DISPOSE-NULL spam
  during boot~~ — Caused by enum-constant arithmetic folding to 0
  because TokenVar didn't override ival()/dval(). Fixed in 12fb103
  with a 9-line override; SMAUG boot is now spam-free.
  tests/testenumconstfold.mad covers it.

- **(RESOLVED 2026-04-30)** ~~SMAUG mud_prog.c:2437 SIGSEGV during
  area_update~~ — Caused by parseExpression treating `,` as a hard
  stop; brace-less `while ((*p=*i)!='\0') ++p, ++i;` ran only `++p`
  per iter, the strcpy walked off its buffer. Fixed in 731f18f with
  parseExprStmt + TokenComma::compile. tests/testcommastmt.mad.

- **(RESOLVED 2026-04-30)** ~~SMAUG mud_comm.c:371 NULL strstr deref~~
  — Caused by function-scope `static char const *p = "literal";` not
  having its initializer hoisted to program startup. Fixed in
  acbfdb0; static initializers now run once before main like C
  semantics. tests/teststaticlocalinit.mad.

- **(RESOLVED 2026-04-30)** ~~Real-typed global Mem-store reloc-out-of-range~~
  — Three independent gaps in TokenCpnd::movreg / movmptr2xval /
  putreg. Fixed in a5f6a49.

- **(RESOLVED 2026-04-29, session 11)** ~~Struct-return-by-value ABI~~
  — Fixed by TokenCallFunc small-struct spill path. SysV
  (rax, rdx) is captured to a 16-byte stack slot whose address
  becomes the call's operand; downstream struct memcpy reads from
  it correctly. SMAUG `pMobIndex->act = fread_bitvector(fp);` now
  loads cleanly and gods.are produces the standard
  `Rooms / Objs / Mobs` area summary.

- **(RESOLVED 2026-04-29, session 12)** ~~SMAUG limbo.are NULL deref~~
  — Resolved earlier in session 11 by the TokenSubscriptExpr lvalue
  override (limbo.are and the rest of the area list — 25 files —
  load end-to-end).

- **(RESOLVED 2026-04-29, session 12)** ~~SMAUG load_vaults
  segfault~~ — Five compounding fixes (mixed string/char-pointer
  ternary unification, TokenTerQ merge_slot rewrite, dtSTRING ↔
  pointer / int64 ↔ string IR coerce extensions, local fixed-array
  LEA re-emit on cache hit, crash-handler stack walk for non-JIT
  faulting RIP). SMAUG now boots through every load_*() phase and
  reaches `[probe] after boot_db` in the bootstrap shim.

- **(RESOLVED 2026-04-29, session 11)** ~~SMAUG umbrella wrapper-frame
  corruption~~ — Root cause was NOT a stack-frame analysis issue. Out
  of 1878 user-defined functions, ~1710 had unbound funcnode labels
  because asmjit's Compiler v1.14 silently drops labels of funcnodes
  added between two `cc.addFunc(node)` calls for the same FuncNode.
  The umbrella has ~125 functions defined twice (shim stubs override
  upstream defs), each sharing one FuncDef + FuncNode. Unbound labels
  emitted as `call $+5` (zero-displacement), pushing extra return
  addresses that corrupted wrapper ret pops → SIGSEGV at
  0xfffffffffffffff0. Fixed by deduping `pending_funcs` in reverse
  order and short-circuiting earlier TokenFuncs that share a FuncDef
  with a later one.

- **(RESOLVED 2026-04-30)** ~~Multi-return runtime output silently truncated~~
  — Root causes were split across both ends of the hidden `__retbuf`
  path: the callee treated the stack-slot parameter for `__retbuf` as a
  Gp directly when writing return slots, and the caller never injected
  the hidden retbuf pointer as arg0 for multi-return calls. Fixed by
  loading `__retbuf` from its parameter slot before stores and by
  prepending the caller's retbuf pointer in `TokenCallFunc::compile()`.
  `tests/testmultiret.expect` now asserts the runtime output.

- **(RESOLVED 2026-04-30)** ~~`ruby::chars` crashes in MadValue destructor~~
  — The stale symptom collapsed into two compiler/runtime gaps: (a)
  namespaced call argument parsing was reusing `current_namespace`
  inside `parseCallFunc()`, so `ruby::chars(chars, s)` resolved the
  first argument back to `__rb_chars`; (b) nested-array values were not
  first-class in `MadValue`, which blocked useful PHP/Ruby array-of-array
  helpers. Fixed by suspending `current_namespace` while parsing call
  arguments, resolving namespace members first only at expression head,
  and teaching `MadValue` to deep-copy / destroy nested `array` values.
  `tests/testrubycharsshadow.mad` and the uncommented `ruby::chars`
  coverage in `tests/testlang.mad` now pass.

## Deferred / Future


- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API + sandboxed-scripting use case** —
  Decouple static globals, create public C API. The load-bearing
  product direction is **embedded sandboxed scripting** for host
  runtimes (Node.js CMSes, game engines, embedded admin consoles,
  plugin marketplaces) that want to give C/C++ devs a way to extend
  them without native addons. Position none of vm/vm2/isolated-vm/
  Workers/WASM/Lua/AngelScript/ChaiScript holds: **JIT-compiled
  C/C++ as a sandboxed script language with PHP/Perl/Python/Ruby/JS
  idioms borrowed via namespaces.** SMAUG (158k lines of unmodified
  upstream C) is the credibility proof. Embedding API design must
  support up front:

  - **Fork-pool architecture.** Parent dlopens libmadc, parses+JITs
    the script once, listens on a unix socket; per-request `fork()`
    inherits R+X JIT pages via Linux COW (~hundreds of microseconds
    cold start, zero parse/JIT cost per call). Child writes JSON to
    stdout, exits.
  - **`MADC_SAFE_MODE`** flag at namespace registration: refuse to
    expose `system`/`execve`/`popen`/`fork`/`clone`/`setuid`/
    `setcap`/`prctl`/`chmod`/`chown`/`chroot`/`mount`/`ptrace`/
    `kill`/`unshare`/`bpf`/`init_module`/raw-sockets even if the
    seccomp filter has gaps.
  - **Curated dlsym allow-list** (~200 entries — stdio / string /
    math / time / regex / basic socket) instead of the current
    "fall through to anything in libc" behavior. Allow-list, not
    deny-list — deny-lists rot every glibc release.
  - **Block `dlopen` / `#load`** in tenant mode (or restrict to a
    host-curated allow-list of pre-vetted shared libs).
  - **JSON-stdin / JSON-stdout** as the canonical IPC contract;
    `madc::stdin_json()` / `madc::reply_json()` helpers.
  - **rlimit wrappers** (`RLIMIT_CPU` / `RLIMIT_AS` / `RLIMIT_FSIZE`
    / `RLIMIT_NPROC`) and seccomp-filter scaffolding so the host
    doesn't have to wire each one by hand.
  - **OS-layer composition**: chroot per tenant, run-as-tenant-uid,
    `unshare(CLONE_NEWNET|CLONE_NEWNS|CLONE_NEWPID)` documented as
    the recommended sandbox stack.

  Threat model is **webhosting-tenant**, not untrusted-end-user —
  mirror mod_php's 20-year track record (tenants are accountable;
  block escape routes between tenants and to host, not all
  dangerous-looking calls). Prioritise this over the last 10% of
  SMAUG ingestion once the runtime blocker is unstuck.

- **Full C23 standard coverage** — After SMAUG 1.8 compatibility is reached, the next
  long-term goal for the C-dialect side is full C23. The early parser-only
  wave is now partly landed (`_Bool`, variadic macros with `__VA_ARGS__`,
  VLAs, `_Static_assert`, `alignof` / `_Alignof`, `typeof`, `nullptr`,
  digit separators). Remaining high-value language work is the deeper
  compatibility surface: designated initializers, compound literals,
  `_Generic`, `_Alignas`, `#embed`, `constexpr`, and the usual
  preprocessor / library parity corners. `.mad` stays as the naming
  convention for files using madc's beyond-C23 extensions
  (multi-language namespaces, etc.); bare `.c` / `.h` files get the
  C-compatible subset.

## Completed

### Session 2026-05-01 → 2026-05-02 (codex Phase 4 stretch on feature/c23-first-picks-codex)

- ~~**Phase 4.1 state split**~~ — `MadcEngine` class with
  `RegistrationPolicy`, `BuiltinRegistry`, `NamespaceRegistry`;
  engine-owned `namespace_preference`; `configure_program` /
  `create_program` / `attach_engine`; CLI routes through engine. Two
  coexistent `Program` instances exercised by unit tests. Eight
  commits `527706a..a4d4d39`.

- ~~**Phase 4.x structured diagnostics**~~ — Engine-owned
  `Diagnostic` / `DiagnosticSeverity` (warning|error) /
  `DiagnosticPhase` (lexer|parser|compiler|runtime), `last_diagnostic`,
  `set_error` / `report_warning`, `format_diagnostic`,
  `print_diagnostic`, `can_show_diagnostic_source`, `ErrorInfo`. Four
  commits `fef63bb..5421ce4`.

- ~~**Phase 4.x engine-owned IO streams**~~ — `bind_input_stream` /
  `output` / `error`, `bind_input_string`, `capture_*_to_buffer`,
  `tee_*_stream` + `tee_*_to_buffer` (via `MadcTeeBuf`),
  `reset_standard_streams`, owned in/out/err `ostringstream` buffers.
  Four commits `cee1ff3, 8f47c80, d8504ed, 7c2f06b`.

- ~~**Phase 4.x madc::level facade + log_threshold**~~ — `LogLevel`
  enum (emerg|alert|crit|err|warn|notice|info|debug),
  `format_log_message` (timestamps + level prefix), `write_log` to
  error stream. `madc::emerg/alert/crit/err/warn/notice/info/debug`
  ostream-style globals via `MadcLogStreambuf` line-buffer. Runtime
  `log_threshold` filter short-circuits both `write_log` and the
  streambuf so filtered levels skip per-character work. Three commits
  `a640c10, ebd81b4, 9ce5902`.

- ~~**Phase 4.x log sink registry + sinks**~~ — `add_log_sink` /
  `clear_log_sinks` / `log_to_error_stream` toggle. Built-in sinks:
  syslog (`enable_syslog_sink` + public static
  `syslog_priority_for(LogLevel)` mapping all 8 levels to LOG_*),
  file (`enable_file_sink` with optional `max_bytes` / `max_files`
  size rotation + `reopen_log_file()` for logrotate integration),
  JSON (`enable_json_sink` + `json_escape` + `format_json_log_line`).
  Declarative `MadcEngine::Config` + `apply_log_config` covers
  threshold, flags, all sinks. Six commits
  `36af7a4, 6e1e76d, 9b0a494, 94d566a, 308d3e7, 615eff8`.

- ~~**Phase 4.2 madc::value (first public type)**~~ — Public
  embedding-API tagged container at `include/libmadc/value.h` /
  `src/madc_value.cpp`. Eight kinds (null, boolean, integer, real,
  string, bytes, array, object); deep-copy semantics, move leaves
  source null, structural equality, accessor mismatch throws
  `std::runtime_error`. Distinct from internal `MadValue`. Public
  API headers go under `include/libmadc/` (mirrors `libmadc.so`)
  so `include/madc/` keeps its embedded-scripting role. New unit
  binary `tests/unit/test_libmadc_value.cpp` (19 cases, 67
  assertions). Commit `bcceaf1`.

### Session 2026-04-26 (post-v0.12.0, session 9 — codegen cleanup)

- ~~**compiler/IR: drop spurious finalize ret + clean up codegen
  mismatches**~~ — Five fixes that drove SMAUG MADC_VALIDATE error
  count from ~50 to 2: dropped trailing cc.ret() outside any active
  function (was the noise-storm source); bind_call_return.narrow
  routed through dest.r64() instead of bare `mov gpw, gpq`; safemov
  Gp,Gp same-or-narrower path uses r64 trick; IRBuilder::load and
  store clamp aggregate Mem sizes (>8) to 8 and explicitly setSize
  the local Mem; subscript-write index path uses load_idx_to_gpq.
  Regression: tests/testnarrowdlret.mad. Commit `f89e422`.

### Session 2026-04-26 (post-v0.12.0, session 8 — diagnostics + bisect)

- ~~**compiler: cur_func_name + extra-index Gp widening**~~ — asmjit
  ErrorHandler now reports `in function: <name>` for every error,
  converting end-of-file noise into a per-function trail. Multi-dim
  subscript extra-index `add gpq, gpw` now widened via
  `load_idx_to_gpq`. Bisect of SMAUG umbrella narrowed bad codegen
  to act_comm.c (20 errors across ~10 functions; do_mstat worst at
  7). Commit `d8b4a59`.

### Session 2026-04-26 (post-v0.12.0, sessions 5–7 — VLA, runtime breakthrough)

- ~~**MADC_DUMP_ASM env knob**~~ — env-gated FileLogger captures full
  asmjit instruction stream to a file for offline analysis. Commit
  `18ce123`.

- ~~**TokenOperator::settype: pointer/fixed-array type propagation**~~
  — `buf+strlen(buf)` (buf=char[N]) typed as `char` instead of `char*`,
  variadic dlsym packing truncated 64-bit pointer to 1 byte at the
  call site. SMAUG's first boot_log line now prints from runtime.
  Commit `3b3ec93`.

- ~~**Fix asmjit instruction-size mismatches: subscript indices and
  IR stores**~~ — load_idx_to_gpq helper widens sub-word index Gps
  via movsxd/movsx; IRBuilder::store widens sub-word source Gp before
  storing into wider Mem; safemul/safeshl/safeshr/safeor/safeand/
  safexor force both Gp operands to r64. Plus MADC_VALIDATE knob.
  Commit `b828832`.

- ~~**compiler: always print cc.finalize() errors with name**~~ —
  was DBG-only, silent in normal builds. Programs with finalize
  errors compiled, exited 0, but main never ran. Commit `d8570b2`.

- ~~**TokenRETURN: handle `return void_call();` in void fn**~~ —
  inner expression runs for side effects; bare ret. Commit `ddc6694`.

- ~~**C99 variable-length array (VLA) support**~~ — `T name[expr]`
  with runtime-valued expr now compiles; backed by stack-resident
  pointer slot; malloc at scope entry, free in cleanup. Build.c
  finally compiles. Commit `c04632d`.

- ~~**safeadd: Xmm-lhs/Gp-rhs and Gp-lhs/Mem-rhs widening**~~ —
  cvtsi2sd / safemov before delegating. Commit `ac024f9`.

- ~~**parser: stop after cast push when initial_brackets == 0**~~ —
  closes SMAUG QUICKMATCH chained-eq through if/while.
  Regression: `tests/testchainedeq.mad`. Commit `ca3dc56`.

- ~~**parser: stop after cast push in stop_on_closing_paren mode**~~
  — closes SMAUG `*(EXT_BV*)pvd->data = fread_bitvector(fp);`.
  Regression: `tests/testderefcastassign.mad`. Commit `9dc614a`.

- ~~**resolveCompoundLHS: TokenDerefExpr lvalue**~~ — `*(expr) op=
  rhs` for pointer-yielding subexpressions now handled.
  Regression: `tests/testcompoundderefexpr.mad`. Commit `603a98d`.

- ~~**saferet: Mem operand support**~~ — load Mem into Gp before ret.
  Commit `7c9df6a`.

- ~~**parser: function-to-pointer decay on `return func;`**~~ —
  `(peek_id == tkSemi && opStack.empty())` added to value-end set.
  Closes SMAUG `tables.c:skill_function`. Regression:
  `tests/testreturnfndecay.mad`. Commit `613f9d2`.

- ~~**TokenCast: don't short-circuit (void *) through (void)-discard
  path**~~ — DataDef::rawtype() collapses pointer-to-void to dtVOID;
  the (void) discard branch's guard needed `!cast_type->is_pointer()`.
  Surfaced by SMAUG `comm.c:init_socket setsockopt(..., (void *)&x,
  ...)`. Regression: `tests/testvoidptrcast.mad`. Commit `78120ee`.

### Session 2026-04-25 (post-v0.12.0, session 4 — safediv mixed + ptr-subscript compound)

- ~~**safediv Gp/Xmm mixed dividend/divisor operands**~~ — op2's
  register family now selects integer vs real division; mixed op3
  is coerced via cvtsi2sd / cvttsd2si before idiv / divsd. Closes
  the SMAUG mud_prog.c blocker reached after the Gp-vs-Xmm safecmp
  closure. Commit `4a609c7`.

- ~~**resolveCompoundLHS raw-pointer subscript lvalues**~~ — `int *p;
  p[i] += N;` (and the rest of the compound-op family) now compile.
  Mirrors TokenSubscript::compile()'s pointer-subscript read path.
  Closes SMAUG `act_info.c` `prgnShow[iShow] += obj->count`. Two
  raw-throw sites in resolveCompoundLHS upgraded to `pgm.Throw(left)`.
  Regression: `tests/testcompoundptrsub.mad`. Commit `09f1c9b`.

- ~~**IRBuilder::coerce error includes src/dst type names**~~ — bare
  throw "unsupported type conversion" gave no signal which gap was
  firing; now the message names the actual types so the next
  investigation has somewhere to start. Commit `e30a666`.

- ~~**variables.c + update.c removed from MadSMAUG deferred list**~~ —
  both now compile cleanly (the IRBuilder::coerce char*→string
  blocker that gated them was closed by session 3's transient
  relabel). MadSMAUG-side change, uncommitted.

### Session 2026-04-25 (post-v0.12.0, session 3 — SMAUG mid-umbrella push)

- ~~**safecmp Gp-vs-Mem and Gp-vs-Xmm mixed comparisons**~~ — both
  paths now supported; closes `skills.c:check_parry` ABI mismatches.
  Commit `f10ffd7`.

- ~~**IRBuilder::coerce char*→string transient relabel**~~ — ternary
  with mixed string-literal + char* branches now coerces cleanly.
  Closes `fight.c:damage` front edge. Commit `2cc65ee`.

- ~~**emit_struct_init handles nested fixed-array members**~~ — SMAUG's
  `liq_table[]` `{ "water", "clear", {0, 1, 10} }` now compiles.
  Commit `b7d6347`.

- ~~**fn-ptr-member-call assignment-LHS guard**~~ — `ch->last_cmd =
  (aRoom ? do_rreset : do_reset);` no longer mis-parsed as a call.
  Commit `b7d6347`.

- ~~**`continue` inside `switch` inside `for` compiles**~~ —
  TokenCONT walks loopstack to skip over switches' (NULL, exit)
  entries. Commit `b7d6347`.

- ~~**parseFunction param-loop hardening**~~ — synthetic names fill
  ids gap from forward-decl/definition param-count mismatches; no
  more std::string-NULL-+8 crashes. Commit `a3a647b`.

### Session 2026-04-25 (post-v0.12.0, session 2 — float quirk root cause)

- ~~**Cross-function xmm-leakage variant of the asmjit float quirk
  closed at the root**~~ — the variadic-dlsym FuncSignature was being
  built without a variadic marker, so asmjit didn't emit the SysV
  `AL = N` setup that variadic functions need. AL was leaking from
  prior code (typically the format string's low byte). Fix:
  `funcsig.setVaIndex(1)` marks the call as variadic and asmjit emits
  the AL load. Both float-quirk variants the previous TODO entries
  filed (multi-arg-printf-reordering AND cross-function xmm-leakage)
  are now closed. `tests/testfloat.mad` is no longer layout-sensitive.
  Single-line fix; commit 3542968.

- ~~**`fd_set` typedef + `FD_*` macros take pointer**~~ — bare `fd_set`
  alias added to `<sys/select.h>` and `<sys/time.h>`; `FD_ZERO/SET/
  CLR/ISSET` now take `fd_set *` (matching glibc), so `FD_CLR(fd,
  &in_set)` no longer expands to `&(&in_set)`. Existing
  `tests/teststructinterop.mad` updated to the pointer call form.

- ~~**`struct hostent` in embedded `<netdb.h>`**~~ — full glibc layout
  (h_name, h_aliases, h_addrtype, h_length, h_addr_list + the legacy
  `h_addr` macro). Required by MadSMAUG `comm.c`.

- ~~**`((char *)expr)[i]` cast-of-pointer subscript**~~ — parser
  recognizes TokenCast-of-pointer as a valid subscript base;
  `TokenSubscriptExpr::compile` routes TokenCast through `compile()`
  (not `operand()`) so the cast emits before the index calc. Closes
  SMAUG `comm.c:3112`. Regression: `tests/testcastsubscript.mad`.

- ~~**`sizeof unary-expr` (no parens)**~~ — `sizeof ok_otype`,
  `sizeof *a`, `sizeof r` parse correctly. Closes SMAUG `grub.c`.
  Regression: `tests/testsizeofnoparens.mad`.

- ~~**Keyword case-labels + multi-decl identifiers**~~ — constant-
  integer-expression parser accepts contextual-identifier keywords
  (`case class:` for enum-tag-named-`class`); multi-variable
  declarations (`sh_int cou, race, class, ...`) accept the same.

- ~~**`try`/`catch`/`throw` as C identifiers**~~ — added to
  `is_contextual_identifier_token` and routed through `parseExpression`
  at statement position when not followed by `{` or `(`. Closes
  SMAUG `magic.c:5758` (`int try;` followed by `try = saving_throw()`).
  Regression: `tests/testkeywordsasidents.mad`.

- ~~**Pre-case declarations + stray `;` in switch bodies**~~ — switch
  parser accepts `OBJ_DATA *clone;` and `;` between `switch(...) {`
  and the first `case`/`default`. C allows it (the variable is
  unreachable but the declaration is well-formed). Regression:
  `tests/testswitchpredecl.mad`.

- ~~**Function-to-pointer decay before comparison/logical/bitwise
  operators**~~ — `if (t->fn == do_cast && ...)` failed because the
  decay heuristic only fired for value-end tokens. Adding `==`/`!=`/
  `<`/`<=`/`>`/`>=`/`&&`/`||`/`&`/`|`/`^` to the trigger set fixed it.
  Without this the call path silently consumed the operator token.
  Regression: `tests/testfnptrcompare.mad`.

- ~~**Struct member offsets after fixed-array members +
  array-of-pointers indexing**~~ — `DataDefSTRUCT` never recorded
  per-member counts; `m_offset()` walked `dd.size` per step instead
  of `dd.size * count`. Plus the parser/compiler "in-place aggregate
  vs stored pointer?" check used bare `is_pointer()` which mis-
  classifies `SKILLTYPE *arr[N]`. Fix: parallel `member_counts` vector
  + `m_count()` accessor + `TokenMember::is_fixed_array_member()`
  shared by parser and compiler. Three compiler sites + one parser
  site updated. Regression: `tests/teststructarrayofptr.mad`. Plus
  forward-typedef'd struct completion now copies `member_counts`
  alongside `members` (regression: `tests/teststructfwdtypedefarr.mad`).

- ~~**`extern char *strrchr/strstr/strdup/strpbrk/strtok/strndup` in
  embedded `<string.h>`**~~ — added with `char *` typed return.
  Previously blocked because adding these decls shifted the binary
  layout enough to re-trigger the float quirk; the setVaIndex fix
  eliminates the layout-shift fragility. Regression:
  `tests/teststrextra.mad`.

- ~~**Cast body stops at matching `)` in BSL chains**~~ — `cout <<
  (int)(a - b) << endl;` failed at parse with "Unexpected keyword in
  expression". Recursive parseExpression on the cast body parsed past
  the matching `)` and consumed the outer `<< endl;` chain. Fix:
  when the cast body starts with `(`, parseExpression with
  `stop_on_closing_paren=true` and `initial_brackets=1`. Regression:
  `tests/testcastparenexpr.mad`.

- ~~**Embedded `<crypt.h>` / `<netinet/in_systm.h>` / `<netinet/ip.h>`
  / `<arpa/telnet.h>`**~~ — `<crypt.h>` `#load`s libcrypt.so and types
  `extern char *crypt(...)`; `<arpa/telnet.h>` carries the TELNET
  protocol constants. Required by MadSMAUG `act_info.c` (crypt) and
  `comm.c` (telnet).

### Session 2026-04-25 (post-v0.12.0)

- ~~**Typed Xmm allocation + `extern char *strchr(...)` in embedded
  string.h**~~ — IRBuilder::newReg now dispatches on real-type size
  and uses `cc.newXmmSs` (float) / `cc.newXmmSd` (double) instead
  of the generic `cc.newXmm` (which asmjit types as `int32x4`).
  This gives asmjit's Compiler register allocator scalar-real
  type hints, which closes the multi-arg printf reordering case
  of the float quirk. With the allocator behaving, added
  `extern char *strchr` to embedded `<string.h>`. Targeted
  regression: `tests/teststrchrtyped.mad`. Closes the SMAUG
  `imc.c:340` front edge. Other char*-returning libc functions
  (strrchr/strstr/strdup/etc.) still trigger a separate cross-
  function xmm-leakage variant; filed under High Priority.

- ~~**`string` as a function parameter name**~~ — parseExpression's
  ttDataType branch now looks up the type-name as a variable first
  (when it's a contextual identifier), so `parsekeys(char *string)`
  works inside the body. Inline `string s = "..."` declarations
  still work via the next-token-is-identifier check. Targeted
  regression: tests/teststringparam.mad. Closes MadSMAUG
  imc-version.c:128.

- ~~**Keyword-as-identifier in enum body**~~ — TokenENUM::parse uses
  `is_contextual_identifier_token` / `contextual_identifier_name`,
  matching the rest of the parser's keyword-as-identifier handling.
  Closes MadSMAUG grub.c:500 (`enum gr_field_type { name, sex, class,
  ... }`). Targeted regression: tests/testenumclass.mad.

- ~~**`sizeof(*arr)` / `sizeof(*ptr)`**~~ — sizeof parser now has
  a `*identifier` branch. For a fixed-array variable, returns the
  element type's size. For a pointer variable, returns the pointed-
  to type's size. Targeted regression: tests/testsizeofderef.mad.
  Closes MadSMAUG update.c:2300-2301
  (`sizeof(times)/sizeof(*times)` element counter).

- ~~**`(*flfunc)(args)` classic C fn-ptr call**~~ — two gaps:
  (1) unary-`*` on a fn-ptr variable now pushes the var as a value
  (function-to-pointer decay reverses through `*`), matching the
  paren branch's behaviour for fn-ptr-typed expressions;
  (2) added a fn-ptr-VAR-call branch in the `(` handler parallel to
  the existing fn-ptr-MEMBER-call branch. Both branches now scan
  opStack with a tighter-than-`=` precedence threshold so `int v =
  (*fp)(arg)` works (with `=` on opStack) while `ch->fn && (other)`
  still doesn't mis-fire. Targeted regression:
  tests/testfnptrparenscall.mad. Closes MadSMAUG reset.c:985.

### Session 2026-04-25 (continued)

- ~~**fn-ptr-member-access + later `(...)`**~~ — fn-ptr-call
  detection at the `(` handler now scans opStack for any pending
  operator (anything not `(`); a non-empty pending op means the
  `(` belongs to a sub-expression, not a call through the fn-ptr.
  Closes MadSMAUG update.c:744-745 (`ch->spec_fun &&
  !IS_AFFECTED(...)`). Targeted regression:
  tests/testfnptrmember_binop.mad.

- ~~**`*++p` / `*--p` followed by a binary operator**~~ — explicit
  tkInc/tkDec branch in the unary-`*` parser builds a
  TokenInc/TokenDec with the pointer as `right` and wraps in a
  TokenDerefExpr. Avoids the recursive parseExpression-with-
  conditional-true that previously consumed any trailing binop
  (so `*++p == 'e'` parsed as `*(++p == 'e')`, a deref of a bool).
  Targeted regression: tests/testderefpreinc.mad. Closes MadSMAUG
  misc.c:2149.

- ~~**`extern` libc/system functions via dlsym late-bind**~~ — at
  TokenCallFunc::compile time, when a declared function has neither
  funcnode nor x86code, try dlsym(RTLD_DEFAULT, name) as a last
  resort and route through the typed-call path. Also extracted the
  int32-sign-extension whitelist into a file-scope helper so typed
  calls get the same movsxd dance. Targeted regression:
  tests/testexterndlsym.mad. Closes the MadSMAUG act_info.c:4642 /
  build.c:1822 `crypt` front edge for any libc symbol resolvable
  through RTLD_DEFAULT (crypt itself isn't, but strcmp/strncmp/etc
  all work).

- ~~**Char literals in macro args**~~ — lexer's macro-arg loop now
  treats `'` symmetrically with `"`, copying char literals verbatim
  through their closing `'` so any `(`/`)`/`,`/`"` inside (e.g.
  `')'`, `','`, `'"'`) doesn't disturb arg parsing. Targeted
  regression: tests/testmacrocharlit.mad. Closes MadSMAUG
  boards.c:599-607 (pager_printf with nested-ternary char-literal
  args).

- ~~**Constant-expression `<<`, `>>`, `&`, `|`, `^` operators**~~ —
  added full C precedence chain (shift → bitwise-and → bitwise-xor
  → bitwise-or) to the parse_constant_* recursive descent. Closes
  MadSMAUG act_obj.c:1735 (`case ITEM_HOLD:` where `ITEM_HOLD`
  expands to `(1 << 14)`). Targeted regression:
  tests/testconstexprshift.mad.

- ~~**`struct tag { ... } *first, *last;`**~~ — TokenSTRUCT::parse
  now routes through parseDeclaration when the post-brace token is
  `*` (pointer decorator), not just identifier or `;`.
  Targeted regression: tests/teststructptrdecl.mad. Closes
  MadSMAUG act_info.c:2721 (whogr_s linked-list definition).

- ~~**`type const *p` interleaved qualifier+star chains**~~ — single
  qualifier-or-star loop in parseDeclaration absorbs both `const`/
  `restrict` qualifiers and `*` pointer decorators, in any order.
  Closes MadSMAUG act_info.c:3074 (`char const *class;`). Targeted
  regression: tests/testconstmid.mad.

- ~~**Compound-assign / inc-dec error diagnostics swept to Throw**~~
  — TokenAddEq/SubEq/MulEq/DivEq/ModEq/BSLEq/BSREq/BandEq/BorEq/
  XorEq and TokenInc/TokenDec converted from raw `throw "..."` to
  `pgm.Throw(this) << "..." << flush`. resolveCompoundLHS's
  unsupported-member / no-elem-type / non-Reg-non-Mem subscript
  base sites also converted (using Throw(left/tse)). 13 sites
  total; messages now carry the operator name and file:line.

### Session 2026-04-24 (post-v0.11.0)

- ~~**Address-taken pointer + `&ptr->member` returned garbage**~~ —
  `TokenMember::operand`'s Mem branch assumed `_obj` was the struct
  itself. For a stack-backed pointer variable, `_obj` was the Mem
  slot holding the pointer value, so `addOffset` walked inside the
  pointer's own storage. Fix: when `_obj.isMem()` AND
  `object.type->is_pointer()`, load the pointer value into a Gp
  first, then index off of it like the existing reg-resident branch.
  Targeted regression: `tests/testaddrtakenptrmember.mad`.

- ~~**`&class->member` failed when `class` was a C variable name**~~
  — unary-`&` handler required `ttIdentifier` at both the postfix-
  chain detector and the simple-ident fallthrough. Fix: use
  `is_contextual_identifier_token()` / `contextual_identifier_name()`
  (existing helpers used by the rest of the parser's keyword-as-
  identifier handling). Targeted regression: `tests/testaddrclass.mad`.
  Closes MadSMAUG tables.c:1861 (`&class->affected` inside
  fwrite_class).

- ~~**Function-like macro parameter substitution cascaded**~~ — a
  per-param sequential sweep let an argument that matched a later
  param name get re-substituted. `CREATE(type, NEWS_TYPE, 1)` where
  the user variable was named `type` (same as CREATE's second
  param) produced `((NEWS_TYPE) = (NEWS_TYPE *) calloc(...))`.
  Fix: single-pass walk over the original body, substituting each
  identifier via a param→arg map; substituted text is never re-
  scanned. Targeted regression: `tests/testmacrosubst.mad`.
  Advanced the MadSMAUG news.c / stances.c ingest by several
  hundred lines.

- ~~**`*(ptr + N)` / `*(ptr - N)` / `*(p = ptr + N)` rejected by
  unary-`*` with "cannot dereference non-pointer type"**~~ —
  `TokenAdd`, `TokenSub`, `TokenAssign` all left `_datatype` as
  `&ddINT` (TokenOperator default), so the expression advertised
  `int` even when operands were pointers. Fix: override `datadef()`
  on each to propagate the correct pointer type (`ptr ± int → ptr`,
  `ptr - ptr → ptrdiff_t`, `assign → LHS type`). Surfaced at
  MadSMAUG mud_prog.c:2552/2553. Targeted regression:
  `tests/testderefptrexpr.mad`.

- ~~**Function-like macros shadowing later definitions**~~ — lexer
  now suppresses function-like macro expansion when the preceding
  tokens form a declaration/definition head. Walks back through
  `tokens`, skipping pointer decorators (`*`), and treats the first
  non-`*` token as a type signal when it is a `ttDataType` keyword,
  `struct`/`class`/`enum`, or a storage-class / qualifier token
  (`const`/`extern`/`static`/`register`/`typedef`/`restrict`).
  Ordinary call sites preceded by `{` / `;` / `,` / operators still
  expand. Closes the MadSMAUG `#define bug(...) ((void)0)` →
  `void bug(const char *, ...)` collision cleanly; the umbrella's
  `#undef bug` workaround around the db.c include can now be
  removed. Regression: `tests/testmacrodefhead.mad` covering
  `void foo(...)`, `char *foo(...)`, and `static int foo(int)`
  definitions all coexisting with same-named function-like macros.

- ~~**`*(TYPE*)expr` failed when TYPE was a typedef'd user type**~~
  — the unary-`*` handler's `(` branch consumed the `(` and called
  `parseExpression` on the inner content, which bypassed the cast
  detection that normally runs on `(`. Typedef'd type names like
  `EXT_BV` then reached the variable-lookup path and failed with
  "use of undeclared identifier". The TODO had ascribed this to
  macro-expansion context, but isolated `(*(EXT_BV*)p).bits[0]`
  and `EXT_BV v = *(EXT_BV*)p;` had the same failure. Fix: peek
  inside the `(` — when the first token is a cast-signature head
  (`ttDataType`, `struct`/`class`, or a typedef'd identifier in
  `datatype_map`), delegate the whole `(...)` back to
  `parseExpression` so its cast detection handles it. Plain
  grouping (`*(a + b)`) is untouched. Regression:
  `tests/testderefcasttypedef.mad`. Closes the mud_prog.c front
  edge at line 1276 (`xIS_SET(*(EXT_BV*)vd->data, flag)`).

- ~~**Real `<` / `<=` / `>` / `>=` used setl/setle/setg/setge after
  ucomisd**~~ — x86 `ucomisd` writes CF/PF/ZF (unsigned-style flags),
  not SF/OF, so the signed setcc variants read garbage flags and
  produced wrong 0/1 results. Both `<` and `>` flipped whenever the
  true answer disagreed with a signed setcc on uninvolved SF/OF.
  `emit_compare` now treats reals like unsigned when picking setcc
  (`setb`/`setbe`/`seta`/`setae`). Targeted regression:
  `tests/testrealcmp.mad`. Both `<` and `>` now agree with C for
  all eight float/double combinations including Mem-backed literal
  RHS (e.g. `1.5 < 2.0`).

### Session 2026-04-24 (SMAUG Phase F front-edge resumption)

- ~~**`goto label;` + forward labels**~~ — function-scoped labels;
  Program::label_map (cleared on each function boundary) holds the
  asmjit::Labels. Forward references work via look-or-create. The
  map on Program (not TokenFunc) because adding an std::map to
  TokenFunc silently regressed downstream codegen through
  multi-inheritance vtable shift. Targeted regression:
  `tests/testgoto.mad`.

- ~~**Unary `-` in brace-init lists**~~ — `isPostfixPosition` now
  recognizes `{` / `(` / `,` / `;` / `=` as expression-opening
  positions where `-` stays unary (previously converted to binary
  subtraction, tripping "Missing operand"). Targeted regression:
  `tests/testnegbraceInit.mad`.

- ~~**`char[N] = "..."` with matching length skips null terminator**~~
  — explicit-size char arrays that exactly match the string literal
  length no longer append an implicit `'\0'` (C89 behavior).
  Inferred and oversized sizes still do. Targeted regression:
  `tests/testcharnoterm.mad`.

- ~~**Compound-assign on expression-base subscripts**~~ —
  `resolveCompoundLHS` gained a `TokenSubscriptExpr` path mirroring
  the TokenAssign write branch (operand-not-compile on base, LEA vs
  MOV via pointer-check, `imul` fallback for non-power-of-2 element
  sizes, `load_mem_to_gpq` for the old-value load). SMAUG's
  `xREMOVE_BIT` / `xSET_BIT` macros expand to this form. Targeted
  regression: `tests/testcompoundsubexpr.mad`. Advances MadSMAUG
  umbrella to "compiles + runs end-to-end" (stub main).

- ~~**`return X;` mis-detected as multi-return**~~ — `TokenRETURN`'s
  multi-return heuristic now consults the consumed stop token via
  `Program::curToken()` (new accessor) and requires `,` specifically
  before considering a second expression. Previously peeked the next
  token and couldn't disambiguate an identifier second-return-expr
  from the start of the next statement. Targeted regression:
  `tests/testreturnnextident.mad`. Also improved error messages in
  `resolveCompoundLHS` and `TokenStmt::compile` default branch
  (both were throwing dangling `c_str()` / `this` pointers, garbling
  the error output).

- ~~**`class` as a plain C identifier**~~ — parseStatement routes
  `class` through parseExpression when it's not followed by an
  identifier or `{` (real class-declaration head); parsePostfixChain
  accepts `class` after `.` / `->` via `is_contextual_identifier_token`
  (which was already set up to accept tkCLASS and the STL keywords —
  just needed to be plugged into the postfix-chain member-name parse).
  Targeted regression: `tests/testclassident.mad`. Advances MadSMAUG
  through skills.c.

- ~~**`safemov(Operand, double, ...)` truncating to int for Mem
  destinations**~~ — when TokenOperator::optimize constant-folded
  a real expression (`double d = 1.0 + 0.5;`) and the caller's
  destination was a Mem, the double-valued fallback called
  `imm((int)d)` and silently dropped the fractional part. Now takes
  the Xmm-through-constpool path when `d1->is_real()`. Regression:
  `tests/testrealconstfold.mad`.

- ~~**Struct-array subscript element stride + base addressing**~~ —
  TokenSubscriptExpr and the TokenAssign write branch now fold
  non-power-of-2 element sizes into the index via `imul`, and both
  sites use `base_expr->operand()` rather than `compile()` to avoid
  the emit_ir_value load-first-element path for numeric-typed
  TokenMember bases. `struct { int bits[N]; }` reads / writes work,
  nested struct arrays like `o.arr[i].member` work with correct
  per-element strides. Targeted regression: `tests/teststructarrsub.mad`.

- ~~**`expr[i].member` parses + compiles for struct element types**~~
  — dot handler's ttSubscript branch now detects TokenSubscriptExpr
  explicitly (the NULL dynamic_cast to TokenSubscript used to
  segfault). TokenSubscriptExpr::compile returns raw element Mem for
  struct/class element types so the TokenMember parent-expr path can
  add the member offset without emit_ir_value corrupting the Mem.
  Advances MadSMAUG from handler.c:4789 to an undefined-function
  front edge in fight.c (`learn_from_failure`). Targeted regression:
  `tests/testsubscriptexprmember.mad`.

- ~~**Leading-dot float literal `.4`**~~ — lexer's single-`.` case now
  peeks for a digit and parses the fractional expansion (consuming
  the optional `f/F/l/L` suffix) before falling back to TokenDot.
  Advances MadSMAUG from handler.c:4683 to handler.c:4789. Targeted
  regression: `tests/testleadingdotfloat.mad`.

- ~~**`(*p).member` now parses as `p->member`**~~ — the dot handler in
  parseExpression cast `lhs_dot` to `TokenMember *` unconditionally,
  but `TokenDeref` and `TokenDerefExpr` both report `ttMember` without
  actually deriving from `TokenMember`. Added explicit branches:
  TokenDeref routes tv_var through the underlying pointer variable
  (no parent_expr — TokenMember's voperand pointer-in-Gp path compiles
  it as `[ptr+offset]`); TokenDerefExpr synthesizes a struct-typed
  object variable and passes the TokenDerefExpr as parent_expr (the
  struct-value "dot chain" branch then handles `[ptr+offset]`).
  Exposed by SMAUG's `xIS_SET((var), bit)` macro after `*vector`
  substitution — `vector` is also a madc keyword (STL container
  reserve), so the unary-`*` handler hands a keyword rather than an
  identifier and falls into the TokenDerefExpr path. Advances the
  MadSMAUG umbrella from handler.c:2989 to handler.c:4683. Targeted
  regression: `tests/testparenderefmember.mad`.

- ~~**Struct-copy initialization and assignment**~~ — `struct S a = other;`
  (decl init) and `a = other;` (plain assign) now emit
  `memcpy(&a, &other, sizeof(S))`. Parser pushes `=` back and falls
  through to the normal init path when the struct RHS isn't `{`;
  `TokenAssign::compile` gained a btStruct/btClass branch that LEAs
  both sides and invokes libc `memcpy`. Handles both `struct S b = a;`
  and `struct Inner extra = p->member;` (where TokenMember::operand
  LEAs the member into a Gp for non-numeric members). Targeted
  regression: `tests/teststructcopy.mad`. Advances MadSMAUG from
  handler.c:1284 to handler.c:2990 (next front edge is the parser
  crash on `(*p).member`).

- ~~**`#include` canonicalization via realpath**~~ — `should_tokenize_include`
  now canonicalizes each resolved quoted-include path through `realpath()`
  before the include-once check. The MadSMAUG umbrella was tripping on
  `mud.h` being pulled in both as `upstream_src/mud.h` (from SMAUG.mad)
  and bare `mud.h` (from ident.c / interp.c / ibuild.c): two distinct
  keys in the seen-files map even though both resolve — via symlinks —
  to the same underlying file. Embedded-header keys starting with `<`
  bypass realpath entirely.

- ~~**`*p++ = rhs` / `*p-- = rhs` as a write target**~~ — the read side
  (`c = *p++`) already compiled through TokenDerefStep, but the write
  side threw `Assignment on a non-variable lval` because
  TokenAssign::compile only dispatched TokenVar / TokenDeref /
  TokenDerefExpr / TokenMember / TokenSubscript* LHS kinds. Added a
  TokenDerefStep LHS branch that mirrors the read: capture `old_ptr = ptr`,
  step the pointer variable, then expose `[old_ptr]` as the Mem lval
  the numeric-assignment path writes into. Closes the MadSMAUG umbrella
  front edge in `act_move.c:182` (`grab_word`). Targeted regression:
  `tests/testderefpostincstore.mad`.

### Session 2026-04-23

- ~~**`->` after expression parents (call / subscript / cast / deref-expr)**~~ —
  `TokenMember::operand()`'s chained-arrow path previously called
  `parent_expr->operand()` unconditionally, which works for `TokenMember`
  parents (they re-materialize their own address each call) but silently
  failed for `TokenCallFunc` / `TokenCallMethod` / `TokenSubscript` /
  `TokenDerefExpr`, whose `operand()` returns a fresh register without
  emitting the computation. Now compiles non-`ttMember` parent expressions,
  so `get_slot(i)->value` and the chained call-ptr dispatch patterns emit
  the producing computation before dereferencing. Targeted regression:
  `tests/testglobalptrarrayarrow.mad`.

- ~~**Mem-backed arithmetic expression codegen**~~ — plain arithmetic /
  bitwise operators now compute through a temp register when the caller's
  destination is Mem, then mirror the result back; compound assignments now
  do the same for stack-backed variables. This closes SMAUG front edges in
  `bet.h` / `hashstr.c` such as `number = (number * 10) + ...`,
  `number *= (multiplier = 1000)`, and `hash = len % STR_HASH_SIZE`.
  Targeted regressions: `tests/testassignexprmem.mad`,
  `tests/testcompoundassignmem.mad`.

- ~~**Unary `*` on fixed arrays (`*arg`, `!*buf`)**~~ — the parser now
  accepts fixed arrays in unary dereference contexts via normal C
  array-to-pointer decay. This advances the MadSMAUG umbrella past
  `interp.c`'s `if ( !*arg )` in `do_timecmd`. Targeted regression:
  `tests/testderefarray.mad`.

- ~~**`struct servent` interop in embedded `<netdb.h>`**~~ — 32-byte
  glibc-matching layout (`char *s_name; char **s_aliases; int s_port;
  char *s_proto;`). Closes the MadSMAUG umbrella bootstrap's
  `sock.sin_port = serv->s_port;` front edge in upstream `ident.c`.
  `tests/testservent.mad` drives `getservbyname("ftp","tcp")` and
  `getservbyname("http","tcp")` end-to-end and asserts
  `sizeof(struct servent) == 32`.

- ~~**Extended `<errno.h>` socket/network constant coverage**~~ —
  added `EWOULDBLOCK` (= `EAGAIN`), `EINPROGRESS`, `EALREADY`,
  `ENOTSOCK`, `EDESTADDRREQ`, `EMSGSIZE`, `EPROTOTYPE`, `ENOPROTOOPT`,
  `EPROTONOSUPPORT`, `ESOCKTNOSUPPORT`, `EOPNOTSUPP`, `EPFNOSUPPORT`,
  `EAFNOSUPPORT`, `EADDRINUSE`, `EADDRNOTAVAIL`, `ENETDOWN`,
  `ENETUNREACH`, `ENETRESET`, `ECONNABORTED`, `ECONNRESET`, `ENOBUFS`,
  `EISCONN`, `ENOTCONN`, `ESHUTDOWN`, `ETIMEDOUT`, `ECONNREFUSED`,
  `EHOSTDOWN`, `EHOSTUNREACH`, plus the System V / extended POSIX
  errors (`EDEADLK`, `ENAMETOOLONG`, `ENOLCK`, `ENOSYS`, `ENOTEMPTY`,
  `ELOOP`, `EDOM`, `EILSEQ`, `EOVERFLOW`, `ENODATA`, `ETXTBSY`,
  `EUSERS`, `EDQUOT`, `ESTALE`, `ENOMSG`). Closes the MadSMAUG umbrella
  bootstrap's `errno != EINPROGRESS` front edge in upstream `ident.c`.

- ~~**Extended `<fcntl.h>` constant coverage for `fcntl()`**~~ — embedded
  `<fcntl.h>` now defines the `F_DUPFD`/`F_GETFD`/`F_SETFD`/`F_GETFL`/
  `F_SETFL`/`F_GETLK`/`F_SETLK`/`F_SETLKW`/`F_SETOWN`/`F_GETOWN`/
  `F_DUPFD_CLOEXEC` command constants and the adjacent `FD_CLOEXEC`,
  `O_NDELAY`, `O_ASYNC`, `O_DIRECTORY`, `O_NOFOLLOW` flags at their
  Linux x86-64 values. This closes the MadSMAUG umbrella `F_SETFL` front
  edge in upstream `ident.c`'s `fcntl(..., F_SETFL, FNDELAY)` path.
  `tests/testfcntl.mad` runs an end-to-end `F_GETFL` / `F_SETFL` round
  trip against a real file descriptor to prove nonblocking mode is
  actually set.

- ~~**Control-flow paren helper + assignment-expression regressions**~~ —
  control-flow condition parsing now goes through a reusable parenthesized
  expression helper instead of per-keyword ad hoc handling, which fixed
  `_Bool` single-statement `if (...) stmt; else stmt;` parsing/branching.
  Function-call RHS assignment now also returns into Mem-backed lvalues
  correctly, so assignment-in-condition loops work again with stack-backed
  locals. Direct regressions now pass: `tests/testc23_bool.mad`,
  `tests/testpostfix.mad`, and `tests/testassigninexpr.mad`.

- ~~**Pointer-return typing for dereferenced call results**~~ —
  `addFunction()` now resolves generic `rtPtr(...)` signatures through a
  helper instead of a few hard-coded pointer cases, so builtin/external
  functions like `__errno_location()` retain typed pointer returns during
  parse. The unary `*` path also now treats identifier-followed-by-`(` as a
  full call expression, which fixes `*get_msg()`, `*(version.c_str())`, and
  `errno` / `*(__errno_location())` parsing. Targeted regressions:
  `tests/test_ptr_fn_deref.mad`, `tests/test_get_argv_deref.mad`,
  `tests/test_errno_deref.mad`.

- **Next MadSMAUG front edge** — full `/workspace/MadSMAUG/src/SMAUG.mad`
  now advances to undeclared `timerisset` at `interp.c` / `do_timecmd`
  (`/workspace/MadSMAUG/src/SMAUG.mad:1257:18`). Likely next work is
  embedded timeval helper macro coverage.

### Session 2026-04-18

- ~~**Assignment as expression inside declaration init**~~ — declaration
  initializers like `int y = (x = 42);` now preserve the outer assignment to
  the declared variable instead of collapsing to the inner assignment only.
  The parser keeps the original assignment-context parse for normal
  initializers, then wraps the result only when the parsed initializer does
  not already assign to the declared variable. Brace initializers stay on the
  existing `init_list` path. `tests/testdeclassignexpr.mad` covers nested
  assignment in declaration init, expression use, and comma declarations.
- ~~**Right-shift operator `>>` integration coverage**~~ — `tests/testbsl.mad`
  now exercises plain arithmetic right shift in addition to the existing
  left-shift cases, and `tests/testbsl.expect` asserts the concrete outputs.
  This closes the lingering gap where `>>=` and `cin >>` were covered but
  plain bitwise `>>` had no integration assertion.
- ~~**`cc.newXxx(name)` percent-safety sweep**~~ — verified that the
  user-derived register names now flow through `"%s"` call sites or
  `DataDef::newreg()`, which already applies the safe formatting wrapper in
  `include/datadef.h`. The remaining named temporaries in `typesafe.cpp` and
  lambda-related paths are fixed literals, not user-controlled format
  strings, so the old TODO entry was stale rather than unfinished.
- ~~**Typed for-init with comma**~~ — `for (int i = 0, j = 10; ...)` now
  works by reusing `parseDeclaration()` for the typed `for` initializer and
  feeding any synthetic comma-continuation declarations into
  `TokenFOR::init_extras`. `tests/testfortypedcomma.mad` covers plain scalar
  declarations, three-variable init, and mixed pointer/scalar init in the
  same typed `for` header.
- **Error diagnostics narrowed** — `Program::compile()` now prints file/line
  context for raw-string compiler failures by anchoring them to the current
  statement token (and the current pre-pass token during FuncNode setup),
  instead of emitting location-less `: error:` lines. Full closure still
  requires replacing deeper raw `throw "..."` sites with token-aware
  diagnostics where available.

- ~~**Function pointer typedefs**~~ — `typedef void DO_FUN(CHAR_DATA *ch, char
  *argument);` (Form 1, SMAUG idiom) and `typedef int (*UNOP)(int);` (Form 2,
  classic C) both register a `DataDefFPTR` in `datatype_map`. Declarations like
  `DO_FUN *cmd;` and `UNOP u;` create function-pointer variables; the `*` on
  Form 1 uses is a no-op because the typedef already names a pointer-like
  storage. Assignment uses C's function-to-pointer decay: `fn = func_name;`
  takes the function's address (previously this mis-parsed as a no-arg call to
  `func_name`). Decay widens to any value-context follower — assignment
  operators (`=`, `+=`, `<<=` etc.), and struct/array-init / call-arg / ternary
  separators (`,`, `}`, `)`, `]`, `:`) — so SMAUG's command-table pattern
  `struct cmd c = { "who", do_who };` works. `cout << endl;` still parses as
  BSL-consumed call because `endl;` has neither an assignment operator on top
  of opStack nor a value-end follower. `tests/testfnptrtypedef.mad` covers
  typedefs + reassignment + invocation. `tests/testfnptrstruct.mad` covers
  the SMAUG command-table pattern.
- ~~**char* coercion in function-pointer indirect calls**~~ — `TokenCallFunc`'s
  fptr path now runs the same `string_cstr` coercion the direct-call path
  does, so `fp("world")` where `fp` is declared `void STRFN(char *)` passes
  the string-literal's `.c_str()` instead of the `std::string*` pointer.
- ~~**Direct invocation through struct-member function pointer**~~ — `c.fn(args)`
  now parses and runs. When the parser sees `(` after a `TokenMember` whose
  datadef is `DataDefFPTR`, it builds a `TokenCallFunc` whose new `src_node`
  field points at the member. At compile time the fptr-call path compiles
  `src_node` to materialise the function-pointer value, instead of looking
  it up from a variable. Also fixes struct-body parsing so `DO_FUN *fn;`
  inside a struct stays a `DataDefFPTR` member (previously got wrapped in
  an extra `DataDefPTR`, which defeated fptr dispatch). `tests/testfnptrmember.mad`
  covers direct invocation.
- ~~**Global function-pointer initialization**~~ — `DO_FUN *g = do_who;` at
  file scope, and `struct cmd tab[] = { {"who", do_who}, ... };` (SMAUG-style
  command tables), now compile and run correctly. Root cause was two-fold:
  (a) `Variable` constructor skipped heap allocation for any `btFunct` type,
  including `DataDefFPTR` which IS storage; (b) user functions compiled AFTER
  `TokenProgram` processed globals, so LEA of the target function's label
  emitted nothing (funcnode was still NULL). Fixes: allocate storage for
  `DataDefFPTR` globals (size 8), and add a pre-pass in `Program::compile`
  that creates a FuncNode label for every user function before globals
  compile, via `TokenFunc::prepareFuncNode` (factored out of
  `TokenFunc::compile`). A new `pending_funcs` vector on `Program` tracks
  user functions + lambdas in source order. Also excludes `DataDefFPTR`
  variables from the existing function-global-x86code backfill loop to
  avoid mistreating their 8-byte data as a `Method*`.
  `tests/testfnptrglobal.mad` covers global fn-ptr + SMAUG command-table
  dispatch.
- ~~**Assignment as a value in enclosing expressions**~~ — `while ((entry =
  readdir(d)) != NULL)`, `if ((n = get()) > 0)`, `y = (x = 42)` now evaluate
  the assignment as an expression that returns the assigned value. Root
  cause: `TokenAssign::compile`'s numeric-assign path wrote the RHS into
  the LHS storage, restored the caller's pre-existing `regdp.first`, and
  returned it — but never mirrored the assigned value into that
  caller-provided destination. Now when the caller's `regdp.first` is a
  distinct Gp (the enclosing comparison / containing-assignment passed
  its own accumulator), copy `_operand` into it via `safemov` before
  returning. Unlocks the standard SMAUG readdir / accept / recv
  assign-in-condition idioms. `tests/testassigninexpr.mad` covers
  while-condition, if-condition, chained assign, and paired assign.
- ~~**`struct sockaddr` + `struct sockaddr_in` + `struct in_addr` interop**~~ —
  `sys/socket.h` adds the 16-byte generic `struct sockaddr`; `netinet/in.h`
  adds the 16-byte `struct sockaddr_in` and 4-byte `struct in_addr` with
  glibc-matching layouts. Also `sa_family_t`, `in_port_t`, `in_addr_t`,
  `socklen_t` type aliases. Real socket bind on loopback works via
  `bind(s, (struct sockaddr *)&addr, sizeof(addr))`. `tests/testsockaddr.mad`
  opens, binds, closes a TCP loopback socket.
- ~~**`struct dirent` interop**~~ — `dirent.h` carries the 280-byte glibc
  layout. `opendir()` / `readdir()` / `closedir()` via dlsym; user code
  iterates and inspects `entry->d_type` / `entry->d_ino`.
  `tests/testdirent.mad` scans `tests/` and counts DT_REG / DT_DIR entries.
- ~~**Fixed-size array members in struct bodies**~~ — `char buf[N]`,
  `int m[N][M]` inside a struct now parses. The parser collects the
  dimension(s) after the member identifier, multiplies them, and calls
  `addMember(name, type, count)` with the product. The member reserves
  `count * sizeof(base)` bytes inline; access the buffer's starting
  pointer via `&obj.buf`. Needed for `struct dirent::d_name[256]` and
  SMAUG's many fixed char buffers.
- ~~**Unary `&` / `*` after a cast**~~ — `(struct sockaddr *)&addr` and
  `(int *)*ptr` now parse. The cast block nulls `_prv_token` after
  consuming its closing `)` so `isUnaryPosition` sees the cast-body's
  head token in a unary context; otherwise the close-paren leaked
  through and `&` / `*` mis-parsed as binary operators.
- ~~**`struct stat` + `struct timespec` interop**~~ — `sys/stat.h` now embeds
  the glibc x86-64 `struct stat` layout (144 bytes) with natural C ABI
  alignment plus the `struct timespec` (16 bytes) used by its `st_atim` /
  `st_mtim` / `st_ctim` triplet. Type aliases (`mode_t`, `uid_t`, `off_t`,
  etc.) land as `#define`s that expand to the concrete madc primitive types.
  File-type predicate macros (`S_ISREG`, `S_ISDIR`, `S_ISLNK`, `S_ISBLK`,
  `S_ISCHR`, `S_ISFIFO`, `S_ISSOCK`) added as function-like macros.
  `st_atime` / `st_mtime` / `st_ctime` aliases resolve to
  `st_Xtim.tv_sec` via object-like macros, matching glibc. `stat()`,
  `fstat()`, `lstat()`, `chmod()`, `mkdir()`, `mkfifo()` resolve through
  the existing dlsym fallback. `tests/teststat.mad` drives real `stat()`
  on a regular file, a directory, a missing path, and checks
  `st_mtime > 0`.
- ~~**Reassigning a struct's function-pointer member**~~ — `c.fn = other_fn;`
  now works. `TokenVar::compile` for a function identifier assumed the
  assignment destination was a Gp register; for struct-member LHS the
  destination is a Mem. Now LEAs the function address into a tmp Gp and
  stores to the Mem when `regdp.first->isMem()`. Also returns the tmp
  unchanged for no-dest callers. `tests/testfnptrreassign.mad` covers
  member reassignment and reassignment through a local fn-ptr variable.
- ~~**`sizeof(object)` for variables and fixed arrays**~~ — the parser now
  resolves `sizeof(identifier)` through normal variable lookup before falling
  back to type lookup, so local scalars and fixed arrays like `char buf[32];`
  return the right compile-time size. `tests/testsizeof.mad` now covers
  `sizeof(scalar)` and `sizeof(buf)`.
- ~~**First real SMAUG source compatibility test (`requests.c`)**~~ — added
  `tests/testsmaug_requests.mad`, which exercises the upstream
  `requests.c` body against a minimal SMAUG shim header. This flushed out the
  `sizeof(buf)` parser gap and the missing opaque `FILE` alias in embedded
  `<stdio.h>`, both now fixed enough for the test to compile and run.
- ~~**printf with `%f`/`%e`/`%g` for doubles**~~ — verified the existing
  variadic call path handles double arguments correctly for both direct libc
  `printf` and a user-defined `...` wrapper around `vsprintf`. Mixed
  string/int/double calls and repeated double arguments all format correctly;
  no compiler change was required. `tests/testprintfdouble.mad` covers `%f`,
  `%e`, `%g`, mixed scalar calls, and multiple doubles per call.
- ~~**`->` member access in certain printf arg combinations**~~ — the parser's
  early comma-count guard in `parseCallFunc()` / `parseCallMethod()` now skips
  "too many parameters" rejection for declared varargs targets, so wrapper
  calls like `wrapper("%s %ld", ch->name, ch->in_room->vnum, ...)` compile
  correctly even when the extra arguments are `->` member expressions or
  macro-expanded member expressions. `tests/testprintfmember.mad` covers plain
  libc `printf`, macro-expanded nested members, and a user-declared `...`
  wrapper around `vsprintf`.
- ~~**`cout << [const char*]` in chained expressions**~~ — `TokenBSL::compile()`
  now routes `dtCHARptr` through `streamout_cstr` before the generic numeric
  stream path, so chained forms like `cout << ident(msg) << endl` and
  `"prefix: " << ident(msg)` no longer fall into the unsupported numeric
  switch. `tests/testcoutcstr.mad` covers plain `char*`, function-returned
  `char*`, and mixed string-prefix chains.
- ~~**`*ptr` dereference on stack variables**~~ — address-taken numeric locals and
  parameters now spill to stable stack slots instead of living only in virtual
  registers. `TokenAddrOf` marks variables as address-taken during parse,
  `TokenCpnd::voperand()` allocates a typed `cc.newStack()` slot for those
  numerics, and `TokenFunc::compile()` spills incoming numeric parameters into
  that slot before the body runs. `tests/testptr.mad` now covers both
  `int n; int *p = &n; *p = ...;` and `int *p = &param;`.

### Session 2026-04-17 (Phase F continues — hashstr.mad runs)

- ~~**Inc/dec on struct members**~~ — `++ptr->links`, `ptr->field--` etc.
  TokenInc / TokenDec extended to `ttMember` (via `->` and `.`) and
  `*deref` via `resolveCompoundLHS`. Shares the load-op-store pattern
  with the compound-assignment operators (e8c3f0b).
- ~~**`c++` / `--x` in compound-comma increment contexts**~~ — same
  underlying fix; SMAUG's `for (ptr = head, c = 0; ptr; ptr = ptr->next,
  c++)` pattern now compiles and runs (e8c3f0b).
- ~~**`char *p; p = "literal";`**~~ — post-decl string-literal
  assignment. TokenAssign detects char* ← dtSTRING and routes through
  `string_cstr` to pull the literal's data pointer; previously wrote
  the std::string operand verbatim so reads printed garbage (acfc8b1).
- ~~**Unsigned comparison ops**~~ — `TokenLT`/`LE`/`GT`/`GE` pick
  setb/setbe/seta/setae when either operand is unsigned. Without this,
  `if (ptr->links < 65535) ++ptr->links;` with unsigned short skipped
  the then-branch because 65535 sign-interprets as -1 (e8c3f0b).
- ~~**Global pointer variable read/write**~~ — `DataDefPTR` overrides
  `movrval2mptr`/`movrval2rptr`/`movint2rptr`/`movmptr2rval` with
  qword semantics. Base-class switch on DataType fell through to the
  unhandled default for rtPtr() values (>= 10000), so `global_ptr = x;`
  was a silent no-op (e8c3f0b).
- ~~**Subscript → member assign**~~ — `p->next = arr[i];` now writes.
  `TokenSubscript::compile` respects a Mem destination by loading
  into a temp and storing; previously overwrote regdp.first with its
  own fresh Gp and abandoned the member's Mem (e8c3f0b).
- ~~**Sub-qword sign/zero-extension in `resolveCompoundLHS`**~~ —
  loading a word-sized member into a Gp64 for arithmetic now uses
  movzx/movsx/movsxd. Plain `cc.mov(gpq, word_ptr)` is invalid x86 and
  asmjit silently emitted truncated ops or dropped them (e8c3f0b).
- ~~**`safemov(Mem, Gp)` size mismatch**~~ — picks r8/r16/r32/r64
  register view based on Mem size, not Gp size. Without this, writing
  a Gp64 (widened member_lhs) to a word-sized member silently dropped
  (e8c3f0b).
- ~~**Parser: comma peek-stop in conditional mode**~~ — nested
  parseExpression (in the cast-body handler) used to consume the `,`
  that terminated the outer function-call arg, so `strcpy((char *)h +
  8, "x")` parsed as one arg (e8c3f0b).
- ~~**`TokenIF::compile` regdp reset**~~ — zeroes regdp before
  condition, then branch, and else branch, matching the regdp-reset
  rule already applied to TokenFOR/WHILE/DO (e8c3f0b).

### Session 2026-04-17 (Phase E finish + Phase F start)

- ~~**`__FILE__` and `__LINE__` macros**~~ — lexer expands to quoted filename and
  `source.line()`; works correctly inside `#define` bodies (each invocation captures the
  call site) (9e2d5ad)
- ~~**`cc.newXxx(name)` format-safety sweep**~~ — 32 direct call sites in compiler.cpp
  routed through `"%s"` format; `DataDef::newreg` already done earlier (c50acbe)
- ~~**Struct interop for libc types**~~ — `struct tm` (56 bytes), `struct timeval`
  (16 bytes), `struct fd_set` (128 bytes) with glibc-x86-64-matching layouts. FD_ZERO /
  FD_SET / FD_CLR / FD_ISSET macros forward to `__madc_fd_*` helpers. `select()` works
  end-to-end with a real pipe. Fixed `safemov(Gp, Mem)` to sign/zero-extend sub-qword
  loads (int32 struct members previously read 8 bytes) (2f08efd)
- ~~**Raw-pointer subscript `ptr[i]`**~~ — for `int *`, `char *`, etc., emit `[ptr + i *
  sizeof(base)]` with SIB scaling. Also fixed `safemov(Operand, Operand)` Gp←Mem path to
  forward to the size-aware variant (189f4ae)
- ~~**App-named bootstrap convention**~~ — documented in README: use `smaug.mad` /
  `myapp.mad` at project root, `#include`ing sources in dependency order with main() last
  (ac0cf4f)
- ~~**Self-referencing structs**~~ — `struct X { struct X *next; ... };` works by pre-
  registering the tag in struct_map before body parse (54087c2)
- ~~**Three-word compound types**~~ — `unsigned short int`, `signed long int`, `long long
  int`, `unsigned long long`, etc. (54087c2)
- ~~**`void` as sole parameter**~~ — `int f(void)` now parses as zero-arg (54087c2)
- ~~**Global fixed-size arrays**~~ — `struct X *arr[N]` at file/static-local scope;
  heap-allocated calloc'd backing storage, voperand loads absolute address (35c6bf0)
- ~~**Multi-variable declarations**~~ — `int a, b, c;`, `char *p, *q;`, `int x = 1, y =
  2;`. Push-back synthetic base-type token so parseCompound iterates naturally. Base type
  preserved for per-var `*` decorators (35c6bf0)
- ~~**stdin / stdout / stderr**~~ — lazy-registered in `<stdio.h>` as int64 globals
  initialized from `*dlsym("stderr")` etc.; returns the FILE* for fprintf (35c6bf0)
- ~~**`register struct X *p;`**~~ — register now accepts struct/typedef tokens via the
  same keyword-delegation path as static (35c6bf0)
- ~~**Unary minus after a keyword**~~ — `return -1;`, `if (-x > 0)` no longer misfires as
  "Missing operand". isUnaryPosition treats keywords as unary positions; isPostfixPosition
  excludes them (35c6bf0)
- ~~**TokenRETURN multi-return detection tightened**~~ — keywords and types excluded from
  the detection peek so `return X;` followed by `if(...)` / `ident()` doesn't misfire
  (35c6bf0)
- ~~**For-loop comma expressions**~~ — `for (a=0, b=1; cond; i++, j--)` — TokenFOR has
  init_extras / incr_extras vectors parsed via conditional parseExpression, compiled in
  order (be6c359)
- ~~**Forward decl + definition param mismatch**~~ — `FuncDef::findParameter` compares
  against DataDef name, so every definition-pass re-pushed all params. Track
  func_already_declared; skip re-push on the 2nd pass (be6c359)
- ~~**Compile-time error handler robustness**~~ — catch(const char*) block no longer
  crashes on NULL err_msg or dangling tb (be6c359)

### Session 2026-04-16 (Phase E underway)

- ~~**Chained `->` and `.` member access**~~ — `a->b->c`, `a->b.c`, `a.b.c`; LHS can be
  TokenVar, TokenMember, or TokenSubscript (b3a9d9a)
- ~~**C fixed-size arrays (1D)**~~ — `int arr[N]`, stack allocation + subscript
  `[base+idx*elem_size]` via SIB; honors regdp.first destination so it works as RHS of
  assignment (fd98935)
- ~~**Multi-dimensional fixed arrays**~~ — `int m[N][M]`, `int cube[N][M][K]`; extra_indices
  vector folded into linear offset (26dca0e)
- ~~**Brace initializer lists for arrays**~~ — `int a[] = {1,2,3}`, size inference, partial
  fill (rest zero), expressions as values (a1774d0)
- ~~**String-literal char-array init**~~ — `char msg[] = "hello"` expands to per-byte init
  list plus null terminator (1bae4f4)
- ~~**`char *msg = "literal"` sugar**~~ — routed through same path as `char msg[]` so the
  two forms produce identical storage (13bbc35)
- ~~**Struct initializer lists**~~ — `struct X v = { s0, s1 }` with scalars, pointers,
  char* (string_cstr coercion), std::string (string_assign); partial fill zero-fills
  remaining numeric/pointer members (8df2dca)
- ~~**Array-of-structs initializer**~~ — `struct Entry tab[] = {{"a",1}, {"b",2}}`; new
  TokenStructLit node for nested braces; TokenSubscript returns a Gp pointer for struct
  elements so `tab[i].name` reaches the member via TokenMember's Gp-base dot-chain path
  (e62d2e7)
- ~~**Crash handler with backtrace**~~ — SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL caught at
  startup, prints signal name + fault address + backtrace to stderr, then re-raises so
  the shell still sees the real exit status (308b622)
- ~~**Remove mis-firing strlen builtin; add `.length()`/`.size()`**~~ — strlen resolves
  via dlsym to libc (correct for char*/char[]); std::string exposes length/size as
  instance methods (f04b7b6)
- ~~**DataDef::newreg format safety**~~ — variable names with `%` (our `__literal__foo`
  literal variable names embedded printf format specs) crashed asmjit's variadic
  `newGpq(name)`; fixed by passing `"%s"` as the format (e62d2e7)
- ~~**emit_struct_init no longer mutates shared Operand**~~ — the coercion of a
  std::string literal to char* was reassigning through the reference returned by
  compile(), which aliased the cached Operand in operand_map for global literals;
  subsequent uses of the same literal in another struct init re-coerced the already
  char-pointer, causing "alice" SSO bytes to be interpreted as a pointer
  (0x6563696c61) and crash in printf. Now uses a local Operand. (e62d2e7)

### Prior

- ~~Escape sequences in string literals~~ (c90acff)
- ~~`[]` subscript operator~~ (c90acff)
- ~~`:=` short variable declaration~~ (7027d4c)
- ~~`[&]` lambda capture by reference~~ (4fa5126)
- ~~`madc::` namespace~~ — `madc::array` + regex functions
- ~~`std::` namespace scoping~~ — `std::vector<T>`, `std::map<K,V>`, `std::set<T>`, `std::list<T>`, `std::cin`
- ~~Register-only iterator~~ — numeric foreach element variables use `vfREGISTER`
- ~~Fix asmjit deprecation warnings~~ — ~70 call sites migrated
- ~~`switch` statement~~ — C-style with fall-through and break (deb3578)
- ~~`>>` input operator / `cin`~~ — reads string, int, double from stdin (1feb665)
- ~~Class methods~~ — hidden `__this` pointer, member access, name mangling (f375944)
- ~~Regex support~~ — `madc::regex_match/search/replace`, upgraded `perl::grep/split` (2aabea0)
- ~~Multiple return values~~ — `q, r := divide(17, 5)` with hidden `__retbuf` (b1c0e86, 8d07f44)
- ~~Ternary operator~~ — `condition ? true_expr : false_expr` with stack-slot merge (f64cf18)
- ~~Multi-return conditional crash~~ — skip cleanup for multi-return paths (8d07f44)
- ~~Compound assignment operators~~ — `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- ~~For-loop regdp clobber bug~~ — `TokenFOR::compile()` resets `regdp` before each sub-compilation
- ~~Hex integer literals~~ — `0xFF`, `0xDEAD`, `0X1A` etc.
- ~~Postfix increment/decrement~~ — `x++`, `x--` with correct old-value-return semantics
- ~~C preprocessor directives~~ — `#define`, `#undef`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`
- ~~Embedded header infrastructure~~ — `#include <name>` routes to embedded headers baked into binary; subdirectory support via `find` + relative-path keys
- ~~dlsym fallback for libc~~ — unresolved function calls try `dlsym(RTLD_DEFAULT)` before erroring
- ~~Variadic dlsym calling convention~~ — dedicated compile path handles int, double, string args
- ~~`#include <iostream>`~~ — cout/cin/cerr/endl gated behind include (lazy registration)
- ~~`#include <math.h>`~~ — M_PI/M_E/M_SQRT2/INFINITY + libm auto-load
- ~~`#include <stdio.h>`~~ — EOF/SEEK_*/NULL + printf/sprintf/snprintf via dlsym
- ~~Lazy symbol registration~~ — `lazy_map` defers addGlobal/addFunction to first use; supports variables, functions, types, structs
- ~~Command line arguments~~ — `int main(int argc, char **argv)` with `get_argv(argv, i)`
- ~~`cout << func()` crash fix~~ — BSL only injects ostream for ostream-consuming functions
- ~~`RTLD_GLOBAL` for `#load`~~ — loaded library symbols visible via dlsym without namespace prefix
- ~~31 additional embedded POSIX/libc headers~~ — 38 total (c971eb1, ea06f5a)
- ~~int64_t integer literal storage~~ — lexer/token widened to int64_t (c971eb1)
- ~~safeneg truncation fix~~ — removed spurious movsx after cc.neg() (c971eb1)
- ~~SMAUG 1.8 requirements analysis~~ — gap analysis in docs/SMAUG_requirements.md (bd9844c)
- ~~**`char *` pointer declarations**~~ — DataDefPTR, pointer parsing in decl/struct/params/sizeof (a3576bc)
- ~~**`->` struct pointer member access**~~ — reuses TokenMember Gp path (683f379)
- ~~**`&` address-of operator**~~ — TokenAddrOf with LEA, unary detection (8b0f8e2)
- ~~**`(TYPE *)` cast expressions**~~ — TokenCast, cast detection in parseExpression (31e5fbd)
- ~~**Forward typedef struct declarations**~~ — placeholder struct filled in-place (e8e6379)
- ~~**Function-like macros**~~ — #define NAME(params) body with substitution (680d982)
- ~~**Multi-line #define with \\ continuation**~~ — both function-like and object-like (680d982)
- ~~**do { } while(0) macro bodies**~~ — regdp reset + TokenInt fix (d15f01c)
- ~~**Ternary inside parentheses**~~ — brackets check before done=true (70425d7)
- ~~**unsigned/signed/long/short compound types**~~ — lexer compound specifiers (8bfab4f)
- ~~**Variadic arg promotion**~~ — sub-64-bit movsx/movzx for printf (299ccf5)
- ~~**C string function redirect**~~ — strlen(char*) → libc via dlsym (78949c0)
- ~~**dlsym return to Mem**~~ — temp Gp for setRet when regdp.first is Mem (41c977a)
- ~~**Typedef'd types in struct members**~~ — identifier resolved against datatype_map (41c977a)
- ~~**struct Type in function parameters**~~ — parseFunction handles struct/typedef names (8b0f8e2)
- ~~**`*ptr` dereference operator**~~ — TokenDeref for read/write through heap pointers (f481ea4)
- ~~**`static` keyword**~~ — persistent locals with vfSTATIC + heap allocation (ebcfd93)
- ~~**`const` and `extern` keywords**~~ — consumed/skipped for C compatibility (ebcfd93)
- ~~**`enum` keyword**~~ — global constant variables with auto-increment (b0f5df4)
- ~~**`typedef` for primitive types**~~ — typedef int sh_int; typedef unsigned char bool; (0047fd0)
- ~~**Compound assignments on struct members**~~ — resolveCompoundLHS helper (b61fdbe)
- ~~**make fulltest**~~ — unit + integration tests in one command (bbc2f04)
- ~~**isUnaryPosition()/isPostfixPosition() helpers**~~ — replaces duplicated checks (59805a6)
- ~~**`<stdarg.h>` / `va_list`**~~ — `...` in function decls, va_start/va_end macros, va_arg intrinsic, packed int64_t[] buffer, format-aware vsprintf helper (9afa644)
- ~~**For-loop increment parsing bug**~~ — `i++`/`i--`/`++i`/`--i` in for-loop third position now works. Root cause: conditional `parseExpression` left `;` in stream; fixed with one extra `nextToken()` in `TokenFOR::parse()`
