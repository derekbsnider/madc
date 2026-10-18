# C programs bind the pack's C layer — plan (0.106.0 arc, section J)

Owner 2026-10-17: "C is separate, uncompressed, and not on the same list";
"just make sure it doesn't get removed again". Agreed 2026-10-13
(`2026-10-13-pack-load-cost-experiments.md`, Test 3 and "The C question"),
dropped when the per-header slices shipped in 0.105.0. KG Decision
`c_pack_layer_never_dropped`, Gap `c_programs_parse_headers_from_text`.

## The problem

A `.c` compile (gnu17) and every `--std=c*` compile parse their system headers
from text on every run. The pack's one profile is frozen under madc (C++) and
labelled gnu++17 / c++17 only (`scripts/forest_pack_share.sh`), so a C config
misses it: `forest_probe_arm` finds no profile whose config serves the
compile, and `forest_missing_fallback` live-parses. `c_headers.c` costs 245M Ir
against gcc -O0 -c 144M and clang 127M (2026-10-17). The same headers compiled
as C++ cost madc 5x less than either. In the parse-cost gate, `c_headers.forest`
equals `c_headers.live`, and `sqlite3.forest` is in the same position.

## The agreed properties (each one must survive the design)

1. The C headers are frozen a SECOND time, under C (gnu17), as the C
   preprocessor sees them. Under C++, `<stdlib.h>` and `<math.h>` are
   libstdc++'s wrappers, so the C++ freeze's C units are not the C view.
2. It has its own label list, beside the C++ one, in `forest_pack_share.sh`:
   one owner, two lists.
3. Which C standards a gnu17 freeze may serve is MEASURED, not assumed.
   `__STRICT_ANSI__` hides glibc's POSIX names under c17, and
   `__STDC_VERSION__` gates C11 names under c99. A standard whose header view
   differs gets its own freeze, or none.
4. Its blocks are stored raw (uncompressed).
5. The C tables sit ahead of the C++ boundary: the C profile is stacked BELOW
   the C++ one in the same library image.
6. It is built on the per-header slices: a bind loads only the units the
   program's include closure reaches.
7. It lands with a GATE, never without one: `forest_pack_gate.sh` is red
   unless a `.c` compile and a `--std=c17` compile bind the C layer. The
   `c_headers.forest` parse-cost row is re-recorded at the new level, so losing
   the bind is a several-fold red.

## What exists (recon at 87080f042)

- A library already carries a STACK of containers, newest first (footer at
  EOF). Every discovery arm walks it and skips a profile whose config does not
  serve the compile (`forest_probe_arm`, `serves_config`). A gnu17 profile
  stacked under the C++ one is reachable with no reader change; it needs no
  new role, since it is another SYSTEM profile with another config word.
- The freeze path is language-agnostic. A `.c` TU freezes under gnu17, and
  `forest_config_gate.sh` already freezes `consumer.c` and binds it from a `.c`
  compile.
- Compression: `madc_cir.cpp` sets the codec to zstd (level 15) when
  appending. `add_seg` with codec None writes a segment the bind reads in
  place. R4d removed the only whole-container raw switch, which was
  role-keyed (`cir_forest_storage_codec`, the dialect image's).
- Labels: a label equal to the profile's own config is skipped, so a list
  naming the freeze's own standard yields one fewer `shared` line. Labels are
  asserted, not proven.
- Measured view equality: the config-blind `content` digest line of
  `--dump-forest` (`forest_bind_gate.sh` `[reprogate]`).
- `forest_bind_env_ok` does NOT protect a wrong label. Its checks are off by
  default, and a want-undefined / have-defined branch macro is skipped. A label
  must therefore be right by measurement.
- Pitfalls:
  - `ensure_ledger_forest` / `ensure_source_forest` reuse the BOUND forest;
    a C compile bound to the C profile would look for the AOT ledger and the
    source arm in the C profile.
  - The relayout rewrites only the LAST container and refuses one already laid
    out, so the order is: freeze C, lay out C, freeze C++, lay out C++.
  - `forest_pack.sh`'s dump and the `pack-bytes` gate cover only the newest
    container.
  - `forest_pack_layout.sh` hard-codes `--std=madc` and `.mad` roots.

## Rulings

- The C roots list starts as the C spellings of the C++ list's C roots, plus
  madc's own C headers. This is the shape of Test 3 (24 headers, 214 units,
  4.3 MB raw), the shape the owner ruled on. Like the C++ list, it is a
  speed-up list, never the coverage boundary: the source arm serves every
  system header as text. It grows deliberately, and every root must freeze
  quietly under gnu17. Cost if wrong: a C program that includes a header off
  the list parses that header from text, as today.
- The raw switch is a CODEC choice on the freeze, `--freeze-codec=none`,
  never a role. A role names what a container is, and the C layer is a SYSTEM
  profile.
- The ledger and source-arm lookups find the profile that CARRIES them,
  rather than reusing the bound one. The C profile does not duplicate them.
- KG: Decision `shared_pack_profile_configs` (one C++ profile, 2026-10-11)
  ruled out per-config C++ profiles. The C layer is the owner's later, explicit
  ruling (10-17) and does not contradict it. Both nodes say so.

## Slices

- **J1 — raw containers.**
  - `--freeze-codec=none|zstd` (default as today: zstd when appending, none
    otherwise). A none container is written with no codec: no level and no
    compressed intern spine.
  - `--dump-forest` reports `compressed=0` for it, and it holds 0 zstd frames.
  - Unit test: a raw container stacked under a compressed one; both bind, and
    the dump reports each one's codec.
- **J2 — the C freeze.**
  - `scripts/forest_pack_c_headers.txt` (v1, canonical order).
  - `FOREST_SHARED_C_STDS` in `forest_pack_share.sh`; `forest_share_check`
    per list.
  - `forest_pack.sh`: the C TU (`tmp/forest_pack_c_tu.c`) is frozen first,
    raw, and laid out (`forest_pack_layout.sh` takes the standard and the
    roots' extension). The C++ profile is then frozen on top, as today.
  - Measurement decides the labels and freezes: freeze the C TU under gnu17,
    gnu11, gnu99, c17, c11 and c99, and compare the `content` digests.
    - Standards equal to gnu17's are labels.
    - c17 must bind (the owner's gate). If its view differs, c17 gets its own
      raw freeze, with the standards equal to it as its labels.
    - Record the table in this plan.
- **J3 — one owner for the ledger and the source arm.**
  `ensure_ledger_forest` / `ensure_source_forest` walk the stack for the
  profile that carries the segment. Reducer: a `.c` compile with a header
  served from the source arm, and a `.c` AOT build.
- **J4 — the gate.**
  - A `forest_pack_gate.sh` leg, run by `forest_pack.sh`: a `.c` compile and a
    `--std=c17` compile on the packed binary, red unless each reports bound
    units ("bound to grove unit" / `--show-stats` units bound > 0).
  - A dump check: a C profile with `compressed=0`.
  - `--selftest` legs: bound is GREEN; mismatch, no log, or compressed is RED.
  - A `c-pack-bytes` baseline line.
  - The parse-cost `c_headers.forest` row (and `sqlite3.forest`) re-recorded
    at the new level.
- **J5 — platform arms.** The win64 and darwin pack scripts freeze their own C
  lists (mingw/UCRT's C set; darwin's libc). They are built on the container
  and run at the seam.
- **J6 — docs.** `docs/test-status.md`, ROADMAP row 8.12J, the KG nodes.

## Targets

- `c_headers.forest` from 243M Ir toward the C++ ratio, and below gcc's 144M.
- A `.c` `<stdio.h>` program binds; the 8 everyday C headers in about 24 ms
  against 159 ms live (Test 3's C-only raw arm).
- Pack size: the C layer adds about 4 to 5 MB raw (Test 3's 4.3 MB for 24
  headers). The owner ruled raw.
- C++ and dialect rows flat.

## Thread safety

The pack is immutable once built. A bind reads a profile through the
per-Program `ForestSide`, so no new shared mutable state is added. A raw
container is read in place, also immutable.

## Validation

Per slice: Tier 1 + Tier 2, the static gate loop, the touched unit binaries,
the packed release's `forest_pack_gate` and parse-cost. The batch lane per
batch. The seam is the 0.106.0 release boundary.
