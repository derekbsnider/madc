# madcide in mainstream IDEs — VS Code, Neovim, Emacs, CLion

Owner (2026-10-08): Chthonia is the beginner's stepping stone; people will
move on to a mainstream IDE — VS Code, Neovim, Emacs or CLion — so each gets a
fully functional plugin that interfaces it with madc. Not scheduled yet; this
plan orders the work.

**Prerequisite: madcide in its own repository** (`2026-10-08-madcide-own-repo.md`,
owner 2026-10-08). That migration completes first; every slice below starts
after it, in the madcide repository, and the plugins release from there. The
existing VS Code extension moves with madcide and is rebuilt on this plan
there, not in the madc repository.

## Principles

- **One server: `madcide`, headless.** Every plugin starts the `madcide`
  binary (`madcide --lsp`, `madcide --mcp`), never `madc` and never a `.mad`
  script (owner 2026-10-08). The madc a user gets is the one madcide carries.
- **A plugin is a thin client.** Every answer — diagnostics, colour, symbols,
  completion, the REPL, commands — comes from madcide's compiler and command
  registry. No plugin carries a grammar, a command list or a language model of
  its own (the VS Code extension's existing rule, now everyone's).
- **Standard protocol first.** Whatever LSP already defines is served as LSP,
  so each editor's built-in client does the work. madc-specific features (the
  REPL, the session's other faces) are a small `$/madc/...` extension, the same
  for every plugin, specified once in madcide.
- **One session, many faces.** `--attach` lets an editor, an agent's MCP
  client and the madcide window share one session's buffers; every plugin
  offers it.
- **Coexistence with C/C++ tooling.** `.mad` files are madcide's. For `.c` /
  `.cpp`, madcide is opt-in per workspace, so it never fights clangd or
  Microsoft's C/C++ extension unasked.

## The plugin binary contract

- One setting per plugin, the madcide path. Empty = `madcide` on PATH, where
  the packages install it (`/usr/bin/madcide`, `bin/madcide`,
  `bin\madcide.exe`). A source checkout builds its own `madcide` and points
  the setting at it.
- Launch: `madcide --lsp` (LSP over stdio), `madcide --mcp --attach auto`
  (MCP for the editor's agent, on the same session), `--serve 127.0.0.1:0`
  when the plugin offers the madcide window. Loopback only (no auth).
- The plugin states the minimum madcide version it needs; madcide reports its
  version in `initialize` (`serverInfo`), and a plugin refuses an older one
  loudly.

## Server work (madcide, once, for every plugin)

What madcide's LSP serves today (`madcide_lsp.inc`): the document lifecycle
(open / change incremental / save / close), `documentSymbol`, `hover`,
`definition`, `references`, `semanticTokens/full`, `executeCommand` (the
registry's commands). MCP: 45 tools (graph.*, nexus.*, test.*).

| # | Server slice | Why |
|---|---|---|
| L0 | **Start without a file:** `madcide --lsp` takes its root from `initialize` (`workspaceFolders` / `rootUri`) and opens documents as the client opens them | every editor starts a server per workspace, not per file; today's start file is the VS Code extension's workaround |
| L1 | `completion` (+ `completionItem/resolve`) and `signatureHelp` from the parse handle — the REPL's completion (`replcomplete`) is the existing owner to share | the first thing a mainstream user expects |
| L2 | `workspace/symbol`, `documentHighlight`, `typeDefinition`, `implementation` from the code graph (graph.search / graph.references / graph.bases) | Ctrl+T, highlight-on-cursor, the class navigation |
| L3 | `rename` + `prepareRename` through the graph's validated edits (graph.replace's owner) | refactoring that the compiler has checked |
| L4 | `semanticTokens/range` and `/full/delta`; `inlayHint` (deduced `auto` / `var` types) | large files; madc's deduced types shown inline |
| L5 | `codeAction` for diagnostics that carry a fix; `formatting` when madc has a formatter (none yet — a named later seat, not a blocker) | quick fixes |
| L6 | **The REPL channel** (`$/madc/repl/*`): open / offer an entry / complete / send input (the program's stdin) / send EOF / interrupt / close, and notifications for the transcript (output, shown value, diagnostics, the prompt). The session's REPL engine (`madc::session_*`, the owner `madcide_repl.inc` already drives) — not a second REPL | the REPL every plugin shows, with madcide's behaviour (continuation lines, completion, a crash restarts, input is the running program's stdin) |
| L7 | **Run** = the REPL's `%run` of the buffer (F5's semantics), and `test.*` (discover / run / results) as LSP commands | run and test from the editor |
| L8 | The test runner and the Problems list stay standard (diagnostics); debugging (a DAP server over the JIT stepper) is a LATER arc, named here so a plugin leaves its seat | completes "fully functional" |

Each slice: tests in madcide's suite driving the protocol (the existing
`test/protocol_probe.js` shape, and `.mad` tests for the server side).

## The plugins

Each plugin: the binary contract above, the editor's own LSP client, a REPL
view on L6, a Symbols view, commands from `executeCommand`, attach.

**VS Code** (`tools/vscode-madcide` today, JS, vscode-languageclient):
1. Spawn `madcide` (the one `madcide.path` setting replaces `madcPath` +
   `madcidePath`); start from the workspace (L0).
2. REPL: a terminal-style panel (`vscode.window.createTerminal` with an
   extension-owned pseudoterminal) over L6; "Run in REPL" = L7.
3. Symbols: a tree view in the explorer (`contributes.views`) fed by the
   document's symbols and L2's workspace symbols; Outline keeps working.
4. MCP: register `madcide --mcp --attach auto` with VS Code's MCP server
   definition API, so VS Code's agents use the session the editor shows.
5. Tests: the Testing API over L7's `test.*`.
6. Ship on the Marketplace and Open VSX (publishing is the owner's call).

**Neovim** (Lua, 0.11+ built-in `vim.lsp`):
1. `vim.lsp.config('madcide', { cmd = { 'madcide', '--lsp' }, ... })` +
   filetype detection for `.mad`; no dependency on nvim-lspconfig (a recipe
   for it as well).
2. REPL: a terminal-style buffer in a split over L6 (`nvim_open_term` fed by
   the transcript notifications; input sent on Enter).
3. Symbols: a side window over documentSymbol / workspace/symbol; the
   built-in pickers and Telescope work through standard LSP.
4. Commands: `:Madcide <command>` from the server's command list.

**Emacs** (Elisp, 29+ built-in `eglot`; an `lsp-mode` recipe beside it):
1. A `madc-mode` major mode for `.mad` (syntax table only — colour from
   semantic tokens, eglot's support), `eglot-server-programs` entry.
2. REPL: a `comint`-derived buffer over L6 (`M-x madcide-repl`), with
   `madcide-run-buffer` = L7.
3. Symbols: imenu / xref already read LSP; a side window
   (`madcide-symbols`) over the same data.

**CLion** (Kotlin, IntelliJ Platform):
1. LSP through the platform's LSP API (CLion is a commercial-platform IDE),
   with LSP4IJ as the route for IDEs without it — decided when the slice
   starts, against the then-current platform.
2. REPL: a tool window over L6 (a console view); Run configurations = L7.
3. Symbols: the Structure view over documentSymbol.
4. Ship on the JetBrains Marketplace (owner's call).

## Order

0. The madcide repository split (the prerequisite above) — complete, with
   madcide's CI green on its pinned madc.
1. Server L0 + L1 (start from the workspace; completion) — every plugin
   needs them.
2. VS Code: the binary contract, L6 REPL, Symbols, MCP — the existing
   plugin, finished first; L6 is designed here and reused by every plugin.
3. Server L2–L4, then Neovim and Emacs (both thin: built-in clients).
4. CLion (the heaviest toolchain), then L5, L7–L8 across all four.

Each plugin is verified by DRIVING the real editor (owner rule: a screenshot
is not verification): `@vscode/test-electron` for VS Code, `nvim --headless`
with a Lua test, `emacs --batch` with ERT, the IntelliJ test framework for
CLion — opening a `.mad` file, typing, completing, running the REPL, reading
the symbols, in CI on Linux, macOS and Windows.

## Thread-safety contract

Plugins are separate client processes. The server keeps madcide's contract:
editor state on the session's thread, cooperative tasks, a parse handle
confined to the thread that opened it; L6 adds the REPL channel on that same
thread (the session process it drives is already its own process).

## Open items for the owner

- Publishing accounts and names on the Marketplace / Open VSX / JetBrains
  Marketplace (outward-facing — the owner's call at release).
- Whether `.c` / `.cpp` support is offered by default in Neovim and Emacs
  (where no dominant C/C++ plugin is assumed) or opt-in everywhere
  (default chosen: opt-in everywhere).
