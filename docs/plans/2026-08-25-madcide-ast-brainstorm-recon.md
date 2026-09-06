# madcide AST arc — brainstorm recon (2026-08-25, session 131)

Input material for the owner brainstorm on madcide v2 item 3 (the
AST-in-memory arc: parse-on-load, project open/save via cc.json, the
live project AST feeding highlighting + status-line info) and the three
vision extensions the owner named for it: multi-language INPUT into the
one cir_node tree; VIEW SWITCHING (original ↔ MC11 ↔ C11 ↔ another
language); a binary on-disk .mc11 for fast reload. Two recon passes:
the repo's own record, and a survey of how real IDEs (Turbo-C/Delphi/
Visual Studio → JetBrains/Roslyn/clangd/tree-sitter/MPS) solved the
same problems. Nothing here is a decision — the numbered forks at the
end are the brainstorm's agenda.

## Part A — what our own record already says

### Settled (recorded owner law / SET IN STONE)

- **The polyglot north star** (docs/plans/madc-vision-and-invariants.md):
  madc is ultimately a polyglot transpiler — read language/standard X,
  emit target Y, through the ONE IR. Invariants I3 (never hardcode a
  std/target — the enum), I4 (registry entries), I5 (render targets
  share one IR + enum), I7 (borrowing is native-in-libmadc, lowered to
  the C/C++ AST — never calls into CPython/libperl).
- **MC11-IR** (.claude/rules/mc11-ir.md, owner-dictated 2026-05-29):
  "All render targets (C11, MC11, C++, madc, …) read this one IR,
  selected via the shared language-target enum." Reverse-rendering
  reads the attached tokens + parse subtree — never reconstructed from
  emitted comments. `.mc11` TEXT is the serialization; the live tree
  holds the real structures.
- **The hub doc's demand 3** (2026-08-20): the meta-level lives inside
  the system — "the IDE browses compiler data." IDE-7's recorded shape:
  highlight SPANS arrive as edit-node PROJECTION HINTS (spans as data;
  the renderer never parses).

### Built vs unbuilt (verified in code)

- `LanguageStd` today = STD_MADC + C78..C23 + C++98..26 ONLY
  (include/madc.h:4520). No non-C-family input values exist; P3
  (polyglot input axis) is explicitly "far-future direction" in
  docs/plans/cpp-support.md — unstarted.
- `--emit=` accepts ONLY `c11` and `mc11` (src/madc.cpp:890-901).
  `--emit=madc`/`c++` reverse-render was forward-designed
  (docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md Phase 5,
  keyed on the `synth_from_origin` marker already present on cir_node)
  and never built.
- `madc::diagnostics`/`outline` child Programs are STACK-LOCALS
  destroyed per query (src/madc_program.cpp:4555-4610);
  `madc_project_execute` scopes every TU's Program to one call
  (src/madc_cir.cpp:5897). Persistent parse handles = genuinely new
  lifetime machinery, not wiring.
- `tui_attr` = {normal, reverse} — the colour palette and SGR emission
  are from-scratch work (IDE-7 as recorded).
- The binary forest machinery (`CirFrozenForest`/`DefArena`,
  --freeze/--pack-forest/--forest-bind) serializes a LOWERED type-graph
  — no tokens, no parse subtree, no spellings. It cannot serve an
  editable-AST reload as-is; a binary .mc11 would be a NEW
  serialization carrying the high-level half.

### The cache ruling, exact scope

OWNER RULING (2026-08-22, session 116; docs/plans/
2026-08-21-project-prelude-forest.md lines 203-223 + CHANGELOG):
"no `.forest` caching of user programs — ever... not automatically,
not opt-in; the only frozen forest is the BUILT-IN system-header pack;
a user who wants to skip recompiling produces a real artifact
explicitly (`-c` objects... or `-o` AOT)." The reverted S5 was a
compiler-transparent warm-launch cache (measured 92ms packed thaw),
motivated by JIT launch latency. Nothing in the record addresses an
IDE-explicit "save project session" artifact — same artifact SHAPE,
different trigger (explicit user action) and purpose (session
continuity). Unresolved; fork #3 below.

⚠️ Terminology: "forest" = (a) the literal CirFrozenForest binary
system-header artifact (what the ruling restricts) AND (b) metaphorical
in-memory compiler state ("the IDE browses the compiler's forest").
Keep the senses distinct in the brainstorm.

## Part B — the industry survey (condensed; full report in session log)

Old school: **Turbo C/Pascal** = no persistence at all — one-pass
compile-to-RAM + a symbol table; "instant" came from minimalism.
**Delphi** = per-unit binary `.dcu` caches keyed on interface
dependencies — the old-school precedent for a binary per-TU artifact.
**Visual C++'s 15-year IntelliSense failure arc** is the cautionary
tale: hand-rolled parser (wrong answers) → real-compiler parser; one
monolithic memory-mapped `.ncb` blob (64K-entity ceilings, whole-blob
invalidation, UI-thread reparse freezes) → per-header `.ipch` PCH
caches + a queryable SQLite-ish database. **IBM Montana/CodeStore**
(VisualAge C++): one persistent code store (decls, bodies, template
instantiations, dependency edges) queried by compiler+browser+debugger
alike — the ancestor of "one IR, many consumers."

Modern: **IntelliJ PSI** = full-fidelity lazy trees + binary
**stub indexes** on disk (declarations-only subsets, each stub
depending ONLY on its own file — per-file invalidation; full PSI
parsed only for files actually opened). **Roslyn** = immutable
red/green trees, FULL FIDELITY (every token/trivia retained — exact
byte round-trip, even invalid code) — the closest analogue to MC11
carrying tokens; incremental reparse = smallest enclosing subtree
(Wagner/Graham incremental-LR — the same theory tree-sitter uses);
persisted caches are SQLite symbol indexes BESIDE the trees, never the
trees themselves. **clangd** = three index tiers (open-files / 
background / prebuilt static) + the preamble(PCH)/body split, per-file
`.idx` caches. **tree-sitter** = GLR incremental reparse with
ERROR/MISSING nodes for mid-keystroke code; highlighting via
declarative QUERIES producing named captures (@keyword, @function...)
— "spans as data with a classification legend," exactly IDE-7's shape.
**JetBrains MPS** = the projectional editor: the AST is the stored
artifact, text/table/diagram are switchable PROJECTIONS — the only
full-strength view-switching precedent; serialization is a swappable
concern (XML / per-root / binary). **ILSpy/dotPeek** = pick the
displayed language (IL/C#/VB) over one unchanged artifact — but
lossy/reconstructed, where MC11's retained tokens make the
original-language view EXACT. **LSP** deliberately refuses AST-level
data (positions/ranges only) because client and server share nothing;
madcide shares the literal tree, so re-imposing an LSP-narrow interface
internally would be a self-inflicted limitation.

Recurring patterns: (1) preamble/body split; (2) persist shallow
stubs/indexes, not full trees — full trees rebuild lazily; (3)
full-fidelity tokens are what MAKE exact round-trip and incremental
reuse possible (validates MC11's core design); (4) Roslyn's
position-free shareable green layer is what MC11 does NOT have — a
named gap only if Roslyn-scale incremental reparse ever becomes a
requirement; (5) incremental reparse = smallest enclosing node, "good
enough," never minimal; (6) projections separate stored model from
displayed syntax (MPS); (7) ⚠️ NO surveyed system normalizes N INPUT
languages into ONE tree type — platforms share protocol/infrastructure
while each language keeps its own node types (IntelliJ, Truffle,
Roslyn); Haxe unifies only the OUTPUT side. MC11's central bet is
architecturally novel — no implementation detail to borrow for that
specific step; (8) monolithic blobs lose to partially-invalidatable
indexed storage past a size threshold (the NCB lesson).

## Part C — the forks for the owner (the brainstorm agenda)

1. **Input-language scope of this arc.** Ship IDE-6/7/8 on what the
   front end parses today (madc/C/C++, by extension → LanguageStd),
   with every new surface keyed on the enum + registry (I3/I4) so
   later front-ends slot in — or open the P3 input axis now? (Recon:
   P3 is recorded far-future; no precedent exists to borrow from for
   N-inputs-one-tree — it is OUR novel bet and deserves its own arc.)
2. **View-switching first slice.** Read-only alternate views are
   nearly free where emitters exist: original source (the buffer),
   `--emit=mc11`, `--emit=c11`. The madc/C++ reverse-render targets
   are designed-but-unbuilt (Phase 5, synth_from_origin) — build them
   as render targets when the view machinery exists to consume them?
   EDITING inside a non-original view (MPS-grade projectional editing)
   is a far seat — first slice edits only the original?
3. **The binary .mc11 cache vs the 2026-08-22 ruling.** Is an
   explicit, user-invoked IDE project artifact a third sanctioned
   category beside .o/AOT, or does "ever" foreclose it? Engineering
   input either way: (a) industry persists shallow stub indexes, not
   full trees; (b) our own cold-start arc made parsing fast (tcc-parity
   bar) — MEASURE a real project's parse-on-load before designing any
   cache (measure-before-designing law); (c) if one is ever built, the
   NCB lesson says partially-invalidatable per-TU artifacts, never one
   blob, and CirFrozenForest's format doesn't carry what an editable
   reload needs anyway.
4. **Re-parse cadence + incrementality.** First slice = whole-TU
   re-parse on save/idle through a PERSISTENT compile-never-execute
   child (the diagnostics/outline machinery given a lifetime), as
   IDE-6 sketched? Function-level incremental reparse (smallest
   enclosing node — the industry-standard "good enough") is a later
   lever; a red/green refactor is NOT proposed.
5. **Where the handle API lives.** madc:: beside diagnostics/outline
   (open/refresh/close/query on a live child Program; a group handle
   for a cc.json project) — the recorded IDE-6 lean. Composition with
   the runtime-eval child policy to verify at design time.
6. **Highlight classification vocabulary.** tree-sitter's capture
   model validates IDE-7: spans-as-data carrying CLASSIFICATION NAMES
   (keyword/ident/literal/comment/...), theme/profile maps names →
   colours (data, like the bindings); tui_attr grows the palette, the
   VT100 target grows SGR. Classification computed from the retained
   tokens/AST — the renderer never parses.
7. **No internal LSP.** madcide reads the real cir_node tree/tokens
   through the handle surface — no narrowed protocol between "parser"
   and "editor" (LSP's restriction was forced by its client/server
   split; we don't have one).
