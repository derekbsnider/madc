# The Nexus and Multi-View documents, cross-referenced with the settled design

**Date:** 2026-09-09 · **Status:** REVIEWED + RULED (owner 2026-09-09: "yes, sounds good to me" — the six rulings in §5 stand as recommended; §5b adds a seventh requirement stated the same day)
(owner request, 2026-09-09: "before we embark onto the client-server step,
please read and cross reference with the two new markdown documents").

**The two documents** (owner, 2026-09-08; mirrored from `inbox/` verbatim):

- [MAD C IDE Nexus — a vision for a modern software development environment](2026-09-08-madc-ide-nexus-vision.md)
  (the "Nexus" doc, §1–§31)
- [MAD C IDE Multi-View Architecture](2026-09-08-madc-ide-multi-view-architecture.md)
  (the "Multi-View" doc, §1–§30)

**Cross-referenced against:**

- [madcide as API gateway, and the code-server north star](2026-08-31-madcide-gateway-and-code-server.md)
  (the RULED layering, state tiers, concurrency, permissions, nexus-not-replacement, LSP-at-the-edge, sequencing)
- [madc as a unified development substrate](2026-06-29-madc-development-substrate-vision.md)
  (Rungs 5–7: time as an axis, MCP, federation)
- [The data hub and projection–rendering abstraction](2026-08-20-data-hub-projection-rendering.md)
  (the five layers, the 15 demands, the keys + levels access model)
- [Level 3 is the web target](2026-09-06-ui-web-target-and-madcide-gui.md) §3.5 (the `ws` target), §3.7 (thread contract)
- [The madcide AST arc](2026-08-25-madcide-ast-arc-design.md) §3.5 (error-tolerant parse, SETTLED and landed), the view seam
- `.claude/rules/mc11-ir.md` (every `cir_node` carries its tokens, parse subtree and file/line/col — BOTH lowered and high-level)
- the owner's post-master order (KG Decision `post_master_order_after_gui_release`, 2026-09-09)

## 1. The one-paragraph verdict

The two documents are the same vision the repository already holds — the
gateway over the running compiler, the headless local code server, the three
time axes joined at a nexus that federates external tools rather than
replacing them — written from the product side and carried further in four
places: (1) **event sourcing** as the foundation of project state (Nexus §20),
(2) **per-node authority and semantic federation** (Nexus §17–§24), (3) the
**View abstraction with correlation maps** as the one mechanism behind splits,
diffs, lenses, history and debugging (Multi-View throughout), and (4)
**debugger, profiler, disassembly and binary Views** (Multi-View §6–§9,
§17–§20) — a tier madc does not have yet. Nothing in them contradicts a
standing owner ruling; four points sharpen a ruling and need one (§5 below).
The consequence for the step we are about to start is concrete: **"the
Terminal in its own window" is an instance of "any View in any container
(pane, tab, window)", and a second window is a second client of the one
session** — so the first client-server slice is the View/container
generalization plus the change-event stream, not a terminal-window feature.

## 2. Where the documents meet what is settled or landed

| Document | Settled / landed counterpart | State |
|---|---|---|
| Nexus §1–§2 the project as the primary object; git = past, AST = present, PM = future, execution = behaviour | Gateway design "North star"; substrate vision Rung 5 (three tenses of one MC11-IR graph) | Settled vision. Execution as a fourth axis is a useful addition to the vocabulary. |
| Nexus §3 the AST as live state; error-tolerant parser; error nodes inside the tree | AST arc §3.5: `TokenError` + the eight `ErrorNodeKind`s, per-statement recovery, handles stay alive (`parse_check`, outline, `parse_enclosing` answer past the break); translate refuses on `error_nodes > 0` | **Landed** (slice A). Slice B+ (statement-head hole synthesis) is the named refinement. |
| Nexus §13 keyboard-first: every operation a command independent of the GUI; profiles as mappings | `profiles/default.menu` = the one command registry + menu map; `.keys` profiles through one `parse_keys`; commands take arguments; the registry gate | **Landed** (S1, P4). The Nexus doc's dotted names (`project.build`) are a spelling question only. |
| Nexus §14 GUI, TUI, CLI and agents are peers over one core | Gateway layering (RULED): thin clients over the API; one composer, one client loop; the TUI byte-identical | **Landed** for TUI + GUI; CLI/MCP/remote = the transport slice. |
| Nexus §15–§16 AI as a participant; agent changes as proposals | State tiers: every client (an MCP LLM included) has a cursor + colour; permission tiers observe/point → edit → project-admin; "born low, promoted" | Settled direction; not built. Proposals = a WRITE-tier that lands as reviewable deltas — the permission model already has the shape. |
| Nexus §22 trust and permissions (READ / EXECUTE / WRITE / DENIED per node) | Hub access model (owner-decided 2026-08-20): keys + levels per domain; a role IS a key; assignments are hub relations | Settled. The Nexus table maps onto keys + levels one-to-one; no third mechanism. |
| Nexus §25 git stays foundational; §27 local-first | "Nexus, not replacement" (RULED); "headless LOCAL code server" | Settled, and the documents agree. |
| Nexus §28 performance and immediacy | The cold-startup arc (tcc-parity bar), the -O0 efficiency law, the incremental web editor | Standing laws. |
| Nexus §29 madc's opportunity: editor, parser, AST, compiler, JIT, runtime in one process | "The running madc IS the compiler" (owner law 2026-08-27); Build compiles the screen through the live parse handle | **Landed** — this is the foundation everything else stands on. |
| Nexus §12 universal search / command language | The command palette, the colon line (one interpreter over the vocabulary), commands with arguments | Landed shape. Semantic queries ("callers of X") wait on the outline's deeper walk (ROADMAP 8.5). |
| Nexus §11 build, test, debug, profile as one model | Problems rows go to the line; Build/Run in-process; the pty Terminal | Build/run half landed. Test runner, debugger, profiler: not present (see §4). |
| Nexus §5–§10 worksets, the Context view, intent, decisions, semantic history, provenance | Gateway "nexus axes" (madcdis links + madcdat adapters, git adapter first); the repository's own `madc-knowledge` graph is the hand-run prototype — its `Decision` nodes and ADR 0001 ARE the doc's "Decision #37" | Planned, not started. |
| Nexus §17 the Nexus as a server node; §18–§24 federation, semantic sync, events, authority, cross-project | Substrate vision Rung 7; gateway "mirror" (peer cores exchanging the splice stream, rebased on receipt) | Planned direction; the Nexus doc is more specific (see §3). |
| Multi-View §3 source Views (original, MC11, emitted C, C++, alternate language) | The view seam (AST-3): a view is a document LENS applied at composition (`madc::emit` over the live buffer into a view buffer); `^K A` cycles original → MC11 → C11 → C++; the lens carries a coordinate map (`vmap`) | **Landed** for one active lens per session. Alternate-language Views = Track 9 multi-syntax + the polyglot-transpiler arc; the `--std=` enum is the "generator" attribute. |
| Multi-View §5 compiler intermediate Views (AST, MC11, MIR, passes) | `--dump-cir`, `--dump-nodes`, `--dump-source`, `--emit=c11\|mc11` on the CLI; MIR text through `MIR_output` (debug-only in `madc_cir.cpp`) | Surfaces exist as CLI dumps; none is a handle query yet. A MIR View is small: expose the module text per function through the parse handle. |
| Multi-View §7 external compiler assembly; §25 compare code generation | The gcc/clang parity methodology (`gcc -S -fverbose-asm`, `clang -S`, `scripts/perf_vs_gcc.sh`) — done by hand in every codegen fix | Not a feature yet. A provider that runs the oracle compiler turns the methodology into a View — the single most useful pair for this project. |
| Multi-View §10 historical Views (git revision, branch, tag, diff) | The madcdat git adapter (planned first adapter) | Planned. |
| Multi-View §11 edit-history Views ("before agent change", "last passing build") | AST arc §"the undo history is the change feed": the piece table records every delta; the mirror plan's splice stream `{at, del, ins}` | The deltas exist in memory for undo; they are not persisted, labelled or queryable. |
| Multi-View §13–§14, §24 correlated Views and correlation maps (source ↔ AST ↔ MC11 ↔ MIR ↔ machine ↔ bytes; exact / approximate / one-to-many) | `mc11-ir.md`: every `cir_node` carries its tokens + parse subtree + file/line/col; the lens `vmap` (EMPTY for wholly rendered views today) | The source ↔ AST ↔ MC11 correlation exists BY CONSTRUCTION in the IR; it is not yet projected into a View map. MIR ↔ machine ranges need MIR's cooperation. |
| Multi-View §15 arbitrary splits (two/three/four-way, nested, saved) | S5: one slot per region, a region's nodes stack (the `^K O` window stack); panel tabs; the resizable splitters (v0.99.2) | Partial: stacking and tabs, no nested grid, and the lens is SESSION-level (`es.vdoc`), not per window — the gap that blocks "source beside its MC11". |
| Multi-View §16 View synchronization (cursor, selection, symbol, scroll, execution) | The lens coordinate map is the seed; presence anchors (byte-anchored, shifted by the one mutation owner) | Not built. |
| Multi-View §27 saved layouts as workspaces | Personalities as data ("Turbo-C / RHIDE = a layout profile" in the gateway design) | Planned: a `.layout` profile through the same parser family as `.keys` / `.menu` / `.theme`. |
| Multi-View §28 GUI and TUI compatibility; `view.open source`, `view.split_right mir` | One composer, layout hints the TUI ignores; commands with arguments | Landed shape; the TUI split/tab side = the owner's post-master item 2. |
| Multi-View §22–§23 the View object {subject, representation, revision, generator, runtime_context}; View providers; plugins | Hub layer 4 "projections are data" (demand 1); the entity-handle API ("never raw byte offsets") | Fits the hub model exactly: a View is a projection with a provider; the subject is an entity handle. |

## 3. Where the documents go further than the rulings

1. **Event sourcing as the foundation (Nexus §20).** The settled concurrency
   model is one authority per session, verbs applied in arrival order, no
   CRDT. The Nexus doc adds: record the verbs (actor, object, payload, causal
   parent) so state = events + snapshots — replication, audit, provenance,
   offline replay. Compatible, and it is the same record three planned
   features already need under different names: the edit-history View
   (Multi-View §11), the mirror's splice stream, and provenance (Nexus §10).
   The hub already lists it (demand 10 "time is representable", demand 14
   "event streams are projectable"). One owner, or three copies later.
2. **Authority per node; conflicting observations coexist (Nexus §21).**
   Beyond the single-authority ruling, but at a different tier: a session is
   authoritative for its working tree and editor state; a CI node for official
   builds. "Local PASS, CI FAIL" as information, not a sync failure. The
   thread-safety law and the hub's keys + levels reach here unchanged.
   Design-for now, build at the federation slice.
3. **Native nexus records (Nexus §2, §5–§8: worksets, requirements,
   decisions, architectural memory as Nexus objects).** "Nexus, not
   replacement" holds the nexus to identity + links + cached projections,
   external tools staying the truth of their axis — and Nexus §25 agrees for
   git. But a decision or a workset often has NO external owner: this very
   repository keeps its decisions in a graph, not in a tracker. The two
   readings reconcile if a nexus record is native when nothing external owns
   it and a cached projection when something does.
4. **The IDE layout (Nexus §26): a CONTEXT panel on the right (symbol,
   history, intent, tests, runtime, agents), a PROJECT sidebar (files,
   symbols, worksets, issues), bottom tabs (Build, Tests, Debug, Profiler,
   Problems, Git, Agent activity).** The GUI workbench already has the rail,
   sidebar, editor, panel and status-bar slots; the Context view is one more
   View category in one more slot. The TUI ruling of 2026-08-31 ("no space
   for extra windows hanging around"; `^P` is a popup) meets Multi-View §28
   (two/three-pane splits, tabs, keyboard switching in the TUI) and the
   owner's 2026-09-09 statement that the TUI should have panes and tabs too —
   a relaxation from "no permanent windows" to "panes and tabs the user
   opens", to be stated as the rule.

## 4. What the documents describe that madc does not have

- **A debugger tier** (Multi-View §17–§20; Nexus §11): breakpoints and
  stepping at source, MIR and instruction level; registers, stack, memory,
  threads and tasks. Not on any ROADMAP track. The backend strategy names a
  "REPL/debug tier" only as a direct-MIR scalpel. A real debugger needs MIR
  to expose per-function code ranges and a source-position table (Tier 2/3
  work in `third_party/mir`, designed for upstream) — a large arc that
  belongs AFTER the nexus axes; what to do NOW is keep the correlation seam
  generic so the debugger later projects "current position" into every View.
- **A profiler and a test runner as Views** (Multi-View §20; Nexus §11).
  Nothing in-tree; the test runner is a shell script. Same disposition.
- **Disassembly, hex and structured-binary Views** (Multi-View §6–§9). No
  disassembler in-tree; a GEN-ASM View needs MIR's code ranges plus a
  disassembler provider (objdump / llvm-objdump externally, or in-tree
  later). The ELF/PE/Mach-O WRITERS exist (the native artifact lanes), so a
  structured binary View can reuse their tables as readers.
- **External-compiler assembly Views** (Multi-View §7, §25): no provider yet,
  but every ingredient exists (the oracle compilers on the container, the
  reducer discipline, `scripts/perf_vs_gcc.sh`).
- **Semantic history, provenance, worksets, the Context view** (Nexus
  §5–§10): the madcdis links + madcdat adapters slice, not started.
- **Federation, semantic pub/sub, cross-project dependency graphs** (Nexus
  §18–§24): the far end of the arc.

## 5. Rulings (owner, 2026-09-09: all six accepted as recommended — KG Decisions `nexus_event_log_from_slice_one`, `nexus_native_records_when_no_external_owner`, `tui_panes_and_tabs_user_opened`, `vocabulary_nexus_session_client`, `debugger_profiler_track_after_nexus_axes`, `view_pairs_order`)

1. **Event log from slice one?** Persist the session's verb + splice log,
   actor-stamped with a causal parent, as THE record that edit history, the
   mirror, provenance and replay all read. Recommendation: yes — it costs a
   data shape now and prevents three parallel logs later.
2. **Native nexus records** for decisions, requirements and worksets when
   no external tool owns them (madcdis), with adapters projecting when one
   does. Recommendation: yes; the repository's own graph is the evidence.
3. **The TUI panes-and-tabs relaxation**: "no permanent windows" becomes
   "panes and tabs the user opens; popups for pick lists". Recommendation:
   state it as the rule (it is what 2026-09-09 said).
4. **Vocabulary**: adopt **Nexus** for the project-state server (the doc's
   `madc-nexus`), keep **session** for one live editing context on it and
   **client** for a window, a TUI, an MCP seat. Three words, one meaning
   each; "gateway" and "headless code server" become descriptions of the
   Nexus, not names.
5. **A debugger/profiler track** on the ROADMAP after the nexus axes, with
   the correlation seam designed now (View providers expose ranges; a
   position projects into every View). Recommendation: yes, as a track row
   with no near date.
6. **Order inside the View work** once the View/container generalization
   exists. Recommendation: source ↔ MC11 correlated (the IR already holds
   the map — cheap and it proves the mechanism), then the code-generation
   comparison (MAD C GEN-ASM beside gcc and clang: our own parity method as
   a feature, useful to us daily), then git-revision Views (the first madcdat
   adapter).

## 5b. Emitted code views are indented and coloured (owner requirement, 2026-09-09)

"All emitted code views should be subject to auto-indenting and colour syntax
highlighting" — every emitted View: today's `^K A` lenses (MC11, C11, C++)
and every future one (MIR, GEN-ASM, external assembly). Findings at the time
of the ruling, so the slice starts from facts:

- **Indentation.** `madc --emit=c11` and `--emit=mc11` emit FLUSH-LEFT text —
  zero indentation and redundant parentheses (`if ((a > b)) {`,
  `return (a - b);`, `(a += i);` from a five-line reducer). The lenses show
  the emitted text as-is. The deepest layer is the EMITTER (`cir_emit_c` /
  the MC11 renderer): indent by block depth and render operators
  precedence-aware (gcc's own spelling) — ONE owner serving `--emit` on the
  CLI and every lens; never a re-indenting shim in the IDE.
- **Colour.** The composer attaches the session's highlight spans
  "stored-space only (a view shows a render)": a lens buffer carries NO
  spans, so the MC11 / C11 / C++ views render uncoloured in both faces. The
  fix is the existing lexer-alone colour path (`lex_spans`, "colour at
  load") run over the EMITTED text into the view buffer's own span rows; a
  MIR or assembly View brings its own classifier as part of its provider.
- **Priority.** Its own slice, EARLY — it is visible in the shipped v0.99.2
  lenses — and independent of the View/container generalization (the
  generalization inherits it: a View's provider yields text + spans).
  KG Decision `emitted_views_indent_and_colour`.

## 6. What this changes about the client-server step

The post-master order (2026-09-09) begins with "the Terminal (and any panel
tab) in its own window — a second window on the same session, the first
client-server step". Read through the two documents, that step becomes:

1. **The View as data on the session.** Generalize today's session-level lens
   (`es.vdoc`, one active view for the whole session) into View objects
   {subject, representation, revision, generator, runtime_context} — the
   editor buffer, a lens, Problems, Output, the Terminal, the Project list,
   later the Context view are all Views. A View has a provider; the subject
   is an entity handle (the gateway's day-one rule), never a byte offset.
2. **Containers as data: pane, tab, window.** The layout places Views in
   containers; a saved layout is a `.layout` profile through the existing
   profile parser family. "Give the Terminal its own window" = move a View
   into a new window container. The TUI reads the same layout with panes and
   tabs (post-master item 2 is the same work, not a second one).
3. **A window is a client.** The web target's loop serves several windows on
   one UI thread (GTK, Cocoa and Win32 all run several windows on one loop;
   the `tick` host op already makes that loop the scheduler's wait). The
   session composes per client (that client's layout, that client's focus),
   presence rides in (a caret and colour per client). A remote window is the
   same client over the `ws` transport; madcdis grows listen/accept there.
4. **Change events, persisted.** The moment two windows show one session, the
   second must learn what changed: the gateway's step 3 event half. Per
   ruling 1, that stream is recorded — it is the edit-history View's source,
   the mirror's splice stream and provenance in one owner.
5. **Correlation first where it is free.** Fill the lens coordinate map for
   the MC11 View from the IR's own token positions; cursor synchronization
   between source and its MC11 is then the first correlated pair and the
   proof of the View mechanism.

Nothing above changes the TUI's bytes; every item is additive data the
composer stamps and the clients read — the rule the whole GUI arc has kept.

## 7. Traceability

- The two documents are mirrored verbatim in `docs/plans/` (dated 2026-09-08,
  their authoring date); `inbox/` remains the drop zone.
- KG: Decision `post_master_order_after_gui_release` carries a pointer to this
  review; the rulings in §5 will land as Decision nodes when made.
- ROADMAP 8.6 "post-master" text points here; Plan Index lists the three
  files.
