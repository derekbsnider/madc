# Build Rules

- Always use `make -C src` to build (not `make` from root)
- Use `make -C src clean` then `make -C src` for a full rebuild
- The binary lands at `bin/madc`
- All object files go to `obj/`
- When work is focused on core madc / libmadc / parser / compiler code and does not touch `madcdat` or shared storage-facing surfaces, prefer `./configure --enable-madcdat=no` first to shrink rebuild and unit-test scope.
- Re-enable `madcdat` before final validation when the task touches `madcdat`, shared public headers, build wiring, or any surface that may affect storage/federation code.

## Backend

- The backend is CIR → c2mir → MIR. asmjit was removed.
- The MIR library (libmir + c2mir) lives at `/workspace/mir` and is the
  **madc MIR fork** (github.com/derekbsnider/mir, branch **`develop`**, pinned at
  the commit in the repo-root `MIR_COMMIT` file — currently `1fdf44d`) — NOT
  upstream MIR. It carries native C99 `_Complex`, `__attribute__((cleanup))`, and
  ABI/codegen fixes the CIR backend depends on.
- **Branch correspondence:** the fork's `develop` tracks madc's `develop`; once
  madc reaches master parity, the fork's `master` tracks madc's `master`. A MIR
  *feature* branch is cut only when a madc feature actually needs new MIR work,
  and merges to MIR `develop` in lockstep with the madc feature merging to develop.
- **Pin discipline:** when madc starts depending on new fork commits, merge them
  to MIR `develop`, push, and bump `MIR_COMMIT` in the SAME madc commit. Never let
  madc depend on un-pushed or unpinned fork work.
