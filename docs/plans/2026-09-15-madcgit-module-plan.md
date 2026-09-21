# libgit2 is a dependency, not a distribution: the `madcgit` module (V6 seam, slice 0.5)

**Owner ruling, 2026-09-15** (on seeing the C-ABI gate fail at the V6 seam):
*"I don't think libgit2 should be part of the madc distribution... we're not
modifying it... it should just be a dependency for madcide."* This supersedes
the 2026-09-13 ruling that vendored it "the MIR way" (`2026-09-13-nexus-L4-design.md`
§4.1, L4a). MIR is in-tree because madc carries fixes to it; libgit2 has zero
divergence, so it is an ordinary dependency — and it belongs to the IDE's nexus,
not to the language.

**What the gate found.** `check-c-abi-surface.sh` (fulltest, ADR 0003) failed on
`lib/libmadc.so`: 876 `git_*` symbols exported — libgit2's whole C API — because
the archive was linked into every madc image. An embeddable library that exports
a third-party API interposes on any host that links its own copy. No V6 slice ran
this fulltest-only gate; the seam's static pre-run did.

**Arc:** `feature/client-server-views-claude`; this slice rides the V6 seam
battery (bank-before-battery law: a known-open fix in the arc is fixed BEFORE the
battery, so the four lanes run once, on the final shape).

## The shape (the GUI precedent, exactly)

`madcwebview` is the model: a module row in `src/madc_modules.cpp`, an embedded
C interface header the `import` tokenizes, an optional library built by its own
`.mk` against the SYSTEM library, never linked into `libmadc`, loaded lazily on
the first call (`MADC_MODULE_LAZY`), and `madc::module_available` to refuse with
a reason where it is absent.

| piece | before (L4a) | after |
|---|---|---|
| libgit2 | `third_party/libgit2` subtree (133 MB) + `third_party/libgit2-madc` recipe, built into every image | the system library: `libgit2-dev` (apt), `libgit2` (brew); found by `pkg-config libgit2` |
| the ONE libgit2 caller (`madc::GitRepo` + shapers + the `git` source adapter) | `src/madcdis_git_repo.cpp` in libmadc | `src/modules/madcgit/madcgit.cpp` in `lib/libmadcgit.so` |
| the engine face (handle table, value shaping) | `src/madc_git.cpp` + C bridges in `parser.cpp` / `ns_common.h` + `madc::git_*` wrappers in `ns_madc.cpp` | the module's C API `madcgit_*` (`include/madc/madcgit.h`, embedded) |
| the dialect face | `madc::git_open/head/…` declared in `<ns_madc>` | `git::open/head/…` in the fragment `<ns_git>` (`import madcgit;` + thin wrappers), auto-included by the `git::` head |
| gates | `check-one-git-owner.sh` (owner = the engine file), `check-libgit2-features.sh` (network off in the vendored config) | `check-one-git-owner.sh` (owner = the module file, plus the READ-ONLY rule: no remote/clone/fetch/push/write API in the owner); the features gate is deleted (nothing to configure) |
| build | `$(GIT2LIB)` on nine link lines, `-I$(GIT2DIR)/include` in DEFINES | `src/madcgit.mk`: `libmadcgit` when `pkg-config --exists libgit2`; `all` builds it when available; nothing else changes |
| tests | `tests/testgit.mad` on `madc::git_*`; `tests/unit/test_gitrepo.cpp` linked with every engine object | `testgit.mad` on `git::*`; `test_gitrepo` built by `madcgit.mk` against the module objects (filtered out of the generic unit list) |
| the nexus PAST verbs | call `madc::git_*`; an absent repository = handle 0 | call `git::*` behind `madc::module_available("madcgit")`; an absent MODULE = handle 0 — the same degradation the design already has for "no repository" |
| Windows / macOS | libgit2 cross-built from the subtree into the PE / Mach-O images | not built on the cross lanes (no libgit2 for the mingw / darwin targets on the container); `testgit`, `testgraphpast`, `testnexus_records` gain `.win64_skip`. Follow-up (packaging order: Linux first): fetch a prebuilt libgit2 for those targets the way `fetch_webview2_sdk.sh` fetches the WebView2 SDK |

## Rulings (defaults; the owner can veto any in a word)

1. **Namespace `git::`**, not `madc::git_*`. The module is not the engine; every
   module and polyglot library owns its namespace (`php::`, `ui_web::`). Nine
   call sites in `madcide_past.inc` and the test move.
2. **The C boundary carries `const char *` and `void *` (a `madc::value *`)** —
   the shape the C bridges already had; the dialect wrappers pass `&out`.
3. **The module resolves libmadc's symbols at load** (bin/madc exports
   `madc::value`, `detail::set_channel_error`, `canonical_path_for_compare`;
   verified with `nm -D`). Linux: undefined at link, bound at dlopen.
4. **`all` builds the module when libgit2 is present** and says nothing when it
   is not — the suite needs it on Linux, the same way the gui stage needs
   WebKitGTK; `provision_container.sh` gains `libgit2-dev`.
5. **The subtree goes** (`git rm -r third_party/libgit2 third_party/libgit2-madc`);
   history keeps it. The trailer gate's subtree exemptions stay harmless.

## Tasks

- T1 the module source (`git mv` the owner; fold the face; the C API)
- T2 engine surgery (parser / ns_common / ns_madc / lexer / module row)
- T3 the dialect face (`madcgit.h`, `<ns_git>`, past.inc, testgit)
- T4 build (Makefile, `madcgit.mk`, provisioning, packaging)
- T5 gates (owner + read-only rule; delete the features gate)
- T6 the subtree removal
- T7 fixtures (`.win64_skip` ×3)
- T8 docs (L4 design §4.1 superseded, ADR 0003, madcide.md)
- T9 container: `libgit2-dev`, `remote_build sync build test`, the git/nexus/madcide
  groups, `check-c-abi-surface`, `pull`
- T10 KG (the Decision) + status; then the seam battery

## As landed (2026-09-15)

T1–T9 shipped as planned, two commits: the module (engine surgery, `<ns_git>`,
`madcgit.mk`, gates, fixtures, packaging, docs) and the subtree removal. On the
container (libgit2 1.7.2 from apt): `check-c-abi-surface` OK with **0** `git_*`
exports from `libmadc.so`; `libmadcgit.so` exports its 10 entry points and binds
13 engine symbols at load; `test_gitrepo` 186/186; `testgit`, `testgraphpast`,
`testnexus_*`, `testmcpclient`, `testmadcide_serve_*`, `_attach`, `_discover`
green in the JIT pass, and `testgit` / `testgraphpast` / `testnexus_records` in
the native-exe pass — the lazy module rides `__madc_module_deps` into a native
program exactly as the GUI module does. One thing the execution added: the
module lives one directory down, so its compile line carries `-I.` for the
engine's src-local headers (the error composer, the path canonicalizer).
