# Design Principles

Apply these to every change — new code, refactors, test layout, build scripts, anything.

## Separation of concerns

- Each module, class, function, script, or file does one thing.
- If a helper needs to know about an unrelated concern (e.g. a test runner
  knowing a specific test's input), that knowledge belongs elsewhere.
- Do not cross layer boundaries: parsers parse, compilers emit code,
  namespace files implement their own namespace only.

## High cohesion

- Put things that change together, together.
- A feature's code, its tests, its fixtures, and its documentation should
  live near each other — not scattered across the tree.
- A function should do one well-named thing; if it needs a topic sentence
  in a comment to explain, split it.

## Low coupling

- Depend on the narrowest contract that works.
- Prefer filename / type / registry conventions over hard-coded lists.
- A change in one component should not force edits to unrelated components.
- If adding a new X requires editing N unrelated places, the design is wrong.
- Follow the 3R credo: reuse existing machinery, reduce duplication, recycle one implementation across surfaces.

## Object-oriented principles

- **Encapsulation:** private state stays private; expose behaviour, not
  internals. `TokenX::compile()` owns how X compiles — callers do not
  manipulate X's internals.
- **Single responsibility:** a class represents one concept. If a `Token`
  subclass both parses and emits three unrelated node types, split it.
- **Open/closed:** prefer extending via new subclass / new fixture file
  / new registry entry over editing a central switch statement.
- **Liskov substitution:** subclasses must honour the base contract
  (e.g. every `TokenBase::compile` returns a valid `Operand &`).
- **Dependency inversion:** depend on the abstract shape (`DataDef`,
  `TokenBase`, filename convention), not on the concrete instance.

## No hard-coding of specifics into general machinery

Generic infrastructure must never special-case individual consumers.

- Test runner: no `case "$base" in testfoo) …`. Use fixture files.
- Parser: no `if (token->name == "specialFunction")`. Use flags or types.
- Compiler: no string comparisons against user-supplied names to trigger
  codegen paths. Use type / flag predicates.

## How to apply

- Before adding a `case` / `if-else` branch, ask: can this be a data
  lookup, a type predicate, or a filename convention instead?
- When a file grows past ~500 lines of code or mixes two topics, split it.
- When two components both know the same fact, one of them is wrong —
  pick the owner and delete the duplication.

See `docs/rules/design-principles.md` for the reasoning and worked examples.
