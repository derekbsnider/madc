# Plan: Integrate the MADC MIR Fork as a Git Subtree

**Status (2026-08-11): EXECUTED, same day.** Drafted by the owner;
adopted with the session-#81 review notes integrated; executed on the
owner's go and shipped as **v0.77.0** (develop @90508122; merge
@713bf0f3). Every acceptance criterion in §29 is met — see ADR 0002
(`docs/adr/0002-mir-subtree-integration.md`) for the standing policy
and `CHANGELOG.md` [v0.77.0] for the validation record (fulltest
1019/0 twice including a mir-less fresh clone, torture byte-identical
to pre-migration, stale-checkout negative control, Mac battery 8/2 of
10 on the v0.77.0 tarball). This document remains as the execution
playbook and the §16–§20 upstream-workflow reference.

## Objective

Move the MADC-specific MIR/c2mir downstream from the separately maintained repository:

```text
derekbsnider/mir
```

into the MADC repository as:

```text
derekbsnider/madc/third_party/mir
```

using **Git subtree semantics with preserved history**.

After the migration:

* `madc/third_party/mir` is the **only canonical maintained MADC-specific MIR downstream**.
* Normal MADC development may modify `third_party/mir` directly as ordinary MADC source.
* `vnmakarov/mir` remains the true upstream MIR project.
* `derekbsnider/mir` is no longer maintained as the MADC downstream.
* `derekbsnider/mir` remains available primarily as:

  * historical record of the former downstream;
  * a GitHub fork through which clean upstream MIR/c2mir bug-fix branches can be submitted to `vnmakarov/mir`.
* MADC no longer requires a separately checked-out `/workspace/mir`.
* MADC no longer requires `MIR_COMMIT` or synchronized MADC/MIR branch and release management.

The migration must be **behaviorally neutral**. Repository topology and build wiring change; compiler behavior must not.

---

# 1. Background

MADC originally depended on asmjit for JIT code generation.

It later moved to MIR/c2mir and began using a fork of:

```text
https://github.com/vnmakarov/mir
```

maintained at:

```text
https://github.com/derekbsnider/mir
```

The MADC MIR downstream has since diverged substantially from upstream and contains functionality and fixes required specifically by MADC.

Examples include MADC-specific MIR APIs, c2mir extensions, ABI/codegen fixes, optimizer fixes, vector support, `_Complex` support, cleanup attributes, object-generation support, and other functionality.

Upstream MIR is still relevant, but development is relatively low-volume / maintenance-oriented. General MIR/c2mir bugs discovered during MADC development should continue to be contributed upstream where applicable.

There is no requirement to maintain `derekbsnider/mir` as an independently consumable downstream MIR distribution.

The primary maintenance goal is therefore:

> Keep normal MADC development within one repository while preserving a clean path for occasionally contributing generic MIR/c2mir fixes upstream.

---

# 2. Target Architecture

The desired long-term structure is:

```text
derekbsnider/madc
├── src/
├── tests/
├── docs/
├── scripts/
└── third_party/
    └── mir/
        ├── mir.c
        ├── mir-gen.c
        ├── c2mir.c
        ├── c-tests/
        └── ...
```

`third_party/mir` must **not** contain its own `.git` directory.

It is part of the MADC Git repository.

Conceptually:

```text
                    vnmakarov/mir
                    TRUE UPSTREAM
                     ↑          │
             clean PR│          │ upstream sync
                     │          ↓
              derekbsnider/mir
               PR TRANSPORT

          ┌────────────────────────────┐
          │ derekbsnider/madc          │
          │                            │
          │ third_party/mir            │
          │ CANONICAL MADC DOWNSTREAM  │
          │                            │
          │ src/                       │
          │ tests/                     │
          └────────────────────────────┘
```

There must **not** be two maintained copies:

```text
derekbsnider/mir
        ↕
madc/third_party/mir
```

That is explicitly not the desired architecture.

---

# 3. Repository Roles After Migration

## `derekbsnider/madc`

Canonical project repository.

Contains the canonical MADC-specific MIR implementation under:

```text
third_party/mir
```

All MADC-specific MIR development happens here.

---

## `vnmakarov/mir`

Canonical upstream MIR project.

Used for:

* reviewing upstream development;
* periodically adopting relevant upstream changes;
* submitting generic MIR/c2mir bug fixes.

It remains the conceptual upstream of `madc/third_party/mir`.

---

## `derekbsnider/mir`

No longer the canonical MADC MIR downstream.

Do **not** keep its `master` or `develop` synchronized with `madc/third_party/mir`.

It remains useful for:

1. historical preservation of the former MADC MIR downstream;
2. hosting temporary clean branches based on `vnmakarov/mir` for GitHub pull requests.

Example future branch structure:

```text
master                         # frozen historical downstream
develop                        # frozen historical downstream

upstream-fix-foo               # temporary clean upstream PR branch
upstream-fix-bar
upstream-fix-baz
```

Do not archive the repository if doing so would interfere with pushing PR branches.

---

# 4. Critical Migration Invariants

The migration must obey the following rules.

## 4.1 Import exactly the MIR revision MADC currently uses

The subtree import must use the commit currently identified by MADC's `MIR_COMMIT`.

Do not simply import the current tip of `derekbsnider/mir/develop`.

The migration must begin with:

```text
MADC + MIR_COMMIT X
```

and end with:

```text
MADC containing third_party/mir corresponding exactly to X
```

This makes the initial migration a source-equivalent operation.

Read `MIR_COMMIT` at execution time — never fossilize a SHA into the
migration commands (the pin moved the same day this plan was drafted).
As of 2026-08-11 it reads `fde19e17...`, which is also the released
`v1.0-madc.0.76.0` fork tag: the import baseline is a released,
fully-validated pin.

---

## 4.2 Preserve MIR history

Do **not** perform a squashed subtree import.

Avoid:

```bash
git subtree add --squash ...
```

The MIR ancestry should remain available in the MADC Git object/history graph.

This is useful for:

* provenance;
* upstream comparisons;
* patch extraction;
* future merges;
* understanding historical MIR changes.

---

## 4.3 Do not modify MIR functionality during the migration

The subtree migration should not simultaneously contain:

* MIR bug fixes;
* MADC compiler features;
* optimizer changes;
* ABI changes;
* refactors unrelated to relocation.

Keep topology/build changes separate from functional development.

---

## 4.4 Normal MADC development must require no subtree commands

After migration, changing:

```text
third_party/mir/mir-gen.c
```

is just ordinary MADC development.

For example:

```bash
git add src/madc_mir_backend.cpp
git add third_party/mir/mir-gen.c
git add tests/foo.mad

git commit
```

This is expected and desirable.

Do not introduce a workflow requiring developers to separately commit or synchronize MIR for ordinary MADC development.

---

# 5. Pre-Migration Preparation

Perform the migration from clean, pushed branches.

Prefer using the existing `develop` integration branches initially because MADC and the MIR fork currently follow corresponding `develop` branches.

Verify:

```bash
cd madc

git switch develop
git pull --ff-only
git status
```

The working tree must be clean.

Record:

```bash
cat MIR_COMMIT
cat MIR_VERSION
git rev-parse HEAD
```

Also record the current test baseline.

At minimum record:

* MADC commit SHA;
* `MIR_COMMIT`;
* `MIR_VERSION`;
* MADC full-test results;
* MIR test results;
* release/packed test results;
* any current SMAUG or large-integration gate currently used.

The exact baseline should be committed to a migration note or otherwise preserved during implementation.

---

# 6. Preserve a Historical Marker in `derekbsnider/mir`

Before changing the relationship between repositories, create a permanent tag on the old downstream marking the final separately-maintained state.

Example:

```bash
cd ../mir

git switch develop
git pull --ff-only

git tag madc-pre-subtree-migration
git push origin madc-pre-subtree-migration
```

Use an appropriate annotated tag if preferred.

The important requirement is that the final pre-migration standalone downstream state can always be recovered.

---

# 7. Add the Existing MIR Fork as a Temporary MADC Remote

From MADC:

```bash
cd ../madc

git remote add mir-legacy git@github.com:derekbsnider/mir.git
git fetch mir-legacy
```

Read the pinned SHA:

```bash
MIR_SHA=$(cat MIR_COMMIT)
```

Verify that the object exists:

```bash
git show --no-patch "$MIR_SHA"
```

If it does not exist locally after fetching, stop and diagnose before continuing.

---

# 8. Import MIR Under `third_party/mir`

Import the exact pinned MIR commit into:

```text
third_party/mir
```

using Git subtree without squash.

The precise command may vary depending on Git/subtree behavior and repository state, but the intended operation is equivalent to:

```bash
git subtree add \
    --prefix=third_party/mir \
    <MIR source/ref corresponding to MIR_COMMIT>
```

Do not use `--squash`.

If `git subtree add` does not accept the raw commit in the desired manner, create an appropriate temporary local ref pointing at `MIR_COMMIT` and import from that.

The resulting history should connect:

```text
MADC history ──────────────┐
                           ├── subtree import
MIR history ───────────────┘
```

---

# 9. Verify the Imported MIR Tree Is Exact

Before modifying build wiring, prove that:

```text
third_party/mir
```

matches the old standalone MIR tree at `MIR_COMMIT`.

Use reliable tree/file comparison.

For example, compare:

```text
old MIR checkout @ MIR_COMMIT
```

against:

```text
madc/third_party/mir
```

excluding Git metadata and generated/untracked build output.

There should be no unexplained source differences.

This is an important migration gate.

---

# 10. Rewire the MADC Build

MADC currently expects MIR as a separate checkout, historically at a location such as:

```text
/workspace/mir
```

Replace this with the repository-local location:

```text
<madc-root>/third_party/mir
```

Update all relevant:

* Makefiles;
* configure logic;
* include paths;
* library paths;
* test scripts;
* CI configuration;
* development tooling;
* agent instructions;
* documentation;
* helper scripts.

Prefer deriving the path relative to the MADC repository root rather than hardcoding an absolute path.

Conceptually:

```make
MIR_ROOT := $(TOPDIR)/third_party/mir
```

Use whatever path mechanism best fits the existing build system.

Build-directory hygiene: madc's recursive makes build per-target libmir
variants INSIDE the MIR tree (`build-*/libmir.a` — six-plus variants
today, via `BUILD_DIR=`). Under `third_party/mir` those would dirty the
subtree on the first build and defeat §4.4's ordinary-source invariant.
Redirect them outside the subtree (e.g. under `obj/`) rather than
gitignoring `third_party/mir/build-*/` — an ignored in-tree build
directory would still muddy §9's exactness verification.

---

# 11. Required Fresh-Clone Behavior

After migration, this workflow must be sufficient:

```bash
git clone https://github.com/derekbsnider/madc.git
cd madc

./configure
make -C src
```

There should be no requirement to separately run:

```bash
git clone derekbsnider/mir
```

and no requirement to initialize a submodule.

This is a subtree, not a submodule.

---

# 12. Remove `MIR_COMMIT`

Once MADC contains the actual MIR source, `MIR_COMMIT` is redundant.

Previously:

```text
MADC commit
    ↓
MIR_COMMIT
    ↓
external MIR repository commit
```

After migration:

```text
MADC commit
    ↓
contains exact MIR source
```

Remove:

```text
MIR_COMMIT
```

and all associated logic including:

* pin checking;
* pin updating;
* documentation requiring pin synchronization;
* scripts validating the external MIR checkout against the pin;
* instructions requiring MIR commits to be pushed before MADC commits.

Git itself now versions the complete source configuration.

---

# 13. Remove the Separate MADC-MIR Release Coupling

Review `MIR_VERSION` and the current custom MIR release/tag policy.

The existing scheme versions the external fork because MADC depends upon a separately released MIR downstream.

That distinction no longer exists.

Prefer removing:

```text
MIR_VERSION
```

and the requirement to publish corresponding MADC-specific MIR releases/tags.

A MADC release itself now identifies the MIR code it contains.

For example:

```text
MADC v0.x.y
```

implicitly includes a precise version of:

```text
third_party/mir
```

because Git versions both together.

If useful, retain informational provenance describing the current upstream MIR baseline, but do not recreate `MIR_COMMIT` under a different name.

---

# 14. Configure Permanent Git Remotes

After migration, MADC should know about true MIR upstream.

Example:

```bash
git remote add mir-upstream https://github.com/vnmakarov/mir.git
```

Optionally retain the GitHub fork remote under a role-specific name:

```bash
git remote rename mir-legacy mir-pr
```

Result:

```text
origin          -> derekbsnider/madc
mir-upstream    -> vnmakarov/mir
mir-pr          -> derekbsnider/mir
```

These names should reflect their roles.

`mir-pr` must not be treated as the source of the canonical MADC MIR downstream.

---

# 15. Normal Development Workflow

After migration, ordinary MADC development operates exclusively in the MADC repository.

Example:

```text
src/foo.cpp
third_party/mir/mir-gen.c
tests/foo.mad
```

can all be changed in one commit.

Example:

```bash
git add src/foo.cpp
git add third_party/mir/mir-gen.c
git add tests/foo.mad

git commit -m "Implement foo support"
```

No additional action is required.

Specifically, do **not**:

* commit separately to `derekbsnider/mir`;
* push MIR first;
* update a MIR pointer;
* cherry-pick ordinary MADC MIR changes anywhere;
* run `git subtree split` for normal development.

Subtree mechanics are only needed when crossing the MADC/upstream-MIR boundary.

---

# 16. Policy for Generic MIR/c2mir Bug Fixes

When possible, distinguish generic MIR bugs from MADC-specific functionality.

If a problem reproduces on stock MIR/c2mir and should reasonably be fixed upstream, preferably commit the generic fix separately.

Good history:

```text
commit A:
    Fix c2mir narrow integer extension bug

commit B:
    Add MADC feature that relies on correct narrow extension
```

Less desirable:

```text
commit A:
    large MADC feature
    + generic MIR bug
    + unrelated refactor
```

Separate commits make upstream extraction substantially easier.

This is a preferred convention, not a requirement for ordinary development.

---

# 17. Exporting a Generic MIR Fix for Upstream

Suppose MADC contains a generic fix under:

```text
third_party/mir
```

that should be submitted to `vnmakarov/mir`.

The target PR must be based on:

```text
vnmakarov/mir master
```

not on the heavily divergent MADC MIR history.

## Step 17.1 — Generate MIR-only history

Create a temporary subtree split:

```bash
git subtree split \
    --prefix=third_party/mir \
    -b mir-export
```

This produces a branch whose root paths look like an ordinary MIR checkout:

```text
mir.c
mir-gen.c
c2mir.c
c-tests/
...
```

rather than:

```text
third_party/mir/mir.c
```

---

## Step 17.2 — Identify the generic fix

Inspect:

```bash
git log mir-export
```

Locate the MIR-only equivalent of the desired fix.

Call this commit:

```text
<MIR_FIX_SHA>
```

---

## Step 17.3 — Fetch true upstream

```bash
git fetch mir-upstream
```

---

## Step 17.4 — Create a clean upstream-based PR branch

Prefer doing this in a temporary Git worktree so the main MADC checkout is not disturbed.

Example:

```bash
git worktree add \
    ../mir-upstream-pr \
    -b upstream-fix-foo \
    mir-upstream/master
```

Then:

```bash
cd ../mir-upstream-pr
git cherry-pick <MIR_FIX_SHA>
```

If the cherry-pick conflicts because the MADC downstream has diverged, manually adapt the change to stock MIR.

The resulting commit must represent the generic upstream fix, not MADC-specific code.

---

## Step 17.5 — Test against stock MIR

Run the appropriate upstream MIR/c2mir test suite.

When practical, include:

* a minimal reproducer;
* regression test;
* MIR interpreter/generator paths where relevant;
* relevant optimization levels.

Ensure the fix stands independently of MADC.

---

## Step 17.6 — Push through `derekbsnider/mir`

Push the clean branch:

```bash
git push mir-pr HEAD:upstream-fix-foo
```

Then create the GitHub PR:

```text
derekbsnider/mir:upstream-fix-foo
                  ↓
vnmakarov/mir:master
```

This is the primary continuing purpose of the `derekbsnider/mir` fork.

---

## Step 17.7 — Cleanup

Once appropriate:

```bash
git worktree remove ../mir-upstream-pr
git branch -D mir-export
```

Delete temporary local/remote branches according to normal PR hygiene.

---

# 18. Important: Do Not Synchronize the Old Fork

Do not establish this workflow:

```text
madc/third_party/mir
        ↕
derekbsnider/mir master/develop
```

Do not regularly push subtree state into `derekbsnider/mir`.

Do not pull `derekbsnider/mir/master` back into MADC.

The old fork is no longer the maintained downstream.

Only temporary clean upstream PR branches need to be pushed there.

Standing exception as of 2026-08-11: three upstream PRs are in flight
with Vladimir riding branches on `derekbsnider/mir` (mir#461, #462,
#463). Leave those PR branches untouched until the PRs resolve — they
are exactly the temporary clean upstream PR branches this section
permits.

---

# 19. Receiving Changes From Upstream MIR

Incoming upstream changes flow directly from:

```text
vnmakarov/mir
```

to:

```text
madc/third_party/mir
```

They do **not** need to pass through `derekbsnider/mir`.

Fetch:

```bash
git fetch mir-upstream
```

Review upstream changes before adopting them.

Do not automatically chase upstream `master`.

Given MADC's divergence, upstream updates should be deliberate.

When appropriate, use subtree-aware merging/pulling, conceptually:

```bash
git subtree pull \
    --prefix=third_party/mir \
    mir-upstream master
```

Do not use `--squash`.

Resolve conflicts in the context of the MADC downstream.

Then run both:

* upstream MIR tests;
* complete MADC validation.

---

# 20. Upstream Synchronization Policy

Upstream synchronization should be intentional.

Preferred sequence:

```text
new upstream MIR changes
        ↓
review relevance
        ↓
fetch upstream
        ↓
merge/subtree pull selected upstream state
        ↓
resolve downstream conflicts
        ↓
run MIR tests
        ↓
run MADC full tests
        ↓
commit
```

Do not configure CI or automation to continuously merge the latest upstream MIR automatically.

MADC carries substantial intentional divergence.

---

# 21. Update `derekbsnider/mir`

Do not rewrite or reset its history simply to make it resemble upstream again.

Preserve the historical MADC downstream.

Update its README prominently.

Suggested message:

```markdown
# MADC MIR Downstream — Historical Repository

The MADC-specific MIR/c2mir downstream formerly maintained in this
repository has moved into the MADC source tree:

    derekbsnider/madc/third_party/mir

That directory is now the canonical maintained MADC version of MIR.

This repository is retained for historical reference and as the GitHub
fork used to submit general MIR/c2mir fixes back to:

    vnmakarov/mir

The historical `master` / `develop` branches are no longer synchronized
with MADC.

New generic upstream fixes may appear here temporarily as dedicated
upstream PR branches.
```

Adjust wording as appropriate.

---

# 22. Add MADC Documentation / ADR

Create an ADR, for example:

```text
docs/adr/XXXX-mir-subtree-integration.md
```

Document:

## Upstream

```text
vnmakarov/mir
```

## Canonical MADC downstream

```text
third_party/mir
```

## Historical / PR transport fork

```text
derekbsnider/mir
```

## Policy

* MADC-specific MIR changes live only in MADC.
* `third_party/mir` is normal MADC source.
* Generic MIR/c2mir bugs should be contributed upstream when appropriate.
* Upstream PR branches are clean branches based on `vnmakarov/mir`.
* `derekbsnider/mir` hosts those PR branches.
* The old fork's `master` / `develop` are not synchronized with MADC.
* Incoming upstream changes flow directly from `vnmakarov/mir` into the MADC subtree.
* Subtree operations are boundary operations, not normal development operations.

---

# 23. Update Existing MADC Documentation

Search for references to:

```text
/workspace/mir
MIR_COMMIT
MIR_VERSION
derekbsnider/mir
MIR develop
MIR master
MIR release
MIR fork release
matching branch
pin discipline
```

Update at least:

```text
docs/build.md
.claude/rules/build.md
.claude/commands/release.md
.claude/commands/promote.md
AGENTS.md
README.md
scripts/
configure.ac
Makefiles
CI configuration
release documentation
```

where applicable.

The `/release` and `/promote` skill files carry the fork-lockstep
ceremony §13 retires (the `MIR_VERSION` bump, the fork tag, the fork
develop/master merges); update both, plus AGENTS.md's "Build system"
section, in the same commit that removes the machinery (Commit 3/
Commit 4 in §28).

Remove language requiring synchronized repositories.

Replace it with the subtree model.

---

# 24. Consider Adding Helper Scripts

After the migration works manually, consider adding helpers for the rare boundary operations.

For example:

```text
scripts/mir-upstream-sync
scripts/mir-upstream-pr
```

Potential `mir-upstream-pr` workflow:

```text
1. subtree split third_party/mir
2. identify/request a MIR-only commit
3. fetch mir-upstream
4. create temporary worktree from mir-upstream/master
5. cherry-pick MIR-only commit
6. leave worktree ready for testing/review
```

Do not automate pushing or opening the PR until the resulting patch has been reviewed and tested.

The helper should optimize repetitive mechanics without hiding which history is being modified.

---

# 25. Migration Validation

The migration is complete only after proving that repository relocation did not change behavior.

Run appropriate MIR tests against:

```text
third_party/mir
```

Then run normal MADC gates.

At minimum:

```bash
make -C src clean
make -C src
make -C src fulltest
```

Build release:

```bash
make -C src release
```

Run the packed/release suite:

```bash
MADC_BIN=bin/madc-release bash scripts/run_tests.sh
```

Run any currently-required:

* MIR `make test`;
* gcc.c-torture gate;
* SMAUG build/boot/soak test;
* native executable lane;
* shared library / libmadc tests;
* the darwin release lane — the MIR content includes the Mach-O
  executable writer and the darwin ABI work, so this is not optional:
  `make -C src release-macos` (which runs `verify_macho_release.sh`
  and `scripts/macho_exe_dylib_gate.sh`), `make -C src machogate`,
  the libc++ flavor lane (`remote_build.sh libcxxjit` at minimum),
  and one on-hardware `scripts/mac_battery.sh` run;
* other release gates documented by the project.

Add one NEGATIVE control: after rewiring, remove or rename
`/workspace/mir` on the build container and prove the build still
succeeds. A build silently picking up the stale sibling checkout reads
as green. (§26's fresh clone covers a new directory; this covers the
*old* one still being present and wrongly reachable.)

Compare results with the pre-migration baseline.

The expected result is no functional regression.

---

# 26. Fresh Clone Validation

Perform a clean clone into a new directory.

Do not rely on the old `/workspace/mir`.

Verify:

```bash
git clone <madc repository>
cd madc

./configure
make -C src
make -C src fulltest
```

The build must use:

```text
third_party/mir
```

without external setup.

Verify there is no hidden dependency on the old standalone MIR checkout.

This is a critical acceptance test.

---

# 27. Remove Obsolete Standalone-MIR Development Infrastructure

After validation, remove remaining assumptions that developers must maintain two repositories.

Potential obsolete items include:

* `MIR_COMMIT`;
* `MIR_VERSION`;
* external MIR-path validation;
* branch correspondence rules;
* matching MIR/MADC release rules;
* scripts that fetch/build `/workspace/mir`;
* instructions requiring MIR changes to be pushed first;
* separate downstream MIR release tags used only to pair with MADC versions;
* the MIR leg of `scripts/remote_build.sh`'s `sync` stage (one
  repository to rsync afterward);
* `scripts/provision_container.sh`'s second-repository restore
  (simplifies to restoring madc alone).

Do not remove historical documentation if it provides useful provenance, but clearly label it historical.

---

# 28. Commit Structure

Prefer making the migration reviewable.

Suggested sequence:

## Commit 1 — Import MIR subtree

```text
Import pinned MADC MIR downstream into third_party/mir
```

No functionality changes.

## Commit 2 — Rewire build

```text
Build MADC against in-tree MIR subtree
```

Still no intended behavior change.

## Commit 3 — Remove external MIR pin/release machinery

```text
Remove standalone MIR dependency and pin management
```

## Commit 4 — Documentation / ADR

```text
Document MIR subtree and upstream contribution workflow
```

The exact number of commits may differ, but avoid combining unrelated compiler development with the migration.

---

# 29. Acceptance Criteria

The work is complete when all of the following are true:

* [ ] `madc/third_party/mir` contains the exact MADC MIR downstream.
* [ ] MIR Git ancestry was imported without `--squash`.
* [ ] `third_party/mir` contains no nested `.git`.
* [ ] Normal MIR-related MADC development requires only ordinary MADC Git commits.
* [ ] A fresh MADC clone contains all required MIR source.
* [ ] MADC no longer requires `/workspace/mir`.
* [ ] `MIR_COMMIT` has been removed.
* [ ] `MIR_VERSION` and standalone downstream release machinery have been removed or explicitly justified.
* [ ] MADC no longer requires synchronized `develop` / `master` branches across two repositories.
* [ ] `vnmakarov/mir` is documented as the true upstream.
* [ ] `derekbsnider/mir` is documented as historical / PR transport rather than canonical MADC downstream.
* [ ] Incoming upstream changes can be merged directly into `third_party/mir`.
* [ ] Generic MADC-discovered MIR fixes can be exported as clean upstream-based commits.
* [ ] Those commits can be pushed through `derekbsnider/mir` and submitted to `vnmakarov/mir`.
* [ ] Existing MIR tests pass.
* [ ] Existing MADC full tests pass.
* [ ] Release/packed tests pass.
* [ ] SMAUG / large-integration gates remain green.
* [ ] The darwin release lane is green post-migration (`release-macos`
      with its gates, `machogate`, the libc++ lane, one on-hardware
      battery run — per §25).
* [ ] The stale-checkout negative control passed: with `/workspace/mir`
      removed or renamed on the build host, the build still succeeds
      (per §25).
* [ ] Fresh-clone build/test succeeds without any external MIR checkout.
* [ ] Documentation describes only one canonical MADC-specific MIR implementation.

---

# 30. Non-Goals

Do not use this migration to:

* attempt to upstream all MADC-specific MIR changes;
* reduce divergence from upstream;
* redesign MIR/c2mir;
* refactor the MADC backend;
* change optimization behavior;
* change ABI behavior;
* change compiler semantics;
* rewrite historical MIR commits;
* make `derekbsnider/mir` identical to `vnmakarov/mir`;
* keep `derekbsnider/mir` synchronized with `third_party/mir`;
* introduce a Git submodule.

Those can be separate future projects where appropriate.

One queued follow-up (owner, 2026-08-11), explicitly out of scope for
the migration itself: GitHub Actions workflows auto-building the
release artifacts on the `v*` tag push (standard runners are free on
public repos). The migration is the enabler — a runner then needs only
`git clone && make`. Design notes so far: the Linux deb/rpm job wraps
`scripts/package_release.sh`, and CI *should* run the packed suite
there (it costs no owner wall-clock and gates the exact artifact); the
macOS artifacts favor keeping the Linux cross build, which wants the
zig `libSystem.tbd` cross-link follow-up (Intel macOS runners are being
retired upstream, so a native x86_64 Apple runner may not exist by
then).

Sequencing (owner, 2026-08-11, post-go): the **Windows release comes
first** — "once we have a working windows build, then we will set up
all the github runner stuff." The runner matrix should automate build
lanes that already exist, and a Windows lane adds a fourth artifact
family that changes the matrix's shape.

---

# 31. Guiding Principle

The long-term maintenance rule should be simple:

> **If a MIR change exists because MADC needs it, maintain it directly in `madc/third_party/mir`. If it is a generic MIR/c2mir bug that affects upstream independently of MADC, maintain the fix in MADC and additionally export a clean version of that fix for an upstream PR.**

Normal development happens in one repository.

Git subtree mechanics are used only when interacting with upstream MIR.

`derekbsnider/mir` is not another codebase to maintain.

That repository is simply the historical former downstream plus the GitHub bridge used when MADC has something useful to contribute back to MIR.

