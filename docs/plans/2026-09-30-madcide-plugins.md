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

**Where the registry lives (2026-09-30, step 2's design).**
- One world entity, `plugins`, holds the registry: `commands` rows `{name, code, title, handler}`, `views` rows `{name, kind, title, key, show}` and `events` rows `{kind, handler}`. It is per world, never a process global (the thread-safety law: no new bare mutable globals), and every client of the world (each `es`) sees the same commands.
- A handler is its function's ADDRESS, kept as an integer in the row and called through the kind's typedef (`cmd_handler`, `event_handler`). A `builtin` handler's address is `(long)&fn`; a `library` handler's will be `dlsym`'s answer, so step 4 fills the same slot.
- A contributed command's code is interned at registration, from `cmd_contrib_base()` upward, in registration order. The converters become world-aware: `cmd_of_w(w, name)` and `cmd_name_w(out, w, code)` read the built-in table first, then the registry, and every input boundary (key profiles, menus, `-c`, the seat) converts through them.
- Registration runs before any profile or menu loads, so a key profile or a menu can name a contributed command: a built-in module registers when the world is made (the first is the REPL pane, step 8), a library plugin when its bundle loads (step 4).
- The dispatcher hands a code at or above the base to the command's handler, with the command's argument.
- A contributed view's kind is interned from `view_contrib_base()` upward, above every `ide_view` enumerator. Its rows' verb (a choose on a row) is the kind's image in `[cmd_view_row_base(), cmd_contrib_base())`, so the code names the list, as Problems' rows carry `cmdGOTO` and Outline's `cmdGOTODEF`.

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
- **G5. `parse_build`'s `shared` kind** (today `exe` and `obj`). Done 2026-10-01 (§8 Stage B item 2, part 1), with `build_native`'s include directories and the `madc::library_*` verbs.
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

  **Built, part 3 (2026-09-30): the file is optional.** `madcide` with no file opens an untitled buffer (a document with no path, the madc kind, named `untitled`); Save asks for its name, and F5 runs it as `untitled`. The headless faces still need a file. Gates: `tests/testmadcide_untitled`, `testmadcide_cli`.

  **Built, part 4 (2026-09-30, owner request): the program's name selects a bundle.** `program_name` reads `madc::sys.argv[0]`'s basename without its extension, and `active_profile` uses the bundle of that name when one exists: a binary, a symbolic link or a hard link named `chthonic` opens the `chthonic` bundle with no flag. The order is `--profile`, then the program's name, then `settings.json`'s `"profile"`, then `default`. No name is special: no bundle is named `madcide`, so madcide itself falls through. Gate: `tests/testmadcide_bundles` (`bundles-progname`).
- **Stage B, plugin code:**
  1. **Contributed commands, views and events** (G4), and the toolbar placement, with handlers in `builtin` form only: the extension points exercised by madcide's own code first (step 2).

     **Built, part 1 (2026-09-30): the toolbar.** A menu file's placement word `toolbar` rows become buttons: `compose_toolbar` carries `{label, action, code, enabled}` rows as the root's `toolbar` hint, the web page draws them as a button row and the terminal as a top line, and each renderer resolves the button's chord through the active key profile (`key_resolver::chord_for`), so the composed tree stays profile-independent. `chthonic.menu` ships Open, Save and Run. Gates: `tests/gui/madcide_toolbar`, `test_web_model`, `test_tui_model`, `testmadcide_chthonic`.

     **Built, part 2 (2026-09-30): contributed commands.** `plugin_command(w, name, title, handler)` registers a command in the world's `plugins` entity and returns its code, or 0 when refused: a built-in's or an earlier contribution's name, a word `plugin_name_ok` refuses, or no handler. `cmd_table_w`, `cmd_of_w` and `cmd_name_w` convert both ranges, and every input boundary reads them: key profiles (`parse_keys` with the world), menus (`load_menu`), `-c` (`run_once`), the seat (`api_run`), the MCP tool list and the LSP server's command ids. The dispatcher and `IdeSession::command` hand a contributed code to its handler with the argument, and the seat gates it at the editor tier (`cmd_min_tier_w`: the core cannot see a handler's effects). Gates: `tests/testmadcide_contrib`; `check-madcide-command-registry.sh` learns the contributed range (shipped code never contributes a built-in's name or one name twice, a bundle's menu and keys may name a contribution, and the enum's codes stay below `cmd_contrib_base()`).

     **Built, part 3 (2026-09-30): contributed views.** `plugin_view(w, name, title, key, show)` registers a view and returns its kind, or `viewNONE` when refused: a built-in's or an earlier contribution's name, a key that is not the plugin's own (`plugin.name`: no core bag key has a dot), or a show command that is not a contributed one. `view_of_w`, `view_name_w`, `view_title_w`, `view_show_cmd_w` and `view_rows_key` answer for both ranges. A layout line names a contributed view (`parse_layout` and `layout_to_text` take the world), and `show_view` shows it like any kind. `compose_chrome_pane`'s contributed arm, `contributed_view_node`, renders the key's rows `{content, file?, line?}` as a choice, each row carrying the view's row verb. A choose on a row goes through `goto_pane_row`, which now takes the view whose rows it reads (Problems, Outline or a contributed view), so one navigation owner serves all three; a row that names no line goes nowhere. Gates: `tests/testmadcide_contrib` (pins 6-9); `check-madcide-command-registry.sh` keeps the ranges disjoint (commands, then row verbs, then contributed commands; `ide_view`'s kinds under `view_contrib_base()`).

     **Built, part 4 (2026-09-30): contributed event handlers.** `plugin_event(w, kind, handler)` subscribes an `event_handler` to a kind of `repl_event` (`reTAKEN`, `reRAN`, `reREFUSED`, `reSTOPPED`; the bindings answer joins with its reply at step 3), refusing a kind outside the enum or a missing handler. `repl_reply` publishes each reply it has taken in (`repl_publish` → `publish_event`), and every handler subscribed to its kind runs in registration order with `{kind, reply}`, on the session's thread, never during composition. Gate: `tests/testmadcide_contrib` (pin 10). Step 2 is complete.
  2. **The `library` transport** (G1, G2, G5): `--build-plugin`, the API table, versioned refusal; on Linux first, then macOS (`MH_DYLIB`) and Windows (DLL), through ROADMAP 6.5 (step 4).

     **Recon (2026-10-01), G1:** `madc -shared` emits an ELF that NEEDs `libmadc.so.0`, and so do `bin/madc` and every `madc -o` executable (the installed madcide included), so a plugin library's value-runtime references (`madarray_*`, `__madc_fmt_text`, `ui::`) bind to the one libmadc already loaded, by soname. madcide's own functions (the registry, the dispatcher) are compiled madc code that nothing exports, so they reach a plugin through the API table. A JIT host loading a madc-built library, handing it a table of host functions and receiving a `var` back works today. A plugin's handlers are registered by its own code at activation through the table's typed slots, so the compiler checks each handler's contract when the plugin is built.

     **Built, part 1 (2026-10-01): the engine's half (G5).** `parse_build`, `build_native` and `project_build` take the kind `shared` (a shared object, `-shared`); the kind vocabulary's refusal text is its owner's (`native_kind_of`), where the three lanes each spelled a copy. `build_native` takes the CLI's `-I` directories (an array of text, in order), so a plugin in the user's directory reaches madcide's `<madcide/plugin>`. `madc::library_open` / `library_symbol` / `library_close` open a library by path over the one dl seam (`madcdl_open_local`'s `bind_now`): every reference binds at open, so a library whose names cannot all bind is refused at load with the loader's reason as data, never at its first call, and its names stay local. `madc::library_suffix` is the target's shared-library suffix from the one spelling owner, and `check-one-library-spelling.sh` now scans the madc code in `tools/` and `examples/` too. Gate: `tests/testbuild_shared` (skipped on darwin until G2's `MH_DYLIB`); `check-live-build-owners.sh` counts the refusal text.

     **Built, part 2 (2026-10-01): madcide's half, on Linux.** `<madcide/plugin_api>` (`tools/madcide/include/madcide/`, shipped as `share/madcide/include/madcide/`, or beside `madcide.exe`) holds the plugin API's version (`MADCIDE_PLUGIN_API`, which `plugin_api_served` and a manifest's `"api"` read), the handler contracts `cmd_handler` / `event_handler` (moved from `madcide_plugins.inc`) and the table `ide_api`: registration (`command`, `view`, `event`) and use (`run`, `command_id`, `get`, `set`). `<madcide/plugin>` adds a plugin's side: `ide::bind` and the `ide::` calls through the table (`set` with `ui::set`'s overloads), the version entry `madcide_plugin_api`, and the declaration of the activation `madcide_plugin_activate(const ide_api *, long w)`, so a definition of another shape is refused when the plugin is built. A manifest's `"code"` names the plugin's source; its library is `<dir>/<name><library_suffix>`. `madcide_plugin_code.inc` owns the table (`madcide_api`, immutable; its `run` posts the command's action event through the one dispatcher, as a bound key does), `plugin_activate` (once per world: the library opened with every reference bound, its version checked, its activation run; a refusal takes back what it registered and closes the library), `plugins_activate` (at session open, after the bundle is selected and before any data loads: the active bundle's code, then each plugin `settings.json`'s `"plugins"` names; a refusal goes to the status line and the session opens without it) and `plugin_build` / `run_build_plugin` (`madcide --build-plugin DIR`: the source built in-process with madcide's include directory, the build's rows printed gcc's way on a refusal). Activation is at load for now: a library's activation costs a `dlopen`, and lazy activation (§5.6) arrives with the `source` transport, where an activation is a compile. Found on the way and fixed in their own commit: two `fulltest` gates step 3e had turned red (a second command call in the client's paste answer, a bare `long` in `<ns_ui_web>`); every synthesized command event now has one builder, `action_event`, gated. Filed: B100 (a `const var &` parameter refuses a text prvalue). Gates: `tests/testmadcide_plugin_library` (builds, activation through `settings.json`, a handler running another command through the table, activation once per world, the four refusals), `testmadcide_cli` (`--build-plugin`'s argv).

     **Recon (2026-10-01), G2 — what the other two platforms need:**
     - **Windows:** the package ships `libmadc-0.dll` (the engine, the twin of `libmadc.so.0`), and `madcide.exe` binds it, so a plugin DLL importing the runtime from it shares the one engine. The PE writer (`third_party/mir/mir-pe.c`) already emits DIR64 base relocations and attributes each import to its DLL (hosted emission probes `params->needed`; a cross build refuses, so DLL tests run in the wine lane under `madc.exe`). A DLL needs the `IMAGE_FILE_DLL` characteristic, an export directory (the defined global symbols, names sorted), and an entry stub that runs the import-addend fixups and the init array at `DLL_PROCESS_ATTACH` and returns TRUE, without the executable stub's UCRT start-up, since the host already ran it.
     - **macOS:** a darwin AOT image of a runtime-needing program is refused until D5 (`libmadc-0.dylib`, built the `libmadc-0.dll` way, with the emit lane naming `@rpath`), and the mac tarball ships no madcide until D5 either. So a plugin library on macOS needs D5 first, then the Mach-O writer's `MH_DYLIB` (no `__PAGEZERO`, `LC_LOAD_DYLINKER` or `LC_MAIN`; an `LC_ID_DYLIB` and an export trie; the ad-hoc signature it already writes).
     - Order: the PE DLL first (its runtime exists), then D5 and `MH_DYLIB`.

     **Built, part 3 (2026-10-01): Windows (G2's DLL).** The PE writer emits a DLL for `madc -shared` (`MIR_object_exec_params.shared_p`): `IMAGE_FILE_DLL`, image base `0x180000000` (moved by its base relocations wherever the loader puts it), an export directory of every defined, named, non-local symbol (the ELF `-shared` dynamic-symbol rule; names in byte order, the loader's binary search), named after the output's basename, and its own entry stub, `pex_dll_stub`: at `DLL_PROCESS_ATTACH` it applies the import-addend fixups, reads the host CRT's argc/argv/envp through three UCRT slots and runs the init array; every reason returns TRUE. A runtime-needing DLL imports the value runtime from `libmadc-0.dll` (`cir_windows_import_dlls`, as an executable does). Found on the way, each fixed in its own commit: the Windows build had been red since 2026-09-28 (an unused interrupt guard in `madc_session_client.cpp`: no backend process on Windows yet, so the guard has nothing to do there), and the plugin registry kept handlers and library handles in `long`, 32 bits on Windows (LLP64), so a plugin library's lookups went through a truncated handle; they are `int64_t` now, the registration verbs taking the typed handler (`cmd_handler` / `event_handler`). Filed: B101 (madc accepts a cast from a pointer to a narrower integer, which g++ and clang++ refuse; it hid the registry's truncation). Gates: `tests/testbuild_shared` and `tests/testmadcide_plugin_library` run under wine (JIT, exe and obj), the latter with a `win64_expect` twin for the `.dll` suffix. The genuine Windows lane runs them at the seam (its channel was down on 2026-10-01).

     **Built, part 4 (2026-10-01): macOS's `MH_DYLIB` (G2's dylib).** The Mach-O writer emits a dylib for `madc -shared`: based at 0 (no `__PAGEZERO`; the rebase stream slides it), no `LC_LOAD_DYLINKER` or `LC_MAIN`, an `LC_ID_DYLIB` naming it `@rpath/<output basename>`, and an export trie of every defined, named, non-local symbol (a compressed prefix tree: siblings never share a first character, so dyld's walk cannot take `_foo` for `_foobar`). Gate: `scripts/macho_dylib_gate.sh` in `make -C src machogate`, both arches: the header, the load commands, the trie's names, and ld64.lld linking a program against the dylib through the trie. Found on the way, fixed in its own commit: `macho_obj_gate.sh` had been red since `--std=madc` became the default whatever the extension (its C fixtures came out mangled, and its legs 4 and 6 look up C names); its C fixtures now compile `--std=c17`. Still waiting for D5: a library that uses the value runtime (every plugin, and any `-shared` image carrying the eval shims) is refused on darwin until `libmadc-0.dylib` exists, so `testbuild_shared` and `testmadcide_plugin_library` keep their darwin skips, now naming D5. D5 is next: the hosted darwin build links `libmadc-0.dylib` (the `libmadc-0.dll` recipe), the Mach-O writer writes the runpath it already receives as `LC_RPATH`s, and the darwin emit lane loads `@rpath/libmadc-0.dylib` where it now refuses.
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
     > **Corrected 2026-10-01 (measured, §8 Stage B item 2's G2 recon):** a plugin's madc code calls the value runtime directly (`madarray_*`, `__madc_fmt_text`, `madc_value_*` are undefined imports of every madc-built shared object), so a plugin library is a shared library that uses the value runtime. It binds to the libmadc the host loaded: `libmadc.so.0` on Linux, `libmadc-0.dll` on Windows (already shipped; `madcide.exe` binds it), and on macOS `libmadc-0.dylib`, the darwin port's D5 (`docs/plans/2026-09-01-darwin-host-port.md`), which does not exist yet. The API table carries madcide's own functions, which nothing exports.
   - Staging: B2's `library` transport lands on Linux, then on macOS and Windows within B2 (the old B6 folds into it), so a plugin library ships on all three platforms before Stage B closes.
3. **madcide is a base; a product built on it is its own program** (owner, 2026-09-30: VS Code is the base, Cursor a product on it).
   - `chthonic` is a product: its own binary, with the chthonic plugin's madc code compiled and LINKED in (the plugin is ordinary madc code, so it is an object like any other; never a textual include of madcide's sources), its bundle's data baked in, and its own identity: its name in `--help` and titles, its own configuration directory (`~/.config/chthonic`), its default bundle. It opens the GUI by default; `--tui` gives the console.
   - One object serves every target: linked into a `.so` / `.dylib` / `.dll`, madcide finds the plugin's registration function through `dlsym` (the `library` transport); linked into madcide it is a built-in; linked into the product, the product's `main` calls `madcide_main(argc, argv, product)` with its descriptor, and the base calls the registration directly.
   - chthonic can live in its own repository, depending on the madc-devel packages: libmadc, madcide's base as a linkable object (a relocatable `.o`; `-shared` gives a `.so` or a `.dll`, and a `.dylib` once G2's `MH_DYLIB` lands), and the plugin API header `<madcide/plugin>` (§5.4). Its code uses only the plugin API and the base's public surface, so it can move out of this tree.
   - Packaging: the madc packages ship libmadc, madc, madcide, the chthonic plugin and a hard link `chthonic` → madcide (the program's name selects the bundle, Stage A part 4); the plugin and the hard link are a subpackage (for example `madcide-chthonic`), which the standalone product replaces, since both install `bin/chthonic`. The standalone chthonic package ships libmadc, madc and the `chthonic` binary.
   - The Microsoft Store carries chthonic only (for now), as "an easy GUI to learn C/C++", through the owner's existing Partner Center presence (packaging arc PK6).
4. **The release line** (owner, 2026-09-30, following the sizing recon):
   - The master release: plan §41.11a steps 0-8, plus the Homebrew tap for macOS and Linux (packaging arc PK6).
   - The release after it, the chthonic product: the product descriptor and `madcide_main`, the base as a linkable object, the chthonic build, the Windows REPL backend (a child of self: chthonic's shell on Windows) and the MSIX for the Store. They ship together: the Store listing needs all of them.
   - First, early: a probe that splits a small piece of madcide into two translation units and links them, since multi-object madc builds are tested only at fixture scale and madcide is one 19.5k-line unit today.
