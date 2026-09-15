# #3 — the master promotion gate (runbook)

**For a freshly compacted session.** Rehydrate first: `bash scripts/resume.sh`,
then read this file. Owner instruction (2026-09-15): *"merge
feature/win-run-env-forward-claude and build / test / etc everything required
for the promotion gate."* This runbook takes develop from #2-merged to
**`check --release` rc=0** — every lane green on ONE promotion HEAD — and then
hands `/promote` to the owner (master promotion is the owner's decision, and a
command handed to the owner ENDS the turn: `feedback_yield_prompt_for_owner_command`).

## SETTLED STATE (evidence — do not re-derive)

- **develop = a6d9ee110** (after this was written; verify with resume.sh). #2
  (the madcgit module on the Windows/macOS cross targets) is MERGED (merge
  `f4341e0d7`) and pushed. All FOUR develop-gated lanes were green on
  `2238597e6` (== the #2 merge tree), `check --promote` rc=0:
  linux-battery 1379/0/9skip + exe/obj 1312/0 + packed + headerless 1345/0/43skip;
  c-testsuite 220/220; wine64 1319/0/69skip; macos both arches 836 units.
- **The genuine-win harness fix is NOT yet on develop.** It is commit
  **`41850fbf2`** (`test(win): forward the test .env into the native Windows
  process`) on branch `feature/win-run-env-forward-claude` — a 19-line change to
  `scripts/win_run.sh` ONLY (stage mode forwards every `MADCIDE_*` var into the
  Win32 child via WSLENV; without it `testmadcide_discover` crashes on the real
  box — wine hides it). develop's `win_run.sh` == the merge-base's (b794e7a6b),
  so **the cherry-pick is clean** (no competing change).
- **Do NOT `git merge` the whole branch.** It forked before #2 (merge-base
  b794e7a6b), so a merge conflicts on `lane-status.tsv`/`claude_status.json`/
  `CHANGELOG.md` and would drag in its second commit `375039347` — a SUPERSEDED
  reactor-era lane-status row. Cherry-pick `41850fbf2` alone; that IS "merge the
  harness fix."
- **The release tier (gates master via `check --release`) is STALE from
  2026-09-09** on all three: `darwin-suite`, `libcxx`, `genuine-win`. They must
  all re-run on the promotion HEAD.
- **The lane-freshness gate is all-CODE_PATHS, not per-lane** (`lane_ledger.sh`
  line 79: `git diff --quiet <recorded> HEAD -- src include third_party tests
  scripts tools examples`). So the `win_run.sh` cherry-pick — a `scripts/`
  change — **stales EVERY lane**, including the four just made fresh. There is
  no shortcut: all seven lanes re-run on the promotion HEAD. This is
  unavoidable given the conservative gate + the owner's instruction to land the
  harness fix; it is not waste, it is the promotion validation on the final
  content. (Never `MADC_PUSH_NOGATE=1`; never record a lane without a real run.)
- **The gcc-torture promote gate** (branching.md): class-(a) standard-C
  failures fixed (≥1608/1652), the 33 class-(c) formally skipped. MET at prior
  promotions (v0.69.0 … v0.99.2). #2 did not touch the compiler, so it should
  still pass; RE-VERIFY on the promotion HEAD (no regression), don't assume.

## TASK SEQUENCE (imperative)

### 1. Cherry-pick the harness fix → the promotion HEAD (H)
```
git checkout develop
git fetch origin ; git rev-parse develop origin/develop   # must match; abort if develop moved
git cherry-pick 41850fbf2                                  # scripts/win_run.sh only — clean
```
H is now `develop` HEAD. Do NOT push yet — the pre-push hook runs
`check --promote`, which the cherry-pick just made STALE. Push after step 2.

### 2. Re-run the four develop-gated lanes on H, record, then push develop
One heavy job at a time. Each records with `scripts/lane_ledger.sh record <lane> "<tally>"`.
```
bash scripts/remote_build.sh battery         # linux-battery — expect fulltest 1379/0/9skip, exe/obj 1312/0, packed, headerless 1345/0/43skip
ssh -p 2299 dev@localhost 'cd /workspace/madc; bash scripts/c_testsuite_lane.sh'   # c-testsuite — expect 220/220
# wine64: rebuild the win exe + module from H first, then the domain suite:
ssh -p 2299 dev@localhost 'cd /workspace/madc; make -C src hosted-x86-64-windows; make -C src madcgit-windows'
bash scripts/remote_build.sh wine            # wine64 — expect 1319/0/69skip (the 3 madcgit tests pass)
bash scripts/remote_build.sh release-macos   # macos BUILD lane — both arches 836 units, verify + package OK
```
Record each green, then:
```
git push origin develop                      # pre-push check --promote must be rc=0
```
(`remote_build.sh battery` includes its own sync; for the ssh-direct lanes,
`remote_build.sh sync` first so the container has H.)

### 3. The three release-tier lanes on H
- **libcxx** (mine, container): the whole suite under `-stdlib=libc++`, JIT+exe+obj.
  ```
  bash scripts/remote_build.sh libcxx        # expect ~jit 1330/0/14skip, exe/obj 1271/0 (the .libcxx_skip fixtures)
  ```
  ⚠️ RISK/FIRST-RUN: the libcxx MODE now also builds the madcgit module (#2).
  If `libmadcgit.so` fails to build under libc++, that is a real finding — fix
  it (fix-what-you-find), do not skip it.
- **genuine-win** (mine — `feedback_genuine_win_lane_is_mine`; needs the owner's
  Windows 11 box online and the harness fix from step 1):
  ```
  # probe the channel first:
  ssh -p 2299 dev@localhost 'ssh -o BatchMode=yes derek@host.docker.internal uname -a'
  # remove the vscode node_modules on the container (breaks scp -r tools):
  ssh -p 2299 dev@localhost 'rm -rf /workspace/madc/tools/vscode-madcide/node_modules'
  # after release-windows built the packed PE (release-win stage), run the suite:
  bash scripts/remote_build.sh release-win   # builds the stripped, forest-packed PE the suite runs
  ssh -p 2299 dev@localhost 'cd /workspace/madc; bash scripts/win_suite.sh'   # MADC_SKIP_EXT="win64 win" is set internally
  ```
  ⚠️ RISK/FIRST-RUN: the packed win release now ships `bin/madcgit.dll`; the 3
  lifted tests run on the REAL box for the first time with the static libgit2.
  wine passed them, but genuine-win is the true oracle (wine only approximates
  WSA + the WSL/Win32 boundary). Classify by OUTPUT MARKER, not rc (WER swallows
  crashes). Expect ~1318→1321 (the 3 tests now pass) — verify the actual tally.
- **darwin-suite** (the owner's Mac runners via GitHub Actions — needs them online):
  ```
  gh workflow run darwin-probe.yml -f build_ref=develop -f suite_gate=true
  gh run watch    # or gh run list --workflow=darwin-probe.yml
  ```
  ⚠️ RISK/FIRST-RUN: the darwin release now ships `lib/libmadcgit.dylib`; the 3
  lifted tests have no macOS skip, so they run on real macOS for the first time.
  The relpath forward-slash fix is a no-op on macOS (paths already forward-slash),
  so they should pass — verify.

Record each: `scripts/lane_ledger.sh record darwin-suite|libcxx|genuine-win "<tally>"`.

### 4. Re-verify the gcc-torture promote gate on H
Read `docs/parity/failset-classification.md` for the procedure; run
`scripts/run_gcc_testsuite.py` and confirm no class-(a) regression against
`docs/parity/torture-failset-current.txt`. #2 didn't touch the compiler; this
is a no-regression check, not new work. If a class-(a) test regressed, that is
a blocker — fix it.

### 5. The gate is green — hand /promote to the owner
```
bash scripts/lane_ledger.sh check --release      # MUST be rc=0 (develop set + release tier all fresh on H)
```
When rc=0: report the full lane table and tell the owner **/promote is theirs to
run** (it re-runs `check --release` and pushes master). Do NOT run /promote or
push master yourself — master promotion is the owner's decision
(`branching.md`, `feedback_all_platform_lanes_before_master`). End the turn on
that hand-off (`feedback_yield_prompt_for_owner_command`).

## External dependencies (may block, not fixable here)
- **genuine-win** needs the owner's Windows 11 box reachable at
  `derek@host.docker.internal` from the container. If the probe fails, report it
  and hold that lane.
- **darwin-suite** needs the owner's self-hosted Mac runners online for
  `darwin-probe.yml`. If the run queues with no runner, report it and hold.
- If either is blocked, everything else can still be driven green; `check
  --release` stays red until they land. Say so plainly; do not fake them.

## Updated-files checklist at the end
`docs/lane-status.tsv` (7 lane records at H), `claude_status.json` (UPDATE 32:
gate green / blocked-on-X), `CHANGELOG.md` if a fix landed, the memory banner.
Mirror per `feedback_mirror_sync_cadence`.
