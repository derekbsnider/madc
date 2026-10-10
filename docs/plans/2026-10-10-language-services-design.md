# ONE set of language-service functions over a document — design (split arc 1C.11)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1C item 11 ("diagnostics, outline, definition, references, hover,
semantic spans — mostly the existing `madc::parse_*` / `graph_*`, now taking a
document. The LSP, the MCP tools and madcide's direct calls all read it;
nothing answers twice").

## 1. What exists

- **The handle reads**: the C++ owners `madc::internal_program_parse_*` /
  `internal_program_graph_*` (`src/madc_program.cpp`) over a `parse_tu_state`,
  their extern-C thunks `madc_parse_*` / `madc_graph_*` (`src/parser.cpp`),
  and the dialect faces in `<ns_madc>` (`src/ns_madc.cpp`). Every read takes an
  int64 parse HANDLE. Rows are `madc::value` objects: a diagnostic
  `{severity, severity_code, phase, phase_code, message, file, line, column}`;
  an outline row `{kind: "function", name, line, column, end_line}` (the TU's
  own function definitions); a graph node `{id, kind, name, line, column,
  end_line}`; a span `{line, column, length, class}` (class a NAME).
- **The document's handle**: `madc::doc_parse` / `doc_parse_checked`
  (`src/ns_madc.cpp`, `doc_parse_impl`; document-component design §2.5): ONE
  version compare decides a re-parse; the slot lives on the document
  (`mutation_context::doc_set_parse`). Its only consumer is
  `tests/testdocument.mad`; madcide keeps its own handle (`ensure_phandle`,
  `madcide_core.inc:2041`) until 1D.17.
- **The composition is madcide's dialect** (`tools/madcide/madcide_lsp.inc`):
  - documentSymbol: `parse_outline`, then `lsp_name_span` (the selection is
    the NAME: its first whole-word occurrence on the row's line);
  - definition: `lsp_word_at` (the word under the position; a caret resting
    just past a word counts), then `graph_definition`; an unknown word answers
    an empty list;
  - references: `graph_definition`, then `graph_references` over the first
    node; each use site's range is the name's LAST whole-word occurrence
    before the site's column (a Call node anchors at its argument list:
    `twice(twice(1))` resolves the inner name); the declaration when asked;
  - hover: `graph_definition`'s first node as `KIND NAME — defined on line N`
    over the word's range, else `parse_enclosing` as `in NAME`, else nothing;
  - semanticTokens: `parse_spans`;
  - diagnostics: `parse_check`, then `lsp_diag_range` (the word STARTING at
    the 1-based column, else the one character there).

  madcide's status line asks `parse_enclosing` (`madcide_core.inc:10198`),
  its Problems `parse_check`, its colour `parse_spans` then `lex_spans`. The
  MCP `graph.*` tools map 1:1 onto `graph_*` over the served document's handle
  (`madcide_mcp.inc` `graph_call`).
- **Coordinates disagree**: a diagnostic's column and a token's stamp (an
  outline row's, a graph node's, `parse_enclosing`'s argument) are 1-based; a
  span's column and `graph_at`'s are 0-based bytes. `parse_enclosing` is
  called both ways: the LSP passes 1-based, the status line 0-based (so a
  caret on a definition's first byte is outside it).
- **The word rule**: `hub::text_buffer::word_left_in` / `word_right_in`
  (`include/madcdis/text_buffer.h`), the ONE word rule; madcide's
  `lsp_word_span` composes them in dialect.

## 2. Decisions

1. **The owner**: `include/madc_lang.h` + `src/madc_lang.cpp`, namespace
   `madc::lang`, C++ first (`cpp-first-api.md`). It composes the handle reads
   and adds no analysis the compiler does not already answer: it is the
   presentation arithmetic (the word under a point, a name's range, a
   diagnostic's range, the hover text) that each consumer wrote on its own,
   now in one place.
2. **The subject is a document**: `(hub::world &, hub::entity_id)`. Its
   handle comes through ONE door, `lang::document_handle(world, e,
   checked_diags)`: `doc_parse_impl` moves here, and `madc::doc_parse` /
   `doc_parse_checked` become faces over it. The text is the buffer's LIVE
   text, unsaved edits included; its version decides a re-parse. Handles stay
   the substrate: the MCP `graph.*` verbs, the project handles and the
   revision (tagged) handles keep their handle reads.
3. **One coordinate**: `lang::point {line, column}`, the line 1-based and the
   column a 0-based BYTE offset in the line (the buffer's unit, the span
   convention); `lang::range {from, to}`, half-open. Every service answers in
   it, and takes its position in it. The compiler's 1-based stamps convert
   in the service, once. The LSP converts bytes to the negotiated encoding at
   its own boundary (1C.12), as `lsp_offset_of` does today.
4. **The functions**, each over a document:
   - `diagnostics(doc)`: `{range, severity, phase, message}`, the severity
     and phase the existing enums (`madc::diag_severity` / `diag_phase`). The
     range is the word starting at the diagnostic's column, else the one byte
     there; a row without a line answers 1:0.
   - `outline(doc)`: `{name, kind, extent, selection}`. The kind is an enum
     (`lang::symbol_kind`; `function` today: Phase 0's Outline kinds widen the
     row builder, not this face). The extent covers line..end_line as whole
     lines; the selection is the name (its first whole-word occurrence on its
     line, else the line).
   - `enclosing(doc, point)`: the innermost definition containing the point,
     as an outline symbol (the status line's question, `enclosing_func_at`),
     or none.
   - `definition(doc, point)`: the word at the point, by the buffer's word
     rule (a point just past a word counts), then `graph_definition`; each
     node's extent. An unknown word answers an empty list.
   - `references(doc, point, include_declaration)`: the definition's first
     node, then `graph_references`; each site's range is the name found
     before the site's column (the last whole-word occurrence); the
     declaration is appended when asked.
   - `hover(doc, point)`: `KIND NAME — defined on line N` over the word's
     range, else `in NAME` with no range, else none.
   - `spans(doc)`: `parse_spans` as `highlight_span`s, each class converted
     ONCE to `HighlightClass` (the conversion `lex_classes` already does,
     made one helper for both).

   Answers are within the document asked about: one handle is one TU. A
   cross-document answer (a project's TUs) is a later seat, not this item.
5. **The dialect face** (`<ns_madc>`, mangled-direct): `madc::doc_diagnostics
   (out, w, e)`, `doc_outline(out, w, e)`, `doc_enclosing(out, w, e, line,
   column)`, `doc_definition(out, w, e, line, column)`, `doc_references(out,
   w, e, line, column, include_declaration)`, `doc_hover(out, w, e, line,
   column)`, `doc_spans(out, w, e)`. Each row carries `{line, column,
   end_line, end_column}` in the one coordinate. Each enum is given as its
   CODE (`severity_code`, `kind_code`, `class_code`), with its name beside it
   for display, as diagnostic rows already do.
6. **The consumers; nothing answers twice**:
   - the C++ LSP (1C.12) calls `madc::lang` directly;
   - the MCP code-graph tools (1C.14) take their handle from
     `lang::document_handle`;
   - madcide (1D.17 / 1D.19): `lsp_document_symbol`, `lsp_definition`,
     `lsp_references`, `lsp_hover`, `lsp_semantic_tokens` and `lsp_publish`
     go when `madcide_lsp.inc` is deleted; the status line, Problems and
     colour call `doc_enclosing`, `doc_diagnostics` and `doc_spans`.
     `lsp_word_at`, `lsp_name_span` and `lsp_diag_range` are the second
     implementation until then: the deletion deadline is 1D.19.
7. **Thread-safety** (C++ standard library convention): a document's handle
   keeps the runtime-eval confinement (used from the thread and Program that
   opened it); the parse slot is written through the world's
   `mutation_context`; every function is otherwise a read, with no new state.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **The owner and the reads with no word**: `madc_lang.h/.cpp`, the
   coordinate, `document_handle` (`doc_parse` moves), `diagnostics`,
   `outline`, `enclosing`, `spans` (one class conversion with
   `lex_classes`). Unit tests over a world's document: a diagnostic's range,
   an outline's selection, enclosing at a definition's first byte, spans as
   enums, a re-parse only when the version moves.
2. **The word-addressed reads**: `definition`, `references`, `hover`. Unit
   tests: a point inside and just past a word; nested calls; the declaration
   on request; both hover forms; an unknown word.
3. **The dialect face**: the seven `doc_*` functions in `<ns_madc>`, and an
   integration test that opens a document through `ui::doc_*`, edits it, and
   asks each one.

## 4. Oracle

clangd over the same C file (LSP `textDocument/definition`, `references`,
`documentSymbol` — `selectionRange` is the name — and `hover` ranges, in
UTF-8 offsets with `positionEncoding: utf-8`). gcc for a diagnostic's line and
column. madcide's current LSP answers (`testmadcide_lsp*`) are the regression
record the service must reproduce.
