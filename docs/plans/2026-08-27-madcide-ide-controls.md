# madcide IDE controls — palettes, reclaimed keys, build/run (IDE-10)

**STATUS: IDE-10a + IDE-10b SHIPPED (2026-08-27, session 138)** — the
palette core (model list+autofocus hints), ^P file palette, the four key
reclaims + ^R (ui::tui_refresh), and the ^B build palette (capture pump
on the MT-4b mixed select; channel::cancel() SIGTERM stop; terminal-mode
runs via suspend/resume; command rows as data). Engine seams that landed
with it: __madc_task_fire_due (yield fires due io beside timers) and
read_keys' unified cooperative wait (50ms cadence while live-but-parked
tasks exist — the MT-4c stdin-unification retires it). Remaining from
this doc: manifest-declared build commands, ^P fuzzy fs walk, the debug
row (ADR-0001 REPL/debug tier), pico.keys palette bindings (owner call).

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
