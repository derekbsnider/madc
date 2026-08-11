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
- **W0.5 — provenance-clean prelude**: ~~assemble the open-licensed
  header tree (evaluate adopting Zig's curated macOS libc tree + LLVM's
  libc++ headers, matched to the target OS version), point
  `gen_darwin_prelude.sh` at it, and DIFF the resulting prelude against
  the current SDK-derived output to find declaration gaps (the
  SDK-derived prelude stays as the private-use oracle, never shipped).
  Ship the license notices.~~ **DONE, session #81 (2026-08-11,
  d7ada73e)** — see "W0.5 as implemented" below. The first diff found
  ZERO declaration gaps; public artifacts are now gated mechanically by
  the binary's own provenance marker.
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
- **W3 — libmadc dylib question**: ~~Linux packages ship `libmadc.so.0`
  because `-o` executables reference it at runtime. Verify what
  darwin AOT output references; if needed, build
  `libmadc.dylib` (`@rpath` install_name) in the hosted MODEs and
  include it in the tarball.~~ **ANSWERED + SHIPPED, session #81
  (04b78626)** — see "W3 as implemented" below. What darwin consumers
  actually reference today is the emitted-C runtime (the 8-symbol
  exception/cleanup vocabulary + `__madc_vla_free`), served by a
  `lib/libmadc_rt.a` subset archive in the tarball. The full
  `libmadc.dylib` stays deferred until darwin AOT `-o` exists (the
  battery's AOT leg still reads the loud decline).
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
pack still gates unit presence); ~~the C prelude remains SDK-derived until
W0.5, so artifacts stay owner-private~~ (CLEARED session #81 — the prelude
is open-provenance and the verify gate enforces it); C++ names outside the
packed set fail loudly on header-less Macs (and deliberately resolve
nowhere else — no CLT-version mixing).

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

   **RESOLVED / SUPERSEDED (session #75) — read the section below instead.**
   Almost every inference in the paragraph above turned out wrong: it is not
   darwin-only, it is not a restore bug, and the check-gate drop set is not
   involved. "The forest HAS the entry" was read off the **decl index**, which
   is a different structure from the **type graph** — the index listed
   `ostringstream` while the arena held no record for it at all.

### The real defect: libc++-flavor freeze, not darwin (session #75)

**Scope correction.** This is a **stdlib-flavor** defect in the freeze, fully
reproducible on Linux x86-64: freeze a libc++ grove set with
`bin/madc -stdlib=libc++ --freeze-append=`, bind it, and `std::ostringstream`
is gone — while a **live** parse of the same headers resolves it. Darwin was
the only place it showed because darwin's grove set is the only **shipped**
forest frozen from libc++ (the Linux forest packs libstdc++). Nothing about
Mach-O, code signing, or the cross-freeze is involved. Everything below was
diagnosed and gated container-side with no Mac in the loop.

Losses are **per-unit and complete**: every alias in `__fwd/sstream.h`
(`stringbuf`, `istringstream`, `ostringstream`, `stringstream`) and
`__fwd/fstream.h` (`ifstream`, `ofstream`) is lost; every alias in
`__fwd/string.h`, `__fwd/string_view.h`, `__fwd/streambuf.h`,
`__fwd/ostream.h`, `__fwd/istream.h` survives. Neither the `using`-vs-`typedef`
spelling, the template's parameter count, nor the `_LIBCPP_PREFERRED_NAME`
re-declaration tail discriminates them — all three were checked and refuted
against the surviving aliases.

**Root cause (fixed).** `using ostringstream = basic_ostringstream<char>;` in
`__fwd/sstream.h` names a template that is only *declared* at that point, so
`instantiate_template_use` misses and madc mints a **concrete opaque forward
tag** instead. That tag is MINTED, never parsed to a `}`, so it has no
completion hook — therefore no project type-id, therefore invisible to
`cir_forest_arena_refresh`, whose sweep walks the project *table*. The
namespaced-alias walk in `cir_forest_arena_complete` then minted the id itself,
**after** the arena snapshot (`f.arena = prog->forest_arena`), found no record
at that slot, and silently skipped the alias — leaving the name in the decl
index and absent from the type graph. The tell-tale was the target tids:
16794105–16794110, contiguous in map-iteration order, far above every
surviving alias's.

The fix is the missing arena write-through at the one funnel every opaque mint
passes through (`Program::stamp_opaque_mint_context`), which is also where the
concrete-vs-placeholder verdict is made. Pattern-context placeholders are
unaffected — `forest_arena_record_aggregate`'s own kill arm still drops them.
Gate: `forest_bind_gate` case **`fwdalias`** (negative-controlled: with the
write-through disabled it fails `bind output '' != 'p=1'`).

**Two further layers were found one level deeper than the alias record.** The
static-member one is FIXED (below); the husk one is still compiled out:

- `FEATURE_FOREST_ALIAS_SHELL_COMPLETE` — with the alias name restored, the
  target is still an empty husk (`Unidentified member 'str'`). **Mechanism FOUND
  and measured (session #76); still compiled out, because it fails WORSE than the
  husk.**
  The earlier reading of this ("needs the origin recorded at the mint site, or a
  canonical-spelling → (template, args) resolver") asked the wrong question. Live
  does not complete these shells through the origin replay at all — it **demands
  completeness**, and `record_pending_template_instantiation` (template name +
  mangled key + arg tokens) is written **unconditionally** by the same opaque arm
  that records the origin only `if (unresolved_surface)`. That is exactly why all
  96 targets measured `origin=NO`: `complete_shell_class_type` is the wrong owner
  for a concrete bodyless-declaration mint. The producer can make the same demand
  itself at freeze time (`request_template_instantiation_completion`), and by end
  of parse `<sstream>` has defined what `__fwd/sstream.h` declared. No
  spelling→type resolver is needed (there is none: searched `from_spelling`,
  `spelling_to_type`, `resolve_type_spelling` — only pch.cpp's builtins-only
  `builtin_datadef_from_spelling`).
  **Measured with it on**, 894-unit libc++ grove set: 96 opaque alias targets →
  **18 complete**, 2 stay incomplete, 76 have no pending record at all
  (`__enable_if_t` SFINAE internals minted by the alias-template arm, which
  records neither origin nor pending — nobody names them). Cost: freeze 37s →
  52s, records 1.27M → 1.55M (+22%), container 7.05 → 7.62MB.
  **Why it stays off:** `std::ostringstream os; os << 7; os.str()` under a bound
  grove goes from a hard error to **exit 0 with the wrong value** (empty). A
  silent wrong answer never ships. The completed class is right in every other
  measurable — `sizeof` 264 == live == clang++, `rdbuf()` non-null,
  `good()/bad()/fail()` identical to live — yet BOTH `operator<<` overloads (int
  and `const char*`) and `str()` yield nothing. So construction, layout and
  stream state all survive the completion, and the remaining defect is one layer
  below: what a completed class CARRIES. Next step: split the write half from the
  read half (does the char reach the stringbuf?) and compare the completed class's
  method BODIES against live.
  Two unrelated defects surfaced while building discriminators, both filed with
  reducers and a clang++ oracle: `(long)os.tellp()` → "conversion of non-scalar
  value requested" (a cast ignores `std::fpos`'s `operator streamoff`), and
  `(std::stringbuf *)os.rdbuf()` → "'stringbuf' is not a member of namespace
  'std'" on a LIVE parse — where the plain declaration `std::stringbuf *sb = 0;`
  works on both flavors, so the alias IS registered and the CAST type-name path
  is what cannot resolve a qualified `std::` name.
  The one piece that DID ship from the attempt: `Program::SilentReplay`, the
  extracted mute-cerr + rewind-diagnostics scope. A freeze-time replay without it
  turns one libc++ SFINAE probe into a compile error on the producer's TU and
  aborts the freeze; `complete_shell_class_type` already owned that discipline
  inline, so both sites now share one implementation.
- ~~`FEATURE_FOREST_CLASS_STATIC_ALIAS`~~ **FIXED (session #76) — and the fix
  is one layer below where the guard sat.** The owner's x86-64 laptop error,
  `undeclared identifier _ZNSt3__15ctypeIcE2idE` at `locale:378`, also
  reproduces on Linux under a libc++ forest bind. Live records a static data
  member's storage Variable **while parsing the class body** (vfEXTERN + the
  alias its OWN owner derives — `class_static_member_itanium_symbol`); a bound
  class never parses that body, and the restore **re-derived** the alias with
  the namespace-variable owner over the FLAT storage key `Tag__member`, so one
  identifier component came out (`_ZSt14ctype_char__id`) where the nested
  `_ZNSt3__15ctypeIcE2idE` is what libc++ exports. Which error you saw depended
  on where the reference came from: a restored grove body named the real symbol
  nothing had declared (undeclared identifier), a consumer-side instantiation
  named the invented one (`import of undefined item _ZSt14ctype_char__id`).
  The guarded pass CORRECTED the name after the wrong owner wrote it — a shim.
  The producer already held the right symbol for every category, so the fix is
  to **transport** it: `cir_forest_global_record.alias_id` (container format
  **v39**), used verbatim at restore, derivation kept only as the no-alias
  fallback. The correction pass and its staging vector are deleted.
  Landing it needed a second, independent fix first: the CIR tree ended up with
  a properly typed `extern struct locale__id <sym>` AND a late
  `extern int <sym>`, because the referenced-global extern pass derived its type
  spec with a bare `append_type_specs`, which cannot express a struct/union tag
  and falls through to `int`. That was one of THREE hand-rolled copies of the
  declaration type-spec derivation (`var_decl`'s `static` arm degraded a
  class-typed file-scope static the same way — `static Cls g;` compiled as
  `static int g`, reduced in `tests/teststaticclassglobal.mad`); all now share
  `append_var_type_specs`. Gates: `forest_bind_gate` case `statmem` plus that
  reducer. Note the constraint the guarded version had discovered and the
  transport sidesteps entirely: availability could not be tested at flush time
  at all, because `cir_open_stdlib_runtime` does not dlopen the flavor runtime
  until the tree build, so dlsym answers "no" for every real libc++ export.
  Frontier after the fix: `std::use_facet<std::ctype<char> >` runs correctly
  under a bound libc++ grove; `std::cout << "hi"` now reaches RUNTIME and
  SIGSEGVs inside `std::locale`'s copy ctor via `ios_base::getloc` — the next
  layer, and a different defect.
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

### W2 third Mac run (session #77) — the C++ surface is alive; one ABI bug is left

Rebuilt both arches at format v39 (`remote_build.sh release-macos`: 835 units
per arch, `verify_macho_release: OK`, tarballs pulled) and re-ran the battery on
the owner's arm64 Mac. The composition of "3 passed / 3 failed" changed
completely from the second run:

**`std::string`, `std::vector` and `std::map` now all work on darwin.** The
static-member alias transport (`bf59611b`) is what did it — probed individually
on the Mac, each prints the right answer. The whole C++ surface used to be dead
here. The `__tree` "incompatible types in assignment to a pointer" warning is
pre-existing and benign: it appears identically on Linux, where the same program
runs correctly.

**What is left is one precise defect, and it is an ABI bug in madc's lowering —
not a darwin bug.** The correlation on the Mac is exact:

| call | kind | result |
|------|------|--------|
| `cout.put()`, `.flush()`, `.write()`, `operator<<(int)` | `basic_ostream`'s own members | **work** |
| `cout.getloc()`, `.fill()`, `.widen()`, `use_facet<ctype<char>>` | inherited from a **virtual base**, returning a class **by value** | **SIGSEGV** |

That is why `std::cout << 42` (a member overload) works while
`std::cout << "hi"` (the free template, which calls `__os.fill()`) faults.

`src/cir_builder.cpp` (~9682) lowers a by-value non-trivial class return by
**prepending an explicit `void*` parameter** to the C11 declaration. Its own
comment names the assumption: *"(g++ canon: get_allocator() receives the sret
slot in %rdi, this in %rsi)"*. That holds **only on x86-64 SysV**, where the
indirect-result pointer is the first argument register. **AArch64 passes it in
`x8`**, outside the argument sequence — so on arm64 the sret pointer lands in
x0, `this` shifts to x1, the callee reads the caller's stack temp as `this`, and
copying its refcounted member faults at a junk address.

Evidence, in the order it was established:

- Every one of the 33 MIR imports resolves on the Mac (probed with `dlsym`) —
  including `ctype<char>::id`, `do_widen` and the `__madc_*` ledger runtime. The
  null is computed at run time, not an unresolved symbol.
- The emitted MIR is **byte-identical on both targets**
  (`proto2: proto u64:p, u64:p` — void result, two pointer args;
  `call proto2, _ZNKSt3__18ios_base6getlocEv, U_1, U_12`). x86-64 prints `up=A`;
  arm64 faults. Same IR, different ABI ⇒ the assumption lives in madc.
- **Oracle** — `clang++ -S -O0` on the Mac for `sink(b.getloc())`:
  `sub x8, x29, #16` immediately before `bl __ZNKSt3__18ios_base6getlocEv`,
  with `this` in x0.
- The vtable is read correctly: madc and clang print identical slots and both
  agree the vbase offset is `vt[-3] == 8`, matching
  `static_cast<std::ios_base*>(&std::cout) - &std::cout`. The virtual-base
  adjustment is *not* the bug.
- Not a general struct-return bug either: an imported C function returning a
  32-byte struct (built as a dylib with clang on the Mac) works on arm64 and
  matches the clang oracle — that path declares a real aggregate-returning proto
  and lets c2mir/MIR place the hidden pointer.

**Fix direction (Tier-1, deepest layer):** declare the callee with its real
by-value struct return and let c2mir/MIR apply the target ABI, instead of
hand-rolling the hidden pointer as a parameter. `__retbuf` stays for madc's own
multi-return feature, which has no ABI counterpart; anything crossing the
platform ABI boundary must use the real type. This predicts breakage on **any**
AArch64 target, so a future Linux-arm64 madc carries the same bug — and no
Linux-x86-64 lane can ever catch it.

**AOT leg** (`scripts/mac_battery.sh`, fixed this session): the earlier note
guessed the wrong half. `-c` **succeeds** and writes the object; the *run* half
declines — `cannot load object: in-process loading of Mach-O objects is not
supported yet (emit an executable instead)`. The leg now encodes that as a
negative-controlled known-unsupported expectation (compile must succeed; the run
may decline loudly), and its success arm starts gating the real round-trip the
day the Mach-O loader lands, with no edit needed.

#### Why the obvious madc-only fix does not work (attempt 1, reverted)

The first cut declared the extern with its REAL by-value struct return and
stored the call into the temp, expecting c2mir/MIR to place the hidden pointer
per target. It **broke x86-64 immediately** — `proto2: proto i64, u64:p`, a
register return, and `std::locale`'s copy ctor faulted at 0x18 on Linux.

The reason is the whole point: **c2mir only sees C, and C classifies returns by
SIZE.** `c2mir/aarch64/caarch64-ABI-code.c: target_return_by_addr_p` is
`(struct|union) && type_size > 2*8`; x86-64 runs the SysV classifier. C++ says a
**non-trivially-copyable** class is returned indirectly *regardless of size*.
`std::locale` is 8 bytes — one pointer — so every C rule puts it in a register.

That also explains the darwin symptom set exactly: `std::string` (32B) and
`std::vector` (24B) exceed the 16-byte threshold, so they were already returned
by address and worked. Only a SMALL non-trivial class is mis-classified, and
`std::locale` is the one on the `operator<<(const char*)` path.

There is no faithful C11 spelling for "this class is always returned
indirectly", so per `.claude/rules/lowering-vs-raising.md` this is a genuine
**Tier-2 raise** (the `_Complex` precedent: no faithful C11 form, ABI fidelity
at stake). The attempt is saved at `tmp/sret-attempt-1.diff`.

#### The design to build

The property belongs to the **type**, not to any one function: in C++ a
non-trivial class is returned indirectly everywhere. So mark the class and let
every ABI path read it.

1. **madc** emits the marker on the struct definition it already generates for
   such a class — an `N_ATTR` on the declaration, reusing the exact attribute
   plumbing `__attribute__((cleanup))` already uses (`NL_EL(decl_node->u.ops, 2)`,
   c2mir.c ~9360). `class_return_via_retbuf()` is already the single owner of
   "is this class non-trivial", so it decides who gets marked.
2. **c2mir** records the bit on the tag node and exposes
   `type_forced_return_by_addr_p(type)`.
3. **Each target ABI file** consults it at ONE point — the function that decides
   register-returnability (`reg_aggregate_size` on aarch64, `process_ret_type`
   on x86-64). Reporting "not register-returnable" makes every downstream path
   (`target_add_res_proto`, the call-res op, the ret ops) fall through to the
   `simple_*` RBLK route already used for large structs. One hook per target,
   not five.
4. **madc's call sites** then declare the extern with the real by-value return
   and store the result — the single owner from `tmp/sret-attempt-1.diff`
   (`external_object_return_call`, adopted by both `emit_symbol_method_call` and
   `class_operator_external_call`).

madc's OWN definitions keep `__retbuf`: both sides agree there, so it is correct
on every ABI, and the callee's deep copy into `*__retbuf` is real semantics, not
a calling convention.

**Gate:** IR-shape, following `scripts/unprototyped_call_abi_gate.sh` — behaviour
cannot distinguish the shapes on x86-64, so the gate must assert the proto: a
small non-trivial class return declares an RBLK arg and no hand-rolled leading
pointer parameter. Its negative control is a small TRIVIAL struct, which must
still return in a register.

**Cost:** a fork commit + `MIR_COMMIT` bump in the same madc commit (pin
discipline), and `MIR_VERSION` + a fork tag if a release ships it. The fork
branch is `origin/feature/indirect-return-attr` (pushed). Note `/workspace/mir`
carries seven remotes and `origin` — the real fork, over SSH — is not the first
one listed; the alphabetically-first `cyanogilvie` is a third-party HTTPS mirror
that will prompt for a password. Push `origin` explicitly.

#### Attempt 2 (also reverted) — and the constraint it uncovered

With the c2mir raise in place (`__attribute__((indirect_return))`, fork
`bb9ae29b` on `feature/indirect-return-attr`), the madc side declared the extern
with its real by-value struct return and stored the call into the destination.
The IR shape came out exactly right on both targets —
`proto2: proto rblk:8(Ret_Addr), u64:p` — `use_facet` printed `up=A`, and the
groves probe printed `sz=5 a=25 b=1`.

Then fulltest came back **1010 passed, 9 failed**, all in the operator lane
(`teststringplus`, the three `*_realhdr` string-plus tests, `testmanip`,
`testopinherit`, `testcstdio`, `testheaderstringops`). The failure is the
interesting part: `teststringplus` prints all four expected lines **correctly**
and then aborts at teardown with `free(): invalid size`.

That is a bit-copy of a `std::string`. c2mir's RBLK path always materializes the
result into the caller's call-arg scratch area and then block-copies it to the
destination (`simple_add_call_res_op` → `target_gen_post_call_res_code`) — there
is no copy elision. libstdc++'s small-string optimization makes `_M_p` point at
the object's OWN internal buffer, so a bit-copy leaves the destination pointing
into the dead scratch slot, and its destructor frees that.

**So the hidden result pointer is not merely an ABI encoding — it is what makes
construct-in-place expressible.** A non-trivially-copyable class must be
constructed directly in the caller's slot, never copied out of a temporary;
that is exactly why C++ returns it indirectly in the first place. Declaring the
real by-value return hands the object back through C's value semantics, which
for these types is precisely what must not happen.

**Revised fix (next slice).** Keep the hidden pointer as a PARAMETER — madc
already passes `&c` directly, so the callee constructs in place and nothing is
copied — and mark it so MIR places it in the indirect-result register instead of
the first argument register. `MIR_T_RBLK` is already exactly that: c2mir models
its own struct returns as a leading `RET_ADDR_NAME` argument of type
`MIR_T_RBLK` (`simple_add_res_proto`), and MIR's backends place an RBLK arg per
the target's indirect-result rule — x8 on AArch64, the first argument register
on x86-64. So the raise moves from the return type to the parameter: an
attribute marking a function's leading pointer parameter as the result address,
which c2mir emits as `MIR_T_RBLK` (size = the class's size) on both the proto
and the call.

This makes the madc side nearly a no-op: the call shape, the argument order and
the construct-in-place semantics all stay exactly as they are today, and only
the parameter's MIR type changes. x86-64 codegen is unchanged by construction.

The fork's `feature/indirect-return-attr` commit implements the return-type
form; it is not merged and `MIR_COMMIT` is untouched, so nothing depends on it.
Revise it to the parameter form rather than building on it.
`scripts/sret_abi_gate.sh` is written and stays valid for the revised fix (an
RBLK still appears in the call's proto); it is unwired from `fulltest` until the
fix lands.

#### Found while gating: `std::locale l = std::cout.getloc();` is broken in the DEFAULT lane

Independent of the sret work, and pre-existing (verified by stashing the fix and
rebuilding — the baseline warns and aborts identically):

```
tmp/loc1.mad:3:46: warning -- incompatible argument type for pointer type parameter
free(): invalid pointer   -> SIGABRT
```

In the libstdc++ lane `getloc` does not bind to the library symbol at all — the
emitted C shows madc's OWN `ios_base__getloc(struct locale *__retbuf, struct
ios_base *__this)` and a call `ios_base__getloc((&l), (&_ZSt4cout))`. So the
warning is about madc's emitted body, not the external ABI path, and the abort
is `l`'s cleanup destructing something that body never constructed. Under
`-stdlib=libc++` the same source binds the real external symbol and exits 0.

This is why the sret gate's reducer uses `std::string c = a + b;` instead: in
the default lane that is a genuinely EXTERNAL non-trivial by-value return
(libstdc++'s `_ZStplIcSt11char_traitsIcESaIcEE...`), so the gate exercises the
path the fix changes. A `getloc` reducer would have gated a madc-emitted
function and proved nothing.

Reducer kept at `tmp/loc1.mad`; own slice, own commit.

#### Known limitation: `--emit=c11` stays x86-64-only for this construct

The fix corrects the JIT/native path, where madc owns the pipeline through MIR
and can hand the placement decision to the target. It does **not** fix
`--emit=c11`, and cannot.

Emitted C declares the callee `void f(struct X *, void *this)` and calls
`f(&c, &a, &b)`. Compiled by gcc/clang on x86-64 against real libstdc++ that
works, because the indirect-result pointer is the first argument register there.
The same emitted C on AArch64 is broken exactly as the JIT was. This predates
the fix — it is the same x86-64 assumption, and the emit path never had the
marker to hand off.

It is not really a madc shortcoming: **portable C11 cannot call a C++ function
that returns a non-trivially-copyable class by value**, for the same reason C
cannot call C++ generally. There is no C spelling for "always returned
indirectly", and the callee must construct in place in the caller's slot. A real
answer would be a generated C++ shim at that boundary; the limitation is
confined to the mangled-direct edge, since madc's own emitted functions use
`__retbuf` on both sides and are portable anywhere.

`cir_emit_c.cpp` therefore DROPS the `ret_addr` attribute rather than rendering
it. gcc/clang do not know it, would warn "unknown attribute", and would then
treat the parameter as the plain pointer it already is — so dropping keeps the
emitted C byte-compatible with its pre-fix behaviour and emits no diagnostic.
The precedent is the same renderer's `linkonce` -> `weak` translation; the
difference is that `ret_addr` has no portable equivalent to translate TO.

#### NEXT SLICE (owner-directed): make `--emit=c11` target-neutral too, via a struct

Owner 2026-08-11: c2mir-internal machinery for the JIT route is fine, but the
emit-C lane needs to work as well — "if this can be done using a struct, then
let's do that".

It can, and it needs no `#ifdef`. **Declare the extern's return type as an
opaque struct larger than 16 bytes.** AAPCS64 and SysV both force anything that
big to the indirect path, so gcc/clang put the destination address in x8 on
AArch64 and in the first argument register on x86-64 *by themselves* — the
emitted C stays target-neutral, which is the same principle as the JIT fix
(describe; let the target decide). Arch-conditional emitted C would violate the
vision doc's no-hardcoded-targets invariant, so `#ifdef` is the wrong tool here
even where it would work.

**The constraint that shapes the design:** the destination must BE the object
the compiler writes into. Verified on the Mac —

```c
struct pad32 { long long a[4]; };
struct pad32 c = f(p);        /* clang arm64: `mov x8, sp` -> &c直接, no copy */
```

Any other spelling (`*(struct pad32 *)dst = f(...)`, a shim that assigns
through a pointer) makes the compiler materialize a temporary and block-copy
it, which is precisely the bit-copy that corrupts a self-referential
small-string. So the emitted form must be an INITIALIZER whose variable is the
result slot:

```c
struct __madc_ret32 { long long __a[4]; };                 /* >16B, 8-aligned */
extern struct __madc_ret32 SYM(void *, void *);
struct __madc_ret32 __madc_objtmp_7 = SYM(&a, &b);         /* slot IS the temp */
/* the object lives at (struct basic_string *)&__madc_objtmp_7 */
```

Only needed for classes **≤16 bytes**; at 17+ the real class type already forces
the indirect path on both ABIs, so the extern can name the real type and no
padding is involved.

**Where it goes.** This is a tree SHAPE (destination declaration fused with the
call), not a rendering choice, so `cir_emit_c.cpp` cannot do it alone — it walks
and prints. Either the builder emits this shape for the emit-C target, or an
emit-C pre-pass rewrites `SYM(&dst, args...)` + the separate `dst` declaration
into the initializer form. Prefer the pre-pass: one IR stays the source of
truth (MC11-IR), and the JIT lane keeps the ret_addr parameter it now uses.

**Gate:** compile the emitted C for arm64 and RUN it. Nothing does that today —
the fidelity gate compiles on x86-64, where both shapes pass, so this gap is
invisible to every existing lane. The Mac is the only place that can currently
run it (`clang -target arm64-apple-macos` on the container can at least compile
and check the x8 setup in the asm; running needs the Mac).

**⚠️ OPEN QUESTION — settle this BEFORE implementing, not during.** The
initializer form works because the destination *is* the object being declared.
That covers a fresh temp. It does not cover the case where the result goes into
an object that **already exists**:

```c++
std::string c;        /* already constructed, maybe already holding a value */
...
c = f();              /* no initializer to fuse the destination into */
```

Here there is no declaration to attach the call to, and every spelling that
assigns through a pointer (`*(struct pad32 *)&c = f(...)`, a shim, a temp plus
copy) reintroduces exactly the bit-copy that corrupts a self-referential
small-string — the failure that killed attempt 2. So the pre-pass needs a
defined answer for it, and the plausible ones differ a lot in cost:

- emit a fresh padded temp as an initializer, then run the class's real
  `operator=` from the temp into `c` (correct, but adds a copy-assign the JIT
  lane does not do — and needs the assignment operator to be reachable at that
  point in the emitted C);
- restrict the rewrite to initialization contexts and leave assignment on the
  old (x86-64-only) shape, which is honest but leaves emit-C partly unfixed and
  must then SAY so rather than look complete;
- hoist the destination's declaration to the call site so the initializer form
  always applies (a bigger tree change, and illegal where the object's lifetime
  must start earlier).

Whichever is chosen, the answer belongs in this doc before code is written; the
first two are the realistic candidates.

**ANSWERED (session #79, 2026-08-11): the question dissolves — the tree already
routes every such case through a madc-declared local.** Reading the actual
emitted C (specimens `tmp/emitc_specimen{,2}.mad|.c` on the container) instead
of reasoning from the source construct:

- `c = b + a;` into an existing string ALREADY lowers to a fresh
  `__madc_objtmp_N` (cleanup-tagged), the extern constructing into `&tmp`, then
  the real mangled `operator=` (`_ZNSt…aSERKS4_(&c, &tmp)`). Candidate 1 was
  already built years-of-sessions ago as the assignment lowering itself.
- `return s + s;` in a madc function ALREADY lowers to temp + real copy-ctor
  into `__retbuf` (`_ZNSt…C1ERKS4_(__retbuf, &tmp)`) — the builder never passes
  its own `__retbuf` through to an extern callee.

So the destination of an extern `__retbuf`-shaped call is ALWAYS a local the
builder itself declared (a named local under initialization, or an objtmp), and
the pre-pass needs **no assignment machinery at all**: it only fuses that decl
with its constructing call into the initializer form. What the specimens DID
surface that the candidates above missed:

- **Decl and call are not adjacent** — argument temps land between them
  (`std::string e = a + b + a;` declares `e`, then the inner temp, then calls
  into `&e`). The fusion therefore MOVES the declaration down to the call
  statement's position. That flips cleanup order between `e` and the argument
  temps (C++ destroys the temp at full-expression end, strictly before `e`);
  madc's lowering already extends temp lifetime to scope end, so this stays
  inside the accepted relaxation — the objects are independent by construction
  (the fused call deep-copies out of its argument temps).
- **Per-symbol all-or-nothing.** The extern has one declaration; a symbol with
  both fusable and unfusable call sites cannot be half-retyped. The pre-pass
  plans all sites first and retypes a symbol only when EVERY site fused;
  otherwise every site keeps the old shape (consistent, x86-64-only) and is
  counted.
- **Residuals (kept on the old shape, counted, warned to stderr):** a
  destination that is not a same-block local declaration (today that is global
  initialization in `__madc_global_init`), a symbol whose address is taken
  outside callee position, labels/gotos or a nested shadowing redecl between
  decl and call, and calls through function pointers (an abstract-param type —
  the same known gap `sret_abi_gate.sh` documents for the JIT lane).

**The mechanics (all verified against real compilers before coding):**

```c
struct __madc_ret_K {            /* per class K, emitted after K's def */
	_Alignas(_Alignof(struct K)) unsigned char
		__pad[sizeof(struct K) > 16 ? sizeof(struct K) : 17];
};                               /* >16B forces the indirect path on BOTH ABIs;
                                    unsigned char members can never be an HFA,
                                    so no AArch64 float-aggregate trap; sizes
                                    computed by the C compiler, never baked */
extern struct __madc_ret_K SYM(void *, void *);   /* was: void SYM(K *__retbuf, …) */
struct __madc_ret_K X = SYM(&a, &b);              /* slot IS the destination */
static void __madc_ret_dtor_K(struct __madc_ret_K *p) { DTOR((struct K *)p); }
                                 /* cleanup shim: gcc requires the cleanup fn to
                                    take a pointer to the VARIABLE's type */
```

Oracle (2026-08-11, container): gcc x86-64 `-O0 -S` passes `&c` directly in
`%rdi`, zero memcpy; clang `--target=arm64-apple-macos13 -O0 -S` emits
`add x8, sp, #24` before the call, zero memcpy; a hand-written C caller in this
exact shape calls real libstdc++ `_ZStpl…` and prints `c=abcd` (rc=0). The
arm64 leg also proves the cross `-S` gate is runnable on the container.

**Where:** a `CirBuilder` method run from `madc_cir_emit` between
`translate_module` and `cir_emit_c`, for `lang == C11` ONLY — the `.mc11` twin
stays the faithful IR serialization, and the JIT/native lanes never see it (the
emit path builds its own tree and tears it down; nothing shares it). Later
`&X` uses are re-wrapped `(struct K *)&X` position-safely
(`c2mir_op_splice_after` + `c2mir_op_remove`), which existing call-site casts
tolerate (they cast an explicit pointer again).

**IMPLEMENTED (session #79).** `CirBuilder::emitc_lower_indirect_returns`,
invoked from `madc_cir_emit` under `lang == celC11` unless
`MADC_XTEST_NO_SRET_LOWER` is set (that knob exists solely as the gate's
negative control; `MADC_XTEST_SRET_LOWER=1` prints per-symbol accept/reject/
fuse decisions to stderr, because a pass-A rejection is otherwise
indistinguishable from "no candidates"). Findings from building it:

- Module items sit inside nested `N_LIST` groups the renderer flattens
  invisibly — the libc++ lane's `getloc` extern and `struct locale` def were
  in one. Pass A flattens; the pads/shims splice anchor stays a DIRECT child
  of the top list.
- The return-spec check must tolerate `extern` riding the same spec list
  (`[N_EXTERN, N_VOID]`) — requiring exactly one `void` spec silently
  rejected every `need_output_extern` proto.
- The default-lane `getloc` husk (TU-defined weak body) is correctly EXCLUDED
  by the has-definition test — its known-wrong body (`free(): invalid
  pointer`, recorded earlier in this doc) reproduces in emitted C exactly as
  in the JIT lane, pre-existing and unrelated to the rewrite; the gate's
  ≤16-byte leg uses `-stdlib=libc++`, where getloc binds the real external.
- A pre-existing ordering defect blocked the arm64 leg: Pass 1.95's late
  declarations landed AFTER the Pass-1.6/1.7/1.8 synthesized-dtor
  definitions that call them (clang rejects; gcc warns). Fixed in its own
  commit — the late lists now splice at an anchor captured before Pass 1.6.

Gate: `scripts/emitc_sret_gate.sh` (wired into fulltest, ~8s): [default]
mechanism audit (no undeclared-definition `__retbuf` extern survives) +
non-vacuity (≥3 rewrites) + trivially-copyable negative control + gcc
compile/RUN against libmadc == `g++ -O0` oracle byte-for-byte; [arm64] clang
cross `-S` with an x8 setup adjacent to every rewritten call; [libc++] the
8-byte locale pad leg compiles, links against real libc++ and runs; [negctl]
the audit must detect the unlowered shape with the rewrite disabled.

**Still owed:** ~~RUNNING the emitted C on the Mac~~ — **DONE, session #80
(2026-08-11)**: emitted specimens compiled with Apple cc on the arm64 Mac, ran,
and byte-matched Apple clang++ oracles; on-target `cc -S` confirms x8. See
"Session #80" at the end of this doc (execution proof + the emit-lane
runtime-open defect it flushed out + the W3 runtime evidence).

The sret fix is real and verified — but re-running the Mac probes shows the
darwin blocker only partly cleared:

| probe | before | after |
|-------|--------|-------|
| `use_facet<ctype<char>>(cout.getloc())` | SIGSEGV | **works** (`up=A`) |
| `cout.write` / `cout << 42` | works | works |
| `cout.fill()` / `cout.widen()` | SIGSEGV @0x9548cb | SIGSEGV @0x9560a2 (same class) |
| `cout << "hi"` | SIGSEGV @0x0 | SIGSEGV @0x0 |

The darwin MIR gives it away: `call proto6, _ZNKSt3__18ios_base6getlocEv, U_1,
U_3` — **no rblk**. The fix is not reaching that call there.

Reproduced on Linux, same binary, same source, one flag apart:

```
LIVE  -stdlib=libc++              -> 2 rblk in the MIR, fill=32
BOUND -stdlib=libc++ --forest-bind -> 0 rblk in the MIR, fill=32
```

Both print the right answer on x86-64 — the old shape coincides with the ABI
there — which is exactly why this hid. On arm64 the bound shape is garbage.

**Hypothesis (WRONG — superseded by the next section): a forest-RESTORED class
loses its non-triviality.** `class_return_via_retbuf` answers from
`class_needs_dtor(cdd)`; if the restored `std::locale` reported no dtor,
`function_retbuf_class` would return NULL and madc would emit getloc as an
ordinary scalar-returning call. Same family as the husk layer: what a restored
class CARRIES.

**This is the remaining darwin blocker and it now has a cheap discriminator** —
`grep -c rblk` between a live and a bound compile of the same probe, on Linux,
no Mac required.

#### The third defect, actually diagnosed (session #78): four emitters, one marker

The discriminator reproduces in one command: same binary, same source, one flag
apart.

```
LIVE  -stdlib=libc++              -> call ... getloc, rblk:8(U_1), U_3
BOUND -stdlib=libc++ --forest-bind -> call ... getloc, U_1, U_3
```

The hypothesis above was wrong, and one look at the emitted C says so — the
bound lane's declaration is *already* the right shape:

```c
/* live  */ extern void ...getlocEv(struct locale  *, void *);
/* bound */ extern void ...getlocEv(struct locale *__retbuf, struct ios_base *p0);
```

Both carry the hidden pointer, so nothing lost its non-triviality:
`has_user_dtor` round-trips through the arena (`madc_cir.cpp` sets
`DF_HAS_USER_DTOR`, `cir_freeze.cpp` restores it), and `class_needs_dtor`
answers the same either way. What differs is **which emitter wrote the
declaration** — abstract params with `void *` is `need_output_extern`; named
params (`__retbuf`, `p0`) is Pass 0.75's referenced-FuncDef typed extern.

madc has **four** emitters of the hidden result-address parameter:

| emitter | serves | carried the marker |
|---|---|---|
| `need_output_extern` | call-site externs | yes |
| Pass 0.75 typed extern (`translate_module`) | referenced FuncDefs | **no** |
| `func_proto` | forward protos | **no** |
| `func_def` | madc's own definitions | **no** |

Pass 0.75 inserts into `typed_proto_syms`, and the `m_output_externs` flush
*skips* those symbols — so whichever emitter runs first wins, exclusively. A
grove supplying a declaration for `ios_base::getloc` is enough to move it from
the first row to the second, which drops the marker. On x86-64 both shapes run
correctly, so the suite stayed green and the answer stayed right; only AArch64
can tell them apart.

**It was never confined to the bound lane.** A mechanism-level audit of the MIR
— *every call to an imported symbol whose proto carries a `__retbuf` arg must
carry it as `rblk`* — found `basic_string::substr` and
`ostringstream::str` unmarked in the **live, default (libstdc++)** lane as well.
The Mac symptom pointed at bind only because `getloc` is what `cout << "hi"`
happens to reach.

**Fix.** `retbuf_param()` is the one owner of that parameter, so it marks
unconditionally — being the result address is what the parameter IS, not a
property of the caller — and `need_output_extern` now builds its result-address
param through the same owner instead of by hand. Unconditional is also the only
timing-safe answer: whether the module will ALSO define the symbol is not known
when a declaration is emitted (a lazily-materialized body may or may not
arrive), and a first attempt that gated the marker on `madc_emits_definition()`
produced exactly the live-vs-bound split it was meant to prevent. Marking
everywhere additionally matches the platform ABI for a weak definition the
linker may preempt.

Routing through the one owner also gives the slot its `__retbuf` NAME in every
prototype, which is what makes it identifiable in the IR — that is what the gate
reads.

**Gate.** `scripts/sret_abi_gate.sh` now asserts the MECHANISM, in both lanes,
keyed on no particular class, symbol or header: zero imported `__retbuf` args
may be plain. Negative controls: a trivially-copyable class return
(`vector::begin` → `__normal_iterator`) must STILL come back in a register; a
minimum marked count, so the gate cannot pass vacuously if the reducer stops
compiling; and — verified once by hand, not shipped — suppressing the marker at
its owner makes the gate name all three offenders and exit 1.

### W2 fourth Mac run (session #78) — the blocker is gone, and the battery could not see it

Rebuilt both arches at `43fe572a` (`release-macos`: tarballs pulled) and ran the
battery on the owner's arm64 Mac. **The tally is unchanged — 3 passed, 3 failed,
the same three legs as session #77.** That is not a null result; it is the
finding.

The five probes run directly, which the battery never reaches:

| probe | session #77 | now |
|---|---|---|
| `cout.fill()` | SIGSEGV @0x9560a2 | `fill=32` |
| `cout.widen('x')` | SIGSEGV | `widen=x` |
| **`cout << "hi"`** | **SIGSEGV @0x0** | **`hi`** |
| `use_facet<ctype<char>>(cout.getloc())` | fixed in #77 | `up=A` |
| `std::locale l = cout.getloc()` | — | `loc ok` (+ the known warning) |

**So the C++-on-darwin arc closes here**, and the remaining battery failures are
all pre-existing layers with their own slices: the husk (`os.str()`, off behind
`FEATURE_FOREST_ALIAS_SHELL_COMPLETE`), the `value` intrinsic SIGSEGV, and the
AOT leg below.

**Two defects in the EVIDENCE RUN itself, which is why the tally moved zero:**

1. **The battery has no leg for `std::cout << "hi"`.** The single symptom that
   drove three sessions of work was never covered by the run whose job is to be
   the release evidence. Its C++ leg (`groves`) fails earlier, on the husk's
   `os.str()`, so everything downstream of that line in the same probe is dark —
   including every stream operation. A leg that a known-open defect can mask is
   not evidence for anything past the mask. The `cout << "hi"` case needs its own
   leg, independent of the groves probe.

2. **The AOT leg's known-unsupported arm is on the wrong branch.** Session #77
   recorded the right intent — "compile must succeed, the run may decline
   loudly" — but the code does the opposite: the `elif` that accepts a loud
   decline only fires when the COMPILE fails. On darwin the compile SUCCEEDS and
   writes the object; the RUN declines ("in-process loading of Mach-O objects is
   not supported yet"). So the leg takes the strict branch and reports FAIL for
   precisely the behaviour it was rewritten to accept. The note and the code
   disagree, and the code is wrong. Correct shape: compile must succeed; then
   the run either produces `aot 42` (the real round-trip, the day the Mach-O
   loader lands) or declines LOUDLY (nonzero + a diagnostic) — a silent nonzero,
   or exit 0 with wrong output, stays a failure.

Full validation at `43fe572a`: fulltest 1019/0, libc++ JIT 1015/0, unit
9019/9019, EXE 990/0, OBJ 990/0, release + packed 1019/0, trailers GREEN.

## Session #80 (2026-08-11) — Mac validation over ssh: battery confirmed, emitted C EXECUTED on arm64, emit-lane defect found+fixed

Driven end-to-end from the container/NAS over ssh to the owner's arm64 Mac
(15.3.2, Apple clang 17); staging dir `~/madc-s79`, binary = the canonical
`dist/` 0.75.0 arm64 tarball (SHA-verified against SHA256SUMS).

### Battery (script @1569e75f on the 0.75.0 binary): 5 passed / 2 failed

Exactly the two KNOWN-open darwin defects and nothing else:

- **groves** — `os.str()` → "Unidentified member 'str'": the husk/alias-shell
  layer (`FEATURE_FOREST_ALIAS_SHELL_COMPLETE`, guarded off — the last C++
  forest layer, already queued).
- **value intrinsic** — the known darwin SIGSEGV (queued, fix direction
  settled).

Both #79 script fixes CONFIRMED on hardware: the `cout << "hi"` leg (3b)
**passes on silicon** — the darwin sret probe is green end-to-end — and the
AOT leg now reads the loud `.o`-load decline ("in-process loading of Mach-O
objects is not supported yet") as known-unsupported instead of #78's false
FAIL. Correction to #79's close-out: "expect 8/0" was wrong arithmetic — the
battery has 7 checks; the honest ceiling with the two known-opens is 5/2, and
a fully-fixed darwin reads 7/0.

### Emitted-C execution proof (the Still-owed item) — CLOSED

Specimens emitted on the container at HEAD with `-stdlib=libc++ --emit=c11`,
compiled ON the Mac with Apple cc, run against Apple `clang++ -O0` oracles:

| specimen | exercises | Mac result |
|----------|-----------|------------|
| locale/getloc (8-byte class) | THE extern-rewrite case (register-size in C, indirect in C++) + cleanup-shim dtor on a live libc++ refcounted object | **runs, byte-MATCH** (`loc ok`); on-target `cc -S` shows the x8 setup adjacent to the rewritten call |
| string-only (twice/+/substr/assign) | in-file `__retbuf` convention, self-referential small-strings | **runs, byte-MATCH** (`cdab bc zz`) |
| full sstream specimen | + ostringstream/use_facet | compiles clean with Apple cc; **cannot RUN on darwin** — see W3 below. Runs + byte-matches on Linux with `-lmadc` (`cdab bc x zz`) |

### Found+fixed en route @7e484749: the emit lane never opened the flavor runtime

The sstream specimen's emitted C was gcc-REJECTED: `_ZNSt3__15ctypeIcE2idE`
undeclared. `MADC_XTEST_FACET` showed the alias-bound facet-id recording
(class_struct_def) declining with `avail=0`: in a pure `--emit` run nothing
had dlopen'd the flavor runtime, so `external_symbol_available`'s dlsym
answered false for every LEGITIMATE alias. Root cause: `madc_cir_emit` was
the one lane still calling `translate_module()` bare — outside
`cir_translate_guarded`, whose open-before-build exists for exactly this
(same family that bit the object lane, then the freeze lane). Fix: the emit
lane flows through the guarded translate (also deletes the parallel bare
translate; gains the exception guard + CIR timing). Gate: `emitc_sret_gate`
leg 2's reducer now inserts through cout (pulls `use_facet` + the facet-id
static) and links `-lmadc`; negative control at the unfixed binary failed
exactly on the undeclared symbol, all legs green with the fix.

### W3 evidence — the darwin runtime gap, now concrete

The sstream specimen aborts on the Mac (`dyld: missing symbol called`,
linked with `-Wl,-undefined,dynamic_lookup`): unlike the never-taken throw
path in the string specimen, its try/catch ENTERS the madc exception runtime
at scope entry. `nm -u` names the complete required surface — 8 symbols:
`__madc_try_push/pop`, `__madc_cleanup_top/push_dtor/discard_to`,
`__madc_exception_type/clear`, `__madc_throw_object`. W3 can be satisfied
for emitted-C consumers by a small runtime-subset archive (libmadc_rt) —
a full `libmadc.dylib` is only needed for the AOT/`-o` story.

### Battery leg 6b — the execution proof becomes standing evidence

`mac_battery.sh` gains leg **6b (emit-C sret on-target)**: `--emit=c11` on a
locale reducer, compiled by the HOST `cc` against libc++, executed, output
gated. An `info`-skip when the Mac has no `cc`; a binary that emits the old
first-argument shape FAILS with the reason (the old shape is silently
x86-64-only — exactly what release evidence must not pass). Verified on the
0.75.0 tarball: the leg correctly reports "predates the indirect-return
pass" (its own negative control). Battery on that binary now reads 5/3, all
three fails explained; a fresh `release-macos` build at ≥7e484749 reads 6/2
(husk layer + value intrinsic still open); a fully-fixed darwin reads 8/0.

### Validation

Negative control (gate vs unfixed binary): leg 2 red on the exact defect,
other legs green. With the fix: gate green (all 4 legs). Suite results at
7e484749: fulltest + libc++ JIT — recorded in claude_status.json (run in
this session; see live_handoff).

## Session #81 (2026-08-11) — W0.5 as implemented: provenance-clean prelude

Owner kickoff: "time to focus on the W0.5 lane... we likely need a bit of
extra recon here, but time to begin." The recon collapsed the estimate:
the candidate tree is a drop-in, and the whole item landed in one session
(commit `d7ada73e`).

### The open tree — recon findings

**Source: the Zig project's curated darwin libc headers**, pinned at the
zig 0.16.0 source release (2026-04-13), `lib/libc/include/any-darwin-any`
— a single arch-NEUTRAL tree (7.6MB, 553 files; Zig consolidated the old
per-arch macos dirs), macOS 26.4-SDK era, actively maintained (SDK 26.0
refresh PR #25257 predates the release). Zig's public position since its
0.8.0 release notes: these headers are APSL — "there is a myth that
Apple's C header files have an encumbered license, but that is not the
case." The standalone fetch-them-macos-headers repo is deprecated/archived
(Nov 2025); the tool now lives in the zig repo
(`tools/fetch_them_macos_headers.zig`), so the zig source release IS the
canonical distribution of the curation. Bonus discovered: zig also ships a
hand-written `lib/libc/darwin/libSystem.tbd` linker stub — a future option
for freeing cross-LINKING from the owner's SDK too (not needed now; SDK
linking is settled as its licensed purpose).

**Coverage**: all 36 prelude header names present. Both triples
(`{arm64,x86_64}-apple-macos12`) flatten cleanly through the unmodified
generator; the printf gate passes.

**THE FIRST DIFF (the sizing milestone): zero declaration gaps.**
Normalized set-diff of open-tree vs SDK-oracle preludes, same triple, same
posture filter: 50 SDK-only lines per arch — the two arches' drift sets
are IDENTICAL — and every one is Apple's own 15.5-vs-26.4 evolution with
the counterpart present on the open side in newer spelling:

| SDK-only category | open-side counterpart |
|---|---|
| fortify `#define memcpy(dest,...) __builtin___memcpy_chk(...)` | same builtins behind a `__memcpy_chk_func` indirection (both defines in the flatten; chain bottoms out at the SAME builtin the SDK prelude used) |
| `malloc_type_*` decls (macOS 14 typed malloc) | present, updated availability text |
| `ELAST 106` | `ELAST 107` (+`ENOTCAPABLE`) |
| DIR members `long __dd_loc/__dd_size` | `size_t` (Apple's change; DIR is opaque to user code) |
| `__MAC_OS_X_VERSION_MAX_ALLOWED __MAC_15_5`, availability machinery | 26.4 series |

**License audit (mechanical, of the exact closure)**: `clang -E -MD` over
the umbrella lists precisely the files flattened. Union over both arches =
240 files: **213 APSL** (`@APPLE_LICENSE_HEADER_START@` /
`@APPLE_OSREFERENCE_LICENSE_HEADER_START@`), **20+1 BSD** (UCB, Citrus
Project, NetBSD Foundation, Alexey Zelkin, Todd C. Miller — holders
reproduced in the NOTICE), **5 bare Apple-copyright shims**
(`arm/_{limits,param,types}.h`, `arm/signal.h`, `stdint.h` — mechanical
typedef/dispatch files whose canonical upstreams are in Apple's
open-source releases), **1 public domain** (`machine/limits.h`, says so
itself). Re-audit is part of a pin bump, not a build (the audit is a fact
about the pinned bytes).

### What shipped (d7ada73e)

- **`scripts/fetch_darwin_open_headers.sh`** — the pin's ONE owner
  (version + SHA256 from ziglang.org's published index). Downloads,
  verifies loudly, extracts, stages
  `/workspace/darwin-open-headers/sysroot-any-darwin-any/usr/include`
  (symlink into the tree), writes the `.PROVENANCE` stamp. Idempotent;
  `provision_container.sh` runs it (disposable-container law) and
  reports it beside the SDK check.
- **Makefile `DARWIN_PRELUDE_SYSROOT`** (default = the open sysroot)
  feeds the one generator call; `MACOS_SDK` stays for compiling/linking
  madc itself. A failed prelude pipeline now REMOVES
  `embedded_headers.cpp` — a stale table from an earlier gen can no
  longer be silently reused (the mtime-poison class).
- **`gen_darwin_prelude.sh`** takes a `<prelude-sysroot>`; bakes the
  sysroot's stamp into the umbrella as
  `/* MADC-DARWIN-PRELUDE-PROVENANCE: ... */` — embedded text rides
  `.rodata`, so THE BINARY NAMES ITS OWN PRELUDE INPUT — and stamps
  `sdk-private` for a stamp-less input (the SDK survives only as the
  never-shipped oracle for future diffs).
- **`verify_macho_release.sh` authority 4**: greps the marker from the
  built binary; positive match on the open stamp shape; refuses
  sdk-private or missing. The W0 resolution's "the hosted release target
  accepts only the open-provenance prelude" is now mechanical.
- **Notices**: `docs/licenses/APSL-2.0.txt` (SPDX canonical) +
  `docs/licenses/NOTICE-darwin-prelude.txt` (the audit above, BSD
  copyrights reproduced, source pointers to opensource.apple.com /
  apple-oss-distributions / ziglang.org). `package_release_macos.sh`
  ships both under `THIRD_PARTY_NOTICES/` and DROPS its owner-private
  banner — the gate enforces what the banner could only warn about.

### Negative controls

- fetch: idempotent second run; SHA mismatch refuses (verify step is
  `sha256sum -c` before any staging).
- generator: missing sysroot → loud refusal naming the fetch script;
  SDK input → `sdk-private` marker (oracle path preserved).
- verify gate: run against the pre-W0.5 session-#74 release binary +
  its own extracted forest — authorities 1–3 pass, authority 4 refuses
  with "no prelude provenance marker". Real bytes, exact failure.

### Hardware validation

`release-macos` rebuilt both arches through the open sysroot: 835 forest
units (identical to the SDK-prelude freeze), verify gates green with the
zig stamp read back out of both binaries, tarballs packaged with the
notices. Mac battery on the fresh arm64 tarball (`~/madc-s81`): **6/2**,
exactly as predicted — the two fails are the pre-existing known-opens
(groves `os.str()` husk, value-intrinsic SIGSEGV); the C-prelude leg
JIT-compiles real programs against the open-flattened text on hardware,
and leg 6b (emit-C sret on-target) now PASSES on a ≥7e484749 build.
Latency info-leg: 231 ms (in family). Six-months-newer header text
(15.5→26.4), zero behavioral change.

### Consequence

With W0.5 done, the provenance chain of a darwin tarball is fully open:
C prelude = Apple APSL/BSD via the pinned Zig curation (marker-gated),
C++ groves = LLVM libc++-18 (Apache-2.0), notices shipped in-artifact.
**Nothing gates PUBLIC upload of the macOS tarballs anymore** — merge ⇒
release can publish them alongside the .deb/.rpm.

## W3 as implemented (session #81, same day)

Owner: "W3 sounds like it should be small... maybe we should get that
done before the release so it can be part of the package."

Session #80's evidence made W3 small: the only runtime surface darwin
emitted-C consumers reference is the exception/cleanup vocabulary plus
`__madc_vla_free`. The recon that collapsed it further: those symbols
already live in **`src/rt/`** — the dual-build C-lane runtime
(forest-carriers S5), deliberately strict C11 with no C++-runtime
dependency, membership owned by `scripts/ledger_sources.txt`. The archive
is therefore a REPACKAGING, not new code:

- **`lib/libmadc_rt.a`** = the same ledger objects libmadc holds,
  re-archived alone (`src/Makefile`; per-mode
  `libmadc_rt-hosted-<arch>-macos.a` builds beside each hosted darwin
  binary, and the Linux twin builds in the default targets so the
  container gates it per-commit).
- **Completeness audit, mechanical**: every
  `need_output_extern("__madc_*")` site in cir_builder.cpp = 15 symbols;
  13 are the ledger pair's exports, and the remaining two are
  `<ns_madc>`'s `__madc_sys_init/_once` — which call
  `madc::sys_populate_args` (full C++ runtime) and are excluded by the
  same membership rule that keeps the ledger sources freestanding. The
  README names that limitation: madc-dialect (`<ns_madc>`) emitted
  programs need the full runtime, which the tarball does not ship.
- **Gate (container, in fulltest)**: `emitc_sret_gate.sh` leg 2b links
  leg 2's emitted C against the subset archive ALONE and runs it; its
  own negative control links with NO runtime and must fail on a
  `__madc_` symbol. Green at 04b78626 (and the negctl verified the
  specimen really references the runtime).
- **Battery leg 6c (on-target)**: emit a cout-inserting program (enters
  try/catch via `__put_character_sequence`), link against the tarball's
  `lib/libmadc_rt.a` with Apple `cc`, run, gate "rt ok". A tarball
  without the archive FAILS with the predates-reason. This turns #80's
  dyld-abort evidence into the standing green.
- **Packaging**: `package_release_macos.sh` ships `lib/libmadc_rt.a` +
  the README "Emitted C" section with the exact link line.

### Darwin AOT `-o` probed on-target (same session — the owner's question)

Four probes on the Mac against the W3 tarball settle what the original
W3 item left open ("verify what darwin AOT output references"):

1. **Runtime-free C program, `-o`: emits and RUNS today** — the axis-B
   Mach-O executable writer + emit-time ad-hoc signature, exercised
   end-to-end on the Mac itself for the first time.
2. **Runtime-needing program, bare `-o`**: loud refusal that names the
   fix ("build it into the image with -static-libmadc (C-lane machinery
   only)").
3. **C++ program, `-o -static-libmadc`**: emit succeeds (static verify
   passes — the darwin forest DOES carry the ledger modules), but dyld
   aborts: `__ZNSt3__14coutE ... Expected in: /usr/lib/libSystem.B.dylib`.
   The executable writer two-level-binds EVERY import to libSystem; C++
   symbols live in libc++.1.dylib. The ONE missing slice for C++
   programs: LC_LOAD_DYLIB for the flavor's link libs + per-import
   binding that resolves there (fork `mir-macho.c` + exe-lane plumbing
   to pass the flavor link libs).
4. **C-lane try/throw/catch, `-o -static-libmadc`: `caught 42` — works
   today.** The ledger-carried rt_except rides inside the image;
   imports are libc-only, which the libSystem binding serves.

So darwin AOT `-o` stands: DONE for runtime-free C and for
C-lane-runtime programs via `-static-libmadc`; one writer slice (libc++
dylib binding) short for C++ programs; the full-runtime dynamic case
stays with the deferred libmadc.dylib. The battery AOT leg's
"known-unsupported" remains about the object LOADER (`-c` + run the
`.o`), a separate axis. Candidate follow-up (owner to call): a battery
leg covering probes 1+4 so this becomes standing evidence.
