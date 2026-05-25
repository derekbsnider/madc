# Multi-Syntax Parsing Plan

Created 2026-05-24.

## Vision

madc already mixes functions from multiple language traditions via
namespaces (php::, perl::, python::, ruby::, js::, rust::). The next
step: let users write code in the SYNTAX of those languages too, not
just call their functions with C syntax.

A Python-style mode where indentation defines blocks. A Ruby-style mode
where `end` closes blocks. A Rust-style mode with `fn`, `let`, `impl`.
Each mode configures the lexer and parser — the compiler and IR are
shared.

## Why This Matters

1. **Accessibility.** Python developers shouldn't need to learn C syntax
   to use madc. Braces and semicolons are the #1 barrier for Python users.

2. **The "Mad" in Mad-C.** The tagline is mixing languages. Currently
   that's function-level mixing. Syntax-level mixing is the full vision.

3. **Editor/IDE integration.** libmadcedit needs syntax highlighting for
   non-C languages anyway. If madc can parse Python syntax, the editor
   can highlight it AND the JIT can run it.

4. **Scripting versatility.** Different tasks suit different syntaxes.
   Configuration files suit Python/YAML-style. Systems code suits C/Rust.
   String processing suits Perl/Ruby. One JIT, many faces.

## Architecture: Syntax as a Configuration Layer

```
Source code (any syntax)
    │
    ▼ Syntax-specific lexer rules (configured)
    │   ├── C-style: { } ; // /* */
    │   ├── Python-style: indentation, :, #
    │   ├── Ruby-style: do/end, def/end, #
    │   └── Rust-style: fn, let, ->, //
    │
    ▼ Syntax-specific parser adaptations (configured)
    │   ├── Block delimiters (braces vs indent vs end)
    │   ├── Statement terminators (; vs newline)
    │   ├── Declaration keywords (int vs let vs def)
    │   ├── Function syntax (int foo() vs def foo(): vs fn foo() ->)
    │   └── Comment syntax (// vs # vs --)
    │
    ▼ Shared AST (same TokenBase tree regardless of source syntax)
    │
    ▼ Shared compiler (same x86-64 JIT regardless of source syntax)
    │
    ▼ Same machine code output
```

The key insight: **the AST and compiler are syntax-independent.** A
function is a function whether declared as `int foo()` or `def foo():`.
The differences are purely in lexing and parsing — which tokens delimit
blocks, how declarations look, what comments look like.

## Syntax Selection (Three Layers)

Syntax is selected by three mechanisms, each overriding the previous:

1. **File extension** → default profile
   - `.mad`, `.c`, `.cpp`, `.h`, `.hpp`, `.hh`, `.cc` → C syntax
   - `.py`, `.pyw` → Python syntax
   - `.rb` → Ruby syntax
   - `.rs` → Rust syntax

2. **`--syntax=X`** → command-line override for the entire file
   - `madc --syntax=python script.txt`
   - Overrides extension-based detection

3. **`#pragma syntax X`** → mid-file switch
   - `#pragma syntax python` switches parser mode from that point
   - `#pragma syntax c` switches back
   - Each `#include` inherits the current mode (or uses its own extension)

Precedence: `#pragma` > `--syntax` > file extension.

## Syntax Profiles

A syntax profile configures the lexer and parser for a language style:

```c
struct SyntaxProfile {
    const char *name;              // "c", "python", "ruby", "rust"
    const char *file_extensions;   // ".c .h .mad", ".py .pyw", ".rb"

    // Lexer configuration
    const char *line_comment;      // "//", "#", "--"
    const char *block_comment_start; // "/*", "\"\"\"", "=begin"
    const char *block_comment_end;   // "*/", "\"\"\"", "=end"
    const char *string_delimiters;   // "\"'", "\"'`"
    bool        indent_significant; // Python-style: indent = block

    // Parser configuration
    BlockStyle  block_style;       // BRACES, INDENT, END_KEYWORD
    bool        semicolons_required; // C: yes. Python/Ruby: no.
    const char *function_keyword;  // NULL (C-style), "def", "fn", "func"
    const char *return_type_style; // PREFIX (int foo), SUFFIX (foo -> int), NONE
    const char *variable_keyword;  // NULL (type name), "let", "var", "my"
    const char *const_keyword;     // "const", "let" (Rust), "final"
    bool        type_inference;    // auto-deduce types when keyword used

    // Keywords that map to existing tokens
    KeywordMap *keyword_overrides; // "elif" -> tkELSE+tkIF, "elsif" -> same
};
```

## Concrete Syntax Examples

### C-style (current default)

```c
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
```

### Python-style mode

```python
def factorial(n: int) -> int:
    if n <= 1:
        return 1
    return n * factorial(n - 1)
```

Lexer changes: `#` comments, `:` opens blocks, newline terminates
statements, indentation increases/decreases emit `tkOpBrc`/`tkClBrc`
tokens (virtual braces).

Parser changes: `def` → function declaration, `name: type` → parameter
with type annotation (reversed from C), `-> type` → return type suffix.

### Ruby-style mode

```ruby
def factorial(n)
    if n <= 1
        return 1
    end
    return n * factorial(n - 1)
end
```

Lexer changes: `#` comments, `end` closes blocks, newline terminates.

Parser changes: `def`/`end` for functions, `if`/`end` for conditionals,
no type annotations (inferred or explicit via separate syntax).

### Rust-style mode

```rust
fn factorial(n: i32) -> i32 {
    if n <= 1 {
        return 1;
    }
    n * factorial(n - 1)
}
```

Lexer changes: `//` comments (same as C). `fn` keyword.

Parser changes: `fn` → function, `let` → variable, `name: type` suffix
syntax, `-> type` return type, last expression is implicit return.

## Implementation Strategy

### Phase 1: Syntax profile infrastructure (2-3 weeks)
- Define `SyntaxProfile` struct
- File extension → profile mapping
- Configure lexer comment style from profile
- Configure statement terminator (`;` required vs optional)
- `#pragma syntax python` directive to switch mid-file (or per-file)
- Gate: C code still works identically (default profile)

### Phase 2: Python-style indentation (3-4 weeks)
- Indentation tracker in lexer: track indent level per line
- Indent increase → emit virtual `{` (tkOpBrc)
- Indent decrease → emit virtual `}` (tkClBrc)
- Newline as statement terminator (emit virtual `;`)
- `def` keyword → function declaration
- `:` after if/for/while/def → block opener
- `#` comment style
- Gate: simple Python-style programs compile and run

### Phase 3: Type annotation variants (2 weeks)
- Suffix type syntax: `name: type` for parameters and variables
- Return type suffix: `-> type` after parameter list
- `let` / `var` variable declaration with type inference
- Gate: `def add(a: int, b: int) -> int:` compiles

### Phase 4: Ruby/Rust profiles (2-3 weeks each)
- `end` keyword as block closer (Ruby)
- `fn` keyword + Rust syntax conventions
- Implicit return (last expression value)
- Gate: Ruby-style and Rust-style programs compile

### Phase 5: Mixed-syntax files (2 weeks)
- `#pragma syntax python` / `#syntax c` directives switch mode within a file
- Each `#include` can have its own syntax (C headers stay C)
- Namespace blocks can specify syntax: `python:: { ... }` uses Python parsing
- Gate: mixed C + Python in one program

## Interaction with Existing Plans

### libmadcedit syntax highlighting
The editor already needs per-language highlighting. With syntax profiles,
the editor's syntax definition directly mirrors the parser's profile:
```c
SyntaxProfile *profile = syntax_profile_for_extension(".py");
madcedit_set_syntax_from_profile(editor, profile);
// Highlighting uses the same comment/string/keyword rules as the parser
```

### Rendering abstraction
The `render { }` block syntax could have language-specific variants:
```python
# Python-style render block
render:
    nav:
        item("File", show_file_menu)
    main:
        heading("Welcome")
        if logged_in:
            text(user.bio)
```

### Pre-compiled headers
Each syntax profile may need its own standard library prelude.
Python-mode might auto-import `range()`, `print()`, `len()`.

## What Changes Per Syntax Profile

| Aspect | C | Python | Ruby | Rust |
|--------|---|--------|------|------|
| Block delimiters | `{ }` | indentation | `do`/`end` | `{ }` |
| Statement terminator | `;` | newline | newline | `;` |
| Function keyword | none (type name) | `def` | `def` | `fn` |
| Variable keyword | type name | `=` (inferred) | `=` (inferred) | `let` |
| Type syntax | prefix (`int x`) | suffix (`x: int`) | none / suffix | suffix (`x: i32`) |
| Return type | prefix (`int foo()`) | suffix (`-> int`) | none | suffix (`-> i32`) |
| Comments | `//` `/* */` | `#` `"""` | `#` `=begin` | `//` `/* */` |
| Boolean | `true`/`false` or `1`/`0` | `True`/`False` | `true`/`false` | `true`/`false` |
| None/null | `NULL`/`0` | `None` | `nil` | `None` |

## What Does NOT Change

- **The AST.** `TokenIF`, `TokenFOR`, `TokenFunc`, etc. are the same
  regardless of source syntax. A Python `if` and a C `if` produce the
  same AST node.
- **The compiler.** x86-64 code generation is syntax-independent.
- **The type system.** `DataDef`, `Variable`, `FuncDef` are unchanged.
- **The IR.** `IRValue`, `IRBuilder` operations are the same.
- **The namespace system.** `php::strlen()` works in any syntax mode.
- **The rendering abstraction.** `render { }` blocks work the same.

The principle: **syntax is skin-deep.** Different languages spell the
same concepts differently. The compiler doesn't care about spelling.

## Prerequisites

- Track 1.2 (code cleanup) — parser needs to be cleaner before adding
  syntax variants
- Track 1.4 (parser cleanup Phase B) — dereference/subscript unification
  reduces the surface area for syntax variants

## Non-Goals

- Full Python/Ruby/Rust language semantics (that's a different language)
- Python's dynamic typing (madc remains statically typed)
- Ruby's metaprogramming (define_method, method_missing)
- Rust's borrow checker or lifetime annotations
- Compatibility with existing Python/Ruby/Rust libraries (different ABI)

The goal is SYNTAX, not SEMANTICS. Write madc programs in Python-like
syntax, compile to the same JIT code as C-style madc.
