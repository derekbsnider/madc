# libmadc in-process compile/exec/eval on CIR→c2mir→MIR — implementation plan

2026-06-10. Owner: the eval reimplementation track (deferred since asmjit
removal; ~91 unit cases in `tests/unit/test_libmadc_program.cpp` marked
`doctest::skip()` as the spec, plus 5 integration tests
`testmadceval*`.mir_skip).

## Verified current state (read the code, ran the tests)

- `program::compile_file/compile_string` **work**: they run
  `Program::load_buffer` (the full lexer+parser). `is_compiled()` keys on
  `pgm->root_fn`.
- `Program::execute()` (madc_program.cpp:4805) is **THE stub** — sets a
  runtime error pointing at `madc_cir_execute()`.
- `program::impl::perform_call` is the complete asmjit-era call surface:
  signature checking, `native_type_from_datadef`, value marshalling,
  `dispatch_call0..4`, fork-per-invocation mode, rlimit wrapping. Its ONLY
  dead dependency is `method->x86code` (the per-Method JIT pointer asmjit
  populated) and `ensure_runtime_initialized()` (which ran the asmjit
  root_fn).
- `madc_cir_execute` (madc_cir.cpp:207) is the one-shot CLI path:
  MIR_init → `build_tu_module` (CirBuilder::translate_module → cir_compile)
  → MIR_load_module → MIR_link → MIR_gen("main") → call → full teardown.
  `build_tu_module` is already factored for reuse (the project engine uses
  it across TUs in one context).
- The script-level `madc::eval_unit/eval_int/...` builtins (testmadceval*)
  are **not declared at all** on CIR — asmjit-era runtime registrations.
- `MadcEngine` is the IO/logging engine (streams/sinks), NOT a MIR holder;
  the project engine keeps MIR state in locals. A persistent session object
  does not exist yet.

## Design

**`CirJitSession`** (madc_cir.cpp, declared in a new `include/madc_cir.h`;
C++-first per `.claude/rules/cpp-first-api.md`):

```cpp
class CirJitSession {
public:
    ~CirJitSession();                       // full MIR/c2m/builder teardown
    bool build(Program *prog, const char *source_name);  // translate+compile+load+link
    void *function_code(const char *emitted_name);       // find item, MIR_gen, memoize
    int   run_main(int argc, char **argv);
    bool  built() const;
private:
    MIR_context_t ctx = NULL;
    c2m_ctx_t c2m = NULL;
    CirBuilder *builder = NULL;             // owns the node arena; outlives module
    MIR_module_t mod = NULL;
    std::map<std::string, void *> gen_cache;
};
```

Lifecycle mirrors `madc_cir_execute` split into phases; `build_tu_module`
is reused verbatim. The session lives in `program::impl`, torn down by
`reset_program()` and the destructor (a recompile = fresh session).

**Wiring (madc_program.cpp):**
- `ensure_runtime_initialized()` → session.build(pgm, display_name) when
  absent. ("Initialized" = module linked. Dynamic global initializers are a
  later increment — c2mir emits them; audit which unit tests need them.)
- `exec_compiled_with_display` → `session.run_main(argc, argv)` instead of
  `pgm->execute()`. (fork/limits wrappers unchanged — they wrap the lambda.)
- `perform_call` → `session.function_code(name)` replaces
  `method->x86code`; the rest of the function survives untouched. Plain
  madc top-level functions emit under their source name; `__madc_eval` is
  the eval entry the eval_* wrappers already synthesize.

**Increments (gate each with fulltest + the relevant unit-test un-skips):**
1. Session + exec (run main) + call/eval for the signatures dispatch_call
   already supports → un-skip the compile/exec/call/eval scalar cases.
2. Audit remaining skips: string args/returns, get_global/set_global,
   register_function (host callbacks into JIT — needs MIR import
   resolution of the callback pointer), load_object.
3. Script-level `madc::eval_*` builtins: register on CIR as runtime
   functions that spin a NESTED engine (security policy: the existing
   `madc_runtime_eval_policy` plumbing is live; clamps already implemented).
   Un-skips testmadceval*.mir_skip (5 integration tests).
4. REPL tier rides the same session (incremental modules — see
   docs/plans IDE-modes research; out of scope here).

## Increment 1 result (landed same day)

`CirJitSession` landed in madc_cir.{h,cpp}; `madc_cir_execute` now delegates
to it (one implementation). libmadc wiring: `compiled_ok` replaces the
never-set `root_fn` as the compiled predicate, `Program::compile()` is the
front-end no-op on CIR, `ensure_runtime_initialized` builds the session,
both exec lambdas share `run_main_now()`, `perform_call` takes its code
pointer from `session.function_code(name)`, and
`invoke_program_zero_arg_function` (the ad-hoc child-Program path serving
eval_unit/eval_body/eval_expression) builds a one-shot session — the
named-entry analogue of madc_cir_execute. Unit results: **91 of 133 cases
pass (49 of the 91 deferred came alive), 0 regressions**, 42 re-skipped by
category (see the suite-head comment in test_libmadc_program.cpp).

GOTCHA for future debugging: madc::program's engine captures std::cout into
its buffers — doctest's own report gets SWALLOWED when cases fail before
restoring streams. Use `bin/test_libmadc_program --out=FILE`.

## Master-branch reference (asmjit-era spec, per user pointer)

`git show master:src/madc_program.cpp` / `master:src/parser.cpp`:
- The `madc_runtime_eval_*` C-shaped host helpers (runtime script-level
  eval) STILL EXIST on develop (parser.cpp ~404+) — increment 3 is only
  their REGISTRATION into the `madc::` namespace on the CIR path (an
  embedded-header declaration set binding via dlsym/-rdynamic, like the
  ns_* namespaces; master registered them engine-side).
- Master's `is_runtime_eval_*_name` lists (parser.cpp ~336-390) enumerate
  the public + helper names: eval, eval_bool/int/double/string,
  eval_expression{,_bool,_int,_double,_string}, __madc_eval_*_runtime.
- Master's perform_call/invoke paths are the behavioral spec for the
  deferred string marshalling, get/set_global, and register_function
  trampolines (asmjit built them; on CIR a host callback needs an import
  the MIR resolver can see — register the pointer with the session).

## Risks / notes
- One MIR_context per session: parallel sessions are independent (MIR
  contexts are isolated); fork mode unaffected (child inherits memory).
- `MIR_gen` per item on demand (matches madc_cir_execute generating only
  main; MIR_link with MIR_set_gen_interface lazily gens callees).
- Teardown order matters: MIR_gen_finish → c2mir_finish → MIR_finish,
  builder deleted last (owns node arena) — copy madc_cir_execute exactly.
- `cir_import_resolver` is process-global (dlsym RTLD_DEFAULT) — fine.
