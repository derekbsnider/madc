# Syntax colour at the prompt — design (split arc 1B.9)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1B item 9 (owner 2026-10-09: syntax colour at the prompt; ONE built-in
default theme; madc.ini's `theme` key overrides it; madcide's themes stay its
own data; Phase 0.5's predicate gates it).

## 1. What exists

- **The painter**: `line_painter::paint(line_view, cols)`
  (`include/madcdis/line_edit.h:1082`) repaints the entry every keystroke
  (Julia's full refresh). It lays each line out through `line_layout`
  (`include/madcdis/text_utf16.h:144`, the one width owner). That owner shows
  a control byte as `^X`, so an escape sequence in the text would be shown,
  not obeyed. `line_view` is the prompt, the lines and the caret
  (`line_edit.h:133`). Nothing colours the entry today.
- **The classifier**: `lexical_highlight_rows` (`src/madc_program.cpp:5920`)
  lexes a text alone (no header, macros unexpanded) in a child Program and
  classifies each token through `highlight_token_rows`. Each row is
  `{line 1-based, column byte 0-based, length bytes, class name}`. Lexing alone
  gives keyword, control, type, ident, number, string, comment, directive,
  include and escape. Operators are unstyled. An incomplete entry lexes, and an
  open `/*` colours to the end. `madc::lex_spans` is its dialect face
  (string class names). The child lexes under its default standard, not the
  caller's. Measured on the -O0 dev binary: 1000 calls on a one-line entry
  (13 rows each) cost 0.79 s, under 1 ms a call.
- **The classes**: `HighlightClass` (`include/tokens.h:150`).
- **The style vocabulary**: `ui_style` and its spec reader `ui_style_of` (JOE
  words, `#rrggbb`, `bg_*`) and `ui_colour_depth`
  (`include/madcdis/ui_style.h`). The style-to-SGR step `emit_sgr` /
  `sgr_colour` and the depth probe `detect_colour_depth` are private to
  `src/ui_term.cpp`'s anonymous namespace (the TUI's `vt_paint_bytes`).
- **The predicate** (Phase 0.5, `b13aff979`): `madc::colour_reaches(stream)`
  (`src/madc_console.cpp:108`). It takes `-fdiagnostics-color=`, then
  NO_COLOR, a terminal, and a TERM that is set and not `dumb` (a VT console on
  Windows). Diagnostics colour their header and caret through it
  (`src/lexer.cpp:11116-11156`). The echoed source line is plain.
- **Themes**: none in the engine; the HighlightClass comment says "the
  compiler never styles". madcide's `tools/madcide/profiles/*.theme` hold one
  `class spec` line each, plus `@gui` / `@presence` scopes that are the IDE's.
  madcide reads them with `load_theme` (`tools/madcide/madcide_core.inc:1460`),
  which falls a class the file leaves unnamed back to its parent. madc.ini has
  no `theme` key (`register_madc_keys`, `src/madc_config.cpp:33`).

## 2. Decisions

1. **The painter paints marks.** `line_view` gains marks: per line, byte
   ranges, each with an opaque prefix (an SGR sequence). The painter writes a
   range's prefix before its first glyph and a reset after its last. A mark
   takes no width, so the layout, the wrap and the caret are as before.
   `line_layout` reports the shown offset of each byte (an optional out
   parameter), so the painter inserts a mark into the laid-out text and never
   counts escape bytes. The painter stays bytes only: it knows no classes,
   styles or depths.
2. **The classifier's C++ face**: `madc::lex_classes(text, standard)`, which
   returns spans of `{line, byte, length, HighlightClass}`. The enum stays an
   enum in C++, and `lex_spans` remains the string boundary over the same
   owner. It lexes under the caller's STANDARD (C17's `class` is an
   identifier; C++'s is a keyword); the child adopts it. The REPL host lexes
   in-process: lexing runs no user code, so crash isolation (D2) does not ask
   for the backend, and a keystroke never waits on the wire.
3. **The style-to-SGR step's one owner**: `emit_sgr`, `sgr_colour` and
   `detect_colour_depth` leave the anonymous namespace as `madc::hub::sgr`
   (from-style to to-style at a depth) and `madc::hub::terminal_colour_depth`,
   declared in `include/madcdis/ui_sgr.h`. `ui_style.h` stays
   dependency-free. The TUI's paint and the prompt call the same functions.
4. **The theme: one owner, one built-in default, madcide's file format.**
   - `madc::theme` (`include/madc_theme.h`): a style per `HighlightClass`,
     and per piece of chrome a theme names (`tui_chrome`, moved to
     `madcdis/ui_chrome.h`: madcide's files colour the grid's chrome too).
     - `theme_default()` is madcide's `default.theme`, JOE parity: keyword,
       control, type and typename bold; string and number cyan; comment
       green; directive blue; include and escape bold cyan; the rest plain.
     - `theme_read(text, theme, err)` reads madcide's `.theme` text: `name
       spec` lines (a highlight class or a chrome name), `#` comments,
       `@scope` lines skipped (they are an IDE's). An unknown name or an
       unreadable spec is REFUSED, naming the line (a misspelling is a
       refusal at load, never a dead colour).
     - A class the file leaves unnamed takes its parent's style: control from
       keyword, typename from type, namespace from typename, directive from
       keyword, include and escape from string. This is madcide's
       `load_theme` rule, moved here.
   - A theme file is a whole scheme, not an overlay; one with no lines
     colours nothing.
   - madc.ini's `theme = PATH` (a path key: relative to the ini's directory,
     `~/` expanded) replaces the default. A file that does not read is a
     warning at start, and the default stays. Syntax colour alone is turned
     off with an empty theme file; no word is reserved in a path key.
   - madcide's themes stay its data. Its `load_theme` and `spans_to_hspans`
     move onto this owner in 1D. One of its files named by `theme` works
     unchanged.
5. **Where colour reaches** (the plan's three):
   - the entry as it is typed: every paint, under the session's standard (the
     loop re-keys on `%std`, 1B.7);
   - the echoed source line under a diagnostic (`show_error_source_line`),
     lexed under the Program's standard, beside the header and caret colour it
     already has;
   - `%type`'s shown text (a type: code) and `%pinfo`'s declaration. The
     session marks the shown text that is code (the reply's `shown_code`), and
     the host colours it. A shown value is not code; it stays plain.
6. **The gate**: Phase 0.5's predicate, `colour_reaches(std::cout)` for the
   entry and the shown text, and the diagnostic's own stream for the echoed
   line. Only the terminal loop paints marks; the cooked (piped) loop never
   does. The depth is the TUI's probe (COLORTERM, TERM). There is no new flag:
   `-fdiagnostics-color=never` and NO_COLOR turn it off as they turn
   diagnostics' off; an empty theme file turns off syntax colour alone.
7. **Thread-safety** (C++ standard library convention): a theme is
   immutable once read (the CLI reads it once at start); the painter and its
   marks belong to one loop; the classifier keeps the runtime-eval
   confinement (one Program per thread). No new global state; the default
   theme is a function's constant.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **The owners**: `ui_sgr.h` (`sgr`, `terminal_colour_depth`, the TUI on
   them); the theme (`theme_default`, `theme_read`, the parent rule);
   madc.ini's `theme`; `lex_classes` with the standard. Unit tests: a theme
   read, refusals, the parent rule, SGR bytes per depth, spans under C17 and
   C++.
2. **The prompt**: `line_layout`'s shown offsets, `line_view`'s marks, the
   painter, and `madc_repl_edit` composing marks per paint under the gate.
   Unit tests: the painter's bytes with marks (widths, wrap and caret
   unchanged); a mark across a wrapped row.
3. **The shown and the echoed**: `shown_code` on the reply and its wire
   field, the host's colouring of it, and the echoed source line. Unit tests:
   the session marks `%type` as code; the echoed line's bytes with colour
   forced on.

## 4. Oracle

IPython 8 (prompt_toolkit lexes the entry and colours it as typed; the
output prompt is plain), ptpython, fish (syntax colour as typed), and JOE 4.6
for the default scheme (madcide's default.theme already matches it). GCC's
`-fdiagnostics-color` for the gate. The test lanes pipe stdin, so the
integration tests see no colour by the gate; colour is checked in unit tests
with the predicate forced (`set_colour_when(always)`).
