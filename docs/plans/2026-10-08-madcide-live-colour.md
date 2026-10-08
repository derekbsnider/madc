# madcide colours what you type — live colour

Owner (2026-10-08), testing 0.102.1: a function typed into `test.cpp` stayed
uncoloured until the file was saved. Scheduled for the release after 0.102.1.

## Today (measured)

A buffer's colour is computed at load — `lex_first_spans` (the lexer alone,
the first paint) and then the background parse (`parse_pending` →
`background_parse` → `ensure_phandle` → `refresh_spans`) — and again on
check / save (`reparse_buffer`). An edit only SHIFTS the existing spans
(`shift_es_anchors`, editor_events.inc), so typed text has no colour and
semantic colour (a type's name, a function's) goes stale, until a save.
`spans_to_hspans` documents it. 0.102.0 behaves the same: not a regression.

Whole-buffer lexing per keystroke is not the fix (`madc::lex_spans`, the
container's -O0 dev binary): 5 KB ~0.5 ms, 85 KB (2.3k lines) ~43 ms,
366 KB (11k lines) ~230 ms.

## Design — VS Code's two layers

**1. As you type: the edited lines re-lex.** The editor layer records, per
client state bag, the byte range an edit touched since that client's colour
last caught up (`ed_text_insert` / `ed_text_erase` through the per-client
anchor shift, so every client viewing the document gets it, each with its own
theme), and whether the inserted or erased text held a character that can
open or close a multi-line token (`/`, `*`, `"`, `\`). Undo and redo restore
a whole snapshot: the whole buffer is dirty. At composition — the seat every
client's frame passes, as `preview_follow` already is — madcide catches the
colour up:

- the dirty range's lines are lexed ALONE (`madc::lex_spans` over their
  text), their old spans dropped and the new ones spliced in;
- a first line that begins inside a block comment lexes in that state: the
  line BEFORE it (which the edit did not move) ends in an open block comment
  — its last span is a comment to the line's end that is no `//` and does
  not close — so the lines lex behind a `/*` whose two columns come back off
  line 1. (The lexer emits a block comment one span per line, so "a span
  crosses the line boundary" never sees one; each span carries its class as
  a code, `k`, for this test);
- a multi-line-capable edit marks the buffer LEX-DIRTY: the line re-lex runs
  now (best effort) and the whole buffer re-lexes at the next pause (layer 2).

A text kind (Markdown) re-derives its own colour as check does today.

**2. On a pause: the buffer re-parses in the background.** The client loop's
`parse_pending` (after each compose) spawns one cooperative task per
document when its text moved since its colour's parse: it sleeps the pause
(`madc::sleep_ms`), returns if the text moved again (a later frame spawns
the next), re-lexes the whole buffer when lex-dirty, then re-parses
(`parse_refresh`) and applies the parse's spans ONLY if the text did not move
while it ran — spans of an older text would land on the wrong bytes. One task
per document in flight. The document's text revision is a counter the edit
paths bump (`ed_text_insert`, `ed_text_erase`, undo, redo).

## Thread-safety contract

The session's thread only, like every editor-state write today: the dirty
range and revision live on the client state bags and the document entity; the
background step is a cooperative task on the session's scheduler (the
existing `background_parse` model), never a thread. A parse handle stays
confined to the thread that opened it (ns_madc's contract).

## Tests

- typing a function into a C++ buffer colours its keyword, number and string
  in the next frame, with no save (layer 1);
- typing inside a block comment keeps the comment's colour over the new text;
  typing `/*` colours the rest of the buffer as comment after the pause;
- after the pause, the parse's colour of a typed type name appears (layer 2),
  and spans of a parse whose text moved meanwhile are not applied;
- undo restores colour for the restored text.

## Status (2026-10-08)

Both layers work (tests/testmadcide_livecolour.mad). The two compiler gaps
layer 2 needed are fixed on their own branches: an unclosed `/*` colours to
the end of the buffer (fix/lexspans-open-comment-claude) and a user type name
colours as a type wherever the parse reads it as one
(fix/parse-spans-type-names-claude). Next: Chthonia in VS Code's Dark+ — the
TUI facelift plan's S1 colour value (`#RRGGBB` in the one style spec, exact
in the window, truecolor or nearest in the terminal) and then S1b's classes.
