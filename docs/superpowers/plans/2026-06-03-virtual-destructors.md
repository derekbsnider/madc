# Virtual Destructors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `virtual ~X()` makes a class polymorphic; `delete base_ptr` runs the most-derived destructor via the vtable; `typeid`/`dynamic_cast` work on a destructor-only base — all matching `g++ -std=c++17` byte-for-byte.

**Architecture:** Faithful Itanium D1/D0 two-slot destructor layout. The parser registers two class-name-independent vtable markers (`"~"` = D1 complete, `"~$deleting"` = D0 deleting) at the destructor's declaration position; `class_vtable_def` resolves them to the current class's complete/deleting destructor symbols (most-derived = the override); a new `synth_deleting_dtor_def` emits `Cls___dtor_deleting(p){ complete_dtor(p); free(p); }`; and `delete` of a virtual-destructor type dispatches through the D0 slot (dropping its own `free`). Stack/by-value destruction is unchanged.

**Tech Stack:** madc parser (`src/parser.cpp`), cir_builder (`src/cir_builder.cpp`/`.h`), `DataDefCLASS` vtable model (`include/datadef.h`), c2mir→MIR backend; doctest units + `.mad` integration matched to g++.

**Spec:** `docs/superpowers/specs/2026-06-03-virtual-destructors-design.md`.
**Branch:** `feature/cpp-virtual-dtors-claude` (already cut off campaign HEAD `0c11549`).
**KG:** Feature `Virtual Destructors`; deferred Gaps `pure_virtual_and_abstract_classes`, `explicit_pseudo_destructor_call`.

> **Decomposition note (read before starting):** `tests/unit/test_class_layout.cpp` does NOT run the lexer/parser — it builds `DataDefCLASS` objects directly with `mkclass(name, ndata, poly)` and tests `compute_layout()`/`build_vtable_groups()` (it links `parser.o` only for the global `dd*` instances). So parser-level behavior is verified through **integration `.mad` tests** (real `bin/madc`), and the doctest only locks the layout/grouping invariant on hand-built classes. That is why parser registration + vtable emission + D0 synth are ONE task (Task 1) — the first observable behavior is the integration RTTI test; the parser change alone has no runnable assertion in this harness.

---

## Conventions every task follows

- **g++ is canon.** Each `.mad` test's expected output is produced by `g++ -std=c++17 -x c++ <file> -o tmp/oracle && tmp/oracle`. If g++ and the plan disagree, g++ wins — stop and report.
- **Single shell commands**, no `&&` chains. Cap madc runs: `( ulimit -t 60; timeout 60 <cmd> )`.
- **Scratch in `tmp/`** only.
- **Gate (run after the task's own test passes):**
  - `make -C src 2>&1 | tail -15` → 0 warnings, 0 errors.
  - `( ulimit -t 600; timeout 500 bash scripts/run_tests.sh > tmp/vd_tests.log 2>&1 ); tail -3 tmp/vd_tests.log` then `grep "^FAIL:" tmp/vd_tests.log | sort > tmp/vd_failset.txt; diff tmp/baseline_failset.txt tmp/vd_failset.txt`. **Baseline = 471 pass / 7 fail** (`testcin testdefer testforeach2 testfortypedcomma`[FLAKY, may flip fail↔timeout — ignore only this] `testfstream testlargesizeofquery testloop`, captured in `tmp/baseline_failset.txt`). Pass count must only rise; no NEW failure.
  - `( ulimit -t 60; bash scripts/check-no-std-hardcoding.sh 2>&1 | head -1 )` → "468 offending lines remain" (unchanged).
  - **SMAUG soak is the COORDINATOR's job, not the implementer's** — the implementer reports DONE; the coordinator re-builds, re-runs the suite, diffs the failset, and runs the SMAUG soak. (Virtual destructors do not appear in C89 SMAUG, but vtable/`delete` codegen changed, so soak anyway.)
- **Commit trailer (exact):** `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Do NOT push (coordinator pushes after verifying). Do NOT edit develop-mirror artifacts.

---

## Task 1: Parser registration + vtable D1/D0 emission + deleting-dtor synthesis

This is the machinery that makes a virtual-destructor class polymorphic and gives it a correct vtable + `_ZTI`, so `typeid`/`dynamic_cast` work on a destructor-only base. Three coordinated edits, one observable test.

**Files:**
- Modify: `src/parser.cpp` (destructor branch ~12004–12030; base-merge loop 11947–11955; `vtable_slot()` is `include/datadef.h:758`)
- Modify: `src/cir_builder.h` (declare `synth_deleting_dtor_def`)
- Modify: `src/cir_builder.cpp` (`synth_deleting_dtor_def` beside `synth_complete_dtor_def` ~3645; module emission loop ~8046–8060; `class_vtable_def` slot loop ~2944)
- Test: `tests/test_vdtor_rtti.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing integration test.** Create `tests/test_vdtor_rtti.mad`:
```cpp
#include <iostream>
#include <typeinfo>
using namespace std;
struct Base { virtual ~Base() {} };
struct Derived : Base { int x; };
int main() {
    Base *b = new Derived();
    cout << (typeid(*b) == typeid(Derived)) << endl;   // 1
    Derived *d = dynamic_cast<Derived *>(b);
    cout << (d != 0) << endl;                           // 1
    delete b;
    return 0;
}
```
Create `tests/test_vdtor_rtti.expect`:
```
1
1
```
Oracle: `( ulimit -t 60; g++ -std=c++17 -x c++ tests/test_vdtor_rtti.mad -o tmp/vr ); tmp/vr` → `1` then `1`.

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_rtti.mad 2>&1 | grep -v setrlimit | tail -6 )`
Expected (pre-fix): a compile/link failure about an undefined `_ZTI*`/`Base__vtable`, or wrong RTTI — the destructor-only class is not polymorphic, so no vtable/`_ZTI` is emitted. (If it already prints `1`/`1`, STOP and report — do not invent a change.)

- [ ] **Step 3a: Parser — register the D1/D0 slots.** In `src/parser.cpp`, in the destructor branch, right after the block that sets `ddc->has_user_dtor = true;` and calls `bind_std_libstdcpp_symbol(...)` (around line 12027), add:
```cpp
		// Virtual destructor: register the Itanium D1/D0 slot pair at the
		// destructor's declaration position. Markers are class-name-INDEPENDENT
		// ("~" = D1 complete-object dtor, "~$deleting" = D0 deleting dtor) so a
		// base and its overriding derived class SHARE the slot (the derived
		// inherits "~"/"~$deleting" via the base-merge loop at ~11952; an override
		// re-resolves them to its own symbols in class_vtable_def). The guard is
		// also true (inherited virtuality) when a base dtor is virtual, which shows
		// up as the markers already being present.
		if ( is_virtual || ddc->vtable_slot("~") >= 0 )
		{
		    ddc->virtual_methods["~" + tag->str] = true;
		    if ( ddc->vtable_slot("~") < 0 )
			ddc->vtable_slots.push_back("~");
		    if ( ddc->vtable_slot("~$deleting") < 0 )
			ddc->vtable_slots.push_back("~$deleting");
		    ddc->has_vtable = true;
		}
```
(Confirm `tag->str`, `ddc`, `is_virtual` are in scope here — all are used a few lines up in the same branch: `tag->str` builds `mangled = tag->str + "___dtor"`.)

- [ ] **Step 3b: Header — declare the deleting-dtor synth.** In `src/cir_builder.h`, next to `node_t synth_complete_dtor_def(DataDefCLASS *cdd);` (~561) add:
```cpp
	node_t synth_deleting_dtor_def(DataDefCLASS *cdd);
```

- [ ] **Step 3c: cir_builder — define the deleting-dtor synth.** In `src/cir_builder.cpp`, immediately after `synth_complete_dtor_def` (ends ~3672), add:
```cpp
// void Cls___dtor_deleting(struct Cls *__this) { <complete-dtor>(__this); free(__this); }
// The Itanium D0 (deleting) destructor: complete-object destruction, then operator
// delete (free, for madc user classes). Referenced from the D0 vtable slot.
node_t CirBuilder::synth_deleting_dtor_def(DataDefCLASS *cdd)
{
	node_t ret_type = node1(N_LIST, simple(N_VOID));
	node_t pspec = node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t param = simple(N_SPEC_DECL);
	append(param, pspec);
	append(param, node2(N_DECL, id("__this"), node1(N_LIST, pointer())));
	append(param, ignore());
	append(param, ignore());
	append(param, ignore());
	node_t param_list = list();
	append(param_list, param);
	node_t decl = node2(N_DECL, id((cdd->name + "___dtor_deleting").c_str()),
			    node1(N_LIST, node1(N_FUNC, param_list)));
	std::vector<node_t> stmts;
	std::string csym = class_complete_dtor_symbol(cdd);   // D1 (plain dtor if no vbases)
	referenced_funcs.insert(csym);
	node_t ca = list();
	append(ca, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id(csym.c_str()), ca)));
	need_output_extern("free", false, { { {N_VOID}, true } });
	node_t fa = list();
	append(fa, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id("free"), fa)));
	node_t items = list();
	for (node_t s : stmts) append(items, s);
	node_t body = node2(N_BLOCK, list(), items);
	return node4(N_FUNC_DEF, ret_type, decl, list(), body);
}
```

- [ ] **Step 3d: cir_builder — emit the deleting dtor for classes that carry a D0 slot.** Find the module emission loop that emits complete dtors (the `emitted_complete_dtors` set ~8050, where `synth_complete_dtor_def(cdd)` is appended). READ the surrounding ~12 lines to learn the exact append-target variable (e.g. `top_list`) and the iteration variable. Add a sibling dedupe set near where `emitted_complete_dtors` is declared:
```cpp
	std::set<DataDefCLASS *> emitted_deleting_dtors;
```
and inside the same loop body (right after the complete-dtor emission), using the SAME append target:
```cpp
		if (cdd->vtable_slot("~$deleting") >= 0
		    && !emitted_deleting_dtors.count(cdd)) {
			emitted_deleting_dtors.insert(cdd);
			node_t dd0 = synth_deleting_dtor_def(cdd);
			if (dd0) append(top_list, dd0);   // MATCH the complete-dtor append target name
		}
```

- [ ] **Step 3e: cir_builder — resolve the D1/D0 markers in the vtable.** In `class_vtable_def`'s slot loop (`for (const std::string &slot : G.slots)` ~2944), BEFORE `Variable *mv = cdd->findMethod(sname);`, special-case the markers (single-inheritance / primary group only here — secondary-group thunks are Task 3):
```cpp
		for (const std::string &slot : G.slots) {
			std::string sname = slot;
			// Destructor slots resolve to synthesized symbols of the CURRENT
			// class (most-derived = the override), not findMethod. D1 = complete
			// dtor, D0 = deleting dtor.
			if (sname == "~" || sname == "~$deleting") {
				std::string dsym = (sname == "~")
					? class_complete_dtor_symbol(cdd)
					: (cdd->name + "___dtor_deleting");
				referenced_funcs.insert(dsym);
				node_t vptr_type = node2(N_TYPE,
					node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t fnref = node2(N_CAST, vptr_type, id(dsym.c_str()));
				append(inits, node2(N_INIT, list(), fnref));
				continue;
			}
			Variable *mv = cdd->findMethod(sname);
			// ... existing body unchanged ...
```
(Leave the rest of the loop body verbatim for non-destructor slots.)

- [ ] **Step 4: Build + verify pass.**
`make -C src 2>&1 | tail -15` (0 warn/err), then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_rtti.mad 2>&1 | grep -v setrlimit )` → `1` then `1`.

- [ ] **Step 5: Regression gate** (failset == baseline +1 new pass = 472; no-std 468).

- [ ] **Step 6: Commit.**
```bash
git add src/parser.cpp src/cir_builder.cpp src/cir_builder.h tests/test_vdtor_rtti.mad tests/test_vdtor_rtti.expect
git commit -m "feat(vdtor): virtual-dtor polymorphism — D1/D0 slots, vtable, deleting-dtor synth (1/4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Virtual dispatch on `delete`

**Files:**
- Modify: `src/cir_builder.cpp` (`TokenDELETE` lowering ~5052–5079; mirrors method-call dispatch ~3315–3340; `find_vslot` is `include/datadef.h:776`)
- Test: `tests/test_vdtor_delete.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing test.** Create `tests/test_vdtor_delete.mad`:
```cpp
#include <iostream>
using namespace std;
struct Base { virtual ~Base() { cout << "~Base" << endl; } };
struct Derived : Base { ~Derived() { cout << "~Derived" << endl; } };
int main() {
    Base *b = new Derived();
    delete b;                 // must print ~Derived then ~Base
    return 0;
}
```
Create `tests/test_vdtor_delete.expect`:
```
~Derived
~Base
```
Oracle: `( ulimit -t 60; g++ -std=c++17 -x c++ tests/test_vdtor_delete.mad -o tmp/vd ); tmp/vd` → `~Derived` then `~Base`.

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_delete.mad 2>&1 | grep -v setrlimit )`
Expected (pre-fix): prints only `~Base` (static dispatch ran the base dtor).

- [ ] **Step 3: Implement.** In the `TokenDELETE` block (~5055), insert a virtual-dtor branch BEFORE the existing `if (cdd && cdd->has_user_dtor)` static path. Dispatch through the D0 slot and DROP the separate `free`. Re-translate the operand for the call argument (the established "same node may appear once" idiom the existing non-virtual path uses for `free`):
```cpp
	if (TokenDELETE *tdl = dynamic_cast<TokenDELETE *>(tb)) {
		DataDefCLASS *cdd = tdl->del_class;
		// Virtual destructor: dispatch through the vtable D0 (deleting) slot, which
		// runs the most-derived complete destructor AND frees. No separate free().
		if (cdd && cdd->vtable_slot("~$deleting") >= 0) {
			size_t grp; int slot;
			cdd->find_vslot("~$deleting", grp, slot);
			const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[grp];
			std::string vfld = (G.this_offset == 0)
				? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
			node_t recv_spec = node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore())),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			node_t recv = node2(N_CAST, recv_spec, translate_expr(tdl->expr), tb);
			node_t vptr = node2(N_DEREF_FIELD, recv, id(vfld.c_str(), tb), tb);
			node_t vpp_dl = list();
			append(vpp_dl, pointer());
			append(vpp_dl, pointer());
			node_t vpp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
						node2(N_DECL, ignore(), vpp_dl));
			node_t vtab = node2(N_CAST, vpp_type, vptr, tb);
			node_t slotref = node2(N_IND, vtab, integer(slot, tb), tb);
			// cast slot to void (*)(void *), call with the pointer (re-translated)
			node_t fp_dl = list();
			append(fp_dl, node1(N_FUNC,
				node1(N_LIST, node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer()))))));
			append(fp_dl, pointer());
			node_t fp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					       node2(N_DECL, ignore(), fp_dl));
			node_t fn = node2(N_CAST, fp_type, slotref, tb);
			node_t darg = list();
			append(darg, translate_expr(tdl->expr));
			node_t dcall = node2(N_CALL, fn, darg, tb);
			node_t items = list();
			append(items, node2(N_EXPR, list(), dcall, tb));
			append(items, node2(N_EXPR, list(), integer(0, tb), tb));
			node_t block = node2(N_BLOCK, list(), items, tb);
			return node1(N_STMTEXPR, block, tb);
		}
		node_t ptr = translate_expr(tdl->expr);
		node_t items = list();
		// ... existing non-virtual path unchanged ...
```
(Keep the existing non-virtual body below verbatim. VERIFY the `void(*)(void*)` declarator: if c2mir rejects the hand-built `fp_type`, copy the proven function-pointer cast shape from the method dispatch at ~3338 / `method_fnptr_type` — `grep -n "N_FUNC" src/cir_builder.cpp` for a working indirect-call cast and match it.)

- [ ] **Step 4: Build + verify pass.**
`make -C src 2>&1 | tail -15`, then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_delete.mad 2>&1 | grep -v setrlimit )` → `~Derived` then `~Base`.

- [ ] **Step 5: No-double-free check.** Scratch `tmp/vd_df.mad`: `new`/`delete` a virtual-dtor object 100× in a loop, print `ok`. Run it — must print `ok`, no `free(): double free`/`munmap_chunk` abort. (Scratch only; do not commit.)

- [ ] **Step 6: Regression gate** (failset == baseline +2 = 473; no-std 468). `delete` codegen changed → COORDINATOR runs the SMAUG soak.

- [ ] **Step 7: Commit.**
```bash
git add src/cir_builder.cpp tests/test_vdtor_delete.mad tests/test_vdtor_delete.expect
git commit -m "feat(vdtor): virtual dispatch on delete via the D0 vtable slot (2/4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Multiple inheritance — this-adjusting D1/D0 thunks in secondary groups

**Files:**
- Modify: `src/cir_builder.cpp` (the destructor-marker branch from Task 1 Step 3e, ~2944; add a destructor thunk parallel to `make_thunk` ~2879)
- Test: `tests/test_vdtor_mi.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing test.** Create `tests/test_vdtor_mi.mad`:
```cpp
#include <iostream>
using namespace std;
struct A { virtual void fa() {} virtual ~A() { cout << "~A" << endl; } };
struct B { virtual void fb() {} virtual ~B() { cout << "~B" << endl; } };
struct C : A, B { ~C() { cout << "~C" << endl; } };   // B is the secondary base
int main() {
    B *b = new C();           // points at the B subobject
    delete b;                 // most-derived first
    return 0;
}
```
Produce the oracle and use ITS exact order for `.expect`:
`( ulimit -t 60; g++ -std=c++17 -x c++ tests/test_vdtor_mi.mad -o tmp/vmi ); tmp/vmi` (expected `~C` `~B` `~A`; use whatever g++ prints).

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_mi.mad 2>&1 | grep -v setrlimit )`
Expected (pre-fix): wrong output or a crash — the secondary-group D0 slot points at C's deleting dtor WITHOUT `this`-adjustment, so it gets the B-subobject pointer.

- [ ] **Step 3: Implement the destructor thunk.** Beside `make_thunk` in `class_vtable_def` (~2879) add:
```cpp
	// This-adjusting thunk for a destructor slot reached through a secondary-base
	// vptr: void Cls__dthunk_<off>_<tag>(char *__self){ target(__self - off); }
	// target = the most-derived complete (D1) or deleting (D0) dtor symbol, which
	// then operates on / frees the COMPLETE-object pointer.
	auto make_dtor_thunk = [&](const std::string &target_sym, size_t off,
				   const char *tag) -> std::string {
		std::string tname = cdd->name + "__dthunk_" + std::to_string(off) + "_" + tag;
		node_t ret_spec = node1(N_LIST, simple(N_VOID));
		node_t pspec = simple(N_SPEC_DECL);
		append(pspec, node1(N_LIST, simple(N_CHAR)));
		append(pspec, node2(N_DECL, id("__self"), node1(N_LIST, pointer())));
		append(pspec, ignore()); append(pspec, ignore()); append(pspec, ignore());
		node_t plist = list();
		append(plist, pspec);
		node_t tdecl = node2(N_DECL, id(tname.c_str()),
				     node1(N_LIST, node1(N_FUNC, plist)));
		// (struct Cls *)(__self - off)
		node_t adj = node2(N_SUB, id("__self"), integer((long)off));
		node_t cls_spec = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t adj_cast = node2(N_CAST, cls_spec, adj);
		node_t a = list();
		append(a, adj_cast);
		node_t call = node2(N_CALL, id(target_sym.c_str()), a);
		referenced_funcs.insert(target_sym);
		node_t body = node2(N_BLOCK, list(),
				    node1(N_LIST, node2(N_EXPR, list(), call)));
		thunks.push_back(node4(N_FUNC_DEF, ret_spec, tdecl, list(), body));
		return tname;
	};
```
Then in the Task-1 destructor-marker branch, thunk when `G.this_offset != 0`:
```cpp
			if (sname == "~" || sname == "~$deleting") {
				std::string dsym = (sname == "~")
					? class_complete_dtor_symbol(cdd)
					: (cdd->name + "___dtor_deleting");
				if (G.this_offset != 0)
					dsym = make_dtor_thunk(dsym, G.this_offset,
						(sname == "~") ? "D1" : "D0");
				else
					referenced_funcs.insert(dsym);
				node_t vptr_type = node2(N_TYPE,
					node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t fnref = node2(N_CAST, vptr_type, id(dsym.c_str()));
				append(inits, node2(N_INIT, list(), fnref));
				continue;
			}
```

- [ ] **Step 4: Build + verify pass.**
`make -C src 2>&1 | tail -15`, then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_mi.mad 2>&1 | grep -v setrlimit )` → matches the g++ oracle order.

- [ ] **Step 5: Regression gate** (failset == baseline +3 = 474; no-std 468). Codegen change → COORDINATOR SMAUG soak.

- [ ] **Step 6: Commit.**
```bash
git add src/cir_builder.cpp tests/test_vdtor_mi.mad tests/test_vdtor_mi.expect
git commit -m "feat(vdtor): this-adjusting D1/D0 thunks for secondary-base virtual dtors (3/4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Hardening — layout doctest + override-chain + unchanged static path

**Files:**
- Modify: `tests/unit/test_class_layout.cpp` (append a case; uses the existing `mkclass(name, ndata, poly)` helper — NO parser)
- Test: `tests/test_vdtor_chain.mad` (+ `.expect`)

- [ ] **Step 1: Write the layout doctest (hand-built class, matching the harness).** `test_class_layout.cpp` builds classes with `mkclass` and calls `build_vtable_groups()` directly — it does NOT parse. So construct the slot order by hand (mirroring the parser's declaration-order registration) and assert the grouping the spec's g++ dump requires. Append:
```cpp
TEST_CASE("dtor D1/D0 markers keep declaration order and group layout") {
    // mirror `struct A { virtual void f(); virtual ~A(); virtual void g(); }`:
    // declaration order f, ~ (D1), ~$deleting (D0), g.
    DataDefCLASS *a = mkclass("A", 0, true);   // poly=true -> has_vtable
    a->vtable_slots.push_back("f");
    a->vtable_slots.push_back("~");
    a->vtable_slots.push_back("~$deleting");
    a->vtable_slots.push_back("g");
    a->compute_layout();
    a->build_vtable_groups();
    REQUIRE(a->vtable_groups.size() == 1);     // single class -> one (primary) group
    const auto &g0 = a->vtable_groups[0];
    REQUIRE(g0.slots.size() == 4);
    CHECK(g0.slots[0] == "f");
    CHECK(g0.slots[1] == "~");                 // D1 right after f
    CHECK(g0.slots[2] == "~$deleting");        // D0 right after D1
    CHECK(g0.slots[3] == "g");                 // g after the dtor pair
    CHECK(g0.addr_point == 2);                 // 2-slot Itanium prologue precedes slot 0
    // find_vslot resolves the markers to consecutive in-group slots
    size_t grp; int s1, s0;
    CHECK(a->find_vslot("~", grp, s1));
    CHECK(a->find_vslot("~$deleting", grp, s0));
    CHECK(s0 == s1 + 1);
}
```
(Read the top of `test_class_layout.cpp` first to confirm `mkclass`'s signature and that `build_vtable_groups`/`find_vslot`/`vtable_groups` are reachable — they are members of `DataDefCLASS`, linked via `parser.o`.)

- [ ] **Step 2: Build the units, verify the new case passes.**
Run: `( ulimit -t 200; timeout 200 make -C src test 2>&1 | tail -20 )`
Expected: the new TEST_CASE passes (Tasks 1–3 already make `build_vtable_groups` group these correctly); all other doctest cases still SUCCESS. If `addr_point` differs, reconcile with `build_vtable_groups` (parser.cpp:3856, `PROLOGUE = 2`).

- [ ] **Step 3: Write the override-chain integration test.** Create `tests/test_vdtor_chain.mad`:
```cpp
#include <iostream>
using namespace std;
struct A { virtual ~A() { cout << "~A" << endl; } };
struct B : A { ~B() { cout << "~B" << endl; } };
struct C : B { ~C() { cout << "~C" << endl; } };
struct Plain { ~Plain() { cout << "~Plain" << endl; } };   // non-virtual dtor
int main() {
    A *a = new C();
    delete a;                 // ~C ~B ~A
    Plain *p = new Plain();
    delete p;                 // ~Plain (static path, unchanged)
    return 0;
}
```
Create `tests/test_vdtor_chain.expect`:
```
~C
~B
~A
~Plain
```
Oracle: `( ulimit -t 60; g++ -std=c++17 -x c++ tests/test_vdtor_chain.mad -o tmp/vc ); tmp/vc` → that exact output.

- [ ] **Step 4: Run it, verify pass** (Tasks 1–2 implement it; this locks the multi-level chain + that the non-virtual sibling still uses the static path).
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_chain.mad 2>&1 | grep -v setrlimit )`
Expected: `~C` `~B` `~A` `~Plain`.

- [ ] **Step 5: Full regression gate** (build 0-warn; `make -C src test` all SUCCESS; `run_tests.sh` failset == baseline, pass count 475; no-std 468). COORDINATOR runs the final SMAUG soak + serpent combat (broad codegen touched).

- [ ] **Step 6: Commit.**
```bash
git add tests/unit/test_class_layout.cpp tests/test_vdtor_chain.mad tests/test_vdtor_chain.expect
git commit -m "test(vdtor): layout doctest + override-chain + non-virtual-sibling (4/4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After all tasks

- Coordinator: final `make -C src fulltest`, failset diff == baseline (expect +4 integration tests → 475), SMAUG boot + serpent combat, then push.
- Update KG: Feature `Virtual Destructors` status → done (landing commit range); leave the two deferred Gaps open.
- Update memory `project_cpp_parser_correctness` (Stage C done) + `MEMORY.md`.
- Finishing-a-development-branch: merge `feature/cpp-virtual-dtors-claude` back into `feature/retire-std-hardcoding-claude` (ff).

---

## Self-Review

**1. Spec coverage:** Parser registration (spec §1) → Task 1 Step 3a. Vtable D1/D0 + dtor-only-class table (spec §2) → Task 1 Steps 3e (markers) + the fact that non-empty `vtable_slots` lets `class_vtable_def` past its empty-guard. D0 synthesis (spec §3) → Task 1 Steps 3b–3d. Virtual `delete` (spec §4) → Task 2. MI secondary-base thunks (spec §2 note) → Task 3. Testing (spec §Testing): dtor-only RTTI → Task 1; delete order → Task 2; MI → Task 3; layout doctest + chain + unchanged static path + no-double-free → Tasks 2/4. Deferred (pure-virtual, pseudo-dtor) → out, KG-tracked. No gaps.

**2. Placeholder scan:** Every code step has complete node-construction code. The "VERIFY/READ" notes (the `emitted_complete_dtors` append-target variable; the `void(*)(void*)` declarator vs the proven method-dispatch cast; `mkclass` signature) are explicit pre-write checks naming the exact grep/precedent — they hedge facts that drift (list-variable names, c2mir declarator acceptance), not vague work. No "handle edge cases" left abstract.

**3. Type/decomposition consistency:** Markers `"~"`/`"~$deleting"` and symbol `cdd->name + "___dtor_deleting"` are identical across Tasks 1–4; `class_complete_dtor_symbol` is the D1 symbol everywhere. `synth_deleting_dtor_def` declared (1 Step 3b), defined (3c), called (3d). `make_dtor_thunk` pushes to the same `thunks` vector `make_thunk` uses. The doctest (Task 4) uses the REAL `mkclass`/`build_vtable_groups`/`find_vslot` harness (verified by reading the file), not a non-existent parse helper — the decomposition note at the top explains why parser registration is folded into Task 1 rather than unit-tested in isolation.
