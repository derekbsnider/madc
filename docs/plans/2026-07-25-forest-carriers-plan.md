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
   (see below). Default **yes** — the sandboxing rule is met by WHO
   reads the file, not by the default: `libmadc` never reads one, so an
   embedding host is unaffected by the axis and the standalone CLI gets
   its config file. `--disable-config-file` removes the file-reading
   path from the binary for builds that want the surface ABSENT rather
   than merely unused (S6).

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
- **Sandboxing rule:** a config file that can redirect where the
  compiler loads frozen state from is an attack surface for sandboxed
  madc (`fork`+`rlimit`+`seccomp` hosts). **As shipped (S6):** the
  reader is a CLI-shape feature — `libmadc` never consults a config
  file, so a host is safe by construction rather than by a build
  default; the forest key rides `enable_external_forest` with the
  sidecar/env arms, so a sandboxed host that turned those off also
  turns arm 5 off; and `--disable-config-file` removes the code path
  entirely for deployments that want the surface gone.
- **As shipped (S6):** relative paths in the file resolve against the
  FILE's directory, a leading `~/` expands, and the parser is STRICT —
  an unknown key or malformed line is a hard error naming file:line,
  never a warning that half-applies. `--config=<file>` names one file
  (and must load); `--no-config` skips the search.

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
   **S1 COMPLETE (2026-07-25): gate GREEN on Apple hardware, both
   arches** — `--dump-forest` reads 30 units from the running binary's
   own `__MADC,__forest` section (AMFI accepted the sectcreate'd signed
   binary); all lanes green (hello 7 / compute 28 / g2x 55 JIT+AOT /
   aot 33) on A64 native and X64-under-Rosetta; grove bind provably
   engaged (`-v`: "#include <stdio.h> bound to grove unit 9") and bind
   output byte-identical to `--no-forest-bind` live parse; Linux
   fulltest 756/0/0/9 + all forest gates green. The gate caught a real
   target-independent parser bug: `typedef struct __sFILE {... fnptr
   members ...} FILE;` (Apple's FILE shape) routes through
   TokenCLASS::parse, whose typedef branches registered only the type
   maps — no `user_typedef_names`, no dkTypedef TopDecl — so the
   freeze never emitted the alias's DK_TYPEDEF record and bound
   consumers lost `FILE` while live parse resolved it. Fixed at the
   parser layer (both branches now record the full typedef surface,
   the using-alias precedent); pinned by the new Linux
   forest_bind_gate case `[fnptrbody]`. Known follow-on: fnptr
   TYPEDEFS themselves (darwin `sig_t`) still cleanly lack — they need
   a DK_FPTR arena kind (the DK_CARRAY precedent).
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
   **S2 COMPLETE (2026-07-25): gate GREEN on Apple hardware, both
   arches** — with the scope the hardware probe narrowed: darwin
   `--freeze-run` needed nothing (temp-file container + re-exec + file
   probe, no self-rewrite), and emitted-pack needed **no re-signer at
   all** — the blob rides the fork writer at emit time. `--pack-forest=
   <container>` with `-o`/`-shared` embeds a frozen container in the
   emitted image's self-image carrier: ELF appends the trailer
   post-write (`snapshot_append_blob`, extracted so ONE owner holds the
   placement-2 shape); Mach-O passes the blob through a new
   `MIR_object_exec_params` tail seam (`extra_segname/sectname/data/
   size`) and `mir-macho.c` lays a read-only one-section `__MADC`
   segment between `__DATA` and `__LINKEDIT`, covered by the emit-time
   ad-hoc signature — signed once, no post-link surgery (the
   `-sectcreate` insight, now first-class in the emitter). Read-back:
   `cir_forest_map_image`'s file probe gained a Mach-O arm (pure byte
   parse, host-neutral — Linux cross madcs verify emitted images).
   Hardware evidence (`~/s2pack`): cross-emitted packed binaries
   carrying the REAL 30-unit darwin groves run on the Mac (rc 42, AMFI
   accepted) on A64 native + X64-under-Rosetta; hosted `--dump-forest`
   over the packed files is byte-identical to the containers; and the
   FULL NATIVE LOOP — hosted madc freezes, emits packed, AMFI accepts,
   reads back — is green on both arches. Linux: permanent
   `forest_emitpack_gate.sh` in fulltest (ELF run + dump parity + both
   refusal arms; per-arch Mach-O dump-parity legs, SKIP when cross
   madcs absent). Also fixed en route: per-target MIR variant-lib rules
   now FORCE-recurse (a fork source change under an existing build dir
   went silently stale — this slice's writer change caught it), and the
   Mach-O probe constants are macro-proof against `<mach-o/loader.h>`.
   **Deferred residue (consciously, LOW priority):** the in-house
   re-signer for rewriting EXISTING signed binaries (on-Mac self-pack
   of an already-shipped image = dev convenience; the product path is
   emit-time/link-time packing, which needs no re-signer anywhere).
3. **S3 — Carrier probe chain + `--with-forest=` + sidecar:** unify
   discovery behind the ordered arms; sidecar file support; failure
   policy knobs. Gate: all shapes × Linux/macOS matrix green.
   **S3 COMPLETE (2026-07-25): gate GREEN — full shape × platform
   matrix.** `ensure_bind_forest` now walks the ordered chain (first
   usable container wins): 1. self-image, 2. (S4 slot) library image,
   3. `<exe>.forest` sidecar, 4. `$MADC_FOREST`, 5. (S6 slot) the
   `madc.ini` forest key — every arm validated identically
   (footer + context hash + version pin + v27 config gate); explicit
   `--forest-bind=` bypasses the chain and is now a LOUD fall-through
   when it fails to open. Failure policy rides `RegistrationPolicy`:
   `forest_missing_policy` (silent dev default / loud one-notice
   packaged-CLI default baked via `MADC_FOREST_EXPECT_*` in product
   MODEs / strict hard error) + `enable_external_forest` (sandbox knob
   gating the sidecar/env arms). `--with-forest=embedded|sidecar|none`
   (configure, default embedded) picks the product carrier: sidecar
   ships `<bin>.forest` (`forest_pack.sh --sidecar`; hosted darwin
   keeps the cross-freeze, drops `-sectcreate`; `make install` places
   `bin/madc.forest`). Evidence — Linux: fulltest 756/0/0/9 + new
   permanent `forest_sidecar_gate.sh` (both external arms bind with
   `-v` engagement + byte parity vs live, order pinned, loud junk /
   explicit-miss surfaces) + full arbiter through BOTH carriers
   (embedded 756/0/0/9, sidecar 756/0/0/9 + loud-missing +
   quiet-mismatch smokes) + exe 740/0 + policy unit tests (6 cases).
   macOS (hardware, A64 native + X64-Rosetta, ~/s3side): 7/7 legs per
   arch — embedded self-dump/bind/run regression, sidecar bind +
   parity, loud-on-missing, quiet-on-mismatch; AMFI accepted all four
   binaries (sidecar-shaped hosted pair = the embedded pair minus the
   `__MADC` section, forest riding beside). Two bugs the gates caught
   en route: (a) the packed-CLI loud notice fired on every
   config-mismatched compile (`--std=` expect_quiet tests) — the
   chain-end policy now knows WHY it ended empty
   (`forest_missing_fallback(config_mismatch)`): multi-dialect
   fall-through is never a notice under loud, still a named hard error
   under strict; (b) the S2 emitpack gate's Mach-O legs asserted
   cross-BINARY dump equality — a rev-skew assertion the context-hash
   pin rightly rejects; each leg now freezes with the same cross madc
   that emits and dumps (carrier transparency per binary). Also
   caught: `pipefail` + `grep -q` on multi-MB `-v` captures dies of
   EPIPE at first-match exit — gate greps read via here-strings.
   NOT in this slice (stated boundary): the
   `enable_external_forest=false` negative path is code-reviewed but
   not integration-tested (the CLI has no knob to flip it; it gets a
   host-driven test with the S4 embedding-host smoke).
4. **S4 — Shared shape:** forest-in-library arm; `madc.dylib` /
   versioned `.so` packaging alignment (libmadc track already has the
   `.so`); thin CLI. Gate: embedding-host smoke + CLI parity.
   **S4 COMPLETE (2026-07-26): gate GREEN.** The chain gained its
   library arms — arm 2 is the **libmadc image** itself
   (`madc_self_lib_path()`: `dladdr` on a libmadc-resident symbol →
   the same per-format probe; skipped when that path IS the executable,
   i.e. the monolithic shape arm 1 already covered), and the sidecar
   arm gained **`<lib>.forest`** after `<exe>.forest`. The IMAGE arms
   are deliberately NOT gated by `enable_external_forest` — the library
   is the installation the host already loaded, not an external
   redirection — so a sandboxed strict host still binds its groves;
   that asymmetry is the slice's central semantic. New
   **`--enable-shared`** configure axis (axis 1 of this plan) links the
   CLI against the shared libmadc; in that shape `make release` packs
   `lib/libmadc.so` (`forest_pack.sh --image`, strip-before-pack) and
   `make install` ships the packed library without re-stripping it.
   The public embedding API grew the knob family
   (`madc::compile_options::enable_forest_bind` / `forest_missing` /
   `enable_external_forest`, `security_policy::allow_external_forest`,
   clamped off under `system_locked`), and
   `Program::forest_bind_enabled` moved into `RegistrationPolicy` so
   ONE owner flows engine → program → child → host. Evidence:
   permanent `scripts/forest_library_gate.sh` in fulltest — 9 legs over
   a staged `bin/` + `lib/` install (thin-CLI live parity;
   library-image bind with `-v` arm naming + output parity; arm order:
   library image beats a present `<exe>.forest` AND a junk
   `MADC_FOREST`; `<lib>.forest` bind; and the host legs with no CLI
   knob — strict+sandboxed binding through the library image, the
   `enable_external_forest=false` refusal S3 owed (same env, knob
   flipped, opposite outcome, chain-empty message never a config
   mismatch), strict-on-empty, silent library default) — plus
   `tests/libmadc_forest_smoke.cpp` (public-API host) and a unit case
   pinning monolithic image identity. Suites: fulltest 756/0/0/9,
   `--exe` 740/0, **thin-CLI parity 756/0/0/9**, and the PRODUCT
   `--enable-shared` shape (release packs the library) arbiter
   756/0/0/9 + an installed-tree run binding through the library image.
   Two real bugs fixed en route: a bare `make -C src` had been building
   nothing but the forest-shape stamp since v0.48.0 (the stamp rule
   sits above `all:` and GNU make takes the first rule as the default
   goal — now `.DEFAULT_GOAL := all`), and a runtime-eval CHILD program
   reverted both forest knobs to the liberal defaults instead of
   inheriting them.
   **NOT in this slice (stated boundary):** the darwin **dylib**
   packaging shape (`-dynamiclib` + `@rpath` install_name for the
   hosted MODEs, its `-sectcreate` carrier, and the Mac hardware legs).
   The hosted darwin build produces no shared library today; the
   discovery code is platform-neutral (the Mach-O file probe already
   reads dylibs), so this is packaging + hardware validation, and it
   rides the darwin packaging work with the Windows/PE arms.
5. **S5 — `-static-libmadc` Tier A:** ledger modules built by c2mir at
   madc build time, carried per-target; cover-analysis selection +
   merge at emit; loud Tier-B refusal. Gate: a try/catch-using C-lane
   program emits `-static-libmadc` and runs with zero madc library on
   a clean machine (Linux + Mac).
   **S5 COMPLETE (2026-07-26): Linux gate GREEN.** The C-lane runtime
   became **dual-build C11 sources** under `src/rt/`
   (`rt_except.c` = the whole SJLJ try/throw/catch + cleanup runtime,
   `rt_vla.c` = VLA scope exit): the host build compiles them into
   libmadc (`$(CXX) -x c -std=c11`, so every MODE's target/sysroot
   flags apply unchanged) and **madc compiles the very same sources
   through c2mir at pack time** into MIR ledger modules. ONE
   implementation, two consumers; `scripts/ledger_sources.txt` is the
   single owner of the list, read by `src/Makefile`,
   `scripts/forest_pack.sh` and `scripts/forest_pack_darwin.sh`.
   `src/exception_runtime.cpp` is deleted, its `__atomic_*` wrappers
   moved to `va_helpers.cpp` beside the other builtin shims — where
   they belong and where they must stay: madc lowers `__builtin_x` to
   `__madc_x`, so a builtin shim compiled BY madc would call straight
   back into itself. That is the real Tier-A boundary, sharper than
   "C-compilable": **strict C11, no compiler builtins.**
   *Carrier:* a new OPTIONAL container segment (`CIR_FOREST_SEG_LEDGER`,
   header + directory + per-module symbol index and `.bmir` bytes), so
   the ledger rides every carrier the forest already rides — self-image,
   library image, sidecars, `$MADC_FOREST` — for free. It is read
   INDEPENDENTLY of the grove bind: the discovery chain was factored
   into one walker (`Program::probe_forest_chain`) that both consumers
   share, with the v27 producer-config gate applied to the grove bind
   only. The ledger is target-specific but dialect-agnostic, so a
   `--std=c99` compile that cannot bind the groves still links the
   runtime. `--dump-forest` reports `ledger`/`ledgermod` lines.
   *Emit:* `-static-libmadc` (+ `-static` alias) pulls ledger modules
   into the compile context BEFORE `MIR_link` — transitively, to a
   fixpoint, whole modules at a time (gcc's .a-member granularity) —
   so the eager object-mode gen puts their code in the capture; at emit
   the existing cover analysis verifies nothing madc-side is left and
   the `libmadc.so.0` DT_NEEDED is dropped unconditionally. Two
   diagnostics, deliberately distinct: **no ledger reached the pull** is
   a BUILD-side message (this madc ships none — use a packed/installed
   build or `--forest-bind=`), while leftover symbols are the **Tier-B
   refusal**, listing them. Evidence: permanent
   `scripts/forest_ledger_gate.sh` in fulltest — 13 checks (ledger
   packs + `--dump-forest` reports it; baseline WITHOUT the flag keeps
   `libmadc.so.0`, so the next leg proves the flag and not an accident;
   try/catch emits with no madc library, no `__madc_*` imports, output
   identical to the JIT run, and runs under an empty library path; VLA
   likewise; Tier-B refusal names symbols; no-ledger carrier gets the
   build-side message and never blames Tier B; `-c` and the .o link
   lane refuse at their own layers).
   Three real bugs fixed en route, each at its own layer: (a) the
   ledger compile activated its own Programs' token/value/type pools
   and then destroyed them, leaving the caller's freeze interning into
   freed memory (SIGSEGV) — the side excursion now restores the
   previously-active Program; (b) every ledger function was getting a
   `__madc_shim_<sym>` MadValue adapter, importing `madc_value_*` —
   precisely the libmadc dependency the flag exists to remove — fixed
   with the existing `aot_skip_eval_shims` knob (a ledger module is
   never host-called through the value ABI); (c) **the cover analysis
   mis-classified COPY-RELOCATED libc data**: `dlsym`+`dladdr` answers
   "where does the process's winning definition live", and `stderr` is
   copied into `bin/madc`'s own `.bss`, so it looked uncovered and
   EVERY AOT program touching stderr kept a needless `libmadc.so.0`
   DT_NEEDED. The cover libraries are now asked directly
   (`dlopen(soname, RTLD_NOLOAD)` + `dlsym`) before falling back to
   dladdr — a fix that improves the default lane too.
   **DARWIN, up to the hardware line.** `forest_pack_darwin.sh` packs
   the ledger with the cross madc (so the modules carry darwin-target
   MIR) and FAILS the pack if the container ends up without one — there
   is no dylib to fall back to on Mach-O, so a hosted madc with no
   ledger could never emit a try/catch program at all. Verified on both
   arches from Linux: the cross-freeze carries the ledger (2 modules /
   22 symbols), the SAME program that still refuses without the flag
   ("program needs the madc runtime, which does not exist as a library
   for Mach-O targets; build it into the image with -static-libmadc")
   now emits a valid `Mach-O 64-bit arm64 / x86_64 executable
   (NOUNDEFS|DYLDLINK|TWOLEVEL|PIE)` with the ledger pulled, ZERO
   strings naming libmadc and ZERO undefined `__madc_*` symbols.
   That exposed the LAST bug of the slice, the cross form of the
   copy-relocation one: **cover analysis on a cross build was probing
   the HOST's symbol universe.** darwin's `stderr` is `__stderrp`,
   absent from glibc, so `dlsym` said "uncovered" and the emit refused.
   A cross build cannot answer "does the target's libc provide this" by
   probing — but it CAN answer the question the analysis actually asks,
   "does LIBMADC define this", because libmadc is loaded in this very
   process. Under `MADC_CROSS_TARGET` the check is now exactly that;
   anything libmadc does not define belongs to the target system, and a
   genuinely missing symbol surfaces at the target's loader instead of
   as a bogus madc-runtime refusal — the ordinary cross-compiler
   contract. (Native builds are untouched: the branch is
   preprocessor-excluded.)
   **STATED BOUNDARIES:** (i) **running** an emitted Mach-O binary
   still needs the owner's Mac, like every S1–S3 darwin leg — the
   emit side is proven, the AMFI/run leg is not. (ii) the **.o link
   lane** (`madc -static-libmadc -o prog a.o`)
   refuses loudly: the ledger is carried as MIR modules, which merge
   into a compile CONTEXT, while that lane merges native relocatables
   through `MIR_object_read` — bridging them needs the MH_OBJECT `.o`
   flavor the fork still lacks (task #4). (iii) a `-static-libmadc`
   image carries **process-global** exception state, not per-thread:
   MIR has no TLS (a Tier-3 floor gap), so the ledger build defines
   `MADC_RT_TLS` empty. Documented in the man page; single-threaded
   programs are unaffected. (iv) Tier-A breadth is the exception +
   VLA runtime; the builtin-shim family (`__madc_popcount`,
   `__madc_bswap*`, the overflow helpers) stays Tier B until madc
   lowers those builtins to native MIR ops instead of calls.
6. **S6 — `madc.ini`** (configure-gated, smallest slice, anytime after
   S3).
   **S6 COMPLETE (2026-07-26): gate GREEN — the carriers track is done.**
   `madc.ini` is a small hand-written reader (`src/madc_config.cpp`,
   `include/madc_config.h`) with a single-owner key table, and the
   precedence rule it completes — **CLI > environment > madc.ini > baked
   defaults** — is enforced in ONE visible place (`main()`, after the
   argument loop) rather than spread across the consumers:
   a `--std=` on the command line wins via `cli_set_std`; `-I` dirs are
   already in front of the ini's `include` dirs because the loop ran
   first (gcc searches configured dirs last); the environment wins by
   being read at each use site *before* the ini value
   (`MADC_FOREST` is discovery arm 4, the `forest` key is **arm 5**;
   `install_resource_guards` checks `MADC_*_LIMIT` before `cpu-limit` /
   `mem-limit`). Keys: `std`, `forest`, `include` (repeatable),
   `cpu-limit`, `mem-limit`. Lookup: `./madc.ini` →
   `$XDG_CONFIG_HOME/madc/madc.ini` (or `~/.config/…`) →
   `<sysconfdir>/madc.ini`, and the first EXISTING file wins outright —
   configs are never merged, because a merged chain makes "why is this
   setting on?" unanswerable. Relative paths resolve against the config
   FILE's directory (a system-wide `/etc/madc.ini` naming
   `forest = groves.msnap` cannot sensibly mean something in whatever
   directory madc was started from) and a leading `~/` expands.
   *Two deliberate design calls.* **STRICT, not tolerant:** an unknown
   key, a foreign section, a missing `=`, an empty value, or a
   non-numeric limit is a hard error naming file:line and the accepted
   keys — a config file is the user's own file, and half-applying it is
   exactly the silent degradation this project refuses (`mem-limit = 8G`
   must say so, not arm an 8 MB guard). **No TOML dependency** (the
   question was raised and answered): the grammar is `key = value` for
   five flat scalars, this code reads a file an attacker may influence
   in a sandboxed deployment so it is sized to be auditable, and a
   template-heavy header library would raise the self-hosting bar for
   every file under `src/`. The reader is one class behind a
   parse-to-value-object seam, so swapping the format later touches one
   file.
   *Where it lives:* the config file is a **CLI-shape feature**.
   `libmadc` never reads one — a file that can redirect where the
   compiler loads frozen state from is an attack surface for a sandboxed
   host — so the CLI parses it and hands the forest path down through
   `RegistrationPolicy::forest_config_path` (on the POLICY, not a plain
   Program field, so `--project` TUs and runtime-eval children inherit it
   through the one propagation point, exactly like `enable_forest_bind`).
   Arm 5 sits inside the `enable_external_forest` gate with the
   sidecar/env arms. A host that wants ini semantics calls the reader
   itself. The `--enable-config-file` configure axis (default **yes**)
   removes the file-reading path entirely for builds that want the
   surface ABSENT rather than merely unused; `--config=` then refuses
   loudly, naming the configure option. New flags: `--config=<file>`
   (the whole search, and it MUST load — a named file that gets ignored
   is the same failure as a named forest container that gets ignored) and
   `--no-config` (skip the search; the two together are a contradiction
   and refuse). The failure diagnostics gained ONE owner for the probed-
   arm list (`Program::forest_probed_arms`), so the loud notice and the
   strict error can no longer drift from the real chain.
   *Hermeticity:* `scripts/run_tests.sh` now passes `--no-config` on
   every madc invocation (including the AOT compile legs, which
   deliberately take no `$BACKEND_FLAG`) — an ambient `madc.ini` in the
   repo root, the developer's config dir, or the system config dir would
   otherwise silently change every test's dialect or include path. A
   local `madc.ini` is gitignored.
   *Evidence:* permanent `scripts/forest_config_gate.sh` in fulltest —
   39 checks over 18 legs, each settings leg PAIRED with a baseline that proves the
   assertion would fail without the config file (the dialect baseline is
   the ABSENCE of `__STDC_VERSION__`, so `std = c99` → `199901L` cannot
   pass by accident; the include fixture is unreachable without the ini;
   `mem-limit = 24` trips the guard and the message names the ini value,
   then `MADC_MEM_LIMIT=4096` overrides it). Arm 5 gets the S3 ordering
   treatment: a valid `$MADC_FOREST` must bind with NO not-a-container
   notice (proving arm 5 was never probed), while the same junk ini path
   with an empty environment IS reached and IS loud. Plus
   `tests/unit/test_config_file.cpp` (15 cases / 60 assertions: the
   strict diagnostics, `[madc]`-only sections, case-insensitive keys,
   quoted values, last-wins scalars, relative/`~` resolution, the
   "explicit 0 is a value, not an absence" distinction the `has_*` flags
   exist for, and the search-chain order). Suites: fulltest
   **{FULLTEST}** with `forest_config_gate: OK`, `--exe` **{EXE}**,
   packed arbiter **{PACKED}**. The **axis-OFF shape** was validated
   directly (a gate cannot reconfigure the tree): rebuilt with
   `ENABLE_CONFIG_FILE=0`, an ambient `./madc.ini` is not read,
   `--config=` refuses naming `enable-config-file`, `--no-config` stays a
   no-op, the unit tests agree with the axis, and the axis-ON restore
   re-reads the file.

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
