# HANDOFF — embedding track COMPLETE; next = promote backlog / AOT / forest-prereq ladder

Date: 2026-06-12 (succeeds `2026-06-12-type-table-track-HANDOFF.md`, whose
stacked banners remain the detailed chronology of the day). This is the
**current cold-restart contract**.

## RESTART STEPS

1. `bash scripts/resume.sh` (STEP 0).
2. Read this file.
3. **TRUST THE LIVE REPO**: `git log --oneline -10`, `git status`, real runs.

## STATE (verified at write time)

- **develop @ `910c832`** — LOCAL-ONLY ~59 ahead of origin. **NEVER push
  without `/release`.** Working tree clean. v0.29.0; MIR fork pinned @
  `9ab36fb` (`MIR_COMMIT`).
- `feature/eval-string-call-claude` is MERGED (--no-ff @ this morning) and
  can be deleted at the user's discretion.
- **Gates at HEAD:** fulltest **580/0/0/18** exit 0, both check gates GREEN;
  gcc.c-torture **1567**, failset NAME-IDENTICAL to `tmp/failset_lsq.txt`
  (55/55; the 34/21 compile/runtime split = the 87f1808 containment
  reclassification); SMAUG `--project` soak ready+124; zero warnings.
  `test_libmadc_program` **132 passed / 0 failed / 11 skipped** — every
  remaining skip is the deferred AOT save/load family.

## WHAT LANDED TODAY (after the morning's branch merge — in commit order)

1. **`739afcb` known-gaps sweep** (user-requested): ctor-comma declarator
   hang (`Q a(1), b(2);`) fixed via comma-continuation in the ctor-syntax
   decl path; VLA-VARIABLE `sizeof` fixed for 1D/multidim/typedef alike
   (runtime dims captured into hidden `__madc_vla_dim_*` uint64 locals at
   declaration — `materialize_vla_dim_capture`; `try_parse_vla_variable_sizeof`
   builds the runtime product; C99 6.5.3.4p2 declaration-time semantics);
   global const array dims (parse-known const scalar int initializers baked
   into `var->data`, new `vfCONSTBAKED` admits the CIR read-fold —
   `const int H = G + 3;` emits a constant file-scope initializer);
   `testfortypedcomma` flake unreproducible (25× green), delisted.
   New tests: `testctorcomma` / `testvlasizeof` / `testconstdim` (g++/gcc-
   verified). Residual recorded in status known-gaps: VLA sizeof in
   constant-required contexts and `alignof(vla)` still fold 8.
2. **`76a6f38` register_function** — the shim machinery REVERSED. Host
   callbacks via compiler-synthesized trampolines: program/engine impls hold
   `host_callback_entry` registries (engine regs seed created programs);
   `reset_program` installs `Program::HostCallbackReg` KIND-CODE records
   (survive Program re-creation); `_parser_init`→`add_host_callbacks`
   declares ordinary prototypes; `CirBuilder::synth_host_trampoline`
   (translate_module Pass 0.73) emits
   `RET name(params) { return __madc_host_cb_<k>([bound,] params...); }` —
   typed pass-throughs, ZERO runtime dispatch (the anti-pyramid principle);
   `cir_import_resolver` consults the linking Program's regs (thread_local
   around MIR_link, cleared on the containment longjmp path) before dlsym;
   the deduced form's user callback rides as the typed adapter's hidden
   first argument.
3. **`d1d413d` shims over trampolines** — `synth_call_shim` re-keyed on the
   function `Variable` (`synth_call_shim_var`; a TokenFunc was only the
   carrier), so host trampolines get marshalling shims and `program::call`
   works on registered names through the same ONE call surface;
   `invoke_with_limits` applies unchanged (cpu_ms/memory_bytes tests green).
4. **`6896a01` fork_per_invocation on CIR** — the forked child ran the
   removed-asmjit `Program::execute()` stub; it now runs `run_main_now()`
   (the CIR JIT session builds IN the child = the isolation model). Parent
   side (output relay, rusage limits, report protocol incl. string values)
   was already complete.
5. **`5c97917` policy tail** — `enable_dlfcn_functions=false` now also gates
   the LINK-time dlsym fallback: a user-source `declaration_only` prototype
   with no sanctioned binding (no `emit_symbol`; addFunction registrations
   have `declaration_only` false) is rejected in `Program::compile()` with
   the exact "dynamic symbol fallback is disabled by registration policy"
   diagnostic. New `FuncDef::decl_file` provenance keeps curated header
   declarations permitted (main unit identified by `tkProgram->source`).
   Plus: missing-main exec_file error, math.h header groups, and the
   runtime_eval child-restriction test (source modernized with the
   `<string>`/`<ns_madc>` includes the eval surface requires).

**The embedding boundary now crosses through compiler-synthesized typed
adapters in BOTH directions** — host→script (shims, 87f1808), script→host
(trampolines, 76a6f38), host→registered-name (shims over trampolines,
d1d413d) — with zero runtime type dispatch anywhere.

## NEXT OPTIONS (user decides)

- **A. The 41 class-(a) torture fixes** — the develop→master promote gate
  (≥1608/1652; `.claude/rules/branching.md`). Worklist:
  `docs/parity/root-cause-worklist.md`. Per `backend-strategy.md` this is
  the gating workstream — downstream tracks should not start before it.
- **B. AOT save/load reimplementation** — the last 11 unit skips
  (save_object/save_executable/load_object on CIR; near-term native builds
  remain `--emit=c11` + external compiler).
- **C. The forest-prerequisite ladder** — see below.

## Required before `2026-06-09-embedded-header-forest-design.md` can proceed

The forest doc itself defers to
`docs/plans/2026-06-09-frontend-representation-refactor.md` for phasing
(forest = its **P4/P5**; critical path **P0 → P3 → P4 → P5**), and both docs
fence the whole refactor behind the correctness work. Concretely, in order:

0. **Correctness fence (precedes the refactor entirely):**
   a. The remaining real-header walls (status `in_progress`): `cout <<
      std::string` free-operator class-rhs as reference; the free-std-fn
      `emit_symbol` migration (retire `try_std_free_function_call` + the
      `__ns_` shim gate); per-red ingredients for testfstream/testloop/
      testdefer; PCH include-guard/macro-state preservation before broad
      real-header PCH regeneration.
   b. **The 41 class-(a) promote backlog** — `backend-strategy.md` forbids
      starting downstream tracks (the forest is one) before CIR parity.
1. **P0 completion — token/value pools.** The TYPE-side substrate is DONE
   (typeid table identity layer + 32-byte `madc_value` ABI landed via the
   eval track; the shim/trampoline machinery is its first codegen consumer).
   The TOKEN-side is NOT: `TokenInt` still stores literals in a 64-bit
   `_token`; `__int128` is still aliased to `ddINT64` (>64-bit literals
   truncate at lex). Needs the out-of-line value pool + kind/handle split +
   real `__int128`/`_BitInt` DataDefs + widening the int64-capped consumer
   rungs.
2. **P3 — uid-keyed side-arrays.** Migrate the 6 madc-only `cir_node`
   fields into uid-keyed side-arrays at the `CirBuilder::make` chokepoint
   (c2mir-blind by construction); the `datadef` side-array holds **typeids,
   not DataDef*** — the moment forest type references become
   memcpy-serializable. `DataDef::type_id` exists; the side-array migration
   does not.
3. **P4 pre-work:** replace the STATIC `compiler_hash` string (pch.cpp:630)
   with a real context hash `{toolchain, --std, layout-affecting -D/target,
   search paths}` (reject-and-reparse on mismatch — never silently use a
   layout-mismatched forest); stand up the acceptance oracle (`madc -dM`
   parity vs `gcc -dM`, C/C++ smoke sets, forest round-trip identity
   load==reparse).
4. Then **P4** (serialize the pre-check cir_node graph: Type/Decl ID tables,
   per-unit zstd frames + trained dict + ID→offset TOC, mmap two-layer) and
   **P5** (the forest proper: generalize the LANDED lazy-body machinery from
   reparse-tokens to copy-subtree+substitute-env with the `(forest-node,
   env)` hash-cons memo).
- **P1** (flat token scan buffer) is an independent speed win that can run
  parallel after P0 (its record must carry `type_id` + value-pool handles
  from the start). **P2** (polymorphism collapse) stays fenced.

**Already-landed assets the forest builds on:** typeid table (system segment
`[0x100,0x01000000)` IS the forest's frozen id space) + 32-byte value ABI;
lazy member-body instantiation (`materialize_and_lower`/`deferred_lazy_bodies`
= the materialize-on-resolve seed); system-header reachability DCE;
`is_system_header_path` as the immutable/volatile seam; `MadhHeader`
(zstd/zlib + source_hash) as the Phase-1 container to extend;
`same_representation` + the append-only PCH id discipline.

## Standing constraints

- USER PRINCIPLE (emphatic): std::string/containers are header-defined
  classes, NOT primitives; NO per-class helpers for ANY libstdc++ class;
  #include is the entire integration cost; non-keywords are never tokenized;
  `madc::value` must not hold `std::string`.
- Gates per commit: fulltest + full-torture failset diff + SMAUG soak (skip
  torture/SMAUG only for changes provably unreachable from the CLI paths);
  capped runs; ONE heavy job; torture runner writes no log — capture full
  stdout (never pipe through tail); engine output-capture swallows doctest
  failure detail — debug via a standalone tmp/ probe.
