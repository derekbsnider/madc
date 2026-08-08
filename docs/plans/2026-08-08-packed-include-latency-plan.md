# Packed-lane `#include <string>` latency (task #25, 2026-08-08)

## Mission (owner directive)

"Nearly 200ms just to use a std::string seems like an awfully large
overhead… `#include <string.h>` the C way takes about 1/4 the time…
figure out how to lower std::string down a bit more so the difference
isn't so huge."

## Baseline (container, `bin/madc-release`, `--show-stats`)

| probe | total | units bound | register | node-record segs | zstd decoded |
|---|---|---|---|---|---|
| bare `int main(){}` | 2ms | 0 | — | 0 | — |
| `<string.h>`+`<stdio.h>` used (C way) | **31ms** | 30 | 1ms | **0** | 15MB |
| `<string>` UNUSED | 154ms | 139 | 32ms | 6 | 57MB |
| `<string>` + one std::string use | **185ms** | 139 | 32ms | 12 | 76MB |

Phase buckets lie today: release "lex" = 96ms at 266 tok/s (forest
bind/restore under the lexer's `#include` window), and ~45ms of
on-demand unit loads land inside "cir build" (84ms shown; real lowering
~35-40ms per the dev-binary comparison).

## Profile (callgrind, unstripped -O2 packed twin at tmp/madc-release-sym; unused-include probe, 779M Ir)

| share | what | root cause (source-verified) |
|---|---|---|
| 12.8% | zstd decompress | 57MB decoded at bind for zero uses |
| 12.1% | `__memset` | `std::vector::resize` zero-fills BOTH the staging buffers (`rb/kb/cb/pb`, `unit_segment` cir_freeze.cpp:3553) AND the typed destination vectors, all fully overwritten right after — payload memset once, copied twice |
| 6.1% | `CirFrozenForest::unit_segment` self | first-load validation loop over every record+child (the corrupt-container firewall — keep) |
| ~14% | malloc/_int_free/consolidate | staging-buffer + token/record allocation churn |
| 4.7% | `__memcmp` | string-keyed map compares (register/intern paths) |
| 3.4% | `madc_pch::deserialize_tokens` | template pattern TOKEN RUNS deserialized eagerly at restore |
| 2.4% | `materialize_from_arena` | typed-DataDef rebuild |
| ~4% | `findVariable`/`intern`/`_Rb_tree` insert | eager registration into live parser maps |

Also source-verified: `snapshot_reader::find()` (madcdis_snapshot.cpp:380)
is a LINEAR scan of the segment directory per lookup (4 finds × unit).

## Why an unused include pays at all (the eager-restore chain)

`forest_restore_decls` → template-record walk (cir_freeze.cpp ~3294):
every template whose declaring unit is in the bound closure and whose
key passes the demand verdict calls `ensure_template_payload()` — which
decodes the payload/token segments and builds `CirRestoredTemplate`
(key, name, param runs, token runs) for EVERY such template. For
`<string>` that is hundreds of surviving templates, none of which the
program may ever instantiate. The laziness is per-TU ("trivial C never
pays"), not per-template. The C path is fast precisely because it has
no templates to restore and libc functions ride the dlsym fallback
(register 1ms vs 32ms).

## Slices

- **A — decode-in-place + O(1) segment lookup — ✅ DONE.**
  `snapshot_reader::read_segment_into(seg, dst, cap)` primitive over a
  shared `decode_payload` core (both read_segment overloads ride it —
  one implementation); `unit_segment` sizes the typed destination
  vectors once from the directory's raw sizes and decodes directly into
  them (staging buffers deleted: one memset instead of two, no second
  copy, no staging mallocs); `_dir_index` built at `open()` so `find()`
  is O(1). **Measured: `<string>` used 185→143ms (−23%), unused
  154→130ms, C-path 31→29ms** — unit loads 57→34ms, register 32→23ms,
  materialize 25→21ms (the indexed find serves the restore paths too).
  Unit suite green (9019 assertions incl. test_madcdis_snapshot).
- **B — lazy template payloads (the big one).** Registration keeps the
  template KEY surface eager (lookup must see the name), but
  `CirRestoredTemplate` payload/token-run decode defers to first
  instantiation request of THAT key. Kills most of the 57MB decode +
  deserialize_tokens + a large slice of materialize for programs that
  use few of a header's templates (all of them, for the unused case).
  Risk: instantiation-path correctness; the forest gates
  (selfexe/bind/emitpack/sidecar/crosstu/library/ledger/config) +
  full battery are the net.
- **C — lazy registration.** register = 32ms flat, all map injection
  (`findVariable`/intern/Rb_tree ~7-8% + memcmp share). The decl-index
  sweep is 2-3ms — resolve through the frozen index on lookup misses
  instead of eagerly injecting 139 units' decls. Biggest architectural
  change; do after A+B re-measure.
- **D — stats attribution.** One depth-guarded forest-work clock
  (InstTimer discipline; entries: map/open/bind/restore/unit-load)
  sampled at the tokenize/parse boundaries so lex/cir-build report NET
  compute and the forest share prints as its own carved lines. Do with
  A so post-A numbers are honest.

Floor estimate: C-path parity is not reachable (C++ genuinely restores
class/template surfaces C doesn't have), but unused-include ≈ C-path
cost and used-`<string>` well under 100ms look attainable from the
profile shares.

## Facts for re-measurement

- Probes: `tmp/lexprobe_bare.mad`, `tmp/lexprobe_cstring.mad`,
  `tmp/lexprobe_nouse.mad`, `tmp/lexprobe_string.mad` (container).
- Unstripped -O2 packed twin: relink `make -C src MODE=release
  ../bin/madc-release` (skip strip), `bash scripts/forest_pack.sh
  bin/madc-release`, copy aside, then `make -C src release` to restore
  the canonical stripped artifact. Callgrind outputs at
  `tmp/cg_nouse.out` / `tmp/cg_string.out`.
- Branch: `feature/forest-string-latency-claude` off develop v0.72.0.
- QUEUED BEHIND THIS TASK: slice 2 of the channel arc (approved):
  standing API rule in cpp-first-api.md + gate ("madc-specific publics
  never REQUIRE std::string; value/const char* paths mandatory,
  std::string kept as convenience"), value overloads on madc::channel,
  eval-family value completions follow-up.
