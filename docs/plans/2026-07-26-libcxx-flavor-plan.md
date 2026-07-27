# The libc++ standard-library flavor — track plan (2026-07-26, owner-directed)

Owner directive, 2026-07-26: after the `.o`-link-lane slice (task #23,
v0.53.0), **do P2** — ahead of the remaining Mach-O polish (#24 the Apple
in-process `.o` loader, #26 the value ABI as Tier-A C11, #22 the
compression vocabulary move). The reason is the one that matters: every
darwin lane madc has today is **C only**, so a Mac user writing C++ hits
the wall immediately, while #24/#26/#22 are completeness items behind
that wall.

**Owner correction, same day — and it reshapes the track: libc++ is a
STANDARD LIBRARY, not a platform.** It is the default on Apple platforms,
the Android NDK's STL, FreeBSD's system C++ library, and available
anywhere clang is via `-stdlib=libc++`. So this is not "the darwin C++
lane": it is madc's **second standard-library flavor**, and darwin is
merely its first consumer. Anything that keys C++-library behaviour off
"is the target Apple" is a hardcoded platform test in machinery that owes
its behaviour to a library choice — precisely what
`design-principles.md` Rule #7 and the vision invariants forbid.

This doc is the "own plan doc when the phase starts" that
[`2026-07-25-madc-on-macos-plan.md`](2026-07-25-madc-on-macos-plan.md)
§"Phase 2" promised, generalized per that correction.

## Goal

`std::string`, `std::vector`, `iostream` and the rest of the script-lane
C++ surface work against **libc++**, resolved mangled-direct exactly as
the current lane resolves against the real libstdc++. No wrapper shims,
no madc-authored twins: same architecture, second ABI flavor
([[project_cpp_mangled_direct]], `string-as-class`).

Two orthogonal dimensions, never conflated:

| Dimension | Values | Chosen by |
|---|---|---|
| target / object format | ELF · Mach-O; linux · darwin · freebsd · android | the target triple / MODE |
| **C++ standard library** | libstdc++ · **libc++** | `-stdlib=`, defaulting per target |

A Mach-O target *selects* libc++ by default; it must never *mean*
libc++. Conversely `madc -stdlib=libc++` on Linux is a first-class,
supported combination — and, as §"Gates" explains, it is how almost all
of this track gets developed and gated.

## The user-facing knob

`-stdlib=libc++` / `-stdlib=libstdc++`, clang's own spelling (no invented
flag), defaulting to the target's usual library: libc++ for
darwin/Apple, libstdc++ for GNU/Linux, and the same question answered
per-target as freebsd/android arrive. It composes with `--std=` the way
clang's does.

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
  machine. **Confirmed 2026-07-26** — that compiler reports:

  ```
   <SDK>/usr/include/c++/v1                        ← libc++, FIRST
   /usr/lib/llvm-18/lib/clang/18/include           ← compiler-owned freestanding
   <SDK>/usr/include                               ← SDK C headers
   <SDK>/System/Library/Frameworks (framework directory)
  ```

  Two notes that follow from it. (a) The generator's line parser keeps
  the first token, so the Frameworks entry lands in the table as a plain
  include dir — harmless (a `<...>` probe there simply misses), and
  framework-style includes are out of scope for now. (b) The
  compiler-owned entry is a **build-host** path. For a CROSS build that
  is correct — those headers are read on the Linux box at compile time.
  For a HOSTED binary it dangles on the Mac, which is by design: madc
  supplies its own freestanding equivalents (the embedded headers /
  darwin prelude), and `gen_sys_includes.sh` records that dir separately
  as `madc_compiler_owned_include_dir` precisely so it can be treated as
  the freestanding bucket rather than a system-library one. The
  burn-down must confirm libc++'s `<stddef.h>`/`<stdarg.h>` pulls land on
  madc's equivalents on a hosted run.
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

### P2.1 — target-derived host tables for the cross lane

**There are TWO generated tables with the same host-vs-target split, and
both matter for libc++** (confirmed 2026-07-26):

| Table | Generator | Cross-macos MODE today |
|---|---|---|
| `#include <...>` search list | `gen_sys_includes.sh` (`$(CXX) -x c++ -E -v`) | Linux `/usr/include/c++/13/…` |
| predefined macros | `gen_predefined_macros.sh` (`$(CXX) -dM -E`) | `__linux__`, `__x86_64__`, **no `__APPLE__`/`__MACH__`** |

The macro table is not a detail: libc++'s `__config` branches on
`__APPLE__`, `__MACH__` and the object-format macros, so a cross madc
that predefines `__linux__` cannot configure libc++ correctly no matter
how good the header search is. The darwin **C** lane gets away with it
today only because the embedded darwin prelude is pre-resolved text
(generated by `gen_darwin_prelude.sh` against the darwin clang) rather
than `__APPLE__`-conditional system headers — a trick that does not
scale to real libc++.

So P2.1 is one symmetric change: for Apple-target MODEs, drive **both**
generators with the darwin clang rather than the build compiler. The
hosted MODEs already do this by construction (their `$(CXX)` *is* the
darwin clang); only the cross MODEs need the knob.

The cross madc must also search the **target's** headers. Mechanism, in
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

### P2.2 — libc++ parse burn-down (developed ON LINUX)

**FIRST MEASUREMENT (2026-07-26, cross madc against the SDK's libc++ 18).**
madc's first `#include <string>` stops at one line:

```
__type_traits/remove_reference.h:37: #error "remove_reference not implemented!"
```

which libc++ guards with `#if __has_builtin(__remove_reference_t)`. **madc
implements no `__has_builtin` at all**, so every such probe answers "no"
and libc++ `#error`s rather than falling back — modern libc++ implements
`<type_traits>` by delegating to compiler intrinsics, and simply refuses
to be compiled without them.

The denominator, measured over the SDK's libc++ 18 headers:

| `__has_builtin(...)` names referenced | count |
|---|---:|
| total distinct | **59** |
| compiler **type-trait intrinsics** (`__remove_reference_t`, `__add_lvalue_reference`, `__decay`, `__is_destructible`, `__array_rank`, …) | **41** |
| ordinary `__builtin_*` functions (most already in madc's table) | 18 |

**Second measurement, and it shortens the critical path a lot: only TWO
of those 41 are mandatory.** libc++'s `__type_traits/` contains exactly
two hard `#error`s —

```
#  error "remove_reference not implemented!"
#  error is_trivially_destructible is not implemented
```

— and every other trait has a C++ template fallback behind its
`__has_builtin` guard. So madc does not need 41 intrinsics to compile
libc++; it needs `__has_builtin` plus **two** traits, after which libc++
configures on its fallbacks. The other 39 become a
compile-speed/robustness follow-on, not a blocker.

The `__has_*` surface libc++ leans on (uses in the SDK's libc++ 18),
none of which madc implements today — all silently evaluate to 0:

| operator | uses |
|---|---:|
| `__has_builtin` | 122 |
| `__has_attribute` | 29 |
| `__has_include` | 18 |
| `__has_feature` | 10 |
| `__has_cpp_attribute` | 8 |
| `__has_keyword` · `__has_extension` · `__has_declspec_attribute` | 11 |

Slice order follows from that:

1. **The `__has_*` family as real preprocessor operators**, each answered
   from madc's own state and each answering **truthfully**:
   `__has_include` from the existing include-resolution path,
   `__has_builtin` from the builtin registry, the attribute forms from
   the attribute handler's known set, `__has_feature`/`__has_extension`/
   `__has_keyword` from the `LanguageStd`/keyword registry. Claiming
   support madc lacks would trade libc++'s clean `#error` for a
   mystifying failure deeper in the header — the opposite of the
   loud-failure contract. They slot in beside `defined` in
   `Program::evaluateIfCondition`'s `parse_primary`.
2. **The two mandatory intrinsics**, `__remove_reference_t` and
   `__is_trivially_destructible`. Registry-driven, so each is a registry
   entry plus its sema, never a call-site special case (Rule #7).
3. Then the burn-down proper — and expect it to land in **template
   metaprogramming**, not intrinsics: with the fallbacks in play, libc++
   compiles `<type_traits>` through exactly the dependent-name /
   partial-specialization machinery the tsubst spine owns. That is the
   real work, and it is the same muscle the libstdc++ burn-down built.
4. The remaining 39 intrinsics, as a speed/robustness pass: each one
   swaps a fallback template instantiation for a direct answer.

**Slice 1 landed (2026-07-26): the `__has_*` operators are real.**
`__has_builtin` answers from madc's builtin knowledge (the 138-entry
alias table that IS its implementation for that family — an alias
rewrites the call to the libc function, so membership is exactly "madc
compiles this"), `__has_include` / `__has_include_next` answer through
the SAME resolver `#include` uses, so "can I include this?" and "will
including it work?" cannot disagree. The rest of the family
(`__has_attribute`, `__has_cpp_attribute`, `__has_declspec_attribute`,
`__has_feature`, `__has_extension`, `__has_keyword`) parse and answer 0
deliberately, which is what they already answered by accident — madc
does support a real subset of each, but there is no registry to ask yet,
and an unbacked yes is the failure mode this whole design refuses.

One layer down, the fix that made it correct: `#if` operands are
macro-expanded before evaluation, and madc **aliases 138 builtins in
`define_map`** — so `__has_builtin(__builtin_memcpy)` was arriving as
`__has_builtin(memcpy)` and answering NO for a builtin madc implements.
`defined` was already protected from expansion; the `__has_*` family now
is too, operand group and all (`<a/b.h>`, `"a.h"`, `clang::foo` survive
intact).

### P2.2b — the embedded-header partition vs libc++'s C wrappers

**Found immediately after slice 1, and it is the next real subject.**
With honest `__has_*` answers, the Linux libc++ probe stops here:

```
cstddef:50: #error <cstddef> tried including <stddef.h> but didn't find
libc++'s <stddef.h> header. ... The header search paths should contain
the C++ Standard Library headers before any C Standard Library
```

madc embeds the freestanding set — `stddef.h`, `stdint.h`, `limits.h`,
`float.h`, `stdarg.h`, `stdbool.h` — and **libc++ ships its own wrappers
for several of those names**, which must be found FIRST (they
`#include_next` the C library's afterwards). madc's embedded copies
shadow them, so libc++ detects that its wrapper was bypassed and
refuses.

This is the header-partition question
(`madc-header-partition-handoff.md`) meeting a real C++ standard
library, and the resolution has to be principled rather than a
name-based exception list:

- clang's own order is **c++/v1 → compiler resource dir → C library**,
  and madc's embedded freestanding headers are precisely its "compiler
  resource dir". So the ordering rule is already written down by the
  canon compiler: the C++ standard library's headers outrank madc's
  embedded set, which in turn outranks the C library's.
- Which means embedded headers must become a **fallback for names a real
  search path cannot satisfy**, not an unconditional first hit. That
  keeps the header-less-Mac promise (nothing on the search path → the
  embedded copy serves) while letting a present libc++ win, and it is
  the same precedence P2.4's frozen groves will need.
- The darwin lane will hit the identical wall once its trait blocker is
  gone; fixing it here fixes it there.

**The mechanism, and it needs no name list.** The embedded set becomes a
**virtual directory positioned in the search list**, at the slot the
generated table ALREADY records as `madc_compiler_owned_include_dir`.
Dirs before it outrank it; dirs after it lose to it. On the current
Linux g++ build that slot is index 3 of 7:

```
0 /usr/include/c++/13/                      ┐
1 /usr/include/x86_64-linux-gnu/c++/13/     │ C++ stdlib — outranks embedded
2 /usr/include/c++/13/backward/             ┘
3 /usr/lib/gcc/x86_64-linux-gnu/13/include/ ← the slot: madc's embedded set
4 /usr/local/include/                       ┐
5 /usr/include/x86_64-linux-gnu/            │ C library — embedded outranks
6 /usr/include/                             ┘
```

So the position is derived from the same generated table the search
already uses — no second list, no `#ifdef`, no per-name exception
(Rule #7). `ns_php.h` and friends need no special case either: no real
dir ships those names, so they resolve from the virtual dir wherever it
sits.

**Measured 2026-07-26 — the change is provably baseline-neutral.**

| madc embeds | libstdc++ 13 ships | libc++ 18 ships |
|---|---|---|
| `float.h` | — | **yes** |
| `limits.h` | — | no |
| `stdarg.h` | — | no |
| `stdbool.h` | — | **yes** |
| `stddef.h` | — | **yes** |
| `stdint.h` | — | **yes** |

libstdc++ ships **none** of the six, so on the existing Linux lane the
reordering cannot change a single resolution. libc++ ships **four** —
exactly the wrappers that must win. Three further checks confirm the
regression surface is empty: the baked-PCH table is generated empty, no
test fixture uses `-I` at all, and no copy of any of the six exists
anywhere under `tests/` or `include/` outside `include/madc/` itself.

**`#include_next` is part of the fix, not a follow-on.** libc++'s
`stddef.h` is a wrapper: it does `#include_next <stddef.h>` and guards a
second one with `__has_include_next(<stddef.h>)`. So a `#include_next`
from a dir BEFORE the slot must be able to land ON the embedded copy —
otherwise libc++'s wrapper reaches past madc's freestanding header
entirely. Slice 1 bought this for free: because `__has_include_next`
answers through the same resolver `#include_next` uses, fixing the
resolver fixes the query with no second edit.

**Testable today, before `-stdlib=` exists**: `-I/usr/lib/llvm-18/include/c++/v1`
puts libc++ ahead of the slot, since `-I` dirs precede the generated
list — which is also gcc's own precedence for `-I` over its resource dir.

Sequencing note: this now precedes the two mandatory intrinsics, because
on the Linux lane it fires first.

Same method as the libstdc++ burn-down: compile a real
`#include <string>` / `<vector>` / `<iostream>` against libc++, reduce
each failure to a minimal `.cpp`, fix at the **deepest layer**, never
shim. Both canon compilers apply — clang is the natural oracle here (it
owns libc++), and per the two-canon rule clang also acts as the scope
filter: what clang rejects, madc need not support.

**Where:** on Linux, against `libc++-18-dev`, via `-stdlib=libc++` —
NOT against the macOS SDK. Same library, same `__config`, same inline
ABI namespace, same headers modulo Apple availability attributes; but
the loop is local, the oracle (`clang++-18 -stdlib=libc++`) is right
there, `--emit=c11` parity applies, and nothing waits on hardware. The
SDK copy then only has to prove the *target plumbing* (P2.1), not the
library semantics.

Deliverable alongside the fixes: a burn-down count in this doc, updated
per slice, the way the tsubst burn-down tracked `[why:]` classes.

Expect the long pole here: libc++ leans on `_LIBCPP_*` attribute macros,
`__attribute__((…))` spellings, and its own `__config` feature
detection.

### P2.3 — the STD-ABI flavor switch

libc++ puts everything in an inline ABI namespace (`std::__1` in the
common build), so every mangled-direct symbol differs from libstdc++'s
(`_ZNSt3__1…` vs `_ZNSt7__cxx11…`). Give `madc_mangle` a **stdlib-flavor**
enum — two values, libstdc++ and libc++ — selected by `-stdlib=` /the
target default, never by a host `#ifdef` and never by "is the target
Apple". It covers:

- the std inline-namespace component in mangled names,
- the `cout` / `cerr` / `cin` global symbol names,
- every place the 21 `__cxx11` references hardcode the GNU spelling.

**Do not hardcode `__1`.** libc++'s inline namespace is
`_LIBCPP_ABI_NAMESPACE`, defined in its own `__config` — `__1` today,
but versioned, and a vendor (Android among them) can build libc++ with a
different one. madc *parses* `__config`, so the mangler must take the
namespace from the parsed environment, exactly as libstdc++'s `__cxx11`
follows `_GLIBCXX_USE_CXX11_ABI`. That keeps the switch genuinely
two-valued instead of growing into a table of vendor spellings, and it
is the deepest layer available: the answer comes from the headers the
program is actually compiled against.

Layouts and sizes are NOT part of the switch: they come from the parsed
headers. If a size turns out to be baked somewhere, that is a bug to fix
at its source, not a second table to maintain.

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
- `-isysroot` and `-stdlib=` are the override spellings (clang/gcc
  parity). No madc-only invented flags.
- The stdlib flavor is selected by `-stdlib=` / the target's default,
  **never** by the build host and **never** by "is the target Apple":
  libc++ is a library, not a platform (Apple, Android NDK, FreeBSD, and
  clang on Linux all use it).
- The inline ABI namespace is **read from the parsed `__config`**
  (`_LIBCPP_ABI_NAMESPACE` / `_GLIBCXX_USE_CXX11_ABI`), never a literal
  in madc's source.
- No embedded libc++ subset. Header-less Macs are served by the frozen
  forest; unpacked + no SDK = loud refusal.
- Sizes/layouts come from the parse. Any baked size found is a defect.

## Gates

The library work gates on **Linux**; only the target plumbing needs a
Mac. That split is the practical payoff of treating libc++ as a library
rather than a platform.

- **Primary, Linux, no hardware:** `madc -stdlib=libc++` against
  `libc++-18-dev`, with `clang++-18 -stdlib=libc++` as the oracle — JIT
  and native lanes, same program compared both ways. A permanent gate
  script in fulltest once the first slice lands, skipping loudly (never
  silently) if `libc++-dev` is absent, per the machogate lesson.
- **`--emit=c11` / oracle parity** (the emitted-C rule): the
  clang-compiled emission must match the clang++-compiled original.
- **Cross-emit + hardware run**, the same shape every darwin slice used:
  a C++ program cross-emitted for both arches and run on the owner's
  Mac; output identical to the Linux libc++ run. This proves the SDK
  tables and the Mach-O plumbing, not the library semantics — those are
  already green by then.
- A **packed** darwin binary compiling C++ with the live parse disabled
  — proving the frozen libc++ groves served the compile (P2.4), the way
  S1 proved grove bind on the Mac.
- Linux fulltest/`--exe`/`--obj`/packed arbiter unchanged throughout:
  this track must not move the existing numbers.

## Sequencing

1. **P2.0 — `-stdlib=` and the flavor plumbing**, plus `libc++-18-dev`
   in the container (`scripts/provision_container.sh`). Makes everything
   after it measurable on Linux.
2. **P2.2** burn-down slice 1: `<string>` parses under
   `-stdlib=libc++` on Linux.
3. **P2.3** ABI flavor: `std::string` round-trips mangled-direct against
   libc++ — the first end-to-end proof, entirely on Linux.
4. **P2.2** slices 2+: containers, then `<iostream>`.
5. **P2.1** target-derived tables for the cross-macos MODEs; darwin
   cross-emit + hardware run.
6. **P2.4** freeze the libc++ groves into the darwin packs;
   header-less-Mac gate.

Note the reordering the owner's correction bought: P2.1 (the SDK tables)
was the prerequisite for measuring *anything* when this was framed as a
darwin track. Framed as a library flavor, the Linux path opens first and
the darwin plumbing lands once the semantics are already proven.

Each step is independently shippable and gated; none of them may move
the Linux baseline.

## Blocked / pending

- Every darwin RUN leg needs the owner's hardware, as in every previous
  darwin slice. Nothing else in this track does.
- *(Resolved 2026-07-26: the container's `/workspace` was lost in a WSL
  crash and reattached the same day; the toolchain it lost is now
  reproducible via `scripts/provision_container.sh`.)*
