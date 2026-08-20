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

## 3. WHAT LANDED IN #104 — five code commits

| commit | what |
|---|---|
| `3c0de0da` | **a pointer comparison against a base subobject owes the base adjustment** — `B2 *p2 = &d; p2 == &d` answered 0 where gcc and clang answer 1. Found because libstdc++'s `_List_base::_M_clear` does exactly that and every std::list program printed a c2mir warning |
| `f68c4690` | **the dumper renders iterator containers** — the shared `class_iterator_iteration_protocol`, the counted loop, the inline-key primitive pair, plus two word defects (`std::__cxx11::list`, and a container word from the canonical spelling) |
| `582d9eed` | **the range-for is the recognizer's SECOND consumer** — `for (std::pair<const int,int> &kv : m)` over a std::map |
| `d237d83b` | **a type that contains ITSELF must not expand forever** — a self-referential container consumed 4 GB and died in `std::bad_alloc`; the guard is an ancestor set in the TYPE domain, two sets (walk and word) |
| `54da4e38` | **and a generated dumper function starts a NEW expansion path** — the guard above then refused a struct dumped BY VALUE holding a pointer to itself. ⚠️ **A 1084/0 fulltest did not catch it**; re-measuring the probe set did. See §3.2 |

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

### 3.2 ⚠️ THE LESSON THAT COST THE MOST: a green suite is a slice

`d237d83b`'s type-path guard refused `php::print_r(k)` on
`struct Link { int v; Link *next; }` — a struct dumped **by value** holding a
pointer to itself. **fulltest was 1084 / 0 with that bug in.** Every pointer test
dumps a POINTER at top level (`php::print_r(&a)`), so the type was never on the
expansion path when its generated dumper was built: the by-value shape had no
coverage anywhere in 1084 tests.

It was found by **re-measuring the probe set AFTER the release commit was already
made**. `tests/testphpdumpselfref.mad` now carries the three by-value shapes, so
the hole is a gate rather than a note.

⚠️ **And the measurement was blind twice before it was right.**
`tmp/patch/coverage.sh` runs `./bin/madc`, which on the NAS is whatever was last
PULLED — it printed `REFUSED` for a shape that had just been fixed, and earlier for
all 21 shapes at once. It now prints a provenance banner (binary, mtime,
`--version`, hostname, HEAD) and says to RUN IT ON THE CONTAINER. An evidence run
that cannot say what it tested is not evidence.

## 4. OPEN DEFECTS — every one has a reducer

- **`size()` / `count()` on a script `value` returns 0 for EVERY kind except
  `array`.** MEASURED (`tmp/probe/valuecount2.mad`, v0.85.0 binary):

  | kind | `size()` | truth |
  |---|---|---|
  | `string` "hello" | **0** | 5 bytes (`strlen(c_str())`) |
  | `integer` 42 | **0** | — |
  | `real` 3.5 | **0** | — |
  | `boolean` true | **0** | — |
  | `array` (2 elems) | 2 | 2 ✓ |

  `Program::add_array_methods` (`src/parser.cpp` ~22948) registers BOTH `count`
  and `size` on `ddARRAY` with `emit_symbol = "madarray_size"`, and
  `madarray_size` (`src/madc_mir_backend.cpp:73`) is
  `v->is_array() ? v->as_array().size() : 0` — the RANGE-FOR bound, whose own
  comment says "Intentionally NOT ns_common::value_count: foreach iterates indexed
  elements only, so an object-kind ctx must read as length 0 here." So one function
  answers two different questions, and the user-facing one gets the foreach answer.
  Meanwhile the C++ `madc::value::size()` (`src/madc_value.cpp:569`) returns
  `_v.size`, the payload BYTE COUNT — so the C++ API and the script surface
  disagree under one name.
  Visible consequence: `php::print_r(x, true)` yields a `string`-kind value whose
  `size()` is 0, which is why every capture test measures
  `strlen(v.c_str())` instead.
  ⚠️ **Correcting an earlier claim of mine:** I wrote that `count()` on an
  OBJECT-kind value reports 0 where PHP's `count()` reports the entry count. That
  case is NOT reachable — `tests/testphpdumpvalue.mad` records that no script can
  construct the object, bytes or instance kinds yet, so it can only arrive from a
  host-supplied value.
  ⚠️ Note that `count()` returning 0 for a SCALAR is not obviously wrong — PHP 8
  raises a TypeError for `count(42)`. It is `size()` on a `string` kind that is a
  plainly wrong number. Deciding what each name means across the nine kinds is a
  choice about the `value` intrinsic's user-visible surface, which is why #104 did
  not take it. `ns_common::value_count` is the owner `madarray_size`'s own comment
  names.
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
- 🔴 **THE FALLBACK SIGNATURE IS A LIVE SILENT WRONG ANSWER, not a warning.**
  I had this filed as an `--emit=c11` cosmetic issue. It is not. MEASURED on the
  v0.85.0 JIT:

  ```c
  int printf(const char *, ...);
  int main(){ printf("%d\n", strcmp("abc","abd") < 0 ? 1 : 0); }   /* no <string.h> */
  ```
  madc prints **0**; gcc prints **1**. The branch is simply not taken. Same for
  `__builtin_strcmp` directly, and `long long l = __builtin_strcmp("abc","abd")`
  gives **4294967295** where gcc gives **-1**.

  **ROOT CAUSE, exactly:** `src/lexer.cpp` rewrites **68** `__builtin_X` spellings
  to the bare libc name via `define_map` (`define_map["__builtin_strcmp"] =
  "strcmp"`, line ~2024 for memcpy and the surrounding block). The builtin registry
  in `src/parser.cpp` ~20346 registers the REAL signatures — but under the
  `__builtin_X` name, which the lexer guarantees the parser can never see. So the
  registration is dead code for every one of the 68, the bare name has no
  declaration unless a header supplied one, and the implicit/dlsym fallback gives
  `long long f()`. A 32-bit negative return then reads as a huge positive in any
  64-bit consumer.
  `add_core_function("memcpy"|"strlen"|"strcmp"|…)` — bare — is registered NOWHERE.

  **What is and is not affected (measured, do not re-derive):**
  - bare `strcmp` with no `<string.h>`: **WRONG** — and that is legal C89, in a
    project whose north star is compiling a C89 codebase.
  - `#include <string.h>` then `strcmp`: **correct** — the embedded header declares
    the comparison family as `int`, which is what `embedded-headers.md` already
    demands. So the header route is protected; the UNDECLARED route is not.
  - `std::string` `<` / `==` / `.compare()`: **correct**. libstdc++ funnels
    `__builtin_memcmp` through an `int` (`char_traits::compare` returns `int`), and
    the truncation preserves the sign. That masking is why a green suite never saw
    this.

  **THE PRECEDENTED FIX** — and the repo has already done it once for this exact
  class. `docs/parity/warning-baseline.txt`'s own history records
  "`__madc_builtin_frame_address` registered with its real void* return cleared
  testbuiltinframeaddress (13 -> 12)": i.e. register the real signature under the
  name the lexer rewrites **TO**. Do that for the rewrite table, driven FROM the
  table so the two cannot drift again (a data-driven pass over the same
  `define_map` entries, not 68 hand-written registrations). The comparison family
  (`strcmp`, `strncmp`, `memcmp`, `strcasecmp`, `strncasecmp`) is the dangerous
  subset and must be `int`.
  **A gate is mandatory:** a test that calls the comparison family with NO header
  in scope and asserts the sign, so the registration cannot silently rot back.
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
