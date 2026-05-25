# Code Cleanup & Simplification Plan

Analysis performed 2026-05-24. The codebase has grown by incremental feature
patching — each new C feature added more if/else branches to mega-functions
instead of refactoring into clean abstractions.

## Current State

| File | Lines | Core Issue |
|------|-------|------------|
| parser.cpp | 15,453 | `parseExpression()` = 5,504 lines (36%), 932 if-branches, 83 dynamic_casts |
| compiler.cpp | 15,835 | `TokenCallFunc::compile()` = 1,329 lines, 45 hardcoded builtin string checks |
| tokens.h + madc.h | ~1,930 | 137 token classes, 40+ just for operator precedence tables |
| datadef.h | 1,219 | Magic +10000/+20000 pointer type encoding, 30 global dd* instances |
| lexer.cpp | 4,230 | Dual macro systems (define_map 214 + macro_map 40), 900-line switch |

## Priority 1: Compiler Builtin Dispatch Table (8-16 hours)

**Problem:** 45 `if (var.name == "__builtin_...")` checks chained in
TokenCallFunc::compile(). Adding a new builtin means editing the 1,329-line
function.

**Fix:** Data-driven dispatch table in a new `compiler_builtins.cpp`:

```cpp
struct BuiltinHandler {
    const char *name;
    int min_argc, max_argc;
    Operand &(*emit)(Program &, TokenCallFunc *, regdefp_t &, Operand &);
};
static const BuiltinHandler builtins[] = {
    {"__builtin_object_size", 2, 2, emit_builtin_object_size},
    {"__builtin_shuffle",    2, 3, emit_builtin_shuffle},
    // ...45 entries
};
```

Also extract:
- `__builtin_shuffle` (250 lines inline → own function)
- `_chk` family consolidation (~150 lines duplicated argument remapping)

**Result:** TokenCallFunc::compile() shrinks by ~400 lines. New builtins
are one table entry.

## Priority 2: Unify Parser Dereference Handling (16 hours)

**Problem:** Unary `*` has 8 separate code paths in parseExpression():
1. `*(expr)` with cast detection
2. `*var` simple variable
3. `**var` multi-level
4. `*(*x)++` postfix step (appears in 4 locations!)
5. `*++p` pre-increment
6. `*p->` member chain
7. `*func(args)` call result
8. `*(other_expr)` fallback

**Fix:** Single `parse_unary_deref()` function parameterized by:
- Inner operand type (variable, expression, cast)
- Postfix step (++/--)
- Following chain (->member, [subscript])

Uses helper: `deref_type_for_expr(expr)` unifying
`deref_type_for_variable()`, `effective_pointer_type_for_member_access()`,
`unwrap_subscript_element_type()`.

**Result:** ~300 lines saved, postfix ++ logic in one place.

## Priority 3: Unify Parser Subscript Handling (8 hours)

**Problem:** 5-6 subscript code paths in parseExpression():
1. Chained fixed-array subscript
2. Simple variable subscript (includes swapped N[ptr])
3. Complex pointer-expression subscript
4. Cast subscript (TokenCast with pointer/SIMD)
5. General pointer-expression fallback
6. Lambda vs subscript disambiguation

Each has its own elem_type resolution logic.

**Fix:** Single `parse_subscript(base_expr)` that:
- Determines base type (variable, member, cast, expression)
- Calls unified `resolve_subscript_element_type(base)`
- Handles swapped subscript detection

**Result:** ~150 lines saved, elem_type resolution in one place.

## Priority 4: Unify Lexer Macro Systems (8 hours)

**Problem:** Two separate maps checked for every identifier:
- `define_map`: 214 simple text substitutions
- `macro_map`: 40 function-like macros with parameters

**Fix:** Single `builtin_registry` with variant type:

```cpp
struct BuiltinMacro {
    enum Kind { SIMPLE, FUNCTION_LIKE };
    Kind kind;
    std::string replacement;        // SIMPLE
    std::vector<std::string> params; // FUNCTION_LIKE
    std::string body;               // FUNCTION_LIKE
};
std::map<std::string, BuiltinMacro> builtin_registry;
```

**Result:** Single lookup per identifier, clearer semantics.

## Priority 5: Flatten Token Class Hierarchy (16+ hours)

**Problem:** 137 token classes. 40+ operator classes exist only to override
`precedence()` and `associativity()`. Inheritance is 4-5 levels deep:
`TokenCallMethod → TokenMember → TokenCallFunc → TokenVar → TokenBase`.

**Fix:**
- Table-driven operator precedence (eliminate 40 classes)
- Break the `TokenMember extends TokenCallFunc` chain — members aren't
  function calls
- Merge single-use tokens into parameterized base classes

**Result:** ~70 classes, cleaner inheritance, easier to add new node types.

## Priority 6: AST Visitor/Walker Pattern (4 hours)

**Problem:** `contains_label()` is a hand-written AST traversal. Future
needs (contains_break, contains_return, etc.) would each be another copy.

**Fix:** Generic traversal template:

```cpp
template <typename Pred>
bool walk_ast(TokenBase *node, Pred pred) {
    if (!node) return false;
    if (pred(node)) return true;
    // dispatch to children by node type
}
// Usage: walk_ast(node, [](TokenBase *n) { return dynamic_cast<TokenLabel*>(n); });
```

**Result:** One traversal, many predicates.

## Priority 7: File Splitting (4 hours)

**compiler.cpp** (15.8K lines) → 5 files:
- `compiler.cpp` (~6K): Core compile() methods, token dispatch
- `compiler_builtins.cpp` (~2K): All builtin handlers
- `compiler_operators.cpp` (~2K): Binary ops, comparisons, SIMD
- `compiler_control_flow.cpp` (~1.5K): if/for/while/switch/goto
- `compiler_helpers.cpp` (~3K): Shared helpers, AST visitors

**parser.cpp** (15.4K lines) → 3-4 files:
- `parser.cpp` (~6K): Statement parsing, declarations
- `parser_expr.cpp` (~5K): Expression parsing (parseExpression)
- `parser_types.cpp` (~2K): Type resolution, struct/class/enum
- `parser_helpers.cpp` (~2K): Shared helpers

## Deferred: Type System Magic Numbers (20+ hours)

Replace `rtPtr(x) = x + 10000` with explicit `DataDefPTR` instances.
High effort, widespread change. Defer until a major version boundary.

## Execution Strategy

- **Phase A** (Quick wins): Priorities 1, 6, 7 — dispatch table, AST
  visitor, file splitting. ~16-24 hours. Immediate maintainability gain.
- **Phase B** (Parser cleanup): Priorities 2, 3 — dereference and subscript
  unification. ~24 hours. Reduces parseExpression by ~1,500 lines.
- **Phase C** (Infrastructure): Priorities 4, 5 — macro unification, token
  hierarchy. ~24 hours. Deeper structural improvement.
- **Phase D** (Long-term): Type system magic numbers. Plan for next major
  version.

Total estimated effort: ~65-100 hours across all phases.
