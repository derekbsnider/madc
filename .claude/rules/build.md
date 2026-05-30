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
  **madc MIR fork** (github.com/derekbsnider/mir, branch
  `feature/complex-support`) — NOT upstream MIR. It carries native C99
  `_Complex` support and c2mir fixes the CIR backend depends on. Push fork
  changes there before relying on them in a madc release.
