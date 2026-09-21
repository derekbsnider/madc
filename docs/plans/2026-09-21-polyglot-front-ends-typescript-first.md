# Polyglot front ends — the front-end seam, TypeScript first

**Status:** DESIGN, owner-signed in dialog 2026-09-21. Not started.
**Supersedes:** nothing. This is the first *plan* for the track that
`docs/plans/madc-vision-and-invariants.md` (the vision) and
`docs/plans/2026-09-03-frontend-architecture-recon.md` (the shape) prepared, and
that `docs/plans/cpp-support.md` P3 lists as "far-future direction".

---

## 1. The rulings this is built on (owner, 2026-09-21)

Five, in the order they were made. Each one closes a door; the design is what
is left standing.

1. **No runtime dependency unless absolutely necessary.** Support a compatible
   SUBSET first, and leave the door open to fasten adaptors where a foreign
   runtime is genuinely required. This keeps the vision's *lowering canon*:
   madc lowers borrowed semantics and implements them in libmadc; it does not
   call CPython/libnode.
2. **No double parsing.** Not a TypeScript→MC11 transpiler plus an MC11 reader.
   The TypeScript lexer+parser manipulates the `cir_node` tree DIRECTLY.
3. **madcide must lex TypeScript in the editor**, so the editor's reader IS the
   compiler's reader — the standing law that the running madc is the compiler
   and the live parse handle is the compilation. Two readers for one language in
   one product is the parallel-implementation rule.
4. **TypeScript first**, ahead of Python.
5. **`madc_value` is available as the fallback** where no specific type is
   defined.

Ruling 2+3 together are what killed the alternative this design first proposed
(an out-of-process front end driving `tsc`'s checker and emitting MC11 text).
It is recorded here because it is the attractive wrong answer: it gets real
types for free from the reference implementation, and it costs a second
TypeScript reader plus a text round trip. Do not revive it without
re-litigating rulings 2 and 3.

## 2. Why TypeScript first (the reasoning, not just the ruling)

- **Its source already carries the types MC11 needs.** MC11 is a typed C AST. A
  TypeScript front end can be a *type-directed lowering*; a Python front end
  must first be an *inference engine* (which is what Cython needs `cdef` for
  and what Shedskin does with whole-program analysis). That is the difference
  between a bounded project and a research project.
- **The useful subset is a far larger fraction of the real language.** Exclude
  `any`, `eval`, prototype surgery and dynamic property addition and most
  hand-written application TypeScript still compiles. Exclude Python's
  metaclasses, descriptors, `__getattr__` and monkey-patching and what remains
  is not Python.
- **It fits what this repo already is** — a VS Code extension, an LSP face and
  the Nexus. The IDE ecosystem's own language becoming a first-class madc input
  is coherent rather than a stunt.
- **AssemblyScript is the existence proof** that a TypeScript-shaped language
  compiles AOT with no JS runtime — while being honest that it got there by
  changing the number model (`i32`/`i64`/`f64` as real types), i.e. it is
  TS-like, not TS. We are making the same category of call (§6).

## 3. What already exists — measured 2026-09-21, do not rebuild it

| asset | where | note |
|---|---|---|
| `fkTYPESCRIPT` | `include/madc/bits/file_kinds` | ALREADY in the enum, in the `fkOTHER` = "an unlisted language" range. madcide classifies a `.ts` file correctly today and then edits it as text because nothing claims the kind. The whole `fkOTHER` range is the queue of languages awaiting a front end |
| range-structured kinds | same file | membership is a range test, so "which front end serves this kind" is a TABLE LOOKUP, not a ladder — the selection mechanism the recon prescribed (gcc's `-x`) already has its data structure |
| the external-tree precedent | `third_party/mir/c2mir/c2mir_api.h` | the whole seam that lets madc feed c2mir a tree it built itself is ~10 functions: `c2mir_next_uid`, `c2mir_uniq_str`, `c2mir_init_node_ops`, `c2mir_node_first_op`/`next_op`/`node_op`, `c2mir_copy_tree`, `c2mir_op_remove`, `c2mir_compile_tree`, plus two dumpers. **It is small because `cir_node` DERIVES FROM `node_t`** — madc allocates nodes of c2mir's own type, so the API never needed constructors. A front end with its own tree type needs the part we never had to build (§5) |
| the node factory | `CirBuilder::make(c2mir_node_code_t, TokenBase *origin)` | ONE factory, `src/cir_builder.cpp:185`. The 33k lines around it are the C++→CIR *lowering*, not the API |
| type identity + value ABI | `docs/plans/2026-06-12-type-table-value-abi-design.md` §6 phases 1–2 **LANDED** | `uint32` typeid is canonical, in fixed segments (primitives `[1,0x100)` ABI-pinned, system forest, project from `0x01000000`); derived types are table ENTRIES not tag arithmetic; the 32-byte `madc_value` carries its type as data with `LOCKED`/`COERCE`/`NULLABLE`/`CONST`, refcounted cells, SSO at 15 bytes + NUL. Its own §1 names "the polyglot vision … loosely-typed frontends (PHP/Perl/Python/JS)" as consumer #3, and `COERCE` is documented as adopting the source's typeid "PHP/JS-style" |
| the `<` disambiguation | `.claude/rules/delimiter-tracking.md`, `DelimDepth` | whether `<` opens an argument list is a NAME question answered by lookup ([temp.names]/3), never by the preceding token; the tracker carries the Program handle so the lookup reaches it. **TypeScript has the identical ambiguity** (generic argument list vs less-than). The nastiest piece of TS parsing is already paid for |
| validated IR mutation | `include/madcdis/verbs.h`, the Nexus | "EVERY MUTATION" is already funnelled through verbs with a change journal. Prior art for the posture in §5: construction is validated, not memcpy'd |
| `--show-stats` | `src/madc.cpp` | already prints input/token/timing stats. The fallback tally in §6 extends it rather than inventing a channel |
| `--emit=mc11` write side | `src/madc.cpp:306` | renders the tree as MC11 text. `fkMC11` is registered in `src/file_kinds.cpp` but **nothing consumes it** — grep `fkMC11` outside the table returns zero hits. The read half is SHELVED by ruling 2; it remains useful for round-trip testing and is not on this plan's path |

## 4. The front-end seam

A **front end** owns lex + parse + name/type resolution for one or more
`file_kind`s and produces `cir_node`. Everything downstream is unchanged,
because the middle and back end are already language-blind: the tree *is* the
interface.

- **Selection is by `file_kind`**, resolved from the file's extension or the
  project manifest, with the `--std=` axis as the explicit override. **No new
  flag**: the vision already puts input language and output target on ONE
  `LanguageStd` enum, and `cpp-support.md` P3 describes exactly this
  generalization ("the input axis becomes *source LANGUAGE + version*"). So
  TypeScript arrives as `LanguageStd` rows, not as `-x`.
- **The C/C++ reader becomes the first implementation of a seam it currently
  *is*.** That refactor is the prerequisite, and it is the one the 2026-09-03
  recon was written to serve: `Program` sheds roles in the order the seams are
  clean (TokenSource first, then lookup/overload/template/type/class services).
  A second front end is the forcing function for it.
- **gcc's `lang_hooks` is the model, ours is far smaller** — gcc's is ~117
  function pointers because its middle end must ask the language about types
  and decls. Ours hands over a finished typed tree.
- **madcide gets TypeScript for free**, because the editor's reader is this
  front end (ruling 3). Highlight spans, the live parse handle, the Nexus's
  node addressing and the REPL tier all come from the same parse.

## 5. The builder surface

What a front end calls to construct the tree. **C++ first, internal first**
(`.claude/rules/cpp-first-api.md`): the TypeScript front end lives in `src/`
and calls `CirBuilder` directly, so **no public/plugin API is built until a
third party actually needs one** — that is a separate decision, deliberately
deferred.

The surface must provide, and no more:

- **node construction with an origin** — `make(code, origin)` already does
  this, and the origin token is not optional: MC11's contract is that every
  node carries its originating tokens and file/line/col
  (`.claude/rules/mc11-ir.md`). A front end that constructs nodes without
  origins breaks reverse-rendering, the IDE's correlation map and every
  diagnostic. This is the single most important constraint on a new front end.
- **operand list plumbing** — the `c2mir_api.h` equivalents, already there.
- **string interning** — `c2mir_uniq_str`, already there.
- **declarations and scopes** — the part that does not exist as an API today
  because madc's parser does it inline. This is the real new work.
- **type references** — `uint32` typeid from the landed table, never
  `DataDef*`, so a front end never holds a pointer into another segment.
- **validation on construction.** A front end is a *producer of trees*, which
  means the tree can be wrong in ways the C++ path structurally cannot be. The
  Nexus verb layer is the posture to copy: refuse an ill-formed construction at
  the seam with a diagnostic, rather than discovering it in c2mir's check phase
  (139 such errors on `c2mir.c` is what that discovery feels like).

## 6. The type model — static first, carrier fallback, MEASURED

Ruling 5 makes `madc_value` available where no type is defined. The design
decision is how hard the front end tries before falling back.

- **Static wherever the annotation or initializer proves it.** Declarations
  with annotations get concrete types; locals infer bottom-up from their
  initializers. No structural-assignability solver, no conditional or mapped
  types, no inference across generic instantiation beyond substitution.
- **Carrier everywhere else** — `any`, unions, un-annotated parameters,
  anything the subset checker declines. This is what makes real TypeScript
  compile on day one instead of after the checker is finished, and it is what
  gives the arc an artifact to diff against `node` from the start.
- **THE GUARDRAIL: every fallback is counted with a reason code, surfaced in
  `--show-stats`, and ratcheted.** If the carrier is the easy path, the static
  path never gets written and TypeScript-on-madc is permanently slow. That is
  the exact crutch failure mode `.claude/rules/parse-once.md` was written
  about, where a re-parse fallback had to be DELETED to force the real
  implementation. Steal its mechanism: a per-file report ("94% statically
  typed, top reason: union type"), a new reason class is a regression the gate
  catches, and the roadmap is a number going down.
- **`string` is lowered from day one, not carried.** It is the most common type
  in any TypeScript program, it is immutable, and its semantics are OBSERVABLE:
  JS strings are sequences of UTF-16 code units and `.length`, indexing and
  `charCodeAt` are defined on them. With `node` as the oracle we cannot quietly
  store UTF-8 and call it the same type. Settle the representation once, early.
  ⚠️ OPEN: UTF-16 buffer vs UTF-8 with a code-unit index adapter — decide with
  the oracle in hand, before the first `string` operation is lowered.
- **`number` is f64 per the language**, with narrowing to integer types where an
  annotation or initializer proves it, driven by the same ratchet.

## 7. The subset boundary — first cut

IN: functions, arrow functions and closures; `class` with single inheritance;
`interface` and `type` aliases as compile-time-only shapes; arrays and object
literals; `enum`; template literals; modules (`import`/`export`) mapped to
madc's own `import`; `for`/`for-of`/`while`; `try`/`catch`/`throw`.

OUT of the first cut, each for a stated reason: `eval` and `new Function` (no
compile-time answer); prototype mutation and dynamic property addition (no
static object layout); decorators (a moving proposal); JSX (a second grammar);
`async`/generators (needs a coroutine lowering — a Tier-1 lowering question in
its own right, not a parser question); namespaces/declaration merging (legacy).

⚠️ This list is a STARTING HYPOTHESIS, not a measurement. The honest version
comes from running the subset against real `.ts` corpora and counting what
refuses — the same discipline that turned "115 constexpr tests" into five
unrelated families. Do that before committing to it.

## 8. Oracle and acceptance

- **`node` is canon**, exactly as gcc is canon for C: the vision's lowering
  canon says match the behavior of L's reference implementation. Compile a
  `.ts` file with madc, run it, diff against `node` running the same file.
- **`tsc --noEmit` is the second oracle** — for type agreement, not codegen: if
  our subset checker types an expression differently from tsc, one of us is
  wrong and it is probably us.
- **A ratcheted lane**, the shape every conformance lane here already has:
  a corpus, a baseline of known failures, RED on any failure outside it, LOUD
  when a baseline entry starts passing. `scripts/c2mir_tests_lane.sh` is the
  most recent template (and carries the fixture-suffix trap in its header).
- **Acceptance for stage 1** is a specific program, named in the plan before
  work starts, that (a) compiles, (b) runs, (c) matches `node` byte for byte,
  and (d) reports its static-vs-carrier fraction.

## 9. Stages

Each stage gets its OWN implementation plan; this document is the design they
share. S0 is the only one that is fully specified here, because S1's size
depends on open question 3 and S4 replaces §7 with a measurement.

- **S0 — the seam.** Extract the front-end interface from the C/C++ reader with
  no behaviour change; the battery is the oracle. `file_kind` → front end
  becomes a table lookup. Nothing new compiles. This is the payment for
  everything after it.
- **S1 — TypeScript lexer.** Tokens, template literals, regex-vs-divide, and
  the `<` question routed through `DelimDepth`'s existing owner. Deliverable is
  madcide highlighting a `.ts` file — user-visible, and it proves ruling 3.
- **S2 — parser to `cir_node`**, carrier-typed throughout, no subset checker:
  functions, control flow, arithmetic, `console.log`. First `node` diff.
- **S3 — the subset checker + the fallback ratchet.** Annotations become real
  types; `--show-stats` reports the fraction; `string` lands lowered.
- **S4 — classes, modules, the corpus lane.** The measurement that replaces §7's
  hypothesis.
- **Later door — adaptors.** A foreign `.so` (a PHP extension, a Node native
  module) binds as a module row in `src/madc_modules.cpp`, the lazy-module shape
  `madcgit`/`madcwebview` already use, with a per-ABI value adaptor over the
  32-byte `madc_value`. Nothing in S0–S4 may assume this never happens, and
  nothing in S0–S4 waits for it.

## 10. Thread-safety contract

Required by `.claude/rules/thread-safety.md`. A front end instance is
**single-threaded and owned by one Program**: concurrent reads of a finished
tree are safe, two front ends over DISTINCT Programs are safe, and one front
end instance driven from two threads is not supported. Construction takes no
locks. The type table's project segment is per-Program (the landed design's
own rule), so a front end never mutates shared state; primitive and
system-segment typeids are immutable and safe to read from anywhere. madcide's
cooperative loop already serializes parse work — a front end inherits that and
adds no new shared mutable state.

## 11. Does this block the vision? (I1–I8 check)

- **One IR** — a front end produces `cir_node` and nothing else; no second tree.
- **No hardcoded standards/targets** — selection is `file_kind` + `LanguageStd`
  rows, data not branches; no new flag, no name tests.
- **One emitter** — untouched; every render target still reads the one tree.
- **No special-casing** — the C/C++ reader becomes an implementation of the
  seam rather than the privileged path; a per-language branch in shared code is
  a violation the gate should catch.
- **Lowering canon** — TypeScript semantics are implemented in libmadc, never by
  linking a JS runtime (ruling 1). The adaptor door is explicitly a door.

## 12. Open questions for the owner

1. **`string` representation** — UTF-16 buffer, or UTF-8 with a code-unit index
   adapter? Observable through `.length`/indexing/`charCodeAt` with `node` as
   oracle, so it is a semantics decision, not an optimization (§6).
2. **Does S0 (the seam extraction) ride this plan, or the front-end
   decomposition the 2026-09-03 recon feeds?** It is the same work; it should
   have one owner, and the recon's version is broader.
3. **How much does madcide need in S1** — highlighting only, or the full live
   parse handle (folding, the Nexus's node addressing, go-to-definition) before
   S2 starts? Ruling 3 makes the editor a first-class consumer; how first-class
   changes S1's size.
4. **Is `import`/`export` mapped onto madc's own `import`**, or kept as a
   TypeScript-local resolution with its own module graph? The first is more
   elegant and constrains both; the second is more faithful to TS resolution
   rules (`node_modules`, `paths`, extensionless specifiers).
