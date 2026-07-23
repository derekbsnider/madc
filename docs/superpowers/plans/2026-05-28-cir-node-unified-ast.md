# Plan: cir_node — Unified madc/c2mir AST Tree

## Context

madc's CIR layer currently builds c2mir node_t trees using c2mir's own allocation functions (`c2mir_new_node` etc.), then hands the tree to c2mir's checker and MIR generator. This approach has fundamental limitations:

1. **No source order** — the parser stores typedefs/structs in unordered maps; the tree can't match c2m's declaration order
2. **No typedef names at usage sites** — variables lose their typedef alias, so the tree emits raw types instead of `ID("EXT_BV")`
3. **No round-trip** — the tree carries no madc-specific data, so it can't regenerate source code
4. **c2mir owns all memory** — madc can't extend nodes or control their lifecycle

The fix: `cir_node` — a struct that extends c2mir's `struct node` with madc-specific fields. madc owns all node memory. c2mir sees a valid `node_t` tree and processes it normally.

## Design

### cir_node extends struct node

```cpp
struct cir_node {
    struct node base;           // offset 0 — IS-A node_t

    // --- madc extensions (invisible to c2mir) ---
    TokenBase   *origin;        // originating madc AST node
    DataDef     *datadef;       // madc type info
    const char  *typedef_name;  // source typedef alias ("EXT_BV"), NULL if none
    const char  *src_file;      // source file (madc format)
    int          src_line;
    int          src_column;
    uint8_t      src_lang;      // 0=C, 1=C++, 2=madc, 3=other
};
```

`(node_t)&cir_node_instance` is valid because `base` is at offset 0. DLIST macros work because `op_link` is at the same offset. c2mir's checker, generator, and dump functions see a normal tree.

### Memory management

- **madc allocates** all `cir_node` objects via a simple arena (pool allocator)
- **c2mir does NOT free** our nodes — we add an `external_tree` flag to `c2mir_options` that skips `reg_memory_finish()` for node/attr memory, or more precisely prevents c2mir from freeing things the caller owns
- **madc destroys** the arena after MIR generation is complete
- **Strings** for N_ID/N_STR nodes: use `c2mir_uniq_str()` (new API) to intern into c2mir's string table, ensuring pointer-equality works in the checker's symbol lookups

### c2mir fork changes

Small additions to our c2mir fork (we control `/workspace/mir/`):

1. **`c2mir_next_uid(c2m_ctx)`** — assigns next uid and ensures `node_positions` has a slot. Lets us allocate our own nodes but use c2mir's uid sequence.
2. **`c2mir_uniq_str(c2m_ctx, s, len)`** — interns a string in c2mir's string table. Returns pointer valid until `c2mir_finish`.
3. **`c2m_options->external_tree`** flag — when set, `reg_memory_finish()` / `c2mir_finish_compile()` skips freeing node-related allocations. The caller manages cleanup.

### CirBuilder class

Replaces the current static C-style functions in `madc_cir.cpp`:

```cpp
class CirBuilder {
    c2m_ctx_t c2m;
    CirArena arena;
public:
    // Leaf builders
    node_t id(const char *name, TokenBase *origin = nullptr);
    node_t integer(long val, TokenBase *origin = nullptr);
    node_t str(const char *s, size_t len, TokenBase *origin = nullptr);
    
    // Type builders — emit ID("typedef_name") when available
    node_t type_list(DataDef *dd, const std::string &typedef_name = "");
    
    // Declaration builders
    node_t typedef_decl(const std::string &alias, DataDef *dd);
    node_t struct_def(DataDefSTRUCT *sdd);
    node_t var_decl(Variable *v);
    node_t func_def(TokenFunc *tf);
    
    // Expression/statement translation
    node_t translate_expr(TokenBase *tb);
    node_t translate_stmt(TokenBase *tb);
    node_t translate_block(TokenCpnd *tc);
    
    // Top-level — walks prog->top_decls in source order
    node_t translate_module(Program *prog);
};
```

Every builder method calls `arena.alloc()` → initializes `base` fields (code, uid via `c2mir_next_uid`, DLIST_INIT) → sets extension fields (origin, datadef, typedef_name) → registers position via `c2mir_set_node_pos`.

### Parser changes

Two additions so CIR can produce a source-ordered tree:

1. **Source-ordered declaration vector** — `Program::top_decls` records every top-level declaration (typedef, struct, union, enum, global var) in parse order. `TokenTYPEDEF::parse()` and `TokenSTRUCT::parse()` append entries instead of returning NULL silently.

2. **Typedef name preservation** — `Variable::typedef_name` and `memberpair_t::typedef_name` record the typedef alias used in source. CIR emits `ID("EXT_BV")` instead of `STRUCT(ID("extended_bitvector"), IGNORE)`.

### Round-trip capability

Every `cir_node` carries `origin` (TokenBase*), `typedef_name`, and `src_lang`. A tree walker can reconstruct source code by reading the madc extensions — the tree is simultaneously a valid c2mir AST and a madc AST.

## Files to modify

| File | Change |
|------|--------|
| `/workspace/mir/c2mir/c2mir.c` | Add `c2mir_next_uid()`, `c2mir_uniq_str()`, `external_tree` flag |
| `/workspace/mir/c2mir/c2mir_api.h` | Declare new API functions, add flag to options struct |
| `/workspace/madc/src/cir_node.h` | **NEW** — cir_node struct, CirArena, CIR_NODE macro |
| `/workspace/madc/src/cir_builder.h` | **NEW** — CirBuilder class declaration |
| `/workspace/madc/src/cir_builder.cpp` | **NEW** — CirBuilder implementation |
| `/workspace/madc/src/madc_cir.cpp` | Thin wrapper using CirBuilder |
| `/workspace/madc/src/madc_cir.h` | Update if needed (public API stays same) |
| `/workspace/madc/include/tokens.h` | Add ttTypedefDecl, ttStructDef to TokenType |
| `/workspace/madc/include/madc.h` | Add TopDecl, top_decls, TokenTypedefDecl, TokenStructDef |
| `/workspace/madc/include/datatokens.h` | Add typedef_name to Variable |
| `/workspace/madc/include/datadef.h` | Change memberpair_t to struct with typedef_name |
| `/workspace/madc/src/parser.cpp` | Return AST nodes from typedef/struct parse, record top_decls, set typedef_name |
| `/workspace/madc/tests/unit/test_cir.cpp` | Add typedef tests |

## Implementation sequence

1. **c2mir API additions** — `c2mir_next_uid`, `c2mir_uniq_str`, `external_tree` flag
2. **cir_node infrastructure** — `cir_node.h`, `CirArena`, `CirBuilder` with basic builders
3. **Migrate translation functions** — port existing `cir_translate_*` to CirBuilder methods (mechanical: `c2mir_new_node` → `builder.make`)
4. **Parser: source-order declarations** — top_decls vector, TokenTYPEDEF/TokenSTRUCT return AST nodes
5. **Parser: typedef name preservation** — Variable::typedef_name, memberpair_t::typedef_name
6. **CIR: walk top_decls + emit typedef names** — translate_module uses source order, type_list emits ID("alias")
7. **Test against SMAUG** — verify error count drops significantly

## Verification

1. `make -C src fulltest` — all existing tests pass (no regressions)
2. `bin/test_cir` — existing 33 tests + new typedef tests pass
3. `bin/madc --std=c --dump-cir tmp/test_typedef_cir.mad` vs `c2m -d` on equivalent C — trees match
4. `bin/madc --std=c --backend=cir MadSMAUG/src/SMAUG.mad 2>&1 | wc -l` — significant reduction from 3733 errors
5. Verify `CIR_NODE(n)->origin` survives checker pass — extension fields intact after `c2mir_compile_tree`
