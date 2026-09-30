# madcide plugins, written in madc — design (2026-09-30)

**Status:** design. The whole design is in the next release (owner, 2026-09-30), beside the REPL, the bug fixes, and `chthonic`, the Thonny-like teaching IDE as a madcide plugin. The release order is plan `madc-repl-thonny-plan-2026-09-24.md` §41.11a.

## 1. The owner's direction (2026-09-30)

- **Configuration, not modes.** "Rather than a hardcoded 'learn' mode", the layout, the menu, and "some ways that the editor works" should be configurable, "like a plugin".
- **Plugins written in madc**, "like how vscode plugins/extensions work". The owner leans towards Neovim's model and asked whether JetBrains/CLion has more to offer, with VS Code as the reference for familiarity.
- **Precompiled plugins:** "we could allow compilation of madcide plugins into .so/.dll/.dylib files for faster startup."

## 2. Precedents

| | VS Code (TypeScript) | Neovim (Lua) | JetBrains/CLion (Kotlin/Java) |
|---|---|---|---|
| Where the code runs | a separate Extension Host process | inside the editor | inside the IDE's JVM |
| How it plugs in | `package.json` `contributes` (commands, menus, keybindings, views, configuration), plus code | the same API the core uses; config (`init.lua`) is code | `plugin.xml` declares implementations of typed extension points; plugins can declare their own |
| Loading | lazy, through activation events | at startup, or lazy through lazy.nvim's command/event/filetype triggers; `vim.pack` built in since 0.12 (2026-03) | at startup; dynamic load/unload since 2020.1 |
| API | a curated, stable, versioned layer | one API (`nvim_*`), reached from Lua in-process and over msgpack-RPC (GUIs, remote plugins) | the platform's internals, whose changes break plugins each release (`sinceBuild` / `untilBuild`) |
| A crash | the editor survives | Lua is memory-safe | memory-safe |

- **Emacs** is the purest case of an editor written in its own extension language, which is madcide's situation (it is written in madc).
- **Zed** is the one editor whose plugins are compiled code (Rust). It runs them in a WebAssembly sandbox, because compiled code can crash its host.
- **JetBrains' split mode** (a frontend process and a backend process, serializable presentation data) is JetBrains moving towards the process boundary that madcide's client-server arc already has.
- **Native parts built at install:** Neovim plugins that ship them (for example, a `build` step running `make`) compile them when the plugin is installed, not at every launch.

## 3. The model: Neovim's, with one thing borrowed from each of the others

- **From Neovim, the core:**
  - a plugin uses the same API madcide's own features use;
  - that API is one API reached two ways: inside madcide, and over the command seat by a plugin in another process;
  - a plugin is a directory on a search path, and there is no ceremony.
- **From VS Code, declarative contributions and lazy activation:**
  - a plugin's keys, menus, toolbar, layout and views are data in its manifest, read without loading its code;
  - its code loads when an activation event fires.
  - madc must compile what Lua only loads, so this is how startup stays fast.
- **From JetBrains, typed extension points that the core uses too:**
  - every kind of contribution is an enum kind with a declared contract;
  - madcide's own REPL pane, Problems and Outline register through the same points as a plugin does (one implementation, no parallel path).
- **Left out of JetBrains:** the XML descriptors, the service/dependency-injection layer, and an API made of internals.
- **VS Code's familiarity** belongs to the surface users touch: a manifest, the command palette, settings, and installing a plugin. It does not shape the architecture.

## 4. What the code has (recon 2026-09-30, HEAD `5025256cc`)

- **Data profiles, loaded by name** (`tools/madcide/profiles/`):
  - `*.keys` (the personality, with `@scope` sections), `*.layout`, `*.menu`, `*.theme` and `*.status`;
  - each has its own line parser, refuses a bad line with its number, and bakes in a rescue default.
  - `toggle_profile` cycles the `.keys` files found in the directory, with no list of names.
  - `init_view_es` (`madcide_core.inc:7800-7812`) hard-codes the names `joe` and `default`.
- **The search path:** `resolve_profile_dir` (`madcide_core.inc:220`) tries three places, and the first that exists wins:
  1. the source tree;
  2. `<exedir>/../share/madcide/profiles`;
  3. `<exedir>/profiles`.

  There is no per-user directory.
- **Settings:** madcide has none. madc's `madc.ini` reader (`include/madc_config.h`, `src/madc_config.cpp`) is typed to the compiler's own keys (`config_settings`: std, stdlib, forest, include, the limits). It has no sections, an unknown key is an error, and scripts cannot read it. Its search is `./madc.ini`, then `$XDG_CONFIG_HOME/madc/madc.ini` (or `~/.config/madc/`), then the system config directory (`src/madc.cpp:385-395`).
- **Commands** are a static enum with one name table (`cmd_table`, `madcide_enums.inc:28-110`). `cmd_of` converts a name once at every input boundary (profiles, menus, `-c`, the seat). `check-madcide-command-registry.sh` counts them and follows the core's includes.
- **Views** are `enum ide_view` (`madcide_enums.inc:468`), and `compose_chrome_pane` (`madcide_core.inc:7073`) switches on the kind. Problems and Outline are the row-list shape: writers set a bag key (`diags`, `outline`), and the composer reads it.
- **The command seat** (`madcide_api.inc`):
  - one JSON line per request, `{"cmd", "args", "seq"}`;
  - the push feed `{"event": …}` (`broadcast_events`) and permission tiers (`grant_tier` / `eff_tier`).
  - The MCP seat, the LSP face and `--attach` are adapters over it.
- **Built-in modules** are compiled in by `#include`: `madcide_repl.inc` (its commands, pump and state keys) is already shaped like a module.
- **Installed madcide is an AOT executable** (`scripts/package_install_gate.sh`: `$bindir/madcide`, `madcide.exe`). A plugin shipped as madc source is compiled by the engine inside madcide, at every launch that activates it.
- **Shared objects:** `madc -shared` emits an ELF `ET_DYN` shared object (`src/madc.cpp:447`), "dlopen/import-consumable". The Mach-O and PE writers refuse it "by design" (`third_party/mir/mir-macho.c:342`, `third_party/mir/mir-pe.c:935`: no libmadc dylib or DLL exists).
- **Building in-process:** `madc::parse_build(diags, handle, kind, outpath)` builds from a live parse handle in-process, with kinds `exe` and `obj` (`include/madc/ns_madc:269-292`).
- **Loading at runtime:** `<dlfcn.h>` is embedded (`include/madc/posix/dlfcn.h`). A library's platform spelling has one owner, `madc_module_library_spelling()` (`src/madc_modules.cpp`).
- **In-process compilation today:** `madc::eval_*` compiles and runs source (values in, values out, through a context) but cannot call back into the host program. The REPL's `InteractiveSession` links a module per entry into its own live program, not into its host.
- **Out-of-process isolation:** `Process` + `child_body` forks the running madc. `SessionClient` compiles in the child, restarts it after a crash, and puts its streams in `chan_select` through a readiness source.

## 5. The design

### 5.1 A plugin is a directory with a manifest

```text
<plugin dir>/<name>/
  <name>.plugin        the manifest (JSON), read without running anything
  *.keys *.layout *.menu *.theme *.status    data contributions, in their existing formats
  <name>.mad           code (Stage B), madc source
  <name>.so            code built from it (Stage B, optional, explicit)
```

- **The manifest is JSON,** read through the one value↔JSON bridge (`wt_json_to_value`, which `js::parse` uses). Reading it compiles nothing and costs microseconds, and it is the format VS Code's `package.json` users know.
  - Its words convert once, at load, to enum codes. An unknown word refuses the plugin with its reason (enum-over-strings: a manifest is an input boundary).
  - Fields:
    - `name`, `title`, `version`, and `api` (the plugin API version it targets);
    - `contributes`: data files by kind, commands (name, title, handler), views (name, title, the bag key its rows live on), and settings with defaults;
    - `activation`: events, for code;
    - `code`: the source file and an optional library.
- **A bundle** is a plugin with no code. `default` is the bundle madcide ships. `chthonic` starts as one (Stage A) and gains code, its Variables view, in the release (§41.11a step 6). No profile is a mode:

  ```json
  { "name": "chthonic", "title": "Learning", "api": 1,
    "contributes": { "keys": "pico", "layout": "chthonic", "menu": "chthonic",
                     "settings": { "repl.std": "" } } }
  ```

- **The search path,** first found by name wins:
  1. the user's directory, `$XDG_CONFIG_HOME/madcide/plugins` (or `~/.config/madcide/plugins`), or `%APPDATA%\madcide\plugins` on Windows. `MADCIDE_CONFIG_DIR` overrides the configuration directory, as `MADCIDE_SESSION_DIR` does the session directory: `run_tests.sh` points it at a directory that does not exist, so the suite never reads a developer's configuration, and a test that needs one names its fixture in its `.env`;
  2. then the directories `resolve_profile_dir` already searches, which gain a `plugins/` beside `profiles/`.

  A plugin dropped in a directory joins with no list edited, as a `.keys` file joins `toggle_profile` today.

### 5.2 Selection and settings

- **Settings are `settings.json`** in madcide's configuration directory: `$XDG_CONFIG_HOME/madcide/` (or `~/.config/madcide/`), or `%APPDATA%\madcide\` on Windows, beside the user's `plugins/`. They are read through the same JSON bridge as the manifests, so values keep their types, and VS Code users know the file.
  - `madc.ini` stays the compiler's. Its reader is typed to the compiler's keys and refuses an unknown one, and editor settings have a different consumer (separation of concerns).
  - Precedence: the command line, then `settings.json`, then the active profile's settings, then each plugin's own defaults.
- **The active profile** is `"profile": "chthonic"` there, and `madcide --profile NAME` overrides it. The default is `default`, so nothing changes without either.
- **Other plugins:** `"plugins": ["a", "b"]` enables them in addition to the profile.
- **A setting's value** is the user's, else the active profile's, else the plugin's own default. `repl.std` gives the REPL's standard (`replstd`) the home it lacks today.

### 5.3 Extension points, typed

One enum, `contribution_kind`, whose contracts are declared in one header, `<madcide/plugin>`:

| Kind | Data or code | Contract |
|---|---|---|
| `keys`, `layout`, `menu`, `theme`, `status` | data | the existing file formats and parsers; a menu row's placement (`bar`, `palette`, `toolbar`) is the `menu_place` enum of §41.11a |
| `settings` | data | a name, a type and a default |
| `command` | code | `bool handler(long w, long es, long doc, const char *arg)`, invoked by the dispatcher on the session thread; the command id is interned at load, above the built-in enum range |
| `view` | data + code | a name, a title and a bag key. The view's rows live on that key, and the plugin's code keeps them current (from a command or an event), as `diags` is kept for Problems. `compose_chrome_pane` gains one arm for a contributed view, which renders the key's rows. |
| `event` | code | `void handler(long w, long es, long doc, var &event)` for a kind of the existing event feed |
| `filekind`, `runner` | code | Stage B's later points: a file-kind handler (`<bits/file_kinds>`) and a build runner (the `^B` rows) |

- **Composition never calls plugin code.** It renders bag state, which keeps it fast, and a plugin in another process then works through exactly the same path (owner law 2026-08-31: compose renders state, handlers mutate it).
- **Handlers are resolved once, at load.** The name in the manifest is converted to a function pointer (`dlsym` in a library, or the compiled module's symbol). A missing handler, or one whose declared contract does not match, refuses the plugin at load, never at use.
- **madcide's own features register through the same points,** compiled in (transport `builtin`). `madcide_repl.inc` is the first to move (Stage B5), which proves the points against a real feature.

### 5.4 One API, two transports

A plugin's code sees one API, declared in `<madcide/plugin>`:
- `ide::run(code, arg)`: any command, built-in or contributed (the dispatcher);
- `ide::get` / `ide::set`: bag state on the session or a document, with the key and scope as data;
- `ide::command_id(name)`: a name interned once, at the plugin's activation;
- the engine's own verbs (`madc::session_*`, `madc::parse_*`, `php::`, …), as any madc program calls them, over handles the plugin opened itself.

**A plugin never holds a handle the core owns.** The REPL pane's session, for example, is reached through the pane's commands (`replbindings`) and its events (`repl_event`), never through its handle. A handle lives in the process and on the thread that opened it, so this is what lets the same plugin run under the `host` transport (`chthonic`'s Variables view is the first case, §41.11a).

**The transports,** `plugin_transport { builtin, library, source, host }`:

| Transport | What it is | When |
|---|---|---|
| `builtin` | compiled into madcide | madcide's own modules |
| `library` | a `.so` loaded into madcide's process | Stage B2 |
| `source` | the `.mad` compiled into madcide's process when the plugin activates | Stage B3 |
| `host` | a forked child of the running madc compiles and runs the plugin, and speaks to madcide over the command seat: API calls become seat requests, and handlers are invoked by `{"event": …}` messages | Stage B4 |

- **The shim:** a plugin's source is the same for every transport. `<madcide/plugin>` has an in-process implementation (a table of function pointers the host passes at activation, SQLite's `sqlite3_api_routines` pattern) and a seat implementation.
- **The seat gains** `get` / `set` requests under its permission tiers.

**Trust.** `library` and `source` run in madcide's process, so a crash there ends the editor, as a C plugin ends Vim. `host` survives a crash, and the child restarts as the REPL backend does. Two defaults:
- A plugin under the user's directory, or shipped with madcide, loads in-process.
- A manifest may ask for `host`, and a setting forces `host` for everything (`plugins.isolate = true`).

### 5.5 Precompiled plugins (the owner's `.so` idea)

- **An explicit build, never a cache.**
  - The command is `madcide --build-plugin <dir>`, the running madc compiling in-process: `parse_open` + `parse_build`, which gains a `shared` kind, the running madc being the compiler. `madc -shared` works too.
  - The library lands in the plugin's own directory, as an install artifact. Nothing is ever written automatically, and nothing lands beside user sources: the owner's 2026-08-22 ruling allows persistence only as explicit artifacts.
- **Versioned:**
  - The library records the plugin API version it was built against.
  - At load, a library whose version or platform does not match is refused with the reason, and the plugin's source form loads instead if it has one.
  - The source is the plugin, and the library is how it ships fast.
- **Why it matters here:** installed madcide is an AOT executable, so a `source` plugin costs one JIT compile of the plugin on every launch that activates it, while a `library` costs a `dlopen`. Stage B measures both on a real plugin before choosing the defaults.
- **Platforms:**
  - Linux ELF works today.
  - macOS dylib and Windows DLL emission are refused today in the MIR writers (a scoping decision, §9). The owner decided they are supported as `.so` is (G2, in B2).

### 5.6 Activation

- Events: `startup`, a contributed command's first invocation, a contributed view's first showing, and a file kind's first opening. Each is an enum code at load (`activation_event`).
- A bundle has no code, so it never activates; its data loads when it is selected.
- Before activation, the plugin's menus, keys and toolbar are already present, because they are data. The first use loads the code, then runs the handler (VS Code's model).

## 6. Engine gaps (Stage B), named

- **G1. One engine per process.** Handles (`handle_table`, `thread_local`) belong to the libmadc instance that opened them. So an in-process plugin must bind to madcide's engine, never load a second libmadc, or its `ui::set(w, …)` would reach a different table.
  - Mechanism: the API table passed at activation (§5.4), or the host exporting its symbols.
  - Recon first: what a `madc -shared` library's undefined references bind to today.
- **G2. dylib and DLL emission** are refused today (`mir-macho.c:342`, `mir-pe.c:935`). The owner decided they are supported as `.so` is (§9): the Mach-O writer emits `MH_DYLIB`, and the PE writer a DLL.
- **G3. Compiling a module into the running process** and resolving its handlers, for the `source` transport. `eval_*` compiles in-process but has no path back into the host, and the REPL links into its own session only. The design is its own slice, which starts by reading how `InteractiveSession` links a module.
- **G4. Command ids at runtime:** `cmd_table` gains a contributed range above its enum. The registry gate learns the range, and the menus and key profiles convert names through the same `cmd_of`.
- **G5. `parse_build`'s `shared` kind** (today `exe` and `obj`).
- **G6. The seat's `get` / `set`,** under its permission tiers, for the `host` transport.

## 7. Thread contract

- **In-process plugin code** (`library`, `source`, `builtin`) runs on the session's thread, between events, and never during composition. A plugin that starts a cooperative task follows the task verbs' contract (`madc_task_io.h`). A plugin's own globals are its own: two plugins share state only through the bag.
- **A `host` plugin's requests** are serialized over the seat, one connection per plugin host, as every seat client's are.
- **The registries** (contributed commands, views, settings) are built at load, on the session's thread, and are read-only afterwards, until a plugin is loaded or unloaded at runtime, which is later work.

## 8. Staging

All of it is in the next release (owner, 2026-09-30). The order, interleaved with the REPL pane's pieces and `chthonic`, is plan §41.11a's release order:
- **Stage A, bundles** (step 1):
  - the manifest loader, for the data kinds and settings;
  - the plugin search path, including the user's directory;
  - `settings.json`;
  - `--profile NAME`;
  - `default` and `chthonic` as bundles, and the command line's file becoming optional (an untitled buffer, decided 2026-09-30).

  **Gate:** a model test composing each bundle; a manifest with an unknown word refused with its reason; a user-directory bundle overriding a shipped one by name; `"profile": "chthonic"` in a test `settings.json` (under a test configuration directory, `MADCIDE_CONFIG_DIR`) selecting `chthonic`.

  **Built, part 1 (2026-09-30): the loader, the search path and settings.** `tools/madcide/madcide_plugins.inc` holds one owner each:
  - `madcide_config_dir` (`MADCIDE_CONFIG_DIR`, then `$XDG_CONFIG_HOME/madcide`, `%APPDATA%/madcide` on Windows, `~/.config/madcide`);
  - `plugin_find`, the search path (the user's `plugins/`, then the shipped one through `resolve_data_dir`);
  - `plugin_manifest_read`, which checks every word at load: `plugin_field` and `contribution_kind` in `madcide_enums.inc`, the name its directory's, `api` 1, a data word one name and never a path;
  - `active_profile` (the command line, then `settings.json`'s `"profile"`, then `default`) and `bundle_select`, which leaves the default in use with the reason on the status line;
  - `bundle_word` (a word the bundle leaves unset is the default bundle's);
  - `bundle_data_path`, the one path the five data loaders read (the bundle's own directory, then `profiles/`; gated by `check-madcide-single-owners.sh`);
  - `setting_get` (`settings.json`, then the bundle's default, then the default bundle's).

  `init_view_es` loads what the bundle names, with the key fallback chain (the bundle's keys, then the default's, then the rescue set), and `repl.std` seeds `replstd`. `plugins/default/default.plugin` ships beside a built-in copy, and the test pins the two to one reading. The packages ship `plugins/`. `keyed_get` no longer adds the key it reads (the carrier's subscript creates the slot it reads, so a validating read had added `"version": null` to a manifest). Gate: `tests/testmadcide_bundles`.

  **Built, part 2 (2026-09-30): `--profile` and `chthonic`.** The command line has one reader, `ide_args_parse` (`tools/madcide/madcide_args.inc`): flags anywhere, the first word that is not a flag is the file, `--` ends the flags, and an unknown flag, a flag missing its value or a second file is refused with the reason and the usage (exit 2); `--help` prints the usage. `--profile NAME` reaches every face (`run_tui`, `run_serve`, `run_lsp`, `run_mcp`, `run_line`, `run_once` → `IdeSession::open`). `plugins/chthonic/` ships `chthonic.plugin` (keys `pico`), `chthonic.layout` (the editor, and the REPL as the visible panel's first tab beside Problems; no sidebar yet) and `chthonic.menu` (File, Edit, Run, View, Help; Stop, Language… and Variables join with their commands). `init_view_es` starts the REPL session when the layout shows it (`view_shown(viewREPL)`), so `chthonic` opens with a running session and `default` forks nothing. Gates: `tests/testmadcide_chthonic`, `testmadcide_cli`'s argv lines; `docs/man/madcide.1` documents `--profile`, settings and plugins.
- **Stage B, plugin code:**
  1. **Contributed commands, views and events** (G4), and the toolbar placement, with handlers in `builtin` form only: the extension points exercised by madcide's own code first (step 2).
  2. **The `library` transport** (G1, G2, G5): `--build-plugin`, the API table, versioned refusal; on Linux first, then macOS (`MH_DYLIB`) and Windows (DLL), through ROADMAP 6.5 (step 4).
  3. **The `source` transport** (G3), with the activation cost measured against `library` (step 5).
  4. **`chthonic`'s code, the Variables view,** shipped as source plus a prebuilt library per platform (step 6).
  5. **The `host` transport** (G6): a crash leaves madcide running, and the plugin restarts (step 7).
  6. **The REPL pane on the points:** `madcide_repl.inc` registers through them, so no built-in path remains beside them (step 8).

## 9. Decided (owner, 2026-09-30)

1. **The manifest is JSON.**
2. **dylib and DLL are supported as `.so` is.**
   - The refusals were a scoping decision, never an owner ruling. The Mach-O plan (`docs/plans/2026-07-25-macho-arm64-plan.md:297-299`) put dylib emission "deliberately out of scope, there is no libmadc.dylib by design". The PE writer copied that posture (`b3195b005`, 2026-08-15: "the Mach-O writer's posture").
   - Linux `.so` works today: `madc -shared`, pinned by `tests/unit/test_native_shared.cpp` (dlopen and dlsym of the emitted `ET_DYN`).
   - G2 is therefore required, not optional, and it is general `madc -shared` parity, not only plugins:
     - the Mach-O writer emits `MH_DYLIB` (an `LC_ID_DYLIB`, the export trie, the ad-hoc signature it already writes);
     - the PE writer emits a DLL (an export directory, base relocations);
     - `madc -shared` gives `.dylib` and `.dll`, spelled by `madc_module_library_spelling()`.
   - A plugin library needs no libmadc dylib or DLL: it reaches the engine through the API table (G1). A general shared library that uses the value runtime does, and that is the deferred `libmadc.dylib` (`docs/plans/2026-08-07-macos-release-lane-plan.md:128-134`) and a `libmadc.dll`.
   - Staging: B2's `library` transport lands on Linux, then on macOS and Windows within B2 (the old B6 folds into it), so a plugin library ships on all three platforms before Stage B closes.
