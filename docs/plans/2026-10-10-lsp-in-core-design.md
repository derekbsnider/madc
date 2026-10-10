# The LSP in madc's core: `madc --lsp` — design (split arc 1C.12)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1C item 12 (owner: "small enough"): `madc --lsp FILE`, a C++ port of
`madcide_lsp.inc` over 1A.1 (documents) and 1C.11 (`madc::lang`); document
sync on madc's documents (a `didOpen` no longer switches an active buffer —
there is none); `executeCommand` through the registry.

## 1. What exists

- **madcide's server** (`tools/madcide/madcide_lsp.inc`, ~1,100 lines of
  dialect). `lsp_handle(S, doc, self_id, req, out, notes)` answers one
  message: a reply in `out`, server-to-client notifications in `notes`.
  - Methods: `initialize`, `initialized`, `shutdown`, `exit`,
    `textDocument/didOpen` / `didChange` (incremental) / `didSave` /
    `didClose`, `documentSymbol`, `definition`, `references`, `hover`,
    `semanticTokens/full` and `workspace/executeCommand`.
  - Ignored silently: `$/cancelRequest`, `$/setTrace`,
    `workspace/didChange*` and `willSave`.
  - Errors: an unknown request answers -32601; a notification sent with an
    id -32600; a URI that is not `file://` -32602; a document with no
    parseable layer -32803; a body that is not JSON -32700.
  - Positions: `positionEncoding` is negotiated (`utf-8` when the client
    offers it, else `utf-16`). `lsp_offset_of` / `lsp_position_of` convert
    through `ui::text_bytecol` / `text_col16`, which are
    `col16_to_byte` / `col16_of_byte` (`include/madcdis/text_utf16.h`).
  - The token legend is keyword, variable, number, string, comment, type,
    function. A class maps onto it by name (ident, parameter, member and
    enumerator to variable; control and directive to keyword; typename and
    namespace to type; include and escape to string).
  - A symbol's kind: function is 12, any other 13.
  - Diagnostics are published as `{range, severity (error 1, warning 2,
    else 3), source: "madc", message}`.
  - `executeCommand` runs madcide's own command table (`api_run`).
  - `didOpen` switches madcide's ACTIVE buffer (`edit_file`); `didClose`
    keeps the buffer.
- **The transport** (`madcide_serve.inc` `run_lsp` / `lsp_stdio_loop`):
  `madc::channel io("stdio://")` and `io.frame_headers()`. Then `readline`
  returns one Content-Length body and `write` frames one message
  (`include/madcdis/channel.h`, `src/madc_header_channel.cpp`). The exit
  code is 0 when `exit` follows `shutdown`, else 1.
- **JSON**: `madc::hub::detail::wt_value_to_json` / `wt_json_to_value`
  (`include/madcdis/world_text.h`, nlohmann) is the repo's one value-to-JSON
  bridge.
- **The answers**: `madc::lang` (1C.11) over a hub world's documents, in
  one coordinate (a 1-based line, a 0-based byte column). Documents:
  `madc::document_open` / `find` (`include/madc_documents.h`) and
  `hub::mutation_context`'s `text_load` / `text_insert` / `text_erase`.
- **The registry** (1A.2): the session's command rows
  (`InteractiveSession`: built-in rows, host rows, module rows from 1B.10),
  run by `submit("%NAME ARG")`. It has no public list of its rows.
- **The CLI** (`src/madc.cpp` `main`): one flag loop. `-i` dispatches to
  `run_repl` AFTER madc.ini is read, so the configuration applies.
- **Clients**: `tools/vscode-madcide` launches `madc --no-config
  madcide.mad FILE --lsp`; it moves to `madc --lsp` at 1D.19. L0 of the IDE
  plugins plan: a server starts per workspace (its root comes from
  `initialize`) and opens documents as the client opens them.

## 2. Decisions

1. **The owner**: `include/madc_lsp.h` + `src/madc_lsp.cpp`, the class
   `madc::lsp::server`, C++ first. `handle(message, reply, notes)` answers
   one decoded message and knows no transport, so a unit test drives it in
   process (as `testmadcide_lsp` drives `lsp_handle`). `serve(channel)` is
   the loop: read one framed body, decode it through the one JSON bridge,
   handle it, write the reply and then each note; it returns the exit code.
2. **The lifecycle** is the specification's:
   - before `initialize`, a request answers -32002 (server not
     initialized) and a notification is dropped, except `exit`;
   - `initialize` negotiates the position encoding and takes the root
     (`workspaceFolders`, else `rootUri`; L0) and the client's capabilities;
   - after `shutdown`, a request answers -32600;
   - `exit` ends the loop with 0 after `shutdown`, else 1.

   The server is named `madc`, with madc's version.
3. **Documents are madc's, in the server's own world**. A URI names a
   document by its canonical path (the one URI rule lives in the server: a
   `file://` URI, percent-decoded, with a Windows drive). The client's text
   is the truth while the document is open.
   - `didOpen`: the document is found or created, and the client's text is
     loaded. No document becomes "active".
   - `didChange`: incremental ranges are applied in order (`text_erase` +
     `text_insert` through the encoding owner); a change with no range is a
     `text_load`.
   - `didSave` publishes.
   - `didClose` closes the document, its parse handle with it, and
     publishes an empty list (clangd's behaviour: the client's Problems for
     a closed file clear).
   - Diagnostics are published after `didOpen`, `didChange` and `didSave`.
4. **The answers are `madc::lang`'s.** The server only converts:
   - Positions go through ONE owner pair (an LSP position to `lang::point`,
     and back) over the line's text: `col16_to_byte` / `col16_of_byte` under
     UTF-16, identity under UTF-8.
   - documentSymbol: DocumentSymbol objects (`range` = the extent,
     `selectionRange` = the name). `lang::symbol_kind` maps to an LSP
     SymbolKind by a switch.
   - definition and references: Location lists.
   - hover: plaintext `contents`, with `range` when `lang` gives one, else
     null.
   - semanticTokens/full: `lang::spans`, delta-encoded, the columns and
     lengths in the negotiated encoding. The legend is madcide's seven
     types, mapped from `HighlightClass` by an enum switch (the class names
     never pass through text).
   - A document the compiler does not parse answers -32803, as madcide's
     does.
5. **`executeCommand` goes through the registry.** The server runs commands
   in a REPL session its host gives it (`lsp::command_host`). The CLI's
   session is the crash-isolated backend (as `madc -i`'s is) with its
   program's output PIPED, never the protocol's stdout, and it restarts
   after a crash or a `%quit`.
   - The server's documents answer the session's buffer question (1B.8),
     so `%check` / `%build` / `%run` see unsaved text. A command with no
     FILE has no current buffer to read (there is none) and says so.
   - The advertised commands (`executeCommandProvider.commands`) are the
     registry's `%` names, as `madc.NAME`. They come from the session's
     completion of `%`: the built-ins, the aliases and the module rows.
   - `madc.NAME` with `arguments[0]` (a path or a `file://` URI) runs
     `%NAME ARG`. The result is `{ok, text, output, diagnostics}`: what the
     command showed, what its program printed, and its rendered
     diagnostics.
   - The editor a command asks for (`%open`, `%edit`) is the client's. A
     session's host may give it an editor, as it gives it buffers; the
     LSP's sends `window/showDocument` (LSP 3.16) for the file at the line
     instead of starting a terminal editor.
6. **The CLI**: `madc --lsp [FILE]`, dispatched after madc.ini like `-i`.
   The configured Program is the active runtime Program for the server's
   life, so every document's parse is its child and searches its include
   directories (`-I`, madc.ini's `include`) as a compile does; a document's
   standard is its kind's. A FILE is opened as a document at start, so its
   first `didOpen` finds it parsed. `--lsp` with `-i` or an artifact
   request is refused.
7. **Thread-safety** (C++ standard library convention): one server, on one
   thread, per process; its world and documents are its own; parse handles
   keep the runtime-eval confinement; the session is the server's.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **The server**: the lifecycle, document sync, the reads, publishing and
   the errors. Unit tests through `handle`: madcide's `testmadcide_lsp`
   assertions where they apply, UTF-16 against UTF-8 on a line with a
   non-ASCII character, and each refusal.
2. **The transport and the CLI**: `serve`, `madc --lsp [FILE]` with the
   configured Program active. An integration test pipes a Content-Length
   framed conversation (`.flags` `--lsp`, a CRLF `.input`) and checks the
   framed replies and the exit code. `docs/man/madc.1` gains `--lsp`.
3. **`executeCommand`**: `command_host`, the documents as the session's
   buffers, the session's editor (`ReplSession::set_editor`, honored by
   the backend's host), the CLI's piped backend session. Unit and
   integration tests: `madc.check` on unsaved text, `madc.run`'s output
   in the result, `madc.open` as `window/showDocument`.

## 4. Oracle

The LSP 3.17 specification for the lifecycle and the error codes. clangd 18
(on the container) for the answers' shapes and ranges, and for `didClose`
clearing diagnostics. madcide's own `testmadcide_lsp.expect` is the
regression record, except where clangd decides otherwise (definition answers
the name — 1C.11).
