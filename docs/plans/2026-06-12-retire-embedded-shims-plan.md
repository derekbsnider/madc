# Retire the embedded bucket-3 header shims — campaign plan

## STATUS (2026-06-12, live — read top-down)

**FULL COLD-START HANDOFF (read that first):**
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` — includes the
process-conformance items (P1–P5) that institutionalize
`madc-header-partition-handoff.md`, the diagnostics arsenal, and the
current wall list. The block below is the shorter chronology.

Branch `feature/retire-embedded-shims-claude` @ `1b91e9f`. Phases 0+1
COMMITTED+GATED (fa25e7f K&R gate / 13383b7 presents_as_cpp; fulltest 582
green, torture 52-name baseline zero-regr, SMAUG soak green). Phase 2+3
sweep COMMITTED as WIP (2d61556 all shims deleted +23k lines removed;
bb8083b preprocessor root causes; 1b91e9f SFINAE pre-check). Integration
currently 546/36/0/18.
Remaining walls, in attack order:
1. CLEARED @1b91e9f (was: typename dependent return types — now a
   sandboxed SFINAE pre-check discards unresolvable candidates). NEW
   first wall in the math family: param-scope leak after a mid-body
   fn-template instantiation — real <cmath> hypot(double,double,double)
   cmath:3571 reports "__z undeclared" (the SECOND 3-arg hypot overload,
   right after __hypot3<float> instantiated from inside the FIRST one's
   body; suspect instantiation save/restore state or stream position).
   Reducer: tmp/m4.mad (#include <math.h> alone, default mode).
2. Class-scope alias (`typedef _Bit_iterator iterator`) not visible in
   hidden-friend operator bodies — blocks real <vector>/<map>/<set>
   (testvector/map/set/containerdtor/template*/subscript*).
3. testmadceval*: `_ISupper` undeclared (ctype enum emission on eval path).
4. teststat/teststatret/testservent: parse error in real sys/stat.h chain.
5. testmultiret/testrust: bogus mangled `_ZNSolsESo` import (operator<<
   overload mis-pick).
6. `--emit=c11` name hygiene on real-header path (operatornew[]__o5,
   operator""s leak into emitted C — JIT unaffected).
Gates for any merge talk: fulltest green + torture diff vs the 52-name
baseline (`docs/parity/torture-failset-current.txt`) + SMAUG soak.
Reducers live in tmp/ (realios/p2/c9/c11/d1-d3/v1-v6/m1-m4) — rerun with
NO flags (default mode is the point).

**Branch:** `feature/retire-embedded-shims-claude` (off `develop` @ `2832fc0`)
**Governing doc:** `madc-header-partition-handoff.md` (the partition model).
**User mandate (2026-06-12):** delete every hand-rolled glibc/libstdc++ twin in
`include/madc/`; real system headers are the only stdlib surface. The shims'
continued existence keeps re-seeding the false belief that they are needed.
Prerequisite for everything downstream (PCH/forest, C23/C++23 compliance).

## End state (the partition model)

- `include/madc/` keeps ONLY: bucket-1/2 compiler-freestanding headers
  (`stddef.h`, `stdarg.h`, `stdbool.h`, `stdint.h`, `limits.h`, `float.h`,
  `alloca.h`→delete if glibc's parses) + the madc-owned `ns_*` headers.
- ALL glibc (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `sys/*`, …) and ALL
  libstdc++ (`<iostream>`, `<string>`, `<vector>`, …) resolve to the REAL
  system files, in every mode including default `STD_MADC`.
- No stdio-backed ostream bodies anywhere; cout/cerr/clog are the real
  libstdc++ objects bound mangled-direct (already proven on the
  `--std=c++17 --no-embedded-headers` path, g++-byte-identical).

## User ruling (2026-06-12, in-session)

**K&R-era recovery (old-style parameter declarations + file-scope implicit-int
definitions) is admitted ONLY under explicit `--std=c##` with ## < 23.** Never
in `STD_MADC`, never in C++ modes. Implemented as `Program::knr_supported()`
(`include/madc.h`). Consequences:
- `scripts/run_gcc_testsuite.py` now passes `--std=c17` by default (torture is
  C-era code; override with `--std=`).
- The 6 torture-derived `tests/*.mad` with implicit-int `main` got
  `--std=c17` `.flags` fixtures.
- SMAUG TUs carry no `-std`; if SMAUG turns out to need K&R, the fix is
  `-std=` in the MadSMAUG manifest generator, not a madc gate.

## Phases (each gated: build clean → fulltest → torture failset diff vs
`docs/parity/torture-failset-current.txt` → SMAUG soak → commit)

- **Phase 0 — dialect gates.** `knr_supported()` + gate the two K&R recovery
  entry points (`is_old_style_parameter_head`, the implicit-int file-scope
  recovery). DONE (this session); was the only wall between simulated
  `STD_MADC` and a running real `<iostream>`.
- **Phase 1 — STD_MADC consumes real C++ headers.** Seed
  `__cplusplus`/`__GNUG__` in `STD_MADC` too (lexer ~1491; predicate becomes
  "madc dialect or explicit C++"). `cplusplus_value_for_std()` already floors
  at `201703L`. Re-probe; fix any remaining dialect-gate divergences at depth.
- **Phase 2 — retire the libstdc++ shims.** Delete `include/madc/{iostream,
  fstream,sstream,string,vector,map,set,algorithm,typeinfo}`; includes fall
  through to the real headers (the resolver already does this when the
  embedded lookup misses). Clean up `mark_embedded_include_flag`/lazy_map
  entries that referenced them. Fix walls at the deepest layer; fulltest gate.
  Expect per-test compile cost (~1.2s real `<iostream>` vs 0.03s shim; PCH is
  the later forest-track fix — use `.timeout` fixtures only where genuinely
  needed).
- **Phase 3 — retire the glibc shims.** Delete the C twins (`stdio.h`,
  `stdlib.h`, `string.h`, `unistd.h`, `time.h`, `math.h`, `sys/*`, net/dns,
  `pthread.h`, …). C-mode real-header parsing is already proven (SMAUG, C89
  audit). Same gates.
- **Phase 4 — bucket-1/2 conformance + cleanup.** Convert the survivors to
  the `$OWN`-derived set (`gcc -print-file-name=include`), `#include_next`
  shims where GCC layers (stdint/limits/float), shrink
  `gen_embedded_headers.sh`, drop dead lazy-registration arms, sync docs/KG/
  status mirrors, record the GCC version pin.

## Verification oracles

- `tests/` fulltest (581/0/0/18 baseline) + both check gates.
- Torture failset names diff (53-name baseline; now run under `--std=c17`).
- SMAUG soak: `cd /workspace/MadSMAUG/runtime/area; timeout 50
  /workspace/madc/bin/madc --project ../../compile_commands.json -lcrypt` —
  exit 124 + "Realms of Despair ready at".
- Real-header parity: `--no-embedded-headers` output == g++ on the same
  source (`feedback_emitc_gcc_parity_oracle`).
