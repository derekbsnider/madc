# Eval Leftovers (B → A0 → A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the three eval leftovers on the CIR backend: DSL string-value
comparison (package B), the MadValue/MadArray → `madc::value` unification
(package A0), and user-call-site scope capture + the full ns_madc
mangled-direct migration (package A).

**Architecture:** Spec: `docs/superpowers/specs/2026-06-10-eval-leftovers-design.md`
(read it first — it holds the settled semantics and the why). B is a rewrite
pass confined to the expression-DSL pipeline in `madc_program.cpp`. A0 deletes
the internal `MadValue`/`MadArray` tagged-union pair in favor of the public
`madc::value` (script `madc::array` = alias), deleting the
`build_runtime_expression_context` conversion layer. A re-keys the proven
asmjit-era scope-capture transform onto `madc::`-qualified public names and
makes the whole `<ns_madc>` surface declaration-only mangled-direct.

**Tech Stack:** C++11, the CIR→c2mir→MIR backend, doctest unit suite,
`make -C src fulltest`, gcc.c-torture failset-diff, SMAUG soak.

---

## Read before starting (repo gotchas that have burned sessions)

- **Capped runs always:** wrap every heavy command:
  `( ulimit -t 120; timeout 180 <cmd> )`. One heavy job at a time.
- **NAS mtime trap:** `touch` every edited source before `make -C src` and
  verify the matching `obj/*.o` actually recompiled (`ls -la obj/ | grep <name>`).
  A "build" that only relinks ships a stale parser.
- **`make -C src` does NOT relink `bin/test_*`** — only `make -C src test`
  does. Relink before concluding a unit fix didn't take.
- **The engine captures `std::cout`:** doctest output gets swallowed on
  failures — run `bin/test_libmadc_program --out=tmp/unit_out.txt` and read
  the file.
- **Pre-edit checklist** (`.claude/rules/pre-edit-checklist.md`) applies to
  every parser/cir edit: the TRACE steps below are mandatory, not optional.
- **Batch edits before rebuilding** — datadef.h cascades to a near-full
  rebuild; do all of a task's edits, then build once.
- Commit per task on the feature branch. Shell commands: single, no `&&`.

## File map (created/modified across the plan)

| File | Role |
|---|---|
| `tests/testmadcevalexprctx.{mad,expect}` (+delete `.mir_skip`) | B integration pin |
| `src/madc_program.cpp` | B rewrite pass; A0 context-root simplification |
| `include/libmadc/value.h`, `src/madc_value.cpp` | A0 null-vivify `object()`/`array()` |
| `tests/unit/test_libmadc_value.cpp` | A0 vivify unit pins |
| `include/datadef.h` | A0: delete MadValue/MadArray, retarget ddARRAY |
| `src/ns_{php,perl,python,ruby,js,rust,common}.cpp`, `include/ns_common.h` | A0: `madc::value` bodies |
| `include/madc/ns_*` + `ns_*.h` variants | A0/A: array spelling + decl-only madc:: publics |
| `src/parser.cpp` | A0: setters/ctx casts, delete conversion layer; A: collector, public-name hook |
| `src/cir_builder.cpp` | A0 refs; A: string branch in TokenScopeContext lowering |
| `src/madc_mir_backend.cpp` | A0 refs |
| `src/ns_madc.cpp` (CREATE) + `src/Makefile` | A: namespace madc impls + extern-C shims |
| `include/madc/ns_madc` | A: declaration-only rewrite + `_ctx` publics |
| `tests/testmadcevalscope` (delete `.mir_skip`) | A integration pin |
| `tests/unit/test_libmadc_program.cpp` | B mixed-compare case; A scope-category un-skips |
| `tests/unit/test_mangle.cpp` | A0 alias-mangling pin |

---

### Task 1: Feature branch

- [ ] **Step 1: Verify clean tree on develop**

Run: `git status`
Expected: `On branch develop`, clean.

- [ ] **Step 2: Branch**

Run: `git checkout -b feature/eval-leftovers-claude`

---

### Task 2 (B): Failing integration test — DSL string compare

**Files:** Modify `tests/testmadcevalexprctx.mad`, `tests/testmadcevalexprctx.expect`; delete `tests/testmadcevalexprctx.mir_skip`.

- [ ] **Step 1: Remove the skip and extend the test**

Run: `git rm tests/testmadcevalexprctx.mir_skip`

In `tests/testmadcevalexprctx.mad`, after the `name_ok=` line (line 31), add:

```cpp
    cout << "lt=" << madc::eval_expression_bool_ctx("user.name < \"zzz\"", ctx) << endl;
    cout << "ge=" << madc::eval_expression_bool_ctx("user.name >= \"echo\"", ctx) << endl;
```

Replace `tests/testmadcevalexprctx.expect` with:

```
rendered=42
int=42
bool=1
name_ok=1
lt=1
ge=1
double=3
rendered_name=echo
string=echo
```

- [ ] **Step 2: Run to verify it fails for the right reason**

Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalexprctx.mad )`
Expected: `name_ok=0` (pointer compare), `lt=`/`ge=` also wrong — FAILING.
Everything else (`rendered=42` … `string=echo`) still correct.

---

### Task 3 (B): The strcmp rewrite pass

**Files:** Modify `src/madc_program.cpp` (the expression pipeline around
`internal_program_runtime_eval_expression`, ~line 4423).

- [ ] **Step 1: TRACE (mandatory).** Confirm these shapes before editing:
  - `TokenBase::datadef()` exists (used at `src/parser.cpp:9856`).
  - `TokenOperator` has `TokenBase *left, *right` (`include/tokens.h:158`).
  - The exact signed-int enumerator name in `DataType`
    (`grep -n "dtINT" include/datadef.h`) — strcmp MUST return signed int
    (`.claude/rules/embedded-headers.md`).
  - `Program::addFunction(name, datatype_vec_t{ret, params...}, (fVOIDFUNC)sym)`
    usage at `src/madc_program.cpp:421`.
  - `parse_expression_unit`'s returned tree populates `left`/`right`
    (the RPN pop at `src/parser.cpp:9536`); check `TokenTerQ` exposure in
    the DSL (it has `condition/true_expr/false_expr`).

- [ ] **Step 2: Add the helpers** (file-local, next to
  `build_expression_input_from_policy` in `src/madc_program.cpp`):

```cpp
bool expression_compare_token(TokenID id)
{
    return id == TokenID::tkEquals || id == TokenID::tkNotEq
	|| id == TokenID::tkLT || id == TokenID::tkLE
	|| id == TokenID::tkGT || id == TokenID::tkGE;
}

bool expression_operand_is_string(TokenBase *tb)
{
    DataDef *dd = tb ? tb->datadef() : NULL;
    return dd && dd->type() == DataType::dtCHARptr;
}

bool ensure_expression_strcmp(Program &pgm)
{
    std::string id = "strcmp";
    if ( pgm.findVariable(id) )
	return true;
    void *sym = dlsym(RTLD_DEFAULT, "strcmp");
    if ( !sym )
	return false;
    // signed int return — embedded-headers.md comparison-family rule
    pgm.addFunction(id, datatype_vec_t{DataType::dtINT, DataType::dtCHARptr,
				       DataType::dtCHARptr}, (fVOIDFUNC)sym);
    return true;
}

// The expression DSL compares string operands by VALUE (spec:
// docs/superpowers/specs/2026-06-10-eval-leftovers-design.md). Comparison
// nodes with two string operands become strcmp(a,b) OP 0; a string vs
// non-string mix is a loud error. Full eval / the real language never
// reach this pass.
bool rewrite_expression_string_compares(Program &pgm, TokenBase *tb,
					std::string &error)
{
    if ( !tb )
	return true;
    if ( TokenCallFunc *call = dynamic_cast<TokenCallFunc *>(tb) )
    {
	for ( size_t i = 0; i < call->parameters.size(); ++i )
	    if ( !rewrite_expression_string_compares(pgm, call->parameters[i], error) )
		return false;
	return true;
    }
    if ( TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb) )
    {
	if ( !rewrite_expression_string_compares(pgm, tq->condition, error) )
	    return false;
	if ( !rewrite_expression_string_compares(pgm, tq->true_expr, error) )
	    return false;
	return rewrite_expression_string_compares(pgm, tq->false_expr, error);
    }
    TokenOperator *op = dynamic_cast<TokenOperator *>(tb);
    if ( !op )
	return true;
    if ( !rewrite_expression_string_compares(pgm, op->left, error) )
	return false;
    if ( !rewrite_expression_string_compares(pgm, op->right, error) )
	return false;
    if ( !expression_compare_token(op->id()) )
	return true;
    bool ls = expression_operand_is_string(op->left);
    bool rs = expression_operand_is_string(op->right);
    if ( !ls && !rs )
	return true;
    if ( ls != rs )
    {
	error = "cannot compare string and non-string values";
	return false;
    }
    if ( !ensure_expression_strcmp(pgm) )
    {
	error = "could not resolve strcmp for string comparison";
	return false;
    }
    std::string id = "strcmp";
    Variable *sv = pgm.findVariable(id);
    TokenCallFunc *cmp = new TokenCallFunc(*sv);
    cmp->file = op->file;
    cmp->line = op->line;
    cmp->column = op->column;
    cmp->parameters.push_back(op->left);
    cmp->parameters.push_back(op->right);
    op->left = cmp;
    op->right = new TokenInt(0);
    return true;
}
```

- [ ] **Step 3: Wire it in** — in `internal_program_runtime_eval_expression`,
  directly AFTER the `validate_expression_ast` rejection block and BEFORE
  `infer_expression_result_type`:

```cpp
    std::string compare_error;
    if ( !rewrite_expression_string_compares(child, expr, compare_error) )
    {
	fail_program_runtime(child,
			     std::string("program::eval_expression rejected parsed expression: ")
			     + compare_error,
			     display_name.c_str(), expr->line, expr->column);
	copy_program_public_error(self, child);
	return false;
    }
```

- [ ] **Step 4: Build (mind the mtime trap)**

Run: `touch src/madc_program.cpp`
Run: `( ulimit -t 600; timeout 900 make -C src )`
Verify `obj/madc_program.o` is newer than the edit. Expected: clean build,
no new warnings.

- [ ] **Step 5: Run the test — verify it passes**

Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalexprctx.mad )`
Expected: exactly the Task-2 `.expect` lines, including `name_ok=1 lt=1 ge=1`.

- [ ] **Step 6: Commit**

```bash
git add tests/testmadcevalexprctx.mad tests/testmadcevalexprctx.expect src/madc_program.cpp
git commit -m "feat(eval): expression-DSL string compares are value compares via strcmp lowering"
```

---

### Task 4 (B): Mixed-compare unit pin + suite gates

**Files:** Modify `tests/unit/test_libmadc_program.cpp`.

- [ ] **Step 1: Add the rejection case** (NOT skipped; place next to the
  existing expression-context cases ~line 540; mirror the adjacent case's
  context construction if the API differs):

```cpp
    TEST_CASE("eval_expression rejects string to non-string comparison") {
	madc::program pgm;
	std::map<std::string, madc::value> user_fields;
	user_fields["name"] = madc::value(std::string("echo"));
	std::map<std::string, madc::value> root;
	root["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root));
	bool result = false;
	CHECK_FALSE(pgm.eval_expression("user.name == 5", result));
	CHECK(pgm.has_error());
    }
```

- [ ] **Step 2: Build + run the unit suite**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: test_libmadc_program now 92+ passed / 42 skipped (the new case
green), all other unit binaries green. On failure read
`bin/test_libmadc_program --out=tmp/unit_out.txt`.

- [ ] **Step 3: fulltest gate**

Run: `( ulimit -t 1200; timeout 1800 make -C src fulltest )`
Expected: 556+ passed / 0 failed / 0 timed out, exit 0 (both check gates).

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_libmadc_program.cpp
git commit -m "test(eval): pin mixed string/non-string DSL compare rejection"
```

---

### Task 5 (A0): TRACE the builtin array lifecycle + alias mangling inputs

No edits in this task — it produces the facts Tasks 6-8 consume. Record
findings in the commit message of Task 6.

- [ ] **Step 1: How do `madc::array` objects construct/destruct on CIR today?**

Run: `( ulimit -t 120; timeout 60 bin/madc --dump-cir tests/testmadcevalexprctx.mad ) > tmp/exprctx_cir.txt 2>&1`
Run: `grep -n "ctx\|array\|MadArray" tmp/exprctx_cir.txt | head -40`
Also: `grep -rn "MadArray\|dtARRAY" src/cir_builder.cpp src/madc_mir_backend.cpp | head -30`
Answer: where the ctor/dtor (or zero-init) for ddARRAY-typed locals comes
from. `madc::value` holds a `std::string` member — zero-init is NOT valid
for it, so the unified type needs real ctor/dtor emission; identify the
hook (the generic class ctor/dtor path or an array-specific one).

- [ ] **Step 2: How does a class PARAMETER's mangled name derive from its
  DataDef?**

Run: `grep -rn "itanium_mangle" src/cir_builder.cpp | head -20`
Run: `grep -n "qualified\|class_name\|namespace" include/madc_mangle.h`
Answer: which DataDef field feeds the mangler for a class-typed parameter
(the `DDClass` name string, or a qualified-name accessor), so Task 8 can
give ddARRAY the `madc::value` identity that mangles to `NS_5valueE` /
`N4madc5valueE` forms matching the host symbols. Verify with
`echo 'namespace madc{class value;} void f(madc::value&);' | g++ -x c++ - -S -o- 2>/dev/null | grep _Z`
(expected host shape: `_Z1fRN4madc5valueE`).

- [ ] **Step 3: Where does the name "MadArray" resolve script-side?**

Run: `grep -rn "MadArray" include/madc/ | head -20`
Run: `grep -rn '"MadArray"' src/ | head`
The C-dialect `ns_*.h` sibling headers spell `MadArray*`; find what maps
that name (typedef in a header, or a parser mapping like
`parser.cpp:1738`). Both spellings must keep resolving after the swap.

---

### Task 6 (A0): `madc::value` null-vivify + unit pins

**Files:** Modify `include/libmadc/value.h`, `src/madc_value.cpp`,
`tests/unit/test_libmadc_value.cpp`.

- [ ] **Step 1: Write the failing unit cases** (in
  `tests/unit/test_libmadc_value.cpp`):

```cpp
TEST_CASE("object() on a null value vivifies an empty object") {
    madc::value v;
    CHECK(v.is_null());
    v.object()["k"] = madc::value(int64_t(1));
    CHECK(v.is_object());
    CHECK(v.as_object().at("k").as_integer() == 1);
}

TEST_CASE("array() on a null value vivifies an empty array") {
    madc::value v;
    CHECK(v.is_null());
    v.array().push_back(madc::value(int64_t(7)));
    CHECK(v.is_array());
    CHECK(v.as_array().size() == 1);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: the two new cases FAIL (current accessors throw on kind
mismatch).

- [ ] **Step 3: Implement vivify** in `src/madc_value.cpp` — in
  `value::object()` and `value::array()`, before the kind check:

```cpp
    if ( _kind == kind::null )
    {
	_kind = kind::object;            // (kind::array in array())
	_object.reset(new std::map<std::string, value>());   // (_array for array())
    }
```

Match the file's existing allocation idiom exactly (read the current
accessor bodies first). Update the value.h header comment: delete the
"Internal `MadValue` … do not mix the two" paragraph (it dies this
package) and document the vivify semantics.

- [ ] **Step 4: Re-run, verify green, commit**

Run: `( ulimit -t 600; timeout 900 make -C src test )`

```bash
git add include/libmadc/value.h src/madc_value.cpp tests/unit/test_libmadc_value.cpp
git commit -m "feat(libmadc): value::object()/array() vivify from null — the unified script array starts null"
```

---

### Task 7 (A0): The swap — delete MadValue/MadArray, everything speaks `madc::value`

**Files:** Modify `include/datadef.h`, `include/ns_common.h`,
`src/ns_{php,perl,python,ruby,js,rust,common}.cpp`, `src/parser.cpp`,
`src/cir_builder.cpp`, `src/madc_mir_backend.cpp`, `src/madc_program.cpp`,
`src/madc.cpp`, `src/madc_c_api.cpp`, `include/madc/ns_*` headers.
This is ONE build unit — batch every edit, build once at the end.

- [ ] **Step 1: datadef.h** — delete `MadValueKind`, `MadValue`, `MadArray`
  (lines ~938-1026) and add `#include "libmadc/value.h"` at the top.
  Retarget the builtin (keep `dtARRAY` until A0.2):

```cpp
class DataDefARRAY: public DDClass {
public:
    DataDefARRAY(): DDClass("madc::value", sizeof(madc::value), DataType::dtARRAY) {}
};
```

  Use the qualified-identity mechanism found in Task 5 Step 2 — if DDClass
  derives mangled names from a different field than the ctor name, set that
  field instead, and keep the display behavior of the `array` spellings.

- [ ] **Step 2: ns helpers** — in each `src/ns_*.cpp` + `src/ns_common.cpp`
  + `include/ns_common.h`: `MadArray` → `madc::value`, `MadValue(v)` →
  `madc::value(v)` (note: `madc::value`'s int/double ctors are `explicit`;
  string ctor copies). Body translation table:

| MadArray idiom | madc::value idiom |
|---|---|
| `a.data` (read loop) | `a.as_array()` (guard `a.is_array()`; null = empty) |
| `a.data.push_back(v)` / `a.push(v)` | `a.array().push_back(v)` |
| `a.at(i)` | `a.array().at(i)` |
| `a.data.clear(); a.assoc.clear();` | `a = madc::value::make_array();` |
| `a.get(key)` / `a.set(key, v)` | `a.object()[key]` |
| `a.count()` | `a.is_array() ? a.as_array().size() : (a.is_object() ? a.as_object().size() : 0)` — extract as a named helper `value_count(const madc::value&)` in ns_common |
| `a.pop()` | back+pop_back on `a.array()` (empty → `madc::value()`) |
| `v.kind == MadValueKind::mvSTRING` / `v.is_string()` | `v.is_string()` |
| `v.as_int()` / `as_double()` / `as_string()` / `as_array()` | `v.as_integer()` / `as_real()` / `as_string()` / nested value |

- [ ] **Step 3: parser.cpp** — the scope setters (~992-1008) cast
  `(madc::value *)ctx` and write `((madc::value *)ctx)->object()[key] = madc::value(value);`.
  The `madc_runtime_eval_*_ctx` helpers (~553-870): the `ctx` void* IS now a
  `madc::value*` — DELETE `build_runtime_expression_context` (parser.cpp:221)
  and `value_from_madvalue_context` (parser.cpp:~200), and pass
  `(const madc::value *)ctx` straight through to `runtime_eval_source` /
  `runtime_eval_expression`. The ctx VALIDATION the deleted converter
  performed (object-ness) already exists downstream
  (`internal_program_runtime_eval_expression` checks `context->is_object()`).
  NOTE: a fresh script ctx that was only `context_set_*`-filled is
  kind::object — already valid; a never-touched ctx is kind::null — decide
  loud-error vs empty-context (match the old converter's behavior for an
  empty MadArray: empty object context).

- [ ] **Step 4: cir_builder.cpp / madc_mir_backend.cpp / madc.cpp /
  madc_c_api.cpp / madc_program.cpp / madcdat_storage.cpp** — mechanical
  `MadArray`/`MadValue` reference updates per the same table
  (`grep -rln "MadArray\|MadValue" src/ include/` until empty, excluding
  this plan/spec and CHANGELOG history).

- [ ] **Step 5: embedded headers** — in `include/madc/ns_*` (C++ variants)
  and `include/madc/ns_*.h` (C variants): keep the script-facing spelling
  `array` / `MadArray` AS-IS in declarations (both names keep resolving to
  the retargeted builtin per Task 5 Step 3 findings); update only if a
  header declares the layout. `make -C src` regenerates
  `embedded_headers.cpp` automatically.

- [ ] **Step 6: array ctor/dtor** — apply the Task 5 Step 1 finding: ensure
  ddARRAY-typed locals/globals now emit real `madc::value` ctor/dtor calls
  (the host exports the symbols; madc's mangler generates
  `_ZN4madc5valueC1Ev` / `D1Ev` — verify with
  `nm -C bin/madc | grep "madc::value::value()"` after build). Zero-init is
  NOT acceptable: `madc::value` holds a `std::string` member.

- [ ] **Step 7: Build once**

Run: `touch` every edited file, then `( ulimit -t 600; timeout 900 make -C src )`
Expected: clean build, zero warnings; verify the obj files recompiled.

- [ ] **Step 8: Test gates (the full ladder — this touches everything)**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Run: `( ulimit -t 1200; timeout 1800 make -C src fulltest )`
Expected: same counts as Task 4 (php/perl/lang/rust/regex/foreach tests pin
the array-helper behavior; testmadceval* pin the ctx path).
Run the torture failset-diff:
`( ulimit -t 3600; timeout 5400 make -C src gcctest ) > tmp/gcctest_a0.txt 2>&1`
then diff the failset against `tmp/failset_lsq.txt` — MUST be byte-identical.
SMAUG soak (parser+runtime touched):
`cd /workspace/MadSMAUG/runtime/area` then
`( ulimit -t 120; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000 )`
Expected: exit 124 and the literal `Realms of Despair ready` in output.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(core): unify MadValue/MadArray onto madc::value — one value type end-to-end

Script madc::array is now the public madc::value (ddARRAY retargeted,
qualified identity madc::value). The build_runtime_expression_context
conversion layer is deleted; keyed context arrays are kind::object,
indexed ns_* arrays are kind::array. Spec:
docs/superpowers/specs/2026-06-10-eval-leftovers-design.md package A0.
Task-5 trace findings: <record the ctor/dtor + mangling-field answers here>"
```

---

### Task 8 (A0): Pin the alias mangling

**Files:** Modify `tests/unit/test_mangle.cpp`.

- [ ] **Step 1: Add the doctest** (match the file's existing case style):

```cpp
TEST_CASE("madc::value class parameter mangles with the madc namespace") {
    // host: void madc::context_set_int(madc::value&, std::string&, long)
    // g++: _ZN4madc15context_set_intERNS_5valueERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEl
    // Verify the mangler's class-param rendering for the ddARRAY identity:
    // the parameter must render as N4madc5valueE (with substitution rules
    // applied per the mangler's existing S_ handling).
    ...use the file's existing itanium_mangle_* helper invocation pattern
    for a namespace function taking a class reference, and CHECK the exact
    g++-verified symbol (generate it: echo the prototype through g++ -S as
    in Task 5 Step 2, paste the literal here before committing).
}
```

(The `...` line is an instruction, not code to paste: mirror the adjacent
`test_mangle.cpp` cases' exact helper calls — they pin symbols against
`c++filt`-verified literals; do the same for one array-taking madc::
public.)

- [ ] **Step 2: Relink + run**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: test_mangle case green against the g++-verified literal.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/test_mangle.cpp
git commit -m "test(mangle): pin madc::value class-parameter mangling for the unified array type"
```

---

### Task 9 (A): ns_madc goes declaration-only mangled-direct (+ the `_ctx` publics)

**Files:** Create `src/ns_madc.cpp`; rewrite `include/madc/ns_madc`; modify
`src/Makefile` (add `ns_madc.o` next to `ns_php.o`), `src/parser.cpp`
(delete the moved extern-C shims, parser.cpp:890-990).

- [ ] **Step 1: Declare the runtime internals** — the `madc_runtime_eval_*`
  functions (parser.cpp ~404-870) need host-visible declarations for
  ns_madc.cpp. Add them to `include/ns_common.h` (the ns_* shared-decl
  home; verify that's where `__php_*` siblings declare their internals —
  mirror it).

- [ ] **Step 2: Create `src/ns_madc.cpp`** — the ns_php.cpp layout: real
  `namespace madc { … }` implementations (THE surface scripts resolve
  mangled-direct), then the extern-C shims as the C-host API. Move the
  `__madc_eval_*_runtime` shim bodies here from parser.cpp verbatim, then:

```cpp
// src/ns_madc.cpp — madc:: script-level runtime eval.
// The namespace functions below are the single real implementation and
// the symbols scripts bind mangled-direct (cpp-first-api.md). The
// extern "C" exports at the bottom are the C-linkage API for C hosts
// consuming libmadc.a/.so — never the script-side resolution path.

namespace madc {

std::string &eval_unit(std::string &out, std::string &source)
{ return *(std::string *)madc_runtime_eval(&out, &source); }

bool eval_bool(std::string &source)
{ return madc_runtime_eval_bool(&source); }

long eval_int(std::string &source)
{ return madc_runtime_eval_int(&source); }

long eval_int(const char *source)
{ std::string s = source ? source : ""; return madc_runtime_eval_int(&s); }

double eval_double(std::string &source)
{ return madc_runtime_eval_double(&source); }

std::string &eval_string(std::string &out, std::string &source)
{ return *(std::string *)madc_runtime_eval_string(&out, &source); }

bool eval_expression_bool(const char *expr)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool(&e); }

long eval_expression_int(const char *expr)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int(&e); }

double eval_expression_double(const char *expr)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double(&e); }

std::string &eval_expression_string(std::string &out, const char *expr)
{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string(&out, &e); }

void eval_expression(long &out, const char *expr)
{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int(&e); }

void eval_expression(double &out, const char *expr)
{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double(&e); }

void eval_expression(std::string &out, const char *expr)
{ std::string e = expr ? expr : ""; madc_runtime_eval_expression(&out, &e); }

// context-carrying forms — ctx IS a madc::value (package A0)
std::string &eval_expression_ctx(std::string &out, const char *expr, value &ctx)
{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_ctx(&out, &e, &ctx); }

bool eval_expression_bool_ctx(const char *expr, value &ctx)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool_ctx(&e, &ctx); }

long eval_expression_int_ctx(const char *expr, value &ctx)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int_ctx(&e, &ctx); }

double eval_expression_double_ctx(const char *expr, value &ctx)
{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double_ctx(&e, &ctx); }

std::string &eval_expression_string_ctx(std::string &out, const char *expr, value &ctx)
{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string_ctx(&out, &e, &ctx); }

// full-eval ctx publics (NEW — package A's rebind targets)
std::string &eval_unit_ctx(std::string &out, std::string &source, value &ctx)
{ return *(std::string *)madc_runtime_eval_ctx(&out, &source, &ctx); }

bool eval_bool_ctx(std::string &source, value &ctx)
{ return madc_runtime_eval_bool_ctx(&source, &ctx); }

long eval_int_ctx(std::string &source, value &ctx)
{ return madc_runtime_eval_int_ctx(&source, &ctx); }

long eval_int_ctx(const char *source, value &ctx)
{ std::string s = source ? source : ""; return madc_runtime_eval_int_ctx(&s, &ctx); }

double eval_double_ctx(std::string &source, value &ctx)
{ return madc_runtime_eval_double_ctx(&source, &ctx); }

std::string &eval_string_ctx(std::string &out, std::string &source, value &ctx)
{ return *(std::string *)madc_runtime_eval_string_ctx(&out, &source, &ctx); }

void context_set_int(value &ctx, std::string &key, long v)
{ ctx.object()[key] = value(int64_t(v)); }

void context_set_real(value &ctx, std::string &key, double v)
{ ctx.object()[key] = value(v); }

void context_set_string(value &ctx, std::string &key, const char *v)
{ ctx.object()[key] = value(std::string(v ? v : "")); }

void context_set_array(value &ctx, std::string &key, value &v)
{ ctx.object()[key] = v; }

} // namespace madc
```

Then the moved extern-C block (each shim now calls the madc:: function or
the runtime internal — keep them byte-compatible with the old exports).

- [ ] **Step 3: Rewrite `include/madc/ns_madc`** as declaration-only — the
  same signatures as Step 2's `namespace madc` functions, `;`-terminated,
  with `array` spelled for the ctx parameters (script alias of
  `madc::value`), NO extern-C block, NO bodies. Header comment: publics
  resolve mangled-direct; the C API for C hosts is in `madc_api.h`.

- [ ] **Step 4: Makefile + parser.cpp cleanup** — add `ns_madc.o` to the
  objects list in `src/Makefile` (next to `ns_php.o`); delete the moved
  extern-C bodies from parser.cpp (KEEP `__madc_scope_set_*` — compiler
  machinery stays, per spec).

- [ ] **Step 5: Build + the three pinned eval tests**

Run: `( ulimit -t 600; timeout 900 make -C src )`
Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadceval.mad )`
Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalexpr.mad )`
Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalexprctx.mad )`
Expected: each matches its `.expect` exactly. If a symbol fails to resolve,
check `nm -C bin/madc | grep "madc::eval"` (the -rdynamic export must show
the namespace functions) and compare the mangled name madc emits
(`--emit=c11` or `-v`) against `nm bin/madc | grep _ZN4madc`.

- [ ] **Step 6: fulltest + commit**

Run: `( ulimit -t 1200; timeout 1800 make -C src fulltest )`

```bash
git add -A
git commit -m "feat(ns_madc): declaration-only madc:: publics resolved mangled-direct; extern-C exports become the C-host API; add eval_*_ctx publics"
```

---

### Task 10 (A): String scope capture — predicate, setter, CIR branch, collector

**Files:** Modify `include/datadef.h` (or the DataDef impl file the trace
picks), `src/parser.cpp`, `src/cir_builder.cpp`.

- [ ] **Step 1: TRACE.** How does the post-A0 codebase identify "the
  std::string class DataDef"? `grep -rn "std::string\" \|basic_string" src/parser.cpp src/cir_builder.cpp | head` —
  find the existing identity (likely the DataDefCLASS qualified name from
  the real-header parse). The new predicate must use that identity, not a
  new string literal, if one exists.

- [ ] **Step 2: Add the named predicate** (the marshalling-boundary
  exception, flagged in the spec) — as a method on DataDef
  (include/datadef.h), implementation per Step 1's finding:

```cpp
    // Marshalling-boundary predicate (libmadc value kinds): true when this
    // DataDef is the std::string class. Sanctioned exception to
    // no-std-hardcoding (same category as the mangler) — see
    // docs/superpowers/specs/2026-06-10-eval-leftovers-design.md.
    bool is_string_class() const;
```

- [ ] **Step 3: The string setter** — in parser.cpp next to the other
  `__madc_scope_set_*` (post-A0 shapes):

```cpp
void *__madc_scope_set_string_runtime(void *ctx, const char *key, void *str)
{
    ((madc::value *)ctx)->object()[std::string(key)] =
	madc::value(*(std::string *)str);
    return ctx;
}
```

- [ ] **Step 4: Collector** — in `is_runtime_eval_scope_supported_variable`
  (parser.cpp ~8667), extend the accepted set:

```cpp
    DataType raw = var->type->rawtype();
    return raw == DataType::dtBOOL
	|| var->type->is_integer()
	|| var->type->is_real()
	|| var->type->is_string_class()
	|| raw == DataType::dtARRAY;
```

- [ ] **Step 5: CIR lowering branch** — in the TokenScopeContext lowering
  (cir_builder.cpp:7787-7806), add the string case next to int/real/array,
  selecting `"__madc_scope_set_string_runtime"` and passing the VARIABLE'S
  ADDRESS (the host reads the std::string by pointer — mirror exactly how
  the array case passes its pointer argument; read that branch first).

- [ ] **Step 6: Build; no behavior change yet** (nothing triggers capture
  for namespace publics until Task 11) — run
  `( ulimit -t 1200; timeout 1800 make -C src fulltest )` to prove no
  regression, then commit:

```bash
git add include/datadef.h src/parser.cpp src/cir_builder.cpp
git commit -m "feat(eval): scope capture learns std::string locals — predicate, setter, CIR branch"
```

---

### Task 11 (A): The user-call-site hook — failing test first

**Files:** Delete `tests/testmadcevalscope.mir_skip`; modify
`src/parser.cpp`.

- [ ] **Step 1: Un-skip and verify the failure**

Run: `git rm tests/testmadcevalscope.mir_skip`
Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalscope.mad )`
Expected (the documented gap): wrong values — `42`/`42`/`echo` NOT all
produced, because no scope context is captured at the `madc::eval_*` call
sites. Record the exact failing output.

- [ ] **Step 2: TRACE.** At the two TokenCallFunc construction sites
  (`parser.cpp:10536` and `:13433`), what does the resolved `Variable` look
  like for a `madc::eval_expression_int("…")` call — `var->name`, the
  FuncDef's `namespace_name` / `function_display_name`? Add a temporary
  `DBG` print, run `bin/madc -v tests/testmadcevalscope.mad 2>&1 | grep -i "eval_expression_int"`,
  record, remove the print. This decides what
  `runtime_eval_scope_target` keys on.

- [ ] **Step 3: Extend the rebind helper** — in `runtime_eval_scope_target`
  (parser.cpp ~8620): after the existing `__madc_*_runtime` mapping, add the
  namespace-public mapping per Step 2's shapes:

```cpp
    // madc:: namespace publics (mangled-direct wrappers): rebind to the
    // sibling _ctx overload so the parser can append the captured scope.
    FuncDef *fd = dynamic_cast<FuncDef *>(var->type);
    if ( fd && fd->namespace_name == "madc"
      && is_runtime_eval_scope_public_name(fd->function_display_name) )
    {
	bool expression = fd->function_display_name.compare(0, 15,
						"eval_expression") == 0;
	if ( expression ? is_runtime_eval_expression_scope_access_enabled()
			: is_runtime_eval_source_scope_access_enabled() )
	{
	    std::vector<const DataDef *> at;   // arg types unknown here —
	    Variable *w = find_namespace_function_overload("madc",
		    fd->function_display_name + "_ctx", at);
	    if ( w )
		return w;
	}
    }
```

  ADAPT per the Step 2 trace: if overload selection needs the actual arg
  types, do the rebind where parseCallFunc has them (the
  `needs_runtime_scope_context` block at parser.cpp:9783 already re-ranks
  with args; the `auto_scope_context` flag set at construction is what
  arms it). Add `"eval_unit"` to `is_runtime_eval_scope_public_name`
  (parser.cpp:372). Extend the `auto_scope_context` assignments at both
  construction sites to also arm for this rebind (mirror the existing
  two-condition shape).

- [ ] **Step 4: Build + run the scope test**

Run: `touch src/parser.cpp`
Run: `( ulimit -t 600; timeout 900 make -C src )`
Run: `( ulimit -t 120; timeout 60 bin/madc tests/testmadcevalscope.mad )`
Expected `.expect`: `42`, `42`, `echo` — int capture through the
expression path (`base`+`bonus` params/locals), the full-eval path, and
the std::string local `who` through Task 10's string capture.

- [ ] **Step 5: Full gates (parser touched — the whole ladder)**

fulltest, torture failset-diff vs `tmp/failset_lsq.txt` (byte-identical),
SMAUG soak — same commands as Task 7 Step 8.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(eval): scope capture fires at the madc:: public call site — rebind to _ctx overloads, capture caller scope"
```

---

### Task 12 (A): Un-skip the scope-access unit categories

**Files:** Modify `tests/unit/test_libmadc_program.cpp` (~lines 1400-1525).

- [ ] **Step 1: Remove `* doctest::skip()`** from the four cases:
  "script-side runtime eval sees current scope when allowed",
  "script-side runtime eval scope access can be disabled independently",
  "script-side runtime expression scope access can be disabled without
  affecting full eval", "script-side full eval scope access can be
  disabled without affecting runtime expression eval".

- [ ] **Step 2: Relink + run**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: all four green (they exercise the policy gates Task 11 honors).
If a case still fails, READ ITS BODY — the asmjit-era spec may assert an
error-message shape that changed; fix the implementation if the contract
is real, update the case only if it pinned an asmjit incidental. Record
which in the commit message. Read failures via `--out=tmp/unit_out.txt`.

- [ ] **Step 3: fulltest + commit**

```bash
git add tests/unit/test_libmadc_program.cpp
git commit -m "test(eval): un-skip the script-side scope-access unit category"
```

---

### Task 13: Final gates, mirrors, hand-off

- [ ] **Step 1: The full ladder one last time** — fulltest (expect
  557+/0/0, two fewer skips than baseline 20), `make -C src test`, torture
  failset-diff byte-identical, SMAUG soak.

- [ ] **Step 2: Sync mirrors** (same session, per `session-handoff.md`):
  `claude_status.json` (head/branch/test counts/current_phase),
  `CHANGELOG.md` [Unreleased] (B, A0, A entries — model on the existing
  eval entries), `docs/test-status.md`, `docs/plans/ROADMAP.md` (eval
  bullets + the A0.2/value-unification queue note), the KG via
  `scripts/kg_query.sh` (Decision: value unification; Feature: call-site
  scope capture), and memory
  (`project_libmadc_eval.md`: B/A0/A landed, C remains; update
  `project_cpp_mangled_direct.md` if the migration taught anything new).

- [ ] **Step 3: Merge** — `feature/eval-leftovers-claude` → `develop` per
  `superpowers:finishing-a-development-branch` (PR or local merge per the
  user's call; develop must stay green).

- [ ] **Step 4: Hand-off note** — branch, tree state, validation results,
  what remains (package C plan: register_function via `MIR_load_external`,
  get/set_global, string call marshalling, fork/limits, policy tail; the
  A0.2 builtin-retirement queue item and its gate).
