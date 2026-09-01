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
- **PK1 — C-ABI surface audit + policy.** Enumerate the exported
  `extern "C"` surface of `libmadc.so`; classify every symbol (C-host
  API vs CIR-emitted machinery vs accidental export); set the export/
  visibility policy. Output: the documented ABI contract + an audit
  script as a fulltest gate (a NEW unclassified extern-C export fails
  loud; negative control included). `.so.1` flips when this gate is
  green and the owner calls it.
- **PK2 — the default flip.** configure defaults become
  `--enable-shared` + forest-in-library; monolithic stays behind the
  flag, still gated. The lane re-shape is decided here (see "Lane
  shape" below).
- **PK3 — staging re-target + install layouts.** `package_release.sh`
  stages the thin CLI + the forest-carrying `.so`. Layouts: Linux
  `/usr/bin/madc` + `/usr/bin/madcide` +
  `/usr/lib/<triplet>/libmadc.so.0` (+ `.so` symlink; the ldconfig
  trigger already exists), man + docs + example `madc.ini`; macOS
  tarball `bin/madc` + `bin/madcide` + `lib/madc.dylib`
  (`@executable_path`-relative rpath, S2 re-signer); Windows zip
  `madc.exe` + `madcide.exe` + `madc.dll` beside each other
  (loader-natural). The madcide binary is built BY the staged madc
  (PK7's link seam is therefore a PK3 prerequisite, see below); its
  data files (profiles/themes/status formats) ship beside it per OS
  convention (`/usr/share/madcide/` on Linux; beside the exe on
  Windows; `share/madcide/` in the mac tarball) — madcide's
  profile-dir resolution must learn the installed location alongside
  the source-tree one.
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
  the packaging).** madcide compiles to a native binary via madc's
  own AOT `--exe` lane, linked against the shared libmadc exactly the
  way the thin CLI is (closes the madcide-design-doc open question).
  Ships in every package (PK3); the packaged madcide's data files
  (profiles/themes/status) resolve from the installed share dir as
  well as the source tree. Sequenced before PK3's final staging even
  though numbered last — the number keeps the design-doc
  cross-references stable.

## Lane shape after the flip (PK2 decision, owner-visible knob)

Proposal: the packed lane runs the PACKAGED default shape (it is the
shipping product); the monolithic optional form keeps its existing
pack/emitpack gates plus a build-and-run smoke — NOT a second full
battery (battery once per merge wave; lane cost stays flat). Dev
binary lanes are unchanged (the dev shape stays monolithic unpacked,
forest = live parse).

## Thread-safety contract

No new runtime state — packaging only. The discovery arms and loaders
keep their existing stated contracts (forest load is process-init,
single-threaded by construction).
