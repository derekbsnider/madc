# madc
## Mad-C Programming Language (My jit-Assembled Dialect of C)

---

This is my answer to a simple C-like language to use stand alone, or embedded, which is JIT assembled/compiled.

My goals are:
1. Make it fast
2. Keep it small
3. Make it easy to use

This project depends on the `asmjit` library. When using Codex, the
`.codex/setup.sh` script will install the required development package
(`libasmjit-dev`) so the build can succeed without manual steps.
