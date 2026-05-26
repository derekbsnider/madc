# Design: --backend CLI Flag for Transpiler Pipeline

**Date:** 2026-05-26
**Scope:** Wire existing Gecko+MIR transpiler pipeline into `bin/madc` CLI and test runner
**Prerequisites:** Phase 0-3 transpiler code already built and linked (madc_tokenizer, madc_grammar, madc_sema, madc_emit_c, madc_mir_backend)

## Problem

The transpiler pipeline (Gecko GLR parse, semantic analysis, C11 emission,
c2mir/MIR execution) exists as library functions but has no CLI entry point.
Tests can only be run via unit tests, not the integration test suite. This
blocks Phase 4+ development — we need the ability to run any `.mad` file
through the transpiler and compare output against legacy.

## Design

### CLI changes (`src/madc.cpp`)

Add two new flags:

- `--backend=mir` — route through the transpiler pipeline instead of
  asmjit. Default remains `asmjit` during the transition period.
- `--emit-c` — dump the generated C11 to stdout and exit (debugging
  tool, implies `--backend=mir` for the parse+sema+emit steps).

Both paths share the same tokenize step (`prog->tokenize()`). The fork
happens after tokenization:

```
tokenize(file)
  |
  +--[asmjit]--> parse() --> compile() --> execute()
  |
  +--[mir]-----> madc_gecko_parse(tokens)
                   --> madc_sema_collect(ast)
                   --> madc_emit_c(ast, sema)
                   --> madc_mir_execute(c_source)
```

For `--emit-c`, stop after `madc_emit_c()` and print the C11 text.

### Argument parsing

Follow the existing pattern in `madc.cpp` (sequential `strcmp` checks).
Add:
- `--backend=mir` or `--backend=asmjit` (string compare after `=`)
- `--emit-c` (boolean flag)

No short flags for either — these are development/transition tools.

### Test runner changes (`scripts/run_tests.sh`)

Add `--backend=mir` passthrough:
- When `--backend=mir` is passed to `run_tests.sh`, prepend
  `--backend=mir` to the madc invocation for every test.
- Output format stays the same (pass/fail/timeout counts).
- Tests use the same `.expect` fixtures — the transpiler must produce
  identical output to pass.

### AOT/object flags

`--emit-object` and `--emit-executable` remain asmjit-only for now.
If `--backend=mir` is combined with these, print an error and exit.
MIR AOT output is a later phase.

### Extern declarations needed in `madc.cpp`

```cpp
// From madc_tokenizer.cpp
extern struct gp_tree_node *madc_gecko_parse(std::deque<TokenBase *> *tokens,
                                              int *out_ambiguity);
extern void madc_gecko_free_tree(struct gp_tree_node *root);

// From madc_sema.cpp
extern SemaInfo *madc_sema_collect(struct gp_tree_node *root);
extern void madc_sema_free(SemaInfo *info);

// From madc_emit_c.cpp
extern std::string madc_emit_c(struct gp_tree_node *root, SemaInfo *sema);

// From madc_mir_backend.cpp
extern int madc_mir_execute(const std::string &c_source,
                             const std::string &source_name);
```

### Success criteria

1. `bin/madc --backend=mir tests/testhello.mad` produces the same
   output as `bin/madc tests/testhello.mad`
2. `bin/madc --emit-c tests/testhello.mad` prints valid C11 to stdout
3. `bash scripts/run_tests.sh --backend=mir` runs all 475 tests and
   reports pass/fail counts
4. `make -C src fulltest` (without `--backend=mir`) is unaffected

### Files modified

| File | Change |
|------|--------|
| `src/madc.cpp` | Add `--backend` and `--emit-c` flag parsing, MIR pipeline call path |
| `scripts/run_tests.sh` | Add `--backend=mir` passthrough to madc invocations |

### Files NOT modified

Everything else. No new files created. No changes to the transpiler
pipeline code itself.
