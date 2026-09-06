# madc Architecture

## Overview

madc is a C/C++ dialect compiler built around **one source-preserving
intermediate representation**. Source text flows through four phases and can
then be executed in-process, emitted as a native artifact, or rendered back
to C source:

```
Source (.mad / .c / .cpp / .h)
    |
    v src/lexer.cpp        — tokenize into a flat token stream
    |                        (#include, import, #load, preprocessor directives)
    |
    v src/parser.cpp       — parse tokens into the TokenBase AST;
    |                        namespace / struct / class / template resolution
    |
    v src/cir_builder.cpp  — lower the AST to a cir_node tree (MC11-IR),
    |                        a c2mir-compatible C11 AST
    |
    v c2mir → MIR          — the pinned madc MIR fork checks the tree and
    |                        generates MIR
    |
    +--> JIT execute        — MIR_gen in-process (the default)
    +--> native output      — -c / -o / -shared: MIR assembles ELF and
    |                         Mach-O objects and executables directly
    +--> C source           — --emit=c11 / --emit=mc11 render the tree
```

There is no bytecode interpreter and no second codegen path; the original
asmjit x86-64 JIT and the Gecko parser experiment were both removed (see
`docs/adr/0001-cir-c2mir-backend.md` — the decision is settled).

## MC11-IR: the cir_node tree

The primary in-memory representation is the **Mad-enhanced-C11 IR**:

- `cir_node` **derives from c2mir's `node_t`** — c2mir consumes the lowered
  C11 view directly; there is no translation step between madc's IR and the
  backend's input.
- Every `cir_node` also **carries its originating lexed tokens and the
  `TokenBase` parse subtree that built it**, including file/line/column — so
  madc retains the high-level structure (classes, templates, namespaces)
  without reconstruction.
- The IR is deliberately **both** lowered and high-level. c2mir sees classes
  as structs + functions and templates already instantiated; madc's render
  targets (C11, MC11, C++, madc) read the retained token/parse view, selected
  via the same language enum that drives `--std=`.

See `docs/rules/mc11-ir.md` for the full definition and the reasoning.

## Source file map

| File | Role |
|------|------|
| `src/madc.cpp` | CLI entry point: flags, mode selection, resource guards |
| `src/lexer.cpp` | Tokenizer; `#include`, the `import` directive (and the low-level `#load`), preprocessor handling |
| `src/parser.cpp` | AST construction; overload resolution, template capture and instantiation (parse-once tsubst), namespace and class semantics |
| `src/cir_builder.cpp` | AST → `cir_node` lowering; construction/destruction assembly, vtables, exceptions |
| `src/cir_emit_c.cpp` | `--emit=c11` / `--emit=mc11` source renderers |
| `src/cir_freeze.cpp` | Forest freeze/thaw: serializing parsed state (the save/load-state machinery behind the packed release binary) |
| `src/madc_cir.cpp` | CIR pipeline driver (translate/execute entries) |
| `src/madc_mir_backend.cpp` | c2mir/MIR integration; runtime support wrappers resolved at MIR link time |
| `src/madc_project.cpp` | `--project compile_commands.json` multi-TU driver |
| `src/madc_mangle.cpp` | Itanium name mangling (both stdlib flavors) |
| `src/madc_api.cpp`, `src/madc_c_api.cpp` | `libmadc` embedding APIs (C++ first, C shim over it) |
| `src/embedded_headers.cpp` | Headers from `include/madc/` baked into the binary |
| `src/madc_value.cpp` | `MadValue` / `MadArray` — the mixed-type array runtime |
| `src/ns_php.cpp`, `ns_perl.cpp`, `ns_python.cpp`, `ns_ruby.cpp`, `ns_js.cpp`, `ns_rust.cpp`, `ns_madc.cpp`, `ns_common.cpp` | The multi-language utility namespaces |
| `src/madcdat_storage*.cpp` | Optional madcdat storage drivers (BDB/GDBM/QDBM/SQLite) |
| `src/madcdis_snapshot.cpp` | madcdis data-substrate snapshot support |
| `src/pch.cpp`, `src/precompiled_headers.cpp` | Precompiled-header support |

Key headers: `include/madc.h` (`Program`, `FuncDef`, `Variable`, parse state),
`include/datadef.h` (the `DataDef` type hierarchy and `DataType` enum),
`include/tokens.h` / `include/datatokens.h` (the `TokenBase` AST classes),
`include/madc_api.h` (the public embedding API), `include/madc/` (embedded
standard/POSIX headers).

## Front end

- **Tokens and AST.** All AST nodes inherit from `TokenBase`. The parser is
  hand-written recursive descent over the token stream, with a small pushback
  queue for speculative reads.
- **Templates are parse-once.** A template's pattern is parsed one time; each
  instantiation re-runs the generic resolver (tsubst) over the saved tree on
  substituted arguments — the g++ model. Source text is never re-lexed to
  instantiate (see `.claude/rules/parse-once.md`).
- **Balanced delimiters have one owner.** All `(` `[` `{` `<` tracking goes
  through `DelimDepth` (`delim_scan_step` / `delimStepStream`) — including the
  template-argument `<` disambiguation (see
  `.claude/rules/delimiter-tracking.md`).
- **Namespaces.** Resolution walks current namespace → `using` imports →
  global. The php/perl/python/ruby/js/rust namespaces are ordinary C++
  libraries registered under those names; `prefer` /
  `#pragma prefer` select precedence between them.
- **Real headers.** `#include <...>` reaches the real installed headers.
  madc presents as GCC to the preprocessor and supports both stdlib flavors:
  libstdc++ (default) and libc++ via `-stdlib=libc++`.

## Lowering highlights

All C++ features lower to C11 constructs in the `cir_node` tree (the Cfront
model); the notable shapes:

- **Classes** → struct layout per the **Itanium C++ ABI**: vtable groups
  (including transitive secondary vtables), virtual bases with dynamic
  offsets, RTTI, `dynamic_cast`, thunks. Methods take a hidden `__this`
  first parameter.
- **`std::string`, streams, containers** are **real standard-library
  objects** — member calls resolve *mangled-direct* against the live
  libstdc++ or libc++ (Itanium symbols), not through wrapper shims.
  Templates that must exist in madc's TU (e.g. container instantiations)
  monomorphize from the real header patterns.
- **Exceptions** → `setjmp`/`longjmp` frames with type-dispatched catch;
  RAII cleanup rides the fork's `__attribute__((cleanup))` support.
- **Multiple return values** → a hidden `__retbuf` out-parameter.
- **References** → pointers; **lambdas** → hoisted functions;
  **VLAs** → `alloca`; the **ternary operator** → a C11 conditional
  expression (c2mir owns branch codegen).
- **Script mode**: top-level statements are collected into a synthesized
  `main()` under the default dialect.

## Backend and outputs

The backend (`libmir` + `c2mir`) lives in-tree at `third_party/mir` —
a subtree of the former madc MIR fork, maintained as ordinary madc
source. It carries native `_Complex`, `__attribute__((cleanup))`,
≤16-byte vector support, the Mach-O executable writer, and ABI/codegen
fixes the CIR path depends on.

One tree serves every output:

- **JIT** — `MIR_gen` in-process execution (the default run mode).
- **Native artifacts** — `-c` (relocatable object), `-o` (executable),
  `-shared` (ET_DYN): MIR assembles ELF directly, with no external
  compiler, assembler, or linker; Mach-O objects are supported for darwin
  targets. `--project` drives multi-TU builds from `compile_commands.json`.
- **C source** — `--emit=c11` renders portable C11 for any C toolchain;
  `--emit=mc11` renders the Mad-enhanced serialization.
- **Forest** — parsed state can be frozen (`cir_freeze.cpp`) and packed into
  the release binary or a sidecar, so common headers load as state instead
  of being re-parsed.

## Runtime interoperability

- Undeclared C functions fall back to `dlsym(RTLD_DEFAULT)` resolution with
  variadic calling; embedded headers declare real return types where it
  matters (see `.claude/rules/embedded-headers.md`).
- `import name [as ns];` binds a module — its interface (an embedded header)
  and its library, spelled for the TARGET by the module map
  (`src/madc_modules.cpp`, the one owner of `.so` / `.dylib` / `.dll`): the
  JIT opens it `RTLD_GLOBAL`; a native artifact gets a `DT_NEEDED` / load
  command / PE import; alias-form members (`ns::f(...)`) resolve at run time
  through `__madc_dl_member` in every lane. `-lfoo` is the build-line
  spelling of the same binding; `#load "<file>" as ns;` the low-level
  verbatim-file one (tooling / fixtures). See `docs/language/import.md`.
- C++ symbols resolve mangled-direct (Itanium) against whichever stdlib
  flavor is active; `extern "C"` exports exist only as the C-host API of
  `libmadc` and for compiler-emitted runtime machinery.

## Error handling and diagnostics

- Parse/compile errors flow through the `throwit` / `throwstream` mechanism:
  message with `file:line:col` and a source caret, printed to stderr, then
  thrown. Error output is never gated behind verbosity.
- `-v` / `--verbose` enables the `DBG()` trace (tokenizer, parse steps,
  lowering decisions). `--show-stats` prints opt-in compile metrics.

## Where to read more

- `AGENTS.md` — the canonical contributor briefing (rules index included)
- `docs/adr/0001-cir-c2mir-backend.md` — the backend decision record
- `docs/rules/` — the reasoning behind each project rule
- `docs/plans/madc-vision-and-invariants.md` — the long-term direction and
  invariants I1–I8
