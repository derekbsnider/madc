# Darwin-suite blockers — compaction handoff (2026-09-16)

**For a freshly compacted session.** Rehydrate: `bash scripts/resume.sh`, then
read this. This continues the **#3 master-promotion gate**
(`docs/plans/2026-09-15-master-promotion-gate-runbook.md`). The libc++ format
bug the owner asked to fix is DONE; three **darwin-specific** bugs now block the
`darwin-suite` release-tier lane. All three are confirmed real madc bugs vs the
darwin-SDK clang oracle — none are `.darwin_skip` candidates.

## SETTLED STATE (evidence — do not re-derive)

- **develop (local) = `58f250811`.** Commit chain on top of origin/develop
  (`7862f951e`, which has the win_run.sh cherry-pick + the 4 develop-gated lane
  records):
  - `478f007c6` — darwin libgit2 staging fix (`stage_libgit2.sh` reads the SDK
    from the MODE via `print-MACOS_SDK`; `darwin-probe.yml` stages libgit2 +
    `LIBGIT2_DIR`). **Validated:** the darwin build (`make release-macos`) +
    hello gate now PASS on the GitHub Mac runners (was failing at `madcgit-deps`).
  - `6370ac2e9` — **THE FORMAT FIX (the master ask — DONE).** `cir_format.cpp`
    classifies a format arg by its **CIR-resolved callee** (`call_target_funcdef()
    -> return_value_type()`), not the parse-bound `arg->datadef()`. Root: a
    polyglot public (js::stringify/perl::substr/php::str_repeat) is declared
    `std::string&` but LOWERS to its lean `const char*` form; the two diverged
    under libc++, so format took `&<const-char*-returning call>` ("lvalue
    required" / "no formatter for pointer type basic_string*"). Reducer
    `tests/teststdformatstringref.mad`. **Validated:** libcxx lane GREEN on this
    content — JIT **1375/0/14skip**, exe 1308/0, obj 1308/0 (was 1342/32). Oracle
    (g++, clang++/libc++, clang-18 -stdlib=libstdc++ vs -stdlib=libc++): identical
    — NO expected library difference; it was purely a madc bug (owner-confirmed).
  - `58f250811` — **darwin blocker #1** (madcgit dylib flat-path). `madcgit.mk`
    build rule, gated `ifdef HOSTED_DARWIN_TARGET`, also `cp -f $@
    $(LIBDIR)/libmadcgit.dylib`. NOT yet darwin-validated.
- **origin/develop = `7862f951e`** (behind local by the 3 commits above — do NOT
  push develop until the gate is green; the pre-push hook runs `check --promote`).
- **feature/madcgit-darwin-ci-claude = `6370ac2e9`** (pushed; the darwin-probe
  transport ref — UPDATE it to the latest HEAD before re-dispatching darwin).
- **darwin-suite last real run (`gh run 35033878993`, feature @6370ac2e9):**
  build PASS, hello gate PASS, JIT **1357 passed / 8 failed / 24 skip**; EXE 294
  advisory (D5, not gating). The 8 JIT failures = the 3 root causes below.
- **Documented darwin baseline was 0 JIT failures** (lane-status.tsv: arm64
  1319/0, Intel 1320/0 @ d3e979c5, 2026-09-09). The 8 are NEW regressions since
  — the V6/reactor/nexus/madcgit arcs merged WITHOUT re-running the darwin lane
  (it only runs at promotion). Per the darwin-host-port plan: "triage first-run
  failures root-cause, no bulk skips."

## THE THREE DARWIN BLOCKERS (root causes; oracle = clang with the macOS SDK)

Darwin-SDK clang on the container is the faithful oracle (Linux glibc headers do
NOT reproduce darwin's declarations):
`clang++-18 -target x86_64-apple-macos12 -isysroot /workspace/sdk/MacOSX.sdk -nostdinc++ -isystem /workspace/sdk/MacOSX.sdk/usr/include/c++/v1 -std=c++20 -fsyntax-only FILE`

### #1 — madcgit dylib flat-path (testgit, testgraphpast, testnexus_layers) — FIX WRITTEN (`58f250811`)
`madc_module_open` (src/madc_modules.cpp) looks for the image FLAT at `<binary
lib dir>/libmadcgit.dylib` (like Linux's flat `lib/libmadcgit.so`); the darwin
build nested it (`lib/madcgit/<arch>-macos/`). testnexus_layers is downstream —
no git module ⇒ its output drops the `versioned` layer (the `.expect` diff).
**NEXT: darwin-validate** (should clear all 3).

### #2 — darwin POSIX shadows the user's own symbols (testmadcide_discover, _attach, _lsp_serve, _lsp_stdio) — NOT FIXED
Two sub-bugs, both compile errors on darwin; both resolve CORRECTLY under
clang-darwin-SDK (oracle `tmp/shadow.cpp` compiles clean):
- **2a** `c.write(js.c_str())` (c is `madc::channel&`) lowers as UFCS
  `write(c, …)` → POSIX `write(3)` ("expected 3 got 2"). It must bind the
  **method** `madc::channel::write(const char*)` (include/madc/ns_madc:601). UFCS
  should not fire when the method exists — darwin method-lookup bug.
- **2b** `send(relay, init)` → POSIX `send(4)` ("expected 4 got 2"). It must bind
  the test's own `void send(madc::channel&, var&)` (testmadcide_discover.mad:27);
  POSIX `send(4)` is not viable for a 2-arg call — darwin overload-resolution bug.
Linux madc resolves both correctly; investigate why darwin prefers the POSIX
system declarations (likely darwin prelude / system-header scoping).

### #3 — import drops darwin's extern "C" (testimportiface) — NOT FIXED
`import m;` (import `<math.h>`, --std=c++20) emits `_Z4sqrtd` (C++-mangled) →
`MIR error: import of undefined item _Z4sqrtd`; darwin libm exports C `sqrt`.
Darwin's `<sys/cdefs.h>` defines `__BEGIN_DECLS` = `extern "C" {` under
`__cplusplus` (SDK line 71), and `math.h:439 extern double sqrt(double);` sits
inside `__BEGIN_DECLS`. So `sqrt` IS extern "C" — madc's `import` parse isn't
honoring darwin's `__BEGIN_DECLS`/extern "C" (glibc's works ⇒ Linux binds C
`sqrt`). `__cplusplus` IS predefined (predefined_macros.cpp:442 = 201703L), so
the miss is likely that the darwin import doesn't process `<sys/cdefs.h>` /
apply its `extern "C"`. Investigate `Program::tokenize_import_directive`
(src/lexer.cpp:1750) + how imported-header linkage is tracked.

## TASK SEQUENCE

1. **Fix #2 and #3** (darwin header handling — the deep pair). Reproduce with the
   darwin-SDK clang oracle for expected behavior; the madc darwin PARSE likely
   needs a native-darwin round-trip to reproduce (Linux madc resolves correctly).
2. **Validate all darwin fixes together** — one darwin round-trip:
   `git push origin develop:feature/madcgit-darwin-ci-claude --force`
   `gh workflow run darwin-probe.yml --ref feature/madcgit-darwin-ci-claude -f build_ref=feature/madcgit-darwin-ci-claude -f suite_gate=true`
   `gh run watch <id> --exit-status` (⚠️ the `tee` wrapper masks rc — read the
   log's own rc line + the suite summary; classify JIT vs advisory-exe).
   Target: darwin JIT back to 0 failures.
3. **Re-run ALL 7 lanes on the final HEAD** (the format + darwin fixes are real
   code all lanes exercise — full re-run is genuine, not ceremony): linux-battery,
   c-testsuite, wine64 (rebuild win exe+madcgit.dll first), macos, libcxx,
   genuine-win, darwin-suite. Record each in `docs/lane-status.tsv`.
   - libcxx already GREEN on `6370ac2e9` content (JIT 1375/0) — re-run on the
     final HEAD to record fresh.
4. **gcc-torture** re-verify (no class-(a) regression — the format fix is the only
   compiler change; it's format-only).
5. `bash scripts/lane_ledger.sh check --release` rc=0 → report the table, HAND
   `/promote` to the owner (never run it; a command handed to the owner ends the turn).

## STANDING CONSTRAINTS (unchanged)
Push only to owner remotes (origin = git@github.com:derekbsnider/madc.git); no
`&&` chains (use `;`); one Bash call = one simple command; QNAP never builds
(container `ssh -p 2299 dev@localhost` via remote_build.sh); one heavy container
job at a time; darwin runs on GitHub-hosted Macs (macos-14 arm64, macos-15-intel);
genuine-win is mine (win_suite.sh, owner's Win11 box `derek@host.docker.internal`
— UP as of this session); never MADC_PUSH_NOGATE=1; master promote = owner decision.
