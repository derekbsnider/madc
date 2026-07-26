# The darwin C++ lane — Route 1 Phase 2 track plan (2026-07-26, owner-prioritized)

Owner directive, 2026-07-26: after the `.o`-link-lane slice (task #23,
v0.53.0), **do P2** — ahead of the remaining Mach-O polish (#24 the Apple
in-process `.o` loader, #26 the value ABI as Tier-A C11, #22 the
compression vocabulary move). The reason is the one that matters: every
darwin lane madc has today is **C only**, so a Mac user writing C++ hits
the wall immediately, while #24/#26/#22 are completeness items behind
that wall.

This doc is the "own plan doc when the phase starts" that
[`2026-07-25-madc-on-macos-plan.md`](2026-07-25-madc-on-macos-plan.md)
§"Phase 2" promised.

## Goal

`std::string`, `std::vector`, `iostream` and the rest of the script-lane
C++ surface work on a Mach-O target — cross-emitted from Linux and
hosted on the Mac — resolved **mangled-direct against the real libc++**,
exactly as the Linux lane resolves against the real libstdc++. No
wrapper shims, no madc-authored twins: same architecture, second ABI
flavor ([[project_cpp_mangled_direct]], `string-as-class`).

## Measured starting state (traced 2026-07-26; empirical legs pending)

**The include search list is generated per build MODE, not hardcoded.**
`scripts/gen_sys_includes.sh` captures `$(CXX) -x c++ -E -v`'s search
list into `src/sys_include_paths.cpp` (gitignored, regenerated at
Makefile *parse* time so a MODE switch cannot leave the previous
toolchain's table behind). Consumers: the lexer's `<...>` resolution, the
system-header classifier that gates library inline-body emission, and
parser.cpp's search.

That yields two very different situations:

- **Hosted-darwin MODEs are already right by construction.** They set
  `CXX = clang++-18 -target <triple> --sysroot $(MACOS_SDK)`, so the
  captured table is the **SDK's** list, and
  `MADC_SYS_INCLUDE_PREFIX_MAP` rewrites the staged SDK prefix to
  `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk` for the run
  machine. If clang reports `usr/include/c++/v1`, the hosted binaries
  already carry it. *To verify the moment the container is back — this
  is a mechanism-level expectation, not an observation.*
- **Cross-macos MODEs are wrong.** `cross-arm64-macos` /
  `cross-x86-64-macos` change only `MIRLIB` and `DEFINES`; `$(CXX)`
  stays the Linux `g++`, so the cross madc bakes
  `/usr/include/c++/13/…` — Linux libstdc++ paths — while emitting
  Mach-O. It embeds a darwin **C** prelude and nothing C++.

**Mangling is centralized but single-flavor.** 21 `__cxx11` references
across 7 files (`madc_mangle.cpp` is the authority; `cir_builder.cpp`,
`madc_cir.cpp`, `parser.cpp`, `include/madc.h`, `include/madc_mangle.h`,
`include/cir_arena.h` carry the rest).

**No hardcoded std-type size table exists** (`grep STDSTRING_SIZE` in
`src/` is empty — the C11-transpiler rule's `STDSTRING_SIZE` is a
computed `sizeof()` macro in emitted C, not a baked constant). Layouts
therefore come from the real parsed headers, which is the whole point of
the string-as-a-real-class model: parse libc++'s `basic_string` and its
24-byte SSO layout follows without a table to update.

## The four pieces

### P2.1 — target-derived header search for the cross lane

The cross madc must search the **target's** headers. Mechanism, in
gcc/clang's own shape:

1. **Baked target table.** For `cross-*-macos` MODEs, invoke the
   generator with the darwin clang the hosted modes already use
   (`clang++-18 -target <triple> --sysroot $(MACOS_SDK)`) and **no**
   prefix map — a cross build reads those headers on the Linux box at
   compile time, so the staged SDK's real paths are the correct ones.
   One Makefile change, mirroring the hosted block.
2. **`-isysroot <dir>` CLI override** (clang's spelling; gcc accepts it
   too): re-roots the baked list at run time, so one cross binary works
   against any staged SDK. Also the answer for a hosted madc whose Mac
   has the SDK somewhere non-standard, and for `xcrun --show-sdk-path`
   integration later.
3. The generated table stays the ONE authority — no second list, no
   `#ifdef __APPLE__` path literals anywhere (Rule #7).

### P2.2 — libc++ parse burn-down

Same method as the libstdc++ burn-down: compile a real
`#include <string>` / `<vector>` / `<iostream>` against the SDK's
libc++, reduce each failure to a minimal `.cpp`, fix at the **deepest
layer**, never shim. Both canon compilers apply — clang is the natural
oracle here (it owns libc++), and per the two-canon rule clang also acts
as the scope filter: what clang rejects, madc need not support.

Deliverable alongside the fixes: a burn-down count in this doc, updated
per slice, the way the tsubst burn-down tracked `[why:]` classes.

*Measurement pending the container's SDK.* Expect the long pole here:
libc++ leans on `_LIBCPP_*` attribute macros, `__attribute__((…))`
spellings, and its own `__config` feature detection.

### P2.3 — the STD-ABI flavor switch (`std::__1`)

libc++ puts everything in the inline namespace `std::__1`, so every
mangled-direct symbol differs from libstdc++'s (`_ZNSt3__1…` vs
`_ZNSt7__cxx11…`). Give `madc_mangle` an **ABI-flavor** enum selected by
the target (never by host `#ifdef`), covering:

- the std inline-namespace component in mangled names,
- the `cout` / `cerr` / `cin` global symbol names,
- any place the 21 `__cxx11` sites hardcode the GNU spelling.

Layouts and sizes are NOT part of the switch: they come from the parsed
libc++ headers. If a size turns out to be baked somewhere, that is a bug
to fix at its source, not a second table to maintain.

### P2.4 — freeze the libc++ groves into the darwin packs

A Mac without Command Line Tools has **no SDK headers at all**, and
embedding a libc++ subset is not the answer (size, licence, drift — and
it would be a madc-authored twin, which the shim-retirement track
deleted for good reasons). The architecture already answers this:
**LOADED == parsed**. The darwin packs are cross-frozen on Linux (S1),
so freezing the libc++ corpus into them means a packed `madc` compiles
C++ on a header-less Mac with no live parse at all. The C prelude
already proves the shape.

Consequence to state plainly in the docs: an **unpacked** madc on a
no-CLT Mac cannot compile C++ and must say so loudly (name CLT/Xcode or
`-isysroot`), never degrade silently.

## Decided defaults (no owner question needed)

- Header search is **target-derived and generated**, never a hardcoded
  Apple path list.
- `-isysroot` is the override spelling (clang/gcc parity). No madc-only
  invented flag.
- ABI flavor is selected by **target**, not by build host — a cross
  madc on Linux emitting Mach-O uses the libc++ flavor.
- No embedded libc++ subset. Header-less Macs are served by the frozen
  forest; unpacked + no SDK = loud refusal.
- Sizes/layouts come from the parse. Any baked size found is a defect.

## Gates

- **Cross-emit + hardware run**, the same shape every darwin slice used:
  a C++ program cross-emitted on Linux for both arches, run on the
  owner's Mac; output identical to the same program on Linux.
- **`--emit=c11` / g++-oracle parity** where applicable (the emitted-C
  oracle rule): the gcc-compiled emission must match the g++-compiled
  original.
- A **packed** darwin binary compiling C++ with `--no-forest-bind`
  disabled — i.e. proving the frozen libc++ groves are what served the
  compile (P2.4), the way S1 proved grove bind on the Mac.
- Linux fulltest/`--exe`/`--obj`/packed arbiter unchanged throughout:
  this track must not move the Linux numbers.

## Sequencing

1. **P2.1** (cross lane sees the SDK) — prerequisite for measuring
   anything.
2. **P2.2** burn-down slice 1: `<string>` parses.
3. **P2.3** ABI flavor: `std::string` round-trips mangled-direct against
   libc++ (this is the first end-to-end proof).
4. **P2.2** slices 2+: containers, then `<iostream>`.
5. **P2.4** freeze the groves into the darwin packs; header-less-Mac
   gate.

Each step is independently shippable and gated; none of them may move
the Linux baseline.

## Blocked / pending

- **Container `/workspace` was lost on a WSL crash (2026-07-26) and is
  being reattached by the owner.** Everything empirical above waits on
  the SDK coming back: the hosted table's `c++/v1` verification, the
  burn-down count, and every gate leg. Nothing in the design depends on
  it.
- Every darwin RUN leg needs the owner's hardware, as in every previous
  darwin slice.
