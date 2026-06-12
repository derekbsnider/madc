# HANDOFF — type-table/value-ABI track + package-C globals; next = verify branch, then string call marshalling

Date: 2026-06-12 (same-day successor to `2026-06-12-strict-equality-HANDOFF.md`,
which remains the ===/!== mechanism reference). This is the **current
cold-restart contract**.

## RESTART STEPS

1. `bash scripts/resume.sh` (STEP 0).
2. Read this file.
3. **TRUST THE LIVE REPO**: `git log --oneline -10`, `git status`, real runs.

## STATE (verified at write time)

- **develop @ `1e2961a`** — LOCAL-ONLY, ~30 ahead of origin. **NEVER push
  develop without `/release`.** Working tree clean. Last release v0.29.0.
- **`feature/eval-globals-claude` @ `ffb2bc6` — UNMERGED, awaiting user
  verification** (4 commits: data_address / storage helpers+unskips /
  __madc_global_init+string globals / plan banner). Gates at branch head:
  clean rebuild zero warnings; units 10/10 (test_libmadc_program **106/0/35**);
  fulltest **577/0/0/18 exit 0**, both check gates GREEN. Torture/SMAUG not
  re-soaked: pure C, the new codegen branch can't fire without class globals.
- MIR fork untouched today (@ `9ab36fb`, pinned).

## WHAT LANDED TODAY (in merge order)

All design docs cite each other; the spine is
`docs/plans/2026-06-12-type-table-value-abi-design.md` (DESIGN AGREED,
user-signed; §6 phases 1+2 marked IMPLEMENTED in its Status block).

1. **`===`/`!==` strict-equality track** — merged early in the day (see its
   own handoff).
2. **Type-table design agreed** + doc-set reconciliation (forest +
   frontend-refactor + madcdis-plan UPDATE blocks; madcdis's 8-byte handle =
   internal pool handle only; `value_header.type_tag` := typeid; ONE table,
   ONE struct guardrails).
3. **Identity layer (phase 1) MERGED**: `include/madc_typeid.h` (segmented
   uint32 typeid space: 0 invalid · `[1,0x100)` primitives, 255 usable —
   user widened from 100 · `[0x100,0x01000000)` system-forest reserved ·
   project base `0x01000000`); `DataDef::type_id`;
   `madc_primitive_for_slot()`/`madc_stamp_primitive_type_ids()`
   (parser.cpp, stamped from `add_datatypes`); `Program::type_id_for()` /
   `type_from_id()` (project segment `vector<DataDef*>`). ABI slots pinned in
   `tests/unit/test_datadef.cpp`. Plan:
   `docs/superpowers/plans/2026-06-12-type-table-identity-layer.md`.
4. **32-byte value ABI (phase 2) MERGED**: `madc_api.h` struct
   `{uint32 type_id; uint32 flags; uint64 size; 16B aligned union}`;
   **`MADC_VALUE_*` kind constants are now typeid ALIASES (numeric values
   CHANGED: INTEGER==10, STRING==31)**; dynamic-kind slots TEXT=31/BYTES=32/
   OBJECT=33 (PRIMITIVE_LAST=33); `MADC_VF_*` flags (HEAP/INLINE_TEXT/
   TYPE_LOCKED/TYPE_COERCE/NULLABLE/CONST); cell runtime
   `include/madc_value_cell.h` + `src/madc_value.cpp` (saturating non-atomic
   refcounts, MADC_CELL_PERMANENT tier); helpers/bridges rewritten in
   `madc_c_api.cpp` (SSO **15 bytes + NUL** — refined from §3's 16;
   `madc_value_copy` = initialization semantics, retains shared cell;
   `madc_value_text` uniform accessor; gradual typing: unrestricted re-tag
   default, COERCE converts within numeric family toward locked domain,
   LOCKED rejects cross-domain, NULLABLE admits typed null = `size==0` with
   kept type_id, CONST read-only; contract survives clear). Plan:
   `docs/superpowers/plans/2026-06-12-value-abi-phase2.md`.
5. **Package-C increment 1 (ON THE UNMERGED BRANCH)**: get/set_global on
   live MIR storage. Mechanism map:
   - `CirJitSession::data_address(name)` (`src/madc_cir.cpp`, after
     function_code) — walks module items (data/bss/ref_data/expr_data),
     returns `item->addr`. Globals emit under their source identifier.
   - `value_from_storage`/`set_storage_from_value` (`src/madc_program.cpp`
     ~:2179) — storage-pointer forms; accesses EXACTLY `type->size` bytes
     (old code wrote int globals as int64 = neighbor clobber on MIR layout;
     neighbor-canary test pins it). `value_from_variable`/
     `set_variable_from_value` are now thin wrappers.
   - get/set_global resolve `jit->data_address(id)` with `var->data`
     fallback; text-carrier globals (`marshals_value_text()`) marshal by
     reading/assigning the LIVE libstdc++ `std::string` object at the
     resolved address ONLY (parse-time fallback would touch unconstructed
     memory) — same mechanism as `__madc_scope_set_string_runtime`
     (parser.cpp:821).
   - **`__madc_global_init`**: file-scope class-global ctor calls moved from
     main-prologue inlining into ONE synthesized module function
     (cir_builder.cpp translate_module, inserted at FRONT of
     func_def_nodes; static once-guard `__madc_gi_done`); main calls it
     (translate_func main branch); `ensure_runtime_initialized`
     (madc_program.cpp ~:3580) invokes it for main-less sessions. THE find:
     dynamic global init never ran in call-only embedding before.

## NEXT QUEUE (in order)

1. **User verifies + merges `feature/eval-globals-claude`** (merge-only — NO
   per-phase mirror sync, user direction; see memory `mirror-sync-cadence`).
   At merge add a CHANGELOG entry for the globals increment.
2. **Package-C increment 2: string call marshalling** (~4 skips: call with
   `std::string` args :1638, string object returns :1656, C-API scalar+string
   :1678, eval string returns via char* :221, host-supplied string bindings
   :543 in test_libmadc_program.cpp). Reuses the live-object mechanism at
   call boundaries; grep `perform_call`/`dispatch_call*` first.
3. **Increment 3: register_function family** (~8 skips incl. engine
   callbacks :2312-:2455) — host→script trampolines; the 32-byte value ABI
   marshalling rule is the foundation.
4. fork/limits (~9 skips), policy stragglers (:824 math.h groups, :1615
   runtime_eval_policy child restriction). **AOT save/load (~12) stays
   DEFERRED** (claude_status: near-term native = --emit=c11).
5. Standing queue behind package C: 41 class-(a) promote backlog (K&R ×23,
   gate ≥1608/1652); `<=>` precedence corner; O2 perf eval.

## GOTCHAS (new today + still-live)

- **c2mir declaration grammar**: `N_SPEC_DECL(N_SHARE(specs), declarator,
  attrs, asm, initializer)` — **5 children, N_SHARE wrap** (c2mir.c:538).
  3 children compiled fine and SEGFAULTED at runtime (cost one cycle).
- **`MADC_VALUE_*` numeric values changed** (typeid aliases) — any external
  consumer comparing raw numbers breaks; in-tree all symbolic.
- `tests/unit/test_libmadc_value.cpp` links **madc_value.o ONLY** (no
  compiler internals) — layout/cell pins live there; helper-behavior tests
  go in test_libmadc_program.cpp.
- **tmp/ probe binaries go stale** — relink after every lib rebuild before
  trusting their output (bit me again today; also `make -C src` does NOT
  relink `bin/test_*`, only `make -C src test` does).
- doctest failures swallowed in test_libmadc_program → host probe recipe:
  `g++ -std=c++11 -I include tmp/probe.cpp lib/libmadc.a
  /workspace/mir/libmir.a -rdynamic -ldl -lz -lm -lpthread` + fprintf(stderr).
- `git commit -F tmp/msg.txt`; capped runs `( ulimit -t …; timeout … )`;
  ONE heavy job; reducers carry original flags; SMAUG manifest at
  /workspace/MadSMAUG/compile_commands.json.
- Execution mode: **inline, no implementation subagents** (Fable cost,
  user direction); dialog over AskUserQuestion menus in design talk.

## KNOWN PRE-EXISTING GAPS (unchanged)

See claude_status.json `known_pre_existing_gaps` — `Q a(1),b(2);` parser
hang, 1D-VLA sizeof, global-const array dim, testfortypedcomma flake,
`<string>` @ c++20 ranges, cout<<std::string free-op wall, PCH
include-guard state, upstream MIR #426.

## MIRRORS

Synced at handoff time: `claude_status.json` (head/branch/phase),
`CHANGELOG.md` [Unreleased] (value-ABI entry; globals entry due AT MERGE),
KG `Decision{type_table_value_abi}` (phases 1-2 implemented + increment-1
note), agent memory (RESTART line → this file; project_type_table_value_abi;
project_libmadc_eval).
