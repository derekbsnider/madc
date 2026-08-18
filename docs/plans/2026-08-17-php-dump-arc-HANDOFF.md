# HANDOFF — php dump intrinsics arc, sessions #100–#104, 2026-08-18

**Read this fully. Assume a cold start.** Run `bash scripts/resume.sh` first (live
git state + orphaned jobs), then `claude_status.json`, then this file, then
`docs/plans/2026-08-17-php-print-r-var-dump-plan.md` §14–§19.

Supersedes the session-#103 revision.

---

## 1. STATE

- Branch `develop`. **VERSION = 0.85.0, RELEASED on develop.** `master` = v0.82.0; v0.83.0, v0.84.0 and v0.85.0 are all unpromoted.
- ⚠️ **NOT PUSHED.** develop is 40+ commits ahead of `origin/develop`. Verify the remote is `derekbsnider/madc` before any push (`git remote -v` — origin is NOT first).
- Working tree clean except the owner's untracked `donut.c`, `test.mad`,
  `testsort.mad` — **never commit them**.
- ⚠️ `git stash@{0}` still holds the SUPERSEDED compile-time pointer expansion
  from #102. It is dead: plan §17 is what shipped. **Drop it.**

### 1.1 Validation of record — v0.85.0

| lane | result |
|---|---|
| fulltest | **1084 / 0 fail / 0 timeout / 9 skip** |
| `--exe` | **1044 / 0** |
| `--obj` | **1044 / 0** |
| release | rc=0 |
| packed (`madc-release`) | **1084 / 0 / 0 TO / 9 skip** |
| headerless | **1058 / 0 / 35 skip** |
| c2mir warning ratchet | **1092 tests compiled, 0 warnings**, all-zero baseline |
| trailer gate | 457 code commits, 0 missing |

`--emit=c11` parity spot-checked for the new walk: the generated C compiles under
`gcc -std=c11` and its output is byte-identical to the JIT's
(`tmp/probe/emitc_iter.mad`). MIR c-tests stand from #102 (`third_party/mir`
untouched). wine / macOS batteries unchanged since v0.82.0 and NOT re-run.

### 2. THE ARC IS COMPLETE — 20 of 20 probe shapes render

**`php::print_r` / `php::var_dump` now render every shape the probe set covers.**
The last gap (`std::map` / `std::set` / `std::list`) closed in session #104.

Re-measure, never summarise: `bash tmp/patch/coverage.sh` on the container.
(The session-#101 release was cut on a one-slice "done" and had to be reverted;
the standing directive is **report coverage, not slices**.)

| shape | result |
|---|---|
| scalar, `const char *` | OK |
| struct / class (access, inheritance, unions, anon unions, bit-fields) | OK |
| fixed array, `char[]` as text | OK |
| multidimensional array (2-D, 3-D) | OK |
| `std::vector`, nested vector, `std::string` | OK |
| `print_r($x, true)` (capture, runtime flag, scalar capture) | OK |
| `madc::value` / `array` — all nine kinds | OK |
| pointer, pointer member, `T**`, null, ring, self-ref, mutual ring, shared-acyclic | OK (#103) |
| enum — plain, fixed base, scoped, class-nested, duplicate value, unnamed value | OK (#103) |
| **`std::map` / `std::set` / `std::list`** — int + string keys, nested, struct/string values, empty, in a struct, through a pointer, captured | **OK (#104)** |
| **hand-rolled container** — class iterator AND raw-pointer iterator | **OK (#104)** |

The ONE remaining refusal is principled and permanent: **a container with
`begin()`/`end()` and no `size()`.** The generated loop is COUNTED off `size()`
because libstdc++ declares the iterator's `operator!=` as a friend FREE function
(`stl_tree.h` 320, `stl_list.h` 324) — `findMethod` cannot see a free function, so
`it != c.end()` has no member to dispatch. `tests/testphpdumprefuse.mad` is built
on that case and its `.expect_err` pins the message, which names the container by
its own name AND says which piece is missing.

## 3. WHAT LANDED IN #104 — four commits

| commit | what |
|---|---|
| `3c0de0da` | **a pointer comparison against a base subobject owes the base adjustment** — `B2 *p2 = &d; p2 == &d` answered 0 where gcc and clang answer 1. Found because libstdc++'s `_List_base::_M_clear` does exactly that and every std::list program printed a c2mir warning |
| `f68c4690` | **the dumper renders iterator containers** — the shared `class_iterator_iteration_protocol`, the counted loop, the inline-key primitive pair, plus two word defects (`std::__cxx11::list`, and a container word from the canonical spelling) |
| `582d9eed` | **the range-for is the recognizer's SECOND consumer** — `for (std::pair<const int,int> &kv : m)` over a std::map |
| (this session's fourth) | **a type that contains ITSELF must not expand forever** — a self-referential container consumed 4 GB and died in `std::bad_alloc`; the guard is an ancestor set in the TYPE domain, two sets (walk and word), and the POINTER path is exempt because its memo already bounds it |

Plan §19 is the AS-BUILT record: §19.2 the recognizer, §19.3 why the loop is
counted, §19.4 the key, §19.5 the two word defects, §19.7 what is still not
covered.

### 3.1 The three things that are easy to get wrong here

1. **The recognizer lives in `src/cir_builder.cpp`, beside its positional twin** —
   not in `src/cir_dump.cpp`. It is a property of a CLASS and both the range-for
   and the dumper key on it. A dumper-private copy is the duplication this repo
   gates against.
2. **`class_nullary_call` selects the NULLARY overload** (via
   `findMethodOverload(name, {})`) and takes the symbol of THAT overload. Prefix
   and postfix `operator++` are the same spelling; a by-name pick is a coin flip.
   The by-name pick remains the fallback so alias-web lookups still resolve.
3. **A discarded reference result must NOT be dereferenced.** `++it;` emitted as
   `*f(&it);` is a dereference with no effect — a warning, and warnings are
   defects here. `class_nullary_call`'s `discard_value` flag exists for it.

## 4. OPEN DEFECTS — every one has a reducer

- **`size()` / `count()` on a NON-ARRAY-kind script `value` returns 0.**
  `add_array_methods` binds both to `madarray_size`, which is the RANGE-FOR length
  helper and deliberately reads 0 for a non-array kind ("Intentionally NOT
  ns_common::value_count", its own comment). So `value s = "hello"; s.size()` is 0
  while the payload has five bytes, and `php::print_r(x, true)` produces a value
  whose `size()` is 0 — which is why every existing capture test measures
  `strlen(v.c_str())` instead. `ns_common::value_count` is the owner
  `madarray_size`'s comment names. Reducer `tmp/probe/valuecount.mad`.
  ⚠️ NOT taken in #104 on purpose: what `size()` should mean across the nine kinds
  is a decision about the `value` INTRINSIC's user-visible surface, not a dump
  bug, and the owner has been shaping that surface.
- **`for (auto &kv : m)`** — the range-for's element type is never deduced from
  `auto`; the explicit-type form works. A parser deduction gap, independent of the
  iteration protocol. Reducer `tmp/probe/feauto.mad`.
- **`std::unordered_map` does not PARSE** — `hashtable_policy.h:613` uses
  `__int_traits`, reported as an undeclared identifier. Unrelated to the dump arc.
  Reducer `tmp/probe/refuse2.mad`.
- **`for (int v : plain_struct)`** over a struct with NO protocol reports c2mir's
  "conversion of non-scalar value requested" twice instead of a madc diagnostic:
  `class_behind` returns NULL for a struct that never earned class-hood, so the
  class arm (with its good message) is never reached. A loud compile error either
  way — a diagnostic-quality defect, not a wrong answer. Reducer
  `tmp/probe/feplain.mad`.
- **Cosmetic, but it should be tightened:** in the RAW-POINTER arm of
  `class_iterator_iteration_protocol`, `end()` is only checked for
  `is_pointer()`, while its decline message says "do not return the same iterator
  type". Compare the POINTEE DataDefs instead. There is no wrong answer behind it
  — the counted loop never calls `end()`, which is required purely as the
  structural signal that this is the iteration protocol — so it was left for the
  next commit that earns a battery rather than re-running one for a check with no
  behavioural effect. The CLASS arm already compares properly (`bc != ec`).
- **`--emit=c11` declares libc functions with the dlsym FALLBACK signature.** Any
  program that reaches `memcpy` / `strlen` through libstdc++ internals emits
  `extern long long memcpy();`, and `gcc -std=c11` warns
  `conflicting types for built-in function`. A plain `std::string` program is
  enough: reducer `tmp/probe/tinystr.mad` (2 warnings). The JIT is unaffected and
  the OUTPUT is byte-identical, so it is a warning-not-wrong-answer — but the
  zero-warnings law covers every lane, and this one is not in the c2mir ratchet
  because the ratchet measures the JIT path. `.claude/rules/embedded-headers.md`
  already states the rule this violates ("declare real return types — never rely
  on the fallback for signed int"); these two just have no declaration on the
  path libstdc++ takes. Pre-existing, independent of the dump arc.
- **D1** — `value f()` by value emits `int f()`, runs, prints nothing, exits 0.
  `eaac1416` fixed the SPECIFIER half (`append_type_specs` has a `dtARRAY` arm);
  the return DECLARATOR half is untouched. Reducer `tmp/r1.mad`.
- **D2** — assigning to a `value &` PARAMETER is wrongly rejected. Reducer
  `tmp/r3.mad`. (Also hit head-on writing #104's probes: `madc::value &cap =
  php::print_r(x, true)` fails where `value cap = ...` works.)
- **c2m cannot PARSE `_Alignas` on a struct member** — gcc and clang accept it and
  C23 puts it there. A Tier-2 raise; the JIT is unaffected because madc's settled
  layout is what c2mir consumes.
- The `tests/testphpdumpptr.mad` fan-out SIGSEGV is unreachable now but was never
  root-caused.
- Object-key ORDER in a captured value dump.
- Two macOS MIR-blob causes.
- README says 1031 EXE/OBJ; measured 1040+.

## 5. SETTLED — do not re-litigate

- **`php::print_r` returns `madc::value &`**, signature
  `template<class T> madc::value &print_r(const T &v, bool ret = false)`.
- **The bodyless placeholder's ZERO arity is load-bearing** (arity-filtered out of
  overload ranking); the default argument is applied by `lower_dump_call`.
- **The capturing form is HOISTED to `m_pending_stmts`** — `({…; &tmp;})` is not
  an lvalue.
- **The sink is an explicit first parameter** on every primitive and every
  generated dumper — which is why ONE generated function serves a printing dump
  and a capturing one, and the memo needs no sink in its key.
- **`src/rt/rt_dump.h` is the ONE dump contract** — flavor, column geometry, the
  ancestor stack, the tag namespace, every prototype.
- **The `madc::value` walk is C++ in `src/rt_dump_value.cpp`, NOT `src/rt/`** (the
  strict-C11 ledger lane). Plan §14.1 has the argument.
- **The ancestor stack is a STACK, not a visited set, keyed on (address, TYPE).**
  Both halves oracle-backed. Plan §17.2.
- **A pointer renders the pointee at the SAME depth** — an indirection is not a
  nesting level.
- **`var_dump` names the REAL C type; `print_r` diverges from PHP nowhere.**
- **An enum's enumerators belong to `DataDefENUM`**, and `forest_record_enum`
  READS that owner.
- **`keyed` is decided by `mapped_type`**, structurally, never by class name.
  A `std::set` / `std::list` renders POSITIONALLY: PHP has no set, and a list of
  values is a list.
- **The container loop is COUNTED off `size()`** — see §2. Not a shortcut.
- **Making the script `value` BE the 32-byte `madc_value`: RECORDED, NOT
  SCHEDULED** (owner: *"I don't believe it's necessary to take on now"*).

## 6. NEXT

- The two macOS MIR-blob causes.
- Then #60 / #61 / #25 / #56 / #55 / #49.
- The `value.size()` decision in §4, when the owner wants the intrinsic surface
  touched.

## 7. TRAPS BANKED FROM THIS ARC

- ⚠️ **Never split a diff with `git apply --unidiff-zero`.** In #103 it spliced a
  hunk into `DataDefSIMD` instead of `DataDefARRAY` and produced an unbuildable
  commit. Zero context cannot verify placement. Rebuild the file instead.
- ⚠️ **The refusal message is a gate, so it must not overstate the gap.** #103's
  message said madc needed "a GENERATED begin()/end() loop" in a way that read as
  "madc cannot call begin()" — it always could. The owner caught it. Say what is
  missing, not what the walk does.
- ⚠️ **`tmp/` is NOT rsynced to the container.** `scripts/remote_build.sh sync`
  skips it, so a probe written locally must be `scp`'d (port 2299) before
  `bin/madc` on the container can see it. `tests/` IS synced.
- ⚠️ **The container refuses a second concurrent suite** and says so. Wait for the
  first; do not pass `MADC_ALLOW_CONCURRENT=1` to get moving.
