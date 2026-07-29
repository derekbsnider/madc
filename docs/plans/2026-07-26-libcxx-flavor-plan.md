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

## Goal (owner-set 2026-07-29: behavior-parity)

**Drive libc++ to behavior-parity with the default libstdc++ flavor.**

FINISH LINE: every in-scope integration test passes under `-stdlib=libc++`
in all four lanes (JIT / exe / obj / packed), measured by a **flavored
suite lane** in the battery — libstdc++-specific tests carry a
`.libcxx_skip` fixture with a one-line reason, the same classification
discipline as `docs/parity/failset-classification.md`. Until that lane
exists, gate-pair twins (`testX_libcxx`) carry the burn-down.

**Parity is behavior, never mechanism** — this revises the original
"mangled-direct exactly like libstdc++" goal, because the two libraries'
export policies differ structurally: libc++ marks nearly its whole
surface `_LIBCPP_HIDE_FROM_ABI` with ABI tags (`size()` mangles as
`...4sizeB8ne180100Ev`), deliberately NOT exported from `libc++.so.1`.
So: resolve **mangled-direct only what `libc++.so.1`/`libc++abi.so.1`
actually export** (`nm -D` is ground truth — the cout/cin/cerr data
symbols, the abi runtime, the few extern-template exports); everything
hidden-from-ABI **compiles from the parsed headers** through the existing
CIR monomorphization spine (the same machinery as vector/map/set — no
wrapper shims, no madc-authored twins, either way). Oracle for libc++
lanes is `clang++-18 -stdlib=libc++`. All flavor differences stay
data-driven (the flavor table / the parsed config) — never
platform-keyed, never name-keyed.

Ladder: P2.4 `<string>` runtime (task #17, active) → P2.5 `<iostream>`
(verifies the `std::__1` chain symbol round-trip) → P2.6 containers →
P2.7 the flavored suite lane + burn-down (the parity ratchet) → P2.8
per-flavor forest/PCH (performance parity — a separate rung, never a
correctness gate).

Non-goals: cross-flavor ABI interop; identical skip sets between flavors.

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

   **RESOLVED 2026-07-27 — and it required no new intrinsic at all.**
   libc++ accepts **either** `__is_trivially_destructible` **or**
   `__has_trivial_destructor`, and madc has implemented the latter all
   along: `type_trait_arity()` in parser.cpp is a real registry of nine
   traits madc answers faithfully from its DataDef model (`__is_class`,
   `__is_union`, `__is_enum`, `__has_trivial_destructor`, `__is_trivial`,
   `__is_empty`, `__is_standard_layout`, `__is_pod`, plus the arity-2
   `__is_same`, `__is_base_of`, `__is_assignable`).

   The `#error` fired only because slice 1's `has_builtin` did not know
   that registry existed — trait intrinsics carry no `__builtin_` prefix,
   so it answered no for a trait madc implements. `is_type_trait_builtin()`
   is now declared in `madc.h` and consulted by `has_builtin`: **one
   registry, two consumers** (the preprocessor query and the parser's
   sema), which is the same "answer from madc's own state" contract slice 1
   established, extended to a second kind of state rather than a second
   mechanism.

   Verified zero blast radius on the existing lane: every `__has_builtin`
   guard in libstdc++ 13 names a trait madc does *not* implement
   (`__builtin_bit_cast`, `__reference_constructs_from_temporary`,
   `__is_convertible`, …), so all of them still answer no exactly as
   before. Only libc++ asks about a trait madc has.

   The lesson is Rule #4, sharply: the registry, its sema, and the nine
   traits were all already there. Building a new intrinsic would have
   duplicated working machinery to fix a one-line lookup gap.
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

**P2.2b LANDED (2026-07-27).** `Program::embedded_header_outranked()` lives
beside its sibling classifier in the header-partition area and is consulted
at both embedded-selection sites (the tokenize branch and
`named_include_provider_exists`, so the once-only dedup key and the
auto-include defer gate agree with what actually resolves). The predicted
`#include_next` behaviour held exactly: libc++'s wrapper chains past the slot
to gcc's real freestanding `stddef.h`, so no second mechanism was needed on
Linux — the embedded copy is the fallback only where no real resource dir
exists, which is the header-less-Mac case.

**A real parse defect surfaced immediately behind it, and it was NOT a libc++
quirk.** With libc++'s wrapper winning, the chain reached gcc's `stddef.h` and
died on its `typedef struct {...} max_align_t;`:

```
/usr/lib/gcc/x86_64-linux-gnu/13/include/stddef.h:436: Expecting alias name in typedef
```

madc pre-registers `max_align_t` as a base datatype (it carries a stable
value-ABI type id, `MADC_TYPEID_MAX_ALIGN_T`), so the name lexes as a
`TokenDataType`. Two independent bugs, both in madc's own machinery:

1. **The struct-typedef path accepted only `ttIdentifier` in the alias
   position, while the general typedef path already accepted `ttDataType` and
   `ttKeyword`.** `typedef int max_align_t;` compiled; `typedef struct {...}
   max_align_t;` did not — the same declaration failing purely because the
   body was a struct. Fixed by extracting the accept set into one helper
   (`typedef_alias_spelling`) that both paths now share, so they cannot drift
   apart again.
2. **The duplicate-name check could not tell madc's own pre-registration from
   a user declaration.** Relaxing it by token kind would have cost the genuine
   `typedef struct{int a;}Foo; typedef struct{int b;}Foo;` diagnostic that gcc
   and clang both emit. Instead `TokenDataType` gained a `builtin` flag that
   `add_datatypes()` stamps across the whole map in one pass (so a future
   builtin needs no second edit), and the check skips only when the *existing*
   entry is madc's own. The user-vs-user conflict still errors.

Worth stating plainly because it generalizes: **this was never about libc++.**
Any real toolchain header that typedefs a name madc pre-registers hit it; the
libc++ lane is simply the first path that reached one.

### Measured burn-down (2026-07-27, Linux, `-I<libc++ 18>`)

| Header | Before P2.2b | After |
|---|---|---|
| `<cstddef>` | `#error` — libc++ wrapper bypassed | **compiles + runs** |
| `<cstdint>` | (unreached) | **compiles + runs** |
| `<climits>` | (unreached) | **compiles + runs** |
| `<stdio.h>` via libc++'s wrapper | (unreached) | **compiles + runs** |
| `<type_traits>` | (unreached) | `#error is_trivially_destructible is not implemented` → after slice 2 + `_Pragma`, **compiles + runs** |
| `<string>` | `#error remove_reference` | same `is_trivially_destructible` `#error` → now `__builtin_nansf` in `<limits>` |

The frontier moved to **exactly the second of the two mandatory intrinsics the
plan predicted**, and `remove_reference` no longer blocks (Linux libc++ 18
carries a fallback for it where the SDK copy measured earlier did not).

**Then step 2 landed the same day** (see above — it was a registry-lookup gap,
not a missing intrinsic), and the frontier moved again:

```
__utility/declval.h:22: use of undeclared identifier '_Pragma'
__type_traits/aligned_storage.h:100: use of undeclared identifier '_Pragma'
```

### Next slice: `_Pragma`

`_Pragma("...")` is the C99/C++11 token-form of `#pragma` — it destringizes its
operand and processes it as a pragma directive. libc++ reaches it through
`_LIBCPP_SUPPRESS_DEPRECATED_PUSH` and friends, which are warning-control
pragmas madc only needs to *accept*, not act on. Two constraints shape it:

- It must route to madc's EXISTING pragma handling, not a second copy — the
  `#pragma pack` machinery is the one that matters and must keep working
  identically. Today that handler reads char-by-char straight off `source`,
  so the slice's real work is extracting a text-driven entry point both the
  directive and the operator call. That is the whole reason this is its own
  slice rather than a bolt-on.
- Unknown pragmas must stay ignorable (gcc's behaviour), so the destringized
  content follows exactly the path an unrecognized `#pragma` line already
  takes.

Note it is not libc++-specific: `_Pragma` is standard C since C99, and any
header may use it (doctest's own macros do).

**LANDED 2026-07-27.** `handle_pragma_body()` is now the one implementation,
entered with `source` positioned at the pragma text; the `#pragma` directive
arrives already positioned and `handle_pragma_operator()` destringizes into the
same stream before calling it. Because the body was already text-driven, the
operator needed no copy of the `pack` / `push_macro` handling — which is the
whole point, since those are the two pragmas madc genuinely acts on.

Three details worth keeping:

- **The operand is read as a TOKEN, not as characters.** The standard
  macro-expands it first and real headers depend on that: libc++ writes both
  `_Pragma(#x)` and `_Pragma(_LIBCPP_TOSTRING(clang diagnostic ignored str))`,
  neither of which is a string literal until expansion has run. Going through
  the lexer buys that expansion for free, and its string case has already
  undone `\"` and `\\` — which `_Pragma("GCC diagnostic ignored
  \"-Wdeprecated\"")` needs — so the token's text IS the destringized pragma
  line, with no second unescaper to drift out of sync.
- **The pushed-back text is newline-terminated**, and that is load-bearing:
  every branch of the handler ends by discarding the rest of its directive
  line, so without a terminator the discard runs off the end of the pushback
  and eats real code. Pushed-back newlines do not advance the line counter, so
  `__LINE__` after a `_Pragma` is unchanged (the test pins this).
- **Not gated on `--std=`.** Both canons accept `_Pragma` in every mode,
  `-std=c89 -pedantic` included; the name is reserved to the implementation, so
  it cannot collide with user code.

Gated by `tests/testpragmaoperator.mad` rather than a libc++ gate leg, since the
feature is standard C: it asserts that the directive and operator spellings
produce the *same* layout (`pack(1)` written both ways), covers `push_macro` /
`pop_macro` and the `DO_PRAGMA(x)` stringize idiom, and matches gcc and clang
byte-for-byte.

### Frontier after `_Pragma` (2026-07-27)

`<type_traits>` now **compiles and runs clean** — the template-metaprogramming
long pole this plan predicted at step 3 did not materialize as a wall. Two
distinct blockers remain, and they are independent:

| Blocker | Reached via | Shape |
|---|---|---|
| `__builtin_nans` / `nansf` / `nansl` undeclared | `<limits>`, so `<utility>` `<string>` `<vector>` | madc has the **quiet**-NaN family (`__builtin_nan{,f,l}`) and `__builtin_inf*` / `__builtin_huge_val*` in the same `macro_map` block; only the **signaling** trio is absent |
| `std::is_destructible` evaluates wrong | `std::is_trivially_destructible<T>::value` | madc says 0 where clang says 1 for a trivial struct |

On the first: do **not** alias the signaling trio to `(0.0/0.0)` to make the
error go away. That is a quiet NaN; `numeric_limits<T>::signaling_NaN()` would
then be silently wrong. A signaling NaN is a distinct bit pattern, and these are
used in constant expressions, so the slice is a real question about how madc
spells one — not a table entry to copy from its neighbour.

**libc++'s traits DO work.** `std::is_same<int,int>`, `std::is_same<int,long>`
and `std::is_class<Plain>` all evaluate correctly against libc++ 18 and match
the clang oracle. An earlier draft of this section claimed the `std::` names did
not register at all; that was wrong, and the way it was wrong is worth keeping,
because it cost a chain of dead-end hypotheses (inline namespaces, attributed
namespaces, attributed class heads, skipped `#if` guards — all four eliminated
by reducer, all four innocent). Every probe had been written as
`(int)std::is_same<int,int>::value`, and the **cast** was the thing failing:

```
S::n              → works          (int)S::n        → "undeclared identifier 'S'"
S::value          → works          (int)S::value    → same
Box<int>::type    → works          &S::n            → "Unknown namespace 'S'"
Box<int>::get()   → works
```

So the real defect is **a C-style cast (or `&`) whose operand is a qualified
name** — `(T)X::y`. It is not libc++-specific, not template-specific and not
namespace-specific; it is core C++ that happens to appear on every line of
trait-using code. Lesson for the next reducer: when a probe fails, reduce the
*probe* before theorizing about the library.

### Worklist status (2026-07-27, end of session)

| # | Item | State |
|---|---|---|
| 1 | `(T)X::y` cast with a qualified operand | **FIXED** @3167da3c (class qualifier) + @08593822 (template-id) |
| 2 | out-of-line static data member unbound | **FIXED** @3167da3c |
| 3 | `&S::n` → "Unknown namespace 'S'" | **FIXED** @cf8f035c — the address-side twin of (1); `parseAddressOfExpression` resolved only namespaces |
| 4 | `std::is_destructible` evaluates wrong | open — the SFINAE/`declval` chain, i.e. the real tsubst burn-down |
| 5 | `__builtin_nans{,f,l}` | **FIXED** @51e224e2, completed @e8e7b4e8 |
| — | `long double` was 64-bit | **FIXED** @114b13a8 — surfaced by (5); see below |
| — | P2.0 `-stdlib=` | **DONE** — the flag replaces the search list; see below |

**`long double` (found via item 5, fixed @114b13a8).** It lexed to the *double*
DataDef: `sizeof` 8 against 16, `printf("%Lg")` printing `nan`, and the mangler
emitting Itanium `e` for a value passed as a `d`. The cause is worth recording
because it generalizes — the original asmjit JIT could not emit x87 80-bit, so
the front end substituted a double to let such source compile. Every *other*
layer was already built for the real type (`dtLDOUBLE`, `is_real()`, the `e`
mangling, `__LDBL_MAX__` in the generated macro table, `copysignl`'s
registration), which is the signature of a narrow backend workaround rather than
a design choice. asmjit went away, c2mir/MIR support the type fully, and nothing
marked the mapping provisional — so it survived the backend swap and became
wrong. **When a backend constraint is lifted, grep for the decisions that cite
it.** An audit of the remaining asmjit references found two live behaviours
still justified by it (`TokenFunc::is_overridden`, lambda hoisting); both are
still correct for other reasons and only their comments were stale.

**A miss worth keeping.** @51e224e2 wrote `__builtin_nansl` against the 64-bit
`long double` with a comment saying "revisit together with the long-double width
itself"; @114b13a8 changed the width without revisiting it, leaving the macro
writing 8 bytes into a 16-byte type — uninitialized stack in the exponent. The
full battery passed over it, because the test asserted `nansl_low64`, which had
been the entire value when the test was written. An assertion scoped to the old
type's width cannot observe the new bytes. Two rules: fixing a constraint is not
done until every workaround *citing* it is revisited (the same grep-for-citations
move as the asmjit sweep, applied to a two-commit-old comment); and scope
assertions to the property, not to today's representation.

### P2.0 `-stdlib=` — DONE, and why `-I` had run out

The boundary this plan predicted arrived exactly where it said it would. A real
`std::string` compile got past every parse blocker and then failed here:

```
/usr/include/c++/13/stdlib.h:38: 'abort' is not a member of namespace 'std'
```

That is **libstdc++'s** header inside a libc++ compile. libc++'s `<cstdlib>`
does `#include_next <stdlib.h>`, which walks past libc++'s own directory and
lands on GNU's, because madc's search table is generated from `g++` and always
carries `/usr/include/c++/13`. Real `clang -stdlib=libc++` does not have those
directories on the path at all. So selecting a library has to **replace** the
C++ search list, not prepend to it — `-I` can put libc++ first, and first is not
the same as only.

**Shape of the fix.** A search list is a property of the *flavor*, so
`gen_sys_includes.sh` now emits **one table per flavor** and `-stdlib=` picks
which — the same thing clang's driver does, one list built per library rather
than one list reordered. `src/sys_include_paths.cpp` grew a
`madc_stdlib_flavors[]` of `{ name, paths, compiler_owned_dir }`; the resource
dir is per-flavor too, since gcc's and clang's differ and it is the slot madc's
embedded set occupies.

Three properties worth keeping:

- **Which flavors exist is a build-host fact, discovered, never listed.** The
  default is whatever `$(CXX)` resolves to; an alternate is recorded when some
  probe reports a *different* library. Candidate probe commands
  (`$(CXX) -stdlib=…`, `clang++-18 -stdlib=libc++`, …) are tool spellings, not
  answers — each is asked what it actually resolved to, by reading the library's
  own `_LIBCPP_VERSION` / `__GLIBCXX__`. A candidate whose compiler or library
  is absent reports nothing and drops out, and the `!= default` test is what
  makes a silent fallback impossible to mistake for a second flavor.
- **One reader.** All five consumers (the lexer's `<...>` resolution, the
  system-header classifier, `#include_next`, and the two embedded-header
  partition predicates) went through `Program::sys_include_paths()` /
  `compiler_owned_include_dir()`, which also collapsed three copies of the
  no-compiler fallback list into one. Deleting the old globals let the compiler
  confirm the sweep was complete.
- **An unavailable flavor fails loud**, naming what the binary was built with —
  because availability is a property of the machine that built it, a diagnostic
  listing the flag's possible spellings would be useless.

Reachable from all three surfaces a flavor can arrive on: `-stdlib=` on the
command line, `stdlib =` in `madc.ini` (CLI wins, same precedence as `std`), and
`-stdlib=` in a `compile_commands.json` entry — that last one because a libc++
project's manifest carries it and ignoring it would compile silently against the
wrong library's headers.

Gate legs 7–10 in `scripts/libcxx_gate.sh` cover it. Leg 7 is the tight one:
under `-stdlib=libc++`, `_LIBCPP_VERSION` is defined and `__GLIBCXX__` is *not*
— a statement about which library served, with nothing in it about paths.

### The frontier after P2.0

`std::string` under `-stdlib=libc++` now reaches deep into libc++ and stops in
its `<cctype>`:

```
c++/v1/cctype:111: 'isalnum' is not a declaration in '::'
```

`_LIBCPP_USING_IF_EXISTS` is empty for madc (`__has_attribute(using_if_exists)`
answers 0, per slice 1's deliberate "no registry to ask yet"), so the line is a
plain `using ::isalnum;`, sitting just after libc++'s block of `#undef`s of the
ctype *macros*.

**⚠️ The first version of this section was written from a broken bisect harness
and its conclusions were wrong.** The harness classified a compile by whether
its first output line was empty — and madc prints a blank line before every
diagnostic, so *every failure read as a pass*. `<string_view>`, `<stdexcept>`
and `<__string/char_traits.h>` do **not** compile alone; the "two-line reducer"
was really a one-line one. Measured again on **exit status**:

| Header alone, `-stdlib=libc++` | Result |
|---|---|
| `<ctype.h>` `<cctype>` `<cwctype>` `<wctype.h>` `<wchar.h>` `<climits>` | compile |
| `<cwchar>` | `cwchar:202` — `std::wcslen`, a **call**, not a using-declaration |
| `<__string/char_traits.h>` | `__functional/reference_wrapper.h:51` |
| `<string_view>`, `<string>` | `cctype:111` — `using ::isalnum;` |
| `<stdexcept>` | `cstddef:50` — **libc++'s own `#error`** |

Five *distinct* failures, not one. That reading is what made the next step
findable, and it is why the harness bug is recorded here rather than quietly
fixed. See [[feedback_reducers_need_flags]] — third instance.

### The one that cracked: `<cstddef>` — FIXED

libc++'s `#error` says it outright: *"`<cstddef>` tried including `<stddef.h>`
but didn't find libc++'s `<stddef.h>` header."* libc++'s `stddef.h` is
**deliberately re-includable** —

```c
#if defined(__need_ptrdiff_t) || defined(__need_size_t) || ...
#  include_next <stddef.h>          // does NOT define _LIBCPP_STDDEF_H
#elif !defined(_LIBCPP_STDDEF_H)
#  define _LIBCPP_STDDEF_H
```

so a first visit through the `__need_*` branch must be followed by a second,
full visit. That second visit only happens under gcc's guard-checked
multiple-include semantics, which `should_tokenize_include()` applies to
**system** headers; a header read as *user* code gets madc's require-once rule
and the second visit is dropped forever.

And every libc++ header was being read as user code. `should_tokenize_include()`
canonicalizes the file through `realpath`, while `is_system_header_path()`
prefix-matched the **raw** generated table entry — and clang reports its own
search dir as `/usr/lib/llvm-18/bin/../include/c++/v1`. The two spellings never
match. GNU's paths are already canonical, which is why this never surfaced
before libc++.

Fixed by canonicalizing the prefixes (cached per flavor) and matching either
spelling; entries that do not resolve keep their raw form, so cross/hosted
tables naming an absent sysroot still work. **Proven before it was written**: a
build with `MADC_SYS_INCLUDE_PREFIX_MAP` rewriting the path to its canonical
form made the `#error` disappear, and the real fix reproduces that against the
un-rewritten table. Gate leg 11 pins it.

Consequence worth noting beyond this bug: `is_system_header_path()` also gates
CIR inline-body emission (reachability DCE), so libc++ headers were on the wrong
side of *that* decision too.

### `cctype:111` — ROOT CAUSE FOUND (fix not yet written)

It is **not a libc++ problem and not an include problem**. It is a C++ parser
bug, reducible to eight lines with no library headers at all:

```cpp
template <class T> T declval();
template <class...> struct vt { typedef void type; };
template <class T, class U, class = void> struct cmp { static const int v = 0; };
template <class T, class U>
struct cmp<T, U, typename vt<decltype(declval<T>() < declval<U>())>::type> { static const int v = 1; };
// madc: "Unexpected end of data"      g++/clang: prints `1 0`
```

The `<` in `declval<T>() < declval<U>()` is a **less-than operator**, but the
scanner reading the partial specialization's template-argument list treats it as
opening a nested argument list. It then consumes `declval<U>`'s `>` as the
closer and runs to EOF looking for a delimiter that does not exist.

**The whole cascade came from this one defect, six headers upstream of where it
was reported.** libc++'s `__utility/is_pointer_in_range.h` contains exactly that
construct. It dies mid-header, so its `_LIBCPP_BEGIN_NAMESPACE_STD` never opens
a namespace — and the matching `_LIBCPP_END_NAMESPACE_STD` then *closes scopes
that were never opened*. From that point the scope stack is wrong, so later
global declarations (glibc's `isalnum` among them) are not reachable from `::`,
and `using ::isalnum;` fails in a file that is itself perfectly fine. Every
order-dependence in the earlier bisects was this: whichever header first drags
in `is_pointer_in_range.h` determines where the damage surfaces.

**The correct rule is already implemented — in one place.** `parser.cpp:38774`
tracks angle brackets *only outside parens and subscripts*, and its comment
cites this exact shape (`decltype(forward<_Tp>(t) < forward<_Up>(u))`) along
with the same "consumed to EOF" symptom. A sibling scanner lacks the guard.
There are six `angle_depth` scanners; audit them against that one:

| Site | Function | Has `paren_depth`? |
|---|---|---|
| parser.cpp:38716 | (the fixed one — reference implementation) | yes, with the citation |
| parser.cpp:3320 | `collect_template_argument_spelling` | yes |
| parser.cpp:38358 | `collect_template_default_argument` | audit |
| parser.cpp:45465 | `skip_template_suffix_tokens` | audit |
| parser.cpp:48445 | (inner scope) | audit |
| parser.cpp:18083/18104 | spelling helpers (string-level) | audit |

Six copies of one rule is the real finding — the fix is to make them share it,
not to add the guard a seventh time (`design-principles.md`,
`no-parallel-implementations.md`).

**A second bug is queued behind it**, seen once `is_pointer_in_range.h` is
worked around: `compare` → `math.h:414: Unknown namespace 'std::__math'` — a
*nested* namespace qualifier, the same family as the `&S::n` fix but for a
qualified `std::__math` name.

### Superseded: the earlier "still open" analysis

`using ::isalnum;` still fails from `<string_view>`, with canonical paths in
place — so it is **not** the same bug. Eliminated by measurement:

| Eliminated | Evidence |
|---|---|
| include resolution / `#include_next` | `-v` trace shows the exact clang chain: `cctype` → `c++/v1/ctype.h` → `/usr/include/ctype.h`. glibc's header **is** read, once, immediately before line 111. |
| the system/user misclassification above | persists after the fix |
| PCH divergence | zero `precompiled` hits in the trace |
| inline namespace | `namespace std { inline namespace __1 { using ::isalnum; } }` compiles |
| `#undef` dropping the function | `#define isalnum(c) 0` then `#undef isalnum` then `using ::isalnum;` compiles |
| `<ctype.h>` not declaring it | `<ctype.h>` + `using ::isalnum;` compiles, both flavors |

Next thread: `<cctype>` compiles alone and *inside `<cwchar>`*, but not inside
`<string_view>` — so ask what registration state ~290 preceding headers leave
behind, and whether glibc's `ctype.h` takes a different branch there (its
`__exctype` / `__isctype_f` declarations are gated on feature macros).

**A caution worth keeping:** `isalnum(65)` *compiles under madc whether or not
`isalnum` is declared*, via the implicit-function fallback. An early probe used
exactly that and I read "the chain works" out of it. Probe a declaration with
`using ::X;`, never with a call.

Nothing here is libc++-specific machinery — it is madc's own include and
registration state, which is the pattern this whole track keeps producing.

### cwchar:202 — ROOT CAUSE FOUND AND FIXED (2026-07-28)

**The defect was never about `using`, wide chars, or libc++.** Qualified
lookup `N::m` probed only N's own variable map; members of N's **inline
namespace set** were invisible until `mirror_inline_namespace_into_parent()`
copied them up at the namespace's CLOSE ([namespace.qual] says they are
members immediately). libc++'s entire body is `std::X` calls inside a
still-open `namespace std { inline namespace __1 {` block, so the first
qualified use of anything in the block — `std::wcslen` inside
`__constexpr_wcslen`, six headers away from where the damage was reported —
failed.

**Why six synthetic reducers missed it:** every hand-built approximation
placed the qualified use after the block's close (shape H), unqualified
(shape F), or qualified through `std::__1::` (shape G) — all of which work.
The failing shape needs the use INSIDE the same still-open block (shapes
C/I/L; types are served by a different path and never failed, K). What
found it: bisecting the real `madc -E` output of `#include <cwchar>` (2893
lines → 22) instead of rebuilding from guesses — the KG note "the
assembled-from-parts approach has now failed six times" was the right
directive.

Fixed in `find_namespace_member()` — the owner of "member of N" — as a
lookup-time walk of the inline set (allocation-free when N has no inline
children); nine flat-probe consumer sites adopted it and the scope-chain
walker's duplicate BFS was deleted. Gates: `tests/testinlinensopen.mad`
(madc == g++ == clang++ byte-identical) and `libcxx_gate` leg 12
(`<cwchar>` compiles and runs against the real header).

**Frontier after the fix:** `<cwchar>`, `<cwctype>`, `<cctype>`,
`<__string/char_traits.h>` all compile and run under `-stdlib=libc++`.
`<string_view>` stops at `__exception/operations.h:29` — `using
terminate_handler = void (*)();`, a using-alias to a FUNCTION-POINTER type
madc's alias parser does not accept (KG
`Gap{using_alias_function_pointer_type}`). `<string>` stops at the
already-queued `math.h:414` `std::__math` nested-namespace qualifier.

### The 2026-07-28 burn-down — nine defects, one session, all core C++

After the cwchar fix (above), the frontier fell in a chain, every defect a
gap in madc's own generic machinery (none libc++-specific), every fix
gated by an oracle-verified test:

| # | Defect | Fix | Gate |
|---|---|---|---|
| 1 | qualified lookup blind to the inline set while the block is OPEN | `find_namespace_member` owns the walk; 9 consumers adopted | testinlinensopen + gate leg 12 |
| 2 | `using X = void (*)();` rejected | abstract twin of typedef Form 2, same parseFnPtrParams owner | testusingaliasfnptr |
| 3 | `std::__math` unknown (nested ns in inline set) | `canonical_nested_namespace` + path fold, 4 resolvers adopted | testnestedinlinens |
| 4 | `<string>` stack-overflow SIGSEGV (allocator CRTP base-arg self-reference) | ONE in-flight registry, both lanes, both cache regimes | testcrtpbasearg + gate leg 13 |
| 5 | `nullopt_t::__secret_tag{}` "not a static member" | nested-TYPE construction arm in the shared resolver | testnestedtagctor |
| 6 | braced ctor lists dropped args after the first (ANY class!) | [dcl.init.list]/3: non-empty braced list of a ctor-ful class routes to ctor selection | testbracedctor |
| 7 | no `__underlying_type` intrinsic; enum fixed base DISCARDED | DataDefENUM.underlying + canon range rule + the intrinsic (decltype model) | testunderlyingtype |
| 8 | decl-only primary + partial specs never real-instantiate | `spec_may_serve` defers the opaque bail (scoped: outside class bodies / dependent parses) | testdeclonlyspec |

Suite grew 776 → 784 along the way, zero failures throughout, every fix
matching g++ AND clang++ byte-for-byte on its reducer.

**Frontier after all of it:** `<cwchar>` `<cctype>` `<cwctype>`
`<__string/char_traits.h>` `<optional>`'s nullopt line, and
`__atomic/memory_order.h` all compile under `-stdlib=libc++`. BOTH
`<string_view>` and `<string>` now stop at ONE defect:
`Gap{common_type_dependent_member_key_explosion}` — `<chrono>` duration
arithmetic instantiates `common_type` with UNRESOLVED
`common_type<...>::type` member-type arguments; the spellings become
instantiation keys and compound exponentially until the MADC_MEM_LIMIT
guard trips (loud, names the knob — the guard did its job at 4G and 12G).
The `::type` in template-ARG position must resolve to its concrete target
before keying. Same dependent-member-type family as the
`std::is_destructible` item — this IS the tsubst burn-down the plan
predicted at step 3, now with a precise reproducer
(`MADC_MTI_PROBE_CLASS=_` shows the compounding key).

Known remainders recorded on the way (KG): plain empty nested
`struct tag {};` doesn't register in the class type-alias surface
(nested_tag family); `&A::B::c` address-of has no nested-namespace
descent; class-scope member aliases over decl-only-primary specs keep the
opaque route (the #8 scoping); enum STORAGE still lowers to int (a fixed
base narrower than int changes only what `__underlying_type` answers, not
layout).

### The worklist as first written (2026-07-27, verified by reducer at HEAD)

1. **`(T)X::y` — cast with a qualified-name operand — fails to parse.** Highest
   value: it masks everything else and is the universal spelling for trait use.
   `X::y` alone parses fine, so the gap is in how the cast's operand is parsed,
   not in qualified lookup.

   **Localized (2026-07-27) — the fix is one missing arm.** A C-style cast whose
   operand is a bare identifier routes to `Program::parsePostfixChain()`
   (`src/parser.cpp`, from the cast branch at ~27931), *not* to the shunting-yard
   identifier arm that owns `::` at statement level. `parsePostfixChain` already
   has a `tkNS` block at **parser.cpp:19675** — and it was written for exactly
   this case, its comment citing `(int)Size::Large` — but it resolves only when
   the qualifier is a **namespace** or a scoped-enum pseudo-namespace
   (`namespace_map` / `namespace_datatype_map`). A **class** qualifier falls past
   it to `findVariable("S")` and dies as "undeclared identifier 'S'". Confirmed
   by the split: `(int)N::n` with `namespace N` works, `(int)S::n` with
   `struct S` does not.

   The missing arm is symmetric with the one above it and needs no new
   machinery: `resolve_expression_class_scope(name)` maps the qualifier to a
   `DataDefCLASS`, and `resolve_class_static_member_type()` /
   `resolve_class_static_member_const_value()` turn the member into a typed
   value — the same pair `parsePostfixChain` already calls sixty lines below for
   the *implicit* enclosing-class case (parser.cpp:19738). Fixing it here rather
   than in the cast branch fixes all ~10 `parsePostfixChain` callers at once.

   **Do not simply resolve every static through the const-value pair.** It
   returns 0 for a member that has real storage, which would turn defect (2)'s
   loud-ish failure into a silent wrong answer in one more place. Either resolve
   storage-backed statics to their `TokenVar`, or restrict the new arm to
   compile-time constants and let the rest keep erroring — decide with (2), not
   around it.

   Note `(int)(S::n)` already works: parenthesizing routes the operand through
   the generic branch instead. That is the workaround, not the fix, and it is
   also why the defect can hide.
2. **Out-of-line static data member definitions are not bound to their
   declaration.** `struct S { static int n; }; int S::n = 5;` then reading
   `S::n` yields **0** — a silent wrong value, which makes this the most
   dangerous of the five — and `S::n = 9` fails with "lvalue required as left
   operand of assignment". In-class `static const int n = 5;` works, and so do
   `enum { n = 5 };` members, so the gap is specifically the out-of-line
   definition.
3. **`&S::n` reports "Unknown namespace 'S'"** — probably the same parse path as
   (1), but confirm rather than assume.
4. **`std::is_destructible` yields the wrong answer**, which is what makes
   `is_trivially_destructible` wrong: libc++ defines the latter as
   `is_destructible<_Tp>::value && __has_trivial_destructor(_Tp)`, and madc's
   own `__has_trivial_destructor` is **correct** (`1 0 1` on
   trivial/non-trivial/`int`, matching g++ exactly). So the defect is upstream of
   the intrinsic, in the SFINAE/`declval` chain libc++ builds on it. This is the
   genuine template-metaprogramming burn-down the plan predicted at step 3 —
   note `__utility/declval.h` only became reachable once `_Pragma` landed.
5. **`__builtin_nans{,f,l}`** as above.

Items 1–3 are core-C++ defects that owe nothing to libc++; the track keeps
paying out that way, which is the argument for treating it as C++ conformance
work that libc++ happens to measure.

**Gate:** `scripts/libcxx_gate.sh`, wired into `fulltest`, six legs — the
wrapper wins when libc++ precedes the slot; the embedded copy still serves
when nothing does; the `<cstddef>` chain compiles *and runs*; `<cstdint>` +
`<climits>` likewise; `clang++ -stdlib=libc++` agrees as the oracle that owns
this library; and the conflicting-typedef diagnostic still fires. It discovers
libc++ by asking clang (`-stdlib=libc++ -E -v`) rather than hardcoding a path,
and SKIPS loudly when clang or libc++ is absent, per the machogate lesson that
a silent skip reads as green.

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

**One such place is already known** (found while doing P2.0, left alone
because the native link lane is a different surface):
`cir_native_link_env()` in `src/madc_cir.cpp` picks the C++ runtime for
`DT_NEEDED` — `libc++` versus `libstdc++.so.6` — behind a host
`#ifdef __APPLE__`. That is the platform-means-library conflation this
whole track exists to undo, and it is doubly confused: the `#ifdef` is a
property of the machine madc was *built* on, while `DT_NEEDED` describes
the *target*. It works today only because the two happen to agree. The
runtime library a program links is the same flavor question `-stdlib=`
already answers, so it belongs in this switch — and the comment there is
right that cover analysis needs the *host's* image names, which is a
genuinely separate concern that should stop sharing one `#ifdef` with the
target's.

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

### P2.7 — the flavored parity lane, and the number it produced

**Shipped 2026-07-29 (`45f64a6f`).** `scripts/run_tests.sh
--stdlib=libc++` runs the whole corpus under the alternate flavor in any
execution lane; `remote_build.sh libcxx` drives JIT + exe + obj.
Mechanics worth keeping straight:

- `--stdlib=NAME` is appended to `HERMETIC_FLAGS`, so all eight
  invocation sites inherit it without a per-site edit.
- Out-of-scope tests carry `tests/<base>.libcxx_skip` (the extension is
  the flavor name with `+` → `x`, derived mechanically — no per-flavor
  branch in the runner).
- The lane prints `FLAVORED RUN — -stdlib=... ; NOT the default-lane
  baseline` so its numbers can never be mistaken for the arbiter's.

**Baseline at `45f64a6f` (JIT leg): 534 passed / 282 failed / 10
skipped.** The decomposition is the actionable part: **257 of the 282
include a stream header**, so `<iostream>` is ~91% of the entire gap and
the other 25 are a mixed tail. This is the number the parity goal is
measured against — a later run that moves it is progress, and a later
run that does not has not advanced the goal no matter what else landed.

The lane is deliberately **not** wired into `fulltest` yet. A gate whose
baseline is 282 failures ratchets nothing; it goes in when the failures
are classified — genuine gaps fixed, out-of-scope ones carrying a
`.libcxx_skip` reason — so that the wired-in number is 0. That is the
P2.7 finish line, not the lane's existence.

Sampled causes in the 25-test non-stream tail, each still to be
attributed: a HYBRID mangling (`_ZStplI…NSt3__1…` — a libstdc++ outer
template name with libc++ inner ABI segments), an unresolved
`__ns_std____1____math_isinf`, and two tests that fail on **output
mismatch with no compile error** — that last class outranks the rest,
being a silent wrong answer.

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

1. ~~**P2.0 — `-stdlib=` and the flavor plumbing**, plus `libc++-18-dev`
   in the container (`scripts/provision_container.sh`). Makes everything
   after it measurable on Linux.~~ **DONE** — see "P2.0 `-stdlib=` — DONE"
   above; gate legs 7–10.
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
