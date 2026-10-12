# vised — the example visual editor

A small editor on madc's public surface only: `ui::` for the screen (the
terminal grid, or `--web` for a window), the document component (`ui::doc_*`:
the file, read-only, modified, undo) with an anchor for the caret, the edit
notification (`ui::doc_subscribe`), and madc's colon mode
(`madc::ex_session`: `:w`, `:q` and every ex command).

    madc examples/vised/vised.mad FILE [ro] [--web]

Typing edits at the caret; the arrows, Home and End move it; `^Z` undoes;
`^S` saves (`:w`), `^Q` quits (`:q`, refused while the file is modified),
`^X` discards (`:q!`); `Esc` opens the colon line.

`vised_core.inc` holds everything but the loop; `tests/testvised.mad` drives
it without a terminal.
