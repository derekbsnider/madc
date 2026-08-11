# Build Rules

- Always use `make -C src` to build (not `make` from root)
- Use `make -C src clean` then `make -C src` for a full rebuild
- The dev binary lands at `bin/madc` (-O0); `make -C src release` produces
  the stripped, forest-packed `bin/madc-release` (-O2); `make -C src debug`
  produces `bin/madc-debug`. Per-mode names — they never clobber each other.
- Run the packed suite with `MADC_BIN=bin/madc-release bash scripts/run_tests.sh`
- All object files go to `obj/` (per-mode subtrees)
- When work is focused on core madc / libmadc / parser / compiler code and does not touch `madcdat` or shared storage-facing surfaces, prefer `./configure --enable-madcdat=no` first to shrink rebuild and unit-test scope.
- Re-enable `madcdat` before final validation when the task touches `madcdat`, shared public headers, build wiring, or any surface that may affect storage/federation code.

## Backend

- The backend is CIR → c2mir → MIR. asmjit was removed.
- MIR (libmir + c2mir) lives IN this repository at `third_party/mir` —
  a subtree import of the former fork, maintained as ordinary madc
  source: edit and commit it like any file. No pin, no separate push,
  no branch lockstep, no fork release.
- It carries native C99 `_Complex`, `__attribute__((cleanup))`,
  ≤16-byte SIMD/vector (`vector_size`/`ext_vector_type`) support, the
  Mach-O executable writer, and ABI/codegen fixes the CIR backend
  depends on.
- libmir build products land under `obj/mir/<variant>` — NEVER inside
  `third_party/mir`. `make -C src` builds the host libmir + c2m itself;
  `make -C src mirclean` removes MIR build products (`clean` does not).
- `vnmakarov/mir` is the true upstream. Generic MIR/c2mir fixes are
  maintained in-tree AND exported as clean upstream-based PR branches
  through `derekbsnider/mir` (historical + PR-transport only; never
  sync its `master`/`develop` with `third_party/mir`).
- Upstream sync is deliberate, never automated — subtree pull without
  `--squash`, then MIR tests + the full madc battery.
- See `docs/plans/mir-into-madc-repo-2026-08-11.md` (§15–§20) for the
  workflows; `derekbsnider/mir` tag `madc-pre-subtree-migration` marks
  the final standalone state.
