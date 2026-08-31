# madcide as API gateway, and the code-server north star

**Owner design discussion, 2026-08-31 (session s146).** This document
captures what was RULED, what was proposed and stands unless vetoed,
and what is explicitly OPEN. It supersedes nothing; it names the arc
the next madcide slices serve.

## The layering (RULED)

- **madc core**: lexer, parser, cir_builder — the MC11 CIR AST — plus
  the compilation/c2mir levers. Exposed as the compiler API. This layer
  substantially exists: `madc::lex_spans`, `parse_spans`,
  `parse_enclosing`, the diagnostics/outline queries, `build_native`,
  retained parse handles ("the running madc IS the compiler",
  owner 2026-08-27).
- **The IDE = an API gateway** over that core plus session semantics
  (buffers, edits, projects, builds). It is not an application; it is a
  service holding sessions.
- **UI clients** speak the gateway API: CLI, TUI, GUI, remote, MCP,
  other users. The existing TUI madcide becomes client #1. The headless
  gates become client #2 — formalized consumers of the same API, not a
  sidestep.

This enables: flexible UIs (CLI vs TUI vs GUI), remote access, MCP
access, and multi-user collaborative access (all owner-named goals).

## State tiers (RULED)

1. **Shared-authoritative** (one truth per session): documents, the
   project, diagnostics, build state. Mutations serialized through the
   hub verbs (the thread-safety law's mutation path).
2. **Shared-presence** (owned by one client, visible to all): caret,
   active highlight/selection, buffer focus, assigned colour. Every
   connected client — an MCP-connected LLM included — gets its own
   cursor and highlight colour. A highlight is a pointing gesture.
3. **Client-private**: prompt drafts, open panes, scroll, and
   EVERYTHING key-shaped. Keys never cross the API; the gateway speaks
   named commands with arguments only.

Byte-anchored presence (every client's caret/mark/highlights) shifts
with every edit through the ONE text-mutation owner — the highlight-
span shift mechanism (s146) generalized to a registry of anchors.

## Concurrency model (RULED direction)

No CRDT/OT: the session process is the single authority; thin clients
send named commands (with arguments) and receive change events, then
re-project. Verbs apply in arrival order. This holds for multi-client
access to ONE session; multi-machine session mirroring would need more
and is out of scope.

Prompts are a CLIENT-side argument collector: a human types the
argument into a prompt; a remote client passes the argument in the
command. The gateway never models prompts.

## Permission tiers (RULED: required)

Because the gateway becomes a headless server (below), clients carry
identity + tier from the first slice, even while day-one auth is
trivial local trust. Proposed tiers (standing, not yet final):
**observe/point** (presence + navigation only) → **edit** →
**project-admin** (add/remove TUs, builds, project settings). An agent
client can be born low and promoted deliberately.

## Keybindings (RULED — repeat owner correction)

Key→action associations are NEVER hard-coded. No exceptions: pane and
modal keys are `@pane`-scoped sections in the keybinding profiles, fed
through the SAME `parse_keys`; baked-in defaults ride the rescue-keys
pattern (an inline DATA profile through the same parser), never a
hardwired dispatch arm. The existing hardwired modal keys (palette
esc/backspace, prompt enter/esc/backspace, confirm y/n) MIGRATE onto
`@palette` / `@prompt` / `@confirm` sections when the mechanism lands —
one dispatch generation, not two. Named residue: navigation inside the
engine's focused-choice widget (arrows/enter in tui_model) — respelling
that from profiles means the scoped tables reaching the engine model;
design when a personality needs it.

**LANDED (s147, feature/madcide-project-claude):** `@scope` lines in
the one `parse_keys` (later lines win — the defaults-merge rule), baked
modal defaults as inline data, and the migration — prompt/confirm/
project/pane arms all dispatch looked-up NAMES. The named residue
stands. `toggle_profile` now cycles the `.keys` files found in the
profile directory (sorted; no name ladder).

**Personality roster (owner, same day: "we should also have ones for
emacs and neovim"):** `emacs.keys` shipped — the honest C-chord subset
(C-x families map directly). Two named ENGINE gaps hold back the rest
of it: no Meta key spelling (an ESC prefix cancels a pending chord) and
no C-SPC/NUL spelling (set-mark). A vim/neovim personality is NOT a
keys file — vim is modal (normal/insert, counts, operator+motion); the
natural substrate is the @scope tables (modes = scopes the MAIN key
stream consults; `i` flips scope) — a dedicated slice, never a flat
profile pretending. *(Owner flagged the missing neovim.keys again
2026-08-31; the modes-as-scopes slice is QUEUED NEXT after the seam —
built on the session surface so the mode routing is written once.)*

**Modes are general + the ^N MODES PALETTE (owner rulings 2026-08-31,
LANDED same day):** "no reason our editor/ide cannot have modes" — any
personality can reach a mode. joe's `^N` (freed from its ^K E shortcut)
raises the modes palette — the one popup-list widget — whose keys are
`@modes` profile data through the one parse_keys (baked defaults:
`:` enters the vi colon line, enter opens the focused row, esc
dismisses). `^N :` is the two-keystroke door into colon mode, and the
palette is the only legal spelling — the chord tables refuse prefix
shadows, so `^n` and `^n :` cannot coexist in the main table. COLON
MODE is the prompt machinery plus ONE interpreter over the existing
vocabulary (`w q q! wq x`, `:N` goto, `e <file>`, `r <file>`, `!cmd`
via the terminal-request seam) — the human face of slice 3's
commands-with-arguments; `:q` refuses a dirty buffer with the q verb's
own message (vim's shape). The vi normal/insert mode joins the palette
as a row when the modes-as-scopes slice lands; its `@normal :` enters
this same line.

## The ^P project window (RULED 2026-08-31)

^P's PRIMARY function is the project (palette quick-open is secondary),
correlating directly with `--project`:

- **A POPUP, never a permanent window (owner ruling 2026-08-31)**: ^P
  raises it, esc/^P dismisses it — the existing pane model. The TUI has
  no space for extra windows hanging around; Turbo C's persistent tiled
  project window is NOT the model here, only its membership semantics.

- **Manifest**: a madc-native project JSON (top-level OBJECT mirroring
  `ProjectManifest`: tus + entry + output + a future commands section).
  `--project` accepts both shapes — object = native, array =
  compile_commands.json import, which stays READ-ONLY interop (never
  clobber a generated file). madcide writes only the native shape.
- **Implicit single-file project**: a session on a lone file IS a
  one-TU project with no file on disk. The manifest materializes when
  the second file is added. Standing defaults (owner veto welcome):
  created beside the launch file as `<base>.prj.json`, announced on the
  status line; add/remove persist immediately (no dirty-manifest state).
- **Grouped rows**: `[project]` TUs (manifest order) → `[open]` buffers
  not in the project (dirty markers) → `[cwd]` matches while filtering.
- **Membership verbs**: `projadd` (argument-taking; the prompt collects
  it for humans), `projdel`, `projaddcur`. Spellings are profile data:
  `@project ins projadd / del projdel / enter projopen` ship in
  joe.keys+pico.keys as DATA; `^k j projaddcur` proposed for joe.keys.
- Removing a project item never touches the file. Opening files stays
  where it is (^K E / palette); new files land in `[open]`.
- ^B correlates: with a manifest open, Build = the `--project` build;
  the manifest's commands section (already named in the ide-controls
  plan) feeds the ^B rows.

**LANDED (s147, feature/madcide-project-claude):** the window
(grouped rows; [cwd] joins only while filtering), the popup semantics,
the implicit project + materialization on the second file
(`<base>.prj.json`, immediate persistence, announce), startup
read-back, projadd/projdel/projaddcur (+ `^k j` in joe.keys),
`@project` keys as data in all three profiles, the native OBJECT
manifest in `--project` (read_project_manifest routes object=native /
array=cc.json import), js::parse + real js::stringify over the one
JSON bridge, and the engine's focused-selection-on-key-events
(ins/del act on the focused row). **The ^B correlation landed in the
same arc (owner: before the merge wave):** with a manifest open the
rows go project-wide — Check project (a project handle's FILE-tagged
per-TU rows; the diags pane leads foreign-file rows with the file, and
goto OPENS it), Build project (`madc::project_build` — the --project
AOT lane in-process; output = the manifest's `output`, else its base),
Run project (verdict first, then `madc::project_run` — the --project
JIT lane in a fork child; win = a child of self) — and the manifest's
`commands` section APPENDS its rows as data ({project} joined
build_subst's vocabulary). An implicit project keeps the single-buffer
rows. Named residues: no cancellation chain into the project emit lane
(Stop lands pre-start only); the frozen-project twin of --run-frozen;
genuine-Win validation of the child-of-self run arm.

Deferred (unchanged): fuzzy filesystem walk; command palette; MRU
ordering + dirty/diag markers ride this arc's compose work.

## North star: the headless code server (OWNER VISION)

The gateway matures into a headless local server ("like a webserver or
database server — a sort of github, but a local instance") providing
ONE API point over three time axes of a piece of code:

- **Present**: compiler truth — what it depends on, what depends on it,
  inputs/outputs, data structures (the MC11 tree's retained
  tokens/positions/subtrees make entities REAL objects, not line-range
  guesses).
- **Past**: git history, issue history.
- **Future**: plans, project tracking.

**Nexus, not replacement (RULED)**: madc never replaces git/Jira — it
is the nexus where the axes meet, because different teams use different
tools. The nexus owns ONLY: stable entity identity (API speaks entity
handles, never raw byte offsets, from day one), the link records
(entity ↔ commits / tracker keys / doc anchors / plans), and cached
projections of external state. External tools remain the truth of
their axis.

**Component homes (RULED)**: **madcdis is the syncing nexus** — the
hub/projection/snapshot/source-adapter machinery already lives there;
sessions, presence, entity identity, links, and sync belong to it.
**madcdat is the push/pull between external systems** — the storage
backends live there; per-tool adapters (git first: local, read-mostly,
doubling as the past axis) implement one adapter contract.

Named hard problem (open, design-for, don't-block): entity identity
ACROSS history (rename/move survival) — structural knowledge +
persisted parse state can beat line-oriented blame; the entity-handle
API keeps the door open.

## LSP — an adapter at the edge, not the native protocol

(owner: "see how LSP may or may not fit into this")

- **Fits as a transport adapter**: an LSP endpoint over the same
  gateway queries makes every LSP-capable editor (VS Code, neovim,
  emacs, helix) a madc client without adopting madcide. The mapping is
  thin: the span classifier's {line, col, len, class} rows ≈
  `semanticTokens`; diagnostics rows → `publishDiagnostics`; the
  outline query → `documentSymbol`; `parse_enclosing` → `hover`.
  Compiler-truth answers through a protocol the world already speaks —
  a reach multiplier worth landing EARLY in the transport slice.
- **Does not fit as the native protocol**: LSP is single-user and
  editor-centric (no presence, no tiers, no multi-client sessions),
  position-addressed (line/character — the byte-offset trap the entity
  handles exist to avoid), and has no vocabulary for the past/future
  axes or project-membership verbs. It is a LOSSY PROJECTION of the
  gateway API — the same relationship git and Jira have to the nexus:
  spoken fluently at the boundary, never the internal vocabulary.
- **Patterns to borrow into the native protocol**: versioned
  incremental document sync (remote clients over latency) and
  capability negotiation at connect — the latter resolves the
  tree-vs-raw-state open question per client: a client DECLARES which
  projections it consumes.
- Out of scope for now: madcide as an LSP *client* (consuming other
  languages' servers for non-madc files) — possible later, not driving;
  the polyglot vision wants native understanding of the C/C++ family
  in-engine.

## Sequencing

1. **The seam slice** (first): an IDE session type (C++-first, per
   cpp-first-api) OWNS the state currently parked on ui entity bags;
   `apply_ide_event`/`compose_ide_tree` become its surface;
   behavior-identical, pinned by the existing gates. The TUI keeps
   render/input/refresh/suspend; the session never touches a tui
   handle. The `@pane` binding mechanism + migration of hardwired modal
   keys lands here (client-side).

   **LANDED (s148, feature/madcide-project-claude):** `class IdeSession`
   (madcide_core.inc = the session layer; the bags stay its private
   storage initially) with the two surface methods plus lifecycle
   (open/attach/close) and the seam verbs; the new
   `tools/madcide/madcide_client.inc` is the TUI client (run_ide, the
   return-key pause, request servicing). Across the seam: client-pushed
   FACTS (viewport, terminal presence, pending chord), the KEY TABLE as
   data (profiles validate handle-free via the new
   `ui::tui_validate_keys` — one converter with tui_bind_keys — and the
   client rebinds on a generation counter), and parked TERMINAL
   REQUESTS (run/projrun/cmd/shell/refresh; the client drains one after
   each event, suspends, calls `term_exec` back, pauses, resumes —
   headless sessions refuse exactly as before). The layer boundary is
   GATED: `scripts/check-madcide-seam.sh` in fulltest (session layer
   tui-free, negative-controlled). Named residues: state still lives on
   the bags (member migration rides later slices); events still cross
   as key/text shapes (commands-with-arguments is slice 3, where modal
   key→name lookup moves client-side); vised keeps its own smaller
   apply_event (an example, not the tool).
2. **The project window** — the first feature written against the API
   (manifest reader/writer symmetry, groups, membership verbs).
   *(Executed s147 with the owner's re-sequencing — the window came
   first and pulled the @scope binding mechanism forward with it; the
   seam slice is now next, and the window's state migrates onto the
   session type with everything else. ^B correlation still open.)*
3. **Commands-with-arguments + events**: prompt completions become
   command calls; change notifications for clients.
4. **Presence + permission tiers**: multi-client sessions, per-client
   colour (proposal standing: the session deals colours from the active
   theme's presence palette at connect; clients supply a display name).
5. **Transports**: value channels / socket / MCP / LSP adapters over
   the same command/query/event vocabulary (LSP early — it multiplies
   clients for free; see the LSP section).
6. **Nexus axes** (madcdis links + madcdat adapters): git adapter
   first; trackers by demand.

## Open questions

- **madcide as an executable artifact** (owner, 2026-08-31: "we should
  also be able to build it as an executable binary, and then we need to
  decide if madc should output the binary directly or if madc should
  emit c11 code and pass that to the compiler we use to build madc").
  Standing recommendation (unruled): BOTH lanes already exist as
  first-class outputs of the one IR (ADR 0001) — default to the direct
  AOT `--exe` lane (self-contained; no C toolchain on the user's
  machine; ELF/PE/Mach-O writers in-tree; the artifact kind the
  frozen-artifact taxonomy sanctions), with `--emit=c11` → system
  compiler as the optimizing RELEASE lane (gcc/clang -O2 beats MIR's
  fast-but--O1-shape codegen, and it exercises the transpiler). The
  real work item is neither codegen lane but the LINK: madcide calls
  the compiler as a library (madc::parse_open/parse_check/
  project_build, runtime-compiled .madv verbs), so its binary must link
  the whole madc engine — the libmadc embedding arc's first real
  consumer: (a) the exe lane learns to link programs that use
  compiler-as-library namespaces against libmadc, (b) data files
  (profiles, themes, verb bodies) resolve relative to the binary. Cut
  as its own slice after the seam + merge wave.

- Is the composed TREE part of the public API (trivial thin clients) or
  a TUI-client detail (raw-state projections only)? Lean: both are
  queries; raw state is the contract personalities build on.
- Colour/identity assignment at connect (proposal above stands unruled).
- joe.keys spellings for the project verbs (`^k j` projaddcur proposed).
- Manifest default name/location (`<base>.prj.json` beside the launch
  file — standing default, owner veto welcome).
- Nexus link-store persistence: repo-adjacent files (rides git, diffs,
  survives clones) vs a madcdat store — leaning repo-adjacent for the
  links themselves, madcdat for caches; NOT yet ruled.
- Permission tier vocabulary and promotion mechanics.
- Entity-identity-across-history design (named above).
