# HANDOFF — top-level `defer` (#16) and top-level `:=` (#18) join the synthesized script-mode main

Written 2026-08-06 at the v0.68.0 release (develop @beadfe22, release
commit @1501a2e3, feature work through @429842b4). This is the next
session's mission. Follow it as written; the SETTLED section is not
open for re-derivation.

## MISSION (imperative)

Make EVERY top-level statement form join the synthesized script-mode
main. Two known holdouts:

1. **#18 — top-level `a, b := f();`** leaves the receivers as undefined
   MIR imports instead of declaring them.
2. **#16 — top-level `defer stmt;`** is rejected outright
   ("'defer' must be inside a function or block").

Ship each as its own commit with rule trailers, a `tests/` gate per
form, and the standard battery discipline. Branch: cut a fresh
`feature/script-toplevel-claude` off develop (develop is the v0.68.0
release state — keep it stable).

## FIRST ACTION — re-verify the reducers at HEAD

The reducers were captured BEFORE the multi-return struct-transport
rework (@a369cb17) landed; the failure shapes may have changed. Run
them before forming any hypothesis:

- container `/workspace/madc/tmp/p_scriptmret.mad` — top-level
  `q, r := divide(17, 5);` after the divide def. At capture time:
  rc=1, `MIR error: import of undefined item q` (+ r).
- container `/workspace/madc/tmp/p_scriptfn.mad` — fn def + PLAIN
  top-level statements: WORKS (prints 42). This is the negative
  control: general script-mode adoption is fine; only specific
  statement forms miss it.
- top-level `defer` reducer: two lines — `defer __builtin_printf("bye\n");`
  then `__builtin_printf("hi\n");` in a bare .mad. Expect the loud
  reject at capture time.
- ALSO probe top-level SINGLE `x := 42;` — untested at capture time;
  it shares the `code = compounds.empty() ? NULL : ...` shape with the
  multi form and may be silently broken too. If it is, it joins #18's
  fix.

## VERIFIED ANCHORS (checked at @429842b4 — line numbers drift, grep the names)

- **The script-mode owner:** `parser.cpp` section
  "`---- Script mode (STD_MADC): file-scope statements → synthesized main ----`"
  (~62514). Key functions:
  - `Program::adopt_script_statement(TokenBase *ts)` (~62731) — routes a
    classified file-scope statement into `script_main_tf->statements`;
    `ensure_script_main(ts)` creates the main on first use.
  - `Program::finalize_script_main()` (~62750) — seals it at end of TU
    parse, handing it to the same queues a parsed definition uses.
- **#16 reject site:** `TokenDEFER::parse` (~45131):
  `if compounds.empty() → Throw("'defer' must be inside a function or block")`.
- **#18 receiver creation:** the `:=` arm in `parseStatement`
  (~61903, "`:= short declaration`"): both the multi form and the
  single form do
  `TokenCpnd *code = compounds.empty() ? NULL : compounds.top();`
  then `addVariable(code, *vtype, ids[i], 1, NULL, /*alloc=*/!code)`.
  At top level `code == NULL` → the receivers become host-allocated
  variables OUTSIDE any compound, and the returned statement (a
  TokenAssign since @a369cb17 — no longer a TokenDecl) gets adopted
  into the script main WITHOUT its receivers.
- **Where working globals register:** plain top-level declarations go
  through `parseDeclaration` and land where the CIR's global walk sees
  them (`prog->tkProgram->variables` — consumed in `cir_builder.cpp`
  ~23700 and ~24819). The `:=` receivers created with `code == NULL`
  never reach that walk → "import of undefined item".

## HYPOTHESES TO TEST FIRST (not conclusions)

- **#18:** the fix is likely at the `:=` arm: when `compounds.empty()`
  and `language_std == STD_MADC`, call `ensure_script_main(tb)` first
  and bind the receivers to the script main's COMPOUND (making them
  main-locals, exactly what receivers inside a written `main` are).
  That also gives class-typed receivers their block-top default-ctor +
  cleanup-dtor via the existing scope machinery (`cir_builder.cpp`
  translate-block scope-vars loop, ~21349) — top-level
  `x, y := labels()` with std::string receivers must work, not just
  ints. Check what `ensure_script_main` exposes (is there a compound
  accessible at parse time? — READ it before assuming).
- **#16:** same shape: at `TokenDEFER::parse`, when `compounds.empty()`
  and STD_MADC, adopt into the script main's compound instead of
  throwing (defer registers on the enclosing compound's `deferred`
  vector — the script main's compound is the right owner; scope-exit =
  end of the synthesized main). Standards modes keep the loud reject
  verbatim.
- Both fixes are DIALECT-GATED (`language_std == STD_MADC`); the
  standards-mode behavior (reject) is correct and tested.

## GATES TO SHIP

- `tests/testscripttopdefer.mad` + `.expect` — top-level defer runs at
  script end (LIFO if two defers).
- `tests/testscripttopmultiret.mad` + `.expect` — top-level
  `q, r := divide(17, 5);` prints 3 2; include a `string` receiver case
  if the class path is in scope.
- Keep `testautoceremonystd`-style negative control if the reject
  message changes shape in standards modes (it should NOT change).
- Doc updates: `docs/language/modern/defer.md` ("top-level defer" gap
  note) and `docs/language/multiple-returns.md` ("Known Limitations"
  top-level `:=` bullet) — REMOVE the limitation notes in the same
  commits that fix them; re-run the doc-example harness
  (`scratchpad/extract_doc_examples.py` + `run_doc_examples.sh`,
  container copies under `/workspace/madc/tmp/docex-*`).

## SETTLED — do not re-litigate

- Multi-return is the struct-transport model (@a369cb17): transport
  struct = the fn's C-level return type; receivers are plain block-top
  scope vars; the `:=` statement is a TokenAssign (multi_vars), NOT a
  TokenDecl. #18's fix works WITH this model (bind receivers to the
  script main's compound), never by reviving the TokenDecl path or a
  parallel slot ABI.
- `Variable::slot_size()` owns the 64-bit ddINT slot-width contract
  (@429842b4). Any new scalar `data` allocation uses it.
- Script-mode adoption stays gated to the MAIN source file and
  STD_MADC (`adopt_script_statement`'s existing checks).
- v0.68.0 is RELEASED (develop @beadfe22; fork tag v1.0-madc.0.68.0 at
  pin 4573a0f3 + GitHub release live). Release evidence: fulltest
  997/0/9skip, lane 993/0/13skip, EXE/OBJ 976/0, docs 53/53
  (tmp/logs/rb-20260806-194726.log).

## QUEUED BEHIND (optional, separate commits, only after #16/#18)

- Multi-return follow-ups: class METHODS (currently loud-rejected),
  reference slots (loud-rejected), receiving into pre-declared
  variables (`a, b = f()` — currently "Expecting := after identifier
  list").
- The pre-existing parse-phase valgrind noise is FIXED (@429842b4) —
  nothing outstanding there.

## TRAPS (bitten this session)

- QNAP never builds/tests — container only (`scripts/remote_build.sh
  sync build`, `TESTS="globs"` is an ENV var; never sync while a
  battery runs).
- Battery discipline: per fix = targeted `TESTS=` globs; per batch =
  fulltest + libcxxjit; EXE/OBJ at session end only.
- `spelling()` returns `const char*` (no `.c_str()`).
- `pgm.Throw(tok) << ... << flush` in token parse methods;
  `m_prog->Throw(tok) << ... << std::flush` in the CIR builder.
- The doc-example extractor treats argv[1] as OUTDIR (no --help).
