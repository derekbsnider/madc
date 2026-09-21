# Self-Host Harness — Plan (ROADMAP 2.11, harness first)

**Status:** ACTIVE — owner go 2026-09-17 ("let's do it — after the compaction").
**Branch:** `feature/selfhost-harness-claude` (off develop `8f90efef0`).
**Owner:** Claude.
**Rules in force:** #1 (gcc/clang canon — every fix ships a reducer with the
g++ AND clang++ oracle), #2 (deepest layer), #6 (targeted tests per fix; the
battery ONCE at the seam), #7 (no per-TU specifics in the lane: the TU set and
the flags come from the Makefile, the caps are uniform), `parse-once`,
`rule-trailers` (every `src/`/`include/` commit), `fix-what-you-find`,
`no-parallel-implementations` (every unit runs under `timeout` + `ulimit -t`),
`report-coverage-not-slices` (the lane's output is a coverage TABLE).

---

## 1. Why

Every Nexus semantic service (code graph, navigation, propose + verification,
explain, intent) runs on madc's OWN parse tree. Developing madc through the
Nexus therefore needs madc to parse madc's C++11 source. Measured
2026-09-17 on the container, it does not: `ns_js.cpp` (291 lines) 265
errors, `madc_mangle.cpp` 38, `lexer.cpp` 338 errors in 57 distinct shapes,
188 of them the undeclared-identifier cascade behind a handful of roots.
The first blocker is universal — `extern thread_local bool madc_verbose;`
(`include/datadef.h:23`) fails in every TU — so the raw counts say nothing
about the real distance. A harness that measures per unit, after each fix,
is the only honest ruler.

## 2. What the harness is

`scripts/selfhost_lane.sh` (+ `make -C src selfhost`), a ratchet lane in the
shape of `scripts/c_testsuite_lane.sh`:

- **TU set = what the Makefile links into `bin/madc`.** `make -s print-OBJECTS`
  names the objects; `make -n -B <objects>` prints the real compile line of
  each. That includes the generated per-mode `embedded_headers.cpp` under
  `obj/` and the strict-C11 `src/rt/*.c` runtime (measured in `--std=c11`,
  because that is the `-std=` the Makefile hands them). Nothing is listed by
  hand: a TU added to the build joins the lane.
- **Flags are derived, never restated.** From each line the lane keeps what
  shapes the preprocessed text (`-I`, `-D`, `-isystem`→`-I`, `-std=X`→`--std=X`)
  and drops what is toolchain-only (`-c -o`, `-M*`, `-W*`, `-O*`, `-f*`, `-x`).
  A preprocessor-shaping flag madc cannot spell is reported, not dropped.
  The line is re-split by the shell (`eval set --`) because `make -n` prints
  shell-quoted text (`-DMADC_VERSION_STR='"0.99.2"'`).
- **Headers standalone.** One wrapper TU per madc-own header (`include/*.h`,
  `include/madcdis/`, `include/madcdat/`, `include/libmadc/`; not the
  script-facing `include/madc/` and not vendored `doctest.h`), compiled with
  the flags of the first C++ TU line. A TU's failure says "something in its
  include closure"; the wrapper says WHICH header, on its own.
- **Driver.** `bin/madc <flags> --emit=c11 <src> > /dev/null`, run from
  `src/` exactly as make does so `-I../include` resolves. There is no
  parse-only flag; `--emit=c11` is parse + sema + lower, and a TU without
  `main()` exits 0 (probed 2026-09-17). PASS = exit 0.
- **Caps.** Every unit runs under `timeout $CAP` AND `ulimit -t $CAP`
  (default 600 s: `parser.cpp` is 73k lines). rc 124 = wall timeout,
  152 = CPU cap. Units run 4-wide by default (`MADC_SELFHOST_JOBS`); the
  lane is ONE container job.
- **Ratchet.** `docs/parity/selfhost-baseline.txt` lists the units that fail
  today, one key per line. RED on any failure outside it; a listed unit that
  passes is shouted so the baseline only shrinks. A fix that turns a unit
  green deletes its line in the same commit.
- **Outputs** (`tmp/selfhost/`): per-unit `.err` (ANSI stripped),
  `results.tsv` (key, rc, secs, error count, first error), `shapes.txt`
  (normalized error-message shapes, then a coarse four-word tally),
  `files.txt` (errors by the file that reports them — TU vs header),
  `failing.txt` (baseline format). Stdout is the coverage table: one row per
  unit, then the top shapes and files, then the tally line
  `selfhost: P passed, F failed of T units (N outside baseline, M baseline
  units now passing)`.
- **Harness refusals.** Missing binary → exit 1. Zero derived compile lines
  or zero selected units → exit 2 (a broken harness must not read as green).
- **Targeted re-runs.** `MADC_SELFHOST_ONLY="lexer.cpp include/datadef.h"`
  runs just those units — the per-fix validation; the whole lane is the
  arc-seam gate.

Thread-safety contract: n/a — a shell lane; each unit is its own process.

## 3. Where it runs

On the container, in the background, from the host:
`scripts/remote_build.sh sync` then
`ssh -p 2299 dev@localhost 'cd /workspace/madc; nohup make -C src selfhost > tmp/selfhost_run.log 2>&1; echo DONE >> tmp/selfhost_run.log'`.
The QNAP never runs it. It is NOT wired into `fulltest` (cost: the giant TUs);
it is a ledger lane (`scripts/lane_ledger.sh record selfhost "<tally>"`) whose
`promote` tier is decided from its measured wall time once the baseline is in.

## 4. Order of work

1. Harness (this document, the lane, the Makefile target, the seeded
   baseline) — one commit, scripts + docs only.
2. First lane run → the coverage table is the gap list. Bank it in the
   status file and ROADMAP 2.11.
3. Fixes, in the order the lane surfaces them, EACH its own trailer'd commit
   with a `tests/` reducer and the g++/clang++ oracle, validated with
   `MADC_SELFHOST_ONLY` on the units it touches (the parser is months of
   battering: a behaviour change gets a reasoned hypothesis, one pass, and
   its reducer — never a probe loop):
   - `extern thread_local T x;` — storage-class-specifier order after
     `extern` (`datadef.h:23`; universal: unblocks every TU's first error).
   - pointer-to-member declarators / types — `void (C::*p)();`
     (`<functional>`'s `_Nocopy_types`; the 2026-06-26 audit's #1 gap).
   - `<unordered_map>` internals, then `tokens.h` / `madc.h` shapes as the
     lane re-ranks them after each fix.
4. Seam = the chosen unit set green (target for THIS arc: every
   `src/*.cpp` TU and every own header PASS; the baseline empty). ONE battery
   + lane records + develop merge there, never per fix.

## 5. Harness lessons (first three runs, 2026-09-17)

Each of these read as a plausible result until checked; each is now a gate
inside the lane.

- **A sub-make prints "Entering directory" lines.** Under `make -C src
  selfhost` the inner `make -s print-OBJECTS` polluted the object list; the
  dry-run derived no compile lines and the lane refused (exit 2 — by
  design). Both inner makes run `--no-print-directory`; make's stderr is
  kept for the refusal message.
- **madc treats every argument after the source as the program's argv.**
  `--emit=c11` appended after the source was not a flag: every unit that
  parsed was RUN ("madc_cir_execute: main() not found", rc=1, zero error
  lines on 40+ units). Flags, then `--emit=c11`, then the source.
- **A diagnostics loop fills the disk.** `<regex>` (ns_perl.cpp) looped in
  error recovery: 2,072,861 error lines, 831 MB, and a container load spike
  when the tally sorted them. Each unit's stderr passes through a byte cap
  (`MADC_SELFHOST_ERR_CAP`, 32M); the loop itself is KG Gap
  `regex_header_recovery_loop`.
- **The subshell's status was `wait`'s, not madc's.** Run 2 read 172/172
  green. The subshell now exits with madc's rc, and a unit with diagnostics
  and exit 0 is rc 99 / `INCO`, never a PASS. An evidence run can be blind:
  a tally that is too good after a change is a harness signal first.
- **A `cir error:` line has no location.** The per-unit error count and the
  shape tally anchored on `: error: `; a tsubst bail prints `cir error: ...`
  bare, so 24 failing units read "0 errors" with an empty first-error column
  and the second-largest family was invisible to the ranking. Both greps
  accept either form (490c08935).
- **`remote_build.sh ... | tail` reports tail's exit status.** Three builds in
  a row "succeeded" with `build madc rc=2` in the stage summary while the
  container kept the previous binary — the reducers "still failed" against
  a stale build. Read the stage summary lines, never the pipeline's rc.
- **A lane ratchet freed by one fix may read green for the wrong reason.**
  The 12 channel units freed by the pack-expansion commit had the tuple
  shape as their FIRST error only; a reducer per fix (with its g++/clang++
  oracle) is what proves the fix, the lane only ranks what is left.

## 6. Not in scope

Running the emitted C (self-compilation to a working binary), linking,
`--project` over the whole build, and the Nexus services themselves. The arc
is the parse; those are the next ones and this lane is their ruler too.
