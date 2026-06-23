# Design — instantiate templates by AST-copy+substitute, NOT by re-parsing (the parity lever)

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **Status:** DESIGN (recon in progress)

> GOAL (user): madc must **lex+parse+instantiate FASTER than g++**. g++ does `testsubscript`
> (parse+sema+instantiate, stdlib from scratch) in ~0.5 s; madc ~2.0 s (~4×). This lever is the
> PRIMARY one that closes the structural 4× — see the priority order in
> `docs/plans/2026-06-23-p1-token-arena-implementation-plan.md` §5. Forest is a LATER, separate
> win (it goes BELOW g++ by skipping the stdlib parse); it does NOT substitute for this.

## READ-CHECK (answer before acting)
1. Why is madc ~4× g++ on `testsubscript`? → **87% of parse is template instantiation**, and madc
   **RE-PARSES** the template body tokens for EACH instantiation. g++/clang never re-parse.
2. How does madc instantiate today? → `instantiate_template_use` (parser.cpp:3469) copies
   `TemplateDef.body` (a **`vector<TokenBase*>`** — tokens), substitutes into the tokens, splices
   them into the lexer stream, and **runs the recursive-descent parser again**; type resolution
   (sema) happens DURING that re-parse.
3. What's the target? → Parse each template body **ONCE** into a parameterized `cir_node`
   "pattern"; instantiate by **clone-subtree + substitute params (type-id remap) + re-resolve
   ONLY the type-dependent nodes**. No parser invocation per instantiation.
4. Is this the forest? → **NO.** Forest-independent, in-memory, helps live project templates.
   Generalizes the landed lazy member-body work (today *defers* the parse → target *never re-parses*).
5. What must change structurally? → madc **entangles parse and sema** (recursive descent resolves
   types as it builds). This lever requires a boundary: syntactic-parse-to-pattern (params as
   placeholders, dependent resolution deferred) vs per-instantiation substitute+resolve.
6. Does it depend on the flat-representation work? → For the CHEAP version, yes: `cir_node` in an
   arena with `uint32` handles + types as `type_id`s makes "substitute" an index remap. But the
   model change (parse-once, copy+substitute) is the load-bearing part; representation makes it fast.

## Current madc model (traced 2026-06-23 — the thing to replace)
- A template body is stored as **tokens**: `Program::TemplateDef::body` = `std::vector<TokenBase*>`
  (madc.h ~1385). Definition-time parsing captures tokens, not a parsed tree.
- `instantiate_template_use` (parser.cpp:3469): copies the `TemplateDef`, parses `<args>` resolving
  type params to `DataDef`s, then (the substitution loop ~parser.cpp:3913) walks `td.body`,
  builds an injected token vector `inj` (cloning unchanged tokens, substituting params), splices
  `inj` into the stream, and the **parser re-runs** over it to build the class/`DataDef`.
- Sema is inseparable from that re-parse (types resolved inline as the body parses).
- Memoization exists (`datatype_map[mangled]`) but is heuristic/fragile (the `is_incomplete_*`
  completeness checks, parser.cpp:3307-3330) — when it misfires it re-instantiates (e.g.
  `iterator_traits<uint32_t>` instantiated 6× for an empty `<iostream>` program, parser.cpp:3318).
- Lazy path: `deferred_lazy_bodies` + `parse_deferred_lazy_body` + `materialize_and_lower`
  (cir_builder.cpp:12649) — DEFERS body parse to ODR-use, but still **re-parses** when triggered.

## Target model (parse-once, copy+substitute) — to be detailed from recon
1. **Parse the body once → a parameterized pattern.** Template params are placeholder nodes
   (type-param → a type-ref placeholder; non-type → a value placeholder). Dependent constructs
   are marked, their resolution deferred. (This is the syntactic/sema split.)
2. **Instantiate = clone pattern + substitute + re-resolve dependent-only.** Deep-copy the pattern
   subtree, remap placeholders to the concrete args (type-id for types, value for non-type),
   re-run sema ONLY on the dependent nodes (dependent name lookup, overload resolution, layout).
   No re-lex, no re-parse.
3. **Memoize by `(template, canonical-arg-env)`** — replace the fragile mangled-name+completeness
   heuristic with a canonicalized env key so `vector<int>` instantiates exactly once.

## RECON IN PROGRESS (2026-06-23) — three background agents; fill this section on return
- **GCC** (`/workspace/gcc`, `gcc/cp/pt.cc`): how `tsubst`/`instantiate_template`/`instantiate_decl`
  substitute into saved GENERIC trees; dependent split (`processing_template_decl`,
  `dependent_type_p`); spec hash tables. → techniques: _TBD_.
- **Clang** (`/workspace/llvm-clang-src`, `lib/Sema/SemaTemplateInstantiate*.cpp`): the
  `TreeTransform`/`TemplateInstantiator` rebuild-by-transform; `TypeDependent`/`ValueDependent`
  bits; specialization FoldingSets. → techniques: _TBD_.
- **tinycc** (`/workspace/tinycc`, `tccpp.c`): token = int id in a flat stream; `TokenSym`
  interning; macro body = saved token-id sequence replayed without re-lex (the closest analogue
  to cheap body reuse); `isidnum_table` lex hot loop. → techniques: _TBD_.

## Sequencing & non-goals
- This is PRIMARY (parity), but it sits ON the flat-representation substrate (token-arena plan #2)
  for the cheap version. Build the substrate far enough that `cir_node` subtrees clone+remap
  cheaply, then convert instantiation from re-parse to copy+substitute.
- Honest scope: separating madc's entangled parse/sema is the biggest single front-end change —
  the heart of a real C++ front end. Sequence it deliberately; it is multi-session.
- Non-goal: do NOT try to keep the re-parse path "as a fallback" alongside the new one (parallel
  impls drift). The seam is a replacement.
- After the structural 4× is gone, closing the last stretch to g++'s absolute speed is
  constant-factor tuning (lexer, name lookup) — informed by the tinycc/gcc/clang recon.
