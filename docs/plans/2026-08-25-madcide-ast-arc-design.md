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

## 3.1 AST-3 — as executed (2026-08-25, session 132; owner pulled it first)

The owner ordered AST-3 ahead of AST-1/2 ("build the view seam").
Shipped on `feature/madcide-ast3-claude`:

- **The coordinate map** (`include/madcdis/doc_lens.h`, `doc_map`): copy
  segments `{disp, stored, len}` as data with a strict value codec; the
  ONE projection owner. Contract refined during pinning: a boundary
  shared by a copy segment's END and a gap-following segment's START
  belongs to the copy that ends there, so the inverse of a gap-adjacent
  caret lands at the EARLIER position — outside a concealed run, the
  safe side for a future put. Empty map answers 0 (a wholly rendered
  view parks at the top). Unit battery `tests/unit/test_doc_lens.cpp`
  (identity, concealed, synthetic, negative controls on add + codec).
- **`madc::emit(out, source, filename, target)`**: the render query —
  the diagnostics/outline child given the emitter (parse-only front-end
  half split out; `madc_cir_emit` into the existing
  `madc::detail::StringCapture`); target speaks the `--emit=`
  vocabulary through the new ONE converter `cir_emit_lang_of()`
  (cir_emit_c.h), which the CLI parse now also rides — AST-4's `c++`
  lands in exactly one place. Oracle: byte-identical to CLI
  `--emit=c11` on a reducer.
- **`ui::lens_to_display` / `ui::lens_to_stored`**: the map's dialect
  face (−1 = malformed/negative; strict codec refuses whole).
- **madcide `^K N`** cycles original → MC11 → C11 over the one
  document. Mechanics chosen: the lens applies at COMPOSITION — the
  edit node presents the rendered text with the lens data (view name +
  map) as hints, while the rendered text also lives in a view-buffer
  entity so the ONE shared navigation implementation (edit_key, find,
  goto-line) works over display space unchanged (`nav_doc()` = the one
  display-vs-stored routing point; the renderer untouched). Stored
  caret parks on "ocaret"; mutations/history/block-markers/pane-goto
  refuse through the one editable gate + an up-front deny of
  stored-space actions; a non-translating buffer surfaces run_check's
  rows; a failed cycle step falls back to the original. The identity
  lens takes no branch anywhere — byte-identity is structural.
- **Gates run**: doc_lens unit battery; testmadcide view battery (enter
  pins, hint data, refusals, display-space find, cycle + caret restore
  + byte-identical exit, lens caret-math pins — all hand-computed,
  matched first run); testvised/testlineed green (identity lens);
  real-pty smoke (heading tags, lowering visible, `^K ^N` ctrl-held
  continuation, original restored, read-only badge).
- **Deliberately NOT in this slice** (named seats stand): the put
  direction (markdown conceal-lens proves it), non-empty maps for
  rendered views (AST-4's `synth_from_origin` line anchoring can feed
  one), spans/colour (AST-2), `--emit=c++` (AST-4).

## 3.2 AST-4 — slice-1 mechanics (banked pre-build, session 132)

Recon findings that shape the slice:
- `cir_emit_c.cpp` is small (~780 lines) and `celMC11` gates nothing yet
  (mc11 == c11 output today); `CirEmitLang` is threaded everywhere.
- A C++ `std::string` TU lowers to ~215 lines of header-derived prelude
  (struct layouts + mangled externs) + synth ctor/dtor/temp groups +
  user expressions as mangled calls. MIXING a re-raised high-level decl
  with lowered machinery does NOT recompile — so Phase-5's
  "suppress synth, emit the origin once" only closes the round-trip
  gate when the WHOLE user statement stream re-renders high-level.
- The path back to source: `Program::keep_trivia` (full-fidelity
  tokens, built for "IDE/.mc11") + `Program::tokens` (lex order; slot
  ids are lazy — NOT ordered) + `token_spelling` (best-effort; the
  `--dump-source` owner). `--dump-source` reproduces the EXPANDED
  stream, so the user's `#include` directives are consumed — a small
  lexer feeder must record the TU's own directives as written.

Slice-1 mechanics (the seam every later target reuses):
- `celCxx` joins `CirEmitLang` through the ONE converter
  (`cir_emit_lang_of`); `--emit=c++` sets `keep_trivia`.
- The C++ render walks the tree and re-renders TU-origin statements
  from the RETAINED tokens at LINE granularity: each item's subtree
  yields its TU line-range (origins' src_file/src_line — the mc11-ir
  rule's "path back to the original source"); lines echo once (a
  rendered-lines set — synth groups share their origin's lines, so
  Phase-5 suppression falls out of the dedup); header-origin machinery
  is replaced by the recorded include directives; no-origin synthetic
  scaffolding (e.g. `__madc_global_init`) is suppressed — the echoed
  source carries its own initializer semantics.
- Cross-language rendering (a madc-dialect TU viewed as C++ — var/
  UFCS/php:: respelled) is the NEXT seat on this seam: the traversal/
  dedup/include skeleton stays, the per-line echo swaps for
  per-construct rendering. Slice 1 REFUSES a cslMadc TU under
  `--emit=c++` (loud, not wrong output); madcide's cycle already falls
  back gracefully on a refused enter.
- Gates: round-trip reducers (C++ input: emitted C++ recompiles under
  g++/clang++ and behaves identically — the `cir_fidelity.sh` shape,
  behavioral compare); testmadcide grows the c++ view arm (positive
  pin on a .cpp buffer, refusal pin on the .mad buffer).

## 3.3 AST-4 — as executed (2026-08-25, session 132; two deviations from §3.2)

Shipped on `feature/madcide-ast4-claude`:
- `celCxx` through the ONE converter; `--emit=c++` and `madc::emit`'s
  `"c++"` both arm `keep_trivia` fidelity lexing.
- Feeders at the owners: `fidelity_include_directives` (the directive AS
  WRITTEN + the writing file, recorded pre-resolution at the one parse
  site); `madc_token_spelling` = the exposed one spelling owner, whose
  ttString arm now RE-ESCAPES the cooked value (also fixed a latent
  macro-arg re-lex bug: a cooked newline in a string macro-arg re-lexed
  unterminated).
- The renderer (`cir_emit_cxx`): the TU's own directives + the whole-TU
  token echo (trivia + spelling) + trailing trivia, source info passed
  as data (CirEmitSource — no Program dependency).
- **Deviation 1 — whole-TU echo, not per-item line-ranges.** The §3.2
  line-range plan was refuted by the rich probe: closing braces and
  class heads/members carry NO cir nodes (the render came out gutted),
  and the token stream already excludes everything the tree selection
  was meant to exclude (#if'd-out regions never lex; lowered machinery
  never enters the stream — Phase-5's suppressions are INHERENT to the
  echo). The tree keeps the validity gate; tree-SCOPED rendering (a
  single function's view) stays the named later lever.
- **Deviation 2 — no compiler-side cslMadc refusal.** Recon found
  `src_lang` is never stamped past its `cslC` default and `STD_MADC` is
  the superset default every plain `.cpp` compiles under — every
  available refusal signal is wrong-grained or absent. Instead: the
  documented contract ("madc-only constructs pass through unrespelled")
  plus madcide's APP-level document-kind rule (`cxx_view_applies` by
  extension) keep the c++ view off madc-dialect buffers. Truthful
  src_lang stamping joins the respelling seat.
- Gates: `scripts/emitcxx_roundtrip_gate.sh` in fulltest (2 reducers —
  strings/escapes/class/macro/global; template/overloads/enum/refs —
  × g++ AND clang++, behavioral byte-compare, two negative controls);
  testmadcide's second editor world over a `.cpp` buffer (cycle to
  [c++], directive + echo pins, stored size untouched, exit home) with
  the `.mad` cycle pins unchanged.
- `--emit=madc` remains the next target on the same seam (the converter
  + CirEmitSource are where it lands).

## 3.4 AST-1 — as executed (2026-08-25, session 132)

Shipped on `feature/madcide-ast1-claude`:

- **The handle surface** (`madc::parse_open` / `parse_open_file` /
  `parse_refresh` / `parse_close`; queries `parse_outline` /
  `parse_check` / `parse_enclosing`; `project_open` / `project_tus` /
  `project_close`): the compile-never-execute child pipeline given a
  LIFETIME. Registry = the ui_sessions discipline (slot+1, closed slots
  stay null, no reuse within a run); thread contract = the runtime-eval
  confinement. The two existing row loops were extracted into shared
  builders (diagnostic_rows_from_child / outline_rows_from_child) —
  outline rows gained `end_line` (additive; consumers are field-keyed).
- **Outline-at-offset** (`parse_enclosing`) is served from
  `pending_funcs`: TokenFunc's head line/column + the inherited
  `TokenCpnd::end_line` (parseCompound already records the closing
  brace) — no new parser state. Innermost = latest-starting match;
  the closing-brace line counts as inside (no end column exists).
- **Project handles apply the manifest TU's options.** The measurement
  itself surfaced the divergence (open-adventure `misc.c` diagnosed a
  phantom `VERSION` error that `--project` doesn't): the per-TU
  -I/-D/-stdlib/--std application (with the `.c` → gnu17 default) was
  extracted from project_parse_all into `apply_project_tu_options`
  (madc_project.h) and both lanes adopt it. Standalone
  `parse_open_file` stays optionless by design (no manifest).
- **madcide adoption**: one handle per buffer (opened at load, closed
  at exit); check/save/outline all whole-TU refresh through ONE entry
  (`reparse_buffer`); the status line shows the enclosing function from
  RETAINED state via `compose_status`'s existing idle_suffix parameter
  (stored-space only — a view's caret is display-space). Composition
  still never runs the compiler.
- Gates: `tests/testparsehandle` (lifecycle, retained queries, extent +
  innermost pins, broken-buffer state, close refusal + non-reuse,
  never-execute trap, a 3-TU manifest incl. a `-D`-dependent TU and a
  relative-include TU, unreadable-manifest refusal); testmadcide status
  pins (`fn add` in the rendered view; a raw status-node pin).

### The parse-at-scale measurement (the disk-cache decision input)

Dev `bin/madc` (-O0), container, wall clock (order-of-magnitude decision
input, not a trend baseline):

| Corpus | parse-on-load (project_open) | largest TU refresh |
|--------|------------------------------|--------------------|
| madc adventure — 11 madc TUs (`advent.cc.json`) | ~210–290 ms | adv_actions.mad (38.6 KB): ~55–66 ms |
| open-adventure — 8 C TUs, 18.1k LOC (dungeon.c 13.4k) | ~430–620 ms | main.c: ~94–119 ms |

Per-TU floor on `.c` TUs is ~47–60 ms — embedded-header cost dominates
small TUs (each child re-parses the libc headers; `.c`/gnu17 TUs do not
ride the forest).

**Disk-cache verdict: NO-GO at current scale.** Parse-on-load for every
real project we have is sub-second; a per-TU stub cache would save at
most ~0.5 s per project open — it cannot pay for its own invalidation
machinery. The trigger to revisit: a working set where parse-on-load
exceeds ~2 s (extrapolating ~25–35 ms/kLOC measured on real C, that is
roughly a 60–150k-LOC project — SMAUG-scale). If built then, it is the
IDE-CACHE kind under §1 R1/R2: per-TU stub files, content-hash
invalidated, run-path refused.

### Re-parse cadence, error recovery, and the undo connection (owner Q, 2026-08-25)

- **Whole-TU refresh at save/check cadence stands.** 55–120 ms for the
  largest real TUs is imperceptible at that cadence; it would be
  sluggish per-keystroke, but nothing parses per keystroke.
- **Probed: the parser stops at the first error** (syntax OR sema — a
  mid-file error retains only the definitions BEFORE it; probe:
  before/broken/after → outline holds only `before`). Consequence: the
  save/check cadence is BETTER than a per-keystroke refresh here — the
  retained state stays the last complete parse while the user types
  through broken intermediate states. **Error-tolerant parsing is
  therefore the PREREQUISITE for any tighter-than-save cadence** (idle
  or keystroke), and sits UPSTREAM of incremental reparse on the
  dependency chain. Both remain named seats, taken only if numbers +
  UX demand them.
- **The undo history is the change feed.** The piece-table buffer
  already records edit deltas (insert/erase at offset) for undo/redo —
  exactly the input an incremental reparser consumes (tree-sitter's
  `edit()`, Roslyn's changed-span Blender). If incrementality is ever
  taken, no new instrumentation is needed; undo itself needs nothing
  special (an undo is just another delta — parsing keys on content).

## 3.5 Error-tolerant parse — the discussion seat (owner, 2026-08-25; DISCUSSION PENDING)

**Owner (mid-AST-1, reacting to the stops-at-first-error probe):** add
node types to CONTAIN pieces of broken code until they resolve — and
they prevent compilation. Proposed vocabulary (owner's list):
`MissingExpression`, `MissingStatement`, `MissingDeclaration`,
`MissingIdentifier`, `MissingType`, `MissingToken`, `UnexpectedToken`,
`SkippedTokens`.

The list splits into the two families the industry converged on
(Roslyn's IsMissing + skipped-token trivia; IntelliJ's PsiErrorElement):
- **Holes** (`Missing*`): zero-width, SYNTHESIZED where the grammar
  required something — the tree stays structurally complete and
  queryable (outline/spans/enclosing keep working past the error).
- **Debris** (`UnexpectedToken`, `SkippedTokens`): REAL source tokens
  set aside, retaining spellings/positions/trivia — the original view
  and the `--emit=c++` echo stay exact mid-error.
Both carry/imply a diagnostic; ANY of them present gates translate —
"prevent compilation" is the owner's ruling and matches MC11's
constraint (cir_node derives from c2mir node_t; error nodes must be
parse-tree citizens — TokenBase kinds — that never lower).

Recon (2026-08-25): every interior parse error throws (Throw/throwit —
thousands of sites) and lands in ONE catch cluster wrapping
`Program::parse`'s top-level statement loop (parser.cpp ~66226), which
records ONE diagnostic and abandons the rest of the stream. That
structure hands us the recovery seam:
- **Slice A — panic recovery at the loop** (small, high yield): catch
  PER top-level statement; record the diagnostic (captured, not
  terminal), wrap the failed region as `SkippedTokens`/`ErrorStmt`,
  skip to a sync point (next `;`/`}` outside every delimiter —
  `DelimDepth`, per the one-tracker law), continue. Interior throw
  sites stay untouched. Yields multi-error diagnostics + post-error
  definitions (outline/enclosing survive mid-edit states).
- **Slice B+ — interior `Missing*` synthesis**: highest-yield interior
  sites stop throwing and synthesize holes instead (statement/decl
  heads first). Per-site migration; taken incrementally, gated by the
  same "error nodes gate translate" rule.

To settle at the discussion: the TokenType/TokenBase placement (new
token classes vs one error class + a kind enum — enum-over-strings
applies), diagnostics linkage (node → Diagnostic index), the translate
gate's spelling (loud pre-c2mir refusal, the madc_cir_emit validity-gate
precedent), and slice A's reducer battery.

## 3.6 AST-2 — as executed (2026-08-25, session 132)

Shipped on `feature/madcide-ast2-claude`:

- **Renderer half**: `tui_attr` grew the six chromatic VT100 base
  colours (`tui_attr_of`/`tui_attr_name` = the one name↔attr converter,
  the tui_key_name discipline); the edit node's hints carry `spans`
  rows `{s, e, c}` (byte offsets + colour name; malformed rows skip);
  `paint_edit` paints spans then the selection LAST through
  `fill_range_overlap` — the selection's inline overlap math extracted
  as THE one range-overlap rule before a second copy existed. The VT100
  target's `emit_sgr` is the one attr→SGR table; normal↔reverse keep
  their historical spellings, so the no-colour byte stream is unchanged
  (testvised/testlineed byte-identity held structurally).
- **Engine half**: `madc_token_highlight_class` (lexer.cpp beside the
  spelling owner; enum + name table in tokens.h) classifies by lexed
  TokenType — keyword/type/number/string, ident by tkIdent;
  `madc::parse_spans` walks the handle's RETAINED TokenStream: rows
  `{line, column, length, class}` with column = the span's START
  (token stamps are END-anchored — the diagnostics convention — and
  the query converts). Parse handles now arm `keep_trivia`
  (re-measured: refresh 52–55 / 100–104 ms on the largest TUs — within
  the unarmed range, so ONE mode, no flag); comment rows derive from
  leading trivia anchored by each token's recorded position, plus the
  trailing trivia. Function names classify by head-line name match
  from `pending_funcs` (the from-the-tree differentiator; a call on
  another line stays ident — pinned).
- **Two lexer defects the span pins exposed (fixed at their owners,
  own commit)**: (1) `pushback_reread` froze the cursor, so compound
  type-specifier heads (`long` in `long add(`) stamped the LOOKAHEAD
  word's end — same-line re-reads now rewind and recount; (2) the
  lookahead ate the space before a non-word (`char *s` → `char*s` in
  the c++ echo and a lost column) — consumed whitespace is given back.
  `emitcxx_rt3.cpp` (char* heads, unsigned long long, long double)
  joined the round-trip gate.
- **App half**: `profiles/default.theme` (class → colour, parsed by
  parse_keys — the ONE profile-line rule, adopted not copied);
  `refresh_spans` converts query rows through the theme into hint rows
  at (re)parse (ensure_phandle/reparse_buffer end with it); composition
  attaches the stored rows, stored-space only. Staleness = the status
  line's: colours refresh on check/save/outline; per-keystroke
  freshness is a named later lever (a lex-only refresh or idle timer).
- **Owner extension (mid-AST-2): VT-102 ANSI / JOE parity + colour
  schemes.** The 8-colour enum became the style STRUCT {fg, bg, flags}
  speaking JOE's whole vocabulary (bold/dim/italic/underline/blink/
  inverse + 8 fg colours + bg_*), bold-as-bright = the 16-colour model
  (no aixterm 90–97); tui_attr_of = the one SPEC parser (whole-spec
  refusal). emit_sgr = reset-then-set with the full parameter list;
  the historical \x1b[7m/\x1b[0m spellings survive, keeping the
  no-colour stream byte-identical. SCHEMES are theme files
  (profiles/*.theme, class-first multi-word-spec lines — the split
  deliberately differs from the keybinding parse; profile_line stays
  the shared line rule), swapped at runtime by name through the one
  prompt mode (^K T; default + classic ship; a missing theme refuses
  and keeps the previous one). view_name relocated above the prompt
  machinery — still the one view-active seat.
- **Named refinements**: a lex-recorded token extent (spelling-length
  drifts cosmetically on non-canonically-written literals); the exact
  function-name token feeder; span-carrying views (display-space
  spans through the doc_map); 256/true-colour styles.
- Gates: `tests/testparsespans` (16 hand-computed rows incl. lead/
  mid-block/trailing comments), testmadcide themed-span pins (9 rows,
  exact offsets, head-line rule pinned), test_tui_model unit battery
  (converter round trip + refusal, span painting, selection-wins,
  bad-row skip), pty colour smoke (SGR 33/32 + historical 7m/0m on a
  real terminal), emitcxx gate now 3 reducers × g++/clang++.

## 4. Still open (owner's)

- Naming/spelling of artifact kinds and file extensions (e.g. the IDE
  cache's name; the save-state extension) — at implementation time.
- The save-state feature's surface and scope — its own arc.
- ~~Disk-cache go/no-go — awaits the AST-1 measurement.~~ **Measured
  (§3.4): NO-GO at current scale; revisit at >~2 s parse-on-load.**
