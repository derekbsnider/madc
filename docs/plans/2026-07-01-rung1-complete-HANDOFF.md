# Rung 1 COMPLETE — hand-off (2026-07-01)

Read this first after compaction. Self-contained; trust it + live `git log` over the
compaction summary (compaction over-compacts — assume cold start).

## 0. State (verify: `git -C /workspace/madc log --oneline -16`, `git status`)
- Branch **develop**, HEAD **`37354ff9`**. Tree CLEAN (only untracked
  `mir-debug-support.md` — NOT ours, never stage).
- **develop is +14 ahead of origin/develop**, ALL gated green, **HELD for the next
  `/release`** (user's standing call — do NOT `git push` raw; the release cuts+pushes).
- Dev binary is **-O0** (the committed default; parse ~0.96s on testmap). This is
  deliberate — -O0 must itself be fast; -O2 is the LAST lever, not for chasing numbers.
  See memory [[feedback_o0_efficiency_o2_last_lever]].
- Commit via `git commit -F -` heredoc; stage files EXPLICITLY; NEVER `git add -A`;
  NEVER stage `mir-debug-support.md`. Co-Authored-By + Claude-Session trailers.

## 1. What landed this session (the 14 held commits)
Rung-1 `madc::dis` interning track is FINISHED (Steps 1-4 of
docs/plans/2026-06-23-arena-interning-HANDOFF.md §2). This session did:
- **Perf fixes** (5739e624): `Variable::name_sid` cache + `finalize_pop1_rec` file_id
  cache — kill redundant intern re-hash. -6.6% insns, hashing -64% (deterministic callgrind).
- **Step 4 — dropped `TokenIdent::str`** in 5 gated tranches: T1 f98772ae (accessors +
  set_token_spelling), T2 679c3045 (parser 386 reads + const-ify lookup family),
  T3 53578349 (lexer/madc_program/cir reads), T-final-A fd540cab (spelling()→pool +
  intern-at-creation), **T-final-B d706c521 (member dropped)**. Bare identifiers now
  carry only a 4-byte interned rec.spelling_id (via `TokenBase::_active_strpool`), not a
  32-byte std::string. Measured **-43% madc token std::string ctors, -3.3% total insns**.
- **Docs**: `docs/parity/2026-06-30-frontend-profile-O0-vs-O2.md` (the full -O0/-O2
  profile + both result tables). Earlier Step-4 handoff
  (2026-06-30-rung1-step4-strdrop-lexerhash-HANDOFF.md) is now SUPERSEDED/DONE.
Every tranche gated: fulltest **673/0/0/16**, 0-warn, tag-arith 0/0, gcc-torture c17
failset == 51-name baseline (docs/parity/torture-failset-current.txt), zero regression.

## 2. Step-4 DESIGN (as landed — for anyone touching tokens)
- `TokenIdent` has NO `str`. `spelling()` is VIRTUAL: base resolves rec.spelling_id via
  `_active_strpool` (bound to the current Program's strpool in _tokenizer_init/_parser_init;
  compile is sequential per-Program incl. --project). Ctors intern at construction so every
  `new TokenIdent(name)` / make_ident / make_datatype works untouched.
- CONTENT / alias subclasses KEEP their own `std::string str` + override spelling():
  **TokenStr** (literal bytes — embedded NULs, mutated post-ctor by wide-conv),
  **TokenREM** (comment text), **TokenKeyword** (static keyword spelling),
  **TokenDataType** (spelling can differ from DataDef name: `_Bool`≠bool, `wchar_t`,
  `size_t`; static/shared across Programs → can't come from a per-Program pool).
- LESSONS banked in memory [[project_step4_strdrop]] (NUL-truncation via const char*;
  `== "lit"` must be `spelling_is` not pointer-compare; TokenMultiOp has its own str;
  const-ify read-only name params; datatype spelling≠DataDef.name). Honor them.

## 3. NEXT (pick with the user — nothing is mid-flight)
1. **`/release`** the 14 held commits to origin whenever the user wants (cuts a version,
   pushes develop). Mirror artifacts (claude_status.json/CHANGELOG/ROADMAP/README) update
   in the release per feedback_mirror_sync_cadence — they are NOT yet synced this session.
2. **More front-end perf** (the -O2-validated worklist in the profiling doc §"Updated
   priority"): (a) template INSTANTIATION = 72% of parse, the dominant structural lever
   (embedded-header-forest / instantiation caching — [[project_embedded_header_forest]]);
   (b) residual `std::map/set<string>` re-key (~1.4% at -O2); (c) `intern_table`
   over-reservation right-size (small dedicated pools fill 4096 buckets each).
3. **Rungs 2-7 are GATED behind develop→master parity** — do NOT start them
   ([[project_madc_vision]] / docs/plans/2026-06-29-madc-development-substrate-vision.md).
4. Task #4 (free-function overload symbol collision) still pending, unrelated.

## 4. Rehydration entry
Read `tmp/claude-coordinator-resume.md` (PICKUP v5) → this file → confirm HEAD `37354ff9`
on develop → ask the user whether to /release, pursue front-end perf (instantiation), or
something else. Do NOT auto-start a new track; rung 1 is a clean stopping point.
