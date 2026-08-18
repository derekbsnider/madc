# HANDOFF — php dump intrinsics arc, sessions #100–#102, 2026-08-18

**Read this fully. Assume a cold start.** Run `bash scripts/resume.sh` first (live
git state + orphaned jobs), then `claude_status.json`, then this file, then
`docs/plans/2026-08-17-php-print-r-var-dump-plan.md` §14–§16.

Supersedes the session-#101 revision of this file.

---

## 1. STATE

- Branch `develop`. **VERSION = 0.84.0.** `master` = v0.82.0.
- **No release cut, deliberately** — the arc is still incomplete (§4). The
  standing owner ruling from session #101 holds: *"I don't want a version bump
  until it's done."*
- 23 commits unpushed on develop. Pushing is fine; releasing is not.
- Working tree clean except the owner's untracked `donut.c`, `test.mad`,
  `testsort.mad` — **never commit them**.
- ⚠️ **`git stash@{0}` holds a SUPERSEDED pointer implementation.** Do not
  restore it expecting to ship it — read §3 and plan §16.1 first.

### Landed in session #102 (all four green together)

| commit | what |
|---|---|
| `ccba73fb` | `php::print_r`/`var_dump` over the FULL `madc::value`/`array` kind gamut — all nine kinds, arbitrary nesting, both flavors, ONE runtime walk |
| `69b910ec` | a container the dumper cannot render is refused BY ITS OWN NAME (was dying four levels into libstdc++ at `_Rb_tree_color`) |
| `a9b38451` | **c2mir bug fix** — a string initializing an array sub-object left the initializer cursor INSIDE it; `char n[2][8] = {"ada","bob"}` silently produced `[ada][]` where gcc/clang give `[ada][bob]` |
| `25f2a171` | multidimensional arrays, print_r byte-identical to PHP |

**Validation of record (the exact content those commits ship):** Linux fulltest
**1076 / 0 / 0TO / 9 skip**, EXE **1036 / 0**, OBJ **1036 / 0**, zero warnings
under `-Werror`; MIR c-tests **1143 tests / 2286 successes / 0 failures**;
trailer gate 446/0. Logs `tmp/logs/rb-20260818-*.log`, `tmp/battery2.txt`.
Packed / headerless / wine / macOS batteries unchanged since v0.82.0 and NOT
re-run.

## 2. MEASURED COVERAGE — re-run, never summarise

Probes are in `tmp/probe/*.mad`; re-measure after any change and put the TABLE in
the report. The session-#101 release was cut on a one-slice "done" and had to be
reverted; the standing directive is **report coverage, not slices**.

| shape | result |
|---|---|
| scalar, `const char *` | **OK** |
| struct / class (access, inheritance, unions, anon unions, bit-fields) | **OK** |
| fixed array, `char[]` as text | **OK** |
| `std::vector`, nested vector, `std::string` | **OK** |
| `print_r($x, true)` (capture, runtime flag, scalar capture) | **OK** |
| **`madc::value` / `array`** — null, boolean, integer, real, string, bytes, array, object, instance | **OK** (session #102) |
| **multidimensional array** (2-D, 3-D; `char[2][8]` rows as text) | **OK** (session #102) |
| `std::map` / `set` / `list` | **REFUSED by container name** — needs S1 `begin()`/`end()` |
| **pointer**, **pointer MEMBER** | **REFUSED by name** — see §3 |
| enum | **REFUSED by name** — see plan §16.4 |

## 3. NEXT — pointers, and the design is settled. READ plan §16 FIRST.

An OPEN QUESTION was put to the owner and **not yet answered**: build the pointer
slice first, or enums (the contained one) first? Ask again rather than guess.

**Pointers: a first attempt was built and REJECTED — it is in `stash@{0}`, not
committed.** It expanded the pointee at compile time and guarded with a stack of
pointee TYPES on the current path plus a fan-out budget. It terminated, but:

> **OWNER, 2026-08-18:** *"if you have a linked list of structures with pointers
> and a loop exists where one structure points back to an earlier one in the
> series, print_r or var_dump would loop endlessly without loop detection"*

It was loop AVOIDANCE BY REFUSAL, not loop detection — it terminated only by
refusing `struct Node { Node *next; }`, i.e. the canonical case. Also measured: an
ACYCLIC 14-level fan-out-2 graph took 57s and then **SIGSEGV** (a flat 32,000-
statement function compiles fine in 3.3s, so it is shape-specific and ⚠️ **NOT
root-caused** — recorded as an open defect in plan §16.1).

The design to build is plan §16.3, in one line each:

1. The ancestor stack becomes a shared, GROWABLE, thread-local facility in
   `src/rt/rt_dump.c` (C11, `MADC_RT_TLS` as `rt_except.c` does), and
   `src/rt_dump_value.cpp` switches to it — one owner, not two.
2. One memoized generated function per (pointee type, flavor), so recursion is a
   CALL: fixes cycles, long lists and the fan-out blowup together.
3. Columns stay compile-time constants inside the body because the geometry is
   LINEAR in depth: compute the base once, emit `base + constant`.
4. The definition goes on a new `m_pending_top_defs`, drained into `top_list`
   (`src/cir_builder.cpp` ~26564); the prototype rides the existing
   `need_output_extern`. There is no existing mid-body top-level queue.

**It must be an ANCESTOR STACK, not a `set<void *>`** — oracle in plan §16.2: PHP
prints a value reachable twice IN FULL BOTH TIMES and marks only a real cycle. A
"visited" flag on each element is unavailable: we do not own the pointee and a
dump must not write to what it reads.

## 4. THEN — the rest, in this order

- **enums** (plan §16.4). `DataDefENUM` must carry its enumerators. The forest
  already serializes them; the cost is that `forest_record_enum`
  (`src/madc_cir.cpp` ~4795) reads them back through a NAME-keyed
  `namespace_map` lookup, so making the type the owner touches the pack path and
  needs the release / packed / headerless lanes, not just fulltest.
- **S1 `begin()`/`end()` protocol** — owed regardless: `for (auto &kv : m)` does
  not work today. Unlocks **S5b** associative rendering (`std::map`/`set`/`list`),
  which nothing above covers.
- Then the two macOS MIR-blob causes (§6), then #60/#61/#25/#56/#55/#49.

## 5. SETTLED — do not re-litigate

- **`php::print_r` returns `madc::value &`**, not by value (a value has no
  emitted C type; by-value emits `int` and silently returns nothing — §6 D1).
  Signature `template<class T> madc::value &print_r(const T &v, bool ret = false)`
  — PHP's own shape.
- **The bodyless placeholder's ZERO arity is load-bearing** (it is arity-filtered
  out of overload ranking). Never give it parameters; the default argument is
  applied by `lower_dump_call`, because the compiler IS the implementation.
- **The capturing form is HOISTED to `m_pending_stmts`**, not a statement
  expression — `({…; &tmp;})` is not an lvalue.
- **The sink is an explicit first parameter** on every primitive, one writer
  owning stdout-vs-buffer.
- **`src/rt/rt_dump.h` is the ONE dump contract** — flavor discriminator, column
  geometry, every prototype. `rt_dump.c` includes it, so the extern-"C"
  prototypes are compiler-checked against their definitions.
- **The `madc::value` walk is C++ in `src/rt_dump_value.cpp`, NOT in `src/rt/`.**
  Its array/object kinds are backed by C++ containers, so a walk needs the C++
  runtime wherever it lives, and `forest_ledger_gate.sh` leg 6 already asserts a
  value-holding program refuses the ledger path. Putting it in `rt_dump.c` would
  break dumping of ORDINARY C types in the lane that rule protects. Plan §14.1.
- **`var_dump` names a value's KIND** (`madc::value::kind_name`), so PHP's
  int/float/bool read as integer/real/boolean; `null` keeps PHP's `NULL`.
  `print_r` diverges from PHP nowhere. Plan §14.3.
- **`*RECURSION*` in the value walk is implemented and UNREACHABLE today** —
  `unique_ptr` value semantics make a cycle unconstructible. Only its FORMAT is
  gated. Do not "fix" the guard as dead code; it becomes reachable with the
  refcounted-cell backing, and the pointer slice needs the same facility.
- **Making the script `value` BE the 32-byte `madc_value`: RECORDED, NOT
  SCHEDULED** (owner: *"I don't believe it's necessary to take on now"*). Measured
  write-up: `docs/plans/2026-06-12-type-table-value-abi-design.md` §9.

## 6. OPEN DEFECTS

- **The fan14 SIGSEGV, not root-caused** — plan §16.1. Reducer
  `tmp/probe/fan14.mad`. A flat 32k-statement function is fine, so it is not a
  size limit.
- **D1** — `value f()` by value emits `int f()`, runs, prints nothing, exits 0.
  Layer: `func_def` return arm → `type_list` → `append_type_specs` (no `dtARRAY`
  case → `int`). Reducer `tmp/r1.mad`. Immediate honest fix: make the
  fall-through LOUD.
- **D2** — assigning to a `value &` PARAMETER is wrongly rejected. Reducer
  `tmp/r3.mad`.
- **A CAPTURED `bytes` value truncates at an embedded NUL** — the direct-print
  path is binary-exact, but `print_r($x,true)` returns through
  `madarray_assign_cstr`. Unreachable today (no script builds a `bytes` value);
  the fix is a length-carrying assignment, i.e. the value-ABI work. Plan §14.4.
- **Object-kind key ORDER** is key order (`std::map` backing) where PHP preserves
  insertion order — deliberate, documented, oracle written in key order so the
  difference stays visible.
- macOS: both darwin arches ship NO MIR module cache; gated at
  `darwin mir-blob-skips 1`. arm64 `wrong result type in proto proto138`;
  x86-64 `duration<double,nano>::operator%=` lowers `%` to integer `umods` on a
  floating operand — fix the LOWERING.
- README's EXE/OBJ counts: now **measured** at 1036/0 (they were PREDICTED
  1031/0). README still says 1031 — correct it with the next release.

## 7. STANDING CONSTRAINTS

- **The QNAP NAS never builds or tests.** Everything via
  `scripts/remote_build.sh` (container, `ssh -p 2299 dev@localhost`). One heavy
  job at a time. `php` 8.3.6 is on the container — it is the dump oracle; capture
  with `cat -A` and never retype from memory.
- MIR c-tests after any `third_party/mir` change:
  `sh c-tests/runtests.sh c-tests/use-c2m-gen /workspace/madc/obj/mir/host/c2m`.
- ⚠️ **`grep -r` here is a ugrep shim with `--ignore-files`**, and `.gitignore`
  lists `claude_status.json` (which is TRACKED), so recursive grep SILENTLY SKIPS
  it. Use **`git grep`** for any coverage sweep.
- ⚠️ **Never edit sources while a container suite runs** — the sync delivers an
  older preserved mtime, make skips it, and a link fails on a symbol whose source
  clearly has it. `touch` edited sources, or wait.
- Push only to `derekbsnider/*`; `origin` is not first in `git remote -v`.
- No `&&` chains; no backticks in a `-m` message; never `git add -A`.
- Scratch and reducers in `tmp/` (gitignored).
- Every `src/`/`include/` commit carries `Hypothesis:`/`Layer:`/`Searched:`/
  `Oracle:` — `scripts/check-rule-trailers.sh` gates it.
- **Do not re-run suites on already-green content.** Source changes invalidate a
  green; docs-only changes do not.
