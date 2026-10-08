# madcide in its own repository

Owner (2026-10-08): madcide has grown enough functionality that it gets lost
inside the madc repository, and Chthonia is one face on it. Plan the split; it
is NOT scheduled yet. When it runs: after the live-colour + Dark+ work lands
(that work spans the compiler and the editor in one feature), as the first item
of the following arc. It is the prerequisite of the IDE plugin work
(`2026-10-08-ide-plugins.md`).

## The model: an ordinary package dependency

madcide depends on madc the way any program depends on a library release.

- madcide declares the madc it is built for (a `madcide.json` `"madc"` pin,
  the `chthonia.json` precedent).
- madcide's CI fetches that madc release and runs madcide's FULL suite against
  it on every platform (Linux 24.04 + 22.04, macOS arm64 + Intel, Windows).
  The full gamut still runs; it answers "is this madcide compatible with that
  madc", not "is madc correct".
- A madc release never waits on madcide. A new madc → madcide bumps its pin,
  its suite runs; a red is a bug against whichever side broke the contract.
- Chthonia pins a madcide release (which brings its paired madc), not madc.

The chain after the split:

    madc (compiler + engine)  ←pin—  madcide (IDE)  ←pin—  Chthonia (a face)

## The boundary

**Stays in madc — the engine madcide consumes** (a versioned public surface):
`ns_ui` / `ns_ui_web` and the webview module, `include/madcdis/` (key_resolver,
focus_state, ui_apply_keys, tui_model, web_model, text_buffer, hub, term_screen,
doc_lens, process), `madc::parse_*` / `lex_spans` / `parse_spans`, sessions and
the session client, the module loader. Engine tests that do not need the IDE
stay (unit tests: test_tui_model, test_web_model, test_keys, test_text_buffer,
… ; tests/ that exercise ns_ui directly).

**Moves to the madcide repo — application code, with its history**
(`git filter-repo --path tools/madcide --path tools/texteditor --path …`):

- `tools/madcide/` (55 files, ~28k lines), `tools/texteditor/` (the editor
  core shared by madcide, plus `lined` and `vised` — their verbs and checks
  already install under madcide's share directory), `tools/vscode-madcide/`.
- Tests: the 59 `tests/testmadcide*`, `tests/gui/madcide*` (17), and the
  other tests that include `tools/madcide` or `tools/texteditor` (testide*,
  testlineed, testvised, testnexus, testmcpclient, …) — 93 in all, with their
  fixtures. A test whose subject is the ENGINE but drives it through madcide
  gets an engine-level twin in madc before it moves (coverage must not leave
  with it); `git grep -l "tools/madcide\|tools/texteditor" -- tests` is the
  list to triage.
- Gates: `check-madcide-*.sh` (7), the `madcide_*` pty/quit/save gates,
  `stage_madcide_data.sh`, `build_shipped_plugins.sh`, and the shared gates
  whose subject is tools/ only (`check-one-anchor-owner`, `-blank-run`,
  `-selection-rule`, `-subject-doc`, `-madcide-staging`). Gates spanning both
  (`check-dialect-literals`, `check-dialect-lean`, `check-one-event-builder`)
  run in BOTH repos, each over its own tree.
- Docs: `docs/madcide.md`, `docs/man/madcide.1`, the madcide plans (history
  stays readable in madc; new madcide plans live in the madcide repo).
- Packaging: madcide's binary (AOT-built by its pinned madc), profiles,
  plugins, plugin API headers, verbs/checks, man page, desktop entry, and
  the Ubuntu 24.04 AppArmor grant for madcide's WebKit windows.
- Agent instructions: an `AGENTS.md` for madcide carrying the rules that apply
  to dialect application code (value-first, dialect-literals, dialect-lean,
  enums-over-strings, thread-safety, testing tiers) — pointers to the madc
  originals where the text is shared, never a fork of them.

## Packaging (default decided; owner may override)

madcide ships its own packages, each carrying its paired madc — the
stand-alone model Chthonia already uses (one download, one install). madc's
packages stop shipping madcide. For ONE transition release, madc's release
notes point to the madcide release. (Alternative not taken: madc's packages
keep bundling a pinned madcide — it re-couples the release trains the split
exists to separate.)

## The engine as a versioned surface

- madc's version is the compatibility contract. A change to the engine API
  madcide uses that is not backward compatible is called out in madc's release
  notes and the CHANGELOG, like any library break.
- Compile-time checks catch a removed or renamed engine function in madcide's
  CI immediately; the madcide suite catches behaviour changes.
- madc keeps no madcide-specific code paths (Rule #7 and "the engine ships no
  application code" — the split makes that boundary physical).

## Steps

1. **Triage the 93 tests**: IDE behaviour (moves) vs engine behaviour driven
   through the IDE (gets an engine twin in madc first). Land the twins on madc.
2. **Create `derekbsnider/madcide`** from a `git filter-repo` of the moving
   paths (history kept). Add `madcide.json` (`"madc": "<current release>"`),
   AGENTS.md, a test runner (madc's `run_tests.sh` conventions — the fixture
   rules are the same), the gates, and CI: fetch the pinned madc release per
   platform, build madcide, run the full suite + gates, package.
3. **madcide CI green on the pinned madc** across every platform lane.
4. **Remove the moved paths from madc** in one commit: tools/, tests, gates
   (and their `fulltest` wiring), packaging stanzas (`package_release*.sh`,
   the Homebrew formula's madcide build, the AppArmor profile's madcide line),
   `gui_lane.sh` madcide cases. madc's battery green without them.
5. **Repoint Chthonia** at a madcide release (`chthonia.json` pins madcide;
   its CI fetches madcide's package, which carries madc).
6. **First releases**: madc (no madcide inside), madcide (its own), Chthonia
   (on madcide) — release notes on each say where the others went.
7. **KG + mirrors**: a Project node for madcide, DEPENDS_ON edges
   (Chthonia → madcide → madc), the agent hand-off docs updated.

## Thread-safety contract

No new runtime state: a repository and packaging change. Each moved component
keeps its existing contract.

## Open items for the owner

- Repository name (`madcide` assumed).
- Whether a madc release may land while madcide's suite is red against it
  (default: yes — madc's own suite gates madc; madcide's red is a madcide
  pin-bump task or an engine bug filed on madc).
