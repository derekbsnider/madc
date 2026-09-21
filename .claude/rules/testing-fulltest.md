# Testing: fulltest gates the MERGE WAVE, targeted tests gate the change

Incremental changes get TARGETED validation: the new/affected tests plus the
touched subsystem's neighbors — never the full battery per commit.
Run `make -C src fulltest` ONCE per merge wave, at the release/merge gate
(or when a change's blast radius is genuinely suite-wide, e.g. lexer/include
machinery, shared codegen).
NEVER re-run a suite on content that is already green — ceremonies are git-only.
Draw the merge wave at FEATURE completion, not at every slice: BANK the whole
feature — every slice AND every known-open fix of it — before spending the
multi-hour push-gate lanes (fulltest + exe + obj + packed + headerless,
c-testsuite, wine, the macOS cross build). A feature with a pending or
known-open fix is NOT a merge wave yet — hold the long suites until it is
complete, so they run once for the whole feature, not once per slice.
The merge wave is the SEAM the arc's plan or design doc names — its RELEASE
BOUNDARY (e.g. V1–V5 of the client-server arc) — never a slice, phase or "V"
below it, however complete that slice is on its own: a slice with its own plan
file and gates is still a slice. A pre-arc conversion slice, a design ruling and
a defect fixed on the way (own commit, TARGETED gate) all ride the seam battery.
Between seams the work banks on the arc's feature branch (pushes freely); the
ONE battery, the lane records and the develop merge/push happen at the seam.
Before launching the battery, name the seam it gates in one sentence; a sentence
that names a slice is a targeted run, not a battery.
When work touches native executable, AOT, runtime-parity, or shared codegen paths,
the merge-wave battery also includes `bash scripts/run_tests.sh --exe`.
Do not leave the tree with JIT green and EXE broken, or EXE green and JIT broken.
Do NOT run integration tests in a shell loop — use the Makefile target.
