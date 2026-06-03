# Virtual Destructors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `virtual ~X()` makes a class polymorphic; `delete base_ptr` runs the most-derived destructor via the vtable; `typeid`/`dynamic_cast` work on a destructor-only base — all matching `g++ -std=c++17` byte-for-byte.

**Architecture:** Faithful Itanium D1/D0 two-slot destructor layout. The parser registers two class-name-independent vtable markers (`"~"` = D1 complete, `"~$deleting"` = D0 deleting) at the destructor's declaration position; `class_vtable_def` resolves them to the current class's complete/deleting destructor symbols (most-derived = the override); a new `synth_deleting_dtor_def` emits `Cls___dtor_deleting(p){ complete_dtor(p); free(p); }`; and `delete` of a virtual-destructor type dispatches through the D0 slot (dropping its own `free`). Stack/by-value destruction is unchanged.

**Tech Stack:** madc parser (`src/parser.cpp`), cir_builder (`src/cir_builder.cpp`/`.h`), `DataDefCLASS` vtable model (`include/datadef.h`), c2mir→MIR backend; doctest units + `.mad` integration matched to g++.

**Spec:** `docs/superpowers/specs/2026-06-03-virtual-destructors-design.md`.
**Branch:** `feature/cpp-virtual-dtors-claude` (already cut off campaign HEAD `0c11549`).
**KG:** Feature `Virtual Destructors`; deferred Gaps `pure_virtual_and_abstract_classes`, `explicit_pseudo_destructor_call`.

---

## Conventions every task follows

- **g++ is canon.** Each `.mad` test's expected output is produced by `g++ -std=c++17 -x c++ <file> -o tmp/oracle && tmp/oracle`. If g++ and the plan disagree, g++ wins — stop and report.
- **Single shell commands**, no `&&` chains. Cap madc runs: `( ulimit -t 60; timeout 60 <cmd> )`.
- **Scratch in `tmp/`** only.
- **Gate (run after the task's own test passes):**
  - `make -C src 2>&1 | tail -15` → 0 warnings, 0 errors.
  - `( ulimit -t 600; timeout 500 bash scripts/run_tests.sh > tmp/vd_tests.log 2>&1 ); tail -3 tmp/vd_tests.log` then `grep "^FAIL:" tmp/vd_tests.log | sort > tmp/vd_failset.txt; diff tmp/baseline_failset.txt tmp/vd_failset.txt`. **Baseline = 471 pass / 7 fail** (`testcin testdefer testforeach2 testfortypedcomma`[FLAKY, may flip fail↔timeout — ignore only this] `testfstream testlargesizeofquery testloop`). Pass count must only rise; no NEW failure.
  - `( ulimit -t 60; bash scripts/check-no-std-hardcoding.sh 2>&1 | head -1 )` → "468 offending lines remain" (unchanged).
  - **SMAUG soak is the COORDINATOR's job, not the implementer's** — the implementer reports DONE; the coordinator re-builds, re-runs the suite, diffs the failset, and runs the SMAUG soak. (Virtual destructors do not appear in C89 SMAUG, but vtable/`delete` codegen changed, so soak anyway.)
- **Commit trailer (exact):** `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Do NOT push (coordinator pushes after verifying). Do NOT edit develop-mirror artifacts.

---

## Task 1: Parser — register the destructor's D1/D0 vtable slots

**Files:**
- Modify: `src/parser.cpp` (destructor branch, ~12004–12030; uses the base-merge loop at 11947–11955 and `vtable_slot()` from `include/datadef.h:758`)
- Test: `tests/unit/test_class_layout.cpp` (append a case)

Background: today the destructor branch parses `~Tag()` but never looks at `is_virtual` (parsed at 11993) — so a virtual destructor sets no vtable slot. The virtual-*method* path at 12218 is the template to mirror. The base-merge loop at 11952 already copies a base's `vtable_slots` into the derived class, so an inherited virtual destructor's markers are already present on a derived class.

- [ ] **Step 1: Write the failing doctest.** Append to `tests/unit/test_class_layout.cpp` (before the final closing brace of the file; follow the existing `TEST_CASE` style there). This parses two classes via the real parser path the other cases in that file use — match how the existing cases construct/inspect a `DataDefCLASS` (read the top of the file first to copy the harness/idioms). Assert:

```cpp
TEST_CASE("virtual destructor registers D1/D0 vtable slots and polymorphism") {
    // class W { public: virtual ~W(); };  -- destructor-only polymorphic class
    DataDefCLASS *W = parse_one_class(
        "class W { public: virtual ~W() {} };");
    REQUIRE(W != nullptr);
    CHECK(W->has_vtable == true);                 // dtor-only class IS polymorphic
    CHECK(W->vtable_slot("~") >= 0);              // D1 marker present
    CHECK(W->vtable_slot("~$deleting") >= 0);     // D0 marker present
    // the two dtor slots are consecutive (D1 immediately before D0)
    CHECK(W->vtable_slot("~$deleting") == W->vtable_slot("~") + 1);
}
```
If `tests/unit/test_class_layout.cpp` has no `parse_one_class` helper, use whatever the existing cases use to obtain a parsed `DataDefCLASS` from source (read the file first and reuse that exact mechanism — do NOT invent a new harness).

- [ ] **Step 2: Build the unit test, verify it fails.**
Run: `( ulimit -t 200; timeout 200 make -C src test 2>&1 | tail -20 )`
Expected: the new CHECKs fail (`has_vtable` false / slots `-1`).

- [ ] **Step 3: Implement.** In `src/parser.cpp`, in the destructor branch, after the destructor `Variable *mvar` is found and pushed (right after the existing block that sets `ddc->has_user_dtor = true;` and binds the std symbol, around line 12027), add the slot registration. Guard on declared-virtual OR inherited-virtual (a base's `"~"` marker already copied in):

```cpp
		// Virtual destructor: register the Itanium D1/D0 slot pair at the
		// destructor's declaration position. Markers are class-name-INDEPENDENT
		// ("~" = D1 complete-object dtor, "~$deleting" = D0 deleting dtor) so a
		// base and its overriding derived class SHARE the slot (the derived
		// inherits "~"/"~$deleting" via the base-merge loop at ~11952 and an
		// override re-resolves them to its own symbols in class_vtable_def).
		// is_virtual is also true (inherited) when any base dtor is virtual,
		// which shows up as the markers already being present.
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
(Confirm `tag->str` is the class name in scope here — it is the same `tag->str` used three lines up to build `mangled = tag->str + "___dtor"`. Confirm `ddc` and `is_virtual` are in scope — both are used in this branch / loop already.)

- [ ] **Step 4: Build + verify pass.**
Run: `( ulimit -t 200; timeout 200 make -C src test 2>&1 | tail -20 )`
Expected: the new TEST_CASE passes; all other doctest cases still SUCCESS.

- [ ] **Step 5: Regression gate** (build 0-warn; `run_tests.sh` failset == baseline; no-std 468). The integration suite should be UNCHANGED here (no codegen yet): still 471.

- [ ] **Step 6: Commit.**
```bash
git add src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(parser): register D1/D0 vtable slots for a virtual destructor (vdtor 1/5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Vtable emission + deleting-destructor synthesis

**Files:**
- Modify: `src/cir_builder.cpp` (`class_vtable_def` slot loop, ~2944–2964; new `synth_deleting_dtor_def` beside `synth_complete_dtor_def` at ~3645; the module emission loop that calls `synth_complete_dtor_def`, ~8046–8060)
- Modify: `src/cir_builder.h` (declare `synth_deleting_dtor_def`)
- Test: `tests/test_vdtor_rtti.mad` (+ `.expect`)

Background: `class_vtable_def`'s slot loop resolves each slot name via `findMethod`. The two destructor markers have no `findMethod` entry — they must be special-cased to the synthesized symbols. `class_complete_dtor_symbol(cdd)` (cir_builder.cpp:3600) already returns the right D1 symbol (the plain base dtor when there are no virtual bases, else `Cls___dtor_complete`). The D0 symbol is new: `Cls___dtor_deleting`.

- [ ] **Step 1: Declare the synth in the header.** In `src/cir_builder.h`, next to `node_t synth_complete_dtor_def(DataDefCLASS *cdd);` (line ~561) add:
```cpp
	node_t synth_deleting_dtor_def(DataDefCLASS *cdd);
```

- [ ] **Step 2: Write the failing integration test.** Create `tests/test_vdtor_rtti.mad`:
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

- [ ] **Step 3: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_rtti.mad 2>&1 | grep -v setrlimit | tail -6 )`
Expected (pre-fix): a link/compile failure about an undefined `_ZTI*`/`Base__vtable`, or wrong RTTI — because Task 1 registered slots but no vtable/symbols are emitted yet. (If it instead prints `1`/`1`, STOP — the gap may already be closed; do not invent a change.)

- [ ] **Step 4: Implement the deleting-dtor synth.** In `src/cir_builder.cpp`, immediately after `synth_complete_dtor_def` (ends ~3672), add:
```cpp
// void Cls___dtor_deleting(struct Cls *__this) { <complete-dtor>(__this); free(__this); }
// The Itanium D0 (deleting) destructor: run the complete-object destruction, then
// operator delete (free, for madc user classes). Referenced from the D0 vtable slot.
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
	// complete-object destruction (members + non-virtual bases + vbases, once)
	std::string csym = class_complete_dtor_symbol(cdd);
	referenced_funcs.insert(csym);
	node_t ca = list();
	append(ca, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id(csym.c_str()), ca)));
	// operator delete: free(__this)
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

- [ ] **Step 5: Emit the deleting dtor for polymorphic-with-virtual-dtor classes.** Find the module loop that emits complete dtors (the `emitted_complete_dtors` set at ~8050, where `synth_complete_dtor_def(cdd)` is appended). In the SAME loop body, when the class carries a D0 slot, also emit the deleting dtor (dedupe with its own set). Add near the top of the function a `std::set<DataDefCLASS *> emitted_deleting_dtors;` and inside the loop:
```cpp
		if (cdd->vtable_slot("~$deleting") >= 0
		    && !emitted_deleting_dtors.count(cdd)) {
			emitted_deleting_dtors.insert(cdd);
			node_t dd = synth_deleting_dtor_def(cdd);
			if (dd) append(top_list, dd);   // match the var name used for synth_complete_dtor_def's append target
		}
```
(Read the surrounding ~10 lines first: use the SAME append target the existing `synth_complete_dtor_def` result is appended to — it may be `top_list` or another list variable. Match it exactly.)

- [ ] **Step 6: Resolve the D1/D0 markers in the vtable.** In `class_vtable_def`'s slot loop (the `for (const std::string &slot : G.slots)` at ~2944), BEFORE the `Variable *mv = cdd->findMethod(sname);` line, special-case the destructor markers:
```cpp
		for (const std::string &slot : G.slots) {
			std::string sname = slot;
			// Destructor slots resolve to synthesized symbols of the CURRENT
			// class (most-derived = the override), not findMethod. D1 = complete
			// dtor, D0 = deleting dtor. In a secondary group these get a
			// this-adjusting thunk (Task 4).
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
(Keep the rest of the loop body exactly as-is for non-destructor slots.)

- [ ] **Step 7: Build + verify pass.**
Run: `make -C src 2>&1 | tail -15` (0 warn/err), then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_rtti.mad 2>&1 | grep -v setrlimit )` → `1` then `1`.

- [ ] **Step 8: Regression gate** (failset == baseline +1 new pass = 472; no-std 468).

- [ ] **Step 9: Commit.**
```bash
git add src/cir_builder.cpp src/cir_builder.h tests/test_vdtor_rtti.mad tests/test_vdtor_rtti.expect
git commit -m "feat(cir): emit D1/D0 destructor vtable slots + deleting-dtor synth (vdtor 2/5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Virtual dispatch on `delete`

**Files:**
- Modify: `src/cir_builder.cpp` (`TokenDELETE` lowering, ~5052–5079; mirrors the virtual method-call dispatch at ~3315–3340 and uses `find_vslot` from `include/datadef.h:776`)
- Test: `tests/test_vdtor_delete.mad` (+ `.expect`)

Background: today `delete p` calls the STATIC type's complete dtor by name then `free`. For a virtual destructor it must instead dispatch through the receiver's vtable D0 slot (which itself frees), so deleting through a base pointer runs the derived destructor.

- [ ] **Step 1: Write the failing test.** Create `tests/test_vdtor_delete.mad`:
```cpp
#include <iostream>
using namespace std;
struct Base {
    virtual ~Base() { cout << "~Base" << endl; }
};
struct Derived : Base {
    ~Derived() { cout << "~Derived" << endl; }
};
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
Expected (pre-fix): prints only `~Base` (static dispatch ran Base's dtor) — the derived dtor did not run.

- [ ] **Step 3: Implement.** In `src/cir_builder.cpp`, the `TokenDELETE` block (~5055). Insert a virtual-dtor branch BEFORE the existing `if (cdd && cdd->has_user_dtor)` static path. When the static type has a virtual destructor (`cdd->vtable_slot("~$deleting") >= 0`), dispatch through the D0 slot and DROP the separate `free` (D0 frees). Mirror the method-dispatch shape at 3315–3340 (load the owning group's vptr field, cast `void**`, index the in-group slot, cast to `void(*)(void*)`, call with the pointer). The pointer is re-translated for the call argument, matching the existing "same node must not appear twice" idiom used a few lines down for `free`:
```cpp
	if (TokenDELETE *tdl = dynamic_cast<TokenDELETE *>(tb)) {
		DataDefCLASS *cdd = tdl->del_class;
		// Virtual destructor: dispatch through the vtable D0 (deleting) slot,
		// which runs the most-derived complete destructor AND frees. No separate
		// free() here. (Itanium deleting-dtor semantics.)
		if (cdd && cdd->vtable_slot("~$deleting") >= 0) {
			size_t grp; int slot;
			cdd->find_vslot("~$deleting", grp, slot);
			const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[grp];
			std::string vfld = (G.this_offset == 0)
				? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
			// recv for the vptr load: (struct Cls *) <expr>
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
			// cast slot to void (*)(void *)
			node_t fp_dl = list();
			append(fp_dl, node1(N_FUNC,
				node1(N_LIST, node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer()))))));
			append(fp_dl, pointer());
			node_t fp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					       node2(N_DECL, ignore(), fp_dl));
			node_t fn = node2(N_CAST, fp_type, slotref, tb);
			// arg: the pointer again (re-translated — single node may appear once)
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
(Keep the existing non-virtual body below verbatim. VERIFY the `void(*)(void*)` declarator shape against how `method_fnptr_type` / the method dispatch builds a function-pointer cast at ~3338 — if the `N_FUNC` declarator nesting differs, match that proven shape instead. The simplest robust alternative, if the hand-built `fp_type` is rejected by c2mir, is to reuse the SAME `void **`/indexing then cast via a `typedef`-free `void (*)(void *)` exactly as the codebase already does elsewhere for indirect calls — grep `N_FUNC` in cir_builder.cpp for a working fn-ptr-cast precedent and copy it.)

- [ ] **Step 4: Build + verify pass.**
`make -C src 2>&1 | tail -15`, then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_delete.mad 2>&1 | grep -v setrlimit )` → `~Derived` then `~Base`.

- [ ] **Step 5: No-double-free check.** Run under the harness; also a scratch `tmp/vd_df.mad` that `new`/`delete`s a virtual-dtor object in a loop 100 times and prints "ok" — must print `ok` with no `free(): double free` / `munmap_chunk` abort. (Scratch only; do not commit.)

- [ ] **Step 6: Regression gate** (failset == baseline +2 new passes = 473; no-std 468). This task changes `delete` codegen → the COORDINATOR runs the SMAUG soak.

- [ ] **Step 7: Commit.**
```bash
git add src/cir_builder.cpp tests/test_vdtor_delete.mad tests/test_vdtor_delete.expect
git commit -m "feat(cir): virtual dispatch on delete via the D0 vtable slot (vdtor 3/5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Multiple inheritance — D1/D0 thunks in secondary groups

**Files:**
- Modify: `src/cir_builder.cpp` (`class_vtable_def` destructor-marker branch from Task 2, ~2944; add a destructor this-adjusting thunk parallel to the existing `make_thunk` lambda at ~2879)
- Test: `tests/test_vdtor_mi.mad` (+ `.expect`)

Background: when a polymorphic SECONDARY base has a virtual destructor, its group's D1/D0 slots are reached through a secondary-base `this` pointer, so they need a this-adjusting thunk (subtract the subobject offset, then call the most-derived destructor) — exactly like the method `make_thunk` at 2879, but for the synthesized destructor symbols (which are not `FuncDef`s). Pin against g++.

- [ ] **Step 1: Write the failing test.** Create `tests/test_vdtor_mi.mad`:
```cpp
#include <iostream>
using namespace std;
struct A { virtual void fa() {} virtual ~A() { cout << "~A" << endl; } };
struct B { virtual void fb() {} virtual ~B() { cout << "~B" << endl; } };
struct C : A, B {                    // B is the secondary base
    ~C() { cout << "~C" << endl; }
};
int main() {
    B *b = new C();                  // points at the B subobject
    delete b;                        // must print ~C, ~B, ~A (most-derived first)
    return 0;
}
```
Create `tests/test_vdtor_mi.expect`:
```
~C
~B
~A
```
Oracle: `( ulimit -t 60; g++ -std=c++17 -x c++ tests/test_vdtor_mi.mad -o tmp/vmi ); tmp/vmi` → `~C` `~B` `~A`. (Confirm the exact order from g++ and use whatever g++ prints — that is canon.)

- [ ] **Step 2: Run it, verify it fails.**
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_mi.mad 2>&1 | grep -v setrlimit )`
Expected (pre-fix): wrong output or a crash — the secondary-group D0 slot points at C's deleting dtor WITHOUT `this`-adjustment, so it receives the B-subobject pointer and frees/destructs the wrong address.

- [ ] **Step 3: Implement the destructor thunk.** In `class_vtable_def`, add a lambda beside `make_thunk` (~2879) that builds a this-adjusting thunk for a destructor symbol (fixed `void(struct Sec*)` signature, no `FuncDef`):
```cpp
	// This-adjusting thunk for a destructor slot reached through a secondary-base
	// vptr: void Cls__dthunk_<off>_<tag>(struct Sec *p){ target((char*)p - off); }
	// where target is the most-derived complete (D1) or deleting (D0) dtor symbol.
	auto make_dtor_thunk = [&](const std::string &target_sym, size_t off,
				   const char *tag) -> std::string {
		std::string tname = cdd->name + "__dthunk_" + std::to_string(off) + "_" + tag;
		node_t ret_spec = node1(N_LIST, simple(N_VOID));
		node_t plist = list();
		node_t pspec = simple(N_SPEC_DECL);
		append(pspec, node1(N_LIST, simple(N_CHAR)));    // param typed char* is fine; we cast
		append(pspec, node2(N_DECL, id("__self"), node1(N_LIST, pointer())));
		append(pspec, ignore()); append(pspec, ignore()); append(pspec, ignore());
		append(plist, pspec);
		node_t tdecl = node2(N_DECL, id(tname.c_str()),
				     node1(N_LIST, node1(N_FUNC, plist)));
		// (char*)__self - off
		node_t adj = node2(N_SUB, id("__self"), integer((long)off));
		node_t a = list();
		append(a, adj);
		node_t call = node2(N_CALL, id(target_sym.c_str()), a);
		referenced_funcs.insert(target_sym);
		node_t body = node2(N_BLOCK, list(),
				    node1(N_LIST, node2(N_EXPR, list(), call)));
		thunks.push_back(node4(N_FUNC_DEF, ret_spec, tdecl, list(), body));
		return tname;
	};
```
Then in the destructor-marker branch from Task 2, when `G.this_offset != 0` use a thunk instead of the bare symbol:
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
(The thunk's `target_sym` already adjusts to the COMPLETE object; for D0, `Cls___dtor_deleting` then frees that complete-object pointer — correct. The param is typed `char*` so the `__self - off` arithmetic is byte-wise; the target call passes the adjusted `char*`, implicitly converted to the target's `struct Cls*` — if c2mir warns on that implicit conversion, wrap `adj` in a `(struct Cls*)` cast using `cdd->name`.)

- [ ] **Step 4: Build + verify pass.**
`make -C src 2>&1 | tail -15`, then
`( ulimit -t 60; ./bin/madc tests/test_vdtor_mi.mad 2>&1 | grep -v setrlimit )` → matches the g++ oracle order from Step 1.

- [ ] **Step 5: Regression gate** (failset == baseline +3 = 474; no-std 468). Codegen change → COORDINATOR SMAUG soak.

- [ ] **Step 6: Commit.**
```bash
git add src/cir_builder.cpp tests/test_vdtor_mi.mad tests/test_vdtor_mi.expect
git commit -m "feat(cir): this-adjusting D1/D0 thunks for secondary-base virtual dtors (vdtor 4/5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Hardening — vtable-layout doctest + single-inheritance override matrix

**Files:**
- Modify: `tests/unit/test_class_layout.cpp` (append a case asserting the g++ slot order)
- Test: `tests/test_vdtor_chain.mad` (+ `.expect`)

- [ ] **Step 1: Write the layout doctest.** Append to `tests/unit/test_class_layout.cpp` a case mirroring the g++ ground truth from the spec (`struct A { virtual void f(); virtual ~A(); virtual void g(); }`): the destructor pair sits at its DECLARATION position (after `f`, before `g`):
```cpp
TEST_CASE("virtual dtor slot pair sits at declaration-order position") {
    DataDefCLASS *A = parse_one_class(
        "struct A { virtual void f(){} virtual ~A(){} virtual void g(){} };");
    REQUIRE(A != nullptr);
    int sf = A->vtable_slot("f");
    int sd1 = A->vtable_slot("~");
    int sd0 = A->vtable_slot("~$deleting");
    int sg = A->vtable_slot("g");
    CHECK(sf >= 0); CHECK(sd1 >= 0); CHECK(sd0 >= 0); CHECK(sg >= 0);
    CHECK(sd1 == sf + 1);        // ~A (D1) right after f
    CHECK(sd0 == sd1 + 1);       // D0 right after D1
    CHECK(sg == sd0 + 1);        // g after the dtor pair
}
```
(Reuse the same `parse_one_class` mechanism Task 1 used.)

- [ ] **Step 2: Build, verify it fails (if Task 1's registration ordered the slots differently) or passes.** Run `( ulimit -t 200; timeout 200 make -C src test 2>&1 | tail -15 )`. If it FAILS because the destructor was registered out of declaration order, fix Task 1's registration so the markers are pushed at the point the destructor is parsed (they already are — the push happens in the dtor branch as the loop reaches it, preserving declaration order). Confirm green.

- [ ] **Step 3: Write the override-chain integration test.** Create `tests/test_vdtor_chain.mad` (three-level single-inheritance chain, delete through the top base, plus a non-virtual-dtor sibling to prove the static path is untouched):
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

- [ ] **Step 4: Run it, verify pass** (Tasks 1–3 already implement this).
Run: `( ulimit -t 60; ./bin/madc tests/test_vdtor_chain.mad 2>&1 | grep -v setrlimit )`
Expected: `~C` `~B` `~A` `~Plain`.

- [ ] **Step 5: Full regression gate** (build 0-warn; `make -C src test` all doctest SUCCESS; `run_tests.sh` failset == baseline, pass count 475; no-std 468). COORDINATOR runs the final SMAUG soak + serpent combat (broad codegen touched across the feature).

- [ ] **Step 6: Commit.**
```bash
git add tests/unit/test_class_layout.cpp tests/test_vdtor_chain.mad tests/test_vdtor_chain.expect
git commit -m "test(vdtor): vtable-layout doctest + override-chain + non-virtual-sibling (vdtor 5/5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After all tasks

- Coordinator: final `make -C src fulltest`, failset diff == baseline (expect +5 passing tests → 476), SMAUG boot + serpent combat, then push.
- Update KG: Feature `Virtual Destructors` status → done (with the landing commit range); leave the two deferred Gaps open.
- Update memory `project_cpp_parser_correctness` (Stage C done) and `MEMORY.md`.
- Finishing-a-development-branch: merge `feature/cpp-virtual-dtors-claude` back into `feature/retire-std-hardcoding-claude` (ff), per the parser-correctness track's "merge back when green".

---

## Self-Review

**1. Spec coverage:** Parser slot registration (spec §1) → Task 1. Vtable D1/D0 emission + dtor-only-class table (spec §2) → Task 2. D0 synthesis (spec §3) → Task 2 (`synth_deleting_dtor_def`). Virtual `delete` dispatch (spec §4) → Task 3. MI secondary-base thunks (spec §2 secondary-group note) → Task 4. Testing (spec §Testing): RTTI/dtor-only → Task 2; delete-dispatch order → Task 3; MI → Task 4; vtable-layout doctest + no-double-free → Tasks 3/5; unchanged static path → Task 5. Deferred (pure-virtual, pseudo-dtor) → out of scope, KG-tracked. No gaps.

**2. Placeholder scan:** Every code step has complete node-construction code. Three "VERIFY/confirm" notes (the `parse_one_class` harness name; the `synth_complete_dtor_def` append-target variable; the `void(*)(void*)` declarator shape vs the proven method-dispatch fn-ptr cast) are explicit pre-write checks with the exact grep/precedent to consult — they hedge facts that can drift (helper names, list-variable names, c2mir declarator acceptance), not vague TODOs. No "handle edge cases" / "add validation" left abstract.

**3. Type consistency:** Markers `"~"` (D1) / `"~$deleting"` (D0) are used identically across Tasks 1–5. `synth_deleting_dtor_def` (declared Task 2 Step 1, defined Step 4, called Step 5) and the emitted symbol `cdd->name + "___dtor_deleting"` match between the synth (Task 2) and both the vtable reference (Task 2 Step 6) and the thunk target (Task 4). `class_complete_dtor_symbol` is the D1 symbol everywhere. `find_vslot`/`vtable_slot`/`vtable_groups`/`VtableGroup::this_offset`/`addr_point` are the existing `DataDefCLASS` members. `make_dtor_thunk` pushes to the same `thunks` vector the existing `make_thunk` uses.
