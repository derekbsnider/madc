# Dialect-Lean — reasoning

## The rulings

Two owner rulings on 2026-08-21, during the JIT Python-contender arc
(`docs/plans/2026-08-21-project-prelude-forest.md`):

1. "As far as --std=madc goes, we try to avoid depending on C++ system
   header parsing unless we can reduce the overhead significantly —
   which is what the pre-compiled header / frozen forest stuff was
   _supposed_ to achieve, but is still falling short — so until such
   time that becomes a reality, the idea is that we lean more heavily on
   madc builtins, and polyglot conveniences (php, python, etc)."
2. "Nothing the madc language defines within its own dialect or the
   polyglot functionality should depend on std::string (beyond potential
   internal to the madc source code usage)."

## The measured failure that motivated them

The adventure game (11 dialect TUs, deliberately written with zero C++
header usage) launched in ~1.5s packed while its Python port launched in
~0.10s. The game code was clean — the PRELUDE was not: `bits/std_format`
did `#include <string>` for exactly one declaration (`std::string
format(...)`'s return type) and `ns_php` for its interop overloads. That
single `<string>` closure meant every dialect TU bound 138–145 of 339
forest units and decoded ~66 MB of zstd frames — per TU, eleven times
per game launch. On the dev binary the same closure was a full live
parse per TU (the A10 parity gate ran 8m23s).

## Why the guard convention (one file, two renderings)

`<ns_madc>` already solved this shape: it declares its value/char*
primary API unconditionally and gates its std::string conveniences on
`#if defined(_GLIBCXX_STRING) || defined(_LIBCPP_STRING)` — the standard
library's own include guards. The conveniences exist exactly when the TU
has already paid for `<string>` (explicitly, or via auto-include when
the script uses `string`). One publics list in one file cannot drift; a
generated or hand-maintained second "lean" copy can (the /dupaudit
divergence class).

## Why std::string is banned from the surface (not just the includes)

A std::string-typed declaration anywhere in the always-served surface
forces the class to exist, which forces the `<string>` parse/bind. It
also leaks include-dependent semantics into the language: the carrier's
old element model typed `arr[i]` as std::string when `<string>` had been
seen and as int64 otherwise — the same spelling meant different things
in different TUs, and the std::string arm materialized element READS
into a hidden temp, so `arr[i] = x` compiled, exited 0, and wrote
nothing. The slot model (`madarray_index_slot`, the keyed slot's twin)
replaced it: one include-independent semantics, writes land in the live
element, and the carrier owns its element surface with zero libstdc++.

## What "internal usage" means

The engine/runtime (ns_common ring slots, madc::value internals, host
C++ implementations behind mangled-direct publics) is ordinary C++ and
uses std::string freely — scripts never parse those sources, so it costs
the dialect nothing. The ban is on the SCRIPT-FACING surface: embedded
fragments, intrinsic declarations, carrier semantics.

## The gate

`scripts/check-dialect-lean.sh` statically scans every dialect-served
fragment for (a) any C++ system `#include` (a header without `.h`), and
(b) any `std::string` mention outside the stdlib-guard conditional. It
runs in fulltest and self-tests with a negative control (a synthetic bad
fragment must FAIL) so the gate itself cannot silently rot. The dynamic
half of the contract is pinned by tests: `testformatring` (dialect
format with zero includes), `testarrayslot` (carrier element slots), and
the packed-binary bind counts recorded in the JIT plan.
