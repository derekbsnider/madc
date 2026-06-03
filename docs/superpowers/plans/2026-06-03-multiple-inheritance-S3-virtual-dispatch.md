# Multiple/Virtual Inheritance — S3: Faithful Virtual Dispatch across MI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace madc's simplified single-vptr scheme with Itanium-faithful **grouped vtables** so virtual dispatch is correct across multiple inheritance — including MI ctor/dtor chaining (to construct every base and set every vptr), secondary-vptr initialization, dispatch that selects the correct vptr+slot, and this-adjusting thunks for overrides reached through a non-primary base.

**Architecture:** A polymorphic class emits ONE grouped `Cls__vtable[]` made of consecutive sub-tables — the primary table (slots inherited from / shared with the primary base, plus the class's own) followed by one secondary sub-table per non-primary polymorphic base. Each base subobject's vptr (`__vptr` at 0, `__vptrN` at the secondary offset) is initialized to point at the START of that base's sub-table (its "address point"). Dispatch loads the vptr of the subobject the method belongs to and indexes by the slot's index *within that sub-table*. When a derived class overrides a method whose slot lives in a secondary sub-table, that slot holds a **thunk** — an emitted C function that subtracts the base offset from `this` and tail-calls the real override. MI ctor/dtor chaining iterates the `bases` vector (offset-adjusted) instead of the single `base_class`.

**Tech Stack:** C++11; `include/datadef.h` (`DataDefCLASS` vtable model); `src/parser.cpp` (slot grouping per sub-table); `src/cir_builder.cpp` (`class_vtable_def` emission, vptr init, dispatch, ctor/dtor chaining, thunk emission); doctest + integration `.mad` tests compared to g++.

**Spec:** `docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md` Part 2 §5 (grouped vtables), §7 (thunks), §8 (ctor/dtor). **Scope regroup (noted):** the spec split dispatch (§5) from thunks (§7, "S4") — but a *demonstrable* MI-dispatch increment needs thunks, so S3 here = §5+§7+the MI-ctor/dtor part of §8. S4 becomes virtual-BASE construction (VTT/construction vtables + runtime `vbase_offset`, spec §6) + the rest of §8; S5 RTTI (§9). Single-inheritance dispatch must stay byte-identical (SMAUG depends on it).

**Current mechanism (recon, all single-vptr — every one of these changes):**
- Flat per-class slots: `DataDefCLASS::vtable_slots` (`vector<string>`), override = slot reuse by name (parser.cpp:11742-11748 inherit, 12000-12006 own). `vtable_slot(name)` linear (datadef.h).
- One runtime `void **vtable`, `calloc(nslots)` (parser.cpp:12068-12073).
- ONE flat C global `void *Cls__vtable[]`, elems `(void*)mangled_method`, `0` for pure — `class_vtable_def` (cir_builder.cpp:2724-2763), emitted in Pass 1.5 (cir_builder.cpp:7504-7512).
- vptr init `obj.__vptr = (void*)Cls__vtable` at offset 0 only — `new` path (cir_builder.cpp:4558-4573) + ctor prologue (cir_builder.cpp:7015-7025).
- Dispatch `((void**)recv->__vptr)[slot]`, always offset-0 `__vptr`, derived flat slot (cir_builder.cpp:3084-3122).
- ctor/dtor chaining traverses single `base_class` (cir_builder.cpp:6985, 7007-7008, 7026-7027; synth_dtor 3402-3438) — does NOT iterate `bases`.
- Secondary `__vptrN` fields are EMITTED (S2, class_struct_def) but never written/read.
- Mangler: NO `_ZTV/_ZTT/_ZTI/_ZTS/_ZTh` (madc emits its OWN `Cls__vtable` naming — fine for user classes; real `_ZTV` only needed for libstdc++ interop = campaign/S5, not here).

---

## File Structure
- **`include/datadef.h`** (`DataDefCLASS`): add the grouped-vtable model — `struct VtableGroup { DataDefCLASS *owner; size_t this_offset; std::vector<std::string> slots; };` and `std::vector<VtableGroup> vtable_groups;` (group 0 = primary @ offset 0; one per secondary polymorphic base). Keep `vtable_slots` as the primary group's slots for the unchanged single-inheritance path during transition, OR make it an accessor onto `vtable_groups[0].slots`. Add helpers: `int slot_in_group(size_t g, const std::string&) const;` and `bool find_vslot(const std::string &m, size_t &group, int &slot) const;`.
- **`src/parser.cpp`**: build `vtable_groups` at the closing `}` (after `compute_layout`) — primary group = inherited primary-chain slots + own virtual methods; one secondary group per `secondary_vptr_owners` entry carrying that base's slots; mark overridden slots.
- **`src/cir_builder.cpp`**: (a) `class_vtable_def` emits the grouped array + records each group's address-point index; (b) vptr init sets `__vptr`→primary address point and each `__vptrN`→its secondary address point (in `new` + ctor prologue); (c) dispatch uses `find_vslot` to pick the vptr field + in-group slot; (d) MI ctor/dtor chaining iterates `bases` with offset-adjusted `this`; (e) emit this-adjusting thunks for secondary-group slots that resolve to a most-derived override.
- **`tests/test_mi_vdispatch.mad`**, **`tests/test_mi_ctor_order.mad`** (create) — `.expect` matched to g++.

---

## Task 1: MI ctor/dtor chaining over `bases` (prerequisite for secondary vptrs)

**Files:** Modify `src/cir_builder.cpp` (ctor/dtor prologue/epilogue 6984-7027; synth_dtor 3402-3438). Test: `tests/test_mi_ctor_order.mad`.

- [ ] **Step 1: Write the failing test**

`tests/test_mi_ctor_order.mad`:
```cpp
#!/../bin/madc
// Every base ctor must run (in declaration order), then the derived ctor; dtors reverse.
class A { public: A() { printf("A+"); } ~A() { printf("A-"); } };
class B { public: B() { printf("B+"); } ~B() { printf("B-"); } };
class C : public A, public B { public: C() { printf("C+"); } ~C() { printf("C-"); } };
int main() { { C c; } printf("\n"); return 0; }
```
`tests/test_mi_ctor_order.expect`:
```
A+B+C+C-B-A-
```
(g++ ref: ctors in base declaration order A,B then C body; dtors reverse C,B,A.)

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_ctor_order.mad )`
Expected: only `A` (the single `base_class`) is constructed/destructed → output like `A+C+C-A-`, missing B.

- [ ] **Step 3: Iterate `bases` in ctor prologue and dtor epilogue/synth**

In `src/cir_builder.cpp` ctor prologue (~7007-7008) replace the single `base->base_call` with a loop over `ocls->bases` (declaration order) calling each base ctor with `this` adjusted by `ocls->base_offset_of(b.base)`; in the dtor epilogue (~7026-7027) and `synth_dtor_def` (~3422-3433) loop over `ocls->bases` in REVERSE with the same offset-adjusted `this`. Use the existing `base_call` helper but pass an adjusted `__this`:
```cpp
// CTOR: construct each base in declaration order (offset-adjusted this), then members, then body.
if (is_ctor) {
    for (size_t bi = 0; bi < ocls->bases.size(); bi++) {
        DataDefCLASS *b = ocls->bases[bi].base;
        if (!b->has_user_ctor) continue;
        size_t off = ocls->base_offset_of(b);
        prologue.push_back(base_ctor_call_at(b, off, tf)); // adjusted (char*)__this+off -> b*
    }
}
```
`base_ctor_call_at(b, off, tf)` builds `B__B( (B*)((char*)__this + off) )` (off 0 → no add; mirror the this-adjust node shape from the S2 method-call site). Symmetric reverse loop for dtors. Replace `synth_dtor_def`'s single `base_class` dtor call with the reverse `bases` loop.

- [ ] **Step 4: Run to verify it passes + regression**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_ctor_order.mad )` → `A+B+C+C-B-A-`.
Run: `( ulimit -t 700; timeout 800 bash scripts/run_tests.sh > tmp/s3t1.log 2>&1 ); tail -1 tmp/s3t1.log` → `461 passed` (460 + this test), same known-6 + flaky. Single-inheritance ctor/dtor unchanged (offset 0).

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp tests/test_mi_ctor_order.mad tests/test_mi_ctor_order.expect
git commit -m "feat(cir): MI ctor/dtor chaining over all bases, offset-adjusted (S3 task 1)"
```

---

## Task 2: Grouped vtable model + per-group slots (parser + datadef)

**Files:** Modify `include/datadef.h`, `src/parser.cpp`. Test: extend `tests/unit/test_class_layout.cpp`.

- [ ] **Step 1: Write the failing unit test**

Append to `tests/unit/test_class_layout.cpp`:
```cpp
TEST_SUITE("class layout — vtable groups") {
    TEST_CASE("MI: primary group + one secondary group") {
        DataDefCLASS *p1 = mkclass("P1", 1, true);  // 1 virtual slot
        p1->vtable_slots.push_back("f1"); p1->virtual_methods["f1"] = true;
        p1->compute_layout(); p1->build_vtable_groups();
        DataDefCLASS *p2 = mkclass("P2", 1, true);
        p2->vtable_slots.push_back("f2"); p2->virtual_methods["f2"] = true;
        p2->compute_layout(); p2->build_vtable_groups();
        DataDefCLASS *mic = mkclass("MIc", 1, true);
        mic->bases.push_back(BaseSpec{p1,0,false,0u,false});
        mic->bases.push_back(BaseSpec{p2,0,false,0u,false});
        mic->compute_layout(); mic->build_vtable_groups();
        CHECK(mic->vtable_groups.size() == 2);          // primary(P1) + secondary(P2)
        CHECK(mic->vtable_groups[0].this_offset == 0);
        CHECK(mic->vtable_groups[1].this_offset == 16); // P2 subobject
        size_t g; int s;
        CHECK(mic->find_vslot("f2", g, s)); CHECK(g == 1); CHECK(s == 0);
        CHECK(mic->find_vslot("f1", g, s)); CHECK(g == 0);
    }
}
```

- [ ] **Step 2: Run to verify it fails (build_vtable_groups/vtable_groups/find_vslot undefined)**

Run: `( ulimit -t 300; timeout 360 make -C src test )`
Expected: COMPILE FAIL.

- [ ] **Step 3: Add the model + builder**

In `include/datadef.h` `DataDefCLASS` add:
```cpp
    struct VtableGroup { DataDefCLASS *owner; size_t this_offset; std::vector<std::string> slots; size_t addr_point; };
    std::vector<VtableGroup> vtable_groups;
    void build_vtable_groups();                    // defined in parser.cpp; run after compute_layout
    bool find_vslot(const std::string &m, size_t &group, int &slot) const {
        for (size_t g = 0; g < vtable_groups.size(); g++)
            for (size_t i = 0; i < vtable_groups[g].slots.size(); i++)
                if (vtable_groups[g].slots[i] == m) { group = g; slot = (int)i; return true; }
        return false;
    }
```
In `src/parser.cpp` add `build_vtable_groups()` (call it right after `apply_member_layout()` in TokenCLASS::parse): group 0 = primary, `this_offset` 0, slots = the existing `vtable_slots` (primary chain + own virtuals, already inheritance-merged by the body loop); then one group per `secondary_vptr_owners` entry, `this_offset` = that base's `bases[].offset`, slots = that base's own `vtable_slots` (copied). A derived override keeps the method's NAME in whichever group(s) declare it — the emitter resolves the name to the most-derived `findMethod`, so no per-group rewrite is needed here.
```cpp
void DataDefCLASS::build_vtable_groups()
{
    vtable_groups.clear();
    if (!has_vtable && !has_vptr_slot) return;
    vtable_groups.push_back(VtableGroup{this, 0, vtable_slots, 0}); // primary (addr_point set in emit)
    for (DataDefCLASS *o : secondary_vptr_owners) {
        size_t off = 0;
        for (auto &b : bases) if (b.base == o) { off = b.offset; break; }
        vtable_groups.push_back(VtableGroup{o, off, o->vtable_slots, 0});
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `( ulimit -t 300; timeout 360 make -C src test )` → `test_class_layout` green (8 suites).

- [ ] **Step 5: Commit**

```bash
git add include/datadef.h src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(class): grouped vtable model (primary + secondary groups) (S3 task 2)"
```

---

## Task 3: Emit the grouped vtable array + record address points

**Files:** Modify `src/cir_builder.cpp` (`class_vtable_def` 2724-2763).

- [ ] **Step 1: Write the failing integration test (dispatch through secondary base, NO override)**

`tests/test_mi_vdispatch.mad`:
```cpp
#!/../bin/madc
class P1 { public: virtual int f1() { return 10; } };
class P2 { public: virtual int f2() { return 20; } };
class MIc : public P1, public P2 { public: int c; };
int main()
{
    MIc m; m.c = 3;
    P1 *a = &m;     // primary subobject @0
    P2 *b = &m;     // secondary subobject @ offset
    printf("%d %d %d %d\n", m.f1(), m.f2(), a->f1(), b->f2());
    return 0;
}
```
`tests/test_mi_vdispatch.expect`:
```
10 20 10 20
```
(No overrides yet — Task 5 adds the override/thunk case. `b->f2()` must dispatch through P2's secondary vtable.)

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vdispatch.mad )`
Expected: wrong result for `b->f2()` / `m.f2()` — only one flat vtable + one vptr; the secondary dispatch reads the wrong slot or vptr.

- [ ] **Step 3: Emit grouped sub-tables back-to-back; record address-point index per group**

In `class_vtable_def`, emit each group's slots consecutively into the single `Cls__vtable[]`, and store each group's starting index. Each slot's function = `findMethod(slotname)` on the MOST-DERIVED class (so overrides resolve), cast to `void*`; pure → `0`. Record the address point (the array index where each group begins) on the `VtableGroup` (add `size_t addr_point;` to the struct). For single-inheritance/one-group classes this emits exactly the old flat table (addr_point 0), byte-identical.
```cpp
size_t idx = 0;
for (size_t g = 0; g < cdd->vtable_groups.size(); g++) {
    cdd->vtable_groups[g].addr_point = idx;
    for (const std::string &slot : cdd->vtable_groups[g].slots) {
        Variable *mv = cdd->findMethod(const_cast<std::string&>(slot));
        node_t e = mv ? node2(N_CAST, voidp_type(), id(mv->name.c_str()))
                      : integer(0);
        append(inits, node2(N_INIT, list(), e));
        referenced_funcs.insert(mv ? mv->name : std::string());
        idx++;
    }
}
```
(Thunks for secondary overrides are substituted in Task 5; here, with no overrides, `findMethod` returns the base method which is correct for the non-override case.)

- [ ] **Step 4: Run — dispatch still needs Task 4 (vptr init + selection). Confirm build clean + vtable array has both groups.**

Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → `0`.
(Functional pass comes after Task 4; do not add to the pass set yet.)

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp include/datadef.h tests/test_mi_vdispatch.mad tests/test_mi_vdispatch.expect
git commit -m "feat(cir): emit grouped vtable sub-tables + address points (S3 task 3)"
```

---

## Task 4: Initialize secondary vptrs + dispatch through the correct vptr/slot

**Files:** Modify `src/cir_builder.cpp` (vptr init 4558-4573 + 7015-7025; dispatch 3084-3122).

- [ ] **Step 1: (test_mi_vdispatch.mad is the test)** It passes only when both (a) secondary vptrs are initialized to their address points and (b) dispatch selects the right vptr+slot.

- [ ] **Step 2: Run to confirm still wrong**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vdispatch.mad )`
Expected: still wrong for the secondary-base call.

- [ ] **Step 3a: Initialize every group's vptr**

In the ctor prologue (7015-7025) and the `new` no-ctor path (4558-4573), after setting the primary `__vptr` to `&Cls__vtable[group0.addr_point]` (== `Cls__vtable`, unchanged), loop over `cdd->vtable_groups` from index 1 and set each secondary subobject's vptr field. The secondary field name matches `class_struct_def`'s naming — make that deterministic: in S2's emitter, name the secondary vptr for the base at offset `K` `"__vptr_<K>"` (change the synthetic name from `__vptrN` to `__vptr_<offset>`), and here set `__this->__vptr_<this_offset> = (void*)&Cls__vtable[addr_point]`:
```cpp
for (size_t g = 1; g < cdd->vtable_groups.size(); g++) {
    size_t off = cdd->vtable_groups[g].this_offset;
    std::string fld = "__vptr_" + std::to_string(off);
    // __this->fld = (void*)(Cls__vtable + addr_point)
    node_t lhs = node2(N_DEREF_FIELD, id("__this", tf), id(fld.c_str(), tf));
    node_t base = id((cdd->name + "__vtable").c_str(), tf);
    node_t ap = node2(N_ADD, base, integer((long)cdd->vtable_groups[g].addr_point), tf);
    prologue.push_back(node2(N_EXPR, list(), node2(N_ASSIGN, lhs,
        node2(N_CAST, voidp_type(), ap, tf), tf), tf));
}
```
(Update `class_struct_def` to emit secondary vptr fields as `__vptr_<offset>` to match.)

- [ ] **Step 3b: Dispatch via find_vslot**

In dispatch (3084-3122), replace the flat `slot = recv_class->vtable_slot(mname)` + always-`__vptr` load with group selection: find the group/slot, load that group's vptr field, index by the in-group slot:
```cpp
size_t grp; int slot;
if (recv_class->find_vslot(mname, grp, slot) && callee) {
    const auto &G = recv_class->vtable_groups[grp];
    std::string vfld = (G.this_offset == 0) ? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
    node_t recv_for_vptr = class_this_arg(tm, dummy, origin); // most-derived this
    node_t vptr = node2(N_DEREF_FIELD, recv_for_vptr, id(vfld.c_str(), origin));
    node_t vtab = node2(N_CAST, vpp_type, vptr, origin);
    node_t slotref = node2(N_IND, vtab, integer(slot, origin), origin);
    node_t fn = node2(N_CAST, method_fnptr_type(callee, G.owner), slotref, origin);
    // this passed to the fn is adjusted to G.owner subobject (the thunk handles the override case)
    return node2(N_CALL, fn, args, origin);
}
```
For single-inheritance (one group, offset 0) this is byte-identical to today (`__vptr`, slot index unchanged).

- [ ] **Step 4: Run to verify it passes + regression + SMAUG**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vdispatch.mad )` → `10 20 10 20`.
Run: `( ulimit -t 700; timeout 800 bash scripts/run_tests.sh > tmp/s3t4.log 2>&1 ); tail -1 tmp/s3t4.log` → `462 passed` (461 + test_mi_vdispatch).
Run the SMAUG soak (port 4000) → boots (single-inheritance virtual dispatch byte-identical).

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp tests/test_mi_vdispatch.mad
git commit -m "feat(cir): secondary vptr init + grouped dispatch (S3 task 4)"
```

---

## Task 5: This-adjusting thunks for secondary-base overrides

**Files:** Modify `src/cir_builder.cpp` (`class_vtable_def` slot fill).

- [ ] **Step 1: Write the failing test (override reached via secondary base)**

Add to `tests/test_mi_vdispatch.mad` a derived override + a virtual call through the secondary base that must land in the override with correct `this`:
```cpp
class Q2 : public P1, public P2 {
public: long q;
    int f2() { return (int)(q + 5); }   // override of P2::f2, defined in most-derived
};
// in main(), append:
    Q2 qq; qq.q = 100;
    P2 *pb = &qq;       // secondary subobject pointer
    printf("%d\n", pb->f2());   // must call Q2::f2 with this = &qq (not &qq+secondary_off)
```
Update `tests/test_mi_vdispatch.expect` to add the line `105`.

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vdispatch.mad )`
Expected: wrong value — the secondary vtable slot points at `Q2::f2` directly, so it is called with `this = P2-subobject` (= &qq + offset), and `q` is read at the wrong place (garbage), not 105.

- [ ] **Step 3: Emit a this-adjusting thunk for secondary-group slots that resolve to a most-derived override**

In `class_vtable_def`, when filling a slot in a NON-primary group (`g > 0`, `this_offset != 0`) whose resolved method (`findMethod`) is defined in the MOST-DERIVED class (i.e. an override, not the base's own method), emit a thunk function and put the thunk's symbol in the slot instead of the method. The thunk: `static RET Cls__thunk_<off>_<method>(void *self, args...) { return Cls__method((char*)self - off, args...); }`. Emit it as a CIR function def (mirror `synth_dtor_def`'s function-emission shape) into the same module, once per (class, group, slot) needing it; record its emit name and use it as the slot's `(void*)` value. Detection of "is an override": `findMethod(slot)`'s owning class (walk where the method's mangled name's class prefix is) == `cdd` (most-derived), AND the group's `owner` != `cdd`.

(Concrete thunk-emission node shape + the override-detection helper are built against the file during execution — they reuse `method_fnptr_type`, the N_FUNC_DEF emission used by `synth_dtor_def` at cir_builder.cpp:3402-3438, and the `(char*)x - off` node shape mirrors S2's `(char*)this + off`. Flagged here, not a logic gap.)

- [ ] **Step 4: Run to verify it passes + regression + SMAUG**

Run: `( ulimit -t 60; ./bin/madc tests/test_mi_vdispatch.mad )` → `10 20 10 20` then `105`.
Run: `run_tests.sh` → `462 passed` (no new test file; test_mi_vdispatch extended). SMAUG soak → boots.

- [ ] **Step 5: Commit**

```bash
git add src/cir_builder.cpp tests/test_mi_vdispatch.mad tests/test_mi_vdispatch.expect
git commit -m "feat(cir): this-adjusting thunks for secondary-base overrides (S3 task 5)"
```

---

## Task 6: Full S3 gate

- [ ] **Step 1: Build clean (0 warnings).** `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → `0`.
- [ ] **Step 2: Unit tests green** (`test_class_layout` incl. the vtable-groups suite).
- [ ] **Step 3: Integration** `462 passed` (460 baseline + test_mi_ctor_order + test_mi_vdispatch), same known-6 + flaky; poly single-inheritance dispatch byte-identical.
- [ ] **Step 4: SMAUG soaks** (port 4000) → ready, no fatals.
- [ ] **Step 5: Gate unchanged** (`check-no-std-hardcoding.sh` → 468, S3 touches no std:: hardcoding) + `git push`.

---

## Self-review notes (author)
- **Spec coverage:** S3 = §5 (grouped vtables) + §7 (thunks) + the MI ctor/dtor part of §8. Regrouped from the spec's S3/S4 split because correct MI dispatch is undemonstrable without thunks (noted at top). Virtual-BASE construction (VTT/construction vtables, §6) + runtime `vbase_offset` move to S4; RTTI (§9) is S5.
- **Regression discipline:** single-inheritance is one group @ offset 0 → emission, vptr-init, and dispatch are byte-identical (Tasks 3/4 guarded). Every behavior-changing task re-runs the 460+ baseline; SMAUG soaks at Tasks 4/5/6.
- **Type/name consistency:** `VtableGroup{owner,this_offset,slots,addr_point}`, `vtable_groups`, `build_vtable_groups()`, `find_vslot(m,group,slot)`, secondary vptr field name `"__vptr_<offset>"` (used identically in class_struct_def emission, vptr-init, and dispatch — Task 4 step 3 changes S2's `__vptrN` to `__vptr_<offset>`).
- **Flagged execution-time items (not logic gaps):** the thunk-function CIR emission node shape (Task 5) and `base_ctor_call_at` node shape (Task 1) reuse existing patterns (`synth_dtor_def` function emission, the S2 this-adjust cast); confirm exact node builders against the file when implementing. `voidp_type()`/`vpp_type`/`method_fnptr_type` already exist in cir_builder (used by the current dispatch/emission).
- **No consumer caveat (logged):** no current test or SMAUG exercises polymorphic MI dispatch; S3 adds the tests that exercise it (test_mi_ctor_order, test_mi_vdispatch). The std:: campaign does NOT need S3 (libstdc++ owns std:: vtables; the campaign calls them via mangled symbols + S2 this-adjust). S3 is "proper C++ for user classes" investment, per the approved full-Tier-B scope.
