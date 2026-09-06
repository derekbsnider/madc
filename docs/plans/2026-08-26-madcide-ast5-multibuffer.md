# AST-5 — madcide multi-buffer (IDE-8) and JOE's ^K E

The arc-design slice cut names AST-5 as "IDE-8 project/multi-buffer
(+ `^K E` edit-file)". This doc is the concrete design executed in
session 136 (buffers first; windows are IDE-9e's seat and come later).

## The JOE model being matched

`^K E` prompts "Name of file to edit"; the file opens as a NEW buffer
in the current window (the previous buffer stays open, invisible);
`^K E` to an already-open path returns to that buffer with its caret
where it was; a nonexistent path opens an EMPTY new buffer ("New
File"). No visible buffer ring — the prompt is the switchboard.

## Design

- **The buffer table** lives on the editor-state bag: `buffers` = rows
  `{ doc, path, caret, mark, bend }`, `bufidx` = the active row. The
  DOC-scoped state (path, modified, read_only, phandle) already lives
  on each document object and needs nothing; the table saves only the
  es-scoped interaction state at switch time and restores the
  target's.
- **Switch discipline** (`switch_buffer`): save `{caret, mark, bend}`
  into the active row; flip `bufidx`; restore the target's; recompute
  access (`derive_access` reads the doc); colour immediately from
  `lex_first_spans` (stage-1 machinery) and mark the parse PENDING —
  the loop runs `ensure_phandle` after the next render, exactly the
  startup's paint-before-parse rule, now ONE mechanism (`pending_parse`
  on the bag; run_ide's startup uses it too instead of its inline
  call).
- **Open-or-switch** (`edit_file`): identity is the path STRING as
  typed (canonicalization is a named residue — the engine's
  canonical_path_for_compare is not a script public; JOE matches
  literally too). A row with the same path switches; otherwise
  `setup_document`, and when the file does not exist, a NEW EMPTY
  document with that path + "New File" (setup_document's 0-on-missing
  contract stays untouched — it is shared with vised/lined, where
  refusal is right).
- **Keys**: `^k e` becomes `editfile` (JOE's own meaning — the owner
  north star is JOE parity); the parse/diagnostics refresh (`check`)
  moves to the free `^k ;`. The help pane projects the profile, so the
  move is self-documenting; flagged for owner review in the CHANGELOG.
- **Status**: `%n` already renders the ACTIVE doc's path — nothing to
  add. The one new seat: the switch message names the buffer
  ("Editing tools/x.c" / "New file tmp/y.c").
- **Views**: `^K N` views wrap the ACTIVE buffer (nav_doc already
  keys off the es view state); a switch clears any open view (the view
  belongs to the buffer it was opened on — restored state must not
  point another buffer's render).
- **What this slice does NOT do**: windows (IDE-9e), the cc.json
  project handle wiring into the buffer table (project-open lists TUs;
  a later seat can seed `^K E` completion from `madc::project_tus`),
  per-buffer undo separation IF the undo stack turns out to be
  es-scoped (recon says the text_buffer owns undo per document — doc-
  scoped, so buffers keep their own history for free).

## Gates

- testmadcide grows the flow: `^K E` to a second file (rows grow,
  status %n follows, caret restores on return, modified flag stays
  per-doc), `^K E` to the SAME path (switches, no new row), `^K E` to
  a missing path (New File, empty buffer), and the check-on-`^k ;`
  rebind pin.
- The pty gates stay untouched (single-buffer sessions).
