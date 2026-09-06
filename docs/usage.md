# madc Usage — the CLI

```bash
madc [options] <source> [program-args...]
```

madc compiles C/C++-dialect source and, by default, JIT-executes it in
process. The same invocation surface emits native objects, executables and
shared libraries, renders C11 source, and drives multi-file projects. This
page covers the command line; the language itself is documented under
[`docs/language/`](language/overview.md).

Run `madc --help` for the complete, always-current option list.

## Running programs

```bash
madc file.mad                 # compile + JIT-run
madc file.mad arg1 arg2       # program args follow the source
madc -E file.mad              # preprocess only
```

A file whose first line is `#!/usr/bin/env madc` can be `chmod +x`'d and run
directly. Top-level statements need no `main()` under the default dialect —
madc synthesizes one (script mode).

Standard library availability needs no setup: **everything in libc and the
active C++ standard library — libstdc++ by default, libc++ under
`-stdlib=libc++` — is available**. Include the normal headers and call; madc
parses the real installed headers and resolves symbols against the real
libraries. There is no curated "built-in function" list to learn.

```c
#!/usr/bin/env madc
#include <iostream>
#include <cmath>
using namespace std;

cout << "hypot(3,4) = " << hypot(3.0, 4.0) << endl;
```

## Language and preprocessor options

| Option | Effect |
|--------|--------|
| `--std=<std>` | `c89`…`c23`, `c++NN`, or `madc` (the default dialect; C++ keywords reserved). A `.c` TU under `--project` defaults to C mode. |
| `-stdlib=libstdc++`\|`libc++` | C++ standard-library flavor (clang's spelling). Replaces the C++ include search list, as clang does. |
| `-D<name>[=value]` | define a macro |
| `-I<dir>` | add an include search directory |
| `-l<name>` | JIT: `dlopen` `lib<name>.so` globally so its symbols resolve. AOT: becomes a `DT_NEEDED` dependency. |
| `--no-embedded-headers` | disable the baked-in headers; use real system headers only |
| `--no-auto-load` | ignore `#load` directives; link explicitly via `-l` |

## Multi-file projects

Projects use the standard `compile_commands.json` (from CMake, `bear`, …):

```bash
madc --project compile_commands.json          # compile all TUs, link, run
madc --project compile_commands.json -o app   # emit one native executable
madc file.json                                # .json input implies --project
```

madc never writes cache files beside your project. To skip recompiling on
later runs, produce a real artifact explicitly: `-c` per-TU objects (madc
runs `.o` files directly as a precompiled cache) or `-o` an AOT executable.

## Native output (AOT)

madc emits native artifacts itself — MIR assembles the ELF/Mach-O image
directly, with no external compiler, assembler, or linker:

```bash
madc -c file.mad                  # relocatable object (file.o)
madc -o prog file.mad             # native executable (PIE by default)
madc -shared -o libx.so x.mad     # shared object
madc -r -o whole.o --project db.json   # single whole-program object (ld -r style)
madc prog.o                       # execute .o files as a precompiled cache
```

- `-static-libmadc` packs the madc runtime pieces the program needs into the
  emitted image, so it runs with no madc library installed (libc/libstdc++
  stay dynamic).
- `-pie` / `-no-pie` select the image layout, gcc-style.
- Emitted executables otherwise locate `libmadc.so` via their `DT_RUNPATH`.

## Rendering and introspection

```bash
madc --emit=c11 file.mad          # render the program as portable C11
madc --emit=mc11 file.mad         # the Mad-enhanced C11 serialization
madc --dump-cir file.mad          # dump the compiler's cir_node tree
madc --dump-source file.mad       # reconstruct full-fidelity source
madc -dM file.mad                 # effective macro table (gcc -dM -E)
```

## Frozen forests (compile once, run without parsing)

Parsed state can be frozen into a container and reused — this is how the
release binary ships its standard headers pre-parsed:

```bash
madc --freeze=prog.frozen prog.mad     # parse + translate, freeze (no run)
madc --run-frozen=prog.frozen          # thaw + run
madc --freeze-run prog.mad             # round-trip in one step
madc --dump-forest=prog.frozen         # inspect a container
```

Grove-backed system `#include`s bind from a discovered container by default
(`--no-forest-bind` forces live parsing). Discovery order: the binary's own
image, the libmadc image, `<exe>.forest` / `<lib>.forest` sidecars,
`$MADC_FOREST`, then the `madc.ini` `forest` key.

## Diagnostics

| Option | Effect |
|--------|--------|
| `-v`, `--verbose` | tokenizer / parser / lowering trace |
| `-g` | debug info: gdb can break, step, and inspect the JIT'd program (forces `-O0`) |
| `-w` | inhibit all warning messages (gcc `-w`); errors are unaffected |
| `--show-stats` | read/lex/parse/c2mir/execute timing and size stats on stderr |
| `-O0` … `-O3` | JIT optimization level |

## Configuration file and environment

Precedence: **CLI > environment > `madc.ini` > defaults.**

The config search takes the first existing file of: `./madc.ini`,
`$XDG_CONFIG_HOME/madc/madc.ini` (default `~/.config/madc/madc.ini`), then
the system config dir. `--config=<file>` names one explicitly;
`--no-config` skips the search (hermetic builds). Recognized keys: `std`,
`stdlib`, `forest`, `include` (repeatable), `cpu-limit`, `mem-limit` — an
unknown key is an error.

| Variable | Effect |
|----------|--------|
| `MADC_CPU_LIMIT=<secs>` | arm an `RLIMIT_CPU` guard (default off — madc runs the program, so no finite default is safe) |
| `MADC_MEM_LIMIT=<MB>` | address-space guard; default 4096 MB + 128 MB per `--project` TU; `0` disables |
| `MADC_FOREST=<file>` | frozen forest container for the discovery chain |

## See also

- [Language overview](language/overview.md) — the madc dialect: types,
  script mode, control flow, and the dialect extensions
- [Supported C++ features](language/cpp-features.md) — what C++ works today
- [Multi-language namespaces](language/overview.md#namespaces) — `php::`,
  `perl::`, `python::`, `ruby::`, `js::`, `rust::`
- [Build and installation](build.md)
