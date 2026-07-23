# HANDOFF — `===`/`!==` strict-equality track MERGED; next = eval package C / promote backlog

Date: 2026-06-12. This is the **current cold-restart contract** (supersedes
`docs/plans/2026-06-11-v0.29-mir-campaign-HANDOFF.md` for state/queue; that
doc keeps the MIR-campaign mechanism map and the c2mir debug recipes).

## RESTART STEPS

1. `bash scripts/resume.sh` (STEP 0 — rehydrates the standing context).
2. Read this file.
3. **TRUST THE LIVE REPO** over any doc: `git log --oneline -10`, `git status`,
   real build/test runs are operational truth.

## STATE (verified at write time)

- **develop @ `7c323cf`** — LOCAL-ONLY, 16 commits ahead of origin.
  **NEVER push develop without `/release`.** Working tree clean.
- v0.29.0 was the last release (2026-06-11, backend correctness: O2=O1
  torture parity). v0.28.0 same day (`<=>` track). MIR fork pinned @
  `9ab36fb` (`MIR_COMMIT`), pushed.
- `feature/strict-equality-claude` @ `59000b4` is fully contained in develop
  (ff-merged after user verification) — safe to delete.
- **Gates at the merged tree** (clean rebuild, zero warnings):
  - fulltest **577 / 0 / 0 / 18**, exit 0, both check gates GREEN
    (no-std-hardcoding, call-emit-symbol).
  - gcc.c-torture **1567/26/29/0/63** — failset **byte-identical** to
    `tmp/failset_lsq.txt` (55 lines; regenerate-and-diff after any change).
  - SMAUG soak green: `cd /workspace/MadSMAUG/runtime/area; timeout 50
    /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json
    -lcrypt 4000` → exit 124 + `Realms of Despair ready at`.
  - Units: 10 doctest binaries green (`make -C src test` — the ONLY target
    that relinks `bin/test_*`).

## WHAT LANDED THIS SESSION (the strict-equality track, 13 commits + docs)

`a === b` = type-domain identity AND value equality; `a !== b` = `!(===)`,
never `operator!=`. STD_MADC-only. Spec (authoritative semantics, incl. the
deferred items): `docs/superpowers/specs/2026-06-11-strict-equality-design.md`.
Plan (per-task record): `docs/superpowers/plans/2026-06-11-strict-equality.md`.

### Mechanism map — where everything lives

| Piece | Site |
|---|---|
| Lexer gating (`===`, `!==` STD_MADC-only; conformance fix — `===` used to lex at `--std=c++17`) | `src/lexer.cpp` `'='` case (~2051) and `'!'` case (~2715); `<=>` precedent ~2900 |
| `Token3NotEq` (+ `tk3NotEq` at enum TAIL — PCH serializes ids, append-only) | `include/tokens.h` after `Token3Eq`; clone case `src/pch.cpp:151` |
| Type predicate `DataDef::same_representation` (tags ARE representation; enums/bool own domains; ptr recursion; fn signatures incl. varargs; double-ptr 20000+x tag-collision guard `!ap`) | declared `include/datadef.h`, defined `src/parser.cpp` ~6339; doctest `tests/unit/test_datadef.cpp` |
| CIR lowering `strict_equality_lowering` (scalar → N_EQ/N_NE or comma-constant w/ side effects; literal-literal folds for constant initializers; class → `class_operator_call(top,origin,"==")` domain rule; `!==` wraps N_NOT; same-class-no-op → loud error) | `src/cir_builder.cpp` after `three_way_builtin_lowering`; hook in `translate_expr` after the tk3Way block |
| `class_operator_call` `opsym_override` param (NULL = derive from token) | `src/cir_builder.cpp` ~6122 + `src/cir_builder.h` |
| `operand_value_datadef` — scalar twin of `operand_object_class` (vfREFERENCE+DataDefPTR unwrap; refs have THREE encodings: DataDefREF instance, bare 20000+x tag, vfREFERENCE flag) | `src/cir_builder.cpp` ~3191 |
| User overload dispatch: `binop_overload_symbol` `tk3Eq→"==="`/`tk3NotEq→"!=="` (member, via class_operator_call BEFORE the strict hook); `object_operator_symbol` same entries (free-operator family); `rewritten_operator_candidate` `!==` → `!(x === y)` when a user `===` exists | `src/cir_builder.cpp` ~4825; `src/parser.cpp` ~6525 and ~6725 |
| Mangling `operator===`⇒`v23eq3`, `operator!==`⇒`v23ne3` (Itanium vendor-extended; SINGLE table) | `src/madc_mangle.cpp` `operator_code` :147; pinned in `tests/unit/test_mangle.cpp` |
| eval-DSL: `tk3NotEq` in `is_expression_compare_token`; `!==` strings → `strcmp != 0`; **comparisons infer int result** (`infer_expression_result_type` — was the segfault: string `!==` marshalled its int result as `char*`) | `src/madc_program.cpp` :395, ~:530, ~:735 |
| Sub-fix: plain enum vars keep `DataDefENUM` identity (was: non-scoped tags decayed to int) | `src/parser.cpp` `TokenENUM::parse` ~22001, dynamic_cast-gated, C mode unchanged |
| Tests | `test3eq` (29 scalar+ref shapes), `test3eqclass` (16 class/overload), `test3eqerr` (.expect_err loud error), `test3eqgate`/`test3noteqgate` (std floor); DSL doctests in `test_libmadc_program.cpp` |

### Deferred (recorded in the spec's Out-of-scope — do NOT re-discover)

- **Script-side `array`/`madc::value` `===`**: whole-value scalar ops do not
  exist on the dtARRAY surface at all (`array v; v = 5;` = "assignment of
  incompatible value") — lands with eval package C. The eval DSL already has
  strict `===`/`!==` over dynamic values.
- Reversed-operand `===` candidate (`operator===(A,B)` with swapped args) —
  slot into `rewritten_operator_candidate` if a use case appears.
- `test3eqclass_realhdr` — blocked on real `<string>` under STD_MADC
  (`_GLIBCXX_BEGIN_NAMESPACE_VERSION` gap, header-partition campaign).
- Loose `==` juggling on `madc::value`; `= default` for `operator===`.

## NEXT QUEUE (in order)

1. **eval package C** — the 38 `test_libmadc_program` skips
   (`docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`: register_function,
   get/set_global, string call marshalling, fork/limits, AOT save/load).
   Note: script-array whole-value ops (deferred above) belong to this work.
2. **41 class-(a) promote backlog** — K&R old-style definitions ×23 is the
   biggest lever (std-gated < C23, never C++); promote gate = ≥1608/1652
   (`docs/parity/failset-classification.md`,
   `docs/parity/root-cause-worklist.md`).
3. Non-blocking: `<=>` precedence corner; O2 perf evaluation (O2 is correct
   since v0.29.0 — measure before flipping default `madc_opt_level` 1→2).

## GOTCHAS (new this session + still-live)

- **`git commit -m` with backticks/parens gets shell-mangled** — ALWAYS
  `git commit -F tmp/msg.txt` for multi-line messages.
- **doctest failure details can be swallowed** in `test_libmadc_program`
  (engine-owned IO redirects during eval tests) — when a CHECK fails with no
  ERROR output, build a tiny host probe in `tmp/` linking `lib/libmadc.a`
  (`g++ -std=c++11 -I include tmp/probe.cpp lib/libmadc.a
  /workspace/mir/libmir.a -rdynamic -ldl -lz -lm -lpthread`) and print
  results via fprintf(stderr).
- **Script-side `madc::eval_expression_bool` printf probes print garbage**
  (pre-existing bool-return marshalling quirk, affects `==` baseline too —
  don't chase it as a `===` bug; use the C++ host probe instead).
- `make -C src` does NOT relink `bin/test_*` (only `make -C src test` does);
  NAS mtime trap; capped runs `( ulimit -t ...; timeout ... )`; ONE heavy
  job at a time.
- `Q a(1), b(2);` ctor-arg multi-declarator HANGS the parser — split
  declarations (test3eqclass does).
- c2m flag ORDER: `-O2`/`-dg2` BEFORE `-eg` (after it = program argv).
  Torture at O2 via `--madc tmp/madc_O2.sh`. Debug-c2m recipe `tmp/c2m-debug/`.
- `/workspace/mir` `make` does not always relink `mir-bin-run`.
- Reducers carry their original flags (`--std=…`, `--no-embedded-headers`);
  flagless = embedded path = masks real-header bugs.
- SMAUG manifest is `/workspace/MadSMAUG/compile_commands.json` (NOT src/).

## KNOWN PRE-EXISTING GAPS (unchanged — see claude_status.json for the list)

`<string>` at `--std=c++20` "Unknown namespace ranges"; testfortypedcomma
flaky; 1D-VLA sizeof = 8; global-const array dim reads 0; upstream MIR #426
lref round-trip; `cout << std::string` free-op-RK-reference wall (real-header
track); PCH include-guard/macro state before broad regeneration.

## MIRRORS (all synced at `7c323cf`)

`claude_status.json` (canonical snapshot) · `CHANGELOG.md` [Unreleased] ·
`docs/test-status.md` Current block · KG `Feature{strict_equality_operators}`
(via `scripts/kg_query.sh`) · agent memory RESTART line.
