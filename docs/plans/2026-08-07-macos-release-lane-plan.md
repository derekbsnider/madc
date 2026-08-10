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

## W1 as implemented (2026-08-10)

The owner's ask ("Mach-O x86 + arm libc++ madc-release binaries with
compressed frozen forest") made W1 concrete: the hosted binaries now pack
the **C++ standard-library groves** beside the C prelude, and a
`release-macos` lane strips, verifies, and tarballs them.

**The C++ world is LLVM's libc++-18 tree** (`LLVM_LIBCXX_INC`,
`/usr/lib/llvm-18/include/c++/v1`) — not the SDK's Apple fork. Three
reasons, all in the src/Makefile comment: Apache-2.0 provenance (the W0
resolution), text already proven at libc++-lane zero, and it sits outside
the CLT prefix map so the cross freezer opens it on the container while
freezer/consumer tables stay byte-identical. madc's own build keeps
compiling against the SDK headers + .tbd stubs.

**Posture: the served tables present GCC** (`MADC_PREDEF_GCC_POSTURE`,
`scripts/gcc_posture_filter.sh` — ONE filter shared by
gen_predefined_macros.sh and gen_darwin_prelude.sh, because the flattened
prelude's kept `-dD` defines carry clang's identity macros and would
re-poison the posture mid-TU). Under `__clang__`, libc++ gates <atomic> on
`__has_feature` probes madc refuses to fake; under GCC posture it keys off
the `__GCC_ATOMIC_*` predefines, which survive the filter.

**Freezer plumbing fixed on the way** (each its own commit):
- `#include_next` / `__has_include_next` now resolve through the embedded
  set position-aware (`embedded_wins_include_next`): libc++'s C wrappers
  reach the prelude by include_next, and on an Apple target the C library
  IS the embedded set. The recursive hosted→cross make no longer leaks the
  CLT prefix map into the cross madc's table (shared HOSTTAB_CXX block +
  per-mode `sys_include_paths.cpp`, embedded_headers.cpp precedent).
- The embedded stdint.h is now the COMPLETE C11 7.20 resource-dir surface
  (least/fast/intmax types + limits + INTN_C), which also unblocks the
  long-standing Linux `<cstdint>` pack blocker; stddef.h gained the
  guarded max_align_t its own comment promised.
- Global-scope struct tags coexist with flat-registered namespace-scoped
  classes (darwin math.h's SVID `struct exception` vs std::exception);
  gate tests/testglobalnstag.mad.
- The object_arg_addr↔class_ctor_call coercion recursion is cycle-guarded
  (libc++ <fstream> under the freeze drain segfaulted on it).
- The mir-cache blob-skip path now honors its own error-containment
  contract (leaks the one-shot MIR context instead of MIR_finish-fataling
  on an unfinished module — the n2 duration<double>::operator%= class).

**Cost accepted in v1:** the darwin containers may ship without the MIR
cache blob when the n2 drain defect trips (consumers rebuild nodes — the
pack still gates unit presence); the C prelude remains SDK-derived until
W0.5, so artifacts stay owner-private; C++ names outside the packed set
fail loudly on header-less Macs (and deliberately resolve nowhere else —
no CLT-version mixing).

New pieces: `scripts/forest_pack_headers_darwin.txt` (the darwin C++
canonical list — deliberately separate from the Linux list; the flavors'
blocker sets diverge), `make -C src release-macos` (build both arches
sequentially → llvm-strip → `scripts/verify_macho_release.sh` proves the
forest bytes + arm64 signature survived), `scripts/package_release_macos.sh`
(tarballs + SHA256SUMS lines + LLVM license notice + quarantine README),
`remote_build.sh release-macos` stage, and `scripts/mac_battery.sh` (W2:
the self-contained evidence run for the owner's Macs).

## W2 first real Mac run (2026-08-10) — the unprototyped-call ABI bug

The battery on the owner's arm64 Mac (macOS 15.3.2) is what W2 existed to
find, and it found a genuine ABI defect on the first run: **2 passed, 4
failed**, with the C prelude probe dying on `SIGBUS`.

Diagnosis (the reasoning matters more than the fix):

- Every faulting address was one of `__madc_builtin_{strcpy,memcpy,memset}_chk`
  / `__madc_builtin_object_size` — all inside ONE 16KB page — while
  `madc_puts`, a page away, worked. That pattern says code-signing page-hash
  invalidation, so it was checked first: `codesign -v` reported **valid**.
  Hypothesis dead in one command.
- lldb (after re-signing a debug copy with `get-task-allow`, since
  linker-signed binaries are not debuggable) gave the real fault:
  `EXC_BAD_ACCESS (code=2)` **inside `_platform_memset`**, storing to
  `0x1002804f4` — with ASLR off, exactly `__madc_builtin_memset_chk`'s own
  address. `x0` held the callee's address; `x1`/`x2` held the correct 2nd and
  3rd arguments. So the helper was called with its OWN address in the
  destination slot.
- The control that named the layer: the SAME call with an explicit prototype
  works (`zzz`); **undeclared** faults. Not the fortify lowering — the
  no-prototype call path.
- Oracle: `clang -S -arch arm64` on `extern long f(); f(b,122,3,32)` passes the
  args in `x0`–`x3` (an ordinary call). madc/c2mir made it **variadic**, and
  Apple arm64 passes every vararg on the **stack** (`mir-aarch64.c`: "all
  varargs are passed only on stack"), so the callee read registers and found
  residue.

Root cause, one line in the fork — `c2mir.c` marked a call vararg when the
callee's parameter list was empty (`dots_p || NL_HEAD (param_list) == NULL`),
a way to let any argument count through without an arity check. Invisible on
x86-64 SysV, where varargs ride the same registers as fixed args; fatal on
Apple arm64. This is a **c2mir defect, not a madc one** — madc's bare `()`
emission matches gcc's gnu89 semantics, and the `--emit=c11` → gcc/clang path
was always correct.

Why it hit the whole C surface: the macOS SDK's `<secure/_string.h>` rewrites
`memcpy`/`memset`/`strcpy`/… into `__builtin___*_chk(...)`, which madc serves
from runtime helpers that no header declares. So every memory-WRITING libc
call on darwin was an unprototyped call, while reads were fine — exactly the
observed split. It would equally break any C89 implicit-declaration call, i.e.
the SMAUG corpus, on Apple silicon.

Fix (fork, Tier 2 — the ABI of a call is c2mir's decision): an unprototyped
call builds its proto from **that call site's actual, default-promoted
argument types** and is NOT variadic. Because the types differ per call site
and `proto_item` hangs off the shared `func_type`, the per-site proto is
stashed on the call node (a slot placed in existing padding, so `struct expr`
does not grow). The emission passes the same promoted type to
`target_add_call_arg_op` so the proto and the pushed op agree; a genuine `...`
callee is untouched.

Gate: `scripts/unprototyped_call_abi_gate.sh` (in `fulltest`). Behaviour
cannot gate this — both shapes run correctly on x86-64 — so it asserts the IR
shape via `c2m -S`: the unprototyped call's proto has no `...` and carries one
entry per actual arg, while a genuinely variadic callee in the same TU still
comes out variadic (the gate's own negative control). A stale `c2m` proved the
control for real: against pre-fix c2mir the gate failed with exactly its
intended message. That staleness was itself a trap worth closing — the tree
sync pushed the NAS's old MIR drivers over the container's fresh ones, so the
sync now excludes MIR build output entirely.

### W2 second Mac run — the darwin runtime table was empty

With the ABI fix in, the battery went **2 passed/4 failed → 3 passed/3 failed**:
the C prelude probe (every `strcpy`/`memcpy`/`memset` on darwin) went from
SIGBUS to clean.

The next failure had a different cause, visible on the owner's x86-64 Mac as
`undeclared identifier _ZNSt3__15ctypeIcE2idE` (`std::__1::ctype<char>::id`):
the hosted-darwin flavor's runtime list shipped **empty**, because
`link_libs()` linked a probe and then read it with `readelf -d` — and an Apple
target emits Mach-O, where the runtime is `LC_LOAD_DYLIB`. The probe reported
nothing, silently, so on a Mac madc had no libc++ to dlopen, every
mangled-direct C++ static failed its CIR-time dlsym probe, and none of them got
declared. Fixed by making the probe format-aware (`-dumpmachine` picks the
container; Mach-O reads through `llvm-otool -L`, subtracting libSystem as the
ELF arm subtracts libc/libm/ld-). No soname is hardcoded — the toolchain still
reports its own runtime. The darwin table now carries
`/usr/lib/libc++.1.dylib`.

Both generated tables also gained their generator as a **prerequisite**: the
rules existed to "generate if missing" with no prerequisites, so editing the
capture logic left the old table in place — the same staleness class as the
stale `c2m`.

### Still open after the second run (precise reducers)

Two darwin-only defects remain, both C++-side and both reduced:

1. **A `__fwd` alias does not survive the darwin bind.** On the Mac,
   `#include <sstream>; std::ostringstream os;` → "use of undeclared identifier
   'ostringstream'" (same for `stringstream`), while in the SAME build
   `std::basic_ostringstream<char>` (the template it aliases) resolves, and
   `std::string`/`std::wstring` (aliases in `__fwd/string.h`) resolve. Evidence
   gathered: the forest HAS the entry (`declindex … type ostringstream` in unit
   22, `__fwd/sstream.h`), the edge `sstream → __fwd/sstream.h` exists, and
   BOTH units bind (`-v` shows "bound to grove unit 630/22") — including when
   the fwd header is included explicitly. So bind is fine and the restore of
   that `type` entry is where it is lost. It does **not** reproduce on Linux
   under `-stdlib=libc++` (freeze + bind of the same header text resolves), so
   suspicion falls on the darwin freeze's check-gate drop set (297 defs dropped
   there) removing what the alias's restore depends on. Next step: instrument
   the decl restore for a dropped-target alias; add a `forest_bind_gate` case
   for an alias whose target lives in another unit.
2. **The `value` intrinsic segfaults on darwin.** `value v = 41; v = v + 1;`
   warns `using pointer without cast for integer type parameter` at the `v + 1`
   line and then SIGSEGVs at 0x0. A signature mismatch on a value-runtime
   helper; the standing trap "intrinsic namespace prototypes must not
   serialize" makes the darwin forest the first suspect (a frozen prototype the
   consumer restores with the wrong types).

The AOT probe's failure is not a bug: in-process loading of Mach-O objects is
genuinely unimplemented ("emit an executable instead"). The battery should
treat it as a known-unsupported expectation on darwin rather than a FAIL.

### Battery hardening (post-W1, same session)

The Linux regression battery over the stdint/stddef completion surfaced a
hidden contract the embedded resource-dir headers had never implemented:
**glibc's `__need` protocol**. Real gcc stddef.h/stdarg.h serve ONE
requested definition per re-inclusion (`__need_size_t`, `__need_NULL`,
`__need___va_list`, …) and CLEAR the request macro; glibc re-includes them
several times per TU under different requests. Three commits, each with
its own reducer:

1. **stddef.h implements the protocol arms** (serve + `#undef` per
   request; no full-header guard on protocol visits). Without it, libc++'s
   `<cstddef>` → `stddef.h` wrapper chain mis-routed — 26 libc++-lane
   fails.
2. **A live `__need_*` request bypasses — and is never recorded by — the
   once-only caches** (`need_protocol_macro_live()`, lexer.cpp): the
   name-level dedup, the baked PCH, and the forest bind are all
   full-content one-shots, and a protocol visit must not consume or
   satisfy them. Without it, glibc `stdlib.h` under `--std=c17` lost
   `wchar_t` on its second protocol re-include.
3. **stdarg.h clears `__need___va_list`** — a leaked request macro marked
   EVERY later include in the TU a protocol visit, defeating the caches
   TU-wide (12 ns_madc/eval/channel fails).

The stdint completion then paid for itself at the PP-parity ratchet:
`forest_dm_oracle --rebaseline` (reviewed) resolved **102 known gcc-vs-madc
divergences** (the `INT*_C` + least/fast limit macros), baseline 305→205
lines, +2 madc-only implementation guards.

Final gate: fulltest **1018/0**, libc++ JIT lane **1014/0** (both +1 for
the new `testglobalnstag`). The darwin release binaries were then
**rebuilt** so the containers re-bake the fixed embedded
stddef/stdarg/stdint — the first tarballs predated these fixes.

The first PACKED run after the protocol fixes then exposed the same
contract's **producer half** (25 varargs/ns_madc failures): the freeze
had formed a husk unit named `stdarg.h` out of stdio.h/wchar.h's
`__need___va_list` servings (8 tokens — two typedef servings), and a
consumer's plain `#include <stdarg.h>` bound it by name — no `va_list`,
no `va_start`. Fix at the producer taps (commit `8a89ed9d`): a protocol
serving belongs to the *includer's* unit — no edge/unit/canon entry
forms for the served header; PP events route to the includer via a
`pack_current_unit()` override; served tokens are owned by token
IDENTITY (`pack_protocol_token_owner` — the buffer mutators keep
pointers stable where absolute indices break), honored by both freeze
partitions (the token bucketer and, via the new `cir_unit_owner_fn`
override, the record partition — the container layer stays
Program-blind). Gate: `forest_bind_gate.sh` case `need`
(negative-control verified against the pre-fix binary). The darwin
forests were dump-verified husk-free (the flattened prelude leaves no
live `__need` requests at freeze time), so the darwin rebuild after
this fix carries the corrected lexer rather than repairing artifacts.
