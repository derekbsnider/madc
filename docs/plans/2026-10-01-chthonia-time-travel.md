# chthonia time travel: scrubbing a program's execution (Phase 7's stepper)

Design, 2026-10-01. The owner's idea: scrub back and forth through a program's
run with a scrubbing wheel, as in a video editor, and watch the line, the
variables and the output follow. Play, pause, step and stop remain; scrubbing
joins them. With tasks, the timeline has a track per task, as an editor has
tracks or layers.

This is the design of the plan's Phase 7 stepper
(`madc-repl-thonny-plan-2026-09-24.md` §21, §31). It answers the executor
question §21 left open, and it replaces Phase 7's "optional step-back" with
stepping in both directions as the model itself.

## 1. Precedents, and what each one gives

| Precedent | What it does | What chthonia takes |
|---|---|---|
| Thonny's "nicer" debugger | Step over (F6), Step into (F7), Step out, Resume, Run to cursor, and **Step back**: it keeps every intermediate state, so a step can be undone as often as wanted (`thonny/plugins/help/debuggers.rst`, since Thonny 2.2) | The commands, their names and keys. A Thonny user already knows stepping backwards |
| Python Tutor (pythontutor.com) | A slider under the code over a recorded execution; each step shows the line, the frames, the heap and the output; it stops at 1,000 steps on purpose, because it is meant for blackboard-sized programs (Guo, SIGCSE 2013) | The scrubber, record-then-browse, and a stated cap |
| rr (O'Callahan et al., "Engineering Record and Replay for Deployability", 2017) | Records unmodified Linux processes at the user/kernel boundary (`ptrace`, scratch buffers, one thread at a time, hardware branch counters for asynchronous events) and replays them exactly; reverse execution is checkpoints plus replay forward | The one-thread-at-a-time rule (madc's tasks already have it) and, only if measured need appears, checkpoints plus re-execution for long runs. Not its machinery: rr refuses to cooperate with the compiler, and madc *is* the compiler |
| Pernosco, WinDbg Time Travel Debugging, UndoDB | Omniscient debugging: the whole run is queryable ("where did this value change?") | "Previous / next change of this variable" as a later query |

## 2. Decided (owner, 2026-10-01)

1. **Time travel is Phase 7's stepper design.** Step over, into and out move a
   playhead over a recorded run; Step back is the same move backwards; the
   scrubber moves it anywhere.
2. **The camera model.** Debug runs the program, which runs ahead and records.
   The playhead follows it live and can be dragged back at any moment; pausing
   is the playhead no longer following. A program waiting for input waits, as
   it would under Run. (Not Thonny's model, where the program stops at every
   step: here stopping at a step is just where the playhead is.)
3. **The executor is the JIT, with statement probes** — §21's stated
   trade-off. The MIR interpreter stays not assumed: its thunks risk every ABI
   path the JIT has proven, and it is slow. One IR, one lowering, so a traced
   program is the program the user runs.
4. **Record state, not inputs.** Each step's changes are kept, so nothing is
   replayed and nothing has to be deterministic: `scanf`, `rand` and the clock
   happened, and the record says what they did. rr-style checkpoints and
   re-execution come only if measured need appears (§6, T4).
5. **The UI stays simple and clean:** one new element, the timeline strip,
   shown only after Debug, one lane unless the program has tasks (§3).
6. **Placement: after the master release** (plan §41.11a steps 0-9), as the
   headline of the chthonia product release that follows it.

## 3. The UI

The mockup (`inbox/chthonia-mockup.png`: editor tabs, Variables and Call Stack
on the right, Shell and Build below, a toolbar of file actions, Run ▾,
Debug ▾, Stop and the step buttons) gains one element, a strip between the
editor and the bottom panel:

```
 ⏮ ◀ ▶ ⏭  ━━━━━━━━━━━●━━━━━━━━━━━━━━━  step 37 / 120   main.c:7
```

- **The playhead moves everything:** the editor's highlighted line, the
  Variables view (locals of the frame at the playhead, then globals), the Call
  Stack, and the Shell, whose output after the playhead is dimmed.
- **The toolbar's step buttons move the playhead:** Step over (the next step
  in this frame or an outer one), Step into (the next step), Step out (the
  first step after this frame returns), Step back (each of these, backwards).
  At the live end, a forward step waits for the program, as Thonny's does.
- **First / last** (⏮ ⏭) and dragging; the strip's own keys are Home / End
  and the arrow keys when it has focus.
- **Tasks:** when the program spawns a `go` task, the strip grows one thin
  lane per task, colored where that task ran; a spawn and a channel's send and
  receive are small marks. Clicking a lane picks whose locals and stack the
  right column shows. A program without tasks never sees lanes.
- **Run (F5) never shows the strip** and never pays for recording.
- **Keys** follow the active key style: Thonny's Debug is Ctrl+F5, Step over
  F6, Step into F7; the other styles bind their own debugger keys, as
  `vscode.keys` already notes.
- **The terminal** draws the strip as one line (`━` and `●`), driven by the
  same keys. A GUI drag reaches it through the pointer phases `ui_enums`
  already carries (down / drag / up).

The Variables view is chthonia's step-6 view. After F5 it can show globals
only, because `main` has returned; the recording is what gives it the
mockup's `n` and `result` while `fact` recurses.

## 4. The engine

### 4.1 The trace build

- A build mode beside `-g`, not a language feature: `madc --trace`, and the
  REPL backend's debug run. It changes no syntax, no `--std` semantics and no
  registry entry (vision invariants I3, I4 unaffected).
- `CirBuilder::translate_stmt` (the one statement lowering, which also reaches
  single-statement `if` / `while` / `for` bodies that `translate_block`'s loop
  does not) emits, under the mode only, a call to the trace runtime before
  each statement, with the statement's site id; function entry and every
  return emit an enter / leave call. Each probe's node carries its
  statement's origin token (file, line, column), as every node does.
- A per-module site table (file, line, column, function) is emitted once, so
  a step event is an integer.
- The probes are ordinary C11 calls: the trace build also flows through
  `--emit=c11`, and gcc or clang can compile the traced C11 — the oracle for
  the trace gate (§7). One IR, one lowering (I1, I2, I5).
- A normal build contains no probe, and a gate says so.

### 4.2 Snapshots: keyframes and deltas

- At each step the probe renders the variables in scope at that statement
  (the frame's locals, block scope included, then the globals) in the show's
  bounded row form (`MADC_DUMP_SHOW_ROW`: 80 columns, 16 elements, the walk
  `cir_dump.cpp` already generates for `%whos` and the Variables view).
- It compares each row with the previous step's and records only the changes.
  Every K steps it records every row: a keyframe. The state at step k is the
  nearest keyframe plus the deltas after it, so a scrub to any step costs the
  same.
- Re-rendering per step, rather than hooking each assignment, catches what an
  assignment hook would miss: a write through a pointer, `v.push_back(4)`, a
  callee that changed a global.
- The rows are text, so the record holds no pointer into the program and
  survives the program's exit.

### 4.3 Output and input

- Under the trace build, the program's stdout and stderr are unbuffered, and
  the trace runtime drains them at each probe, so every chunk of output
  carries the step that wrote it.
- Input the program read is recorded with its step, so the Shell shows the
  typed `3` exactly where the program consumed it.

### 4.4 Calls, frames and tasks

- Enter / leave events give the call stack at any step (function, the line of
  the call), and Step over / out are computed from them.
- The task runtime's two switch points (`task_switch` and `task_exit_switch`,
  `src/rt/rt_task.c`) record each switch; a task gets a small id the first
  time the trace sees it (`madc_task` has none of its own). Spawns
  (`__madc_go`) and channel sends / receives (`madc_task_chan.cpp`) are
  events. Tasks are cooperative on one OS thread, so the record is one ordered
  sequence, each event tagged with its task: the lanes are a view of it.

### 4.5 Where the record lives

- The program runs in the REPL backend's child process, as F5 does. The trace
  runtime batches its events into a stream to madcide, beside the program's
  output; madcide keeps the record and its index, so scrubbing is local and
  never waits on the running program.
- The event kinds are an enum (`trace_event_kind`), and the stream is its
  input boundary: madcide converts once, at receipt.
- The Variables view reads rows of the shape `bindings` already gives it
  (`{name, type, value}`), taken at the playhead.

### 4.6 Limits

- A cap on recorded steps (a setting; its default is measured in T1, in the
  region of 100,000). At the cap, recording stops and so does the program,
  and the status line says why. Stop works as it does under Run.
- An infinite loop therefore ends at the cap, as in Python Tutor.

## 5. Thread-safety contract

- The trace runtime is per process (the backend's child): one writer, the
  program's OS thread, on which every cooperative task runs. It needs no
  lock.
- madcide's record and index are the session's state, read and written on the
  session's thread between events, as every bag entity is.
- Real threads (the F2 cores arc) would be parallel lanes whose interleaving
  must be recorded (rr's one-thread-at-a-time, or an order of shared
  accesses). The UI model does not change; the runtime contract does, and it
  is decided with F2.

## 6. Staging (its own arc, after the master release)

- **T1 — the trace build and runtime, headless.** `madc --trace=FILE prog`
  writes the record as JSON lines (steps, deltas, keyframes, output, input,
  enter / leave). Measure the cost per step and set K and the cap from it.
- **T2 — the debug run in chthonia, one lane.** The backend's debug op, the
  stream, madcide's record and index, the timeline strip (a new `uinode` role
  with arms in `tui_model.h`, `web_model.h` and `page.js`), the step commands
  as playhead moves, and the Variables view, the Call Stack and the Shell
  following the playhead.
- **T3 — tasks.** Lanes, spawn and channel marks, a lane's selection driving
  the right column.
- **T4 — long runs, only if measured.** A ring buffer, or rr's checkpoints and
  re-execution from the nearest one (which needs the program's inputs
  recorded at the runtime's syscall boundary).
- Later: expression-level Step into (Thonny's "small step", from the retained
  parse subtrees), "previous / next change of this variable", heap arrows.

## 7. Gates

- T1: `tests/testtrace_*` — golden records, deterministic, under JIT and
  `--exe`; the same program's `--emit=c11 --trace` output built by gcc and by
  clang gives the same record (madc's oracle rule); a normal build has no
  probe symbol (a `fulltest` gate).
- T2: the session op in `test_session_backend`; `testmadcide_chthonia` gains a
  debug section (a debug run of §34's program, Step back, a scrub to step k
  showing that step's rows and output); a `tests/gui` case drags the playhead.
- T3: a traced `go` program's lanes and marks, golden.

## 8. Open, decided at each stage's start

- K and the cap's default: T1's measurement.
- How the strip shows a run that is still going (the live end marked, the
  bar growing).
- Expression-level stepping's granularity: the later slice.
