# Forest carriers & installation shapes — track plan (2026-07-25, owner-directed)

## Goal

The **frozen forest is THE precompiled-header model on every platform**
(owner directive 2026-07-25; the `.madh` bake is dead and stays dead —
embedded live-parse text is a fallback lane and freeze-time input,
never the product path). This track makes the forest — and madc's
installation shape — **configurable rather than set in stone**:

- a **single fully-embedded binary** (no installation, zero external
  requirements) stays a first-class shape on every platform;
- a **"full and proper" packaged installation** (shared `libmadc` +
  thin CLI, optional config file, sidecar forest) becomes an equally
  supported shape;
- madc gains **`-static-libmadc` compiling** so madc-emitted binaries
  that use madc-specific functionality need no `madc.so` /
  `madc.dylib` / `madc.dll` at runtime.

## The law: one format, one loader, N carriers

There is exactly ONE forest blob format and ONE bind engine
(`LOADED == parsed`). What this track adds is **discovery**: a small,
deterministic probe chain over *carriers*. The blob bytes are
identical in every carrier — only transport differs. Nothing in this
plan introduces a second serialization or a second loader
(no-parallel-implementations law).

Probe chain (first hit wins, order fixed):

1. **Self-image** — the running executable carries the forest
   (per-format arm: ELF trailer / Mach-O section / PE overlay-or-section).
2. **Library image** — when madc is built shared, the forest rides
   `libmadc`'s image, found via `dladdr` on a known libmadc symbol →
   image path → the same per-format probe. Embedding hosts get the
   groves for free.
3. **Sidecar file** — `<image>.forest` beside the binary, then a
   configured path (`MADC_FOREST` env, `madc.ini` key, baked default).

The existing blob header validation (format version, compiler hash)
guards every arm identically — a stale sidecar is rejected the same
way a stale trailer is today.

## Carrier matrix per platform

| Platform | Self-contained carrier | Notes |
|----------|------------------------|-------|
| Linux (ELF) | **Trailer** (shipped today, unchanged) | ELF tolerates trailing bytes; nothing to churn. |
| macOS (Mach-O) | **`__MADC,__forest` section** | Appended blobs are ILLEGAL on signed Mach-O: the file must end exactly at the code signature — AMFI kills anything with trailing bytes on arm64. |
| Windows (PE) | **Overlay** (trailer twin) for unsigned binaries; a proper `.madc` section is the signed-binary answer later | PE overlays are tolerated and idiomatic; Authenticode signing excludes nothing after the cert table, so signed PE needs the section + a re-sign story (deferred with the Windows track). |

### macOS: two distinct cases, only one needs a re-signer

- **madc itself (hosted binaries):** the forest is placed at LINK time
  via `-Wl,-sectcreate,__MADC,__forest,forest.bin` — lld computes its
  ad-hoc signature AFTER section layout, so the shipped binary needs
  **no re-signer at all** on the build path. Freeze in the container,
  embed at link, signature covers it.
- **Post-link packing** — the `--freeze-run` self-rewrite flow and
  madc-EMITTED packed programs on the Mac: these rewrite an
  already-signed file, so they use the **in-house ad-hoc re-signer**,
  a reuse of `mir-macho.c`'s CodeDirectory/SHA-256 code (the writer
  already signs every emit; no new crypto, no Apple codesign).

Read-back on darwin switches from the trailer probe to the self-image
section API (`getsectiondata` on the running image); ELF keeps the
trailer probe. Same loader behind both — the probe chain's per-format
arm is the only platform code.

### Who freezes the darwin groves

**Build-time cross-freeze in the container (recommended, product
path):** the freezer must hold Apple target facts (arm64-darwin
`long double == double`, darwin type layouts) — exactly what the
Linux-hosted cross madcs carry since v0.43/0.44. The darwin embedded
prelude text is the freeze-time INPUT (its legitimate long-term role;
its runtime tokenization disappears once the groves bind).
First-run self-freeze on the Mac stays a possible dev convenience,
NOT the product path (run-only Macs, and the rewrite would need the
re-signer anyway).

## Configure axes (orthogonal; shapes are combinations, not builds)

1. **Link shape** — `--enable-shared`:
   - *Monolithic* (default, today's product): static libmadc into the
     CLI; single binary.
   - *Shared*: `libmadc.so.1.0` / `madc.dylib` / `madc.dll` + a thin
     CLI. The forest rides the LIBRARY image (probe arm 2); one forest
     serves the CLI and every embedding host.
2. **Forest carrier** — `--with-forest=embedded|sidecar|none`:
   - *embedded* (default): section/trailer per the matrix — the
     no-installation shape.
   - *sidecar*: forest installed as a separate file (distro packaging;
     one forest shared across CLI + library; dev iteration without
     re-packing).
   - *none*: live-parse fallback only (dev shape; what unpacked
     `bin/madc` is today).
3. **Config file** — `--enable-config-file`: `madc.ini` lookup
   (see below). OFF by default in embedding-host builds.

The runtime probe chain accepts any carrier it finds regardless of the
configured default — configure sets what the BUILD produces and what
the binary EXPECTS, not an artificial restriction on discovery.

### Failure policy (the G2 lesson: no silent degradation)

- Configured *embedded* but the blob is missing/stale at runtime:
  standalone CLI **falls back to live parse with a loud stderr
  notice** (liberal default); embedding hosts get a **strict mode**
  where a missing forest is a hard error (registration-policy knob,
  same family as the existing sandbox policies).
- Configured *sidecar* and the file is absent: same policy pair.
- `-static-libmadc` requested but a needed runtime piece is not
  ledger-carried (below): **refuse at emit with a clear message** —
  exactly the existing cover-analysis behavior, extended.

## madc.ini (configure-gated)

- Precedence: **CLI flags > environment > madc.ini > baked defaults.**
- Lookup order (when enabled): `./madc.ini` (or beside the exe on
  Windows) → user config dir → system config dir; first hit wins.
- Initial keys: forest sidecar path, include-path additions, default
  `--std=`, resource-limit overrides (liberal-defaults rule applies).
- **Sandboxing rule:** OFF by default for embedding-host builds — a
  config file that can redirect where the compiler loads frozen state
  from is an attack surface for sandboxed madc (`fork`+`rlimit`+
  `seccomp` hosts). The standalone CLI enables it freely.

## `-static-libmadc` — madc-emitted binaries with zero madc-library dependency

**Semantics** (gcc precedent: `-static-libgcc` / `-static-libstdc++`):
the emitted binary carries the madc runtime pieces it needs INSIDE the
image; no `DT_NEEDED libmadc.so.0` / dylib load command / DLL import.
libc/libSystem stay dynamic (full `-static` à la musl is a separate,
Linux-only question — and impossible on macOS, which has no static
libSystem; the flag spelling deliberately scopes what it promises).
`-static` is accepted as an alias where full-static cannot exist
(darwin) and reserved for a future true-full-static on Linux.

**Mechanism — the AOT ledger:** the madc runtime pieces are
precompiled to MIR object modules at madc's own build time and carried
in the same blob/carrier as the forest (the ledger is already part of
the save-state model). At `-static-libmadc` emit, the cover analysis —
which already computes exactly which runtime symbols a program needs —
selects the required ledger modules and the existing multi-object
merge machinery (`MIR_object_read` × N → emit, proven since the R4b
`.o`-merge lane) links them into the output. MIR remains the compiler,
assembler, linker, and signer; no external toolchain enters the
product path.

**Tiering (honest scope):**

- **Tier A (this track):** the C-lane machinery — `__madc_*`
  compiler-machinery symbols (scope-set/VLA category), the
  setjmp/longjmp exception runtime, va helpers: C-compilable sources
  that c2mir itself can build into ledger modules. Covers every
  program the Apple cross lane calls "runtime-needing" today for
  machinery reasons.
- **Tier B (deferred, tracked):** the script-lane C++ runtime
  (MadValue/MadArray, `ns_*` namespace publics, anything mangled-direct
  against libstdc++/libc++). These exist only as host-toolchain C++
  objects; carrying them on the ledger requires either madc compiling
  its own runtime (the self-hosting arc — the strategic convergence)
  or MIR growing a foreign-ELF/Mach-O relocatable reader (large fork
  surface). Until then `-static-libmadc` REFUSES loudly for programs
  needing Tier-B pieces, listing the symbols — the same loud-refusal
  contract the Apple cross lane ships today.

## Sequencing

1. **S1 — Mach-O forest section (madc itself):** container cross-freeze
   of darwin groves (prelude as freeze input) → `-sectcreate` at
   hosted link → darwin section read-back arm → hosted binaries ship
   PACKED. Gate: hosted G2 re-run on hardware; grove bind on the Mac
   == live parse output; Linux packed arbiter untouched.
   *Implementation notes (landed on `feature/forest-carriers-claude`):*
   the freezer is the same-arch CROSS madc, which now embeds the same
   per-target darwin prelude as the hosted binary
   (`DARWIN_PRELUDE_TARGET` covers cross + hosted MODEs); the freeze
   reuses `--freeze-append` onto an EMPTY file (placement 2 = the
   release-pack compression profile; container starts at offset 0, so
   the file is a valid standalone container for `-sectcreate`);
   `scripts/forest_pack_darwin.sh` generates the TU from the prelude's
   `.MANIFEST` (one owner of the header list) and gates on every name
   being a directory unit; hosted MODEs regained zstd via per-target
   static libs (`/workspace/zstd/libzstd-{arm64,x86-64}-macos.a`) so
   the consumer reads the release codec; darwin-inline bodies whose
   callees are libSystem-private (`__maskrune`, `__swbuf`,
   `__sincos_stret`…) are unresolvable on the Linux freezer host and
   revert to DEFBODY body-span carry — consumers derive them on first
   use (the v26 path, the same lowering G2 proved live).
2. **S2 — In-house re-signer + darwin `--freeze-run`/emitted-pack:**
   CodeDirectory reuse from `mir-macho.c`; post-link pack + re-sign;
   AMFI acceptance on hardware is the gate.
3. **S3 — Carrier probe chain + `--with-forest=` + sidecar:** unify
   discovery behind the ordered arms; sidecar file support; failure
   policy knobs. Gate: all shapes × Linux/macOS matrix green.
4. **S4 — Shared shape:** forest-in-library arm; `madc.dylib` /
   versioned `.so` packaging alignment (libmadc track already has the
   `.so`); thin CLI. Gate: embedding-host smoke + CLI parity.
5. **S5 — `-static-libmadc` Tier A:** ledger modules built by c2mir at
   madc build time, carried per-target; cover-analysis selection +
   merge at emit; loud Tier-B refusal. Gate: a try/catch-using C-lane
   program emits `-static-libmadc` and runs with zero madc library on
   a clean machine (Linux + Mac).
6. **S6 — `madc.ini`** (configure-gated, smallest slice, anytime after
   S3).

Windows arms (overlay carrier, `.madc` section, `madc.dll`) ride the
Windows/PE track when it starts — the probe-chain design above is
already shaped for them.

## Decided defaults (owner directions folded in)

- Both flagship shapes are first-class: fully-embedded single binary
  AND packaged shared install; configure chooses, neither is
  second-class.
- Defaults stay what ships today: monolithic + embedded forest.
- Loud-fallback for the CLI, strict mode for embedding hosts.
- `-static-libmadc` spelling per gcc precedent; `-static` aliases on
  darwin, reserved on Linux.
- One blob format/loader forever; carriers are the only variation
  axis.
