# madcide IDE controls — palettes, reclaimed keys, build/run (IDE-10)

**STATUS: IDE-10a + IDE-10b SHIPPED (2026-08-27, session 138); IDE-10c
SHIPPED (2026-08-27, session 139)** — the palette core (model
list+autofocus hints), ^P file palette, the four key reclaims + ^R
(ui::tui_refresh), and the ^B build palette (capture pump on the MT-4b
mixed select; channel::cancel() SIGTERM stop; terminal-mode runs via
suspend/resume; command rows as data). Engine seams that landed with it:
__madc_task_fire_due (yield fires due io beside timers) and read_keys'
unified cooperative wait (50ms cadence while live-but-parked tasks
exist — the MT-4c stdin-unification retires it). IDE-10c made the
default ^B rows INTERNAL (madc::build_native + madc::compiler_path; see
§IDE-10c below) and gave Esc back-out of any pane. Remaining from this
doc: manifest-declared build commands, ^P fuzzy fs walk, the debug row
(ADR-0001 REPL/debug tier), pico.keys palette bindings (owner call).

Owner direction (2026-08-27, verbatim rulings): madcide is an IDE, not
JOE — it needs controls for compiling, starting, stopping, debugging
madc applications; multiple files WITHOUT split-screen proliferation
(a pop-up project file list to switch between files); and JOE's four
control-key arrow synonyms are RECLAIMED (real arrow keys exist):

| Key | Ruling                                              |
|-----|-----------------------------------------------------|
| ^P  | project/file palette (VSCode Quick-Open style)      |
| ^F  | shortcut for ^K F (find)                            |
| ^N  | shortcut for ^K E (open/edit file)                  |
| ^B  | build palette (compile, run, etc — all build options)|
| ^R  | keep JOE's refresh/retype (owner: "don't forget")   |

None of the four collide with terminal flow control (^S/^Q untouched).
The ^N ruling settles half of the pending ^K E review: ^K E stays,
^N is its front door.

## Lineage & north star (OWNER, 2026-08-27)

madcide's spiritual lineage: **Borland/Turbo C++ → Joe's Own Editor →
VS Code**. The IDE-integration FEEL of Turbo/Borland C++ (compile/run/
debug lives inside the editor — IDE-10c's internal builds are exactly
this), the PORTABILITY and KEYBINDINGS of JOE (the default profile;
runs on any terminal), and the MODERN FEATURES of VS Code (palettes,
live diagnostics, and — named want — **plugins written in madc code**).

Two standing consequences:

- **madcide is exercise #3 for the `madc::ui` abstraction layer**
  (#1: colossal cave adventure, #2: examples/texteditor). A **GUI
  madcide** — looking and
  feeling more like VS Code — is the SAME CODEBASE rendering
  differently by environment: madcide composes a model tree
  (`compose_ide_tree`) and never paints; the terminal renderer is one
  consumer of that tree, a GUI renderer is another. This is why
  rows-as-data, data-driven hints ({list, focus}, tabwidth, spans), and
  the no-direct-painting discipline are load-bearing: every madcide
  feature must stay expressible as model data + semantic events, or the
  GUI twin forks.
- **Plugins in madc code**: madcide is itself madc source; the natural
  plugin seat is the machinery it already dogfoods — loaded madc
  modules contributing DATA (palette rows, build rows, key actions,
  panes) through the same registries the built-ins use. Design rule
  when the slice lands: a plugin registers rows/verbs, never paints,
  never binds keys imperatively — the same discipline as above.

## Where madcide diverges from JOE

JOE's model is buffers + windows + keys. madcide adds a second axis:
the workspace KNOWS it is a madc project. The engine seats exist —
`project_open`/`project_tus` (manifest + TU list), `parse_open`/
`parse_check` (live diagnostics, dogfooded since stage 2), `exec://`
channels, and MT-4b's mixed select: ONE wait spanning keystrokes,
background-parse completion, and a running child's output. Compile/
run/stop is another case in the loop, never "shell out and freeze".

## One palette mechanism, N palettes

^P and ^B are the SAME widget with different data: a popup overlay
list with incremental filter + arrow/enter selection — the ^T options
overlay (IDE-9d, rows as data) generalized with a typed filter. One
implementation, future consumers include a command palette
(no-parallel-implementations).

- **^P file palette**: rows = open buffers (MRU first — ^P+enter is
  "toggle to last file"), then the project's TUs when a manifest is
  open; name + dirty/diagnostic markers. Enter switches the active
  buffer IN PLACE (the AST-5 ring addressed by name, no splits).
  ^K O (IDE-9e) stays the separate opt-in for two-files-visible.
  Later slice: fuzzy filesystem walk (the full VSCode behavior).
- **^B build palette**: action rows AS DATA: check (parse-only),
  build exe, build .o, run (JIT), run native, stop (visible while
  running). Default rows ship built-in; the project manifest grows an
  optional commands section later so a project declares its own
  entries — all build options without hardcoding any.

## Run / stop / output

A build or run spawns through an `exec://` channel; the IDE loop
selects on it (MT-4b's dogfood, as stage 2 was the parse's). Output
streams into a virtual `[build]` buffer in the ring — ^P-switchable,
jump-to on first error — the editor stays live while it streams.
No forced splits; a bottom output window becomes possible with
IDE-9e for those who want it.

- **stop, slice 1**: honest process termination — close the exec
  channel (child reaped; the Process dtor SIGKILLs a stubborn one).
- **stop, principled**: MT-3 scopes + cancellation give in-process
  tasks the same verb, so "stop" means one thing everywhere. IDE-10b
  is deliberately the concrete consumer MT-3 designs against.
- **debugging**: a real arc, not a palette row — it rides the
  direct-MIR REPL/debug tier reserved by ADR 0001. The palette gets a
  debug entry when that engine seat exists, not before.

## ^R refresh — the "terminal is lying" verb

With delta painting (IDE-9c), refresh must DROP the previous-frame
baseline and hard-clear the terminal (ED + full repaint), not merely
request a repaint: screen corruption means the model's idea of the
terminal is wrong, and a diff against a wrong baseline repairs
nothing (the s135 model-vs-terminal-truth lesson). External junk on
the tty (wall, a chatty background child) is exactly when it's used.

## IDE-10c — INTERNAL builds (owner rulings 2026-08-27, post-10b)

**SHIPPED (session 139).** `madc::build_native(out_diags, path,
"exe"|"obj", outpath)` = the CLI's AOT lane in-process (child-Program
parse + `madc_cir_emit_native`; diagnostics rows either way, a silent
failure synthesizes one error row); `madc::compiler_path()` = the
running compiler's own executable ({madc} in build_subst — Run rows
spawn a child OF SELF through the terminal suspend path). Default ^B
rows: Check / Build / Build object internal, Run rows `{madc} {path}` /
`./{base}`; the exec:// pump machinery stays for manifest-declared
external commands. MT-3b (same day) brought Stop back for internal
builds: the build task opens its own scope and publishes the handle —
Stop = `scope_cancel`, which reaches the child parse through the
cancellation chain (a pre-start flag covers the spawn-to-first-run
window); a stopped build reports "Build stopped", no diags-pane noise.
`madc_object_mode` is now entered ONLY through the scoped
`ObjectModeScope` guard in both emit lanes (dupaudit family
object_mode_emit_scoping, gated: check-object-mode-scope.sh — a leaked
flag would poison an in-process caller's later JIT sessions). Esc backs
out of any pane (one arm in apply_ide_event's key path; palette and
prompts already esc-cancel earlier). Residues: backend diagnostics
still print to stderr (backend-diagnostics-as-data), backend has no
yield points (the emit briefly blocks the loop), failure rows are
pane-shown but not yet auto-jumped-to.

**OWNER RULING**: all ^B items are INTERNAL — madcide runs inside the
compiler; never shell out to a PATH `madc` to do madc things. **OWNER
RULING**: Esc backs out of ANY menu/pane (today only the ^P palette is
esc-closable; ^T options/help/outline/diags/build are not).

Recon DONE (bank — do not re-derive):
- "child" in the parse-handle docs means a child PROGRAM (the
  runtime-eval confinement) — fully in-process. The scaffold:
  `madc_source_diagnostics` / `madc_parse_open_file` in src/ns_madc.cpp.
- THE artifact seat exists: `madc_cir_emit_native(Program *, const char
  *source_name, MadcNativeKind, outpath, cc_link_args)` —
  src/madc_cir.cpp:2088, decl src/madc_cir.h:209; the CLI -o/-r flow
  calls it (src/madc.cpp ~1805; `madc_cir_link_objects` ~1148 for .o
  inputs; MadcNativeKind: mnkRelocatable/mnkShared/mnkExecutable/
  mnkPieExecutable).
- Plan: NEW engine public `madc::build_native(value &out_diags, const
  char *path, const char *kind /*"exe"|"obj"*/, const char *outpath)` —
  child-Program parse (cooperates via parse_yield_point) + emit_native
  when clean; diagnostics rows either way (they land in the diags pane:
  click-to-error, structurally better than [build] text). Run inside a
  `go` task from madcide. RESIDUE: the backend (c2mir/MIR gen/link) has
  no yield points yet — that phase blocks the loop briefly (parse
  cooperates; backend yields = a stage-2 extension).
- Run rows: a child OF SELF for isolation (the user program's exit()/
  crash/stdin must not be the IDE's) — never a PATH `madc`. The
  self-binary-path helper EXISTS: src/madc_globals.cpp ~37-51
  (GetModuleFileNameA / _NSGetExecutablePath / readlink /proc/self/exe)
  — expose (e.g. `madc::compiler_path`) and template Run as
  "{madc} {path}" through the terminal-mode suspend path. NOTE for the
  terminal wrapper: /proc/self/exe must be resolved IN the engine, not
  in the sh -c line (sh would resolve its own exe).
- Stop: an in-process build is not cancellable until MT-3; terminal
  runs are ^C-able in their own terminal. Drop Stop from the DEFAULT
  rows; the exec:// capture pump machinery STAYS (external tools /
  manifest commands later — it is tested and gated) but leaves the
  defaults.
- Esc-any-pane: in apply_ide_event's key arm before edit_key (the
  palette routes earlier; prompts already esc-cancel).

## Slice cut

- **IDE-10a**: palette core (one popup-list widget: filter + select)
  + ^P file palette (buffers + project TUs) + the four key reclaims
  (^F→^K F, ^N→^K E, ^B reserved/stub until 10b, ^P→palette) + ^R
  full-refresh. Bindings and palette rows are both data — one slice.
- **IDE-10b**: ^B build palette — default action rows through
  exec:// + the MT-4b select in the IDE loop; `[build]` output
  buffer; stop = channel close. After the MT-4b seal so the engine
  seat it dogfoods is on develop.
- Later: manifest-declared build commands; fuzzy fs walk in ^P;
  debug entries with the REPL/debug tier arc.

## IDE-9e — windows (^K O split) — DESIGN (decided 2026-08-27, s139)

JOE's third axis lands: **buffers + windows + keys** (AST-5 gave the
ring; this gives two-files-visible). The engine recon settled the
shape: `tui_model` is ALREADY per-slot for everything that matters —
`paint_edit` keeps scroll (`_scroll[slot]`) and hshift per edit slot,
the caret rides each node's hints, and the cursor paints only on the
focused slot. Multiple edit nodes already compose (the prompt shape:
first = flexible, later = one row each). The ONLY engine gap is
height partitioning — and per the lineage north star (the model tree
IS the renderer contract; the GUI twin renders the same data), that
gap closes as DATA, not as engine layout policy:

- **Engine (one hint)**: an edit node's `hints["rows"]` fixes its
  height; unhinted keeps today's rule (first unhinted = flexible,
  other unhinted = 1 row — prompts unchanged). `edit_slot` grows a
  `rows` field read beside caret/tabwidth in walk(). Unit-gated in
  test_tui_model (two hinted edits partition; the negative control
  is the unhinted prompt shape staying byte-identical).
- **madcide (windows as data)**: bag state `windows` = a list of
  `{buf, caret, grow}` rows + `winat` (active index). ABSENT list =
  today's single-window compose, byte-identical (the belt: no split,
  no delta). compose_ide_tree with N>1 windows emits, per window, its
  OWN JOE status line (per-window %n/%m/%R — the recon's "each window
  carries its own status line") + its edit node, heights computed by
  the composer (even split of the content rows ± each window's `grow`,
  min 3; the ACTIVE window absorbs the remainder) and carried as the
  rows hint; the active window's edit node carries the focus hint (the
  palette's AUTOFOCUS seat). Panes/prompt keep taking their rows from
  the bottom, shrinking the split — same rule as today.
- **Focus routing = the AST-5 machinery**: the active window's buffer
  IS the active buffer (edit_file's save/restore seam); nextw/prevw
  saves the leaving window's caret into its row, switches winat, and
  restores — exactly the buffer-switch shape. Two windows on ONE
  buffer (JOE's ^K O default) share the doc; carets live per window;
  the INACTIVE window's caret clamps at compose (min(caret, doc size))
  so edits in one window never strand the other.
- **Views/lenses stay active-window-only** (slice 1): inactive windows
  compose their plain buffer; the view seam (nav_doc) applies to the
  focused window. Named residue: per-window views.
- **Keys — JOE-exact, collisions relocated** (the JOE-parity ruling:
  window verbs take their real seats):
  `^k o` splitw · `^k n` nextw · `^k p` prevw · `^k g` groww ·
  `^k t` shrinkw · `esc 0` killwin · `esc 1` onlywin (JOE tw0/tw1).
  Displaced madcide actions: `profile` and `theme` leave the key
  namespace and become ^T OPTIONS rows (IDE-9d's rows-as-data overlay
  — they are config toggles; that is where config lives); `outline` →
  `^k i` (mnemonic: Index; documented debt — JOE binds ^K I to
  explode, whose show-only-one intent `esc 1` covers, so explode is
  deferred permanently to a slice that finds it a new seat); `view` →
  `^k a` (mnemonic: AST views; debt — JOE's center-line, which madcide
  lacks). ⚠️ OWNER REVIEW requested alongside the pending "^K ;
  rebind review": the outline/view seats + the profile/theme demotion
  to Options rows. pico.keys untouched (no collisions; its palette
  bindings are already a pending owner call).
- **Esc does NOT close windows** (esc backs out of PANES; a window is
  layout, not a pane) — killwin is explicit.
- **Tests**: testmadcide gates — split tree shape (2 status + 2 edit
  nodes, rows hints partition), nextw round-trip (per-window carets
  survive), same-buffer split + edit-clamp, killwin/onlywin, grow
  bounds, single-window compose byte-identical when no split ever
  happened; test_tui_model unit legs for the rows hint.

Slice 2 (later): explode proper, per-window views, horizontal splits
(the GUI twin wants them; the terminal model stays vertical stacks),
bottom output window pinned to [build].
