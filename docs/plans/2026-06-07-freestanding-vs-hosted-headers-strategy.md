# Header strategy: madc owns the FREESTANDING set, consumes the LIBRARY set unmodified

**Date:** 2026-06-07. **Branch:** `feature/realhdr-parse-gaps2-claude`.
**Status:** strategy/principle note. **The authoritative OPERATIONAL plan is
`madc-header-partition-handoff.md` (repo root)** — a 6-step implementation
handoff (discover the set via `gcc -print-file-name=include` + checksum →
classify 3 buckets by `#include_next` → implement a madc-owned freestanding dir →
GCC-impersonation macros via `gcc -dM` diff → `<type_traits>` as a BUILTINS
workstream → search order → acceptance tests). This doc is the principle + madc's
current-state mapping + two refinements; defer to the partition handoff for the
steps.

**THE FORK IS RESOLVED → option B.** The partition handoff commits to B
explicitly ("Do NOT handroll the C or C++ standard library… consume everything
else stock"; the conformance milestone is getting real
`<type_traits>/<tuple>/<vector>/<string>` through). So: consume real libstdc++/
glibc unmodified; retire ALL madc library stubs **including** the custom
header-defined-class `<string>`. The real-header track IS this plan.

**TWO REFINEMENTS this doc adds to the partition handoff:**
1. **Borrow the freestanding headers from c2mir** (`/workspace/mir/c2mir/mirc*.h`
   + `x86_64/mirc_x86_64_*.h`) — madc's OWN backend already ships them, matched to
   the `cir_node→c2mir→MIR` pipeline. Implementation source for the handoff's
   Step 3 (gcc's `-print-file-name=include` dir is the COMPLETENESS cross-check,
   per Step 1; do NOT add gcc's dir to the search path — handoff Non-goal #2).
2. **Add a `__madc__` identity macro** (handoff Step 4 lists identity macros but
   only gcc's) — be a PEER like Clang (`__clang__` + `__GNUC__`), not a guest.
   madc has NO identity macro today.

**KEY RECONCILIATION — `--no-embedded-headers` is a DIAGNOSTIC, not the end-state.**
It falls through to the filesystem, which includes **gcc's own `$OWN` freestanding
dir** (gcc's `stddef.h` etc.) — which the partition handoff Non-goal #2 says madc
must NOT use (they assume gcc builtins madc may not match). So the asm-label /
`__need_XXX` / multi-line-comment blockers we hit were partly in GCC's freestanding
headers madc shouldn't even consume; the end-state replaces them with madc's OWN
freestanding dir (from c2mir) + system LIBRARY dirs only. (Those lexer fixes are
still general-good — glibc/libstdc++ headers have the same constructs.)

**STEP 5 IS THE BIG WORKSTREAM (reframe).** Many remaining "parser" failures on
real headers are actually **missing predefined macros or missing `__is_*`/`__has_*`
builtins**, NOT lexer/parser bugs (handoff Non-goal: don't treat header failures as
parser bugs by default). VERIFIED: madc implements **none** of the type-trait
builtins (`__is_base_of`, `__is_trivially_constructible`, `__underlying_type`,
`__type_pack_element`, `__integer_pack`, …) — a semantic-layer checklist that is
the real `<type_traits>`/`<tuple>` conformance milestone. (The current `iterator<>`
blocker is a genuine parser bug — template-id-vs-alias — and stays on the parser
track; but expect most of the rest to be the builtin checklist.)

---

(original strategy note follows)

## The principle (per C/C++ "freestanding vs hosted" split)
A peer compiler (gcc, clang) does NOT ship the standard library; it ships the
small, nearly-static set of **compiler-owned freestanding headers** and
**impersonates** itself well enough that the *unmodified* system library headers
parse. Three buckets:

1. **Pure compiler headers (madc supplies, no system twin):** `stddef.h`
   (`size_t`/`ptrdiff_t`/`offsetof`/`max_align_t`), `stdarg.h`
   (`va_list`→`__builtin_va_list`, `__builtin_va_*`), `stdalign.h`,
   `stdnoreturn.h`, `iso646.h`, the intrinsic family
   (`immintrin.h`/`xmmintrin.h`/… → pure `__builtin_*` codegen shims). Define
   things only the code generator knows. Cannot be written in portable source.
2. **Thin layering shims (madc supplies; `#include_next` falls through to the
   system one):** `stdint.h` (gcc's is ~`#include_next <stdint.h>`), `limits.h`
   (compiler piece + `syslimits.h` + `#include_next <limits.h>`), `float.h`,
   `stdatomic.h`. madc owns the compiler-known part; glibc owns the rest.
3. **Pure library/system headers (madc NEVER supplies — consume unmodified):**
   libstdc++ `<string>`/`<vector>`/`<type_traits>`/`<tuple>`/streams; glibc
   `<stdio.h>`/`<stdlib.h>`/`<string.h>`/`<math.h>`/… These churn with the
   library version; handrolling them is the nightmare to AVOID.

**The oracle (authoritative, version-pinned):** `gcc -print-file-name=include`
prints the directory of headers gcc considers its own — that directory IS the
bucket-1/2 set, by construction. You don't infer the split; you read it off the
toolchain you impersonate. (NB: this is the *physical compiler/library* split —
deliberately NOT the standard's language-lawyer "freestanding library facilities"
clause, which calls `<type_traits>`/`<new>`/`<initializer_list>` "freestanding"
even though they ship from libstdc++. For "what files must madc supply to make the
real headers parse", the toolchain directory is the authority.)

**The mechanism that makes unmodified libstdc++/glibc parse — be a PEER, like
Clang, not a guest in gcc's house.** Clang defines `__GNUC__`/`__GNUC_MINOR__`/
`__GNUC_PATCHLEVEL__` ONLY to the degree the system headers' `#if __GNUC__ >=`
branches need — *and simultaneously defines its own identity `__clang__`*, ships
its OWN freestanding headers (`lib/clang/<ver>/include`, pointedly NOT gcc's), and
implements its own builtins. It impersonates gcc no more than necessary for the
hosted headers to parse; it does not pretend to BE gcc. madc must do exactly that:
- Set `__GNUC__`/`__SIZEOF_*__`/`__*_TYPE__`/feature-test macros to match the
  target ABI so `#if __GNUC__` and `__is_trivially_constructible` resolve as under
  gcc, AND
- declare madc's OWN identity (a `__madc__` / `__MADC__` version macro — **madc
  does NOT define one today**; add it, the Clang-equivalent of `__clang__`), and
- ship madc's OWN freestanding headers (borrow from c2mir — see below), not gcc's.
Then the gnarliness in `<type_traits>` collapses into a
**builtin-implementation checklist**, not a header-parsing war.

**Borrow the freestanding headers from c2mir — it's madc's OWN backend.** c2mir
already ships exactly this set, written for the code consumer madc lowers to:
`/workspace/mir/c2mir/mirc.h`, `mirc_iso646.h`, `mirc_stdalign.h`,
`mirc_stdbool.h`, `mirc_stdnoreturn.h`, and per-arch
`x86_64/mirc_x86_64_{stddef,stdarg,stdint,float,limits,linux}.h` (the `_linux.h`
carries the target predefined-macro set). These are the ideal source for madc's
freestanding set — preferred over copying gcc's dir, since they're already
matched to madc's `cir_node`→c2mir→MIR pipeline. Learn/borrow from them rather
than reimplement.

## madc's current state (mapped onto the three buckets)
- **Impersonation: ✓ strong already.** `predefined_macros.cpp` defines
  `__GNUC__=13`, `__GNUC_MINOR__=3`, `__GNUC_PATCHLEVEL__=0`, `__SIZEOF_*__`,
  `__*_TYPE__`; `#include_next` is implemented (`lexer.cpp` `resolve_include_next_path`).
- **Freestanding set: PARTIAL.** `include/madc/` has `float.h limits.h stdarg.h
  stdbool.h stddef.h stdint.h`. **Missing vs gcc's dir:** `stdalign.h
  stdatomic.h stdnoreturn.h iso646.h cpuid.h unwind.h` + the ~101 intrinsic
  headers + the C++ bridges (`<cstddef>` etc.). And `stddef.h`'s `offsetof` is the
  portable `((T*)0)->m` form, not `__builtin_offsetof` (works, but be gcc-faithful
  since madc IS the compiler).
- **Library stubs: PRESENT and WRONG (bucket 3 violation).** `include/madc/` ships
  `string vector map set algorithm iostream sstream fstream typeinfo` (libstdc++)
  and `stdio.h stdlib.h string.h math.h ctype.h time.h unistd.h …` (glibc). These
  **shadow** the real headers (checked before the filesystem) and break include
  chains — the documented embedded-stub-shadowing blocker. They are exactly the
  bucket that madc must NOT supply.
- **madc-specific (legit, keep):** `ns_php/ns_perl/ns_python/ns_ruby/ns_js/ns_rust`
  (+ headers) — the borrowed-language namespaces; not standard headers at all.

## ★ STRATEGIC FORK — RESOLVED → B (per `madc-header-partition-handoff.md`) ★
(Kept for context; the partition handoff commits to B. The text below explains
the two options.)

madc's `include/madc/string` is **not** a freestanding header — it's the
retire-std-hardcoding campaign's **custom header-defined `std::string`** (a thin
class binding to real libstdc++ Itanium symbols; see
[[project_string_as_class]], [[project_cpp_mangled_direct]]). That is **option A**:
madc supplies its own thin `<string>`/`<vector>`/… and never parses libstdc++'s
real ones.

This research advocates **option B**: consume libstdc++'s real `<string>`/`<vector>`
unmodified (the real-header track — what we've been advancing, now at the
`iterator<>` blocker), supplying only the freestanding set + impersonation.

The two are different architectures. B is "win the header-parse war once, via
impersonation + builtins" (the 6 cleared blockers this session are exactly that
work). A is "avoid parsing the gnarly real headers by shipping thin binding
headers." **Decision needed:** is the end-state to parse real libstdc++ (B,
retire ALL library stubs incl. the header-defined-class ones), or keep the
header-defined-class layer (A) for the std:: types and only consume real headers
elsewhere? The research strongly favors B.

## Action checklist (if B / the real-header end-state)
0. **Add madc's own identity macro** (`__madc__`/`__MADC__` + version) alongside
   the gcc-compat macros — be a peer (Clang's `__clang__`), not an impersonator.
1. **Complete the freestanding set by BORROWING c2mir's** (`mirc_*.h`,
   `x86_64/mirc_x86_64_*.h`) rather than copying gcc's dir: add `stdalign.h
   stdnoreturn.h iso646.h stdatomic.h` and the C++ bridges; add intrinsic headers
   lazily (only ISAs you support). Point `stddef`/`stdarg` at
   `__builtin_offsetof`/`__builtin_va_list`/`__builtin_va_*`. (gcc's
   `-print-file-name=include` dir remains the cross-check for COMPLETENESS of the
   set; c2mir's `mirc_*` are the implementation source.)
2. **Make `#include_next` chaining correct** for the layering shims (`stdint.h`,
   `limits.h`) so glibc's real versions are reached.
3. **Retire the library stubs** (`string vector map set algorithm iostream
   sstream fstream typeinfo` + the glibc duplicates) — this IS the
   retire-std-hardcoding deletion step. Real libstdc++/glibc then parse via
   impersonation. (`--no-embedded-headers` already simulates this end-state for
   testing — that's how the real-header frontier is being driven.)
4. **Fill the builtin/`__has_*` checklist** as the real headers demand
   (`__is_trivially_*`, `__has_builtin`, etc.) — the work that replaces the
   header-parsing war.
5. Search order: madc's freestanding dir FIRST, then system (it already is for the
   embedded layer; verify with the real-header probe).

## Maintenance reality
The freestanding set is small, stable (stddef/stdarg-class unchanged for decades),
and madc's by spec; intrinsics grow ~once/year per ISA and only if you opt in.
The library set is large, churning, and the system's by spec — consumed stock.
This INVERTS the maintenance worry: B is the technique that lets you NOT handroll
the library.

Related: [[project_string_as_class]], [[project_cpp_mangled_direct]],
[[project_template_instantiation]], [[project_north_star_c23_cpp23]];
`2026-06-07-template-id-disambiguation-research.md` (the current `iterator<>`
blocker on the B path).
