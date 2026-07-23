# Parser Source-Order Declarations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor madc's parser to preserve source-ordered top-level declarations (typedefs, structs, enums, globals) in the AST, and record typedef alias names at usage sites, so the CIR layer can produce a c2mir-compatible node_t tree identical to what c2m's own parser generates.

**Architecture:** c2mir's checker processes the MODULE -> LIST children sequentially — a typedef must appear before its first use. Today, madc's parser stores typedefs and struct definitions only in unordered lookup maps (`datatype_map`, `struct_map`) and returns NULL from `TokenTYPEDEF::parse()` / `TokenSTRUCT::parse()`. These declarations never enter the AST and their source order is lost. This plan adds: (1) AST node types for typedef and struct declarations, (2) a source-ordered declaration vector in `Program`, (3) typedef-name preservation on `Variable` and struct members, and (4) updates to CIR translation to walk the ordered declarations and emit typedef names.

**Tech Stack:** C++11, madc parser (parser.cpp), CIR translator (madc_cir.cpp), c2mir API, doctest unit tests

---

## How c2mir Works (Reference)

c2mir's parser (`c2mir.c:transl_unit`) loops over external declarations and appends each to a LIST in source order. The final tree is `N_MODULE(N_LIST:(declaration | FUNC_DEF)*)`.

Key behaviors CIR must match:

1. **Typedef declarations** are `N_SPEC_DECL(SHARE(LIST(TYPEDEF, type_spec)), DECL(ID("alias"), LIST([POINTER])), IGNORE*3)` — they appear in the MODULE LIST in source order.

2. **Typedef names at usage sites** — when a variable is declared as `EXT_BV x`, c2m emits `ID("EXT_BV")` as the type specifier, NOT `STRUCT(ID("extended_bitvector"), IGNORE)`. The checker (line 6898) resolves `N_ID` type specs by looking up the previously-seen typedef SPEC_DECL and copying its type.

3. **Struct-typedef combos** like `typedef struct X { ... } Y;` produce a SINGLE SPEC_DECL containing both the struct definition (with full member list) and the TYPEDEF keyword.

4. **Order matters** — the checker processes declarations sequentially. If `EXT_BV` is used in `struct char_data` but the typedef hasn't appeared yet, the checker emits "incomplete type".

## File Structure

| File | Changes | Purpose |
|------|---------|---------|
| `include/tokens.h` | Add `ttTypedefDecl`, `ttStructDef` to `TokenType` enum | New AST node type IDs |
| `include/madc.h` | Add `TokenTypedefDecl`, `TokenStructDef` classes; add `top_decls` vector to `Program`; add `typedef_name` field to `Variable` and `memberpair_t` | AST nodes + source-order tracking + typedef name preservation |
| `src/parser.cpp` | Return AST nodes from `TokenTYPEDEF::parse()`, `TokenSTRUCT::parse()`, `TokenENUM::parse()`; populate `top_decls`; set `typedef_name` on variables/members | Parser produces ordered declarations |
| `src/madc_cir.cpp` | Walk `prog->top_decls` instead of maps; emit `N_ID("alias")` for typedef'd types | CIR translation matches c2m's tree shape |
| `tests/unit/test_cir.cpp` | Add typedef test cases | Verify typedef round-trip |

---

### Task 1: Add AST Node Types for Declarations

**Files:**
- Modify: `include/tokens.h:16-24` (TokenType enum)
- Modify: `include/madc.h` (new classes, new vector on Program)
- Modify: `include/datatokens.h:53-77` (Variable class, memberpair_t)

- [ ] **Step 1: Add TokenType enum values**

In `include/tokens.h`, add after `ttStructLit`:

```cpp
	ttStructLit,
	ttTypedefDecl,	// typedef declaration — preserves source order in AST
	ttStructDef	// standalone struct/union definition — preserves source order in AST
```

- [ ] **Step 2: Add `typedef_name` to Variable**

In `include/datatokens.h`, add a field to the `Variable` class after `std::string storage_alias_name;`:

```cpp
    std::string typedef_name;  // if declared via typedef, the alias used in source (e.g. "EXT_BV")
```

And in the Variable constructor, initialize it to empty string (it already is by default for std::string, no action needed).

- [ ] **Step 3: Add typedef_name to memberpair_t**

In `include/datadef.h`, change `memberpair_t` from:

```cpp
typedef std::pair<std::string, DataDef *> memberpair_t;
```

to a struct that also carries the typedef name:

```cpp
struct memberpair_t {
    std::string first;    // member name
    DataDef *second;      // member type
    std::string typedef_name;  // source typedef alias (empty if raw type)
    memberpair_t() : second(nullptr) {}
    memberpair_t(const std::string &n, DataDef *d) : first(n), second(d) {}
    memberpair_t(const std::string &n, DataDef *d, const std::string &td) : first(n), second(d), typedef_name(td) {}
};
```

Note: all existing code constructs `memberpair_t` via `{name, &dd}` or `emplace_back(name, &dd)` — the 2-arg constructor preserves compatibility.

- [ ] **Step 4: Add top-level declaration tracking to Program**

In `include/madc.h`, add a tagged-union-style entry for source-ordered declarations. After the `pending_funcs` vector (line ~925):

```cpp
    // Source-ordered top-level declarations for CIR tree generation.
    // Each entry records what was declared and in what order, matching
    // the order c2m's parser would produce in its MODULE LIST.
    enum class DeclKind { dkTypedef, dkStruct, dkUnion, dkEnum, dkGlobalVar };
    struct TopDecl {
        DeclKind kind;
        std::string name;         // typedef alias name, struct tag, or variable name
        DataDef *dd;              // the DataDef (struct, typedef target, etc.)
        TokenDataType *tdt;       // for typedefs: the TokenDataType entry
        Variable *var;            // for global vars: the Variable
        const char *file;
        int line;
        TopDecl() : kind(DeclKind::dkStruct), dd(nullptr), tdt(nullptr), var(nullptr), file(nullptr), line(0) {}
    };
    std::vector<TopDecl> top_decls;
```

- [ ] **Step 5: Add TokenTypedefDecl and TokenStructDef AST node classes**

In `include/madc.h`, after the `TokenDecl` class:

```cpp
// AST node for a typedef declaration — preserves source order.
// Returned by TokenTYPEDEF::parse() so typedefs appear in the AST.
class TokenTypedefDecl : public TokenBase
{
public:
    std::string alias;       // typedef alias name
    DataDef *target_type;    // what the typedef resolves to
    TokenTypedefDecl(const std::string &a, DataDef *t) : alias(a), target_type(t) {}
    virtual TokenType type() const { return TokenType::ttTypedefDecl; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp) { return _operand; }
};

// AST node for a standalone struct/union definition — preserves source order.
// Returned by TokenSTRUCT::parse() when the struct has no variable declarator.
class TokenStructDef : public TokenBase
{
public:
    DataDefSTRUCT *sdd;
    bool is_union;
    TokenStructDef(DataDefSTRUCT *s, bool u = false) : sdd(s), is_union(u) {}
    virtual TokenType type() const { return TokenType::ttStructDef; }
    virtual asmjit::Operand &compile(Program &, regdefp_t &regdp) { return _operand; }
};
```

- [ ] **Step 6: Build and verify compilation**

Run: `make -C src clean && make -C src`
Expected: Compiles with no errors. The new types exist but are not yet used.

- [ ] **Step 7: Run existing tests to verify no regressions**

Run: `make -C src test`
Expected: All unit tests pass (no behavioral change yet).

- [ ] **Step 8: Commit**

```bash
git add include/tokens.h include/madc.h include/datatokens.h include/datadef.h
git commit -m "feat: add AST node types for typedef/struct declarations and source-order tracking"
```

---

### Task 2: Parser — Return AST Nodes from TokenTYPEDEF::parse()

**Files:**
- Modify: `src/parser.cpp` — `TokenTYPEDEF::parse()` method (all return paths)

Currently `TokenTYPEDEF::parse()` registers the typedef in `datatype_map` and returns NULL. We need it to also: (a) append a `TopDecl` to `prog->top_decls`, and (b) return a `TokenTypedefDecl` AST node.

- [ ] **Step 1: Find all return paths in TokenTYPEDEF::parse()**

The function has these return paths (search for `return` between `TokenTYPEDEF::parse` and the next top-level function):

1. Line ~11313: `return pgm.parseKeyword(...)` — delegates to another keyword (e.g. `typedef enum`)
2. Line ~11338: `return NULL;` — after enum alias registration
3. Line ~11378: `return NULL;` — after fptr form 2
4. Line ~11408: `return NULL;` — after fptr form 1
5. Line ~11460: `return NULL;` — normal typedef (final path)

- [ ] **Step 2: Add helper to record a typedef TopDecl**

At the top of `TokenTYPEDEF::parse()`, or as a local lambda, add:

```cpp
auto record_typedef = [&](const std::string &alias, DataDef *dd, TokenDataType *tdt) {
    Program::TopDecl td;
    td.kind = Program::DeclKind::dkTypedef;
    td.name = alias;
    td.dd = dd;
    td.tdt = tdt;
    td.file = TokenBase::_parse_file;
    td.line = TokenBase::_parse_line;
    pgm.top_decls.push_back(td);
};
```

- [ ] **Step 3: At each `return NULL` after a typedef registration, return a TokenTypedefDecl instead**

For each path where `pgm.datatype_map[alias] = tdt;` is followed by `return NULL;`:

Replace:
```cpp
    return NULL;
```
With:
```cpp
    record_typedef(alias, base_dd, tdt);
    return new TokenTypedefDecl(alias, base_dd);
```

Where `alias` is the typedef alias string and `base_dd` is the DataDef that was registered.

For the enum path (~line 11338):
```cpp
    record_typedef(alias, enum_alias_dd, tdt);
    return new TokenTypedefDecl(alias, enum_alias_dd);
```

For the fptr paths (~lines 11378, 11408):
```cpp
    record_typedef(alias, fptr, tdt);
    return new TokenTypedefDecl(alias, fptr);
```

For the normal typedef path (~line 11460):
```cpp
    record_typedef(alias, base_dd, tdt);
    return new TokenTypedefDecl(alias, base_dd);
```

The `return pgm.parseKeyword(...)` path (typedef enum) can be left as-is — `TokenENUM::parse()` will be updated in Task 4.

- [ ] **Step 4: Verify the Program::parse() loop handles the new token type**

In `Program::parse()` (line ~15630), the loop does:
```cpp
ts = parseStatement(tb);
if (ts) {
    if (ts->type() != TokenType::ttCompound)
        tp->statements.push_back((TokenStmt *)ts);
    else
        ast.push(ts);
}
```

`TokenTypedefDecl` has `type() == ttTypedefDecl` which is not `ttCompound`, so it will be added to `tp->statements`. This is correct — it appears in source order among other top-level statements.

The JIT compiler's `TokenProgram::compile()` iterates `statements` and calls `compile()` on each. Our `TokenTypedefDecl::compile()` returns `_operand` (a no-op), so the JIT path is unaffected.

- [ ] **Step 5: Build and run tests**

Run: `make -C src clean && make -C src && make -C src fulltest`
Expected: All tests pass. The new AST nodes exist but the JIT compiler ignores them.

- [ ] **Step 6: Commit**

```bash
git add src/parser.cpp
git commit -m "feat: TokenTYPEDEF::parse() returns AST nodes and records source order"
```

---

### Task 3: Parser — Return AST Nodes from TokenSTRUCT::parse()

**Files:**
- Modify: `src/parser.cpp` — `TokenSTRUCT::parse()` method

`TokenSTRUCT::parse()` has multiple return paths. The ones that return NULL after registering a struct in `struct_map` (without a variable declarator) need to return a `TokenStructDef` and record a `TopDecl`.

- [ ] **Step 1: Identify the return-NULL paths in TokenSTRUCT::parse()**

Key paths:
1. Forward declaration: `struct tag;` → registers in struct_map, returns NULL
2. Struct definition without variable: `struct tag { ... };` → registers, returns NULL
3. Typedef struct: `typedef struct tag { ... } alias;` → registers both, returns NULL
4. Struct with variable: `struct tag { ... } var;` → returns `parseDeclaration(tdt)` (already in AST)

Only paths 1, 2, and 3 need fixes.

- [ ] **Step 2: Add helper to record struct/typedef TopDecls**

At the top of `TokenSTRUCT::parse()`:

```cpp
auto record_struct = [&](DataDefSTRUCT *sdd, bool is_union) {
    Program::TopDecl td;
    td.kind = is_union ? Program::DeclKind::dkUnion : Program::DeclKind::dkStruct;
    td.name = sdd->name;
    td.dd = sdd;
    td.file = TokenBase::_parse_file;
    td.line = TokenBase::_parse_line;
    pgm.top_decls.push_back(td);
};
auto record_typedef_from_struct = [&](const std::string &alias, DataDef *dd, TokenDataType *tdt) {
    Program::TopDecl td;
    td.kind = Program::DeclKind::dkTypedef;
    td.name = alias;
    td.dd = dd;
    td.tdt = tdt;
    td.file = TokenBase::_parse_file;
    td.line = TokenBase::_parse_line;
    pgm.top_decls.push_back(td);
};
```

- [ ] **Step 3: For struct definition without variable (path 2)**

After `pgm.struct_map[tag->str] = dds;` and before the `return NULL;`:

```cpp
    record_struct(dds, is_union);
    return new TokenStructDef(dds, is_union);
```

- [ ] **Step 4: For typedef struct (path 3)**

The typedef alias loop already registers into `pgm.datatype_map`. After the loop, before `return NULL;`:

For the combined typedef+struct, record BOTH the struct and the typedef(s):

```cpp
    // Record the struct definition
    record_struct(dds, is_union);
    // Record each typedef alias
    // (aliases were already registered in the loop above)
```

Each alias inside the loop should also get a `record_typedef_from_struct(alias->str, alias_dd, tdt);`.

Then return a `TokenStructDef` (or a `TokenTypedefDecl` for the primary alias — the struct is embedded in the typedef in c2m's tree).

- [ ] **Step 5: For forward declaration (path 1)**

After `pgm.struct_map[tag->str] = dds;`:

```cpp
    record_struct(dds, is_union);
    return new TokenStructDef(dds, is_union);
```

- [ ] **Step 6: Build and run fulltest**

Run: `make -C src clean && make -C src && make -C src fulltest`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/parser.cpp
git commit -m "feat: TokenSTRUCT::parse() returns AST nodes and records source order"
```

---

### Task 4: Parser — Preserve Typedef Names at Usage Sites

**Files:**
- Modify: `src/parser.cpp` — `parseDeclaration()`, struct member parsing

When the parser resolves a type via `datatype_map` lookup (e.g., `EXT_BV x;`), it currently stores only the underlying `DataDef *` on the variable. We need to also record the typedef alias name.

- [ ] **Step 1: Find where parseDeclaration sets the variable type**

Search for where `Variable` objects are created with a `TokenDataType`'s definition. The key function is `parseDeclaration()` (around line 9381).

When the type comes from `datatype_map`, the `TokenDataType *tdt` is available. The variable gets `type = &tdt->definition`. We need to also set `var->typedef_name = tdt->str` when the name differs from the DataDef's own name (indicating it's a typedef alias, not a primitive type name).

- [ ] **Step 2: In parseDeclaration, set typedef_name on the Variable**

After the variable's type is assigned from a TokenDataType lookup, add:

```cpp
if (tdt && !cir_is_builtin_type(tdt->str) && tdt->str != tdt->definition.name)
    v->typedef_name = tdt->str;
```

Actually, a simpler approach: always set it from the TokenDataType name. The CIR layer can check if it's a known typedef vs a builtin:

```cpp
v->typedef_name = tdt->str;
```

- [ ] **Step 3: In struct member parsing, record typedef_name**

In `TokenSTRUCT::parse()`, where `addMember()` is called, if the member type came from a typedef lookup, pass the typedef name into the memberpair:

After the member DataDef is resolved from a TokenDataType lookup, and before `addMember()`:

```cpp
std::string member_typedef = "";
if (member_tdt && member_tdt->str != member_tdt->definition.name)
    member_typedef = member_tdt->str;
```

The `addMember()` call would pass this through. Since `memberpair_t` now has a `typedef_name` field (from Task 1), update `DataDefSTRUCT::addMember` to accept and store it, or set it post-hoc on the last member added.

- [ ] **Step 4: Build and run fulltest**

Run: `make -C src clean && make -C src && make -C src fulltest`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/parser.cpp include/datadef.h
git commit -m "feat: preserve typedef alias names on variables and struct members"
```

---

### Task 5: CIR — Walk top_decls in Source Order

**Files:**
- Modify: `src/madc_cir.cpp` — `cir_translate()` function

Replace the current map-walking passes (Pass 0, Pass 0.25, Pass 0.5) with a single walk over `prog->top_decls` in source order.

- [ ] **Step 1: Replace the struct/typedef/global passes**

The current `cir_translate()` has:
- Pass 0: struct defs from struct_map
- Pass 0.25: typedefs from datatype_map  
- Pass 0.5: globals from tkProgram->variables

Replace all three with a single loop:

```cpp
    // Pass 0: Emit top-level declarations in source order.
    // This matches the order c2m's parser would produce.
    std::set<std::string> emitted_structs;
    for (auto &td : prog->top_decls) {
        c2mir_pos_t pos = { td.file ? td.file : "<decl>", td.line, 0 };
        switch (td.kind) {
        case Program::DeclKind::dkTypedef: {
            node_t n = cir_typedef_decl(c2m, td.name, td.dd, pos, emitted_structs);
            if (n) {
                c2mir_op_append(c2m, top_list, n);
                // Track structs emitted inline within typedefs
                DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
                if (!sdd) {
                    DataDefPTR *ptr = dynamic_cast<DataDefPTR *>(td.dd);
                    if (ptr) sdd = dynamic_cast<DataDefSTRUCT *>(ptr->base_type);
                }
                if (sdd && !sdd->members.empty())
                    emitted_structs.insert(sdd->name);
            }
            break;
        }
        case Program::DeclKind::dkStruct:
        case Program::DeclKind::dkUnion: {
            DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
            if (sdd && !sdd->members.empty() && !emitted_structs.count(sdd->name)) {
                emitted_structs.insert(sdd->name);
                node_t sd = cir_struct_def(c2m, sdd, pos);
                if (sd) c2mir_op_append(c2m, top_list, sd);
            }
            break;
        }
        case Program::DeclKind::dkGlobalVar: {
            if (td.var && !dynamic_cast<FuncDef *>(td.var->type)) {
                node_t gd = cir_var_decl(c2m, td.var, pos);
                if (gd) c2mir_op_append(c2m, top_list, gd);
            }
            break;
        }
        case Program::DeclKind::dkEnum:
            // Enum constants are already expanded as integer constants
            break;
        }
    }
```

- [ ] **Step 2: Keep the extern function prototype pass (Pass 0.75)**

The extern function prototypes from `funcdef_map` don't have source positions — they come from embedded headers and `#include`d declarations. Keep this pass but move it after the top_decls loop.

- [ ] **Step 3: Build and run CIR tests**

Run: `make -C src && LD_LIBRARY_PATH=/usr/local/lib:lib bin/test_cir`
Expected: All 33 CIR tests pass.

- [ ] **Step 4: Run fulltest**

Run: `make -C src fulltest`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/madc_cir.cpp
git commit -m "feat: CIR walks top_decls in source order instead of unordered maps"
```

---

### Task 6: CIR — Emit Typedef Names at Usage Sites

**Files:**
- Modify: `src/madc_cir.cpp` — `cir_type_list()`, `cir_var_decl()`, `cir_param_decl()`, `cir_struct_def()`

When a variable, parameter, or struct member was declared using a typedef name, emit `N_ID("typedef_name")` as the type specifier instead of the raw type nodes. This matches c2m's tree.

- [ ] **Step 1: Add a typedef-aware type list builder**

Add a new helper that checks for a typedef name before falling back to raw type emission:

```cpp
// Build type specifier LIST, using typedef name if available.
// If typedef_alias is non-empty, emit ID("alias") — c2mir's checker
// will resolve it from the previously-emitted typedef SPEC_DECL.
static node_t cir_type_list_maybe_typedef(c2m_ctx_t c2m, DataDef *dd,
                                           const std::string &typedef_alias,
                                           c2mir_pos_t pos)
{
    if (!typedef_alias.empty()) {
        node_t list = c2mir_new_node(c2m, N_LIST);
        node_t id = c2mir_new_str_node(c2m, N_ID, typedef_alias.c_str(),
                                        typedef_alias.size() + 1, pos);
        c2mir_op_append(c2m, list, id);
        c2mir_set_node_pos(c2m, list, pos);
        return list;
    }
    return cir_type_list(c2m, dd, pos);
}
```

- [ ] **Step 2: Use typedef names in cir_var_decl**

In `cir_var_decl()`, when building the type list for a variable that has `v->typedef_name` set, use the typedef-aware builder:

```cpp
node_t type_list = cir_type_list_maybe_typedef(c2m, base_dd, v->typedef_name, vpos);
```

When a typedef name is used, skip the pointer unwrapping — the typedef name already includes the pointer-ness (e.g., `FOO_PTR` is already `typedef FOO *FOO_PTR`). The pointer goes in the typedef SPEC_DECL, not repeated at the usage site.

- [ ] **Step 3: Use typedef names in cir_struct_def members**

In `cir_struct_def()`, access the member's typedef_name:

```cpp
const std::string &mtypedef = sdd->members[i].typedef_name;
node_t mspec = cir_type_list_maybe_typedef(c2m, mbase, mtypedef, pos);
```

When `mtypedef` is set, skip pointer unwrapping for that member.

- [ ] **Step 4: Use typedef names in cir_param_decl**

Parameters may also use typedef names. The `Variable` for function parameters (in `tf->method->parameters`) carries `typedef_name`. Pass it through.

- [ ] **Step 5: Test with the typedef test file**

Run: `bin/madc --std=c --dump-cir tmp/test_typedef_cir.mad 2>&1 | head -40`

Compare against: `cd /workspace/mir && ./c2m -d` on the equivalent C file.

The type specifiers should show `ID "sh_int"` and `ID "BYTE"` at usage sites, not raw `INT` / `UNSIGNED CHAR`.

- [ ] **Step 6: Test with SMAUG**

Run: `bin/madc --std=c --backend=cir MadSMAUG/src/SMAUG.mad 2>&1 | wc -l`

Compare error count against the previous 3733. The "incomplete type" errors should be eliminated, and many "incompatible pointer types" errors should also resolve since c2mir can now match typedef'd types.

- [ ] **Step 7: Commit**

```bash
git add src/madc_cir.cpp
git commit -m "feat: CIR emits typedef names at usage sites, matching c2m's tree shape"
```

---

### Task 7: Add CIR Unit Tests for Typedefs

**Files:**
- Modify: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Add a test for simple typedef**

```cpp
TEST_CASE("CIR: typedef int") {
    const char *src = "typedef int myint;\nmyint add(myint a, myint b) { return a + b; }\nint main() { return add(3, 4); }";
    CHECK(cir_run(src) == 7);
}
```

- [ ] **Step 2: Add a test for struct typedef**

```cpp
TEST_CASE("CIR: typedef struct") {
    const char *src =
        "typedef struct point { int x; int y; } POINT;\n"
        "int main() { POINT p; p.x = 10; p.y = 20; return p.x + p.y; }";
    CHECK(cir_run(src) == 30);
}
```

- [ ] **Step 3: Add a test for pointer typedef**

```cpp
TEST_CASE("CIR: typedef pointer") {
    const char *src =
        "#include <stdlib.h>\n"
        "typedef struct node { int val; } NODE;\n"
        "typedef NODE *NODE_PTR;\n"
        "int main() { NODE_PTR p = (NODE_PTR)malloc(sizeof(NODE)); p->val = 42; int r = p->val; free(p); return r; }";
    CHECK(cir_run(src) == 42);
}
```

- [ ] **Step 4: Add a test for typedef ordering (member uses typedef)**

```cpp
TEST_CASE("CIR: typedef used in struct member") {
    const char *src =
        "typedef int sh_int;\n"
        "typedef struct inner { sh_int value; } INNER;\n"
        "typedef struct outer { INNER item; int count; } OUTER;\n"
        "int main() { OUTER o; o.item.value = 99; o.count = 1; return o.item.value + o.count; }";
    CHECK(cir_run(src) == 100);
}
```

- [ ] **Step 5: Run CIR tests**

Run: `make -C src && LD_LIBRARY_PATH=/usr/local/lib:lib bin/test_cir`
Expected: All tests pass (33 old + 4 new = 37).

- [ ] **Step 6: Commit**

```bash
git add tests/unit/test_cir.cpp
git commit -m "test: add CIR typedef unit tests"
```

---

## Verification

After all tasks are complete:

1. `make -C src fulltest` — all integration + unit tests pass
2. `bin/test_cir` — 37+ CIR tests pass
3. `bin/madc --std=c --dump-cir tmp/test_typedef_cir.mad` — tree matches `c2m -d` output for equivalent C
4. `bin/madc --std=c --backend=cir MadSMAUG/src/SMAUG.mad 2>&1 | wc -l` — significant error reduction from 3733
5. Diff `--dump-cir` output against `c2m -d` output for a representative SMAUG subset to verify tree shape match

## Self-Review

**Spec coverage:** All 4 requirements from the "How c2mir Works" section are addressed: typedef SPEC_DECL nodes (Tasks 2-3), typedef names at usage sites (Task 4+6), struct-typedef combos (Task 3), source ordering (Task 5).

**Placeholder scan:** No TBD/TODO items. All code blocks show actual implementation. Test cases have concrete expected values.

**Type consistency:** `TopDecl` struct used consistently across Tasks 2-5. `typedef_name` field added in Task 1, populated in Task 4, consumed in Task 6. `memberpair_t` changed to struct in Task 1, backward-compatible with existing 2-arg construction.
