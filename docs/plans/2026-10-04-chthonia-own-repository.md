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
- Its tests: `tests/testmadcide_chthonia.*`, `tests/gui/madcide_chthonia.mad`,
  and any other test that exists only for the bundle.
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

## 4. Packaging in the Chthonia repository

- **Linux**: `.deb` (one per supported Ubuntu release, as madc's), `.rpm`,
  and a tarball. The `.deb`/`.rpm` depend on madc's runtime package
  (libmadc); the tarball carries it.
- **Windows**: the zip, and **MSIX** — package identity, the
  `AppxManifest.xml`, the signing certificate, the Evergreen WebView2
  runtime as a declared dependency, Start-menu entry and file associations
  (`.c`, `.cpp`, `.h`, `.mad`). The package carries `libmadc-0.dll` beside
  `chthonia.exe` (PE binding is adjacency).
- **macOS**: `Chthonia.app` with its `.icns`, in a `.dmg`; notarization when
  the signing identity exists.
- **Notices**: a Chthonia package that bundles libmadc ships madc's licence
  and every third-party notice madc's packages ship (webview, zstd,
  cmark-gfm, the GCC runtime and mingw-w64 on Windows, libc++ on macOS),
  checked by the same shipped-notices gate madc uses (chthonia plan §7d).
- **Version**: Chthonia's own (`chthonia_version.h`), independent of madc's;
  Help ▸ About shows both.

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
  `.git/info/exclude`, which is never committed.
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
   Plus B30 (`std::map` brace initialization), and §3 here: `libmadcide`,
   the installed harness headers, the minimum-version contract.
2. **The external build, proved inside madc.** In-tree Chthonia builds as
   the repository will: linking `libmadcide` against an INSTALLED madc
   (`stage_install.sh`), its tests through the installed harness headers —
   never `madcide_base.mad` source. Its three-platform packaging (§4) is
   written now as files under `tools/chthonia/`, ready to move.
3. **The Chthonia repository, prepared.** The sanitized initial commit (§5)
   built from those files; its CI installs madc from a madc release's
   assets and builds, tests and packages on Linux, Windows and macOS.
4. **The cut, in madc.** The §2 "Moves" files leave; packagers, the install
   gate and the documentation stop naming Chthonia. `VERSION` = 0.102.0.
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
   packages require madc ≥ 0.102.0 (`.deb`/`.rpm` Depends, the Homebrew
   formula's `depends_on "madc"` — the tap update after madc's formula
   lands, owner-gated as the tap is).
8. MSIX and the macOS `.app`/`.dmg`, in the Chthonia repository, as point
   releases if they are not ready for step 7.

## 7. Settled (owner, 2026-10-04)

- Licence: MPL-2.0, madc's.
- Visibility: the same as madc's (public).
- After the cut madc does not include, distribute or package Chthonia in any
  form; there is no transitional madc release carrying it. Chthonia depends
  on madc, and madc does not know Chthonia.

Thread contract: none at run time — this plan moves files and packaging; no
runtime state is added.
