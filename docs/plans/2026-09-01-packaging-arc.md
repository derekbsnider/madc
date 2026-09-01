# Packaging arc — thin CLI + shared libmadc as the default install

**2026-09-01, owner-directed.** Successor to
`docs/plans/2026-07-25-forest-carriers-plan.md` — all the S1–S6
machinery this arc rides shipped there; that plan's "Decided defaults"
section is SUPERSEDED by the rulings below.

## Goal

Flip the default install from the monolithic single binary to the
PACKAGED shape: a thin `madc` CLI dynamically linked against a shared
`libmadc` that CARRIES the frozen forest (discovery arm 2), plus config
+ man pages, delivered as per-OS packages (Linux / macOS / Windows).
The monolithic all-in-one binary remains a first-class OPTIONAL form.
Owner emphasis (2026-09-01): the main thing is the right library and
packaging shape, and all the proper build scripts and tests in order.

## Rulings (owner, settled — do not re-litigate)

- **Default install = packaged** (2026-08-31): thin CLI + shared
  libmadc carrying the forest; monolithic = optional form, no longer
  the default.
- **ABI stance** (2026-09-01): **C ABI stable, C++ version-locked.**
  The `extern "C"` C-host API (`.claude/rules/cpp-first-api.md`)
  carries the soname promise — the soname major bumps only on a C-ABI
  break. The C++ surface (mangled-direct namespace publics, the
  template headers) makes no cross-version promise: C++ embedding
  hosts rebuild per release. The forest blob is already version-locked
  to its library (one-format/one-loader law) — library + forest move
  as one artifact.
- **soname**: stays `libmadc.so.0` (pre-promise) until the PK1
  C-surface audit is green and the owner calls it; `.so.1` opens the
  promise era.
- **One package**: the thin CLI and the library ship in the SAME
  package — they can never skew (no cross-package exact-version dep
  machinery).
- **Format order** (2026-09-01): Linux deb + rpm + tarball first
  (a re-target of the existing `package_release.sh`, not new format
  machinery); macOS tarball now, brew later; Windows zip now,
  **Microsoft Store (MSIX) when the time comes** — the owner has an
  existing Store presence (WSLTux,
  https://www.microsoft.com/store/apps/9P781RW2VM6G) and ruled that
  route (2026-09-01). A Store listing is auto-surfaced through
  winget's `msstore` source, so winget discovery comes free; a
  community winget manifest for the plain zip is optional, not a
  slice.
- **GitHub Actions before ecosystem repos** (owner 2026-09-01): the
  release-build workflow (PK5) precedes brew / winget / apt-yum (PK6)
  — bottles, manifests, and repo metadata all consume its artifacts.
- **The packages ship a compiled `madcide` binary** (owner
  2026-09-01): madcide is part of the packaging, not a trailing
  optional. The packaging pipeline compiles it with the just-built
  madc itself (the AOT `--exe` lane), linked against the shared
  libmadc — the dogfood proof of the packaged shape. This folds the
  old "madcide-as-binary" open question into PK3/PK7 below.

## Inventory — what already exists (this arc builds on, not from zero)

- S1–S6 forest carriers, all gated (2026-07-25 plan): `--enable-shared`
  link shape, `--with-forest=embedded|sidecar|none`, the 5-arm
  discovery chain (self-image → **library image** → sidecar →
  `$MADC_FOREST` → `madc.ini` arm 5), loud-fallback CLI / strict
  embedding-host policy, the strict `madc.ini` reader,
  `-static-libmadc` ledger including the `.o` link lane.
- `scripts/package_release.sh` builds .deb + .rpm today (validated at
  every release; the shipped v0.95.2 rpm even served as a regression
  oracle) — but it stages the MONOLITHIC `madc-release` as
  `/usr/bin/madc` with `libmadc.so.0` merely alongside.
- `scripts/package_release_macos.sh` / `package_release_windows.sh`,
  `verify_macho_release.sh` / `verify_pe_release.sh`.
- `make -C src install-libmadc` staging target; the S2 in-house
  Mach-O re-signer.

## Slices

- **PK0 — cold-start measurement (the flip gate).** Interleaved A/B on
  the container: packed monolithic vs packed shared (thin CLI +
  forest-in-library). Cases: hello cold start, madcide start, an
  embedding host. TSV trend + callgrind Ir beside wall time (wall time
  alone untrustworthy). The flip does not ship before the numbers are
  seen; a material regression is a finding brought to the owner, never
  silently accepted.
- **PK1 — C-ABI surface audit + policy. ✅ EXECUTED 2026-09-01.**
  The complete extern-C surface (1088 unmangled dynamic exports)
  classified: 94 ABI (every `madc_api.h` declaration exported), 33
  `madc_*`-prefixed internals + 8 misc allowlisted with dispositions
  (`scripts/c-abi-internal-exports.txt`), the rest by class (machinery
  / polyglot / carrier / MIR / GDB-JIT / parser singletons). Contract
  + visibility policy: **`docs/adr/0003-c-abi-stance.md`**. Gate:
  `scripts/check-c-abi-surface.sh` in fulltest — self-testing negative
  controls on every run (bogus export bites, missing declaration
  bites), SONAME pinned (`libmadc.so.0`; the `.so.1` flip is a
  deliberate edit there + ADR, on the owner's call). Green on both
  the QNAP copy and the container library. Residue named in the ADR:
  the 33 contract-prefixed internals rename to `__madc_*` in the
  `.so.1` era (forest-ledger symbol-name blast radius — deliberate
  slice, not this gate).
- **PK2 — the default flip. ✅ EXECUTED 2026-09-01.** `./configure`
  now defaults to the shared shape (`enable_shared_cli=yes` in
  configure.ac + the generated configure; monolithic via
  `--disable-shared`) — a plain tree builds the thin CLI, and
  `make release` produces the packaged product (thin `madc-release` +
  forest packed into `lib/libmadc.so`). The lane re-shape landed as
  decided below: fulltest builds BOTH vehicles (`madc-thin` existed;
  `madc-mono` added — the same `madc.o` whole-archived against the
  static libmadc) and `scripts/mono_cli_gate.sh` (negative-controlled:
  the shape assertion must bite on the thin vehicle) is the monolithic
  form's standing coverage. Collateral fixes that ride the flip:
  `package_release.sh` no longer re-strips the installed
  `libmadc.so.0` (post-flip it arrives stripped-then-forest-packed —
  a second strip would silently drop the appended forest), and
  `remote_build.sh pull`'s stale "links statically" comment now states
  that the libs block is what makes a pulled thin compiler runnable.
  Validation (container): plain configure ⇒ `ENABLE_SHARED_CLI=1`;
  thin `bin/madc` 58/58 JIT subset; mono/ABI/library-image gates
  green; release chain + packed subset + exe subset + self-exe gate
  green (phase 2). Full battery rides the merge wave as always.
- **PK3 — staging re-target + install layouts. ✅ LINUX LEG EXECUTED
  2026-09-01.** `package_release.sh` now ships the packaged shape:
  the (post-flip automatically thin) `madc-release` as `/usr/bin/madc`,
  the forest-carrying `libmadc.so.0` (never re-stripped — see PK2),
  **`/usr/bin/madcide`** compiled in-pipeline BY the release compiler
  (`bin/madc-release -o`, stripped), and
  `/usr/share/madcide/profiles/*` (all 8 data files); rpm `%files`
  extended to match. Proven end to end on the container:
  `madc_0.97.0-1_amd64.deb` (10.1 MB — 2.6 MB SMALLER than the
  monolithic-era 0.95.2 deb) + `madc-0.97.0-1.x86_64.rpm` built;
  a dpkg-extracted root runs the installed `madc` (forest evidence:
  `forest-bind: [library-image] opened container (345 units)` from
  the installed `.so`) and the installed `madcide` loads its profiles
  from `usr/share/madcide` on a real pty (NORESCUE, file renders).
  The package-run suite was skipped for this shape proof
  (`MADC_PKG_SKIP_SUITE=1`) — the merge-wave battery validates the
  content. PK4 formalizes the extract-and-run smoke as a gate.

  **✅ PK3 TWINS EXECUTED 2026-09-01 (s151).** The residues closed:
  - **Windows zip:** `bin/madcide.exe` — compiled in-pipeline by the
    release PE under wine (`wine madc-release-…exe -o`, probed first:
    the fresh 0.97.0 hosted PE compiles madcide to a 307 KB PE that
    RUNS under wine, printing its usage line — it binds
    `libmadc_rt.dll`, the full-runtime "win64 twin of libmadc.so"
    already in the zip). Data beside the exe: `bin/profiles/*`
    (resolve_profile_dir's `<exedir>/profiles` arm). The packager
    smoke-runs the STAGED exe (usage line = loads + binds + runs;
    the TUI pty probe is PK4's). NOT stripped — an external strip
    rewriting a MIR-writer PE is the re-strip-the-forest risk class.
    Plus `madc.ini.example` at the zip root (non-live name: `./madc.ini`
    is a real search arm — an example must never shadow a config).
  - **macOS tarball: madcide DEFERRED with a stated reason (probe
    evidence 2026-09-01).** The darwin `-o` lane itself refuses
    runtime-needing programs: *"program needs the madc runtime, which
    does not exist as a library for this native-emit target"* (probed
    on madc-mac with the 0.97.0 tarball binary; JIT of the same
    program is green). madcide needs the C++ script-lane runtime, so
    `-static-libmadc` (C-lane only) cannot carry it either — **no
    venue can produce a runnable mac madcide today**; the mac-side
    compile venue question is moot until darwin grows the
    libmadc_rt.dll twin. BANKED SLICE: **PK3-mac-runtime** — a
    hosted-macos full-runtime dylib (whole-archive `LIBMADC_STATIC`,
    the exact recipe `bin/libmadc_rt.dll` uses), the darwin emit lane
    naming it (`@rpath` + `@executable_path/../lib`), then madcide in
    the mac tarball. Until then the tarball README states the gap and
    the packager ships `share/doc/madc/examples/madc.ini`.
  - **Linux tarball:** `madc-<ver>-linux-x86_64.tar.gz` — the SAME
    `stage()` implementation the deb/rpm use, parameterized by prefix
    (rootless, relocatable). Required the deep fix: `-o` output baked
    an ABSOLUTE build-tree DT_RUNPATH; `cir_native_link_env` now puts
    the relocatable arm FIRST (`$ORIGIN/../lib` on ELF,
    `@executable_path/../lib` on Mach-O, PE unchanged — no runpath),
    then the compiler's own libdir, then `/usr/local/lib`. The
    packager asserts the $ORIGIN proof: `ldd` on the staged
    `bin/madc` + `bin/madcide` must bind the STAGED `lib/libmadc.so.0`
    (an old-policy binary fails the check — built-in negative
    control).
  - **madcide man page** (`docs/man/madcide.1`, gzip-staged into
    deb/rpm/tarball) + **example `madc.ini`**
    (`docs/examples/madc.ini`, all keys commented = inert if copied
    verbatim) in all packages; `madc.1`'s stale `.TH` stamp
    ("0.38.0", 2026-07-23) made version-neutral.
  - **Fix-what-you-find (rode this wave, own commit):** the first
    release-windows pack on s149b-wall content caught the lexer
    splitting C23 `_FloatN` literal suffixes (`0.0f16` in mingw's
    avx512fp16intrin.h, reached from every `<windows.h>` TU) into two
    tokens — the ledger build of rt_posix_time.c refused, killing the
    pack. Fixed at the scanner: ONE `eat_real_suffix` (three
    hand-rolled copies consolidated) knowing f16/f32/f64/f128/bf16,
    typed by the `_Float16`-type precedent (nearest-supported
    approximation); invalid widths pushed back whole so they stay
    loud. Reducers tests/testfloatnsuffix{,bad} with the gcc gnu2x
    oracle; release-windows green again (234 units, verify OK).
  - **Library-family coherence (owner discussion 2026-09-01).** Two
    products, honest names on every platform: the ENGINE
    (`libmadc.so.0` / `libmadc-0.dll` / `libmadc-0.dylib`-someday) and
    the tiny **emitted-C archive** `libmadc_rt.a` (try/catch context
    stack — one shared copy per program, which is why it is a link
    archive — plus VLA scope-exit; for `--emit=c11` output compiled by
    a foreign toolchain on a madc-less box). Executed: (a)
    `libmadc_rt.dll` → **`libmadc-0.dll`** (it was the FULL engine
    misnamed `_rt` since W3.5 — ABI number mingw-style like
    libstdc++-6/libwinpthread-1, twin of `.so.0`, bumps with it;
    implib → `libmadc.dll.a`; one code site,
    `cir_windows_import_dlls`, plus Makefile/verify/packager); (b)
    the linux deb/rpm/tarball now SHIP `libmadc_rt.a` (win/mac
    tarballs already did) — `make release` builds
    `lib/release/libmadc_rt.a` (per-mode-names law extended: the rt
    archive was still on the one shared dev path).
- **PK4 — package-install validation gates.** Install-then-run smoke
  per OS: dpkg/rpm install into a scratch root on the container; the
  Windows/Mac staging flows formalized as scripts (today they are
  manual box rituals).
- **PK5 — GitHub Actions release builds.** One release workflow:
  ubuntu (native Linux build + the mingw cross Windows build — the
  same scripts the container runs), macos-14 (arm64) + macos-13
  (x86_64 — a build we cannot produce today, the owner's Mac is
  arm64). Artifacts = the PK3 packages, attached to the GitHub release
  at stable versioned URLs. Boundary: Actions builds + a smoke subset
  ONLY — the full battery remains the local push gate (container +
  lane ledger, `.claude/rules/testing-fulltest.md`). macOS
  signing/notarization deferred (the S2 re-signer covers ad-hoc
  Mach-O signing).
- **PK6 — ecosystem repos** (each its own slice; all consume PK5
  artifacts): brew tap with bottles; **Microsoft Store submission**
  (MSIX built in the PK5 workflow on a windows runner —
  `makeappx`/`signtool`; submission through the owner's Partner
  Center, manual first, Store submission API later if wanted; winget
  rides the `msstore` source automatically); the apt/yum repo-hosting
  decision.
- **PK7 — madcide-as-binary (a PK3 prerequisite, owner-ruled part of
  the packaging). ✅ EXECUTED 2026-09-01.** madcide AOT-compiles via
  `madc -o` into a 393 KB binary linked against the shared libmadc
  exactly like the thin CLI (closes the madcide-design-doc open
  question — no new link machinery was needed). The one new piece:
  `resolve_profile_dir` in madcide_core.inc — the source tree
  (`__FILE__`-relative, unchanged behavior) then the installed
  layouts anchored on the running binary via `madc::compiler_path()`
  (process-self in the AOT shape): `<exedir>/../share/madcide/
  profiles` (Linux/mac packages) then `<exedir>/profiles` (Windows
  zip). Proven on a pty with the build tree DELETED after compile
  (the true installed situation): share-dir arm loads the joe profile
  (no RESCUE, file renders); negative control (share hidden) triggers
  the RESCUE announcement. Source-tree arm: testmadcide/testvised/
  testlineed 3/3. The rescue fatal message now names all three
  layouts.

## PK0 results (2026-09-01, container, first pass — GATE MET)

`scripts/perf_pack_shapes.sh`, interleaved A/B, n=21 per shape per
case; mono = the shipped v0.97.0 packed `madc-release`; shared = thin
CLI + forest-packed `lib/libmadc.so` built from code-identical
content. Forest evidence: `forest-bind: [library-image] opened
container (345 units)` — discovery arm 2 served, no live-parse
fallback; the pack gate ran in-build and was baseline-clean
(93/93 parse errors, fill-dropped 0, bind cache == no-cache).

| case    | mono median | shared median | delta |
|---------|-------------|---------------|-------|
| hello   | 14.80 ms    | 15.17 ms      | +0.37 ms |
| testint | 5.15 ms     | 5.64 ms       | +0.49 ms |

callgrind Ir (hello): mono 78.97M, shared 81.62M (+3.3%) — consistent
with the wall delta; the cost is ld.so dynamic-link work on a ~16 MB
`.so`, ~0.4–0.5 ms per process start.

Sizes: mono CLI 15,978,184 B; thin CLI 149,808 B + `libmadc.so`
16,124,536 B (forest inside) — shared total +1.8%.

Verdict: the flip costs under half a millisecond of cold start —
immaterial against the tcc-parity budget. PK0 does not block the
flip. Residual cases (madcide start, embedding host) land with
PK7/PK1 respectively; raw TSV at container `tmp/pk0/pk0_results.tsv`.

## Lane shape after the flip (DECIDED at PK2, 2026-09-01)

The packed lane runs the PACKAGED default shape (it is the shipping
product — automatic, since `make release` now builds it); the
monolithic optional form's standing coverage = the `madc-mono`
build in fulltest + `mono_cli_gate.sh` (link + run + shape assertion,
negative-controlled) plus the existing pack/emitpack gates — NOT a
second full battery (battery once per merge wave; lane cost stays
flat). Dev binary lanes run the thin dev CLI (unpacked, live parse —
the forest axis is unchanged).

## Thread-safety contract

No new runtime state — packaging only. The discovery arms and loaders
keep their existing stated contracts (forest load is process-init,
single-threaded by construction).
