# HANDOFF — the php dump intrinsics arc (INCOMPLETE), session #100, 2026-08-17

**Read this fully before acting. Assume a cold start.** Run `bash scripts/resume.sh`
first — it prints live git state and orphaned background jobs, which a compaction
summary cannot. Then read `claude_status.json` (canonical) and this file.

`docs/plans/2026-08-17-post-v0.84.0-HANDOFF.md` is **CLOSED** — its §3 (the php
dump arc) is delivered for scalars through positional containers. Do not work
from it; its §4 (the two macOS MIR-blob causes) is carried forward here as §4.

---

## 1. Where things stand

**The dump arc is merged to `develop` and carries NO version bump.** VERSION
stays **0.84.0**; the CHANGELOG entry sits under `[Unreleased]`.

> **OWNER, 2026-08-17:** *"I don't mind the merge, I just don't want a version
> bump until it's done."* So the merge is fine and the release is not. Do not cut
> a version for this arc until §3's slices are actually finished — and note this
> refines the standing release-cadence rule ("a feature merge to develop comes
> WITH a release"): that applies to a COMPLETE feature. An incomplete arc merged
> as a checkpoint stays unreleased.

`master` still carries **v0.82.0**; v0.83.0 and v0.84.0 are released on
`develop` and unpromoted, deliberately (§5).

Working tree clean. Untracked `donut.c`, `test.mad`, `testsort.mad` are the
owner's, **not ours to commit**.

## 2. Validation of record (the merged tree)

| lane | result |
|---|---|
| Linux fulltest (JIT) | **1070 / 0 / 0TO / 9 skip** |
| dump + foreach + string + vector + subscript families | **36 / 0** in JIT, `--exe` and `--obj` |
| `--emit=c11` → `gcc -O0` on the scalar dump test | output identical to the JIT run |
| warnings | 0 under `-Werror` |

Six new tests, all PHP-oracled where PHP can express the value:
`testphpprintr`, `testphpprintrstruct`, `testphpvardump`, `testphpseq`,
`testforeachkeyed`, `testforeachrefindex`.

The PHP captures live in `docs/plans/2026-08-17-php-print-r-var-dump-plan.md` §2
(and `tmp/or_pr*.php`, `tmp/or_vd.php`, `tmp/or_seq.php` in the container). They
were taken with `cat -A`; do not retype them from memory, and do not "tidy" the
8-space nested step or the blank line after a nested `print_r` block.

`php-cli` is now in `scripts/provision_container.sh` (`PKGS_oracle`) and in its
`BINS` check: it is an ORACLE like g++ and clang++, and nothing madc BUILDS needs
it — which is exactly why its absence would have read as green.

## 3. NEXT WORK — the dump arc's remaining slices

The arc is **partial by design**: every uncovered type is refused by name with
`no dumper for type 'X' yet`, so nothing lies. `docs/plans/2026-08-17-php-print-r-var-dump-plan.md`
is the contract, and its **§12 is current wherever it contradicts the design** —
read that section before starting any of these.

In the order §12 recommends:

- **S3c — pointers, with PHP's `*RECURSION*`.** The blocker is structural: the
  walk is EXPANDED at compile time, so `struct Node { Node *next; }` would
  recurse forever. Following a pointer needs (a) a GENERATED dumper FUNCTION per
  pointee type, which makes the column a runtime parameter instead of a literal,
  and (b) a runtime **ancestor STACK**, not a visited set — PHP prints the same
  value twice when it appears twice and says `*RECURSION*` only for a cycle, so
  the test is "is this address currently being printed", pushed and popped around
  each aggregate. Nothing landed so far blocks either; the primitives already
  take `col` as an ordinary `int`.
- **S1 — the `begin()`/`end()` protocol.** Owed regardless of the dumper:
  `for (auto &kv : m)` does not work today (`member reference is not a structure
  or union`) and `for (int v : std::set<int>)` errors where g++ runs. It also
  unlocks S5b.
- **S5b — associative rendering** (`[key] => value`), gated on S1.
- **S6 — `madc::value` / `array`.** The ONE type that wants a RUNTIME walker: it
  carries its own kind, so its walk must switch on that at runtime. It cannot
  live in `src/rt/rt_dump.c` (the ledger's rule is strict C11 with no C++
  dependency, and the value API is C++). Decide between generated code driving
  the existing extern-C value API in a loop, and a Tier-B runtime function that a
  `-static-libmadc` Mach-O program cannot link. Check whether a kind query is
  exported before choosing.

**`print_r($x, true)` — the diagnosis below CORRECTS what this handoff said
before. Read it and ignore any earlier "second overload" framing.**

**OWNER, 2026-08-17:** *"php.net/print_r defines print_r as
`function print_r(mixed $value, bool $return = false): string|true` … so it's
not two functions, it's one function with a default value for the second
parameter."* Correct, and it dissolves the blocker I had recorded (that a second
declaration's return type would lose to the first's). There are no overloads
here to collide. The faithful mapping is direct:

| PHP | madc |
|---|---|
| `mixed $value` | `template<class T> … (const T &v` |
| `bool $return = false` | `, bool ret = false)` |
| `string\|true` | `value` — madc's mixed type holds either the string or `true` |

`value` is not an awkward stand-in for the union return; a union-typed result is
what `value` IS (v0.75.0, `project_value_intrinsic`).

**The real blocker, verified in the code rather than reasoned about:**

1. **The placeholder is minted with ZERO parameters, so there is nowhere for a
   default argument to live.** `register_skipped_namespace_template_function`
   (`src/parser.cpp:50433`) ends at
   `pgm.addFunction(parse_id, datatype_vec_t{ret ? ret : &ddINT64}, NULL)` — and
   `addFunction`'s second argument is the DATATYPE VECTOR
   (`include/madc.h:4889`), here a single element holding only the return type.
   No parameters ⇒ `FuncDef::param_defaults` (`include/madc.h:221`) is empty ⇒
   the call-site default fill that already exists for free functions
   (`src/parser.cpp:25199`) and methods (`:18063`) has nothing to fill from.
   **The information is already available at that exact point:**
   `capture_free_function_overload` runs a few lines earlier and captures every
   declaration's signature so "the call site can select by arity". So the fix is
   to carry the declared parameters and their defaults onto the placeholder — not
   to invent new state, and not a new arity mechanism.
2. **Still genuinely open: can a madc function return `value` BY VALUE?** Every
   value-returning entry in `<ns_madc>` is `value &f(value &out, …)` —
   out-parameter style throughout, which suggests the by-value path was never
   needed and therefore never proven. Verify before designing on it. If it does
   not work, that is a defect to fix at the ABI, not a reason to change PHP's
   signature.
2. ~~`var_dump`'s container type word~~ — **DONE.** The owner ruled the
   long canonical spelling unacceptable and it is fixed: `std::string(2) "hi"`,
   `std::vector<std::string>(2)`. The mechanism (inverting
   `Program::namespace_datatype_map` by type IDENTITY, plus a sequence's element
   type from `operator[]`) and its two traps are in plan §12.10 — read it before
   touching the type words again. Still OPEN and separate: a scalar's word is the
   canonical type, so a `size_t` shows as `unsigned long`; and `std::array`'s
   extent is not in the word (the count carries it).

## 4. THEN — the two macOS MIR-cache blob causes (carried from v0.84.0)

**Both darwin arches ship with NO MIR module cache**, while linux packs 467 KB and
win64 497 KB. Correctness survives (consumers fall back), which is why the pack
exits 0, but every consumer compile on macOS pays full compile price. Gated at
`darwin mir-blob-skips 1`, so it cannot grow or be forgotten.

```
arm64:   MIR error during module compile: wrong result type in proto proto138
x86-64:  func duration_double_std____1__ratio_1_1000000000___operator%=:
         in instruction 'umods': unexpected operand mode for operand #1.
         Got 'ldouble', expected 'int'
```

- **x86-64** is the tractable one. `%` on a floating duration rep is ill-formed
  C++, so two candidate layers: (a) whichever pass speculatively instantiates
  `duration<double, nano>::operator%=` during the pack, and (b) the `%` lowering
  choosing an integer opcode from a floating operand type. **Fix (b) if it is
  (b)** — an integer modulo emitted for a floating operand is wrong wherever it
  comes from.
- **arm64** names no source function; dump the module to localize `proto138`.

Lower the baseline to 0 in the same commit that fixes them.

Then: **#60** macOS headerless cell (`sandbox-exec` denying the SDK subpath),
**#25** lazy decompression, **#61** pointer-to-member-function remainder, **#56**
follow-ons, **#55**, **#49**.

## 5. Promotion — still an owner decision, now with three releases behind it

`master` = v0.82.0. A promote wants platform evidence, and the Windows/macOS
*suites* were last run at v0.82.0:

```
bash scripts/remote_build.sh release-win wine headerless-win    # Windows
bash scripts/remote_build.sh release-macos                      # macOS packs
# then the Mac battery on hardware: derek@192.168.1.79, LC_ALL=C
```

**Do not promote without asking.** When the Mac battery does run, leg 3c reports
darwin's DK_NONE and closure-drop census — record those numbers in
`docs/plans/2026-08-17-pack-degradation-gate.md` §5, which still says
"unmeasured".

## 6. The dump arc's deliberate limits — do NOT "fix" these silently

Each is a refusal with a reason, and each has a named enabler:

- **A base member shadowed by a same-named derived one is skipped.**
  `class_struct_members` renames the hidden one to `<name>__flatN` and that rule
  has **no reader** (`grep __flat` = one site, the emitter). Lifting it needs ONE
  owner for "the emitted field name of member i", shared by the emitter and any
  reader. That refactor changes every emitted struct — do it deliberately, with
  the suite as the oracle.
- **Multidimensional arrays are refused.** `member_counts` holds the FLATTENED
  total and `m[i]` on an `int[2][3]` yields a ROW, so a flat walk would index past
  the first row and print adjacent storage. PHP renders a nested array; both want
  the dim chain (`member_dims` / `Variable::dims`), which the walk does not carry.
- **An aggregate argument must be a variable or a member selection.** The walk
  rebuilds the access once per member (a c2mir node has one parent), so a
  struct-returning CALL would be evaluated once per field. Refused out loud.
- **`madc::value` is excluded from the sequence test on purpose.** It is a class
  WITH registered methods (its `c_str` is `madarray_cstr`), so it would pass a
  structural sequence test and then be called through the wrong runtime.

## 7. Traps this session added to the record

- **Editing on the NAS while a container suite runs poisons make mtimes.** The
  running fulltest built its win64 objects from the sources synced at its start;
  a later sync delivered an edited `cir_builder.cpp` with an OLDER (preserved)
  mtime, so `make hosted-x86-64-windows` skipped it and the win64 link failed
  with `undefined reference to CirBuilder::class_index_iteration_protocol` —
  while the Linux lanes were green. Same class as the known
  container-side-`git checkout` trap. **Remedy: `touch` every edited source
  before the next sync**, or do not edit during a run. Tell: `nm -C <obj> | grep
  <new-symbol>` finds nothing while the source clearly has it.
- **A statement expression's LAST statement must be an expression.** c2mir
  rejects one ending in a `for` loop ("last statement in statement expression is
  not an expression"), which a dump legitimately does (print_r of a text
  container at top level emits only the character loop). `lower_dump_call` closes
  every dump with a discarded `0` so no shape has to remember.
- **`is_integer()` is TRUE for a pointer, a pointer-to-data-member and a function
  pointer**, and a SIMD vector's follows its element. Any "is this a number" test
  must exclude them FIRST or it prints an address as a decimal.
- **A POD `struct` is not a `DataDefCLASS`** — madc promotes one only when it
  earns class-hood. An aggregate walk must key on `DataDefSTRUCT`.

## 8. Standing constraints (do not relearn these)

- The **QNAP NAS never builds or tests**. Everything goes through
  `scripts/remote_build.sh` (container, `ssh -p 2299 dev@localhost`). One heavy
  job at a time — it refuses concurrent runs and checks BEFORE syncing.
- **Push only to `derekbsnider/*`** — verify the URL. This tree has `mir-pr` and
  `mir-upstream` remotes; `origin` is not first in `git remote -v`.
- No `&&` chains or shell variable substitution in a single Bash tool call, and
  no backticks in a `-m` message.
- Never `git add -A`; untracked files here are usually not ours.
- Scratch/reducer files go in `tmp/` (gitignored).
- Every `src/`/`include/` commit carries `Hypothesis:` / `Layer:` / `Searched:` /
  `Oracle:`; `scripts/check-rule-trailers.sh` gates it.
- **Do not re-run suites on already-green content.** A source or freeze-FORMAT
  change invalidates prior greens; docs-only does not.

## 9. Pre-merge `/dupaudit` — two families, neither divergent

Scope: the branch diff's code files — `src/cir_dump.cpp` (new), `src/cir_builder.cpp`
class-call builders, `src/parser.cpp`'s classifier, `src/rt/rt_dump.c` (new).
Neither finding is divergent, so neither blocked the merge. Both are planned work
**with a gate**, which is the only form a consolidation ships in here.

1. **`template_id_head_split` (KG, open, 3 sites).** One rule — "is this spelling a
   template-id, and what is its head" — has an owner:
   `include/spelling_delim.h:79 split_template_id_parts`, which scans with
   `SpellingDelimDepth` and takes the first `<` **at angle depth 1**.
   `src/cir_dump.cpp:162-165` hand-rolls the head as `word.substr(0, word.find('<'))`,
   and lines 204 / 244 use a bare `find('<')` as the template-id test.
   **No divergence today** — the first `<` in a well-formed type spelling IS at depth
   1 — so this is redundancy, not a bug. It matters because it is the **third
   recurrence** of the recorded `char_level_angle_scanning` family, whose own KG note
   already says `check-one-delim-tracker.sh` keys on variable NAMES; a bare `find('<')`
   has no counter at all, so the gate cannot see it by construction, and fulltest was
   green. Fix: call `split_template_id_parts(word, head, args, SpellingTail::Ignore)`
   and use its bool as the test and `head` as the head. Gate: extend
   `check-one-delim-tracker.sh` to flag a bare first-`<` find on a spelling outside
   `include/spelling_delim.h`, using the existing allowed-exception marker convention.

2. **`var_emit_name_bypass` — the family is clean, the GATE is too narrow.** It did
   NOT grow: `cir_dump.cpp` routes every variable access through `translate_expr(arg)`,
   and its own `id()` calls are on locally synthesized loop counters and length
   temporaries, never a `Variable`'s name. But
   `scripts/check-var-emit-name-bypass.sh` hard-codes `src/cir_builder.cpp` in all four
   of its greps (lines 50, 62, 68), and CIR emission is now **two** files. A gate that
   names one file cannot gate a subsystem — make it iterate `src/cir_*.cpp`. Until then
   the next file added here is unguarded by construction, which is exactly how the
   sixth angle-bracket scanner was born.

**Dropped (the third slot), and why:** `rt_dump.c`'s `__madc_dump_pr_*` /
`__madc_dump_vd_*` pairs look like a dozen duplicated primitives, but they fail the
tie-breaker — PHP's two renderers differ per primitive **by design**, and merging them
would push a flavor branch into every output function. Recommending that merge would be
worse than the redundancy.
