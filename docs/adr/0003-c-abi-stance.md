# ADR 0003 — The libmadc ABI stance: C ABI stable, C++ version-locked

- **Status:** Accepted (owner ruling 2026-09-01)
- **Date:** 2026-09-01
- **Context version:** v0.97.0 (develop), packaging arc PK1
  (`docs/plans/2026-09-01-packaging-arc.md`)

## Context

The packaging arc flips the default install to a thin `madc` CLI +
shared `libmadc` carrying the frozen forest. A shared library needs a
stated ABI contract: what a consumer may link against, what survives
an upgrade, and when the soname moves. libmadc's dynamic export
surface (as of v0.97.0, ~49,900 defined dynamic symbols) has four
very different populations, and only one of them can carry a
stability promise.

## Decision

**The C ABI is stable; the C++ surface is version-locked.**

- **The C-host ABI is exactly the `extern "C"` surface declared in
  `include/madc_api.h`** (94 functions as of v0.97.0). Post-promise,
  removing or incompatibly changing one of these bumps the soname
  major. Additions are minor-version fair game.
- **The C++ surface makes no cross-version promise.** The mangled
  exports (~48,800 symbols: `madc::`/`php::`/… namespace publics, the
  template headers' instantiations) exist for two version-locked
  consumers — script code resolving mangled-direct against the
  library image, and C++ embedding hosts, which rebuild per release.
  The forest blob is already version-locked to its library
  (one-format/one-loader law), so library + forest + C++ surface move
  as one artifact.
- **soname:** `libmadc.so.0` is the pre-promise era. `.so.1` opens
  the promise, on the owner's call, once the PK1 gate is green and
  the internal-export renames below have been weighed. The thin CLI
  and the library ship in ONE package and can never skew.

## Export classification (gated)

`scripts/check-c-abi-surface.sh` (fulltest, negative-controlled)
classifies the complete extern-C (unmangled) export surface; an
export outside every class fails the build loud. Classes, as of
v0.97.0:

| class | count | disposition |
|---|---|---|
| `madc_*` declared in `madc_api.h` | 94 | **THE C ABI** — the promise |
| `madc_*` listed in `scripts/c-abi-internal-exports.txt` | 33 | internal (engine globals, cell/put/dl runtime entries) wearing the contract prefix — NOT ABI; rename to `__madc_*` is a `.so.1`-era item (forest/ledger symbol-name blast radius, deliberate) |
| `__madc*` | ~347 | compiler-emitted machinery (scope/vla/task/chan runtime) — dlsym targets, must stay exported, never ABI |
| `__js_/__perl_/__php_/__py_/__rb_/__rust_*` | ~226 | polyglot namespace runtime entries — dlsym targets, version-locked |
| `madarray_*` | ~43 | carrier (value/array) runtime entries — dlsym targets, version-locked |
| `MIR_/_MIR_/c2mir*/__mir_/mir.*` | ~251 | in-tree MIR/c2mir — internal |
| `__jit_debug_descriptor/_register_code` | 2 | GDB JIT interface — required names |
| `dd[A-Z]*/tk[A-Z]*` | ~84 | parser singletons — accidental exports, non-contract, visibility-trim candidates |
| misc allowlisted | 8 | `get_argv`, va-interp builtins, … — each with a stated disposition in the allowlist |

## Visibility policy

Default visibility is **retained** for now: the JIT resolves the
machinery, polyglot, carrier, and mangled namespace publics via
`dlsym(RTLD_DEFAULT)` against the library image — those exports are
functional, not accidental, and `-fvisibility=hidden` would break
script binding. Trimming the genuinely accidental classes
(`dd*`/`tk*`, misc singletons) is a `.so.1`-era slice that must ship
with a dlsym-resolution audit proving nothing the JIT binds went
dark.

## Consequences

- A new extern-C export must land in a class deliberately: declared
  in `madc_api.h` (ABI), or `__madc_*`-prefixed (machinery), or
  allowlisted with a disposition — otherwise fulltest fails naming it.
- A `madc_api.h` declaration that stops being exported fails the same
  gate (broken contract is caught at build, not at a consumer's link).
- The SONAME is pinned in the gate; moving to `.so.1` is a deliberate
  one-line change in the same commit as the promise documentation.
- Windows (`libmadc-0.dll` — renamed from the historically misnamed
  `libmadc_rt.dll` in the PK3 wave: it is the FULL engine, the twin of
  `libmadc.so.0`, and it carries the same ABI number mingw-style like
  its shipped neighbors `libstdc++-6.dll`/`libwinpthread-1.dll`; its
  import lib is `libmadc.dll.a`, and `_rt` now names ONLY the tiny
  emitted-C archive `libmadc_rt.a` on every platform) and macOS
  (`libmadc-0.dylib` when the darwin runtime-library slice lands)
  export surfaces ride their own lanes; this gate covers the ELF
  surface. Extending the classifier is a PK3/PK5-era follow-up. The
  `.so.0` → `.so.1` weigh-in bumps all platform twins together.
