# madcide AST arc + the frozen-artifact taxonomy — brainstorm writeup

**Status: BRAINSTORM OUTPUT (owner + agent, 2026-08-25, session 131).**
This is the design record of the madcide v2 item-3 brainstorm. Section 1
is an OWNER RULING (it refines the 2026-08-22 cache ruling). Section 2
is the agreed design direction for the AST arc; section 3 the proposed
slice cut. Recon inputs: [2026-08-25-madcide-ast-brainstorm-recon.md]
(2026-08-25-madcide-ast-brainstorm-recon.md) (the repo record + the
Turbo-C→JetBrains/Roslyn industry survey). Work order context:
[2026-08-25-madcide-phase2.md](2026-08-25-madcide-phase2.md) "Owner
review" item 3.

## 1. The frozen-artifact taxonomy (OWNER RULING, refining 2026-08-22)

The owner's framing (2026-08-25, verbatim intent): the 2026-08-22
problem was the BLURRING of the line between the SYSTEM-HEADER FOREST
"ROM" (CirFrozenForest) and freezing the PROGRAM "RAM" as a startup
cache. Freezing program state is not banned — a save/load "program
state" (the console-emulator save-state concept) is a potentially
useful madc LANGUAGE FEATURE. The proper startup-speed levers remain
`.o` and executables. Machinery may be REUSED across these; the LINE
between the concepts must always be drawn and kept.

### The four artifact kinds

| Kind | What | Authority (who writes) | Consumer | Lifecycle |
|------|------|------------------------|----------|-----------|
| **ROM** | The system-header forest (CirFrozenForest) | The toolchain build; users never write one | The compiler, every compile | Immutable; versioned with (appended to) the madc binary |
| **Execution artifact** | `.o` object / AOT executable | The user, explicitly (`-c` / `-o`) | The run path | The ONLY way a program persists past its source for RUNNING |
| **Program save-state** | The running program's RAM (heap, globals, world data) — the emulator save-state; a FUTURE language feature | The program/user, explicitly (a `madc::` surface / CLI verb) | An explicit resume surface | Named seat, own future arc; lineage: hub `world_save`, Smalltalk/VisualAge images |
| **IDE session cache** | madcide's parsed-AST session state | madcide | madcide only | Disposable; execution NEVER depends on it; stale/deleted ⇒ the IDE reparses |

### The three rules that draw the line

- **R1 — artifact identity.** Every frozen file self-declares its KIND
  in its header; each loader accepts ONLY its own kind. Machinery reuse
  happens BELOW that line: the arena/serialization plumbing (DefArena,
  the freeze writer) is a shared library layer, but artifact kinds
  never cross loaders (enum-over-strings applied to files). When the
  first non-ROM kind is implemented, a negative-controlled gate pins
  that the run path refuses the other kinds.
- **R2 — no ambient consultation, ever.** The startup path consults
  ROM plus whatever artifact was EXPLICITLY named on the command line —
  nothing else. No discovery of frozen anything beside sources.
  **Explicitness is what separates a feature from a cache**: a
  save-state is saved and loaded deliberately (staleness is the user's
  visible choice); a cache is consulted ambiently and must therefore be
  transparently correct — S5's sin was the implicit consultation as a
  startup lever, not the freezing. This rule preserves everything the
  2026-08-22 ruling protected.
- **R3 — save-states PIN the program, never embed it.** The emulator
  model: the save file references the cartridge (a hash-pin of the
  exact source/`.o`/exe) and carries RAM only; it refuses to load
  against a different program. Consequence: a frozen PROGRAM IMAGE
  never exists as an artifact at all — programs persist as `.o`/exe,
  runtime as save-state, headers as ROM; every byte on disk belongs to
  exactly one category. (A bare script wanting a save-state produces
  its `.o` first — a fair ask.)

### Relation to the 2026-08-22 ruling

What STANDS unchanged: no startup-latency caching of user programs;
`.o`/AOT are the execution artifacts; the only ROM is the built-in
system-header pack. What is REFINED: the "ever" applied to
ambient program-state caching (R2), not to the save-state feature
category (a legitimate future language feature under R1/R3) nor to the
IDE session cache category (legitimate, kind-locked, run-path-refused).

### Industry cross-references (where the industry went)

- **IDE cache as its own category**: IntelliJ persists binary STUB
  INDEXES (declaration-only PSI subsets; each stub depends only on its
  own file ⇒ per-file invalidation; full PSI reparsed lazily only for
  opened files); Roslyn persists SQLite symbol indexes BESIDE the trees
  (trees themselves are never persisted); clangd persists per-TU
  `.idx` files + on-disk preamble PCHs. Nobody who scaled persists
  full trees; everyone's IDE cache is disposable and separate from the
  build.
- **The blob lesson**: Visual C++'s `.ncb` (one memory-mapped blob,
  whole-blob invalidation, 64K-entity ceilings) cost ~15 years before
  the industry converged on per-unit, partially-invalidatable, indexed
  storage. Any madc disk cache is per-TU files, never one blob.
- **Save-state lineage**: Smalltalk/VisualAge images (the persistent
  image IS the program state); emulators pin-the-ROM (R3's model).
- **Binary per-unit artifact precedent**: Delphi `.dcu` (interface-
  keyed reuse) — the old-school version of kind-typed per-TU caching.

## 2. The AST arc design (IDE-6/7/8, industry-cross-referenced)

### Scope decisions

- **Input languages**: this arc ships on what the front end parses
  today (madc/C/C++ via LanguageStd); every new surface is keyed on
  the enum + registry (invariants I3/I4) so later front-ends slot in.
  The P3 polyglot INPUT axis stays its own future arc — the survey
  found NO precedent for N input languages normalized into one tree
  type (platforms share protocol/infrastructure, each language keeps
  its own node types — IntelliJ, Truffle, Roslyn; Haxe unifies only
  output). MC11's N-inputs-one-tree is OUR novel bet; it deserves its
  own arc, not a rider on the IDE.
- **No internal LSP.** LSP refuses AST-level data by stated design
  ("much simpler to standardize a text document URI... than an
  abstract syntax tree") because client and server share nothing.
  madcide shares the literal cir_node tree — the handle surface
  exposes real nodes/tokens/positions; narrowing it internally would
  self-inflict LSP's constraint.

### IDE-6 — persistent parse handles

- A `madc::` handle surface (beside diagnostics/outline — the same
  compile-never-execute child-Program machinery given a LIFETIME):
  open/refresh/close a live child per TU; a project handle groups the
  TUs of a cc.json manifest (`read_compile_commands` — the existing
  --project reader). Today's children are stack-locals destroyed per
  query and `madc_project_execute` scopes every TU Program to one call
  — the handle registry is new lifetime machinery (the ui_sessions
  handle-table discipline is the in-repo precedent shape).
- **Re-parse cadence**: whole-TU re-parse on save/idle first.
  Industry-standard incrementality (re-parse the smallest enclosing
  reparseable node — Roslyn's Blender, IntelliJ IReparseableElementType,
  tree-sitter; all on Wagner/Graham incremental-LR, all self-described
  as "good enough, not minimal") is a NAMED LATER LEVER, taken only if
  whole-TU numbers demand it. A Roslyn-style red/green split is NOT
  proposed — MC11 stays one token-carrying tree.
- **Queries the IDE consumes**: outline (exists), diagnostics (exists),
  outline-at-offset (enclosing function/class at the caret — the
  status line; LSP documentSymbol-shaped, served from the retained
  tree), highlight spans (IDE-7), find-decl/refs later.
- **MEASURE parse-at-scale inside this arc** (measure-before-designing
  law): time parse-on-load for adventure (11 TUs) and a large corpus;
  the numbers decide whether a disk cache exists at all. If built, it
  is the IDE-CACHE kind under R1/R2: per-TU stub-shaped files
  (declarations + spans), invalidated by content hash of the TU + its
  include closure (our preamble problem), full tree re-parsed on open.
  CirFrozenForest's format cannot serve this anyway (lowered
  type-graph only — no tokens/parse subtrees/spellings), so the format
  is new by necessity as well as by R1.

### IDE-7 — colour and highlight spans as data

- Classification comes from the retained tokens/AST; spans arrive as
  edit-node PROJECTION HINTS carrying CLASSIFICATION NAMES
  (keyword/identifier/literal/comment/type/function/...), and a
  theme/profile maps names → colours — data, exactly like the
  keybindings. Industry validation: tree-sitter's query captures
  (@keyword, @function → theme-mapped) and LSP semanticTokens' legend
  are both "spans as data with a name legend"; ours reads the real
  tree instead of re-deriving.
- Renderer side: `tui_attr` grows a small palette (today: normal,
  reverse); the VT100 target emits SGR. The renderer never parses.

### IDE-8 — project + multi-buffer

- Open cc.json → TU list pane; per-TU buffers + parse handles; save
  project writes the same json. `^K E` becomes JOE's edit-file here
  (check moves again). Buffer switch = handle switch; the world/bag
  model already carries per-entity text components.

### View switching — an EDITOR-INHERENT capability (owner, second pass)

**OWNER (2026-08-25, mid-brainstorm):** view switching is something the
editor inherently handles — differentiating WHAT IS DISPLAYED from THE
ACTUAL STORED FORMAT — because the same separation is what lets the
editor edit non-plain-text documents (markdown, RTF, DOCX, PDF, …)
while displaying mostly-standard text and HIDING formatting characters.

The general mechanism is a **document LENS** between the stored text
component and the edit node (the hub doc's lens get/put applied to
documents):
- display text = get(stored) — a projection, computed as DATA;
- a coordinate MAP (display offset ↔ stored offset) rides with it;
- an EDITABLE lens translates display-space edits back to stored-space
  mutations (put); a read-only lens has no put.
- The caret/selection TRUTH stays in STORED offsets on the bag,
  projected through the map for display; the edit node grows optional
  display-text + span-map hints; the renderer still never parses —
  the lens computes, the renderer paints.

Instances of the one seam:
- **identity lens** — plain text; display == stored (today's editors;
  the degenerate case, zero cost);
- **code views** — original (identity, editable) / MC11 / C11 / C++
  (rendered projections of the handle's tree; read-only first) — the
  madcide case;
- **formatted documents** — markdown displayed as mostly-plain text
  with formatting characters CONCEALED and/or rendered as attributes
  through the SAME span machinery as IDE-7; RTF/DOCX/PDF as future
  CODECS behind the same seam (DOCX = zip+XML, RTF = text+control
  words; PDF is the far end).

Precedents: Bravo/Word's ORIGINAL piece table (formatting runs kept
separate from text — our text component's own lineage); vim conceal /
Emacs invisible-text properties; Typora/Obsidian live preview (display
rendered, store markdown); ProseMirror replace-decorations; MPS
projections. The recurring failure mode to design against: caret math
across concealed ranges — which is exactly why the coordinate map is a
first-class data structure, not per-view arithmetic.

- **First slice — the view seam + read-only code views**: the
  display/stored separation with the identity lens as the default, and
  ORIGINAL / MC11 (`--emit=mc11` exists) / C11 (`--emit=c11` exists)
  as the first non-identity consumers. Edits happen through editable
  lenses only (the original view, in this slice).
- **In this arc (OWNER 2026-08-25, mid-brainstorm): `--emit=c++`.**
  The C++ reverse-render target gets BUILT (the May 2026 Phase-5
  forward design: keyed on the `synth_from_origin` marker already on
  cir_node — emit the single high-level decl, suppress the implied
  lowered machinery), as a render target on the one IR + `--std` enum
  per the SET-IN-STONE MC11 rule, and madcide's view switcher consumes
  it beside MC11/C11. `--emit=madc` rides the SAME mechanism and
  follows (a seat, not this arc's gate). Cross-language views (view C
  input as madc, etc.) fall out of the same mechanism as targets land.
- **Far seat — projectional EDITING in a non-original view** (JetBrains
  MPS is the only full-strength precedent: the AST is the stored
  artifact, text/table/diagram are switchable projections). MC11 is in
  a STRONGER position than both precedents: MPS never has original
  text; ILSpy/dotPeek reconstruct lossily — MC11 retains the actual
  tokens, so the original view is exact by construction.

## 3. Proposed slice cut (execute after owner sign-off)

1. **AST-1**: the `madc::` parse-handle surface (open/refresh/close +
   outline-at-offset) over persistent children; per-TU handles + the
   cc.json project handle; the parse-at-scale measurement. Gates:
   handle-lifecycle reducer, outline-at-offset pins, testmadcide
   status-line pin.
2. **AST-2**: highlight spans (classification from retained tokens) +
   tui_attr palette + SGR in the VT100 target + theme-as-data; madcide
   consumes spans as edit-node hints. Gates: span pins per language
   mode, pty colour smoke, testvised byte-identity (no colour bound).
3. **AST-3**: THE VIEW SEAM — the editor-inherent display/stored
   separation (document lens: get + coordinate map; put for editable
   lenses; identity lens = today's behavior, byte-identical), with the
   read-only code views (original / MC11 / C11) as its first
   non-identity consumers in madcide. Gates: coordinate-map unit
   battery (display↔stored round trips, concealed-range caret math),
   view-swap pins, testvised/testlineed byte-identity (identity lens).
4. **AST-4**: `--emit=c++` — the reverse-render C++ target (owner-
   required): synth_from_origin-aware emission from the retained
   high-level structure, wired into the `--emit=` enum and the madcide
   view switcher. Gates: round-trip reducers against the g++/clang++
   oracles (emitted C++ recompiles and behaves identically), view pins
   in testmadcide. `--emit=madc` follows on the same seam.
5. **AST-5**: IDE-8 project/multi-buffer (+ `^K E` edit-file).
6. **Parallel/when-scheduled**: the R1 kind-typed artifact header +
   run-path-refusal gate lands with the FIRST non-ROM artifact
   implementation (the disk cache if measurement demands it, or the
   save-state arc, whichever first).
7. **Named seats, not this arc**: the save-state language feature
   (own design + thread-contract per owner law); `--emit=madc`
   (follows `--emit=c++` on the same seam); the EDITABLE markdown
   conceal-lens (the put direction's proof — first formatted-document
   codec on the view seam); RTF/DOCX codecs (PDF = far end);
   incremental reparse; P3 polyglot input.

## 4. Still open (owner's)

- Naming/spelling of artifact kinds and file extensions (e.g. the IDE
  cache's name; the save-state extension) — at implementation time.
- The save-state feature's surface and scope — its own arc.
- Disk-cache go/no-go — awaits the AST-1 measurement.
