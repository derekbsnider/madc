# madcide: staged parsing, background compile, and editor state

Owner directives (2026-08-26, mid-session): "we don't want pausing during
loading … we should be able to start display in colour syntax highlighting
right after lexing, and continue the parsing in the background while
letting the user edit; we also do need to plan the saving editor state in
binary form for caching purposes."

This doc turns those into three staged slices, each honouring the banked
rulings (no user-program cache files 2026-08-22; thread-safety law
2026-08-20; the frozen-artifact taxonomy's kind-typed artifacts).

## Where the time actually goes (measured, session 135/136)

- First paint no longer waits on anything (s135 wave 3: paint before
  parse-on-load; NAS 0.38s).
- The PARSE (parse_open) blocks the event loop after that first paint:
  keystrokes queue until it lands. C files: near-instant (embedded
  headers). C++ files: 0.70s (release) / 3.32s (dev -O0) on the NAS —
  installed-header cost, not TU cost.
- Highlight spans "pop in" only when that parse lands, because the ONLY
  span source today is `madc::parse_spans` (retained full-parse state).

## Stage 1 — lexical spans at load (colour right after lexing)

Of the span classes, `keyword` / `number` / `string` / `comment` are
purely lexical — that is also everything real JOE shows (a .jsf is a
lexer). Only `type` and `function` need the tree.

- New engine public `madc::lex_spans(value &out, const char *text,
  const char *filename)`: tokenize the buffer text ONLY — no include
  ingestion, no parse, no sema — and emit the same `{line, column,
  length, class}` rows.
- ONE classifier: factor `internal_program_parse_spans`'s token loop
  (class mapping + trivia comment rows + the tfSYNTHPOS skip) into a
  shared helper both publics call. No second classification table.
- madcide `run_ide`: paint document → `lex_spans` → paint colours →
  THEN `ensure_phandle`; when the full parse lands, `refresh_spans`
  replaces the rows wholesale (types, functions, diagnostics join).
- Cost: milliseconds even for large files (lexing the TU text is not
  the expensive part; headers are).
- Contract note: `--std` keyword sets differ by dialect; lex_spans
  resolves the std from the filename the same way parse_open does.

## Stage 2 — the parse leaves the event loop (edit while it compiles)

The .mad app stays single-threaded; concurrency is ENGINE-owned with a
stated contract (thread-safety law):

- `parse_open` (or a new `parse_open_bg`) returns the handle at once;
  the compile runs on one engine worker; the handle's retained state
  swaps atomically on completion. Publics (`parse_spans`, `parse_outline`,
  `parse_enclosing`, `parse_check`) serve the LAST COMPLETED state
  (empty until the first completion) — concurrent reads safe, one
  writer inside the engine.
- Completion must WAKE the blocked event loop, not wait for the next
  keystroke: the tui target's poll watches an engine wake fd beside the
  tty; the loop surfaces it as a semantic event (`parse-ready`), and
  madcide's existing "first compose after handle" path repaints spans +
  the fn seat. This wake-fd seam is generic (a future build/notify uses
  the same channel).
- Edits during the compile: the buffer is COPIED into the child at
  parse_open (already true — the child owns its text), so edits never
  race the worker; a parse that lands after further edits is stale by
  one round exactly as today's save/check cadence is.
- This slice carries the TSan-lane note: it is the first engine-side
  worker the madcide path exercises.

**THE WALL (recon 2026-08-26, before any stage-2 code):** the front end
relies on STATIC active-owner state — `TokenBase::_active_strpool` /
`_active_valpool` ("bound to the currently-processing Program … compile
is sequential per-Program", tokens.h) and the static
`_parse_file/_parse_line/_parse_column` parse cursor. A second compile
thread rebinding those while the main thread lexes/evals is a data
race by construction.

**RULED (owner, 2026-08-26 evening — after the MT arc landed):** the
decision is between a THREAD and COOPERATIVE CHUNKED parsing; fork is
DROPPED (its one unique virtue, crash isolation, is the error-tolerant
parse arc's job; against it: every product serialized over a pipe,
parse-handle queries crossing a process boundary, and win64 has no
fork — the three-platform law). **Stage 2 = cooperative chunks on the
MT-1 substrate; the thread is not rejected but DEFERRED to F2/M:N,
where it was already scheduled.**

Why cooperative won, precisely: a STACKFUL task makes the parser
resumable FOR FREE — a recursive-descent parser's state is its C++
call stack, which is exactly what an MT-1 context preserves across
yield(). Chunking needs only YIELD POINTS (a check in the token pump
every N tokens, or at top-level-declaration boundaries), not a
re-entrant rewrite. A 0.7s release-tier parse sliced at ~10ms is
imperceptible at the keyboard. The one real work item: the
ACTIVE-OWNER STATICS MUST SWITCH PER TASK — a parse task yielding
mid-parse while the editor runs a keystroke lex_spans (its own child
parse) would clobber the actives — and that is the SAME per-context
state-switch seam MT-2 shipped for exception state
(`__madc_except_state_save/restore` at every switch): enumerate the
parse-session actives, save/restore them in the same place. Single OS
thread = no races, no TSan; the swap set is bounded and DETERMINISTIC
to gate (interleave two parses at every yield point — results must be
byte-identical to serial). The enumeration is a DOWN PAYMENT on the F2
audit (the same inventory thread_local needs), so the thread upgrade
later changes no user-visible behavior — the Go contract again.
Dogfood bonus: madcide is a madc program, so stage 2 becomes the first
real consumer of the language's own `go`/channels (the IDE spawns the
parse task and recvs progress).

Stage 1 (shipped) removes the visible symptom at load: colour is
immediate, and the parse blocks only the FIRST keystrokes (0.7s
release-tier on the NAS). Stage 2's urgency is now the EDIT cadence
(check/save reparse pauses), which is also where incremental reparse
(the banked lever) competes for the same budget.

## Stage 3 — editor state and the IDE-cache kind (measure first)

Two different things hide in "saving editor state":

1. **Editor state** (caret, window top, search history, bookmarks —
   JOE's `~/.joe_state` shape): small, per-path, no ruling against it.
   Plain text under the profiles convention (`state` beside the
   profiles, or `$XDG_STATE_HOME/madcide`), loaded at open. Not binary,
   not a cache — nothing to invalidate.
2. **Parse cache** (retained AST to skip the load-time parse): this is
   the frozen-artifact taxonomy's **IDE cache** kind — kind-typed
   artifact, kind-locked loader, and the 2026-08-22 ruling stands: no
   .forest caching of user programs; the only frozen forest is the
   system-header pack. An IDE-cache artifact is a DIFFERENT kind and
   needs (a) an owner scoping call on what it may hold, and (b) a
   measurement showing it beats stages 1+2 — which likely make it moot
   at load time (colour is instant via lex_spans; the parse is
   background). Decision deferred until stages 1+2 are measured.

## Order

1. Stage 1 (small, standalone, immediate owner-visible win).
2. Stage 2 (engine worker + wake fd; its own design pass on the wake
   seam before code).
3. Stage 3.1 (editor state file, trivial) any time; 3.2 only on
   measurement.
