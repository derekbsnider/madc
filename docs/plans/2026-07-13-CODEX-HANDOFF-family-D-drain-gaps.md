# CODEX HANDOFF — Family D drain-gap ladder: continue rung-by-rung to net-positive, then merge

**For:** Codex (GPT-5.6). **From:** Claude, 2026-07-13.
**Branch:** `feature/forest-sliceA-draingaps-claude` · **HEAD:** `1cfab946` · ALL gates GREEN (fulltest 694/0/0/16, release pack rc=0, packed suite 694/0/0/16).
**Branch ownership:** this branch is hereby handed off. Cut `feature/forest-sliceA-draingaps-codex` from `1cfab946` and work there (agent-suffix convention, `.claude/rules/branching.md`). Do NOT touch `develop` yet — see the merge criterion below.
**Strategic plan:** `docs/plans/2026-07-13-instantiate-bucket-plan.md` — read the "FAMILY D RUNG 1–11" stamp blocks; they are the campaign history and each ends with the next-rung recon.
**Status mirrors:** `claude_status.json` (current snapshot), `CHANGELOG.md` [Unreleased] (one entry per rung), KG milestone `forest_perf_ladder` (query: `bash scripts/kg_query.sh -ro "MATCH (m:Milestone {name:'forest_perf_ladder'}) RETURN m.summary"`).

---

## ⚡ START HERE — read this whole file + the plan doc's FAMILY D blocks, then execute §TASK.

### THE MISSION (one sentence)
The release binary's forest pack drains ~2700 deferred libstdc++ bodies at pack time; bodies that fail to drain are REVERTED to DEFBODY (lazy re-parse on consumer use — correct but slower); each failure is a real parser/CIR gap in plain C++ code — fix them at the deepest layer, ONE FAMILY PER COMMIT, driving the drop count down until it nets positive vs the campaign entry baseline (429), then merge the combined branch to `develop`.

### `/goal` (the contract — falsifiable end-state)
The reducer-corpus pack-drop count (see §METRIC) is **< 429** (currently **471**, entry was 429 before the ladder rungs began raising-then-lowering it — the trajectory this sitting: 483 → 480 → 471), with the full gate matrix green at every commit: `make -C src fulltest` = 694(+new tests)/0/0/16, `make -C src release` rc=0, `MADC_BIN=bin/madc-release bash scripts/run_tests.sh` = same counts as fulltest. Every rung ships a live `.mad` test byte-identical to g++. No name-keyed fixes (Rule #7).

---

## §METRIC — the ladder (measure before AND after every fix)

```bash
# Reducer corpus (43-byte '#include <fstream>' TU — tmp/_bf3.cpp, already present):
timeout 120 bin/madc --freeze=tmp/_bfNEW.msnap tmp/_bf3.cpp > tmp/bfNEW_freeze.log 2>&1
grep -c "pack drop" tmp/bfNEW_freeze.log          # THE number: currently 471
grep -c "c2mir check errors" tmp/bfNEW_freeze.log # check-gate subset: currently 41
```
Baseline log to diff against: `tmp/bfX_freeze.log` (471). Always `diff` sorted drop
lists old-vs-new: a fix must ELIMINATE its family and ideally add ZERO new drops
(a body ADVANCING to its next gap is acceptable — that is the ladder — but must be
recorded honestly in the commit message).

```bash
# Release corpus (fires inside `make -C src release`; the log is binary-ish — use grep -a):
grep -a -c "pack drop" <release build log>        # currently 509
```

## §TASK — rung 12 first, then the queue

**RUNG 12 (first move): `insert__o3` ×9 — "incompatible return-expr type in function returning a struct/union".**
- The failing bodies: `basic_string<C,T,A>::insert(const_iterator __p, size_type __n, _CharT __c)` (basic_string.h:1786), 5 std::allocator + 4 pmr variants. Body ends `return iterator(this->_M_data() + __pos);` — a struct-by-value return of a ctor-expression where `iterator` is the member typedef of `__gnu_cxx::__normal_iterator<pointer, basic_string>`.
- **Both naive live reducers PASS** (`tmp/red_ctorret_1.mad` plain class, `tmp/red_ctorret_2.mad` member-typedef ctor return) — the gap is PACK-DRAIN-CONTEXT-ONLY, like rung 11 was. Suspect the rung-11 disease pattern (see §LESSONS: arbitrary overload-set bind / drain-order-dependent resolution) or template-instantiation typedef resolution in the drain scope. Trace with the §TOOLS probes before forming a hypothesis.
- Done when: the 9 insert drops are gone from the freeze log, a live test pins the fixed shape (== g++), full gate matrix green.

**The queue after rung 12** (from the named check gate on `tmp/bfX_freeze.log`; re-census after each rung — counts shift):
1. `_M_construct__mti__o2` ×9-items (9 check errors EACH: "invalid types of comparison operands", "invalid type argument of unary *", "invalid operand types of +", "incompatible types in assignment to struct/union") — the `_M_construct(_InIterator, _InIterator, forward_iterator_tag)` member-template instantiation; iterator-type substitution produces a non-pointer type so every iterator op fails. char16/char32 identity variants dominate.
2. `seekp` / `seekp__o2` ×6 — ostream.tcc:283 "invalid types of comparison operands": fpos comparison (`__err == pos_type(off_type(-1))` shape) falling to a raw struct compare.
3. `__alloc_traits::_S_select_on_copy` ×3.
4. `basic_filebuf` family ×2 each (underflow/seekpos/seekoff/pbackfail/overflow/open/_M_seek) + `basic_ifstream/basic_ofstream::open__o2` (fstream:760:63 "incompatible argument type for pointer type parameter").
5. Bigger banked families (separate sittings): `__cerb` ×108 (parse-level .tcc definition drains — UNCHANGED by rungs 6–11, separate probe needed), wchar identity split (bogus `basic_istream<int32_t, char_traits<wchar_t>>` owners; reclaims ~24 trap stubs), emit-C extern-cout cleanup-attr hygiene (pre-existing, reducer `tmp/red_plain_io.mad`).

**Known pre-existing bugs banked OUT OF SCOPE** (task list #35/#36; do not chase unless a rung lands on them): polymorphic member-subobject vptr never initialized (live crash, reducer `tmp/red_arrow_8.mad`); dynamic Itanium vbase offsets (`while (s >> a)` on a real stream hangs — madc's vbase model is static; rung 8a's transitive fix is correct only for most-derived views).

## §TOOLS (all landed — use them, don't rebuild them)

- **Named check gate** (landed rung 11): every check-gate line in the freeze log prints the defective item's SYMBOL — `pack check gate: item N (symbol): K check error(s)`. Census one-liner:
  `grep -o "pack check gate: item [0-9]* ([a-zA-Z_0-9]*)" LOG | sed 's/.*(//; s/)//' | sort | uniq -c | sort -rn`
  ⚠️ Error LINES anchor at the CLASS HEAD for in-class bodies (basic_string.h:87:22 etc.) — attribution is by the named gate line FOLLOWING the errors, never by header line/col, and never by proximity to drop lines (that trap cost rung 11 a wrong attribution).
- **`MADC_CHECK_ATTRIB=1`** — same naming for the NORMAL compile path (madc_cir.cpp:476, throwaway deep-copy check).
- **FNTPL deduction tracer** (compile-time): `touch src/parser.cpp; make -C src -j6 CXXFLAGS="-std=c++11 -Wall -O0 -g -DMADC_DEBUG_FNTPL=1"` → every fn-template deduce/instantiate/rank/WINNER prints. Filter `grep "FNTPL"`. Rebuild WITHOUT the flag after (touch parser.cpp again) — do not commit a flagged binary.
- **Env probes** (committed, runtime-gated): `MADC_CREFA_PROBE` (translate_return class-assign detection), `MADC_MTCALL_PROBE`, `MADC_RETPROBE`, `MADC_MANIP_PROBE`, `MADC_DEBUG_CTORTMPL`. Add new probes env-gated in the same style; they stay committed.
- **Reducers live in `tmp/`** (gitignored) and MUST run with the original flags. Promote a reducer to `tests/NAME.mad` + `tests/NAME.expect` when its rung lands (fixture conventions: `.claude/rules/test-fixtures.md`).
- **g++ is the oracle** for every reducer/test: `g++ -x c++ file.mad -o out` — madc output must be byte-identical.

## §GATE (every commit)

```bash
make -C src fulltest                                   # 694+/0/0/16 (builds + unit + integration)
make -C src release                                    # rc=0; grep -a the log for the release drop count
MADC_BIN=bin/madc-release bash scripts/run_tests.sh    # packed suite — THE arbiter
```
Plus the ladder measurement (§METRIC) recorded in the commit message.
⚠️ NEVER rebuild/relink while fulltest runs (mid-suite binary swap = phantom failures).
⚠️ Verify `make` rc AND `bin/madc` mtime before re-testing a fix — a failed build + stale binary reads as "fix didn't work" (this trap fired TWICE this campaign).

## §LESSONS (paid for — do not re-learn)

1. **THE GAP LADDER:** drain bodies fail through SEQUENTIAL gaps. Fixing one family advances bodies to their next failure — drop counts can rise locally while the campaign nets down. Diff drop SETS, not just counts. Every drop is a correctness-neutral DEFBODY revert; the packed suite stays green throughout — it is the arbiter, not the check gate.
2. **Check gate ≠ last arbiter:** a fix can advance bodies past the c2mir CHECK into MIR-GEN fatals that only the release pack + `--run-frozen` self-exe gate catch (rung 6). Never skip the release leg.
3. **Ranked-callee typing (rung 11, SYSTEMIC):** the parse binds a call to an ARBITRARY member of a late-bound overload set (whichever instantiation registered last — drain-order dependent!) and defers ranking to CIR. Any code reading a call token's raw `datadef()` for TYPE decisions inherits the poison. The canonical helpers: parser side `Program::operand_value_datadef` / `resolved_call_funcdef`; CIR side `CirBuilder::operand_object_class` (now ranked-callee-aware), `call_target_funcdef`, `ref_returning_call_type`. If a drain-only failure smells like a wrong TYPE, suspect this first and probe the bound callee symbol name.
4. **Pack-drain failures that don't reproduce live** are usually (a) drain-order poison (lesson 3), (b) bodies only PARSED at drain (live uses mangled-direct dispatch to libstdc++, so the body parse never runs), or (c) drain-scope resolution differences. Build the live reducer anyway — if it passes, instrument the drain.
5. **`this` is a KEYWORD token** (tkCPPKEYWORD), not ttIdentifier — parse arms testing `type() == ttIdentifier` silently exclude it (rungs 9, 10 were both this disease in different arms). `contextual_identifier_name()` is the TokenBase-safe spelling helper (`spelling()`/`spelling_is()` are TokenIdent members — casting a keyword token to TokenIdent compiles in some arms and is UB).
6. **⚠️ `--run-frozen` does NOT validate method_map restore** — only grove-BIND consumers do; the packed suite catches what self-exe cannot.

## SETTLED — DO NOT RE-LITIGATE

- **Forest = SAVE STATE / LOAD STATE.** LOADED must EQUAL parsed. NEVER re-parse/re-derive at load, no parallel formats, NO new record families (owner constraint). Any fix that "just re-parses at bind time" is wrong by fiat.
- **Fix at the deepest layer, by KIND, never by NAME** (`AGENTS.md` Top-10 #2/#7; a fix keyed on "move"/"insert"/"basic_string" strings is WRONG). Shortcuts are categorically unacceptable — no shims over symptoms.
- **One family per commit**, full gate matrix per commit, ladder numbers + honest findings (pack-neutral, hypothesis-refuted, advanced-to-next-gap) in the commit message. Rungs 1–11 are LANDED AND VERIFIED — build on them, do not redo or refactor them.
- **Merge criterion:** merge to `develop` ONLY when the reducer-corpus count is below the 429 entry baseline and all gates are green. Not before.
- **Process:** single shell commands (no `&&` chains); scratch files ONLY in `tmp/`; NEVER `git add -A` (untracked `mir-debug-support.md` at repo root is foreign — stage explicit files); capped test runs (the Makefile targets already do this); update the plan doc stamp + CHANGELOG + `claude_status.json` + KG milestone at each rung close (mirror duties, `docs/agent-handoff.md`).
