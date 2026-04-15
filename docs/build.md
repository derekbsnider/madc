# Building madc

## Requirements

- `g++` with C++11 support
- asmjit v1.14, installed at `/usr/local/` (built from source)
  - Headers: `/usr/local/include/asmjit/`
  - Library: `/usr/local/lib/libasmjit.so`

> **Note:** The apt package `libasmjit-dev` installs an older, incompatible version at
> `/lib/x86_64-linux-gnu/`. The Makefile explicitly prefers `/usr/local/lib` via
> `-L/usr/local/lib -Wl,-rpath,/usr/local/lib`. Do not remove those flags.

## Installing asmjit v1.14 from Source

```bash
git clone https://github.com/asmjit/asmjit.git
cd asmjit
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
```

Verify the right version is used:
```bash
pkg-config --modversion asmjit    # should show 1.14.x from /usr/local
```

## Build

```bash
make -C src           # build bin/madc
make -C src clean     # remove object files and compiled test binaries
```

## Output

- Binary: `bin/madc`
- Object files: `obj/*.o`

## Unit Tests

```bash
make -C src test      # build and run all tests/unit/*.cpp
```

Test binaries are written to `tests/unit/` and cleaned by `make -C src clean`.

## Running a Program

```bash
bin/madc tests/testint.mad
```

With verbose debug output (tokenizer, parser, JIT compiler traces):
```bash
bin/madc -v tests/testint.mad
```

Or make the file executable (it has the shebang `#!/../bin/madc`):
```bash
chmod +x tests/testint.mad
tests/testint.mad
```

## Compiler Flags

The Makefile compiles with `-std=c++11 -Wall`. Key flags:

| Flag | Purpose |
|------|---------|
| `-I/usr/local/include` | asmjit headers |
| `-L/usr/local/lib` | asmjit library (v1.14, NOT apt version) |
| `-Wl,-rpath,/usr/local/lib` | ensure correct `.so` is loaded at runtime |
| `-lasmjit` | link asmjit |
| `-ldl` | `dlopen`/`dlsym` support |
