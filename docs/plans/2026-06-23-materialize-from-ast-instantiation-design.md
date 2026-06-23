# Design — instantiate templates by AST-copy+substitute, NOT by re-parsing (the parity lever)

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **Status:** DESIGN (recon COMPLETE — gcc/clang/tinycc; ready to sequence)

> GOAL (user): madc must **lex+parse+instantiate FASTER than g++**. This lever is the PRIMARY
> one that closes the structural 4× — see the priority order in
> `docs/plans/2026-06-23-p1-token-arena-implementation-plan.md` §5.
>
> **SUCCESS METRIC (user, exact):** `madc(lex + parse + cir-build) ≤ g++ -fsyntax-only -O0`
> (at minimum MEET; ideally BEAT). It is the front-end-to-front-end comparison: madc's three
> `--show-stats` phases **lex + parse + cir build**, vs g++ `-fsyntax-only` (no codegen) — so
> c2mir-compile and execution are EXCLUDED. Measured baseline (`testsubscript`): madc
> 0.46+1.10+0.38 = **1.94 s** vs g++ **0.50 s** (~3.9× to close). HOW: T1-T6 + lex L2-L5 get to
> MEET (from-scratch parity); the **forest** (skip stdlib parse) gets to BEAT. Re-measure with
> `--show-stats` (sum the three phase lines) vs `g++ -fsyntax-only -O0` on the same TU, idle box.

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

## Target model (parse-once, copy+substitute) — VALIDATED by gcc+clang recon (see RECON RESULTS)
1. **Parse the body once → a parameterized pattern.** Template params are placeholder nodes
   (type-param → a type-ref placeholder; non-type → a value placeholder). Dependent constructs
   are marked, their resolution deferred. (This is the syntactic/sema split.)
2. **Instantiate = clone pattern + substitute + re-resolve dependent-only.** Deep-copy the pattern
   subtree, remap placeholders to the concrete args (type-id for types, value for non-type),
   re-run sema ONLY on the dependent nodes (dependent name lookup, overload resolution, layout).
   No re-lex, no re-parse.
3. **Memoize by `(template, canonical-arg-env)`** — replace the fragile mangled-name+completeness
   heuristic with a canonicalized env key so `vector<int>` instantiates exactly once.

## RECON RESULTS (2026-06-23) — gcc, clang, tinycc all agree on the model

**Convergent verdict:** gcc and clang both store the template body as a **fully-parsed AST**
(gcc `DECL_SAVED_TREE` on the template's result decl, cp-tree.h:5563; clang
`PatternDecl->getBody()` → `Stmt*`) and instantiate by **transform-and-substitute over that AST**
(gcc `tsubst_stmt`/`tsubst_expr`, pt.cc:19604/21386; clang `SubstStmt` → `TreeTransform`,
SemaTemplateInstantiate.cpp:4091). **Neither ever re-lexes or re-parses a body per instantiation.**
Both keep a token buffer for inline member bodies ONLY until the class closes, then parse once to
AST and discard the tokens (gcc `cp_parser_save_member_function_body` parser.cc:36757; clang
`LateParsedTemplate` SemaTemplate.cpp:11593). This is exactly madc's target.

### Core template techniques (gcc + clang), each mapped to madc
| # | Technique | gcc | clang | madc action |
|---|---|---|---|---|
| T1 | **Body parsed once → AST stored on the template** | `DECL_SAVED_TREE` (pt.cc:28709) | `getBody()`→`Stmt*` (SemaTI-Decl.cpp:4936) | Store the parsed `cir_node`/parse-subtree on the template `DataDef`; STOP storing only `TemplateDef.body` tokens for re-parse |
| T2 | **Substitute by `(depth,index)` array lookup — NOT by name** | `TMPL_ARG(args,level,idx)`; params carry `TEMPLATE_PARM_LEVEL/IDX` (pt.cc:17264) | `MultiLevelTemplateArgumentList(depth,pos)` (Template.h:76); `TransformTemplateTypeParmType` (SemaTI.cpp:1436) | Tag every param-ref node with `(depth,idx)`; substitution = `args[depth][idx]`. **Replaces madc's substitute-by-matching-param-name-in-tokens.** O(1) per ref |
| T3 | **Copy-if-changed walk + dependence short-circuit** | dependent flags `TYPE_DEPENDENT_P` cached (pt.cc:29649) | `TreeTransform` returns original node if no child changed (TreeTransform.h:7567); `AlreadyTransformed` skips non-instantiation-dependent subtrees (SemaTI.cpp:1585) | Add `is_instantiation_dependent` bit to the node, set bottom-up at parse; the instantiation walk SKIPS non-dependent subtrees (returns the shared node) ⇒ **O(n_dependent), not O(n_body)** |
| T4 | **Memoize by (template, canonical args), pre-hashed, checked FIRST** | `decl/type_specializations` spec_entry w/ cached hash (pt.cc:121,1272) | `FoldingSetVector<…SpecializationInfo>` + `Profile()` (DeclTemplate.h:986) | Replace the fragile `datatype_map[mangled]`+`is_incomplete_*` heuristic (parser.cpp:3307-3330) with `map<(tmpl*, canonical-arg-fingerprint) → DataDef*>` checked before ANY work |
| T5 | **Two-phase: resolve non-dependent at parse, dependent at instantiate** | `processing_template_decl` counter (pt.cc:758) | dependence bits computed bottom-up (DependenceFlags.h; ComputeDependence.cpp) | A "in-template-body" parse mode that defers dependent name/overload resolution but resolves+shares non-dependent names once |
| T6 | **Per-instantiation local map: pattern param-decl → concrete** | `local_specializations` hash_map (pt.cc:89) | `LocalInstantiationScope` (Template.h:365) | Scoped map `pattern-param DataDef* → concrete DataDef*` for the clone walk |
| T7 | **Lazy body instantiation** (layout eager, bodies deferred to ODR-use) | `instantiate_decl` deferral (pt.cc:28760) | `PendingInstantiations` queue (SemaTI-Decl.cpp:4956) | madc already has `deferred_lazy_bodies`/`materialize_and_lower` — keep, but make it materialize from AST, not re-parse |

### Lex / representation techniques (tinycc) — the SECONDARY lever (token-arena plan)
| # | Technique | tinycc | madc action |
|---|---|---|---|
| L1 | **Saved token sequence = flat `int[]`, replayed by pointer-advance (NO re-LEX)** | `TokenString`/`begin_macro`/`macro_ptr` (tccpp.c:1053); inline fn bodies use the SAME path (tccgen.c:8648) | madc's `vector<uint32_t>` id-stream (token-arena Phase 3). **One mechanism for macros AND inline bodies.** NOTE: this removes re-LEX (macros); templates additionally remove re-PARSE via T1-T6 |
| L2 | **Branch-free char classification** | `isidnum_table[257]` bitmask IS_ID/IS_NUM/IS_SPC (tccpp.c:51) | Replace `isalpha`/`isdigit` in the lexer hot loop with a 257-byte table; byte-load + AND |
| L3 | **Direct-index macro/define lookup** | `table_ident[tok-TOK_IDENT]->sym_define` O(1) (tccpp.c:1274) | madc's sid-indexed `InternKeyedMap` already matches; store the macro-def pointer keyed by sid (no hash) |
| L4 | **Hash-during-scan, intern once** | rolling hash inline in `next_nomacro` (tccpp.c:2675) | madc ALREADY does this (incremental hash @7d6bc31) — validated |
| L5 | **Slab/bump alloc; token = small id+value, not an object** | `toksym_alloc`/`tokstr_alloc` (tccpp.c:138); `int tok`+`CValue` | madc's `TokenArena` (Phase 1) + flat `TokenRec` already match |

### The two "no re-work" mechanisms are DISTINCT — madc needs BOTH
- **Macros + inline function bodies → no re-LEX** (tinycc L1): replay a saved id-stream *through the
  parser*. = token-arena Phase 3 (id-vector substitution).
- **Templates → no re-PARSE** (gcc/clang T1-T6): transform an already-parsed AST; the parser never
  runs again. = THIS design.

### Interning the NAME INTO THE AST (cross-cutting, all three)
gcc `IDENTIFIER_NODE`, clang `IdentifierInfo*`, tinycc `TokenSym` int id — all use the interned id
**as the name stored in the AST/decl**, so every name comparison in the instantiation machinery is
pointer/int equality. madc has `StringPool` (token-level); extend it so AST/`DataDef` names carry
`spelling_id`, not `std::string` — this pays off heavily inside the substitution walk.

## STEP 0 — PROFILE RESULTS (callgrind testsubscript, -O0, 2026-06-23) — measure-first CORRECTED both narratives
**Ir inclusive:** `parse` 54.5% · **lexing (`tokenize`/`_getToken`) ~45%** · **`instantiate_template_use`
29.6%** · `parseFunction` 18.5% · **`findVariable` (name lookup) 7.3%**.
**Top self-cost (-O0):** `Source::get` 3.6%, `_getToken` 3.4%, `Source::peek` 2.8%,
**`std::string::operator+=(char)` 2.7%** (identifiers built char-by-char!), `detect_include_guard`
2.1%, `Source::good/eof` ~2.7%, **`TokenArena` `vector<Chunk>` accessors ~6% combined**, string
`compare`/`==`/`memcmp`/`strlen` ~5%.

**RANKING FROM DATA (this supersedes both unranked narratives):**
1. **Lexing ~45% Ir — biggest single cost, and char-by-char.** `Source::get`/`peek` per char +
   `std::string::operator+=(char)` per identifier char. Fix = tinycc L1/L2: scan the identifier
   SPAN off the flat buffer with `isidnum_table`, intern `[start,len)` directly — no per-char
   method call, no `std::string` growth. (Most concentrated; the span-scan is -O-INDEPENDENT.)
2. **Instantiation ~30% Ir (`instantiate_template_use`) — SPREAD (no hot self-fn) = re-parse "death
   by a thousand cuts."** Attacked by ELIMINATION = materialize-from-AST (T1-T7). CONFIRMS the
   primary lever.
3. **Name lookup ~7-12% (`findVariable` 7.3% + string-key ops ~5%) — REAL but THIRD, NOT "most of
   the 4×."** The external review's RANK was wrong (measure-first earned its keep); C1/C2/C3 are a
   secondary win, do after #1/#2.
Bonus: `TokenArena::alloc()` ~6% -O0 cost in `_chunks.back()`/`end()`/iterator — cheap raw-pointer-
to-current-chunk fix (mostly inlines at -O2). `detect_include_guard` 2.1% self — glance at it.

**CAVEAT — this is -O0; -O2 is the honest ranking.** -O0 inflates the trivial accessors dominating
the self-list (`string::size`, `vector::empty`, `Source::good/eof`, iterator ctors, arena vector
ops) — they inline to nothing at -O2, which is how g++ (a -O2 binary) runs. **-O2 re-profile DONE
(see "PROFILE RESULTS AT -O2" below):** it confirmed the ranking, shrank the accessor noise to
reveal ~40% of instructions are `std::string`+`std::map`+`malloc` machinery, and RAISED
`findVariable` 7.3%→12.2% (interning is a heavier lever at ship config than -O0 showed).

## STEP 0 — PROFILE RESULTS AT -O2 (the ship config; 2026-06-23) — confirms "remove the machinery"
Total Ir 2.19B (vs 4.06B at -O0 — ~half inlines away). With accessor noise gone, the residual IS
the C++ machinery, in the THREE categories the architectural principle names:
- **malloc/free ~15% self** (`_int_free` 4.7, `malloc` 3.7, `_int_malloc` 3.5, `free` 2.1,
  `operator new` 1.2) — biggest category; `std::string`-per-token + node alloc.
- **`std::string` churn ~11%** (`despace_spelling` 3.2 ⟵ single-fn hotspot, `push_back(char)` 2.5,
  `memcpy` 2.6, `_M_construct`/`_M_assign`/`_M_append` ~2.8).
- **string compare+hash ~15%** (`__memcmp` 5.5, `operator==(string,char*)` 1.9, `_Hash_bytes` 2.8,
  `_Rb_tree<string>::find` ×2 ~1.8 incl. `datatype_map`, `strcmp`/`strlen` ~3).
- Lexer char loop `Source::get` 5.1 + `_getToken` 4.0 — REAL at -O2. `dynamic_cast` 1.8 +
  MIR/c2mir backend ~2 — confirmed NON-priorities. `TokenBase::operator new` 1.3 (arena now cheap;
  the -O0 ~6% `vector<Chunk>` overhead inlined away as predicted).
⇒ ~40% of -O2 instructions are `std::string`+`std::map`+`malloc` machinery. REMOVE it (flat buffer +
interned `[start,len)` spans + id-keyed lookups), don't tune it.

**-O2 inclusive phase split** (refines, doesn't change, the -O0 ranking): `parse` 36.6%,
`parseFunction` 30.0%, lexing ~30%, `instantiate_template_use` 28.7%, **`translate_module` (CIR
build) 20.0%** (materialize lambda 14.7%), **`findVariable` 12.2%**. TWO refinements:
1. **`findVariable` ROSE 7.3%(-O0) → 12.2%(-O2)** — name lookup matters MORE at ship config; lever
   C2 (intern ALL lookups, incl. sema `_Rb_tree<string>` like `datatype_map`) is HEAVIER than -O0
   implied. (External review was directionally right at -O2.)
2. **CIR build 20% inclusive** — newly visible; part of the gate (lex+parse+**cir-build**); keep on radar.
Artifacts: `scratchpad/cg_O2.out`, `cg_O2_self.txt`, `cg_O2_incl.txt`. NOTE: bin/madc is now -O2;
rebuild -O0 (`make -C src`) when resuming dev if the -O0 default is wanted.

## STEP 0 — PROFILE FIRST (the ranking gate; do BEFORE any implementation)
**Two analyses agree on the costs but not on the rank, and BOTH say measure.** Our `--show-stats`
shows instantiation = 87% of parse (the re-parse). An external cross-check (claude.ai, 2026-06-23,
reasoning from code structure — UNMEASURED) attributes "most of the 4×" to **name lookup**: bare
`std::string` identifiers (no interning) + ~170 string-keyed `std::map` (RB-tree) sema tables +
`findVariable` re-hashing the same name up the scope chain. These are NOT in conflict — the
**re-parse INVOKES the string-keyed lookups repeatedly**; materialize-from-AST removes the
re-invocation (T3/T5), interning makes the residual lookups cheap. They compound. What we LACK is
function-level attribution. So:
- **Step 0 = callgrind a representative TU (`testsubscript`), record top self-cost frames.** It
  decides the ORDER: if `std::map::find`/`std::string::compare`/`findVariable` dominate → do the
  interning + `unordered_map` levers first (cheaper, helps even pre-materialize); if parser
  recursive-descent / cir-build dominate → materialize-from-AST first. Fix the table that is
  actually hot, not all 170.

### Cross-checked constant-factor levers (the SECONDARY lever, beyond tinycc L1-L5)
The external review correctly flags that our interning so far is LEXER-side; the SEMA tables are
still string-keyed `std::map`. Confirmed in-code. Levers, cheap→deep:
- **C1 (cheap, low-risk):** mechanically convert proven-hot `std::map<std::string,…>` sema tables
  (`lazy_map`, `type_aliases`, `static_member_types`, namespace maps) → `unordered_map`. Keeps the
  string-key model (no call-site refactor); kills the RB-tree char-compares. Do only the
  profile-proven-hot ones.
- **C2 (the real fix, = the cross-cutting "intern the name INTO the AST"):** key the sema maps on
  `spelling_id`/atom, not `std::string`. Give `Variable`/`DataDef` an atom field. Stage: intern at
  lex (DONE: StringPool), migrate hottest sema tables first. This is the g++/clang model.
- **C3:** `findVariable` parent-chain — compute the name's atom/hash ONCE, pass it up the scope
  chain instead of re-hashing per level (deep scope chains in template code).
- **C4 (SUPERSEDED by T2):** the per-instantiation string-keyed subst map is REAL —
  `parser.cpp:3942` (`std::map<std::string,…> subst; … subst.find(s)` per param ref). T2 (substitute
  by `(depth,index)` array lookup) eliminates the map entirely — strictly better than re-keying it.

## Sequencing & non-goals
- Step 0 (profile) FIRST — rank the levers above (C1-C3 + materialize-from-AST T1-T7) by measured
  payoff. Do not implement on either narrative alone.
- This is PRIMARY (parity), but it sits ON the flat-representation substrate (token-arena plan #2)
  for the cheap version. Build the substrate far enough that `cir_node` subtrees clone+remap
  cheaply, then convert instantiation from re-parse to copy+substitute.
- Honest scope: separating madc's entangled parse/sema is the biggest single front-end change —
  the heart of a real C++ front end. Sequence it deliberately; it is multi-session.
- Non-goal: do NOT try to keep the re-parse path "as a fallback" alongside the new one (parallel
  impls drift). The seam is a replacement.
- After the structural 4× is gone, closing the last stretch to g++'s absolute speed is
  constant-factor tuning (lexer, name lookup) — informed by the tinycc/gcc/clang recon.
