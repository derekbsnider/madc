# HANDOFF — macOS lane session #81 (2026-08-11)

Read this FULLY after compaction (compaction-rehydration law: assume cold
start). `claude_status.json` live_handoff carries the same facts in
compressed form; THIS file is the authoritative session-crossing record.

## SETTLED — do not re-derive, do not re-litigate

Session #81 shipped THREE arcs on `feature/macos-release-lane-claude`,
all validated and pushed through **3bbf3479**:

1. **W0.5 — provenance-clean darwin prelude** (`d7ada73e` + trim
   `8e3b57cd`): the embedded C prelude is flattened from the pinned
   zig-0.16.0 open header tree (`scripts/fetch_darwin_open_headers.sh`
   = the pin's ONE owner; sysroot at
   `/workspace/darwin-open-headers/sysroot-any-darwin-any`). First diff
   vs the SDK oracle: ZERO declaration gaps (both arches, identical
   50-line version-drift sets, all counterparts present). Binaries bake
   their provenance into `.rodata`
   (`MADC-DARWIN-PRELUDE-PROVENANCE:` marker);
   `verify_macho_release.sh` authority 4 refuses sdk-private/missing
   (negctl'd on the pre-W0.5 binary). License audit: 240-file closure =
   213 APSL + 21 BSD + 5 Apple-copyright shims + 1 public domain;
   notices ship in `THIRD_PARTY_NOTICES/` (`docs/licenses/`). The SDK
   stays build-side only (linking + never-shipped oracle diffs).
   **PUBLIC macOS artifacts are UNGATED.**
2. **W3 — libmadc_rt** (`04b78626`): tarballs ship `lib/libmadc_rt.a`
   = the ledger pair (`scripts/ledger_sources.txt` stays the one list
   owner; `src/rt/` untouched — a re-archive, zero new code) for
   emitted-C consumers (`cc program.c -L<dir>/lib -lmadc_rt -lc++`).
   Completeness audited mechanically: 15 emitter-referenceable
   `need_output_extern("__madc_*")` symbols = 13 ledger exports + the
   `<ns_madc>` sys pair (full-runtime by the ledger membership rule;
   named limitation in README-macos.txt). Gates: `emitc_sret_gate.sh`
   leg 2b (archive-ALONE link + rt-negctl that must fail on a
   `__madc_` symbol) in fulltest; battery leg 6c on-target.
3. **Darwin AOT C++ — flat dylib binding** (`708abad6` + fork
   `fde19e17`): `madc -static-libmadc -o` C++ programs now RUN on the
   Mac. Fork `mir-macho.c`: with `params->needed` extras the bind
   stream opens flat-namespace (`SET_DYLIB_SPECIAL_IMM|0xE` = −2, the
   `-undefined dynamic_lookup` shape — no .tbds on a header-less Mac);
   NO extras ⇒ stream unchanged (two-level libSystem). madc:
   `cir_apple_extra_dylibs` adds `/usr/lib/libc++.1.dylib` on
   Itanium-mangled imports (import-CLASS trigger, hosted+cross
   identical); the two Apple emit lanes' DRIFTED refusal copies
   consolidated into `cir_apple_runtime_refused`. Gate:
   `scripts/macho_exe_dylib_gate.sh` (in `machogate` + the
   release-macos recipe), 8/8; NEGCTL on real bytes: the pre-slice
   `otry2` executable fails every leg-B check (same 129 imports,
   two-level→flat). Battery leg 6d green ON HARDWARE ("aot c++ ok").

**Darwin AOT `-o` now covers**: runtime-free C, C-lane-runtime static
(try/catch/VLA via the forest-carried ledger), AND C++ programs.
Session probes also proved: runtime-free `-o` and C-lane
`-o -static-libmadc` worked pre-slice (plan doc has the 4-probe table).

## VALIDATION EVIDENCE (final, at 8efb3360/3bbf3479)

fulltest 1019/0 (9 skipped; emitc_sret_gate leg 2b in-suite);
libcxxjit 1015/0 (13 skipped); unit 9019/9019; macho_exe_dylib_gate
8/8; EXE 990/0; OBJ 990/0; release rc=0; packed 1019/0 (9 skipped).
Mac battery (arm64 15.3.2, tarball at the AOT slice): **8/2 of 10** —
fails are EXACTLY the two known-opens (groves `os.str()` husk =
FEATURE_FOREST_ALIAS_SHELL_COMPLETE layer; value-intrinsic SIGSEGV).
Latency info-leg ~232ms (in family).

## STATE

- Branch `feature/macos-release-lane-claude` @ **3bbf3479**, pushed to
  `derekbsnider/madc`. Working tree clean EXCEPT three owner-untracked
  files — NEVER `git add -A`: `test.mad`, `testsort.mad`,
  `docs/plans/mir-into-madc-repo-2026-08-11.md` (the owner's draft —
  do not edit, do not commit unless directed).
- Fork `/workspace/mir` on `feature/indirect-return-attr` @
  **fde19e17**, pushed to `derekbsnider/mir` (verify remote before ANY
  push — 7 remotes, origin NOT first). `MIR_COMMIT` = fde19e17...
  NOTE: this fork branch has NOT merged to fork `develop` — lockstep
  merge happens when the madc branch merges (or is mooted by the
  migration below).
- Mac: `ssh -o BatchMode=yes derek@192.168.1.79`, `export LC_ALL=C;
  export LANG=C` in EVERY remote command (perl panics otherwise).
  Staging `~/madc-s81` (per-session `~/madc-sNN` pattern).
- Container: all builds/tests via `scripts/remote_build.sh` (stages:
  sync/fulltest/libcxxjit/exe/obj/release/packed/release-macos/
  package-macos) or `ssh -p 2299 dev@localhost`. QNAP never builds.

## PENDING — owner decisions, in order

1. **Merge ⇒ release** (per cadence: feature merge comes WITH a
   release). The branch is DONE. This release: darwin tarballs go
   PUBLIC for the first time; the fork changed this cycle ⇒ the LAST
   `MIR_VERSION` bump + annotated fork tag at fde19e17 (per
   `.claude/rules/build.md`) — "last" because of item 2.
   Release-notes candidates: public macOS tarballs; darwin AOT `-o`
   for C and C++; libmadc_rt; the provenance story.
2. **MIR subtree migration** (owner's draft:
   `docs/plans/mir-into-madc-repo-2026-08-11.md`, 31 sections): move
   the fork into `third_party/mir` via subtree with preserved history;
   retire MIR_COMMIT/MIR_VERSION/lockstep; `derekbsnider/mir` becomes
   historical + upstream-PR vehicle. **My delivered opinion: DO IT,
   release first, migrate second** (a quiet released baseline to diff
   against; migrate-first makes a three-feature release the first
   exercise of the new topology). The owner had not yet decided at
   handoff time.

### Migration review notes (delivered in-conversation; fold into the
### draft when executing — they exist nowhere else)

1. §4.1 pin freshness: MIR_COMMIT moved TODAY to fde19e17 — read it at
   execution time, never fossilize a SHA in commands.
2. Build-dir hygiene: per-target libmir builds live INSIDE the mir
   tree (`build-*/libmir.a`, 6+ variants, created by madc's recursive
   makes with `BUILD_DIR=`). Under `third_party/mir` redirect them
   (e.g. into obj/) or gitignore — else the first build dirties the
   subtree and defeats §4.4's ordinary-source invariant.
3. §25 validation must add the darwin lanes: release-macos (verify +
   macho_exe_dylib_gate), machogate, libcxxjit, a Mac battery run —
   the fork content includes the Mach-O writer. Plus one NEGATIVE
   CONTROL: after rewiring, remove/rename `/workspace/mir` on the
   container and prove the build still succeeds (silently picking up
   the stale sibling checkout reads as green — the house
   stale-artifact class).
4. `remote_build.sh` loses its `sync mir` leg; provisioning simplifies
   to one repo restore. Name both in §27.
5. In-flight upstream PRs mir#461/#462/#463 pend with Vladimir on
   `derekbsnider/mir` — §18's freeze is fine, leave those branches
   untouched until resolved.
6. `/release` + `/promote` skills carry fork-lockstep steps §13
   retires — name them (and AGENTS.md's build section) in §23's doc
   list.

## OPEN on the lane (recorded, queued — none block release)

- Groves husk (`os.str()` battery red) — FEATURE_FOREST_ALIAS_SHELL_COMPLETE layer.
- Value-intrinsic SIGSEGV on darwin (fix direction = the owner's
  always-included-intrinsics law).
- libmadc.dylib — the full-runtime dynamic AOT class (madc::value/sys).
- In-process Mach-O `.o` loader (the `-c` round-trip; battery AOT leg
  correctly reads the loud decline meanwhile).
- Known+recorded elsewhere: default-lane getloc-husk body defect
  (`free(): invalid pointer`), cast type-name gap.

## GOTCHAS the next session must not rediscover

- Battery is now **10 checks**; expected on current tarballs: 8/2.
  A pre-W3 tarball reads a leg-6c FAIL (predates-reason); a pre-slice
  binary reads a leg-6d FAIL (dyld missing __ZNSt3__14coutE).
- `macho_exe_dylib_gate.sh` SKIPs without the cross madc
  (`make -C src cross-arm64-macos`) or
  `obj/hosted-arm64-macos/forest.bin` (`make -C src release-macos`).
  The forest-less cross madc needs `--forest-bind=<forest.bin>` for
  any `-static-libmadc` work.
- Ledger symbols in image symtabs spell with THREE leading
  underscores (`T ___madc_try_push` — Mach-O `_` + the symbol's two).
- `--emit` + `-v` must never combine (stdout pollution); env probes
  instead (`MADC_XTEST_FACET` etc.).
- Push gate = fulltest + libcxxjit; session end = EXE+OBJ+release+
  packed (ALL FOUR). Trailers on every src/include commit
  (check-rule-trailers gates it).
