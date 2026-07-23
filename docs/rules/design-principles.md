# Design Principles — Reasoning

See `.claude/rules/design-principles.md` for the rules themselves.

## Why these principles, for this project

madc is a compiler — one of the most leverage-sensitive codebases there
is. A small coupling mistake in the parser leaks into every token; a
misplaced concern in the compiler spreads to every codegen site. The
only way to keep the project tractable at ~20k lines is to enforce
separation of concerns at every level.

## Separation of concerns — worked examples

### Good

- `src/lexer.cpp` tokenizes. `src/parser.cpp` parses. `src/compiler.cpp`
  emits code. Each is replaceable; each has its own bugs, its own tests,
  and its own rules files.
- `src/ns_php.cpp` implements only `php::` functions. When someone
  changes `perl::grep`, they don't need to understand the PHP module.
- Test runner only knows the filename convention for fixtures. Each
  test's input is in `tests/foo.input`, its expected output in
  `tests/foo.expect`. The runner is trivially mechanical.

### Bad (what we avoided)

- An earlier iteration of the test runner hard-coded `case testcin.mad)
  echo "Alice 42 …"`. Adding a second stdin test would have meant
  editing the runner and the test. Now both tests just live with their
  fixtures. The runner didn't change.
- A pre-CompoundLHS design had three copies of the "load member, op,
  store member" pattern in `+= -= *=` — each had a subtly different
  treatment of sub-qword types. Consolidating into `resolveCompoundLHS`
  made the size-aware load fix a one-site change.

## High cohesion — worked examples

### Good

- `.claude/rules/embedded-headers.md` and `docs/rules/embedded-headers.md`
  live together; one contains the constraints, the other the why.
  A change to the embedded-header rule touches both in one place.
- `tests/testcin.mad` ships with `testcin.input` and `testcin.expect`
  as siblings.

### Bad (anti-pattern to avoid)

- Scattering one feature's documentation across `docs/usage.md`,
  `docs/architecture.md`, `docs/language/foo.md`, and several
  `CHANGELOG.md` entries. If a feature is cohesive, document it as
  one thing.

## Low coupling — worked examples

### Good

- `DataDef` virtual methods (`movrval2mptr`, `is_pointer`, `is_numeric`,
  etc.). `DataDefPTR` overrides what it needs; callers never branch on
  the concrete type.
- The embedded-headers system: adding a new header means dropping a
  file in `include/madc/`. The build regenerates; no C++ changes.
- The current `libmadc` direction reuses one C++ implementation across
  three surfaces: CLI, native embedding, and the future C shim. That is
  the project-level form of the 3R credo: **reuse, reduce, recycle**.
  Reuse existing machinery before inventing parallel abstractions,
  reduce duplicated logic and policy models, and recycle one coherent
  implementation across user-facing surfaces.

### Bad (anti-pattern)

- A pre-cleanup parser had a list of "special function names" that
  needed custom parsing. Every new function meant editing the parser.
  Replaced by a generic `addFunction()` registry.

## OOP principles — how they play out here

### Encapsulation

`Token::_operand` is the node's private state. Callers go through
`operand()` / `compile()` — they do not reach in and read `_operand`
directly. If a subclass needs to cache something, that's its business.

### Single responsibility

Every `TokenX` represents one AST node kind. `TokenInc` handles `++`,
full stop. It delegates to `resolveCompoundLHS` rather than re-implementing
member access. The same helper is reused by `TokenAddEq`, `TokenSubEq`,
and 8 others.

### Open/closed

Adding a new PHP function: drop it in `ns_php.cpp` and register. No
changes to the parser, compiler, or runtime required.

Adding a new stdin-driven test: drop a `.input` file. No runner changes.

### Liskov substitution

Every `TokenBase::compile` returns `Operand &` and sets `regdp.first`
to a valid operand. Every `DataDef` reports `size` and `type()`
consistently. Breaking these would cascade failures everywhere.

### Dependency inversion

The compiler depends on `DataDef *`, not on specific `DataDefSTRUCT`,
`DataDefPTR`, `DataDefVECTOR` subclasses. It asks "is_numeric?",
"is_pointer?", "size?". That's how you add a new type without editing
the compiler.

## Red flags

If any of these are true, stop and redesign:

- "I need to add a special case for X in this general function"
- "To add feature Y, I need to edit files A, B, C, D, E"
- "This script has a list of project-specific names"
- "I can't test this module without running the whole pipeline"
- "Changing this private thing breaks unrelated tests"

## How to apply in practice

- Before adding a branch: can this be a lookup in a registry or a
  filename convention?
- Before duplicating code: where does the shared concept belong?
- When documenting: rules in `.claude/rules/`, reasoning in
  `docs/rules/`, reference material in `docs/language/` or
  `docs/architecture.md`. Never mix.
