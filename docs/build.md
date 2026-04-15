# Building madc

## Requirements

- `g++` with C++11 support
- asmjit v1.14, installed at `/usr/local/` (built from source)
  - Headers: `/usr/local/include/asmjit/`
  - Library: `/usr/local/lib/libasmjit.so`

> **Note:** The apt package `libasmjit-dev` installs an older, incompatible version at
> `/lib/x86_64-linux-gnu/`. The Makefile explicitly prefers `/usr/local/lib` via
> `-L/usr/local/lib -Wl,-rpath,/usr/local/lib`. Do not remove those flags.

## Build

```bash
make -C src           # build bin/madc
make -C src clean     # remove object files
```

## Output

- Binary: `bin/madc`
- Object files: `obj/*.o`

## Running a Program

```bash
bin/madc tests/testint.mad
```

Or make the file executable (it has the shebang `#!/../bin/madc`):
```bash
chmod +x tests/testint.mad
tests/testint.mad
```

## Debug Output

Debug output is currently always on (`#define DBG(x) x`). This produces verbose
tokenization, parse, and compile traces on stdout before execution output. This
is a known limitation — see the TODO file.
