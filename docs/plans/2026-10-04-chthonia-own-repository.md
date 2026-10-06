# Chthonia in its own repository — plan (2026-10-04)

## 1. The owner's direction

- 2026-09-30 (madcide plugins arc): madcide is the base, and a product built
  on it (Chthonia) has its own build and can live in its own repository,
  depending on the madc-devel packages. The mechanism was left open.
- 2026-10-04: the split is agreed, cut at Chthonia's next release, with the
  boundary prepared now. The Chthonia key style stays in madc's base
  profiles. Chthonia's packaging, MSIX included, lives in the Chthonia
  repository. And: "we need for the chthonia repo to be clean of all the LLM
  identification … I'd like to try to keep Chthonia sanitized with that
  respect … while I have no issue with this for the madc project itself"
  (§5).

## 2. The boundary

Chthonia's code already depends only on madcide's public surface: its two
sources include `<madcide/product>` and `<madcide/plugin>` and nothing else,
and both headers ship in `share/madcide/include`. The one tie that is not
public is the build: `tools/chthonia/chthonia.json` compiles madcide's whole
base from source (`../madcide/madcide_base.mad`).

**Moves to the Chthonia repository:**
- `tools/chthonia/` — the product: `chthonia_main.mad`, `chthonia.json`, the
  icon, the desktop entry, `chthonia_version.h`.
- `tools/madcide/plugins/chthonia/` — the bundle: plugin manifest, menu,
  layout, the Variables view's code.
- Chthonia's help topics (chthonia plan §7d).
- Its tests: `tools/chthonia/tests/` (`bundle.mad` and `gui/window.mad`,
  moved there from `tests/testmadcide_chthonia.*` and
  `tests/gui/madcide_chthonia.*`), and any other test that exists only for
  the bundle.
- Its packaging (§4) and its install-gate probes (Linux 6 and 7, macOS 1c
  and 1d in `scripts/package_install_gate.sh`).
- Its user documentation (the Chthonia passages of `README.md`,
  `docs/madcide.md`, `docs/man/madcide.1`, and the package READMEs).

**Stays in madc:** every base capability, whichever product drove it —
editing commands, project and build machinery, the Help system and the
Markdown module, the plugin API, and all key styles including Chthonia's.
The test for a feature: if a madcide user would want it, it is base.

## 3. What madc ships first (madc-devel)

1. **madcide's base as a library**: `libmadcide` (static and shared)
   exporting `madcide_main(argc, argv, product)`, so a product links the
   base instead of compiling its source. The build manifest grows a library
   reference where it names `madcide_base.mad` today.
2. **The product test harness**: `IdeSession` and the drive helpers
   (`madcide_repl_drive.inc` and the event builders) as installed headers,
   so a product's tests run against an installed madc.
3. **A version contract**: the plugin API already carries `"api": 2`; a
   product's build manifest also names the minimum madc version, and the
   build refuses an older one with the reason.
4. **The boundary gate, now**: a check in `fulltest` that the files listed in
   §2 under "Moves" include only madcide's public headers, so the boundary
   cannot erode before the cut.

### 3a. Designed against the code (2026-10-05)

What exists (survey at 652cbf4ba):
- `-shared` already builds madc units into a shared library in-process
  (`build_native(.., "shared", ..)` → `madc_cir_emit_native`, kind
  `mnkShared`; madcide's `--build-plugin` uses it). MIR writes the image: its
  DT_NEEDED is `libmadc.so.0`, then the runtimes, then the `-l` spellings, and
  its RUNPATH is `$ORIGIN/../lib` plus madc's libdir. Every defined global is
  exported (no visibility control). References inside the image resolve when
  it is emitted — no PLT — so a product cannot interpose on the base's own
  calls; exporting the base's ~1000 functions is untidy, not unsafe.
- `-l<name>` (or a path) already links a library into `--project` builds: the
  native closure (`madc_project_emit_native` joins the CLI's `-l` with the
  TUs' `module_link_libs`) and the JIT (`-l` libraries `dlopen`ed
  `RTLD_GLOBAL` before the run). `bin/madc` binds the engine as
  `libmadc.so.0`, so a JIT-run product and a loaded `libmadcide` share one
  engine, as plugin libraries already do (`testmadcide_plugin_library`).
- No manifest key names a library; there is no `-L`; madc writes no static
  archive (`-c` and `-r -o x.o` are its only relocatable forms).
- The base finds its data through the running binary
  (`resolve_data_dir`: `<exedir>/../share/madcide/<sub>`, then beside the
  exe), so a product installed beside madc finds madcide's data.
- `madcide_main` is the base's one entry (`madcide_base.inc`), a C++ name;
  a product's plugin calls the base only through the `ide_api` table it is
  handed, never by symbol.
- Chthonia's tests include the base's SOURCES
  (`madcide_core.inc` + `madcide_repl_drive.inc`) and call its internals
  (`setup_editor`, `builtins_activate`, `es_int`, `repl_doc_if_any`, …).

Slices:
1. **A manifest names the libraries it links:** `"libs": ["madcide"]` (a name
   spelled by `madc_module_library_spelling`, or a path), read by
   `read_project_manifest`, joined to the native closure beside `-l`, and
   opened before a JIT run the way `import` opens a module (madc's `../lib`
   first). Gate: a project linking a madc-built shared library, run JIT and
   built `--exe`.
   **Done (2026-10-05):** each entry binds on the first TU through
   `Program::bind_module_namespace` (link form), the binder `import` and
   `#load` use. The binder now reports a library that does not open, and
   each caller words the refusal. A build opens the library into the
   building process, as `import` does. Gate: `tests/testproject_libs`.
2. **`libmadcide` built and installed:** `madcide_base.mad` built `-shared`
   into `lib/libmadcide` (the platform's spelling) by the build and the
   packagers, staged into `<libdir>` by `stage_install.sh`; `chthonia.json`
   names `"libs": ["madcide"]` and drops the base unit; the boundary gate's
   one exception goes. The static form: madc has no archive writer, so the
   static base is the one relocatable object `-r` writes, linked as a unit —
   the archive format is not needed for madc's own link.
   **Shared form done (2026-10-05):** every packager builds libmadcide with
   its release madc into the directory that madc searches first (`lib/` on
   Linux and macOS, the release set beside `madc.exe` on Windows) and ships
   it beside libmadc; `chthonia.json` names `"libs": ["madcide"]`, and the
   boundary gate has no exception. Measured on Linux: the library builds in
   2.4 s (2.3 MB, 981 functions exported, no SONAME, so chthonia's image
   names `libmadcide.so`), and chthonia then builds in 0.12 s. Linking it
   showed both images registering the web host: `register_host` now
   compares an `identity` the host states (`testuiwebtwoimages`). Measured
   under wine with the release set: `madcide.dll` builds in 1.5 s (1.9 MB)
   beside `madc.exe`, chthonia.exe builds against it in 0.17 s and imports
   `madcide.dll`, and `--help` prints.
   **Static form verified (2026-10-05):** madcide's base compiled to one
   object (`madc -c`, 12 s) links with the product's units' objects
   (`madc -o chthonia base.o plugin.o main.o`, 0.19 s) into a 2.3 MB image
   that needs only libmadc and the system libraries. It first refused a
   duplicate `ui_web::__last_host`: a fragment's namespace-scope variable
   is now `inline` (`testfragmentobjects`, gated by
   `check-fragment-inline-vars.sh`).
3. **`<madcide/harness>`:** `IdeSession` (the class definition moves out of
   `madcide_core.inc` into the header, which the base includes — one
   definition, so the layout cannot drift) and the drive helpers
   (`madcide_repl_drive.inc`'s event builders, `settle`, `enter_line`) plus
   the base functions product tests call, declared there and defined in the
   base. Installed in `share/madcide/include/madcide/`. Chthonia's tests
   include it and link `libmadcide` instead of the base's sources.
   **Done (2026-10-05):** three headers. `<madcide/vocabulary>` holds the
   codes a product names (`ide_cmd`, `ide_pane`, `ide_slot`, `ide_view`;
   `madcide_enums.inc` includes it). `<madcide/session>` holds `IdeSession`
   (`madcide_core.inc` includes it). `<madcide/harness>` includes both and
   declares the 29 base functions product tests call; its drive helpers
   are `inline`, taken from `madcide_repl_drive.inc`, which is gone. The
   six madcide tests that used the drive file include the harness.
   `check-madcide-harness.sh` holds each declaration to a definition in
   the base. `testmadcide_harness` builds libmadcide, then runs a product
   that includes only the harness and links the library through its
   manifest. That product first failed: the auto-include scan read every
   angle include as a system header. It now classifies by the resolved
   path, as gcc and clang do (`testautoincludeangle`). The staging script
   already installed every header in the directory.
4. **The version contract:** the manifest's `"madc": "0.102.0"` is the
   minimum; a build by an older madc refuses with both versions.
   **Done (2026-10-05):** `read_project_manifest` reads `"madc"` (VERSION's
   form, `major.minor.patch`, compared field by field as numbers). An older
   madc refuses before it parses anything: `this project needs madc X or
   newer (this is madc Y)`. A value of any other form refuses too. Every
   manifest consumer reads through it: `madc --project`, `project_run`
   (which now prints a refusal's reason on stderr instead of returning -1
   silently), `project_build`'s rows and the run child. A madc from before
   the key ignores it, as it ignores any key it does not know. Gates:
   `tests/unit/test_project_manifest.cpp`, `tests/testproject_madcversion`.
   The tree's VERSION reads 0.101.0 until the release commit, so the key
   goes into Chthonia's manifest in its own repository at the cut, not
   into the in-tree `chthonia.json` (built by this madc, always the same
   version).
5. **The boundary gate:** `check-chthonia-boundary.sh` with no exception —
   Chthonia's manifest names no base source, and its tests include only
   installed headers.
   **Done (2026-10-05):** Chthonia's tests are in its own directory
   (`tools/chthonia/tests/`, §6 step 2), so the gate's include rule covers
   them, and a new rule fails any file of the moving set (sources, scripts,
   fixtures) that names madcide's sources (`tools/madcide/`,
   `madcide_base`, `madcide_core`). Running them showed two codes a test
   names that only the base declared, the plugin transport and a menu
   row's placement; they moved into `<madcide/vocabulary>`, and `ev_code`
   joined the harness. It also showed that the command line's `-l` skipped
   madc's own lib directory, which `import` and `"libs"` search first:
   `-l` and a frozen forest's libraries now open through
   `madc_module_open` (`tests/testlink_selflibdir`, gated by
   `check-one-library-opener.sh`).

Thread contract: none new — `libmadcide` is the base's code, whose contract
(the session's thread) is unchanged; the manifest's libraries are opened once,
before the program runs.

## 4. Packaging in the Chthonia repository

- **Linux**: `.deb` (one per supported Ubuntu release, as madc's), `.rpm`,
  and a tarball. The `.deb`/`.rpm` depend on madc's package; the tarball
  unpacks into madc's prefix (the install layout below).
- **Windows**: the zip, and **MSIX** — package identity, the
  `AppxManifest.xml`, the signing certificate, the Evergreen WebView2
  runtime as a declared dependency, Start-menu entry and file associations
  (`.c`, `.cpp`, `.h`, `.mad`). The zip unpacks into madc's folder, where
  `libmadc-0.dll` and `madcide.dll` already sit beside `madc.exe` (PE
  binding is adjacency). MSIX bundles everything (owner, 2026-10-05): it is
  sandboxed in its own folder, so it carries madc's runtime (`libmadc-0.dll`,
  `madcide.dll`) and madcide's data beside `chthonia.exe`.
- **macOS**: `Chthonia.app` with its `.icns`, in a `.dmg`; notarization when
  the signing identity exists.
- **Notices**: a Chthonia package that bundles libmadc ships madc's licence
  and every third-party notice madc's packages ship (webview, zstd,
  cmark-gfm, the GCC runtime and mingw-w64 on Windows, libc++ on macOS),
  checked by the same shipped-notices gate madc uses (chthonia plan §7d).
- **Version**: Chthonia's own (`chthonia_version.h`), independent of madc's;
  Help ▸ About shows both.
- **Installed into madc's prefix (owner, 2026-10-05):** Chthonia requires a
  madc installation (libmadc, madc, madcide) and installs along with it; it
  is never installed without the base madc package. madcide's base finds
  its verbs, checks, profiles and shipped plugins through the running
  executable (`resolve_data_dir`: `<exedir>/../share/madcide/<sub>`, then
  `<exedir>/<sub>`), so chthonia beside madc finds them unchanged, and
  Chthonia's bundle installs as a shipped plugin
  (`share/madcide/plugins/chthonia`), where the plugin search path already
  looks. The deb/rpm/tarball install into madc's prefix and depend on madc's
  packages; the Windows zip unpacks into madc's folder. MSIX is the one
  format that cannot install into another package's location, so it
  bundles everything (Windows, above). Chthonia has no Homebrew keg: its
  Linux packages are the distribution-specific `.deb`/`.rpm` (owner,
  2026-10-05).

## 5. A sanitized repository (owner, 2026-10-04)

The Chthonia repository carries no identification of AI assistance, in any
form. madc is unaffected.

- **History**: the repository starts from a new initial commit. madc's
  history is not imported (its commits carry assistant attribution).
- **Commit messages**: plain prose under the owner's authorship. No
  co-author line naming an assistant, no session link, no "generated with"
  line, and none of madc's rule trailers (Hypothesis/Layer/Searched/Oracle
  are madc's process). An assistant working there commits with the owner's
  identity and a plain message.
- **Files**: no agent instruction files in the tree — no `AGENTS.md`,
  `CLAUDE.md`, `GEMINI.md`, `.claude/`, `.cursor/`, `.windsurfrules`,
  `.aider.conf.yml`, `.github/copilot-instructions.md`. Guidance for working
  on Chthonia lives in madc (this plan, and a madc doc for the Chthonia
  workflow); any local agent file in a Chthonia checkout is excluded through
  `.git/info/exclude`, which is never committed. A checkout carries a local
  `.claude/settings.json` with `"includeCoAuthoredBy": false` (owner,
  2026-10-05), excluded the same way, so an assistant there adds no
  co-author line (`chthonia_export.sh` writes both).
- **Content**: comments and documentation read as ordinary project text —
  no dated owner rulings ("owner 2026-09-30"), no references to madc plan
  sections or sessions, no assistant or tool names. Every file moved in §2
  gets this pass before its first commit there.
- **Gate**: a check script kept in madc (`scripts/chthonia_sanitize_check.sh`),
  run against a Chthonia checkout before every push: it fails on a commit
  message or a tracked file carrying assistant identification. It lives in
  madc so the Chthonia tree holds no list of what it is avoiding. A local
  `commit-msg` hook (`.git/hooks`, uncommitted) runs the message half on
  each commit.
  **Written (2026-10-05):** with no argument it checks madc's moving set
  (`tools/chthonia/`, `tools/madcide/plugins/chthonia/`) and runs in
  `make gates`, so a Chthonia file cannot gain such text before the move;
  `--checkout DIR` checks a Chthonia checkout (its tracked files, the agent
  files it must not track, every commit message); `--message FILE` is the
  hook's half. The moving set's comments had their pass the same day.

## 6. Order: madc v0.102.0, then Chthonia's release right behind it

The owner (2026-10-04): madc v0.102.0 is the clean divide — every change and
fix since v0.101.0, B30 fixed, and no Chthonia — and it is the release the
Chthonia repository depends on; Chthonia's release follows "very soon after"
madc's master release. No transitional release: no madc release has ever
packaged Chthonia (v0.101.0's packaging names it nowhere; its packaging in
`package_release*.sh`, the Homebrew formula and the install gate came after),
so nobody loses it on upgrade and no package files overlap. A transitional
release would be the FIRST to ship it.

The dependency runs one way (Chthonia → madc), so there is no chicken and
egg. The one ordering constraint: Chthonia's CI needs madc v0.102.0's
packages before madc's tag exists. `release.yml`'s `workflow_dispatch` (`tag`
+ `build_ref`) already meets it: it builds every platform's packages from any
commit into a DRAFT release, and a draft creates no tag until it is
published.

1. **madc v0.102.0's scope lands on develop.** Every base capability
   Chthonia's first release needs, since after the cut Chthonia can use only
   what a madc release ships: the rest of chthonia plan §7f (`.` prefix,
   `%build`, the IDE layer, `%git`), §7d (Help and Markdown on cmark-gfm),
   §7e stage 1 (Git), and §7a (recent files, font size, Full screen,
   Interrupt/Send EOF, the Windows console). The debugger arc is after.
   Plus B30 (`std::map` brace initialization) — DONE 2026-10-04 (e6d7b05a1,
   with the copied-reference-argument fix 49ace4461 under it and B170's
   `std::map<std::string, T>` insert / brace-init crash, 55cb04451) — and §3
   here: `libmadcide`, the installed harness headers, the minimum-version
   contract.
2. **The external build, proved inside madc.** In-tree Chthonia builds as
   the repository will: linking `libmadcide` against an INSTALLED madc
   (`stage_install.sh`), its tests through the installed harness headers —
   never `madcide_base.mad` source. Its three-platform packaging (§4) is
   written now as files under `tools/chthonia/`, ready to move.
   **Designed (2026-10-05):**
   - Chthonia's tests move under `tools/chthonia/tests/`: `bundle.mad`
     (from `tests/testmadcide_chthonia.*`) and `gui/window.mad` (from
     `tests/gui/madcide_chthonia.*`), with their fixtures. Each includes
     `<madcide/harness>` and none of the base's sources; their comments get
     the §5 pass as they move.
   - `tools/chthonia/scripts/build.sh` builds Chthonia with a given madc:
     `madc -I <madcide include dir> --project chthonia.json -o <out>`. A
     command-line `-I` reaches every unit of a project (2026-10-05), so the
     manifest names no include directory; the installed one is
     `<prefix>/share/madcide/include`, the in-tree one
     `tools/madcide/include`.
   - `tools/chthonia/scripts/run_tests.sh` runs each test with
     `madc -I <include dir> -lmadcide`, reading `.env`, `.expect` and
     `.timeout` the way madc's runner does, and the window tests under
     `xvfb-run`. It is Chthonia's own, since its repository has no madc
     scripts.
   - madc's lane `scripts/chthonia_lane.sh` (ledger row `chthonia`)
     builds `lib/libmadcide` with the madc under test and runs both scripts
     against it and `tools/madcide/include`. `--installed` runs them
     against an installed madc instead: the relocatable tarball
     `package_release.sh` writes, unpacked, as Chthonia's CI will. Tier 1
     for a change to madcide or Chthonia; the seam battery runs it on the
     packed release binary; the installed form runs on a release's
     packages (step 5's candidate).
   - `tests/testmadcide_product` stays in madc (the product descriptor is
     base). It links Chthonia's plugin as its example product, so at the
     cut (step 4) it moves to a product of its own.
   **Packaging written (2026-10-05):** `tools/chthonia/scripts/`:
   - `common.sh`, which each script sources: the madc (a command), madcide's
     headers beside it (`share/madcide/include`, or `include` beside
     `madc.exe`), the bundle's directory (read from `chthonia.json`'s
     plugin unit, so the move changes the manifest alone), the checksum
     file's lines, and the modes a package installs.
   - `stage.sh linux|macos|windows ROOT`, the one staging: the program, the
     bundle as one of madcide's shipped plugins (`share/madcide/plugins/
     chthonia`, `bin\plugins\chthonia` on Windows) with its library built
     by the madcide beside madc, the desktop entry and icons (Linux), the
     licence (`tools/chthonia/LICENSE`, MPL-2.0).
   - `package_linux.sh`: the `.deb` and `.rpm` require the madc that built
     them or newer (chthonia links libmadc and libmadcide) and WebKitGTK 6.0
     and GTK 4 (a window is its default); the tarball unpacks into a madc
     folder. Each is checked to hold exactly the staged files, each
     readable by every user.
   - `package_windows.sh` (the zip for madc's folder, with a Windows madc),
     `package_macos.sh` (the tarball for a madc folder, on a Mac).
   - `check_install.sh PREFIX`: madc's install-gate probes 6 and 7 for an
     installed Chthonia, plus its bundle: the menu bar has no Window menu
     [control: the bundle hidden, the default profile's is listed].
   - `run_tests.sh` finds the bundle where it is, through
     `MADCIDE_PLUGIN_PATH`, a base capability: directories the plugin
     search path takes between the user's and the shipped
     (`testmadcide_bundles` pin 11), since after the cut an installed madc
     carries no Chthonia bundle. `ico_png_images.py` moved in: only
     Chthonia's packaging uses it.
   The lane runs `package_linux.sh` in both forms; the installed form
   installs the tarball into the unpacked madc folder, every path it
   carries removed first, and runs `check_install.sh` there. Measured under
   wine: `package_windows.sh` against a madc folder made from the release
   set (its own copy of the bundle removed) wrote the zip, `chthonia.exe`
   (399 KB) and the bundle with `chthonia.dll` built by `madcide.exe`;
   unpacked into the folder, `chthonia.exe --help` prints its usage and its
   menu bar is the bundle's. MSIX, the `.app` and the `.dmg` remain step 8.
3. **The Chthonia repository, prepared.** The sanitized initial commit (§5)
   built from those files; its CI installs madc from a madc release's
   assets and builds, tests and packages on Linux, Windows and macOS.
   **Prepared (2026-10-05):** `scripts/chthonia_export.sh [--madc VER] DIR`
   makes the repository from the moving set: `tools/chthonia/` at its top,
   the bundle at `plugins/chthonia/`, `chthonia.json` naming that unit and
   `"madc": VER` (default `VERSION`; the cut runs it with `--madc 0.102.0`
   before deleting the set, since `/release` bumps `VERSION` after the
   cut); one commit under this checkout's git identity with a
   plain message; agent files excluded through `.git/info/exclude`; the
   local `.claude/settings.json` (§5), which git must report ignored; a
   `commit-msg` hook running `chthonia_sanitize_check.sh --message`; then
   `--checkout` must pass. Its CI is `tools/chthonia/.github/workflows/ci.yml`
   (inert in madc): the madc release named by `chthonia.json` (or a
   dispatch's `madc_tag`; a draft reads with the `MADC_RELEASES_TOKEN`
   secret) installed from its assets on ubuntu-24.04, windows-latest,
   macos-14 and macos-15-intel; build, tests (the window tests under
   xvfb-run on Linux), packages, and on Linux and macOS the tarball
   installed into the madc folder and `check_install.sh`; a `v*` tag drafts
   the release. `tools/chthonia/README.md` is its user documentation.
   `scripts/chthonia_lane.sh --installed TARBALL --exported` runs that Linux
   job's steps from an export: green on the container. The repository's
   creation on GitHub, the first push and the secret are the owner's.
4. **The cut, in madc.** The §2 "Moves" files leave; packagers, the install
   gate and the documentation stop naming Chthonia. `VERSION` = 0.102.0.
   What leaves madc's packaging has its replacement in
   `tools/chthonia/scripts/` already: `stage_install.sh`'s program, desktop
   entry and icons (`stage.sh`), the packagers' chthonia builds
   (`package_*.sh`), install-gate probes 6 and 7 (`check_install.sh`), and
   `scripts/chthonia_lane.sh` (Chthonia's CI runs its scripts).
   Tier 3 at this seam, then every platform lane's full suite
   (`lane_ledger.sh check --release`).
5. **The candidate.** `release.yml` dispatched with `tag: v0.102.0` and
   `build_ref:` the cut commit: the draft release v0.102.0 holds every
   package. Chthonia's CI installs madc from the draft (a read token for
   madc's drafts, a repository secret) and runs green on three platforms.
   A defect found here is fixed in madc and the dispatch re-run — the tag
   still does not exist.
6. **madc v0.102.0.** Promote develop → master at the candidate commit and
   push the tag: the release builds the same commit Chthonia tested, and the
   draft is published.
7. **Chthonia's release, the same day.** Its CI re-runs against the
   published packages (the bytes it already tested), then its tag. Its
   packages require madc ≥ 0.102.0 (the `.deb`/`.rpm` Depends).
8. MSIX and the macOS `.app`/`.dmg`, in the Chthonia repository, as point
   releases if they are not ready for step 7.

## 6a. The owner tests every environment first; Ubuntu 22.04 too (owner, 2026-10-05)

"Before we push anything I want to test on all three environments, and this
also means that the Ubuntu 22.04 version needs to be built so I can test it
on my WSL." Nothing is pushed — develop, master, a tag, the Chthonia
repository — until the owner has tested the built packages on Linux,
Windows and macOS and says go. Steps 5–7 above follow that test.

**One .deb per Ubuntu release** (owner, 2026-10-04), built ON the release:
its glibc and libstdc++, and its own headers in the forest. On Ubuntu the
revision is `<rel>~ubuntu<VERSION_ID>` for madc and Chthonia alike
(`madc_0.102.0-1~ubuntu22.04_amd64.deb`): an older release's package sorts
below a newer one's, so a distribution upgrade upgrades it, and Chthonia's
`madc (>= 0.102.0)` holds for each. Each Depends is what the binaries link
at the build host's versions (`dpkg-shlibdeps`), so a package for a newer
release never installs on an older one. `package_release.sh --deb` and
`package_linux.sh --deb` build the .deb alone; the rpm and the tarballs
come from 24.04. `release.yml` gains `linux-packages-jammy`, Chthonia's CI
`linux-jammy` (ubuntu-22.04 runners). Locally the 22.04 packages are built
in a debootstrap jammy root on the build container (`/workspace/jammy`).

**Measured in the jammy root (2026-10-05):** madc builds clean with g++ 11
(`-Werror`), C programs run, the modules (madcgit, madcmark, madcwebview)
build. C++ needed fixes:
- glibc 2.35's `bits/floatn.h` typedefs `_Float128` (from `__float128`)
  and the other `_FloatN` names for every C++ compile, since it predates
  g++ 13. madc announces the host's g++ (11 there) but had g++ 13's C++
  built-ins and no `__float128`. Fixed: a `_FloatN` spelling is a C++
  built-in only from the g++ that added it (`_Float16` 12, the rest 13 —
  `Program::floatn_keyword_active`), and `__float128` is a type; every
  current lane (g++ 13, mingw 13, clang) is unchanged. Reducer
  `tests/testfloatngxx`.
- libstdc++ 11 reaches madc C++ defects that libstdc++ 13 does not. Each was
  a general defect, reduced to a test that fails on 24.04 too and fixed there
  (8c65eb4aa … b14b827f4, f29b0db8f, 87e86edc1, efaf8a592): template
  instantiation (bindings, incomplete arguments, specializations, dependent
  callables, destructor bodies), const objects baked only from constant
  initializers, an opaque derived from a template parameter keyed by its
  (name, index) — the `std::vector<std::string>` reallocation crash — and an
  unevaluated call taking its selected overload's type (libstdc++'s
  `__niter_base(reverse_iterator)`).
- The tests/ suite (JIT) in the root: 405 failing at first; 49 at b14b827f4
  (32 of them pin `-stdlib=libc++`, which the root lacks — the runner now
  skips a test pinning a flavor its madc was not built with, d481e70bd);
  16 + 1 timed out at cacbd8069; 1959 passed, 0 failed, 1 timed out at
  32b3eba45 — the timeout testmadcide_plugin_host's purposeful plugin
  crashes, whose ~330 MB cores a piped core_pattern collected (no core now,
  efaf87abc); 1960 passed, 0 failed, 0 timed out at efaf87abc; 1991 passed,
  0 failed, 0 timed out, 41 skipped at 22deed106 (after the c2mir-tests
  fixes; 24.04 at the same content: 2023 passed, 9 skipped — the 32 more
  skips are the tests pinning `-stdlib=libc++`). Two tests leaned on their caller: testfloatncomplex (C
  content, now `--std=gnu11`) and testrepl_terminalkeys (TERM, now pinned).
- The release build's forest pack: libstdc++ 11's `<algorithm>` reaches
  `<functional>` (13's does not), so the 22.04 pack holds `std::function`'s
  internals, and the pack gate refused it — `_Nocopy_types` (a pointer to
  member function of a never-defined class) and `_Any_data` (a union with
  member functions) had no restorable record, so `_Function_base` dropped.
  Both were general forest defects, each reduced in `forest_bind_gate.sh`
  and fixed (8f91aaab0 pointers to members, 5ea9971ef the union-layout
  class). Then: closure drops 0, fill drops 0, `package_release.sh --deb`
  rc 0, the .deb's install gate PASS, Depends `libc6 (>= 2.34), libgcc-s1
  (>= 4.7), libstdc++6 (>= 11), libzstd1 (>= 1.4.0), zlib1g (>= 1:1.2.0)`.
- GTK 4.6 has no `GtkFileDialog` (4.10): the file dialogs fall back to
  `GtkFileChooserNative` there (`madcwebview_chrome.cc`), chosen at compile
  time.

## 7. Settled (owner, 2026-10-04)

- Licence: MPL-2.0, madc's.
- Visibility: the same as madc's (public).
- After the cut madc does not include, distribute or package Chthonia in any
  form; there is no transitional madc release carrying it. Chthonia depends
  on madc, and madc does not know Chthonia.

Thread contract: none at run time — this plan moves files and packaging; no
runtime state is added.
