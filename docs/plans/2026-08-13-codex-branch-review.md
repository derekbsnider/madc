# Review — `feature/win64-46b-burndown-codex` (Codex, session #88)

Reviewer: Claude. Range `1c540b4a..84dcb607` (8 commits), against the brief
in `2026-08-13-win64-46b-burndown-CODEX-HANDOFF.md`.

## Verdict

**Accept with two fixture findings to resolve before merge.** The work
exceeds the brief: alongside packages A and B, Codex diagnosed and fixed
**four real compiler defects at depth**, each with rule trailers carrying
a genuine oracle and each shipping a reducer or unit coverage. The
methodology was followed, not asserted.

### Verified independently (reviewer's own runs, at Codex HEAD)

| lane | result |
|---|---|
| `fulltest` | rc=0 — 1029 passed / 0 failed / 9 skipped |
| libc++ JIT | 1025 passed / 0 failed / 13 skipped |
| EXE | 1000 passed / 0 failed (of 1029 JIT-passing) |
| OBJ | (in flight at time of writing) |

Codex's reported numbers reproduce. Its wine result (981/0/0/57) and its
diagnosis of the rotating wine failures as a **wineserver connection
reset** — proven by a persistent `wineserver -p` producing repeatable
zero-failure runs — supersedes the earlier "wine wobble" hand-wave in the
round-2 notes. That is a better answer than the one it replaced.

### The four compiler fixes (all sound on inspection)

- `fix(mir)` — MIR combine treated a side-effecting `MIR_ALLOCA` as a
  movable value definition. Two lines in `mir-gen.c`. **See F4.**
- `fix(layout)` — Win64 virtual-base offsets eight bytes too large;
  `compute_layout` did not reuse nonvirtual base tail padding. Oracle:
  `g++ -fdump-lang-class` (the correct instrument). +52 lines of unit
  coverage in `test_class_layout.cpp`.
- `fix(pp)` — eager prescan plus a fresh unpainted argument `Source`
  violated the C raw-argument and blue-paint rules. Reducer:
  `testmacroargprescan.mad`.
- `fix(c2mir)` — Win64 long-double conversions skipped because c2mir
  treated every memory-shaped ABI value alike.

Three touch `third_party/mir`, raising fork divergence; the layout and pp
fixes are madc-side. All four are target-semantics fixes, not shims — the
layer chains in their trailers hold up against the code.

### Test sources modified — checked individually

- `testlang` ✅ genuine root fix. `getenv("HOME")` is NULL on Windows, so
  `std::string path = getenv("HOME")` threw — that IS the `std::logic_error`
  in the round-2 report.
- `testmadcevalscope` ✅ correct and important. The test passed a `long&`
  to an `int64_t&` API; on LLP64 that would let the callee write 8 bytes
  into a 4-byte object. madc's warning was preventing stack corruption —
  the test was wrong, not the diagnostic.
- `testtemplate` ✅ acceptable. `Box<long>` → `Box<int64_t>` preserves the
  test's stated intent ("8-byte type parameters") on a target where `long`
  is not 8 bytes.
- `testtranssecondary` ✅ comments only, documenting the LLP64 oracle
  numbers alongside the LP64 ones.
- `testnsmadcautostring` ⚠️ **F1**.

---

## Findings

### F1 — a test was rewritten where a domain skip was the right tool

`testnsmadcautostring.mad` was replaced wholesale: its `exec://sort`
channel exercise became an `eval_expression_string` call, justified as
"Wine does not ship a POSIX `sort`".

Two problems. First it is **inconsistent**: `testexecchannel` carries a
`.win64_skip` for the *identical* reason, so the same fact produced two
different dispositions. Second and worse, the rewrite **removes Linux
coverage** — the original exercised the channel's gated `std::string`
overloads (`write(string)`, `readline(string&)`), which is what the test's
own header comment says it is for; the replacement exercises a different
`madc::` entry point entirely. A win64 environment limit must never cost
POSIX-host coverage.

**Disposition:** restore the original test plus a `.wine64_skip` — and
then F2 removes the need for even that.

### F2 — the `exec://` tests depend on an external command's collation

*(folded in at owner request, 2026-08-13)*

`testexecchannel`, `testvaluesort` and `testnsmadcautostring` all spawn
`exec://sort` and assert on its output. The **channel machinery** is what
they exist to test — spawn a child, write its stdin, read its stdout —
and the choice of `sort` is incidental to that. It is also the only part
that does not port:

- Windows `sort.exe` is not POSIX `sort` (case folding, CRLF) — so even
  where a `sort.exe` exists the expected output differs.
- POSIX `sort` collation is **locale-dependent**; the macOS lane already
  carries an `LC_ALL=C` warning for exactly this.
- Wine ships none at all.

So the dependency is fragile on all three platforms, and "copy Windows'
`sort.exe` into the container" would fix none of it: it cannot enter the
repo or `provision_container.sh` (Microsoft-copyrighted), it would die on
the next container rebuild (the container is provisioned *from the repo*),
and it would convert a missing-file failure into a wrong-collation
failure.

**Fix: make madc itself the `exec://` child.** The suite has just built
`bin/madc` / `madc-<mode>.exe`; a small `.mad` child that reads stdin and
emits a deterministic transformation is always present, has no locale
sensitivity, and behaves identically on Linux, macOS and Windows. The
tests then assert on the channel, which is their subject. Three skips
dissolve on their own merits rather than by importing a binary.

*Design note:* the child must be located the same way the runner locates
the binary under test (`MADC_BIN`), so the packed/release and hosted lanes
each exercise their own artifact rather than a stale `bin/madc`.

### F3 — a win64→wine64 reclassification is asserted, not evidenced

`testvaluesort` moved from `.win64_skip` to `.wine64_skip`, its fixture
claiming "PASSES with Windows sort.exe on the real Windows runner
(verified 2026-08-13)". I found no corroborating `win_run.sh` invocation
in the branch, and Codex's own summary describes wine-side work
(`wineserver -p`), not a real-Windows battery.

The distinction is exactly what the two domains mean: `win64_skip` = "out
of scope on Windows", `wine64_skip` = "passes on real Windows, fails only
under wine". The second is a **claim about hardware we own and can
test** — `scripts/win_run.sh` runs the binary as a genuine Windows process
on the owner's box (real PE loader / ntdll / ucrtbase, not wine).

**Disposition:** run it through `win_run.sh` and keep the fixture if it
passes; otherwise revert to `.win64_skip`. (F2 makes the question moot,
but the *rule* stands: a `wine64_skip` requires real-Windows evidence.)

### F4 — the MIR alloca fix looks upstreamable

`fix(mir)`'s defect — combine substituting across a side-effecting
`MIR_ALLOCA` — reads as **generic MIR**, not madc-specific and not
Win64-specific: any MIR consumer whose generated code allocas inside a
function that later spills could hit it. Per the established upstream
process (authored-by-us bugfixes only, owner review gates submission),
this is a candidate for a `vnmakarov/mir` PR, which would also shrink
fork divergence.

**Disposition:** owner decision. Not a blocker.

### F5 — the MSVC oracle precedence needs checking against a settled non-goal

Codex reports recording "platform-aware GCC/Clang/MSVC oracle precedence"
in the plan and KG. MSVC as a reference for *what Windows C semantics
are* is defensible. MSVC as an **ABI** oracle contradicts a settled
non-goal of this lane — "MSVC-ABI interop (madc.exe is a mingw-world
binary; C++ interop with MSVC-built objects is explicitly out)". The
codegen/ABI/type-model oracle for win64 is `x86_64-w64-mingw32-gcc`, full
stop.

**Disposition:** read what was recorded; narrow it to semantics if it
reaches further.

### Not yet reviewed

`include/datadef.h` (+106), `src/lexer.cpp` (+146), `src/cir_builder.cpp`
(+72), `src/embedded_headers.cpp` (+421 — expected to be regenerated
output from the ns-header changes; confirm it is generated, not
hand-edited).

---

## Before merge

1. Resolve F1 and F3; land F2 (which subsumes both).
2. Finish the unreviewed surface above.
3. `/dupaudit` scoped to the touched subsystems (`branching.md` requires
   it before a feature branch merges) — the relevant surfaces are the
   layout/packing paths and the macro-expansion paths.
4. Merge to the lane branch, then `develop`; per the standing owner rule
   a feature merge to `develop` ships **with** a release.
