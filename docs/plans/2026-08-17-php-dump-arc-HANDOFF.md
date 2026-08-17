# HANDOFF — php dump intrinsics arc, sessions #100–#101, 2026-08-17

**Read this fully. Assume a cold start.** Run `bash scripts/resume.sh` first (live
git state + orphaned jobs), then `claude_status.json`, then this file.

Supersedes `docs/plans/2026-08-17-post-v0.84.0-HANDOFF.md` (CLOSED).

---

## 1. STATE — no release was cut, deliberately

- Branch `develop`. **VERSION = 0.84.0.** `master` = v0.82.0.
- The dump work is MERGED to develop and **UNRELEASED**. Release prep was
  written and then **reverted** in session #101 — see §2. Do not cut a release
  until §4 is done.
- Working tree clean. Untracked `donut.c`, `test.mad`, `testsort.mad` are the
  owner's — **never commit them**.
- 18 commits unpushed on develop. Pushing is fine; releasing is not.

## 2. WHY THERE IS NO RELEASE — read before reporting any progress

I reported "print_r is complete" having completed **one slice** of it (PHP's
`$return` parameter). The owner cut the release on that basis, then found
associative containers and `*RECURSION*` still missing:

> *"you said this was done, now in your message you're saying associative
> containers are not done? and no recursion"*
> *"what the heck? I'm here thinking things are done, and there is still a lot
> yet to do?"*

The owner had ruled earlier: *"I don't want a version bump until it's done."*
That ruling stands.

**DIRECTIVE: report COVERAGE, not slices.** State which shapes work and which are
refused, measured (§3) — never "feature X is done". A slice landing is not a
feature landing.

## 3. MEASURED COVERAGE — `php::print_r` today (run, not summarised)

Probes live in the container at `tmp/probe/*.mad`; re-run after any change.

| shape | result |
|---|---|
| scalar (`int`) | **OK** |
| `const char *` | **OK** |
| struct / class (access, inheritance, unions, bit-fields) | **OK** |
| `std::vector<int>`, nested `vector<vector<int>>` | **OK** |
| `std::string` | **OK** |
| `print_r($x, true)` (capture, runtime flag, scalar capture) | **OK** |
| **`madc::value` / `array`** | **REFUSED** — `no dumper for type 'array' yet` |
| **`std::map`, `std::set`** | **REFUSED**, and badly — see §5 |
| **pointer** | **REFUSED** — `no dumper for type 'P*' yet` |
| **enum** | **REFUSED** — `no dumper for type 'C' yet` |
| **`int[2][3]`** | **REFUSED** — multidimensional |

`php::print_r` NEVER existed before this arc — the plan's "not just
`madc::value`/`array`" phrasing was aspirational. **Nothing was regressed.**

Validation of record: **Linux fulltest 1071 / 0 / 0TO / 9 skip, 0 warnings**
(`tmp/logs/rb-20260817-204445.log`); `testphpprintrreturn` + `testvalueinit`
green in JIT, `--exe`, `--obj` (`rb-20260817-205943.log`). Trailer gate 446/0.

## 4. NEXT TASK — `madc::value` / `array`, the FULL kind gamut. Do this FIRST.

> **OWNER, 2026-08-17:** *"the most critical thing for print_r and var_dump to
> support is the full gamut of the madc::value / madc::array values"* … *"and
> those are **easiest** since they are runtime determinable"*

The owner is right, and my earlier sequencing (this slice LAST, behind
`begin()`/`end()` and pointers) was wrong. A value carries its own kind tag, so
there is **no per-type compile-time expansion**: one runtime function, ordinary
recursion, one ancestor stack. Order:

1. **Add C enumeration accessors to `include/madc_api.h`.** They do NOT exist —
   verified: no `madc_value_*` count/element/key accessor, and the C++ class
   (`include/libmadc/value.h`) has only `size()`. Need element count,
   element-at-index, and key-at-index for the `object` kind. Additive, and useful
   to embedding hosts on their own. The kind tag is already there:
   `madc_value_get_type_id`, `madc_value_kind_name`, `madc_value_text`,
   `_get_integer`, `_get_real`, `_get_bool`.
2. **One C11 runtime walker in `src/rt/rt_dump.c`:**
   `__madc_dump_value(void *sink, const madc_value *v, int col, int flavor)`
   switching on kind — `null`, `boolean`, `integer`, `real`, `string`, `bytes`,
   `array`, `object`, `instance` (the `value::kind` enum). **It MUST be C11 in
   rt_dump.c, not a C++ helper** — the ledger membership rule
   (`scripts/ledger_sources.txt`) is what lets a `-static-libmadc` binary dump.
   That is exactly why step 1's accessors must be C.
3. **`*RECURSION*` here, not later.** Push/pop the address around each
   array/object; emit the marker when the address is ALREADY on the stack. PHP
   prints a repeated value twice and says `*RECURSION*` only for a CYCLE — an
   ancestor stack, never a visited set. Exact output for both flavours is plan
   §2; note print_r's odd shape (`Array` on the entry line, then a line with ONE
   leading space before `*RECURSION*`).
4. **The CIR side shrinks.** `is_array_object(dd)` in `src/cir_dump.cpp`
   currently refuses; make it emit a SINGLE call to the walker. The `void *sink`
   parameter already threaded through every primitive gives
   `print_r($x, true)` capture on the value path for free.
5. **`instance` kind** needs a type name from `type_id`. If that lookup is not
   reachable from C, SAY SO rather than quietly printing `Object`.

Oracle: php-cli 8.3.6 (in `scripts/provision_container.sh`). A value's `object`
kind IS a PHP associative array — `[key] => value`. Capture with `cat -A`; never
retype from memory.

**Done means:** every kind in `value::kind` renders in BOTH flavours, oracled,
with a test in `tests/`. Not "the first kind works".

## 5. THEN — the refusal diagnostic is a BUG. Fix it early; it is small.

`php::print_r(std::map<std::string,int>)` does not say "associative containers are
not supported yet". It walks INTO libstdc++'s red-black tree and dies four levels
down:

```
cir error: php::print_r: member '_M_t': member '_M_impl': member '_M_header':
member '_M_color': no dumper for type '_Rb_tree_color' yet
```

Useless to a user. A container the dumper cannot handle must be refused BY THE
CONTAINER'S NAME, before descending into its members. My claim that "every
uncovered type is refused by name, so nothing lies" was only half true, and this
is the half that was false.

## 6. THEN — the rest, in this order

- **S1 `begin()`/`end()` protocol** — owed regardless: `for (auto &kv : m)` does
  not work today (`member reference is not a structure or union`) and
  `for (int v : std::set<int>)` errors where g++ runs. Enables `std::map`/`set`
  dumping, which §4 does NOT cover.
- **S5b** associative rendering for C++ containers, gated on S1.
- **S3c** pointers for ordinary C/C++ types (a GENERATED dumper FUNCTION per
  pointee type, making `col` a runtime parameter). §4's ancestor stack is the
  model to copy.
- enums; multidimensional arrays (`member_counts` holds the FLATTENED total and
  `m[i]` on `int[2][3]` yields a ROW, so a flat walk reads past the first row).
- Then the two macOS MIR-blob causes (§8), then #60/#61/#25/#56/#55/#49.

## 7. SETTLED — do not re-litigate

- **`php::print_r` returns `madc::value &`, not by value.** A value has no
  emitted C type today; by-value emits `int` and silently returns nothing (§8 D1).
  Signature: `template<class T> madc::value &print_r(const T &v, bool ret = false)`
  — PHP's own shape (one function, default second parameter, `string|true`).
- **The placeholder's ZERO arity is load-bearing** — *"the 0-param placeholder is
  arity-filtered out of the ranking"*. Never give it parameters. A default
  argument needs no parser support: the compiler IS the implementation, so
  `lower_dump_call` applies it.
- **The capturing form is HOISTED to `m_pending_stmts`, not a statement
  expression** — `({…; &tmp;})` is not an lvalue and a `value &` consumer takes
  its address.
- **The sink is an explicit first parameter** on every primitive, with ONE writer
  owning stdout-vs-buffer. Not a file-static "current sink".
- **Runtime bindings are READ off the registered `operator=` table**
  (`FuncDef::emit_symbol`), never respelled as `madarray_assign_*`.
- **Making the script `value` BE the 32-byte `madc_value` struct: RECORDED, NOT
  SCHEDULED** (owner: *"I don't believe it's necessary to take on now"*). The
  measured write-up is `docs/plans/2026-06-12-type-table-value-abi-design.md` §9;
  two earlier cost claims about it were WRONG and are corrected there.
- Plan §12 and §13 are current wherever they contradict the earlier design text.

## 8. OPEN DEFECTS (plan §13.6)

- **D1** — `value f()` by value emits `int f()`, runs, prints nothing, exits 0.
  Layer: `func_def` return arm → `type_list` → `append_type_specs` (no `dtARRAY`
  case → `int`). Reducer `tmp/r1.mad`. Immediate honest fix: make the
  fall-through LOUD. Real fix: §7's recorded migration.
- **D2** — assigning to a `value &` PARAMETER is wrongly rejected
  ("assignment of incompatible value"). Reducer `tmp/r3.mad`.
- macOS: both darwin arches ship NO MIR module cache (linux 467 KB, win64
  497 KB); gated at `darwin mir-blob-skips 1`. arm64: `wrong result type in proto
  proto138`. x86-64: `duration<double,nano>::operator%=` lowers `%` to integer
  `umods` on a floating operand — fix the LOWERING if that is where it is; an
  integer modulo from a floating operand is wrong wherever it comes from.
- README's EXE/OBJ `1031/0` is **PREDICTED, not measured** — measure or correct
  before any promote.

## 9. STANDING CONSTRAINTS

- **The QNAP NAS never builds or tests.** Everything via
  `scripts/remote_build.sh` (container, `ssh -p 2299 dev@localhost`). One heavy
  job at a time — it refuses concurrent runs BEFORE syncing.
- ⚠️ **`grep -r` here is a ugrep shim with `--ignore-files`**, and `.gitignore`
  lists `claude_status.json` (which is TRACKED), so recursive grep SILENTLY SKIPS
  it. Use **`git grep`** for any coverage sweep. This already caused one missed
  surface.
- ⚠️ **Never edit sources while a container suite runs** — the sync delivers a
  file with an older preserved mtime, make skips it, and a win64 link failed on a
  symbol whose source clearly had it. `touch` edited sources, or wait.
- Push only to `derekbsnider/*`; `origin` is not first in `git remote -v`.
- No `&&` chains; no backticks in a `-m` message; never `git add -A`.
- Scratch and reducers in `tmp/` (gitignored).
- Every `src/`/`include/` commit carries `Hypothesis:`/`Layer:`/`Searched:`/
  `Oracle:` — `scripts/check-rule-trailers.sh` gates it.
- **Do not re-run suites on already-green content.** Source changes invalidate a
  green; docs-only changes do not.
