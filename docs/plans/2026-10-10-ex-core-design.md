# The ex core — design (split arc Phase 1B.6)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1B item 6. Rule source: D24 (`madc-repl-thonny-plan-2026-09-24.md`)
and its Layering bullet — madc is ex, madcide is the visual part.

## 1. What exists

- **Documents** (1A.1, `include/madcdis/document.h`, `hub.h`): a hub world
  holds documents (path, kind, read-only, saved version, anchors, parse slot)
  beside their `text_buffer` (piece table, lines, literal `find`, checkpoint /
  undo / redo). Every `world::text_*` mutator publishes one `text_change`;
  anchors shift on it. `doc_checkpoint` / `doc_undo` / `doc_redo` carry the
  anchors and the clean state through history. The disk I/O (`doc_open`,
  `doc_load_file`, `doc_save`) is written ONCE, as `ui::` publics over a
  `ui_session` (`src/ns_ui.cpp:1576-1660`) — nothing below can reach it.
- **The registry and the modes** (1A.2): `InteractiveSession::command_of`
  answers what an entry is, its mode included; `:` is colon mode, the HOST's,
  and the session refuses it (`src/madc_session.cpp:644`).
- **The one regex engine** (1A.3): `madc::regex` (`include/madc_regex.h`).
- **The hosts**: `madc_repl_edit` (the line editor on a terminal) and
  `madc_repl_run` (cooked lines: a pipe, a transcript), `src/madc_repl.cpp`.
  Neither holds a document; a `:` entry reaches the session and is refused.
- **madcide**: `colon_command` (`tools/madcide/madcide_core.inc:7733`), 15
  string-compared forms; lined's `.madv` verbs (`tools/texteditor/verbs/`)
  call `ui::text_*` with bare line numbers. The per-language editing tables
  (`madcide_lang.inc:71-218`: comment opener, block delimiters, default
  indent, indent detection) are dialect code.

## 2. Decisions

1. **One owner, in libmadc: `madc::ex`** (`include/madc_ex.h`,
   `src/madc_ex.cpp`). It edits a hub world's documents through
   `mutation_context` — the one mutation path, so an ex edit publishes,
   shifts anchors and groups undo exactly as madcide's edits do. It is not
   in `madcdis` (the substrate stores; it names no regex, no file I/O) and
   not in the session (colon mode is the host's, in the process that edits).
2. **Document files get one owner below the publics**:
   `madc::document_open` / `document_reload` / `document_save` (beside the
   ex core, `src/madc_documents.cpp`, `include/madc_documents.h`) over a
   `world &`. `ui::doc_open` / `doc_load_file` / `doc_save` delegate to them
   (Rule #4: the ex core's `:e` / `:w` / `:r` and madcide's File menu read
   and write through ONE path).
3. **`madc::ex::session`** — per host, confined to the world's thread:
   the ring's current and alternate buffer (entities), each buffer's current
   line as a document ANCHOR (left gravity — `.` follows edits for free),
   marks `'a`–`'z` as anchors too, the last search pattern, the last
   substitute (pattern, replacement, flags — for `&` / `&&`), and the
   options (`number`, `tabstop`, `shiftwidth`, `expandtab`; per session, the
   per-language default indent applied when a buffer opens), and the
   session's DIRECTORY: `:cd` / `:pwd` are the session's, relative names in
   `:e :r :w :sav` and the shell commands resolve against it, and the
   process's own directory never changes (no global state; vim's `:lcd`
   shape). The buffer list is the world's documents — every one, madcide's
   included — numbered for `:ls` / `:b N` in the order first seen; a
   document stays loaded when another becomes current (vim's `hidden`).
   Messages are vim's: `"name" 3L, 14B`, `"name" [New] 2L, 4B written`,
   `... appended`, `:ls`'s rows. One divergence: vim's `:sav` keeps a
   buffer for the old name as the alternate; madc renames the one buffer.
4. **Grammar — vim's ex, line-oriented**:
   - addresses `N`, `.`, `$`, `/re/`, `?re?`, `'x`, `\/` `\?` (last search),
     each with `+N` / `-N` offsets (a bare `+` / `-` is 1); `%` = `1,$`;
     ranges with `,` and `;` (`;` makes the first the current line);
   - a command name resolves ONCE to an enum through one table of
     `{full name, shortest abbreviation, code}` rows — vim's minimal
     abbreviations (`s[ubstitute]`, `d[elete]`, `co[py]`/`t`, `m[ove]`,
     `sor[t]`, `sav[eas]`, `up[date]`, `bn[ext]`, `bp[revious]`/`bN[ext]`);
     a name no row covers is refused (`not an editor command: 'xyz'`),
     never guessed;
   - `!` after the name is the force flag (`q!`, `e!`, `w!`); `:!cmd`,
     `:r !cmd`, `:w !cmd` are the shell forms.
5. **The command set** is D24's list, nothing more: `p`, `nu`/`#`, `l`, `a`,
   `i`, `c`, `d`, `j`, `=`, `r`, `w`, `e`, `q`, `wq`, `x`, `up`, `sav`, `s`,
   `&`, `&&`, `g`, `v`, `m`, `t`/`co`, `>`, `<`, `u`, `red`, `sor`, `ret`,
   `cd`, `pwd`, `ma`/`k`, `marks`, `se`, `ls`/`buffers`/`files`, `b N`, `bn`,
   `bp`, `h`/`help`, `!cmd`, `r !cmd`, `w !cmd`, and a bare address (`:N`,
   `:$`, `:/re/`) moving the current line. No visual or normal mode.
6. **Regular expressions are PCRE2's modern syntax** (D24: unescaped
   `+ ? | ( )`), through `madc::regex` (`each_groups` gives a match's capture
   groups). A replacement reads `&` (the match), `\0`–`\9`, `\n` and `\r`
   (a line break: the line splits — sed's and vim's spellings), `\t` a tab,
   `\&` / `\\` literal. `s` flags: `g`, `i` / `I`, `n` (count only), `e` (no
   match is no error), a leading `&` (the last flags); `:&` repeats the last
   `:s` without its flags and remembers none, `:&&` with them (vim's rule).
   `c` (confirm) needs normal mode — refused. An empty pattern is the last
   one used, the range's own search included. `:g`'s marks follow the
   commands' edits as vim's line marks do (a deleted or joined-away line is
   not run); `a i c u red` are refused under `:g`. One divergence from vim:
   `:g/re/s/x//n` reports "N matches", as `:s n` does — vim's `:g` calls them
   substitutions though none was made.
7. **One ex command = one undo group** (the document owns the groups, the
   command layer sets the boundaries — 1A.1's rule): the core checkpoints
   before a command that edits; `:g` is ONE group for every command it runs;
   `u` / `red` are `doc_undo` / `doc_redo`.
8. **Input mode** (`a`, `i`, `c`): the command answers "text wanted"; the
   host feeds lines until a line holding only `.`, then the core inserts
   them in one group. ed's rule; vim's ex mode alike.
9. **Output and errors are values, not prints**: a command returns its
   shown lines and, refused, one error text; the host prints them (stdout,
   and stderr for the error, as the session's refusals). Nothing in the core
   writes to a stream — madcide's pane and the plain prompt show the same
   text.
10. **The shell forms run through `madc::Process`** (the one spawn owner):
    `/bin/sh -c` on POSIX, `cmd /c` on Windows, in the session's directory;
    `%` in the command is the current buffer's name (vim's expansion, the
    one useful one), `\%` a `%`. The output is stdout then stderr; a
    nonzero status adds `shell returned N` (vim's). A path a command names
    joins the session's directory through `host_path_join` (the one
    relative-path owner, beside `host_path_absolute`); existence and
    directory tests are `host_path_type_of`.
11. **The hosts** (`madc -i`, both loops): an entry `command_of` calls
    colon mode goes to the host's `ex::session`, never to the session.
    - The host starts with ONE buffer, as vim does: `madc -i FILE` opens
      FILE (and still runs it first, D20 / D25 — `python -i`); a bare
      `madc -i` has an empty untitled buffer.
    - D24's aliases: `:q` ends the REPL (refused while a buffer is modified:
      `no write since last change (add ! to override)`; `:q!` ends it
      anyway); `:h` / `:help` is `?`.
    - Input mode in the line editor: a continuation prompt; in cooked input:
      plain lines.
12. **The per-language editing tables move into madc** beside
    `src/file_kinds.cpp` — `src/file_kind_editing.cpp`, declared in
    `include/madc_file_kinds.h`: the line-comment opener, the block
    delimiters, the default indent and `indent_detect`, by file kind (enum
    codes, not names). A buffer opens with the unit its first lines show,
    else its kind's default (`shiftwidth` / `expandtab` — `>` / `<` read
    them); madcide's `madcide_lang.inc` copies move onto these publics in 1D
    (item 18) and are deleted there.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **Addresses, ranges and the command table** — the parser alone, unit
   tested against vim's answers (`tests/unit/test_ex.cpp`).
2. **The ed core on a world** — `p nu l = a i c d j m t > < u red`, marks,
   the current line; unit tests over a world.
3. **Search and substitute** — `/re/`, `?re?`, `s`, `&`, `&&`, `g`, `v`,
   `sor`, `ret`.
4. **Files and buffers** — `madc::document_*` and the `ui::doc_*`
   delegation (its first consumers are these commands); `e`, `r`, `w`, `wq`,
   `x`, `up`, `sav`, `q`, `ls`, `b`, `bn`, `bp`, `cd`, `pwd`, `se`, the shell
   forms.
5. **The hosts** — `madc -i` colon mode in both loops, `madc -i FILE` as the
   current buffer, `:q` / `:h`; transcript tests (`tests/testex_*.mad` +
   `.input`).
6. **The editing tables** into madc; `>` / `<` / `:set` defaults read them.

## 4. Oracle

vim's ex mode (`vim -e -s`, `ex`) on the same buffer and command is the
reference for every address, range and edit; GNU ed for `a` / `i` / `c` /
`j` / `=`. A test names the command line it compared.

## 5. Thread-safety contract

An `ex::session` and the world it edits are confined to the host's thread
(the hub's rule); the core holds no global state — the option and regex
state are the session's. `madc::document_*` touch only the world passed in.
