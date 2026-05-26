# --backend CLI Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the existing Gecko+MIR transpiler pipeline into `bin/madc` via `--backend=mir` and add `--backend=mir` passthrough to the test runner.

**Architecture:** Add two flags to `madc.cpp` (`--backend=mir`, `--emit-c`) that fork the execution path after tokenization. The MIR path calls existing functions: `madc_gecko_parse()` → `madc_sema_collect()` → `madc_emit_c()` → `madc_mir_execute()`. The test runner passes `--backend=mir` through to `bin/madc` when requested.

**Tech Stack:** C++11, Gecko (GLR parser), c2mir/MIR (C11 compiler + JIT)

---

### Task 1: Add `--backend` and `--emit-c` flag parsing to `madc.cpp`

**Files:**
- Modify: `src/madc.cpp:395-453` (variable declarations and arg parsing loop)

- [ ] **Step 1: Add variable declarations**

At `src/madc.cpp`, after line 405 (`bool emit_pch = false;`), add:

```cpp
bool emit_c = false;
bool use_mir_backend = false;
```

- [ ] **Step 2: Add flag parsing in the arg loop**

At `src/madc.cpp`, inside the `for` loop, before the final `else` block (line 449), add two new `else if` branches:

```cpp
} else if (strncmp(argv[i], "--backend=", 10) == 0) {
    const char *backend = argv[i] + 10;
    if (strcmp(backend, "mir") == 0)
        use_mir_backend = true;
    else if (strcmp(backend, "asmjit") != 0) {
        std::cerr << "Unknown backend: " << backend
                  << " (use 'mir' or 'asmjit')" << std::endl;
        return 1;
    }
    filearg = i + 1;
} else if (strcmp(argv[i], "--emit-c") == 0) {
    emit_c = true;
    use_mir_backend = true;  // --emit-c implies MIR pipeline
    filearg = i + 1;
```

- [ ] **Step 3: Build and verify parsing works**

Run: `make -C src`
Expected: clean build, no errors.

Then: `bin/madc --backend=invalid 2>&1`
Expected: `Unknown backend: invalid (use 'mir' or 'asmjit')`

Then: `bin/madc --backend=asmjit tests/testhello.mad`
Expected: `Hello, World!` (same as default)

- [ ] **Step 4: Commit**

```bash
git add src/madc.cpp
git commit -m "feat: parse --backend= and --emit-c CLI flags"
```

---

### Task 2: Add MIR pipeline execution path to `madc.cpp`

**Files:**
- Modify: `src/madc.cpp` (add externs near top, add MIR path in main execution block)

- [ ] **Step 1: Add extern declarations and include**

At `src/madc.cpp`, after the existing includes (after line 28 `#include "madc_pch.h"`), add:

```cpp
#include "madc_sema.h"

extern "C" {
struct gp_tree_node;
}

// Transpiler pipeline (Gecko + MIR)
extern struct gp_tree_node *madc_gecko_parse(std::deque<TokenBase *> *tokens,
                                              int *out_ambiguity);
extern void madc_gecko_free_tree(struct gp_tree_node *root);
extern std::string madc_emit_c(struct gp_tree_node *root, SemaInfo *sema);
extern int madc_mir_execute(const std::string &c_source,
                             const std::string &source_name);
```

- [ ] **Step 2: Add AOT+MIR conflict check**

At `src/madc.cpp`, after the `prog->aot_tracking = true;` line (line 456), add:

```cpp
if (use_mir_backend && (emit_object_path || emit_executable_path)) {
    std::cerr << "--backend=mir does not support --emit-object or "
              << "--emit-executable yet" << std::endl;
    return 1;
}
```

- [ ] **Step 3: Add MIR execution path**

At `src/madc.cpp`, in the main execution block (line 517, `if ( argc >= 2 && filearg < argc )`), replace the block with a backend fork after tokenization. The full replacement for lines 517-558:

```cpp
if ( argc >= 2 && filearg < argc )
{
    if ( !(tp=prog->tokenize(argv[filearg])) )
        return 0;

    if ( use_mir_backend )
    {
        // Transpiler pipeline: Gecko parse → sema → emit C → MIR execute
        int ambiguity = 0;
        struct gp_tree_node *ast = madc_gecko_parse(&prog->tokens, &ambiguity);
        if ( !ast )
        {
            std::cerr << "Gecko parse failed for " << argv[filearg] << std::endl;
            return 1;
        }

        SemaInfo *sema = madc_sema_collect(ast);
        std::string c_source = madc_emit_c(ast, sema);

        if ( emit_c )
        {
            std::cout << c_source;
            madc_sema_free(sema);
            madc_gecko_free_tree(ast);
            return 0;
        }

        struct timeval before, after;
        gettimeofday(&before, NULL);
        int result = madc_mir_execute(c_source, argv[filearg]);
        gettimeofday(&after, NULL);

        DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

        madc_sema_free(sema);
        madc_gecko_free_tree(ast);
        return (result < 0) ? 1 : 0;
    }

    // Legacy asmjit pipeline
    if ( !prog->parse(tp) )
        return 0;
    if ( !prog->compile() )
        return 0;

    if ( emit_object_path )
    {
        if ( prog->save_object(emit_object_path) )
            cerr << "wrote " << emit_object_path << endl;
        else
            cerr << "failed to write " << emit_object_path << endl;
        return 0;
    }

    if ( emit_executable_path )
    {
        if ( prog->save_executable(emit_executable_path) )
            cerr << "wrote " << emit_executable_path << endl;
        else
            cerr << "failed to write " << emit_executable_path << endl;
        return 0;
    }

    // set script argc/argv after tokenize/parse/compile (tokenizer_init resets members)
    prog->script_argc = argc - filearg;
    prog->script_argv = argv + filearg;
    g_active_program = prog.get();

    struct timeval before, after;

    gettimeofday(&before, NULL);
    prog->execute();
    gettimeofday(&after, NULL);

    DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

    return 0;
}
```

- [ ] **Step 4: Build and test**

Run: `make -C src`
Expected: clean build.

Run: `bin/madc --backend=mir tests/testhello.mad`
Expected: `Hello, World!` (or a transpiler error if testhello uses unsupported features — either way, the pipeline runs).

Run: `bin/madc --emit-c tests/testhello.mad`
Expected: C11 source code printed to stdout.

Run: `bin/madc tests/testhello.mad`
Expected: `Hello, World!` (legacy path unchanged).

- [ ] **Step 5: Commit**

```bash
git add src/madc.cpp
git commit -m "feat: --backend=mir routes through Gecko+MIR transpiler pipeline"
```

---

### Task 3: Add `--backend=mir` passthrough to test runner

**Files:**
- Modify: `scripts/run_tests.sh:19-23` (flag parsing section)
- Modify: `scripts/run_tests.sh:50-53` (madc invocation lines)

- [ ] **Step 1: Add flag parsing**

At `scripts/run_tests.sh`, replace the flag parsing block (lines 19-23) with:

```bash
RUN_EXE=0
BACKEND_FLAG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) RUN_EXE=1; shift ;;
        --backend=*) BACKEND_FLAG="$1"; shift ;;
        *) break ;;
    esac
done
```

- [ ] **Step 2: Add backend flag to madc invocations**

At `scripts/run_tests.sh`, modify the two `bin/madc` invocation lines (lines 50-53). Change:

```bash
    if [ -f "$input_file" ]; then
        out=$(timeout 5 bin/madc "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>/dev/null)
    else
        out=$(timeout 5 bin/madc "${flags[@]}" "$t" "${args[@]}" 2>/dev/null)
    fi
```

To:

```bash
    if [ -f "$input_file" ]; then
        out=$(timeout 5 bin/madc $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>/dev/null)
    else
        out=$(timeout 5 bin/madc $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>/dev/null)
    fi
```

Note: `$BACKEND_FLAG` is intentionally unquoted so it expands to nothing when empty.

- [ ] **Step 3: Test legacy path is unchanged**

Run: `bash scripts/run_tests.sh`
Expected: `475 passed, 0 failed, 0 timed out, 0 skipped` (identical to before)

- [ ] **Step 4: Test MIR backend path**

Run: `bash scripts/run_tests.sh --backend=mir`
Expected: Some number of passes/fails. This gives us the real transpiler parity count.

- [ ] **Step 5: Commit**

```bash
git add scripts/run_tests.sh
git commit -m "feat: run_tests.sh --backend=mir passthrough for transpiler testing"
```

---

### Task 4: Verify and document baseline

- [ ] **Step 1: Run full legacy test suite**

Run: `make -C src fulltest`
Expected: All unit tests and integration tests pass (no regressions).

- [ ] **Step 2: Run transpiler test suite and record baseline**

Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tee /tmp/mir_baseline.txt`

Record the pass/fail count. This becomes the new tracked transpiler parity number.

- [ ] **Step 3: Update claude_status.json with accurate count**

Update the `transpiler_tests` field in `claude_status.json` with the actual count from step 2.

- [ ] **Step 4: Commit**

```bash
git add claude_status.json
git commit -m "docs: update transpiler test baseline from --backend=mir runner"
```
