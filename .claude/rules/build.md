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
- The MIR library (libmir + c2mir) lives at `/workspace/mir` and is the
  **madc MIR fork** (github.com/derekbsnider/mir, branch **`develop`**, pinned at
  the commit in the repo-root `MIR_COMMIT` file — that file is the single
  source of truth for the pin; never restate its value elsewhere) — NOT
  upstream MIR. It carries native C99 `_Complex`, `__attribute__((cleanup))`,
  ≤16-byte SIMD/vector (`vector_size`/`ext_vector_type`) support, and
  ABI/codegen fixes the CIR backend depends on.
- **Branch correspondence:** the fork's `develop` tracks madc's `develop`; the
  fork's `master` tracks madc's `master` (started at the v0.38.0 promotion). A MIR
  *feature* branch is cut only when a madc feature actually needs new MIR work,
  and merges to MIR `develop` in lockstep with the madc feature merging to develop.
- **Pin discipline:** when madc starts depending on new fork commits, merge them
  to MIR `develop`, push, and bump `MIR_COMMIT` in the SAME madc commit. Never let
  madc depend on un-pushed or unpinned fork work.
- **Fork releases:** versioned `<upstream-base>-madc.<madc-version>`
  (e.g. `1.0-madc.0.38.0`) — upstream base = the newest upstream MIR release
  merged into the fork; suffix = the full madc release version that depends
  on it. The repo-root `MIR_VERSION` file holds the fork release madc
  depends on (`MIR_COMMIT` stays the machine pin; never restate either
  value elsewhere).
- **Release cadence:** when a madc release ships fork changes, release BOTH:
  bump `MIR_VERSION` in the release commit and push an annotated tag
  `v<MIR_VERSION>` on the fork at the commit `MIR_COMMIT` pins. If the fork
  is unchanged since the previous release, `MIR_VERSION` stays put and no
  new fork tag is cut.
- The upstream-base component bumps only when a newer upstream MIR release
  is merged into the fork.
- Historical: `madc-v0.38.0` on the fork is the superseded first-format
  pairing tag (same commit as `v1.0-madc.0.38.0`; kept, do not reuse the
  naming).
