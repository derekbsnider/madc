# RESTART HANDOFF — 2026-05-31 (session 2, late) — READ FIRST after restart/compaction

Authoritative rehydration entry. Supersedes all earlier handoffs. The C++23/C++ class
model work of this session is essentially COMPLETE; the next phase is the **C-stability
pivot**.

## Read order on restart
0. **`docs/plans/madc-vision-and-invariants.md`** — the north-star vision (madc as a
   polyglot transpiler; cir_node the universal C/C++ IR) + the **invariants I1–I8** +
   the "does this block the vision?" checklist. EVERY change must satisfy these.
1. This file (top to bottom).
2. **`docs/plans/cpp-support.md`** — the authoritative C++23/C23 compliance roadmap
   (honest status table + prioritized P0/P1/P2 + the std-subsystem P2.11–14).
3. Memories (the WHY/lessons): `project_north_star_c23_cpp23`,
   `feedback_correct_over_shortcuts`, `feedback_no_false_choice_menus`,
   `project_std_enum_gatekeeping`, `project_sequencing_cpp_then_c`,
   `project_mir_setjmp_returns_twice`, `project_recycle_old_transpiler_carefully`,
   `project_string_as_class` (now grep-verified-complete), `feedback_remove_cruft_promptly`.
4. Then verify live state (below).

## LIVE STATE (verify on restart)
- Repo `/workspace/madc`, branch **`feature/cir-stdstring-claude`** (off develop),
  HEAD **`d5ab5a7`** (or later). MIR fork `/workspace/mir` @ `53cdb85`, untouched this session.
- Build `make -C src`; test `make -C src fulltest`; single test `bin/madc tests/NAME.mad`;
  emit C `bin/madc --emit=c11 FILE.mad`.
- **Gate baseline: `make -C src fulltest` = ~410 passed** / ~47 failed (pre-existing
  vla/struct/fnptr-array/complex/multiret — NOT this session's; verify the failset is the
  same) / ~61 skipped. + flaky `testfortypedcomma` (fail↔timeout, IGNORE only that). NEVER
  drop the pass count.
- VERIFY: `git rev-parse --short HEAD` >= d5ab5a7; `make -C src` clean; `make -C src fulltest`
  ~410; `grep -rn "dtSTRING\b\|dtSTRINGref" src/ include/` == **0** (the crutch stays dead).
- ⚠️ **P2.10 (commit d5ab5a7) was reported by its subagent at 410 green but I had NOT yet
  re-gated it myself when this handoff was written** (context filled). FIRST: re-run
  `make -C src fulltest`, confirm ~410 + no new failures vs baseline. (Coordinator-verify
  before trusting — see the dtSTRING lesson below.)

## HOW WORK IS DONE (the methodology that worked all session)
- **Subagent-driven, ONE task at a time** (skill: superpowers:subagent-driven-development).
  Fresh opus subagents (`Agent` tool, model:opus). Brief them LIBERALLY (the WHY +
  architecture + exact file:line anchors + the relevant invariants + gcc-canon +
  "gate fulltest, don't regress" + an escape hatch: "commit what's green / report
  BLOCKED|DONE_WITH_CONCERNS, never force a shim"). Memory `feedback_liberal_subagent_context`.
- **SERIALIZE builds** — never run two `make -C src` concurrently (object-file races). The
  coordinator gate-runs `make -C src fulltest` AFTER a subagent finishes, then dispatches
  the next. Parser/cir_builder edits especially can't be parallel.
- **Coordinator VERIFIES every task before accepting**: read the diff, re-run the gate,
  diff the failset vs baseline (must be empty), and for removals run the grep. Do NOT relay
  a subagent's "done" — verify it. (This is the hard-won dtSTRING lesson.)
- ⚠️ **SendMessage cannot resume a PRIOR session's subagent across a restart** — those
  processes are gone. Dispatch FRESH subagents; reconstruct context from this doc + the
  roadmap. (Within a live session SendMessage DOES resume a completed background agent by
  its agentId.)

## WHAT'S DONE THIS SESSION (376 → 410 green, every fix gcc-compared + fulltest-gated + invariant-checked)
- **P0 correctness-crash tier COMPLETE** (7): P0.1 class by-value return (generalized
  `__retbuf`) · P0.2 class-ref param member access (+ a `ref_params`/`__this` align bug) ·
  P0.3 virtual-dispatch SIGSEGV (vptr-init in ctor-less `new`) · P0.4 range-for over raw
  array · P0.5 copy-assignment double-free (synthesized memberwise `operator=`) · P0.6
  `arr[i]->method()` arrow-chain · P0.7 `vector<T*>` multi-element (double-ptr member render).
  Plus the dead-code HYGIENE sweep.
- **P1:** P1.1 exceptions try/catch/throw (SJLJ, scalar) + **P1.1c RAII dtor-unwind**
  (front-end, gcc-verified) · P1.2 lambda return-type deduction · P1.3a class self-type
  (user-class operators now work) · P1.3b `this`/`this->member`.
- **P2 feature-completeness:** P2.1 + P2.1b full operator set (binary/compound/bitwise/
  shift/logical/unary/`()`/`->`/postfix) · P2.2 `enum class` · P2.3 `auto` deduction · P2.4
  `const` enforcement · P2.5+5b+5c access control (data+methods; **class defaults private**)
  · P2.6 derived→base upcast · P2.7 range-for-by-ref · P2.8b `string&` operators · P2.10
  object-returning lambda fn-ptr.
- **★ P2.14 — the `dtSTRING`/`dtSTRINGref` crutch ELIMINATED (grep=0, verified).** std::string
  is now a fully generic `dtRESERVED` class recognized by identity, like vector/map/set.
  `typespec_t` (DataDef* registration) + `MadValueKind` + Variable-ctor/dtor-by-identity +
  enum deletion + `canonical_string_class` shim removed. **P2.9 dissolved.** 219→0.
- **★ Architecture crystallized:** `docs/plans/madc-vision-and-invariants.md` (I1–I8),
  referenced from AGENTS.md. The `--std=`/`LanguageStd` enum is the dialect gatekeeper on
  BOTH input (feature gating) and output (c2mir target) ends; one IR; no special-casing.

## THE PIVOT — C-STABILITY PASS (next phase; user-agreed: core C++ done → now C)
Per `project_sequencing_cpp_then_c`. Sequence/scope:
1. **P1.1b — MIR `setjmp`-returns-twice fix (FORK, `/workspace/mir`).** ROOT-CAUSED
   (`project_mir_setjmp_returns_twice`): c2mir doesn't model `setjmp` as returns-twice →
   locals across `setjmp` not spilled → stale registers after `longjmp` → exception
   rethrow loops + dtor-unwind SIGBUS (frame-size-scaled). The CORRECT fix is in the fork
   (returns-twice modeling / spill); the `volatile`-locals trick is a SHIM to AVOID.
   Confirm via a minimal C `setjmp`-reducer first. Unblocks the `.mir_skip`'d exception
   tests (`testrethrow`, `testexcept_dtor_{rethrow,string,order,nested}`).
2. **P2.11–13 std-dialect subsystem:** P2.11 build the keyword/feature→standard REGISTRY
   (replaces the ad-hoc `is_c_mode()` gating) + close gaps (`operator` keyword ungated;
   audit lambda/`auto`/`enum class`); P2.12 de-hardcode the c2mir target std (enum-driven,
   `celC11`→`LanguageStd`); P2.13 standards conversion (input-std ≠ output-std).
3. **P2.8** foreach VLA / MadArray catch-all (range-for over a VLA still crashes — tighten
   `translate_foreach` to error on unrecognized containers instead of assuming MadArray).
4. **The ~47 C-side integration failures** (Track 1.3: vla/struct-init/fnptr-array/complex/
   multiret) — the develop→master parity gate (`feedback_promote_parity`).
5. Smaller C++ follow-ups (opportunistic): P2.5b→ note class-default-private done; P2.10
   leftovers (lambda in std::function-like wrapper); the `string("lit")` ctor-call-in-lambda
   + no-paren-lambda parser gaps; `"lit" + string` (const-char*-LHS operator+).

## KEY LESSONS LOCKED THIS SESSION (don't repeat)
- **"Done" for a removal = `grep` returns 0 + gate green, COORDINATOR-VERIFIED.** Never
  relay a claimed "DONE" (the dtSTRING crutch survived a week of partial-then-"done" passes
  because the hard embedded 20% kept being deferred while the claim was made).
- **Correct over shortcuts** (`feedback_correct_over_shortcuts`): fix at the deepest layer
  even into the fork; a legacy artifact's mere EXISTENCE is a drift risk → eliminate, don't
  route around. (MIR setjmp → fork fix, not the volatile shim.)
- **No false-choice menus** (`feedback_no_false_choice_menus`): when the path is clear,
  state it and proceed; reserve AskUserQuestion for genuine forks.

## ONE-LINE SUMMARY
C++ class model is complete + correct + fully de-special-cased (dtSTRING crutch eliminated,
grep=0); 410 green; vision+invariants crystallized. NEXT = the C-stability pivot (MIR
setjmp-returns-twice fork fix · std-dialect registry/gating/target/conversion · the ~47
C-side fails → develop→master parity). Re-gate P2.10 (d5ab5a7) on restart first.
