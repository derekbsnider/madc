# Build — Why

madc now has two materially different validation footprints:

1. core compiler / runtime / `libmadc`
2. optional `madcdat` storage/federation backends and their unit tests

When the task is clearly in the first bucket, leaving `madcdat` enabled
just burns time:

- more object files rebuild
- more unit binaries relink
- more optional backend dependencies stay in the path

That extra coverage is valuable when a change may affect shared public
headers, build wiring, or the storage layer itself. It is mostly noise
when the task is isolated to parser/codegen/runtime/public-embedding work.

So the operational rule is:

- for core-only work, prefer `./configure --enable-madcdat=no`
- for storage work, or anything that might affect storage, re-enable it
  before final validation

This is a workflow default, not a license to skip relevant coverage. If a
change can plausibly affect `madcdat`, the broader enabled build remains
the honest gate.

## Backend — why no asmjit flags

The build used to link the manually installed asmjit v1.14 at
`/usr/local/`, with `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` in the
Makefile to avoid the older apt-packaged copy. asmjit (the original
x86-64 JIT) was removed: madc now lowers its AST to a `cir_node` C11 IR
that c2mir compiles to MIR. The backend is the MIR library (libmir +
c2mir), in-tree at `third_party/mir`. The old asmjit link flags no
longer exist.

## The MIR subtree — why in-tree, why no pin

Until 2026-08-11 the MIR downstream lived in a separate repository
(`derekbsnider/mir`) coupled to madc by a commit pin (`MIR_COMMIT`), a
declared release dependency (`MIR_VERSION`), mirrored branch pairs, and
a lockstep merge/tag/release ceremony. Every one of those mechanisms
existed to answer one question — "which MIR does this madc need?" —
and every one was a drift/ceremony surface (stale pins in prose, forks
depending on un-pushed work, releases that had to be cut twice).

The subtree migration (`docs/plans/mir-into-madc-repo-2026-08-11.md`)
answers the question structurally: `third_party/mir` is ordinary madc
source, so the madc commit IS the pin and the madc release IS the MIR
release. Contributors clone one repository; a MIR-touching fix and the
madc feature needing it land in one commit. The import preserved full
MIR history (no squash), so provenance and upstream comparisons still
work; `vnmakarov/mir` remains the true upstream, and generic fixes are
exported as clean upstream-based branches pushed through
`derekbsnider/mir` (its final standalone state is tagged
`madc-pre-subtree-migration`).

Build products (libmir variants, c2m) land under `obj/mir/`, never in
the subtree — a build that dirties `third_party/mir` would break the
exactness invariant that makes the subtree auditable against MIR
history. They sit outside `clean`'s scope because MIR sources change
rarely and rebuilding MIR on every madc clean build would tax the
usual clean-build loop; `mirclean` exists for the rare full reset.

Historical (superseded by the migration, kept for provenance): the fork
release naming was `<upstream-base>-madc.<madc-version>` — e.g.
`1.0-madc.0.76.0`, the last one cut — where the upstream base answered
"how stale are we against upstream?" and the suffix "which madc
consumes this?". `MIR_COMMIT` was the machine pin; `MIR_VERSION` the
declared release dependency; the annotated `v<MIR_VERSION>` fork tag
the release itself. The pin's value was never restated in prose (a
literal `2ffebff` once sat stale in two documents) — that
file-is-the-single-source rule survives in spirit: today the pin is
the commit itself.

## One memory-budgeted job count (2026-10-11)

The build container runs inside the owner's WSL2 VM: 16 GB of RAM, 4 GB of
swap, 20 cores and no cgroup memory cap. Builds were sized by cores alone —
`-j20` in `remote_build.sh`, `-j"$(nproc)"` in the packaging scripts — which
was safe only while one build ran at a time. Two -O2 release builds started
side by side (ten jobs each, then valgrind) exhausted the VM, and the owner's
whole WSL went down with it; the container restarted and its disk had to
remount.

`scripts/build_jobs.sh` is now the one answer to "how many jobs": the cores,
cut down to MemAvailable less a 2 GB reserve, at 1.5 GB a job (the -O2
compiles peak at 1.6 GB for `parser.cpp`, 0.9 GB for `cir_builder.cpp`, 0.7 GB
for `lexer.cpp`), and capped by a cgroup `memory.max` when one is set. It reads
memory when the build starts, so a second build beside a running one still
over-commits — the rule keeps builds one at a time. The hard cap sits at the
container level (`docker update --memory=12g --memory-swap=12g`, set the same
day: the container now dies before the VM does); the script is the part the
repository can enforce, and `check-one-build-jobs.sh` keeps a literal job
count from coming back.

