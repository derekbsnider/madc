# Plan — C++ standard "bar": configurable default + per-feature gating

**Status:** design (deferred from the 2026-06-17 session at user request). Companion:
handoff §19N 0c; task #33. Serves invariant "no hardcoded standards" (see
`docs/plans/madc-vision-and-invariants.md`).

## Problem

madc parses a lot of C++20 syntax, but the practical "bar" for how high a `--std=`
it can serve is gated by **how much C++20+ *system-header* machinery it must
digest to `#include` the standard library**. Concretely: `--std=c++20` currently
gets past `set::contains` then dies on `Unknown namespace 'ranges'` — C++20
`<set>`/`<map>` constructors and members are ranges/concepts-constrained. So a
single monolithic `--std=` flag is all-or-nothing: selecting C++20 opens the
*entire* C++20 header surface at once, including the parts madc can't yet handle.

Two real hardcoding smells motivated this:
- the default `__cplusplus` was a bare literal (fixed: `84a03ab` introduced the
  configurable `Program::default_cpp_std`);
- the std level is otherwise coarse — there is no way to say "C++20 syntax, but
  only the library features we actually support".

## The two-axis model

1. **Coarse axis — `default_cpp_std`** (landed, `84a03ab`): the C++ level plain
   madc *presents* when no `--std=` is given. Keep it at **C++17** — the last
   level whose full standard-header surface madc digests. Raising it is one
   assignment; an embedder/CLI can override it.

2. **Fine axis — a feature-macro registry** (this plan): madc sets `__cplusplus`
   per the selected `--std=`, but only **advertises the `__cpp_*` / `__cpp_lib_*`
   feature-test macros for features it actually supports.** libstdc++ gates its
   own feature regions on these (`#if __cpp_lib_ranges`, `#if __cpp_concepts`,
   `#if __cpp_impl_three_way_comparison`, …), so the headers themselves compile
   *away* the surface madc can't handle yet. This is using libstdc++'s own gating
   to scope our support, rather than fighting it.

   This generalizes the existing hand-set block at `lexer.cpp:1615` (which
   already special-cases `__cpp_concepts` / `__cpp_impl_three_way_comparison` at
   the C++20 floor) into a real, std-gated registry.

Net effect: `--std=c++20` can mean "C++20 *syntax* permitted" while the C++20
*library* surface opens **feature-by-feature** as each is implemented — enabling
incremental ranges/concepts work instead of an all-or-nothing wall.

## Design sketch

- A table: `{ macro_name, value, min_std, supported }`. Emit `__cpp_*` only when
  `language_std >= min_std && supported`. The `supported` flag flips on as each
  feature lands (with a test that proves the corresponding header region compiles).
- Keep `__cplusplus` honest (set per `--std=`), so user code that checks
  `__cplusplus` behaves correctly; the *library* gating rides on the finer
  `__cpp_lib_*` macros, matching how libstdc++ is actually written.
- Drive everything from the `LanguageStd` enum — no per-feature `#ifdef` islands
  in madc's own source; the registry is the single source.

## Sequencing / relationship to other work

- Independent of the ranges *implementation* itself — this is the gating
  framework that lets ranges land incrementally. Build the framework first.
- `testset`/`testmap` (`contains`, C++20) are correctly rejected at the C++17
  default today (verified parity: `g++ -std=c++17` also rejects). They become
  reachable only after (a) the framework here + (b) actual ranges support, OR by
  rewriting those tests to C++17 API (`count()`/`find()`). Decide per test.
- Not on the critical path for `vector<T*>` (that is std-independent — see
  `2026-06-17-vector-of-pointers.md`).

## Open questions (for the user)

- **Use case for a runtime "ceiling":** is it (a) just "don't hardcode the
  default" (done + this registry), or (b) a *policy cap* an embedder enforces so
  hosted scripts cannot request above a configured std (ties into the
  sandboxed-embedding direction)? (b) is a distinct libmadc-API feature; (a) is
  this plan.

## Done =

A feature-macro registry drives `__cpp_*`/`__cpp_lib_*` emission off the
`LanguageStd` enum; raising support for a C++20+ library feature is "flip a
`supported` flag + add a test", not "bump `__cplusplus` wholesale"; no magic std
literals remain in madc source.
