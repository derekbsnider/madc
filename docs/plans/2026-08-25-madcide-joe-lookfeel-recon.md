# madcide — JOE look/feel/workings recon (owner directive, 2026-08-25)

**Owner (2026-08-25):** JOE is the favourite editor; madcide's goal is to
REPLACE it — support all the JOE features the owner is used to, then ADD
the things beyond it. One concrete call already made: JOE has a SINGLE
status line at the top; madcide should emulate that to maximize screen
real estate. This doc banks the empirical recon (a real `joe` 4.6 driven
on a real pty at 80x24, plus its shipped `/etc/joe/joerc`) and the gap
list against madcide's current composition.

License note: JOE is GPL; madc is MPL 2.0. This recon observes BEHAVIOR
and CONFIG VOCABULARY (facts on a screen, documented format escapes) —
no JOE source is read or ported, ever. The repo already names JOE openly
as the parity target (joe.keys, the tui_attr vocabulary).

## A. Observed screen model (pty capture, joe 4.6, xterm 80x24)

1. **One window = one INVERSE status line at the TOP + 23 content rows.**
   No heading banner, no persistent bottom menu. The bottom row is
   normally CONTENT.
2. **Startup hint on the bottom row, transient** (`-xmsg`: "Joe's Own
   Editor 4.6 (utf-8) ** Type Ctrl-K Q to exit or Ctrl-K H for help **")
   — overwritten by content on first scroll/repaint.
3. **The status line is DATA** — two format strings in joerc:
   - `-lmsg \i%k%T%W%I%X %n %m%y%R %M %x` (left-justified)
   - `-rmsg  %S Row %4r Col %3c` (right-justified; FIRST char = the fill
     character between the halves)
   Escape vocabulary (from joerc's own docs): `%t/%u` time, `%T`
   overtype/insert, `%W` wordwrap, `%I` autoindent, `%X` rectangle mode,
   `%n` name, `%m` "(Modified)", `%*` modified star, `%R` read-only,
   `%r/%c` row/col (width-padded `%4r`), `%o/%O` byte offset, `%a/%A`
   char under cursor, `%w` char width, `%p` percent, `%l` line count,
   `%k` entered prefix keys (chord echo!), `%S` shell-running, `%M`
   macro-recording, `%y` syntax, `%e/%b` encodings, `%x` CONTEXT,
   `%dd/%dm/%dY` date, `%Ename%` env, `%Zname%` option value. Attribute
   escapes: `\i` inverse `\u` underline `\b` bold `\d` dim `\f` blink
   `\l` italic — the same JOE attribute vocabulary tui_attr already
   speaks (AST-2).
4. **`%x` context = the first non-indented line going BACKWARDS** — a
   textual heuristic for "enclosing function". Observed live: after
   scrolling into fn30, the status line showed `int fn30(void) {`.
   madcide's AST-1 `parse_enclosing` is the SUPERIOR analogue (exact,
   parse-backed, position-true) — keep ours, render it in the `%x` seat.
5. **Prompts OVERLAY the top status row** ("Find (^K H for help):"),
   with `-aborthint ^C` / `-helphint ^K H` as the advertised keys; TAB
   completion at prompts. Messages ("File saved") also render on the
   top row until the next keystroke.
6. **^T options = a letter-keyed multi-row overlay under the status
   row** showing LIVE option values ("I Autoindent ON", "D Tab width 8",
   "R Right margin 77", ...). Format-driven per joerc (same escape
   language as -lmsg).
7. **Scroll mechanism (byte counts from a 30-row scroll):** JOE scrolls
   with `CSI M` (delete-line) + raw LF at the bottom margin (1 + 38)
   and repaints ONLY delta rows with `EL` (erase-to-EOL, 9) — terminal
   scroll ops + minimal repaint, NOT a full-grid repaint per step. That
   is the "instant" JOE feel, and the EL discipline is exactly what
   keeps short lines from leaving stale tails of longer predecessors.
8. Not captured this pass (next recon round): `^K O` window split (each
   window carries its OWN status line), `^K H` help ribbon paging, the
   `^K ;`/tags flow, prompt history (up-arrow), rectangular blocks
   visuals, shell-window (`F1`-era `joe -shell`) rendering.
9. **Syntax colours (owner report 2026-08-26: madcide's colours didn't
   match joe's).** joe 4.6 splits syntax CLASSES
   (`/usr/share/joe/syntax/c.jsf`) from colour SCHEMES
   (`/usr/share/joe/colors/*.jcf`; `default.jcf` unless configured).
   The default scheme for C is bold-accents, not rainbow: Comment
   green; Constant cyan (c.jsf: Number and String both inherit
   +Constant); Keyword BOLD (no colour); Type BOLD (no colour); idents
   plain; Preproc blue; Define bold blue; IncLocal cyan / IncSystem
   bold cyan; Escape bold cyan; Brace magenta; Bad bold red.
   **Applied**: `profiles/default.theme` is now this mapping for the
   classes we classify (keyword/type bold, string/number cyan, comment
   green; `function` unthemed — joe has no function class); the old
   colourful look ships as `vivid.theme`. **Named classifier seats**
   (HighlightClass extensions) to reach full parity: hcPreproc (blue;
   directives are consumed at lex — needs directive extents recorded),
   hcBrace (magenta; braces are hcNone operators today), string-escape
   sub-spans (bold cyan), hcBad. joe also ships 8 more schemes
   (gruvbox, solarized, zenburn, molokai, ...) — portable later as
   pure theme data.

## B. madcide today (compose_ide_tree) — the delta

Current: heading banner at TOP ("=== madcide: file ==="), edit node,
optional pane (help/diagnostics/outline) as a labeled box, STATUS +
MENU at the BOTTOM.

| Concern | JOE | madcide today | Delta |
|---|---|---|---|
| Status line | one inverse line at TOP, format-string data | status + menu at BOTTOM, fixed shape | move to top; ONE line; format-as-data |
| Heading banner | none | "=== madcide: … ===" row | drop (real estate) |
| Persistent menu | none (hints live in prompts) | bottom menu row | drop; adopt -aborthint/-helphint style prompt hints |
| Prompt placement | overlays the TOP status row | bottom prompt line | move to top overlay |
| Chord echo | `%k` on the status line | (none visible) | render entered prefix (`^K`) in the status line |
| Enclosing context | `%x` textual heuristic | AST-backed `fn X` suffix | keep AST version, render in the `%x` seat |
| Row/Col | right-justified `Row %4r Col %3c` + fill char | "Ln, Col" in bottom status | adopt rmsg right-align model |
| Options | ^T live overlay | (none) | named seat |
| Scroll | dl+LF scroll ops, delta repaint w/ EL | grid repaint (see defect below) | fix correctness first; scroll-op perf a later lever |

## C. Open defect — scroll artifacts (owner report, 2026-08-25)

Symptoms: scrolling down sometimes leaves "extra bits of the previous
line"; lines containing `{` appear DOUBLED while scrolling. Suspects
(hypothesis, unverified): the grid painter's repaint/diff path during
scroll — a shorter new line over a longer old one needs erase-to-EOL
(JOE's EL discipline); brace-only lines are the shortest lines in code
AND (with AST-2 spans) attribute-transition rows, so they concentrate
both failure modes. First discriminator: does it reproduce with no
theme loaded? Test plan: a pty scroll harness driving the REAL madcide
binary (tmp/joe_recon.py's minimal VT100 screen-reconstruction
interpreter is the seed — promote it into a proper harness), asserting
every reconstructed screen row equals the expected buffer line after N
scroll steps; the assertion stays as the regression gate.

## D. Proposed slices (named seats; owner-gated ordering)

- **IDE-9a — status line as data**: JOE's format vocabulary (%-escapes +
  \\attr escapes through tui_attr, the one spec parser) rendered from a
  profile-owned format string; TOP placement; rmsg right-align + fill
  char; %k chord echo; the AST-backed enclosing in the %x seat.
- **IDE-9b — screen real estate**: drop the heading banner and the
  persistent menu; prompts and messages overlay the status row with
  aborthint/helphint; transient startup hint at the bottom.
- **IDE-9c — scroll correctness (the defect), then feel**: fix the
  stale-tail/doubled-line artifacts at the renderer layer with the pty
  regression harness; delta-repaint + EL discipline; terminal scroll
  ops (dl/LF) as a later measured perf lever.
- **IDE-9d — ^T options overlay** (live values, letter-keyed).
- **IDE-9e — windows (^K O split)**: per-window status lines — the big
  one, after AST-5's multi-buffer plumbing.

AST-5 (project/multi-buffer) stays the next arc slice; IDE-9a/9b are
small and high-visibility; IDE-9c starts with the defect (bugs first).
