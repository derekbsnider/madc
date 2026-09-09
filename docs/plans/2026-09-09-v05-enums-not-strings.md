# Slice V0.5 — enums, not strings (the IDE and its engine seams)

Owner law 2026-09-09 (repeat correction; `.claude/rules/enum-over-strings.md`,
KG Decision `enums_not_strings_everywhere_owner_law`): keys, actions, targets
and view / region / tab discriminators are enums or numeric constants — in the
engine AND in dialect code. Text is legal only at an INPUT boundary (a profile
file, the page's JSON, a command line, a manifest), converted ONCE at load; a
misspelling is a REFUSAL at load; dispatch is a `switch` on the code. Two harms
named by the owner: an undefined string code goes undetected; string compares
in a hot path cost what an integer compare does not.

This slice converts the IDE (`tools/madcide`) and the engine seams it rides on.
It precedes V1 of the client-server design
(`2026-09-09-nexus-client-server-design.md` §4).

## 1. Measured inventory (2026-09-09, develop 4392d6e0)

| Where | What | Count |
|---|---|---|
| `tools/madcide/madcide_core.inc` | `== "…"` sites | 173 |
| — of which the action dispatcher (`apply_ide_event` and the modal `*_event` arms: `a == "save"`, `pc == "paneclose"`, `a == "bld-run"`, `a == "opt-…"`, vi `a == "vichange"` …) | ~110 |
| — discriminators on the state bag: `pane` (8 spellings), `paneltab` (3), `view` (3), `pmode` (12), `vimode` (2), build-request `kind` (4), row `tag` (`prj`), `slot` (`popup`) | ~40 |
| — legitimate text at a boundary (the colon line's verbs, profile parsing `rest == "-"`, `scope == "gui"`, file extensions, `"\r"`, the manifest's `kind`) | ~20 |
| `tools/madcide/madcide_client.inc` | `strcmp(target, "term")` (the ONLY per-target fact) | 1 |
| `tools/madcide/madcide.mad` | `target = "term"` / `ui_web::target()` | 2 |
| `src/ns_ui.cpp`, `src/ui_term.cpp`, `include/madc/ns_ui`, `ns_ui_web` | `"term"` / `"web"` target-name sites | 14 |
| `tools/texteditor/vised_core.inc` | `k == "^s"` key-name ladder (vised's own bindings) | 10 |
| Engine rows crossing INTO the dialect with a string discriminator | diagnostics `severity`, dialog `mode` | 2 |

Already enums (no work): `ev["event_code"]` (`ui::event_kind`), `ev["key_code"]`
(`ui::key`), `ev["phase_code"]`; the shared editor core (`editor_events.inc`)
reads only the codes.

## 2. Decided shape (standing defaults; owner veto welcome)

**Enum style.** Prefixed UNSCOPED enums with a fixed underlying type — the
owner's own spelling (`enum : uint8_t { ui::NONE, ui::LINE, … }`): `switch` on a
`long` read from a bag takes `case cmdSAVE:` by integral promotion, `var ==
paneOUTLINE` compares as an integer, and a misspelt enumerator is a compile
error. The engine's existing `enum class key / event_kind / pointer_phase`
stay as they are (compared with `==`; never switched on).

**Homes.** Engine-facing vocabularies in `include/madc/bits/ui_enums` (the one
text for engine + dialect): the UI level, the dialog mode. IDE-private
vocabularies in a new dialect fragment `tools/madcide/madcide_enums.inc`
(included first by `madcide_core.inc`): commands, panes, panel tabs, prompt
modes, vi modes, build-request kinds, row tags. The View REPRESENTATION
(`mc11 / c11 / c++`) is the emitter's target set (`CIR_EMIT_TARGETS`) and lands
as the shared emit-target enum with V1 Views — not a second copy here.

**The command vocabulary is CODE; its names are the boundary.** `enum ide_cmd
: unsigned short { cmdNONE = 0, cmdSAVE, … }` — every verb the dispatcher
implements: the registry commands (`profiles/default.menu` rows), the eight
key-named motions (`left … pgdn` — "the action name IS the key spelling"), the
modal-scope actions (`pcommit`, `projclose`, `paneclose`, …), the pane-row verbs
(`bld-*`, `opt-*`, `goto`, `w f q`) and the vi operators. ONE name → code table
(`cmd_table()`, a literal built once and cached on the bag) is the converter
every input boundary uses: the `.keys` profiles (`load_profile`,
`bind_rescue_keys`, the modal defaults), `.menu` (`load_menu`), the `-c`
command line (V1.5) and the `:` line. An unknown name REFUSES the profile /
menu with the offending line (`Profile 'x' line 12: unknown action 'svae'`).
`cmd_name(code)` is the OUTPUT converter (messages, the help pane, the palette).

**Actions carry codes end to end.**
- The engine's binding table value becomes `{ name, code }`
  (`tui_bindings::binding`); `bind_keys` accepts a string value (name, code 0)
  or an integer value (code, name ""); `key_step` / `tui_event` gain
  `action_code`; the event object gains `action_code`. The IDE binds
  `{ seq: code }`; other tools keep binding names (behaviour-identical).
- A choice option / menu item / tab / button may carry an integer `code` hint
  beside `action`; `focus_state` records `option_codes` and a `choose` event
  carries `action_code`; the menu bar's accelerator lookup is
  `seq_for_code(code)` when the item has one (else `seq_for_action(name)`).
- The page and the native menu post NAMES (`{kind:'action', action:id}` — the
  webview library's menu callback speaks ids): the DOM model converts at that
  boundary from the `id → code` table it recorded at the last render. One
  converter, one input shape; `page.js` unchanged.
- The dispatcher: `long code = ev["action_code"]`; `switch (code) { case
  cmdSAVE: … }`. `cmdNONE` = an unbound chord (the `Unbound: seq` message).

**Targets declare a level; programs open a level.**
- `include/madc/bits/ui_enums`: `namespace ui { enum level : unsigned char {
  ui::NONE = 0, ui::LINE, ui::TUI, ui::WEB, ui::GUI, ui::GFX2D, ui::GFX3D }; }`; the names
  (`ui_level_name` / `ui_level_from_name`) live beside `pointer_phase_name` in
  `madcdis/ui_events.h` — the engine derives them, never re-typed.
- `ui::register_host(name, level, ops)`: a host declares the level it serves
  (`<ns_ui_web>`: `ui::WEB`; the fake test host: `ui::WEB`). The grid frontend is
  `ui::TUI` intrinsically.
- `ui::open(ui::level)` — the target that serves that level (the grid for
  `ui::TUI`; the first registered host of the level; `0` + stderr "no target
  serves the <name> level" otherwise — `ui::LINE` refuses until V2.5).
  `ui::open(const char *name)` stays for name-addressed opening (tests).
  `ui::level_of(t)` reports the opened frontend's level; madcide's one
  per-target fact becomes `ui::level_of(t) <= ui::TUI` ("a real terminal").
- `madc --capabilities=json` gains `ui.levels`: the levels this build has a
  target for — `tui` (built in) plus `web` when the module map carries a
  `MADC_MODULE_GUI` row (data, never a name test). The gate reads the enum
  names from `bits/ui_enums` and asserts manifest ⊆ enum, `tui` present.

**Discriminators on the bag are integers.** `ui::set(w, es, "pane",
paneOUTLINE)`; `es_int(w, es, "pane", paneNONE)`; `switch (pane)`. Names appear
only where text leaves the program (`pane_name()` for the probes and the page's
CSS hooks) — so every `.expect` stays byte-identical.

**Engine rows crossing into the dialect carry codes.** The diagnostics row adds
`severity_code` (`DiagnosticSeverity`, already an enum in `madc.h`; the name
stays for display); the dialog event adds `mode_code` (`ui::dialog_mode {
open, save }` in `bits/ui_enums`; the request JSON keeps the name — the host
echoes it and the model converts at the boundary).

**Modal scope tables stay DATA keyed by the key's canonical spelling** (the
engine's own `key_resolver` shape, the gated ONE owner): their VALUES resolve
to codes at load; the handlers switch on the code. Key-name text never appears
in a dialect compare.

## 3. Sub-slices (each a commit with trailers; targeted tests per slice)

| # | Content | Tests / gate |
|---|---|---|
| **a. Levels** | `ui::level` + names; `register_host(name, level, ops)`; `open(level)`, `level_of`; capabilities `ui.levels`; madcide opens by level (`--gui` → `ui::WEB`), `has_term` from `level_of` | `testuihostfake` (registers with a level; opens by level; a level nobody serves refuses); `capabilities_json_gate.sh` (`ui.levels` ⊆ enum names, `tui` present); `testmadcide` + GUI 17 byte-identical |
| **b. Codes in the engine** | `tui_bindings::binding {name, code}`; `action_code` on `key_step` / `tui_event` / the event object; `code` hints → `option_codes` → `choose.action_code`; the DOM model's `id → code` table + boundary conversion; `seq_for_code`; `severity_code`; `dialog_mode` + `mode_code` | `test_tui_model` (bind an integer value; choose returns the code); a `testuihostfake` posted action resolves to its code; `testmadcide` unchanged (names still flow) |
| **c. The command enum** | `madcide_enums.inc` (`ide_cmd`, table, `cmd_of` / `cmd_name`); load-time resolution + refusal in `load_profile` / `bind_rescue_keys` / `modal_keys` / `load_menu`; the composer stamps `code` on items / tabs / buttons / rows; the dispatcher switches; the test seam's `ev_action` resolves through `cmd_of` | `testmadcide` + `testidemenu` + `testidedialog` + `testidehints` + GUI byte-identical; a new `testmadcide` clause: a profile with a misspelt action refuses naming its line; `check-madcide-command-registry.sh` re-anchored (registry ⊆ table ⊆ enum, every enumerator has a `case`) |
| **d. Discriminator enums + the gate** | `ide_pane`, `ide_tab`, `ide_prompt`, `ide_vimode`, `ide_reqkind`, `ide_tag` on the bag; `*_name()` at the output boundaries; vised's key ladder → `key_code` switch | `scripts/check-madcide-enums.sh` (fulltest): no `ev["action"]` / `ev["key"]` / `ev["event"]` read in `tools/`; no string literal written to or compared against a discriminator slot (`pane paneltab view pmode vimode kind tag slot`); every `case cmd…:` / `case pane…:` label names an enumerator that exists; negative controls for each rule |
| **e. Docs + mirrors** | `docs/madcide.md`, `docs/ui-*.md` (the `open(level)` API), `CHANGELOG`, `claude_status.json`, KG | — |

The full develop-set battery runs ONCE at the end of the slice (the merge
wave); a–d run their targeted tests.

## 4. Out of scope (named, not forgotten)

- The View representation enum (V1 — the emitter's target set becomes a shared
  `bits/` enum; `enter_view(w, es, doc, "mc11")` converts then).
- Moving the modal `@scope` tables into the engine's key resolver (a later
  gateway slice; the tables are data either way).
- `tools/texteditor` beyond vised's key ladder (lined has none; the shared
  core already reads codes).
