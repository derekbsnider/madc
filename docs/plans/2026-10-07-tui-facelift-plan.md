# TUI facelift — the terminal workbench looks like the window (plan, 2026-10-07)

**Status: PLAN, for the owner's review. No code yet.**

Owner, 2026-10-07: "the TUI mode needs improvement before we'd do another
release... it should be able to greatly resemble the GUI version... maybe take
some cues from NeoVIM and/or Turbo Pascal or something, but right now, it's
really sad". This gates the next feature release; it is the arc ROADMAP row 8.6
already names ("paned views and tabs in TUI mode too — the Turbo-C / RHIDE
shape", `docs/plans/ROADMAP.md:402`).

## 1. Where it stands — measured

Chthonia v0.0.2's Linux tarball, `chthonia hello.cpp --tui` on a 120×36 pty
(`TERM=xterm-256color`, `COLORTERM=truecolor`), the screen read back by a
terminal emulator (pyte):

```
 0|[New ^n] [Open ^o] [Save ^s] [Run f5] [Stop ctrl+f2]
 1|  hello.cpp                             <git branch>* Row    1 Col   1     <- status, inverse
 2|#include <iostream>                                         Symbols     <- sidebar header, inverse
 3|                                                             (empty)
 4|int main() {
 5|    std::cout << "Hello, World!" << std::endl;
 6|    return 0;
 7|}
 …
24| REPL  Problems  Output  Terminal                                       <- panel header, inverse
25|c++17>
```

- Colour: 4304 of 4320 cells default fg/bg. 16 cyan cells (the string and
  the number), 9 bold (`int`, `return`), 105 inverse (the bars). That is the
  whole palette: JOE parity, 8 ANSI colours.
- No menu bar: F10 does nothing; Alt+F arrives as Esc + `f` and types an `f`.
- No frames or dividers, no editor tab strip, no line numbers, no panel
  backgrounds. The status line sits on row 1 (JOE's place), not at the bottom.

Why, region by region (recon of `include/madcdis/tui_model.h`,
`src/ui_term.cpp`, `tools/madcide/`):

| GUI region | TUI today | Cause |
|---|---|---|
| Menu bar | dropped | the root `menu` hint is ignored by the grid; in the window it is the NATIVE menu (`src/madcwebview_chrome.cc`), so nothing exists to port |
| Toolbar | `[Label chord]` text row | `toolbar_line`, `tui_model.h:1605` |
| Editor tab strip | dropped | composed only when `haspanel` (`madcide_core.inc:9400`), and `run_ide` sets `S.panel(!has_term)` (`madcide_client.inc:209`) |
| Editor | text, minimal colour spans, no gutter | `paint_edit`, `tui_model.h:1301` |
| Side panel, bottom panel | bands with an inverse header row | `region:sidebar` / `region:panel` are the only region values read (`tui_model.h:1509,1544`) |
| Status bar | one inverse row, flat text | `status.items{left,right}` is web-only (`web_model.h:836`) |
| Dialogs, palette | inline `list:1` rows | `popup` / `dialog` hints ignored (`madcide_core.inc:2682`) |
| Splits | a blank column between panes | `divide_extents`, `tui_model.h:908` |

Underneath: `ui_style` is fg/bg 0..8 plus attributes (`ui_style.h:35`, "256/
true-colour is a named later seat"); `emit_sgr` emits only 30–37/40–47
(`ui_term.cpp:78`); no box drawing anywhere; no mouse (no 1000/1006 modes, no
pointer events from the grid); the theme's `@gui` colours reach only the web.

## 2. The target

The same workbench as the window, in cells. 100 columns shown; colours below.

```
 File  Edit  Run  Build  View  Tools  Help
  New  Open  Save │ ▶ Run ▾  ■ Stop
 hello.cpp ×                                                │ SYMBOLS  OUTLINE  MARKDOWN PREVIEW
   1  #include <iostream>                                   │ ▌main  int ()
   2                                                        │  name  std::string "Chthonia"
   3  int main() {                                          │  x     int 5
   4      std::cout << "Hello, World!" << std::endl;        │
   5      return 0;                                         │
   6  }                                                     │
   7  █                                                     │
────────────────────────────────────────────────────────────┤
 REPL  PROBLEMS  OUTPUT  TERMINAL                           │
 c++17> %run hello.cpp                                      │
 Hello, World!                                              │
 c++17> int x = 5                                           │
 5                                                          │
 c++17> (x+2) * 10                                          │
 70                                                         │
 c++17> █                                                   │
 hello.cpp                                                            ROW 7  COL 1
```

- **Colours are the window's.** Editor background `--bg` (#1b1d24), text
  `--fg`, chrome surfaces `--sb-bg` (#262b3a), dividers `--sb-line`, the
  accent `--accent` on the active tab and the selected row, the syntax palette
  `--pal-*` — the same tokens `page.css` and the theme's `@gui` lines use, so
  the two faces match exactly in truecolor.
- **Menu bar (Turbo Pascal / Turbo Vision):** row 0 from the root `menu` hint
  (the same ids, titles, enabled flags and per-profile chords the native menu
  gets). F10 or Alt+letter opens it; a dropdown is a framed box with a shadow,
  chords right-aligned, disabled rows dim, submenus to the right; Esc, the
  arrows and Enter drive it.
- **Toolbar:** one row from the root `toolbar` hint, glyphs for the icons the
  window draws (▶ ▾ ■), the Run dropdown reusing the menu's dropdown.
- **Tabs:** the editor's open files above it; the panel and sidebar headers
  uppercase as in the window; the active tab in the accent colour, underlined.
- **Editor:** a line-number gutter (dim), the current line faintly
  highlighted, the theme's syntax colours.
- **Dividers:** box drawing (`─ │ ┤ ┬ ┴ ┼`) where the window draws hairlines;
  the side panel runs the full height beside the editor and the bottom panel,
  as in the window.
- **Status bar:** the bottom row, its own surface; the file name bold on the
  left, `ROW n  COL n` on the right with dim labels (`status.items` segments).
- **Dialogs and the palette (Neovim floating windows):** centred framed boxes
  with a title, a filter line and buttons (`[ OK ]  [ Cancel ]`), painted over
  the workbench with a shadow.
- **Mouse (Neovim `mouse=a`):** click a menu, a tab, a toolbar button, a list
  row, or the text (caret), wheel to scroll; Shift+drag still selects in the
  terminal itself.

Degradation, decided at open and kept per target:

- colour: truecolor (`COLORTERM=truecolor|24bit`, Windows Terminal's
  `WT_SESSION`) → 256 colours (`TERM=*-256color`, nearest match) → 16 → 8
  (today's look);
- glyphs: Unicode box drawing in a UTF-8 locale, else ASCII (`+ - |`);
- width: below ~80 columns the toolbar folds into the menu and the side panel
  hides (View shows it), as a narrow window would.

### 2a. Syntax colours — VS Code's C/C++ look for Chthonia (owner, 2026-10-07)

Owner: "Chthonia's syntax highlighting seems to be a little lacking... are you
sure it's the same as JOE? I wonder if using VSCode's C/C++ styling would be
better".

It is not JOE's. The one classifier (`madc_token_highlight_class`,
`src/lexer.cpp:10534`; `HighlightClass`, `include/tokens.h:149`) knows
keyword, type, number, string, comment and the parse-tree-backed function.
Chthonia has no theme of its own and uses `profiles/default.theme`, which
colours five of those as JOE does. JOE also colours preprocessor lines, include
paths, escapes and braces; `default.theme`'s own header names those classes as
not yet classified.

Chthonia's look becomes VS Code's Dark+. VS Code colours C/C++ in two layers:
TextMate scopes, and the C/C++ extension's semantic tokens on top. madc's parse
tree can supply both layers:

| Class (new = ★) | Example | Dark+ |
|---|---|---|
| ★ preprocessor directive | `#include`, `#define` | #C586C0 |
| ★ include path | `<iostream>` | #CE9178 |
| ★ control keyword | `return` `if` `for` `while` | #C586C0 |
| keyword / type keyword | `int` `void` `const` | #569CD6 |
| function (parse tree) | `main` | #DCDCAA |
| ★ type / class / namespace name (parse tree) | `std`, `string` | #4EC9B0 |
| ★ variable / parameter / member (parse tree) | `cout`, `name` | #9CDCFE |
| string, char | `"Hello"` | #CE9178 |
| ★ escape | `\n` | #D7BA7D |
| number | `0` | #B5CEA8 |
| comment | `// …` | #6A9955 |
| operators, punctuation | `<<` `;` | #D4D4D4 |

- The classes go into the ONE classifier and its span query. Directives,
  include paths and escapes come from the line text, as comments come from
  trivia. The name kinds come from the parse tree, as `function` already
  does. `HighlightClass` and `highlight_class_name` grow the new names, and a
  theme that does not name a class leaves it plain.
- `chthonia.theme` (the Chthonia repo) carries the Dark+ values. The window
  shows them as they are; the terminal shows them exactly in truecolor (S1)
  and nearest-match below it. VS Code's themes are MIT-licensed.
- madcide's `default.theme` stays JOE's look (owner directive 2026-08-26) and
  gains the JOE classes it lacked (Preproc blue, Escape bold cyan, Brace
  magenta). A `vscode.theme` ships beside it; `^K T` switches.
- Gate: a classification golden for a fixed C and a fixed C++ file (every
  class appears at least once), plus the TUI and DOM goldens that show the
  colours.

## 3. How it fits the architecture

- **One composer, one tree.** The grid learns to show hints the web already
  receives — `region:editor|statusbar`, the editor's `tabs` array, root
  `menu`, toolbar `icon`/`drop`, `status.items`, `popup`/`dialog`, root
  `theme` — under the existing contract "a lower level ignores the hints it
  cannot show". No TUI-only compose path. The one composer change: the
  `haspanel` decision (`S.panel(!has_term)`) becomes the frontend's declared
  capability (it can show tabs, panels, dialogs), not "is it a terminal".
- **One style vocabulary.** `ui_style`'s colour becomes a value (ANSI index,
  256-index or RGB) and the theme becomes the one colour source for both
  faces: the `@gui` tokens and the palette move from `page.css` defaults into
  the theme, which the web and the grid both read.
  `check-one-style-vocabulary.sh` keeps one owner.
- **Enums, not strings.** Chrome element kinds (menubar, dropdown, tabstrip,
  gutter, divider, statusbar, frame, shadow), the colour capability and the
  glyph set are enums in the model.
- **Input owners unchanged.** `key_resolver`, `focus_state` and `ui_apply_keys`
  stay the only input owners. The menu bar's navigation goes into
  `focus_state` (it is the first non-native menu), and mouse hits become the
  same events the window's clicks send. The painted chrome records which
  action each cell belongs to, and a mouse hit looks it up there.
- **Personalities keep their keys.** The look is layout and chrome data. The
  default profile and Chthonia get the workbench above. The `joe` personality
  keeps JOE's look (status on top, no persistent menu bar; F10 still opens the
  menu bar, as in Midnight Commander).
- **Thread safety.** One frontend's model, grid and terminal capabilities
  belong to that frontend, on its client-loop thread. No new globals; the
  capability probe runs once at open and is stored on the target.
- **File size.** `tui_model.h` is 1657 lines. The facelift splits it first:
  grid + diff, layout, paint, chrome (menu bar, dropdowns, dialogs), with
  behaviour unchanged.

## 4. Slices — the seam is the facelift complete (the next release)

| # | Slice | Proves it |
|---|---|---|
| S0 | **Snapshot harness:** pyte-backed golden screens (text, colours, attributes) for madcide and Chthonia at 120×36 and 80×24, driven by keys (open a menu, switch a tab, run the REPL); today's screens recorded first; the `tui_model.h` split (no behaviour change) | goldens pass on today's look; a deliberately broken colour fails them |
| S1 | Colour: the colour value in `ui_style`, 38;2 / 38;5 SGR, capability detection, the theme as the shared colour source; editor syntax colours and surfaces match the window | goldens at each capability; the web's colours unchanged (gui lane) |
| S1b | Syntax classes (§2a): the new classes in the one classifier, `chthonia.theme` (Dark+), the JOE classes in `default.theme`, `vscode.theme` | the classification golden; Chthonia's TUI and window goldens in Dark+ |
| S2 | Frames: dividers and junctions, ASCII fallback, the gutter, the current line | goldens in Unicode and ASCII |
| S3 | Tabs and status bar: the editor tab strip (the `haspanel` capability), styled panel/sidebar headers, `status.items` segments at the bottom | goldens; switching tabs by keys |
| S4 | Menu bar: row 0, dropdowns as overlays (a z-ordered layer painted last, with shadows), F10 / Alt+letter, enabled flags, chords, submenus | drive F10 → File → Save; a disabled row cannot be chosen |
| S5 | Toolbar glyphs and the Run dropdown | drive the Run dropdown |
| S6 | Dialogs and the palette as floating framed windows | drive Open…, the palette filter |
| S7 | Mouse: SGR 1006 on POSIX and Windows VT input; hits through the chrome map | synthetic mouse bytes click a menu, a tab, a row |
| S8 | The Terminal tab keeps its program's colours (`term_screen.h` maps SGR onto the one style; ROADMAP's pending item) | a coloured `ls` in the Terminal tab |
| S9 | Windows: Windows Terminal and conhost, by driving input on the owner's box | `chthonia --tui` from cmd, keys and mouse |

S0 as built (2026-10-08): the screen interpreter is madc's own
`scripts/vtscreen.py` (VT100 + UTF-8 + SGR in every colour depth), shared with
the scroll gate — no `pyte` dependency for the platform lanes to carry. Goldens:
`tests/tui_golden/` (startup, ^T options, F5 into the REPL, at 120×36 and
80×24), checked by `scripts/tui_golden_gate.sh` in fulltest with a negative
control; `--record` re-records a deliberate look change. Chthonia's goldens live
in the Chthonia repository, against its bundle. `tui_model.h` split into
`tui_grid.h` (grid + repaint diff), `tui_keyparse.h` (bytes → keys) and the
model; layout / paint / chrome separate as S2–S6 add the chrome.

S1 as built (2026-10-08): a theme names an exact colour `#rrggbb` (or
`bg_#rrggbb`) in the one spec parser; `ui_style` keeps the value AND its
nearest 8-colour index (by hue — plain RGB distance puts Dark+'s comment green
nearer xterm's yellow), so an index-only renderer is unchanged. The window shows
the value (`fgx-rrggbb`, page.js); a terminal detects its depth once
(`COLORTERM`/`WT_SESSION` → 24-bit, `*256color` → the nearest xterm-256 entry,
else the index). `vscode.theme` carries Dark+ for today's classes; goldens at
16 / 256 / truecolor. The surfaces (editor background, chrome) arrive with the
chrome they paint, S2/S3. Found on the way: a popup pane never took the
keyboard in the terminal (^T Options showed but its rows could not be chosen);
popups now take focus while up (`dialog_hints`), pinned by the
`options-scheme` golden.

S1b as built (2026-10-08), the lexical and type-name classes: `control`
(the classifier's own control-flow set; the parser's statement-start
predicate answers another question) and `typename` (an identifier the parse
read as a user's type, `tfTYPENAME`) from the tokens; `directive`, `include`
and `escape` from the line text, in one pass after the token rows of both
span queries (`source_text_rows`, madc_program.cpp). A directive line's
comments and strings colour there too — the lexer consumes the line, so no
token or trivia carries them. Rows never overlap: an escape splits its
string row, a raw string's backslashes stay text. A theme that does not name
a refined class gives it its parent's spec (`load_theme`: control → keyword,
typename → type, directive → keyword, include and escape → string), so an
older scheme never shows a plain hole. Found on the way, each its own commit
and reducer: a span measured the re-rendered spelling, not the source
(`'\0'` coloured its `;`; now the lex-recorded extent), and a comment before
consumed directive lines coloured a line low (now the lexer's own comment
positions, `Program::_trivia_comments`). Then the parse-tree names
(2026-10-08/09): `variable` / `parameter` / `member` / `enumerator` from the
tree node built from each name (#9CDCFE; enumerator #4FC1FF), and
`namespace` — a definition's, an alias's and a using-directive's name and
every qualifier before `::`, marked by the qualifier owners
(`classify_qualifier_before_scope`, `canonical_nested_namespace`) as
`typename` is marked; a scheme without it shows its nearest named ancestor
(`namespace` → `typename` → `type`), so it takes #4EC9B0 in Dark+. A scoped
enum's name before `::` is its type, not a namespace. Chthonia names
madcide's shipped `vscode` scheme (the bundle's `"theme"`), so it needs no
theme file of its own. S1b is complete.

S2 as built (2026-10-09): the FRAME layer (`madcdis/tui_frame.h`): a divider
is a line whose cells record their arms (up / down / left / right), and one
pass draws each cell's glyph from its arms — so where a panel's top divider
ends on a sidebar's it is `┤`, where a split's divider stands on it `┴`, and
no code picks a junction (S6's dialog borders reuse it). The sidebar and the
panel each give their edge column / row next to the centre to a divider; a
vertical split's blank column is one. The model always paints box drawing;
the terminal spells it in ASCII when its locale is not UTF-8
(`detect_glyph_set` beside `detect_colour_depth`, `ui_glyph_ascii` at
emission). The gutter is layout data: a pane line's `gutter` word
(`default.layout`'s editor pane; Chthonia's too) rides the edit node as a
hint, and the grid numbers the lines in at least three columns and marks the
caret line. The chrome's colours are the scheme's `divider`, `gutter`,
`gutter_current` and `current_line` (Dark+ in `vscode.theme`), handed to the
grid as the root's `chrome` hint and read into an enum (`tui_chrome`) once;
a syntax span keeps the caret line's background. The goldens pin the locale
(`LC_ALL=C.UTF-8`), add the REPL's panel in ASCII and in Dark+; the scroll
gate now expects each row's number with its line.

S3 as built (2026-10-09): the editor's tab strip is a frontend CAPABILITY —
`tabs()` on the ui frontend (`ui::tabs(t)`: the grid and the window draw
strips, line mode does not), recorded as the session's `hastabs` fact — in
place of `haspanel` ("a window"), which still decides where runs and the
shell go (S8). The grid draws a node's `tabs` hint as a strip in the flow (the
editor's open files) through the one strip builder that also heads a pane;
a chrome band's titles are uppercase, as the window's. The status bar's edge
is layout data: a `status bottom` window line (default.layout and the baked
default carry it) docks the status node at the screen's last row, full
width under the bands, drawn from its `items` segments (the left side from
the edge, the right against it; the file name bold, the labels dim); a
layout without the line keeps JOE's status line on top, byte-identical. The
theme's `tab`, `tab_active` and `statusbar` colour them (Dark+: #969696,
bold underlined white, white on #007acc). Goldens: the strip switched by
keys (`^K E second.c`, then `^K E golden.cpp`).

S4 as built (2026-10-09): the menu bar is the focus owner's
(`madcdis/ui_focus.h`): the root `menu` hint — the data the window's native
menu reads — becomes the bar's menus and rows, each title given its hotkey
letter (its first letter or digit no earlier title took, Turbo Vision's lit
letter). The one keys → events loop (`ui_apply_keys`) gives an open bar every
key: left/right between menus, up/down over the selectable rows (a separator
or a disabled row is never lit), Enter or a row's letter chooses — the SAME
`action` event (id + code) a bound chord or the window's menu produces — and
Esc or F10 closes. A key no binding took opens it: F10 the first menu, Alt and
a menu's letter that menu; a profile that binds the key keeps it (Emacs's
M-f). A terminal's Meta prefix reads as Alt: ESC and a printable in one burst
is Alt and that key (`tui_keyparse`), and Alt and a printable writes back as
ESC and the byte. The grid paints the bar and the open dropdown last, over
everything: the bar on row 0, the dropdown a framed box under its title (a
second `tui_frame`, so its separators meet its border as `├ ┤`), each row's
chord from the loaded profile right-aligned, a disabled row dim, a shadow to
the right and below. The layout's `menubar` window line (default.layout and
the baked default) holds the bar on row 0; without it the bar shows only while
open (JOE's rows unchanged). The scheme's `menubar`, `menu`, `menu_selected`,
`menu_hot` and `shadow` colour it (Dark+: #3c3c3c bar, #252526 surface,
#04395e selection, underlined letters). Goldens: F10 (Unicode, ASCII, Dark+),
F10 then `s` saving the file, Alt+E then a disabled Cut's letter (the menu
stays). Submenus wait for submenu data (the menu file has none).

S5 as built (2026-10-09): the toolbar row is the plan's target — `New  Open
Save │ ▶ Run ▾  ■ Stop`: a button with a run, debug, stop, step or breakpoints
icon shows its shape (`▶ ▷ ■ ↷ ↓ ↑ ●`), the file commands their words, a
separator row a divider, a button whose `drop` names a menu ends in `▾`, a
disabled one dim; the chords are the menus' now. The ASCII spelling of every
chrome glyph is one owner, `ui_glyph_ascii` (`ui_box_ascii` grown: box
drawing plus `> # * v ^`). The arrow's `menushow MENU` is the frontend's when it
draws menus: `ui::menus(t)` (the grid) is the session's `hasmenus` fact, and
the command parks a `menuopen` request the client serves with
`ui::menu_open(t, title)` — the grid drops that bar menu under its toolbar
button (the same dropdown, keys and action events as S4); a client that draws
none (a window's native menu, a browser page) or a target that refuses gets the
session's choice list, as before. The scheme's `toolbar` colours the row
(Dark+: the side-bar surface). Goldens: the toolbar in Unicode and ASCII from a
fixture bundle (`tests/tui_golden/plugins/tbgolden`, installed per run in the
harness's config directory and launched with `--profile`); the drop itself is
pinned by the unit cases and `testmadcide_menushow` until S7's mouse clicks it.

Each slice: unit tests in `tests/unit/test_tui_model.cpp`, its goldens,
Tier 1 + Tier 2. The battery runs once, at the seam. A GUI or TUI change is
verified by driving input, never by a screenshot alone.

Prerequisite, already fixed on `fix/win-console-attach-claude`: from cmd on
Windows, `chthonia --tui` refused with "ui: the terminal target needs a
console on stdin/stdout" (`madc::console_attach` reopened stdout write-only;
`GetConsoleMode` needs read access).

## 5. What we borrow

- **Turbo Pascal 7 / Turbo Vision:** the menu bar on row 0 with highlighted
  hotkey letters; F10 and Alt+letter; dropdowns with borders and drop
  shadows; dialogs with buttons; a status line with key hints.
- **Neovim:** truecolor themes (`termguicolors`); a gutter with line numbers;
  tabline and statusline as styled surfaces; floating windows with borders;
  `mouse=a`; diff-based redraw (madc's `tui_diff_plan` already does this).
- **What we don't borrow:** modal editing, and windows dragged around the
  screen (Turbo Vision's desktop). The layout is the window's, and the panes
  are fixed.

## 6. Defaults decided here (the owner may overrule any)

1. The default and Chthonia look is the workbench above. `joe` keeps JOE's
   look, with F10 still opening the menu bar.
2. Truecolor when detected, else 256 colours, else 16 or 8.
3. Unicode box drawing with an ASCII fallback.
4. Mouse on by default; a setting turns it off.
5. The toolbar is shown, as in the window; View hides it, and it folds into
   the menu below ~80 columns.
