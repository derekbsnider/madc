# Build Rules

- Always use `make -C src` to build (not `make` from root)
- Use `make -C src clean` then `make -C src` for a full rebuild
- The binary lands at `bin/madc`
- All object files go to `obj/`

## asmjit Library

Two versions of asmjit are installed on this system:
- `/usr/local/lib/libasmjit.so` — v1.14, namespace `asmjit::v1_14::` (CORRECT one)
- `/lib/x86_64-linux-gnu/libasmjit.so` — old package version (DO NOT USE)

The Makefile uses `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` to ensure the correct version.
Never remove these flags from the Makefile.
