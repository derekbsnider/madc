# Phase-1 CIR Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add char literals, subscript-expression reads, and `var_decl` initializers to `CirBuilder` so `--backend=cir` moves past its 55/419 baseline.

**Architecture:** `CirBuilder` (`src/cir_builder.cpp`) walks the madc `TokenBase` AST and emits c2mir `node_t` trees handed directly to c2mir. Each construct is added as a `translate_expr` case or a `var_decl` change, verified against `c2m -d` via `scripts/cir_diff.sh`, unit-tested against a new builder-path test helper, and measured with `tmp/cir_triage.sh`.

**Tech Stack:** C++11, c2mir node API (`c2mir_api.h`), doctest (`tests/unit/test_cir.cpp`), bash verification scripts.

**Reference shapes (confirmed via `c2m -d` + legacy `madc_cir.cpp`):**
- char literal → `N_CH` (union field `u.ch`, `mir_char`), NOT `N_I`
- subscript read → `N_IND(base, index)`
- scalar init → bare expr as 5th `SPEC_DECL` operand
- brace init → `LIST(INIT(LIST(), val), ...)`; nested element's val is itself `LIST(INIT(...))`
- designated → `.f=v` → `INIT(LIST(FIELD_ID(ID f)), v)`; `[i]=v` → `INIT(LIST(const-expr), v)`

**Key facts discovered during planning:**
- Two subscript token classes: `TokenSubscript` (named base, already handled at `cir_builder.cpp:655`) and `TokenSubscriptExpr` (`include/madc.h:512`, fields `base_expr`/`index`, type `ttSubscript`, **unhandled** — the 49 failures).
- `TokenChar` (`include/tokens.h:927`): value via `ival()`.
- `TokenDecl` (`include/madc.h:198`): `initialize` (scalar), `init_list` + `has_brace_init` (flat brace), nested elements are `TokenStructLit` (`include/madc.h:240`, field `inits`). **No designator field.**
- Local decl path: `translate_stmt:956` calls `var_decl(&td->var, td)` — `origin` IS the `TokenDecl`.
- Global decl path: `translate_module:1180` calls `var_decl(td.var)` — only a `Variable*`; `td.origin` (`parser.cpp:14782`) is the decl-head type token `tb`, NOT a `TokenDecl`. Globals therefore need separate init wiring (Task 5).
- `test_cir.cpp`'s `cir_run` helper uses the **legacy** `cir_translate`, not `CirBuilder`. New tests need a builder-path helper (Task 0).

---

## Task 0: Add a CirBuilder-path test helper

**Files:**
- Modify: `tests/unit/test_cir.cpp:40-85` (add helper alongside `cir_run`)

- [ ] **Step 1: Add `cir_run_builder()` after `cir_run` (after line 85)**

```cpp
// Helper: same as cir_run but exercises the LIVE CirBuilder path
// (cir_run uses the legacy cir_translate). Use this for all new tests.
static int64_t cir_run_builder(const char *source) {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);

    CirBuilder builder(c2m);
    node_t tree = builder.translate_module(prog.get());
    REQUIRE(tree != nullptr);
    REQUIRE(cir_report_errors(stderr, tree) == 0);

    int ok = cir_compile(mir_ctx, c2m, tree, "test_mod");
    REQUIRE(ok == 1);

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);

    MIR_item_t func_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
         item != NULL; item = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_func_item &&
            strcmp(item->u.func->name, "main") == 0)
            func_item = item;
    }
    REQUIRE(func_item != nullptr);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);
    int64_t result = val.i;

    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
    return result;
}
```

- [ ] **Step 2: Add a smoke test using the new helper (after the new helper)**

```cpp
TEST_CASE("CIR builder: return literal") {
    CHECK(cir_run_builder("int main() { return 42; }") == 42);
}
```

- [ ] **Step 3: Build and run test_cir**

Run: `make -C src test 2>&1 | grep -iE "test_cir|cir builder|assertion|error" | head`
Expected: builds; "CIR builder: return literal" passes. (`cir_report_errors` is declared in `src/madc_cir.h`, already included.)

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_cir.cpp
git commit -m "test(cir): add cir_run_builder helper exercising the live CirBuilder path

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 1: Char literals (`ttChar`)

**Files:**
- Modify: `src/cir_builder.h` (declare `ch`)
- Modify: `src/cir_builder.cpp:113` (add `ch` builder after `real`), `:580` (add case in `translate_expr`)
- Test: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Write the failing test (after the Task 0 smoke test)**

```cpp
TEST_CASE("CIR builder: char literals") {
    CHECK(cir_run_builder("int main() { char c = 'A'; return c; }") == 65);
    CHECK(cir_run_builder("int main() { return 'Z' - 'A'; }") == 25);
}
```

- [ ] **Step 2: Run it — expect FAIL**

Run: `make -C src test 2>&1 | grep -iE "char literals|FAILED|untranslatable" | head`
Expected: FAIL — the builder emits an error node ("unhandled expression ... token type 10") so `cir_report_errors` is nonzero / the REQUIRE trips.

- [ ] **Step 3: Declare `ch` in `src/cir_builder.h`**

Find the line declaring `node_t real(double val, TokenBase *origin = NULL);` and add directly after it:

```cpp
	node_t ch(long val, TokenBase *origin = NULL);
```

- [ ] **Step 4: Implement `ch` in `src/cir_builder.cpp` (after `real`, line 118)**

```cpp
node_t CirBuilder::ch(long val, TokenBase *origin)
{
	cir_node *cn = make(N_CH, origin);
	cn->base.u.ch = (mir_char)val;
	return cn->as_node();
}
```

- [ ] **Step 5: Add the `translate_expr` case (in `src/cir_builder.cpp`, after the Real-literal case at line 586)**

```cpp
	// Char literal (C char constants emit N_CH, value via ival())
	if (tb->type() == TokenType::ttChar)
		return ch(tb->ival(), tb);
```

- [ ] **Step 6: Run the test — expect PASS**

Run: `make -C src test 2>&1 | grep -iE "char literals|FAILED" | head`
Expected: "CIR builder: char literals" passes, no FAILED.

- [ ] **Step 7: Verify node shape against c2m**

```bash
printf "int main(void){ char c = 'A'; return c; }\n" > tmp/t_char.c
scripts/cir_diff.sh --checked tmp/t_char.c
```
Expected: ends with `MATCH: trees are structurally identical` (the char init now appears as `CH`, not `IGNORE`/`I`).

- [ ] **Step 8: Full suite + triage delta**

```bash
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: fulltest green (419/0 — default backend unaffected); triage shows `unhandled_expr` dropped by roughly the 32 ttChar cases (record the actual numbers; note any newly-surfaced modes).

- [ ] **Step 9: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): translate char literals to N_CH

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Subscript-expression reads (`TokenSubscriptExpr`)

**Files:**
- Modify: `src/cir_builder.cpp:655-662` (extend the subscript handling)
- Test: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("CIR builder: subscript-expr reads") {
    // base is an expression (pointer arithmetic result), not a bare name
    CHECK(cir_run_builder(
        "int main() { int a[3]; a[0]=7; int *p=a; return (p+0)[0]; }") == 7);
    // chained / nested subscript through an expression base
    CHECK(cir_run_builder(
        "int main() { int a[2][2]; a[1][1]=9; return a[1][1]; }") == 9);
}
```

- [ ] **Step 2: Run it — expect FAIL**

Run: `make -C src test 2>&1 | grep -iE "subscript-expr|FAILED|untranslatable" | head`
Expected: FAIL — `TokenSubscriptExpr` falls through to the unhandled-expression error node (token type 24).

- [ ] **Step 3: Replace the subscript block (`src/cir_builder.cpp:655-662`)**

```cpp
	// Array subscript on a named variable: name[i] (+ multi-dim extras)
	{
		TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(tb);
		if (tsub) {
			node_t n = node2(N_IND,
				id(tsub->object.name.c_str(), tb),
				translate_expr(tsub->index), tb);
			// Multi-dim fixed array: a[i][j] -> IND(IND(a,i),j)
			for (size_t k = 0; k < tsub->extra_indices.size(); k++)
				n = node2(N_IND, n, translate_expr(tsub->extra_indices[k]), tb);
			return n;
		}
	}

	// Array subscript on a sub-expression: expr[i] == IND(expr, i)
	{
		TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tb);
		if (tse)
			return node2(N_IND,
				translate_expr(tse->base_expr),
				translate_expr(tse->index), tb);
	}
```

- [ ] **Step 4: Run the test — expect PASS**

Run: `make -C src test 2>&1 | grep -iE "subscript-expr|FAILED" | head`
Expected: passes, no FAILED.

- [ ] **Step 5: Verify node shape against c2m**

```bash
printf "int a[3];\nint main(void){ a[1]=5; return a[1]; }\n" > tmp/t_sub.c
scripts/cir_diff.sh --checked tmp/t_sub.c
```
Expected: `MATCH: trees are structurally identical`.

- [ ] **Step 6: Full suite + triage delta**

```bash
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: fulltest green; `unhandled_expr` drops by up to the 49 ttSubscript cases (record actuals + newly-surfaced modes — many will move to c2mir_rejected/runtime, that is expected).

- [ ] **Step 7: Commit**

```bash
git add src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): translate subscript-expression and multi-dim reads to N_IND

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Local `var_decl` scalar + flat-brace initializers

**Files:**
- Modify: `src/cir_builder.h` (declare `init_value`)
- Modify: `src/cir_builder.cpp:360-361` (replace the dropped initializer)
- Modify: `src/cir_builder.cpp` (add `init_value` helper before `var_decl`, ~line 303)
- Test: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("CIR builder: scalar initializers") {
    CHECK(cir_run_builder("int main() { int x = 7; return x; }") == 7);
    CHECK(cir_run_builder("int main() { int a = 3, b = 4; return a + b; }") == 7);
}
TEST_CASE("CIR builder: flat brace initializers") {
    CHECK(cir_run_builder(
        "int main() { int a[3] = {10,20,12}; return a[0]+a[1]+a[2]; }") == 42);
}
```

- [ ] **Step 2: Run them — expect FAIL**

Run: `make -C src test 2>&1 | grep -iE "scalar initializers|flat brace|FAILED" | head`
Expected: FAIL — `var_decl` emits `init_node = ignore()`, so initialized values read as 0 (returns 0 / garbage), not 7 / 42.

- [ ] **Step 3: Declare `init_value` in `src/cir_builder.h`**

Find the `node_t var_decl(...)` declaration and add directly before it:

```cpp
	// Recursively build an initializer value node: scalar expr, or for a
	// nested brace element (TokenStructLit) a LIST(INIT(LIST(), val), ...).
	node_t init_value(TokenBase *elem);
```

- [ ] **Step 4: Add the `init_value` helper in `src/cir_builder.cpp` (immediately before `var_decl`, line 304)**

```cpp
node_t CirBuilder::init_value(TokenBase *elem)
{
	if (!elem) return ignore();
	TokenStructLit *sl = dynamic_cast<TokenStructLit *>(elem);
	if (sl) {
		node_t inner = list();
		for (size_t i = 0; i < sl->inits.size(); i++)
			append(inner, node2(N_INIT, list(), init_value(sl->inits[i])));
		return inner;
	}
	return translate_expr(elem);
}
```

- [ ] **Step 5: Replace the dropped initializer (`src/cir_builder.cpp:360-361`)**

Replace:
```cpp
	node_t init_node = ignore();
	// TODO: initializers will be ported later when we integrate fully
```
with:
```cpp
	node_t init_node = ignore();
	TokenDecl *tdecl = dynamic_cast<TokenDecl *>(origin);
	if (tdecl && tdecl->has_brace_init) {
		// Brace init: LIST(INIT(LIST(), val), ...)
		node_t lst = list();
		for (size_t i = 0; i < tdecl->init_list.size(); i++)
			append(lst, node2(N_INIT, list(), init_value(tdecl->init_list[i])));
		init_node = lst;
	} else if (tdecl && tdecl->initialize) {
		// Scalar init: bare expression
		init_node = translate_expr(tdecl->initialize);
	}
```

- [ ] **Step 6: Run the tests — expect PASS**

Run: `make -C src test 2>&1 | grep -iE "scalar initializers|flat brace|FAILED" | head`
Expected: both pass, no FAILED.

- [ ] **Step 7: Verify node shapes against c2m**

```bash
printf "int main(void){ int x = 7; int a[3] = {1,2,3}; return x + a[2]; }\n" > tmp/t_init.c
scripts/cir_diff.sh --checked tmp/t_init.c
```
Expected: `MATCH` (scalar init is the bare `I`/expr; brace init is `LIST(INIT(LIST(), I), ...)`).

- [ ] **Step 8: Full suite + triage delta**

```bash
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: fulltest green; `runtime_mismatch` + `timeout` drop substantially (this is the silent-miscompile class). Record actuals.

- [ ] **Step 9: Commit**

```bash
git add src/cir_builder.h src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "feat(cir): emit local scalar and flat/nested brace initializers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Nested brace initializers (verify recursion end-to-end)

The `init_value` helper from Task 3 already recurses through `TokenStructLit`. This task confirms it on real nested aggregates and locks it with tests.

**Files:**
- Test: `tests/unit/test_cir.cpp`
- (Modify `src/cir_builder.cpp` only if a gap is found.)

- [ ] **Step 1: Write the tests**

```cpp
TEST_CASE("CIR builder: nested brace initializers") {
    CHECK(cir_run_builder(
        "int main() { int m[2][2] = {{1,2},{3,4}}; return m[0][0]+m[1][1]; }") == 5);
    CHECK(cir_run_builder(
        "struct P { int x, y; };\n"
        "int main() { struct P a[2] = {{1,2},{3,4}}; return a[1].x + a[0].y; }") == 5);
}
```

- [ ] **Step 2: Run them**

Run: `make -C src test 2>&1 | grep -iE "nested brace|FAILED|untranslatable" | head`
Expected: PASS. If they FAIL, the gap is in `init_value`/struct-member emission — investigate with:
```bash
printf "int m[2][2] = {{1,2},{3,4}};\n" > tmp/t_nest.c
scripts/cir_diff.sh --checked tmp/t_nest.c
```
and fix `init_value` to match the `LIST(INIT(LIST(), LIST(INIT(...))))` shape before continuing. (Struct-member-access rejection, if it appears, is a Phase-2 concern — note it and keep the array-nesting test.)

- [ ] **Step 3: Verify against c2m**

```bash
scripts/cir_diff.sh --checked tmp/t_nest.c
```
Expected: `MATCH`.

- [ ] **Step 4: Full suite + triage delta**

```bash
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: fulltest green; record delta.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp tests/unit/test_cir.cpp
git commit -m "test(cir): cover nested brace initializers (array + array-of-struct)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Global `var_decl` initializers

The global path (`translate_module:1180`) passes only `td.var`; `td.origin` is the decl-head type token, not a `TokenDecl`. This task determines how global initializers are represented and wires them through.

**Files:**
- Modify: `src/cir_builder.cpp` (`translate_module:1178-1184` global-var case)
- Test: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("CIR builder: global initializers") {
    CHECK(cir_run_builder("int g = 5; int main() { return g; }") == 5);
    CHECK(cir_run_builder(
        "int t[3] = {4,5,6}; int main() { return t[0]+t[1]+t[2]; }") == 15);
}
```

- [ ] **Step 2: Run it — expect FAIL, and investigate representation**

Run: `make -C src test 2>&1 | grep -iE "global initializers|FAILED" | head`
Then determine where the global initializer lives:
```bash
grep -n "dkGlobalVar\|initialize\|init_list\|has_brace_init" src/parser.cpp | sed -n '1,40p'
```
Expected outcome: identify whether the global's `TokenDecl` is reachable (e.g. recorded in `top_decls` as the origin, or retrievable from the variable's declaration). Two cases:
- **(a)** A `TokenDecl` exists for the global and can be referenced from the `TopDecl` — thread it through (Step 3a).
- **(b)** Globals store init on the `Variable` (e.g. `var->get<>()` / a separate global-init list) — read it there (Step 3b).

- [ ] **Step 3a: If a `TokenDecl` is reachable — pass it through**

Add a `TokenDecl*` to `TopDecl` (or locate the existing decl) and change the global case at `src/cir_builder.cpp:1180`:
```cpp
		case Program::DeclKind::dkGlobalVar: {
			if (td.var && !dynamic_cast<FuncDef *>(td.var->type)) {
				// origin carries the decl head; pass the TokenDecl when present
				node_t gd = var_decl(td.var, td.origin);
				if (gd) { stamp(gd, td); append(top_list, gd); }
			}
			break;
		}
```
…and in the parser (`parser.cpp:14782`) set `gtd.origin` to the `TokenDecl` for global variables (so `dynamic_cast<TokenDecl*>(origin)` in `var_decl` succeeds). Confirm `var_decl`'s existing `tdecl` branch (Task 3) then handles it unchanged.

- [ ] **Step 3b: If init lives on the `Variable` — read it in `var_decl`**

Extend `var_decl`'s initializer block to also consult the variable's stored initial value when no `TokenDecl` is present (exact accessor determined in Step 2; e.g. a constant scalar via `v->get<int64_t>()` emitted as `integer(...)`). Implement the minimal path that makes the scalar test pass; defer aggregate globals if they need more than the variable carries, and log that in the commit.

- [ ] **Step 4: Run the test — expect PASS (at least the scalar case)**

Run: `make -C src test 2>&1 | grep -iE "global initializers|FAILED" | head`
Expected: scalar global passes. If the array-global case can't pass without parser work, mark that assertion with a comment and a triage note rather than forcing it.

- [ ] **Step 5: Verify + full suite + triage**

```bash
printf "int g = 5;\nint t[3] = {4,5,6};\nint main(void){ return g + t[2]; }\n" > tmp/t_glob.c
scripts/cir_diff.sh --checked tmp/t_glob.c
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: `MATCH` for what's implemented; fulltest green; record delta.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(cir): emit global variable initializers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Designated initializers (investigate, then implement or defer)

Only 2 tests use designated init (`testnestdesignatedinit`, `testflexarrayemptyinit`). The JIT backend handles them, but `TokenDecl` has no designator field, so this task first finds the representation.

**Files:**
- Modify: `src/cir_builder.cpp` (`init_value` / `var_decl`) — only if representation supports it
- Test: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Investigate the parser representation**

```bash
grep -n "has_brace_init\|init_list\|StructLit\|designat\|FIELD\|\.field\|\\[.*\\] *=" src/parser.cpp | grep -iE "init|designat|field" | head -30
printf "struct P{int x,y;};\nstruct P p={.y=9,.x=5};\nint main(void){return p.x+p.y;}\n" > tmp/t_desig.mad
bin/madc --backend=jit tmp/t_desig.mad ; echo "rc=$?"
```
Determine: does `{.y=9,.x=5}` get normalized to positional order in `init_list` (so positional `INIT(LIST(), val)` emission is already semantically correct), or is designator info captured somewhere reusable?

- [ ] **Step 2a: If positional-normalized — add a test and confirm it already passes**

```cpp
TEST_CASE("CIR builder: designated initializers (positional-normalized)") {
    CHECK(cir_run_builder(
        "struct P { int x, y; };\n"
        "int main() { struct P p = { .y = 9, .x = 5 }; return p.x*10 + p.y; }") == 59);
}
```
Run: `make -C src test 2>&1 | grep -iE "designated|FAILED" | head` — expect PASS (Task 3's brace path already emits the values in normalized order). Then verify: `scripts/cir_diff.sh --checked tmp/t_desig2.c` against a c2m reducer.

- [ ] **Step 2b: If designators ARE captured — emit the designated shape**

Extend `init_value`/`var_decl` to wrap with the designator list: `.f=v` → `node2(N_INIT, /*designator*/ append(list(), node1(N_FIELD_ID, id(field))), val)`; `[i]=v` → `node2(N_INIT, append(list(), <const-expr>), val)`. Add the test from Step 2a and make it pass.

- [ ] **Step 2c: If unreachable — DEFER, with explicit note**

Add a `tests/<name>.mir_skip` for the 2 designated tests (or document in the triage note) and record in the commit + memory that designated init needs parser work (capturing a designator on each init element). Do not silently drop.

- [ ] **Step 3: Full suite + triage**

```bash
make -C src fulltest 2>&1 | tail -3
bash tmp/cir_triage.sh 2>/dev/null | sed -n '1,8p'
```
Expected: fulltest green; record delta.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(cir): designated initializers (implemented or deferred per investigation)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Measure, document, and update memory/status

**Files:**
- Modify: memory `project_cir_triage.md`, `MEMORY.md`
- (Optionally) `docs/test-status.md`

- [ ] **Step 1: Final histogram**

```bash
bash scripts/run_tests.sh --backend=cir 2>/dev/null | tail -1
bash tmp/cir_triage.sh 2>/dev/null
```
Record the new `--backend=cir` pass count and the full histogram.

- [ ] **Step 2: Update memory `project_cir_triage.md`**

Append a "Phase-1 result (2026-05-29)" section: baseline 55/419 → new count; per-target deltas; which failure modes shrank; the new dominant remaining bucket (drives Phase 2). Update the `MEMORY.md` one-line pointer if the headline number changed.

- [ ] **Step 3: Update the KG (if reachable)**

```bash
scripts/kg_query.sh -ro "MATCH (n) WHERE n.name CONTAINS 'cir' RETURN n.name LIMIT 5"
```
If reachable, add/update a Decision or Session node recording the Phase-1 coverage result. If not, note the sync debt.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs(cir): record Phase-1 coverage results and remaining gap

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 5: Push**

```bash
git push
```

---

## Self-review notes

- **Spec coverage:** char (Task 1), subscript (Task 2), scalar/flat/nested local init (Tasks 3-4), global init (Task 5), designated (Task 6), verification loop + measurement (every task + Task 7). Test-helper gap (legacy `cir_run`) covered by Task 0. All spec sections map to tasks.
- **Honesty gate:** every implementation task re-runs `tmp/cir_triage.sh` and records actuals, including newly-surfaced failure modes — matching the spec's "gate-clearing ≠ green" caveat.
- **No silent caps:** Task 6 forbids silently dropping designated init (skip-file + note if deferred).
- **Type consistency:** `ch(long, TokenBase*)`, `init_value(TokenBase*)`, `var_decl(Variable*, TokenBase* origin)` names are used consistently across declaration (cir_builder.h) and definition/call sites.
- **Risk:** Task 5 has a genuine branch (3a vs 3b) because the global-init representation is not yet known — its Step 2 is an investigation with concrete commands, not a placeholder.
