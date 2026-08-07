# macOS release lane — plan (2026-08-07)

## Goal

Ship madc for macOS as release artifacts alongside the Linux .deb/.rpm:
the -O2, stripped, forest-packed madc binary for **arm64** and
**x86-64** darwin, distributed from the GitHub release (and later a
Homebrew tap), with the same "packaged artifact is a tested artifact"
discipline the Linux lane has.

Prompted by the v0.69.0 promote (2026-08-07): the libc++ parity
campaign (P2.7, lane zero) closed the library-semantics gap, so the
Macs only have to prove target plumbing — and most of that plumbing
already exists and has run green on the owner's hardware.

## What already exists (verified 2026-08-07)

- **Hosted cross-builds, both arches**: Makefile MODEs
  `hosted-arm64-macos` / `hosted-x86-64-macos` build
  `bin/madc-hosted-{arm64,x86-64}-macos` — madc itself as Mach-O,
  cross-built on the container with clang-18 + the owner's SDK +
  ld64.lld (Route 1, `2026-07-25-madc-on-macos-plan.md`). Current
  slice sizes: 7.3MB (arm64) / 8.2MB (x86-64), un-packed.
- **JIT on Apple Silicon**: upstream MIR carries the MAP_JIT / W^X
  arms under `__APPLE__ && __aarch64__`; the fork adds nothing
  host-side.
- **Darwin forest carrier**: `scripts/forest_pack_darwin.sh` embeds
  the forest in a `__MADC,__forest` section (a signed Mach-O cannot
  carry the ELF-style EOF trailer), and the full
  freeze → pack-emit → AMFI → read-back loop is green on owner
  hardware, **both arches**.
- **Signing (run-locally grade)**: lld ad-hoc-signs arm64 output
  automatically; the pack step re-signs after modifying the binary
  (that is what the AMFI leg proves).
- **Self-sufficiency**: hosted binaries embed the generated SDK
  prelude + embedded headers — the owner's Macs are run-only (no
  Xcode CLT, no /usr/include) and that is the supported posture.
- **Universal-binary tooling**: llvm-lipo ships with the provisioned
  llvm-18, so fusing universal2 on Linux is available if wanted.

## W0 — RESOLVED (owner, 2026-08-07): provenance-clean prelude

**Finding that forced the decision:** the hosted prelude is not
compiled data — `gen_darwin_prelude.sh` embeds `clang -E -dD` output,
i.e. flattened Apple header TEXT with every `#define` kept, and madc
SERVES it as headers to user programs (that is its purpose). The
script's own law ("SDK-derived: never committed, synced, or
redistributed") therefore applies to any public artifact embedding it.

**Decision: regenerate the prelude from openly licensed sources**
instead of the SDK tree, so public artifacts are defensible by
construction (the Zig approach — Zig has publicly shipped a curated
macOS libc header tree for years):

- **C++ surface**: libc++ headers are LLVM, Apache-2.0-with-LLVM-
  exception — same upstream the SDK copies; freely redistributable
  with attribution.
- **C/POSIX surface**: there is no "clang libc" — assemble the tree
  from Apple's own open-source releases (`apple-oss-distributions`:
  Libc, xnu bsd headers, availability machinery; APSL/BSD). Zig's
  curated macOS libc tree is the base candidate rather than curating
  from scratch (original licenses carried).
- **License notices** (APSL/BSD/Apache) ship in the artifact
  (`usr/share/doc/madc/`).
- **The SDK remains build-side only** — cross-LINKING still uses the
  owner's SDK (`.tbd` stubs, crt); that is "developing with the SDK,"
  its licensed purpose. Zero SDK-derived text in shipped artifacts;
  the unchanged law now has a mechanical meaning the packaging step
  can gate on (the hosted release target accepts only the
  open-provenance prelude dir).

## Decisions to make (with defaults proposed)

1. **Thin binaries vs universal2 — DEFAULT: two thin binaries.**
   A fat binary is the two slices concatenated (~sum of sizes; each
   slice carries its OWN forest — the frozen forests differ per arch
   because type layouts differ, e.g. x86-64 darwin `long double` is
   80-bit x87, arm64's is double). Estimated ~33-34MB universal vs
   ~16-17MB per thin artifact (7-8MB slice + ~9MB zstd forest).
   Thin fits Homebrew's per-arch bottles naturally and halves the
   download; universal2 is only a direct-download convenience.
   ALSO: the forest self-probe (`cir_macho_find_forest`) walks a thin
   Mach-O header — teaching it to slice a FAT header is a small,
   known work item required before any universal2 ship.
2. **Signing posture — DEFAULT: ad-hoc now, notarization later.**
   Ad-hoc signatures run fine locally (and are mandatory on arm64);
   downloaded binaries carry the quarantine xattr and hit Gatekeeper's
   "unverified developer" friction (documented workaround:
   `xattr -d com.apple.quarantine` or right-click-open). Real
   frictionless distribution = Apple Developer ID + notarization
   ($99/yr + notarytool flow) — a later, owner-funded upgrade. A
   Homebrew tap mostly sidesteps quarantine and is the idiomatic
   channel for a dev CLI regardless.
3. **Packaging format — DEFAULT: tar.gz per arch on the GitHub
   release** (binary + man page + LICENSE + CHANGELOG), named
   `madc-<ver>-macos-{arm64,x86_64}.tar.gz`, listed in SHA256SUMS
   beside the .deb/.rpm. A `.pkg` installer and a Homebrew tap are
   follow-ons, not v1.

## Work items (order)

- **W0 (owner)**: ~~settle the SDK-prelude redistribution question~~
  RESOLVED 2026-08-07 — provenance-clean prelude (section above).
- **W0.5 — provenance-clean prelude (NEW, gates public artifacts)**:
  assemble the open-licensed header tree (evaluate adopting Zig's
  curated macOS libc tree + LLVM's libc++ headers, matched to the
  target OS version), point `gen_darwin_prelude.sh` at it, and DIFF
  the resulting prelude against the current SDK-derived output to
  find declaration gaps (the SDK-derived prelude stays as the
  private-use oracle, never shipped). Ship the license notices.
- **W1 — release-grade hosted builds**: add packed -O2 hosted release
  targets (the darwin twin of `make release`): build hosted slice,
  strip, `forest_pack_darwin.sh`, verify read-back. Wire into
  `remote_build.sh` as a stage (e.g. `release-macos`).
- **W2 — Mac-side battery**: the container cannot execute darwin
  binaries, so define the test evidence that runs on the owner's Macs
  against the exact packed artifacts. Minimum: the packed suite's
  darwin-runnable subset + the forest gates + an AOT emit/run leg,
  driven by a self-contained script (`scripts/mac_battery.sh`) the
  owner can run and paste/return the log from. The release checklist
  gains "mac battery log in evidence" beside the Linux packed run.
- **W3 — libmadc dylib question**: Linux packages ship `libmadc.so.0`
  because `-o` executables reference it at runtime. Verify what
  darwin AOT output references; if needed, build
  `libmadc.dylib` (`@rpath` install_name) in the hosted MODEs and
  include it in the tarball.
- **W4 — packaging step**: extend `scripts/package_release.sh` (or a
  sibling `package_release_macos.sh`) to stage the tarballs +
  extend SHA256SUMS; `gh release upload` alongside the .deb/.rpm.
- **W5 (optional, later)**: universal2 (FAT-aware forest probe +
  llvm-lipo fuse), Homebrew tap, Developer ID + notarization.

## Non-goals (v1)

- No .pkg installer, no notarization, no universal2 in the first ship.
- No SDK content in the repo or the sync path — unchanged law.
- The QNAP/container division is unchanged: container builds
  everything; Macs only run the battery.
