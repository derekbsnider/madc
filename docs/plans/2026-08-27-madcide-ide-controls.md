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
  `^k t` shrinkw · `^k 0` killwin · `^k 1` onlywin (JOE spells tw0/tw1
  on Esc digits, but madcide's Esc is chord-cancel + close-pane by
  construction and ruling — an Esc-prefixed binding can never complete,
  the chord machinery hard-codes Esc as cancel — so the digits move
  under ^K; JOE's ^K digits are bookmarks, which madcide lacks: debt
  documented).
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

## OWNER RULING (2026-08-27) — the running madc IS the compiler; ^B never execs madc

> **SHIPPED (session 140, feature/parse-build-run-claude):** the engine
> pair `madc::parse_build` / `madc::parse_run` (build/run the LIVE
> parse handle's tree; `internal_program_parse_build/run` beside the
> handle registry), the fork discipline (`__madc_task_atfork_child` +
> io-layer hook; pid-guarded tui atexit recovery; the system(3) signal
> shape; guest exits via `exit()` so its atexit semantics run), and the
> madcide flows (Build refreshes the handle from the SCREEN text and
> emits from it — the disk does not compile; Run = `run_buffer`
> red-parse-refusal → suspend → `parse_run` → pause → resume; the
> `Run {madc} {path}` row is DEAD, `{madc}` substitution survives for
> manifest commands only). Gates: testparserun (engine pair),
> testmadcide live-buffer-build / native-build-fail inverse pair +
> run-headless + run-refused-red + run-internal. The fork-isolation
> eval children (exec_compiled_in_child / call_in_child) adopted the
> same atfork reset. gcc/clang --emit lanes remain the deferred
> SECONDARY arm; Windows remains the flagged owner call.

Verbatim core: "I never wanted Ctrl-B to present a pile of options that
result in calling out to an external madc binary — that makes no
sense! The only external binary option would be when you want to use
gcc or clang to do the compiling, and that would also require emitting
C/C++ code to hand off — but that is secondary to the PRIMARY method
of doing everything using the madc that is ALREADY parsing the program
you are seeing on the screen."

This corrects two shipped shapes that violated the intent:
- IDE-10c's `Run {madc} {path}` row (a child-of-self is STILL an
  external madc that re-parses) — REPLACED by fork-Run below.
- `madc::build_native` re-parsing the file from its path — it must
  emit from THE BUFFER'S EXISTING parse handle's tree (the handle
  madcide already keeps live for diagnostics/outline/spans). The
  buffer on screen — unsaved edits included — is what compiles.

The ruled ^B architecture:
- **Check** = the handle's diagnostics (already true).
- **Build exe / .o** = emit from the handle's cir_node tree (which IS
  c2mir's node_t — no translation) straight through the existing emit
  seam. No fresh parse, no child Program from the path.
- **Run** = fork() at the post-parse point (POSIX): the child inherits
  the parsed tree by COW, hands it to c2mir → MIR, runs; the parent
  IDE gets process isolation without exec. Child discipline: (1)
  __madc_task_atfork_child scheduler reset (queues/waiters/timers to a
  fresh root; single OS thread makes the fork itself clean); (2) tui
  suspend BEFORE the fork, parent waits, resume on reap (the IDE-10c
  suspend seam); (3) exit discipline — the guest's atexit semantics
  run, the parent's inherited handlers must not fire in the child;
  (4) output/stop ride the existing fd/pid machinery (exec:// pump,
  chan_readable, wait_or_kill) — only the spawn step changes.
- **Run native** = run the BUILT artifact (the user's own program) —
  stays.
- **gcc/clang lanes (SECONDARY, later)**: the only external-compiler
  rows, fed by --emit=c11/c++ output, labeled as such.
- **Windows**: not live (ui_term is POSIX-only — no win tui target).
  When it lands: cygwin-style copy OR forest serialization over a pipe
  (ephemeral transport, not a cache file — flag against the
  no-user-program-forest-cache ruling; owner call at that time).

## Windows console TUI (recon QUEUED — owner direction 2026-08-27)

Owner direction: target **Windows 10+ VT mode** — enable
`ENABLE_VIRTUAL_TERMINAL_PROCESSING` on the output handle and drive the
console with the SAME VT-102-style escape codes the Linux/macOS target
already emits. No antiquated DOS-style console-cell API
(WriteConsoleOutput grids). The owner's sketch:

```c
#include <windows.h>

void enable_vt()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD mode;
    GetConsoleMode(out, &mode);

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(out, mode);
}
```

What that means for the architecture: the entire paint side — the grid
renderer, SGR table, tui_diff_spans/tui_diff_plan scroll optimization,
alternate-screen enter/exit — is reused byte-for-byte; only the
`tui_target` TERMINAL layer needs a Windows implementation (ui_term.cpp
is one POSIX implementation of the interface, registered under
`#ifndef _WIN32`; the model tree / rows-as-data engine is
platform-neutral by design).

Recon items for the slice (the POSIX seat → its Windows twin):

1. **Raw mode**: termios save/raw/restore → `GetConsoleMode` +
   `SetConsoleMode` save/restore on BOTH handles (input: drop
   `ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT`;
   output: the owner's VT-processing enable +
   `DISABLE_NEWLINE_AUTO_RETURN` recon).
2. **Input**: poll(STDIN) + tui_keyparse VT-sequence parsing →
   `ENABLE_VIRTUAL_TERMINAL_INPUT` makes the console DELIVER VT
   sequences, so tui_keyparse itself should be reusable; read via
   ReadFile/ReadConsole on the input handle. Verify which chords the
   VT input mode actually delivers (^S/^Q/^Z handling has no termios
   IXON/ISIG axis on win — recon what PROCESSED_INPUT off yields).
3. **Resize**: SIGWINCH → no signal; with VT input the console can
   deliver resize as window-buffer-size events via ReadConsoleInput —
   recon whether mixing ReadConsoleInput (events) with VT byte reads is
   needed, or whether polling GetConsoleScreenBufferInfo at the wake
   cadence suffices.
4. **The scheduler seam (MT-4c)**: taskio::host_wait_readable parks on
   an fd via poll(); the console input is a HANDLE —
   WaitForSingleObject/WaitForMultipleObjects arm in the io-wait hook's
   win side (which today is the cheap-blocking pipe design). This is
   the one seat that touches the scheduler; keep it inside taskio.
5. **suspend/resume** (^K Z, Run native): console-mode restore is the
   termios-restore twin; system() exists.
6. **Run** (fork-Run): SEPARATE and already flagged — no fork on
   Windows; the cygwin-style-copy vs forest-serialization-over-a-pipe
   decision (owner call) is not a TUI blocker: the win TUI can land
   with Build/Check/Run-native live and the fork-Run row absent.
7. **Validation**: wine's console emulation is only partly trustworthy
   for VT input — genuine-Windows validation (the win64 release lane's
   model) is the oracle; recon what a pty-style automated gate looks
   like there (ConPTY is the Windows pseudo-console API and could
   drive a CI-shaped gate later).

Deliverable shape: one new `tui_target` implementation (win console),
registered under `_WIN32` beside the POSIX registration, plus the
taskio win wait arm. Sequenced AFTER the variadic class-template arc.

### Recon findings (2026-08-28, s141 — code-verified against the POSIX seats)

Each item below names its POSIX seat and the Windows twin. Items marked
**[validate-win]** are documented Windows semantics that genuine-Windows
hardware must confirm before the slice ships (wine's console is not the
oracle — item 7).

1. **Raw mode** — POSIX seat: `term_target::enter_grid_mode`
   (src/ui_term.cpp): termios raw (IXON off, ISIG off, ECHO/ICANON off,
   OPOST off), `_saved` = the pre-open state, `leave_grid_mode` restores
   it. Win twin: save BOTH handles' modes at open (`GetConsoleMode` on
   `GetStdHandle(STD_INPUT_HANDLE/STD_OUTPUT_HANDLE)`), then
   input `&= ~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT)`,
   `|= ENABLE_VIRTUAL_TERMINAL_INPUT`; output
   `|= ENABLE_VIRTUAL_TERMINAL_PROCESSING|DISABLE_NEWLINE_AUTO_RETURN`
   (the OPOST-off twin). The escape stream is byte-identical: the
   alternate screen (`\x1b[?1049h`), CUP/SGR/DECSTBM/DL/IL, and cursor
   show/hide all ride VT processing on Win10 1809+. `isatty` twin:
   `GetConsoleMode` succeeding IS the "is a console" test.

2. **Input** — `tui_keyparse` (include/madcdis/tui_model.h:660) is a raw
   bytes→keys CSI/SS3 state machine with an explicit bare-ESC flush; with
   `ENABLE_VIRTUAL_TERMINAL_INPUT`, `ReadFile` on the input handle
   delivers exactly that alphabet, so the parser is REUSED UNCHANGED.
   The 25 ms escape-grace read (`input_ready(25)`) twins as a bounded
   console wait. Chords: Windows has no IXON/ISIG axis — with
   PROCESSED_INPUT off, ^S/^Q arrive as 0x13/0x11 and ^Z as 0x1A;
   ^C arrives as 0x03 **[validate-win]** (conhost vs Windows Terminal
   may differ on ^C once VT input is on; also probe ^Space and
   shift-modified keys, known VT-input gaps in conhost).

3. **Resize** — no SIGWINCH. Decision: do NOT mix `ReadConsoleInput`
   event records with `ReadFile` VT bytes (two readers on one stream,
   and event-record reads bypass the VT translation). Instead poll
   `GetConsoleScreenBufferInfo` (the `srWindow` extent) at the
   `read_keys` loop head — the exact seat where the POSIX loop checks
   `g_winch` — and bound the blocking wait (~200 ms cadence) so a
   resize with no keystroke still surfaces. One cheap API call per
   wake; the model's resize keyev flows unchanged.

4. **Scheduler seam (MT-4c)** — FINDING, a live pre-req bug for the
   slice: `io_probe_readable`'s `_WIN32` arm (src/madc_task_chan.cpp)
   is PIPE-ONLY — `PeekNamedPipe` FAILS on a console handle and the
   failure arm returns `true` ("readable"), so a console-parked
   `host_wait_readable` would busy-wake `read_keys` forever. The slice
   adds a console arm: `GetConsoleMode(h,·)` succeeding classifies the
   handle; probe via `GetNumberOfConsoleInputEvents` (>0). CAVEAT
   **[validate-win]**: non-key records (focus/menu/mouse) count as
   events and can signal without producing VT bytes — confirm whether
   VT-input mode filters them before `ReadFile`, else the probe needs a
   husk-drain (`ReadConsoleInput` discard of non-key records) — keep it
   inside taskio. `io_wait_hook`'s win arm upgrades the same way:
   consoles are WAITABLE objects — split waiters into waitable
   (console: `WaitForMultipleObjects`, real timeout) and pipes (the
   existing 1 ms `PeekNamedPipe` cadence); with any pipe present the
   wait quantum stays 1 ms, console-only waits block properly.

5. **Suspend/resume** (^K Z, Run-native) — `enter_grid_mode` /
   `leave_grid_mode` are already the one shared pair; the win twin is
   SetConsoleMode-restore of the two saved modes + the same VT exit
   bytes. `system()` exists (cmd /c); the atexit recovery's pid guard
   (`g_live_pid`, a fork-child discipline) holds trivially — no fork on
   Windows.

6. **Run (fork-Run)** — the separate owner call; comparison in the next
   subsection. NOT a TUI blocker: the win TUI lands with
   Build/Check/Run-native live and the fork-Run row absent.

7. **Validation** — wine's VT OUTPUT emulation is usable for smoke; VT
   INPUT delivery under wine is the untrustworthy leg, so
   genuine-Windows hardware (the win64 release-lane model) is the
   oracle for items 2 and 4's caveats. CI-shaped gate later: ConPTY
   (`CreatePseudoConsole`, Win10 1809+) can host the packed PE, script
   input bytes, and capture output bytes — the pty-gate twin.

Deliverable shape (confirmed): src/ui_term.cpp is `#ifndef _WIN32` for
the POSIX body; the slice adds the win `term_target` twin (same
registration name under `_WIN32` — a sibling `#else` body or
ui_term_win.cpp, one tui_target per platform) + the taskio console
probe/wait arms. The paint side (grid renderer, SGR table,
tui_diff_spans/tui_diff_plan, alternate screen) is reused byte-for-byte.

### Windows Run — copy the AST to a child (OWNER CALL, banked 2026-08-28)

POSIX Run = fork at post-parse: the child inherits the LIVE parse tree
and runs c2mir on it ([the running madc IS the compiler]). Windows has
no fork. Two candidate designs:

- **(a) cygwin-style self-copy**: `CreateProcess` of the own exe
  suspended + replicate the parent's memory regions into it. Faithful
  to fork's zero-serialization cost, but fights ASLR/handle
  inheritance/CRT state, needs deep win-internals machinery this repo
  otherwise never carries, and its failure modes are silent corruption.
  (Cygwin itself needs a decade of special cases to keep this working.)

- **(b) forest-serialization-over-a-pipe**: the buffer's parse handle
  FREEZES its tree via the existing cir_freeze machinery into a child
  `madc --run-frozen` reading the container from a pipe/temp handle;
  the child thaws (LOADED == parsed — never a re-parse) and runs c2mir.
  Rides two standing invariants — the forest IS save/load state, and
  one implementation per concern (the freeze path already exists,
  battle-gated by the packed/headerless lanes). Cost: freeze+thaw
  latency (the packed-launch class, ~150 ms order) instead of fork's
  ~0; the child is a REAL process with clean CRT/console state, which
  on Windows is a robustness win (no inherited-console fights).

Recommendation to the owner: **(b)** — it reuses gated machinery and
its cost class is already the accepted packed-launch latency; (a)
imports a permanent maintenance frontier for one feature. The ruling is
the owner's (banked as recon item 6's fork; madcide's Run row stays
absent on win until it lands either way).
