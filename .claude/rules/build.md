# Build Rules

- Always use `make -C src` to build (not `make` from root)
- Use `make -C src clean` then `make -C src` for a full rebuild
- The binary lands at `bin/madc`
- All object files go to `obj/`
- When work is focused on core madc / libmadc / parser / compiler code and does not touch `madcdat` or shared storage-facing surfaces, prefer `./configure --enable-madcdat=no` first to shrink rebuild and unit-test scope.
- Re-enable `madcdat` before final validation when the task touches `madcdat`, shared public headers, build wiring, or any surface that may affect storage/federation code.

## asmjit Library

Two versions of asmjit are installed on this system:
- `/usr/local/lib/libasmjit.so` — v1.14, namespace `asmjit::v1_14::` (CORRECT one)
- `/lib/x86_64-linux-gnu/libasmjit.so` — old package version (DO NOT USE)

The Makefile uses `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` to ensure the correct version.
Never remove these flags from the Makefile.
