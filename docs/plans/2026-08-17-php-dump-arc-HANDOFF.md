# HANDOFF — php dump intrinsics arc, sessions #100–#103, 2026-08-18

**Read this fully. Assume a cold start.** Run `bash scripts/resume.sh` first (live
git state + orphaned jobs), then `claude_status.json`, then this file, then
`docs/plans/2026-08-17-php-print-r-var-dump-plan.md` §14–§18.

Supersedes the session-#102 revision.

---

## 1. STATE

- Branch `develop`. **VERSION = 0.84.0.** `master` = v0.82.0.
- **No release cut yet.** The owner's standing ruling from #101 — *"I don't want a
  version bump until it's done"* — still applies, and §2 shows ONE shape still
  uncovered (`std::map` / `set` / `list`). Read §3 before deciding.
- Working tree clean except the owner's untracked `donut.c`, `test.mad`,
  `testsort.mad` — **never commit them**.
- ⚠️ `git stash@{0}` still holds the SUPERSEDED compile-time pointer expansion.
  It is now dead: plan §17 is what shipped. **Drop it** rather than read it,
  except for the record in §16.1.

### Landed in session #103 (four commits, all green together)

| commit | what |
|---|---|
| `eaac1416` | a `madc::value`'s base type is `long long`, not the `int` it fell through to — `append_type_specs` had no `dtARRAY` arm, so `value *p` emitted `int *p`; plus the `&v` decay that then became necessary |
| `0785bb84` | **`print_r`/`var_dump` FOLLOW a pointer** — a generated dumper FUNCTION per (pointee, flavor) + a shared runtime ancestor stack. Rings, self-pointers, mutual rings, shared-acyclic, null, `T**`, pointer members |
| `647ff2dd` | **a `madc::value` is 16-ALIGNED** — `DataDefARRAY` reported alignment 1, so a `value` member landed at offset 4 where gcc/clang put it at 16. Silent at -O0; SIGSEGV in the packed -O2 lane |
| `07a9b79c` | **an enum knows its own enumerators** — `DataDefENUM::enumerators` is the one owner; PHP 8.1's enum shape with the real backing type |

### Validation of record — the exact content those four commits ship

| lane | result |
|---|---|
| fulltest | **1080 / 0 fail / 0 timeout / 9 skip** |
| `--exe` | **1040 / 0** |
| `--obj` | **1040 / 0** |
| release | rc=0 |
| packed (`madc-release`) | **1080 / 0** |
| headerless | **1054 / 0 / 35 skip** |

Zero warnings under `-Werror`; trailer gate 451/0. Logs `tmp/logs/rb-20260818-*.log`.
`third_party/mir` was NOT touched this session, so the MIR c-tests (1143 tests /
2286 successes / 0 failures) stand from #102. wine / macOS batteries unchanged
since v0.82.0 and NOT re-run.

⚠️ **A note on the history:** `647ff2dd` was committed once with the hunk spliced
into `DataDefSIMD` by a `git apply --unidiff-zero` split, then reset and redone
from the file. If you are auditing, the current commit is correct and the working
tree was proven byte-identical to the validated content afterwards. **Do not
split a diff with `--unidiff-zero`** — zero context cannot verify placement.

## 2. MEASURED COVERAGE — re-run, never summarise

`bash tmp/patch/coverage.sh` on the container re-measures `tmp/probe/p*.mad` and
prints OK / REFUSED / WARNED per shape. The session-#101 release was cut on a
one-slice "done" and had to be reverted; the standing directive is **report
coverage, not slices**.

| shape | result |
|---|---|
| scalar, `const char *` | **OK** |
| struct / class (access, inheritance, unions, anon unions, bit-fields) | **OK** |
| fixed array, `char[]` as text | **OK** |
| multidimensional array (2-D, 3-D) | **OK** |
| `std::vector`, nested vector, `std::string` | **OK** |
| `print_r($x, true)` (capture, runtime flag, scalar capture) | **OK** |
| `madc::value` / `array` — all nine kinds | **OK** |
| **pointer, pointer member, `T**`, null, ring, self-ref, mutual ring, shared-acyclic** | **OK** (#103) |
| **enum** — plain, fixed base, scoped, class-nested, duplicate value, unnamed value | **OK** (#103) |
| **`std::map` / `set` / `list`** | **REFUSED by container name** — the ONE remaining shape. See §3. |

17 of 20 probes OK; the 3 refusals are all the associative/list containers, and
each names ITSELF rather than a libstdc++ internal.

## 3. NEXT — S1, the iterator protocol. It is the last shape AND a language gap.

`std::map` / `set` / `list` are refused because `dump_sequence` needs a POSITIONAL
`size()` + `operator[]`, which they do not have. The honest path is the iterator
twin of the existing predicate — and **it is owed regardless of the dumper:**
`for (auto &kv : m)` does not work today either.

Do NOT reach into `_Rb_tree` instead. That is exactly what commit `69b910ec`
(session #102) was written to stop; the by-name refusal is the correct behaviour
until the protocol exists.

### 3.1 MEASURED in #103 — three findings that CHANGE the design

Run these before designing anything; they are cheap and they redirect the work.

1. **The explicit-iterator loop ALREADY WORKS on a `std::map`.** `tmp/probe/mapiter_a.mad`:
   `for (std::map<int,int>::iterator it = m.begin(); it != m.end(); ++it)` with
   `it->first` / `it->second` runs and prints correctly. So begin/end/compare/
   increment/arrow on a real libstdc++ tree iterator are NOT the gap.
2. **What IS missing is `auto` deduction, in two places.**
   `for (auto &kv : m)` fails with *"member reference type 'auto' is not a
   structure or union"* (the range-for's element type is never deduced), and
   `auto it = m.begin(); ++it;` fails to PARSE — *"Expecting ';' after for
   condition"* on the `++it`. `tmp/probe/mapiter_c.mad`, `tmp/probe/mapiter.mad`.
   Those two are the language half of S1 and are independent of the dumper,
   which knows the iterator type and needs no `auto`.
3. ⚠️ **libstdc++ declares `operator==` / `operator!=` on EVERY one of these
   iterators as FRIEND FREE functions, not members** — `stl_tree.h` lines
   315/320 and 396/401, `stl_list.h` 318/324 and 408/414. `findMethod` will not
   find them, so **a member-dispatch `it != end()` cannot be generated at all.**
   madc does have free-operator machinery (`Program::free_operator_overloads`,
   `instantiate_free_operator_template`, `CirBuilder::std_free_operator_instantiation`)
   but every entry point is TOKEN-driven, and the dumper has no token.

**So the generated loop should be COUNTED, not comparison-based:**

```
long n = c.size();  It it = c.begin();
for (long k = 0; k < n; ++k) { <use *it>; ++it; }
```

`operator*` and prefix `operator++` ARE members (verified at the same lines), so
this is entirely member dispatch. It is not a shortcut: var_dump's head line
states the count anyway, so `size()` is already required, and the loop shape is
the one `dump_sequence` emits today. ⚠️ `operator++` needs ARITY selection —
prefix and postfix are both spelled `operator++` and differ only in the postfix
dummy `int`, so `findMethod` alone picks either.

### 3.2 The remaining design question — capture the oracle FIRST

A `std::map`'s element is `std::pair<const K, V>` and PHP renders a map as
`[key] => value`, NOT as a pair. So the keyed containers need the key rendered
BETWEEN `[` and `] => `:

- An INTEGRAL key already works with no new primitive: `dump_key_idx` takes a
  runtime `long long` (`__madc_dump_pr_key_idx`).
- A STRING key does not. It needs an inline-key primitive PAIR
  (`pr_key_open` / `pr_key_close` plus the var_dump twins) so the key's own walk
  can render between them — and then the "does a key owe print_r's end-of-entry
  newline" rule has to be answered NO for a key, which `dump_pr_end_entry`
  currently decides from depth alone.
- var_dump QUOTES a string key (`["name"]=>`) and not an integral one (`[1]=>`),
  and the compiler knows which — so the quote is a compile-time flag, not a
  runtime test.

Distinguish keyed from positional STRUCTURALLY, never by name:
`class_has_type_alias(cls, "mapped_type")` is true for `std::map` and false for
`std::set` / `std::list` (both of which therefore render POSITIONALLY, exactly
like a vector — PHP has no set, and a list of values is the honest form).

**The layer chain:**

- `CirBuilder::class_index_iteration_protocol` (`src/cir_builder.cpp` ~22059) is
  the model to copy: a TYPE-CHECKED structural predicate, no method matched by
  name-only, shared by the range-for and the dumper.
- S1 adds its twin: `begin()`/`end()` nullary, returning the SAME class type,
  which itself has `operator!=`, `operator++` and `operator*`. Type-checked, for
  the same reason the index one is — a by-name match is what made
  `for (int v : map)` SIGSEGV in the first place.
- `container_needs_iterator_walk` (`src/cir_dump.cpp`) is the refusal site. Its
  BODY becomes that predicate and the call site starts dumping.
- Generating the loop needs an iterator OBJECT local plus operator calls on it.
  `class_nullary_call` already owns the nullary-method call; the object-temp
  machinery is `object_arg_addr`'s `__madc_objtmp`.
- Then **S5b**: a map's `*i` is a `std::pair<const K, V>`, and PHP renders a map
  as `[key] => value`, not as a pair — so the dumper must use the pair's `first`
  as the PHP KEY. Capture the oracle from php-cli 8.3.6 before writing it.

**This was NOT attempted in #103** — it is a language feature with its own tests
across every lane, not a dumper slice, and starting it would have put four
validated commits at risk. It is the next thing.

## 4. THEN — in this order

- The two macOS MIR-blob causes (§6).
- Then #60 / #61 / #25 / #56 / #55 / #49.

## 5. SETTLED — do not re-litigate

- **`php::print_r` returns `madc::value &`**, not by value. Signature
  `template<class T> madc::value &print_r(const T &v, bool ret = false)`.
- **The bodyless placeholder's ZERO arity is load-bearing** (it is arity-filtered
  out of overload ranking). The default argument is applied by `lower_dump_call`,
  because the compiler IS the implementation.
- **The capturing form is HOISTED to `m_pending_stmts`** — `({…; &tmp;})` is not
  an lvalue.
- **The sink is an explicit first parameter** on every primitive, and on every
  generated dumper — which is why ONE generated function serves a printing dump
  and a capturing one, and the memo needs no sink in its key.
- **`src/rt/rt_dump.h` is the ONE dump contract** — flavor, column geometry, the
  ancestor stack, the tag namespace, every prototype.
- **The `madc::value` walk is C++ in `src/rt_dump_value.cpp`, NOT `src/rt/`.**
  Plan §14.1 has the whole argument (the ledger lane).
- **The ancestor stack is a STACK, not a visited set, and is keyed on
  (address, TYPE).** Both halves are load-bearing and both have oracles: PHP
  prints a twice-reachable object in full both times, and without the type
  `struct T { int v; int *p; }` with `p = &t.v` reports a false cycle. Plan §17.2.
- **A pointer renders the pointee at the SAME depth** — an indirection is not a
  nesting level.
- **`var_dump` names the REAL C type**; `print_r` diverges from PHP nowhere.
- **An enum's enumerators belong to `DataDefENUM`**, and `forest_record_enum`
  READS that owner. Do not reintroduce the `namespace_map` reverse lookup.
- **Making the script `value` BE the 32-byte `madc_value`: RECORDED, NOT
  SCHEDULED** (owner: *"I don't believe it's necessary to take on now"*).

## 6. OPEN DEFECTS

- **D1** — `value f()` by value emits `int f()`, runs, prints nothing, exits 0.
  `eaac1416` fixed the SPECIFIER half of its root (`append_type_specs` now has a
  `dtARRAY` arm); the return DECLARATOR half is untouched. Reducer `tmp/r1.mad`.
- **D2** — assigning to a `value &` PARAMETER is wrongly rejected. Reducer
  `tmp/r3.mad`.
- **c2m cannot PARSE `_Alignas` on a struct member** — "syntax error on struct
  (expected '<declarator>')" — while gcc and clang both accept it and C23 puts
  alignment-specifier in specifier-qualifier-list. It does NOT affect the JIT
  (which builds `node_t` directly, where N_ALIGNAS on an N_MEMBER is handled and
  now correct), but it means `--emit=c11` output containing a value member cannot
  be recompiled by `c2m` itself. Reducer `tmp/or/align.c`. Tier 2 (raise c2mir).
- **The §16.1 fan14 SIGSEGV is not root-caused, only unreachable.** Plan §17.4.
- **A CAPTURED `bytes` value truncates at an embedded NUL** — unreachable today;
  the fix is the value-ABI work. Plan §14.4.
- **Object-kind key ORDER** is key order where PHP preserves insertion order —
  deliberate, documented, oracle written in key order.
- macOS: both darwin arches ship NO MIR module cache; `darwin mir-blob-skips 1`.
  arm64 `wrong result type in proto proto138`; x86-64
  `duration<double,nano>::operator%=` lowers `%` to integer `umods` on a floating
  operand — fix the LOWERING.
- README's EXE/OBJ counts say 1031; **measured 1040**. Correct with the next
  release.

## 7. STANDING CONSTRAINTS

- **The QNAP NAS never builds or tests.** Everything via
  `scripts/remote_build.sh` (container, `ssh -p 2299 dev@localhost`). One heavy
  job at a time. `php` 8.3.6 is on the container — it is the dump oracle; capture
  with `cat -A` and never retype from memory. Oracles live in `tmp/or/`.
- **A WARNING IS ALWAYS YOURS TO FIX** (owner, this session). There is no
  "pre-existing" disposition for one. Both new dump tests carry `.expect_quiet`,
  so stderr must be EMPTY — that is the gate that keeps it true.
- MIR c-tests after any `third_party/mir` change:
  `sh c-tests/runtests.sh c-tests/use-c2m-gen /workspace/madc/obj/mir/host/c2m`.
- ⚠️ **`grep -r` here is a ugrep shim with `--ignore-files`** — use **`git grep`**
  for any coverage sweep.
- ⚠️ **Never edit sources while a container suite runs** — the sync delivers an
  older preserved mtime and make skips it. `touch` edited sources, or wait.
- ⚠️ **Never split a diff with `git apply --unidiff-zero`.** Zero context cannot
  verify placement; it silently spliced a hunk into the wrong class this session.
  Split by rebuilding the file instead.
- Push only to `derekbsnider/*`; `origin` is not first in `git remote -v`.
- No `&&` chains; no backticks in a `-m` message; never `git add -A`.
- Scratch and reducers in `tmp/` (gitignored).
- Every `src/`/`include/` commit carries `Hypothesis:`/`Layer:`/`Searched:`/
  `Oracle:` — `scripts/check-rule-trailers.sh` gates it.
- **Do not re-run suites on already-green content.** Source changes invalidate a
  green; docs-only changes do not.
