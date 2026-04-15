# madc — Mad-C Programming Language

**My jit-Assembled Dialect of C** — a C-like scripting language that JIT-compiles directly to x86-64 machine code using [asmjit](https://asmjit.com/). No bytecode, no interpreter, no separate compilation step.

---

## Goals

1. **Fast** — register-first execution model; scalars live in virtual registers by default
2. **Small** — single binary, no runtime VM, no GC
3. **Easy to use** — familiar C syntax, scripting-friendly (shebang support)

---

## Quick Start

```bash
# Build
make -C src

# Run a program
bin/madc tests/testint.mad

# Run with debug trace
bin/madc -v tests/testint.mad

# Run as a script (files start with #!/.../bin/madc)
chmod +x tests/testint.mad
tests/testint.mad
```

---

## Language Features

- **Data types:** `int8_t`–`int64_t`, `uint8_t`–`uint64_t`, `float`, `double`, `char`, `string`, `stringstream`
- **Control flow:** `if`/`else`, `for`, `while`, `do`/`while`
- **Functions:** user-defined functions with return values and arguments
- **`register` keyword:** explicitly marks a variable as register-only (never written to memory)
- **Structs:** `struct teststruct` (user-defined structs planned for Phase 2)
- **Output:** `cout << x << endl`, `puts()`, `putchar()`, `puti()`, `printf()`
- **Operators:** arithmetic, bitwise, comparison, increment/decrement, `<<` stream operator

### Example

```c
#!/usr/bin/env madc

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

register int i = 0;
for (i = 1; i <= 10; i++) {
    cout << i << ": " << factorial(i) << endl;
}
```

---

## Building

Requires:
- `g++` with C++11 support
- asmjit v1.14 installed at `/usr/local/` (see [`docs/build.md`](docs/build.md))

```bash
make -C src           # build bin/madc
make -C src clean     # clean objects
make -C src test      # run unit tests
```

See [`docs/build.md`](docs/build.md) for full setup instructions including asmjit installation.

---

## Testing

```bash
# Run all integration tests
for t in tests/*.mad; do
    out=$(timeout 5 bin/madc "$t" 2>/dev/null); ec=$?
    [ $ec -eq 0 ] && echo "PASS: $t" || echo "FAIL($ec): $t"
done

# Run unit tests
make -C src test
```

**Current status: 24/24 integration tests pass. 25/25 unit tests pass.**

See [`docs/testing.md`](docs/testing.md) and [`docs/test-status.md`](docs/test-status.md) for details.

---

## Documentation

| Doc | Contents |
|-----|----------|
| [`docs/build.md`](docs/build.md) | Build requirements, asmjit setup |
| [`docs/usage.md`](docs/usage.md) | Language reference, CLI flags |
| [`docs/architecture.md`](docs/architecture.md) | Compiler internals, token hierarchy |
| [`docs/testing.md`](docs/testing.md) | Integration and unit test guide |
| [`docs/test-status.md`](docs/test-status.md) | Per-test results and known issues |
| [`docs/plans/revival-plan.md`](docs/plans/revival-plan.md) | Development roadmap (Phases 1–4) |
| [`CHANGELOG.md`](CHANGELOG.md) | Change history |

---

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| **Phase 1** | Foundation: verbose flag, char literals, struct fix, `register` keyword, doctest | **Complete** |
| **Phase 2** | User-defined structs, classes, `std::` namespace, `#include` | Planned |
| **Phase 3** | `php::` namespace, dlopen fallback, first-class `dlopen`/`dlsym` | Planned |
| **Phase 4** | `libmadc.so` embedding API | Planned |

---

## Architecture

```
Source (.mad file)
    │
    ▼ src/lexer.cpp     — tokenize source
    │
    ▼ src/parser.cpp    — build AST (tree of Token* objects)
    │
    ▼ src/compiler.cpp  — walk AST, emit x86-64 via asmjit
    │
    ▼ JIT execute        — run machine code in-process
```
