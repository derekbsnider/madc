# MI Stage S5 — RTTI / dynamic_cast / typeid — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give madc faithful Itanium-ABI run-time type information — emitted `type_info` objects, a real vtable RTTI prologue, `dynamic_cast<T*>`, and `typeid` — for user-defined polymorphic classes, lowered to portable C11 and interoperating with libstdc++'s `__dynamic_cast`.

**Architecture:** Each polymorphic user class emits an ABI-faithful `type_info` object (`_ZTI<cls>`) whose vptr points at the real libsupc++ type_info-class vtable (so libstdc++ runtime accepts it), plus a name string (`_ZTS<cls>`). The madc grouped vtable (built in S3) gains the standard two-word prologue per address point — `[ap-2]=offset_to_top`, `[ap-1]=&_ZTI<cls>` — so `__dynamic_cast` and polymorphic `typeid` can read the runtime type from any subobject pointer. `dynamic_cast<T*>(e)` lowers to `__dynamic_cast(e,&_ZTI<src>,&_ZTI<dst>,hint)`; `typeid` reads `vptr[-1]` (polymorphic) or references `&_ZTI<T>` (static). std:: classes reference libstdc++'s own `_ZTI*` via the mangler and emit nothing.

**Tech Stack:** C++11 (madc source); cir_node / MC11-IR builder (`src/cir_builder.cpp`); Itanium mangler (`src/madc_mangle.cpp`); doctest (`tests/unit/`); `.mad` integration tests matched to g++; c2mir → MIR backend.

---

## Ground truth (probed on this toolchain — do not re-derive, verify if changing)

Probes archived this session (`tmp/rtti_probe.cpp`, `tmp/tio.cpp`); re-probe with
`g++ -std=c++17 -S -O0 -fno-exceptions` + an out-of-line key function to force emission.

**type_info object layouts** (all entries pointer-width; emit as a `void*[]`, byte-identical to g++):

```
__class_type_info   (NO bases)                size 16:
  [0] = (void*)(_ZTVN10__cxxabiv117__class_type_infoE + 16)   // vptr (address point)
  [1] = (void*)_ZTS<cls>                                       // name string

__si_class_type_info  (exactly ONE public non-virtual base)  size 24:
  [0] = (void*)(_ZTVN10__cxxabiv120__si_class_type_infoE + 16)
  [1] = (void*)_ZTS<cls>
  [2] = (void*)&_ZTI<base>

__vmi_class_type_info (MI, OR any virtual base, OR a non-public/repeated base)  size 16+8+16*N:
  [0] = (void*)(_ZTVN10__cxxabiv121__vmi_class_type_infoE + 16)
  [1] = (void*)_ZTS<cls>
  [2] = (void*)( (unsigned long)flags | ((unsigned long)base_count << 32) )  // .long flags; .long count;
  then base_count base entries, each TWO words:
    (void*)&_ZTI<base>
    (void*)(long)( (offset << 8) | (is_public?0x2:0) | (is_virtual?0x1:0) )
```

`__vmi` flags (`__flags_masks`): `0x1 __non_diamond_repeat_mask`, `0x2 __diamond_shaped_mask`.
For S5 compute conservatively: flags=0 unless a virtual base is reachable by >1 path (diamond) → set `0x2`; a non-virtual base type repeated → `0x1`. When unsure, **0 is always safe** (the runtime falls back to a full search). The `offset` for a virtual base entry is the *vbase_offset* (madc's static `vbase_offset[base]`); g++ encodes it as the offset but the runtime re-reads it via the vptr at run time — for our statically-laid-out objects the static offset is correct.

**`_ZTS<cls>` contents** = the bare mangled type name (NO `_Z`): e.g. class `C` → `"1C"`, `Dia` → `"3Dia"`. This is exactly `source_name(cls)` from the mangler. Verify: `echo _ZTI1C | c++filt` → `typeinfo for C`; `echo _ZTS1C | c++filt` → `typeinfo name for C`.

**`__dynamic_cast`** — SysV order `rdi,rsi,rdx,rcx`:
`void *__dynamic_cast(const void *src, const __class_type_info *src_ti, const __class_type_info *dst_ti, ptrdiff_t src2dst_hint)`.
g++ for `dynamic_cast<C*>(A*)` (A public non-virtual base of C at offset 0) emitted `src2dst_hint = 0`.
Hint rule: offset of the **src** subobject within **dst** if src is a *unique public non-virtual* base of dst; else `-1` (always-correct general fallback). `-2` = not a public base; `-3` = multiple public non-virtual bases. **Use the precise offset when src is a unique public non-virtual base of dst (via `base_offset_of`), else `-1`.** A null source pointer → `__dynamic_cast` returns null.

**Vtable today (after S3):** flat `void *Cls__vtable[]` of function-pointer slots only, NO prologue; `VtableGroup::addr_point` = the flat index of a group's first function slot; dispatch `((void**)(tab+addr_point))[slot]`; vptr init sets each subobject vptr to `tab+addr_point`. Anchors: `class_vtable_def` (cir_builder.cpp:2738), `build_vtable_groups` (parser.cpp, after findMethod), dispatch (cir_builder.cpp:~3160-3185), vptr-init in `new` (~4804-4824) and ctor prologue (~7261-7300).

**Mangler:** `src/madc_mangle.cpp` / `include/madc_mangle.h`; `source_name(name)` = `<len><name>`; existing `itanium_mangle_*` family. User classes are un-namespaced → simple `source_name`.

**Intrinsic parse pattern:** identifier-named intrinsics are handled by name in the expression parser — `sizeof` at `src/parser.cpp:9297`, `__builtin_types_compatible_p` at `:9322`, `va_arg` at `:9354`. `dynamic_cast`/`typeid` lex as plain identifiers (the C++-keyword block in tokens.h:1010-1030 is a **comment**), so mirror this name-comparison approach — no new keyword token needed.

**Scope guard:** changes are gated/no-op for non-polymorphic classes (no vtable → no type_info, no prologue). SMAUG is C89 (no virtual functions) → unaffected; verify it stays green anyway.

---

## File structure

- `src/madc_mangle.cpp` / `include/madc_mangle.h` — add `itanium_typeinfo_sym`, `itanium_typeinfo_name_sym`, `itanium_typeinfo_name_string`. (S5b)
- `tests/unit/test_mangle_rtti.cpp` — new doctest binary asserting the three helpers vs the known c++filt strings. (S5b)
- `include/datadef.h` — `DataDefCLASS`: add `typeinfo_flavor()` enum helper + `is_unique_public_nonvirtual_base()` predicate used by both type_info emission and the dynamic_cast hint. (S5b/S5c)
- `src/parser.cpp` — `build_vtable_groups`: account for the 2-word prologue in `addr_point`. (S5a) Expression parser (~9297): parse `dynamic_cast` (S5c) and `typeid` (S5d).
- `src/cir_builder.cpp` — `class_vtable_def`: emit the prologue (S5a) + RTTI slot wired to `_ZTI<cls>` (S5b); new `class_typeinfo_def` emitter + Pass that emits one per polymorphic class (S5b); extern decls for libsupc++ vtable symbols + `__dynamic_cast`; lowering for `TokenDynamicCast` (S5c) and `TokenTypeid` (S5d).
- `include/tokens.h` — `TokenDynamicCast`, `TokenTypeid` token classes. (S5c/S5d)
- `include/madc/typeinfo` — minimal header binding `std::type_info` (`name()` → `_ZNKSt9type_info4nameEv`). (S5d)
- `tests/test_rtti_*.mad` + `.expect` — integration tests, each matched to g++. (S5a-S5d)

---

## Verification harness (used by every "run" step; cap heavy runs, one at a time)

```
( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )            # expect 0
( ulimit -t 700; timeout 800 bash scripts/run_tests.sh > tmp/g.log 2>&1 ); tail -1 tmp/g.log
bash scripts/check-no-std-hardcoding.sh | head -1                                        # must NOT increase (468)
# SMAUG soak (124 = survived):
cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc /workspace/MadSMAUG/src/SMAUG.mad 4055 > /workspace/madc/tmp/s.log 2>&1; echo $?; pkill -9 -f 'bin/madc'; grep -c "ready at" /workspace/madc/tmp/s.log
```
Diff the integration FAIL set against `docs/parity/torture-failset-current.txt`-style baseline: the known-6 fails + flaky `testfortypedcomma` only. Commit messages: no embedded `"`; end with the Co-Authored-By trailer.

---

# Stage S5a — Vtable RTTI prologue

**Why first:** `dynamic_cast`/polymorphic-`typeid` read `vptr[-1]` (type_info) and `vptr[-2]` (offset_to_top). Adding the prologue is a pure layout change with no observable behavior change yet (RTTI slot is a placeholder null until S5b), so it can be landed and regression-gated on its own.

### Task S5a.1: build_vtable_groups reserves the 2-word prologue

**Files:**
- Modify: `src/parser.cpp` (`DataDefCLASS::build_vtable_groups`)
- Test: `tests/unit/test_class_layout.cpp`

- [ ] **Step 1: Read the current function.** Run `grep -n "build_vtable_groups" src/parser.cpp` and read its body. Identify where `addr_point` is assigned per group (it is the running count of function slots emitted so far).

- [ ] **Step 2: Write the failing unit test.** Append to `tests/unit/test_class_layout.cpp` (use the existing `mkclass(name,ndata,poly)` helper and the existing pattern for building bases). For a single polymorphic class `S` with one virtual method, the primary group's `addr_point` must now be `2` (past `[offset_to_top, &type_info]`), not `0`:

```cpp
TEST_CASE("S5a: vtable group address points include the 2-word RTTI prologue") {
    DataDefCLASS *S = mkclass("S5a_S", 0, /*poly=*/true);   // 1 virtual method -> 1 slot
    S->compute_layout();
    S->build_vtable_groups();
    REQUIRE(S->vtable_groups.size() == 1);
    CHECK(S->vtable_groups[0].addr_point == 2);   // prologue = offset_to_top, &_ZTI
}
```

- [ ] **Step 3: Run it, verify it fails.**
Run: `( ulimit -t 200; timeout 300 make -C src test 2>&1 | tail -20 )`
Expected: this CHECK fails (addr_point == 0 today).

- [ ] **Step 4: Implement.** In `build_vtable_groups`, define `const size_t PROLOGUE = 2;` at the top. When laying out the flat table, start the running flat index at `PROLOGUE` for the first group and add `PROLOGUE` before each subsequent group's slots. Concretely, where the code currently does `running = 0;` then per group `G.addr_point = running; running += G.slots.size();`, change to:

```cpp
const size_t PROLOGUE = 2;            // [offset_to_top, &_ZTI<cls>] precedes each address point
size_t running = 0;
for (size_t g = 0; g < vtable_groups.size(); g++) {
    running += PROLOGUE;              // this group's prologue
    vtable_groups[g].addr_point = running;
    running += vtable_groups[g].slots.size();
}
```
(Keep `G.slots` exactly as built — only `addr_point` and the implicit flat size change.)

- [ ] **Step 5: Run unit test, verify pass.**
Run: `( ulimit -t 200; timeout 300 make -C src test 2>&1 | tail -5 )`
Expected: all assertions pass, including the new one.

- [ ] **Step 6: Commit.**
```bash
git add src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(class): reserve 2-word RTTI prologue in vtable group address points (S5a.1)"
```

### Task S5a.2: class_vtable_def emits the prologue entries

**Files:**
- Modify: `src/cir_builder.cpp` (`class_vtable_def`, ~2738-2836)
- Test: `tests/test_rtti_vdispatch.mad` (+ `.expect`)

- [ ] **Step 1: Write a failing integration test** that confirms virtual dispatch STILL works after the layout shift (the prologue must not break existing dispatch). Create `tests/test_rtti_vdispatch.mad`:

```cpp
#include <iostream>
using namespace std;
class Animal { public: virtual int speak() { return 1; } };
class Dog : public Animal { public: int speak() { return 2; } };
int main() {
    Dog d;
    Animal *a = &d;
    cout << a->speak() << endl;   // 2 (virtual dispatch through base ptr)
    return 0;
}
```
Create `tests/test_rtti_vdispatch.expect`:
```
2
```

- [ ] **Step 2: Run it, verify it FAILS** (dispatch indexes the wrong slot because S5a.1 moved `addr_point` to 2 but the table still has no prologue words, so `tab[2]` is past the single function slot).
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_vdispatch.mad 2>&1 | grep -v setrlimit | tail -3 )`
Expected: wrong/garbage output or crash — NOT `2`.

- [ ] **Step 3: Implement the prologue emission.** In `class_vtable_def`, the `inits` list is built by iterating `vtable_groups` and appending one `N_INIT` per slot. Before each group's slot loop, append the two prologue entries. Add this just inside `for (size_t g ...)`, before `for (const std::string &slot : G.slots)`:

```cpp
        // Itanium prologue for this address point: [offset_to_top, &type_info].
        // offset_to_top = -(this_offset): how far back to the most-derived object.
        node_t vtype = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
                             node2(N_DECL, ignore(), node1(N_LIST, pointer())));
        node_t otop = node2(N_CAST, vtype, integer(-(long)G.this_offset));
        append(inits, node2(N_INIT, list(), otop));
        // RTTI slot: &_ZTI<cls> — placeholder NULL until S5b emits the object.
        node_t vtype2 = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
                              node2(N_DECL, ignore(), node1(N_LIST, pointer())));
        node_t rtti = node2(N_CAST, vtype2, integer(0));
        append(inits, node2(N_INIT, list(), rtti));
```
(Two fresh `N_TYPE` nodes — do not share a node between two inits.)

- [ ] **Step 4: Run the integration test, verify PASS** (dispatch realigned: `addr_point=2` now points exactly past the 2 prologue words at the first function slot).
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_vdispatch.mad 2>&1 | grep -v setrlimit | tail -3 )`
Expected: `2`

- [ ] **Step 5: Full regression gate.** Build (0 warnings), run `scripts/run_tests.sh` (only the known-6 + flaky fail; the 6 MI tests + this new test pass), run the SMAUG soak (boots), check the no-std gate did not increase.

- [ ] **Step 6: Commit.**
```bash
git add src/cir_builder.cpp tests/test_rtti_vdispatch.mad tests/test_rtti_vdispatch.expect
git commit -m "feat(cir): emit Itanium vtable prologue (offset_to_top + RTTI slot) per address point (S5a.2)"
```

---

# Stage S5b — type_info object emission + mangler

### Task S5b.1: mangler RTTI symbol helpers

**Files:**
- Modify: `include/madc_mangle.h`, `src/madc_mangle.cpp`
- Test: `tests/unit/test_mangle_rtti.cpp` (new)

- [ ] **Step 1: Write the failing doctest.** Create `tests/unit/test_mangle_rtti.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
bool madc_verbose = false;
#define DBG(x) do { if (madc_verbose) { x; } } while (0)
#include "doctest.h"
#include "madc_mangle.h"

TEST_CASE("RTTI symbol mangling matches c++filt") {
    CHECK(itanium_typeinfo_sym("C")        == "_ZTI1C");
    CHECK(itanium_typeinfo_name_sym("C")   == "_ZTS1C");
    CHECK(itanium_typeinfo_name_string("C")== "1C");
    CHECK(itanium_typeinfo_sym("Dia")      == "_ZTI3Dia");
    CHECK(itanium_typeinfo_name_string("Dia") == "3Dia");
}
```

- [ ] **Step 2: Add a build rule + run it to verify it fails to compile** (helpers undeclared).
In `src/Makefile`, mirror the existing `test_class_layout` / `test_mangle` unit-binary rules to add a `test_mangle_rtti` target linking `madc_mangle.o` (find the existing `test_mangle` rule with `grep -n test_mangle src/Makefile` and copy it, swapping the name). Then:
Run: `( ulimit -t 200; timeout 300 make -C src test_mangle_rtti 2>&1 | tail -10 )`
Expected: compile error — `itanium_typeinfo_sym` not declared.

- [ ] **Step 3: Declare + implement.** In `include/madc_mangle.h` (near the other declarations):

```cpp
// RTTI symbols for a user class (un-namespaced). source_name = <len><name>.
std::string itanium_typeinfo_sym(const std::string &class_name);        // _ZTI<name>
std::string itanium_typeinfo_name_sym(const std::string &class_name);   // _ZTS<name>
std::string itanium_typeinfo_name_string(const std::string &class_name);// <name> mangled (no _Z)
```
In `src/madc_mangle.cpp` (after `itanium_mangle_nested`, before the `_sub` block):

```cpp
std::string itanium_typeinfo_sym(const std::string &class_name)
{
    return "_ZTI" + source_name(class_name);
}
std::string itanium_typeinfo_name_sym(const std::string &class_name)
{
    return "_ZTS" + source_name(class_name);
}
std::string itanium_typeinfo_name_string(const std::string &class_name)
{
    return source_name(class_name);
}
```

- [ ] **Step 4: Run the doctest, verify pass.**
Run: `( ulimit -t 200; timeout 300 make -C src test_mangle_rtti 2>&1 | tail -5 ); ( ulimit -t 30; ./bin/test_mangle_rtti | tail -3 )`
Expected: all CHECKs pass. Cross-check by hand: `echo _ZTI3Dia | c++filt` → `typeinfo for Dia`.

- [ ] **Step 5: Commit.**
```bash
git add include/madc_mangle.h src/madc_mangle.cpp tests/unit/test_mangle_rtti.cpp src/Makefile
git commit -m "feat(mangle): _ZTI/_ZTS type_info symbol helpers + doctest vs c++filt (S5b.1)"
```

### Task S5b.2: type_info flavor + base predicate on DataDefCLASS

**Files:**
- Modify: `include/datadef.h` (`DataDefCLASS`), `src/parser.cpp` (definitions)
- Test: `tests/unit/test_class_layout.cpp`

- [ ] **Step 1: Write the failing unit test.** Append to `tests/unit/test_class_layout.cpp`. Build `A` (1 virtual, no base), `C : public A`, and `D : public A, public B` using the existing base-wiring pattern (set `bases` with `BaseSpec{base, offset, is_virtual=false, access=0, is_primary}`), then:

```cpp
TEST_CASE("S5b: type_info flavor selection") {
    DataDefCLASS *A = mkclass("S5b_A", 0, true);
    A->compute_layout();
    CHECK(A->typeinfo_flavor() == DataDefCLASS::TI_CLASS);     // no bases

    DataDefCLASS *B = mkclass("S5b_B", 0, true);
    B->compute_layout();
    DataDefCLASS *C = mkclass("S5b_C", 0, true);
    C->bases.push_back({A, 0, false, 0u, true});
    C->compute_layout();
    CHECK(C->typeinfo_flavor() == DataDefCLASS::TI_SI);        // single public non-virtual base

    DataDefCLASS *D = mkclass("S5b_D", 0, true);
    D->bases.push_back({A, 0, false, 0u, true});
    D->bases.push_back({B, 16, false, 0u, false});
    D->compute_layout();
    CHECK(D->typeinfo_flavor() == DataDefCLASS::TI_VMI);       // multiple bases
}
```

- [ ] **Step 2: Run it, verify it fails** (no `typeinfo_flavor`).
Run: `( ulimit -t 200; timeout 300 make -C src test 2>&1 | tail -10 )`
Expected: compile error.

- [ ] **Step 3: Declare + implement.** In `include/datadef.h` inside `DataDefCLASS`, add:

```cpp
    enum TypeInfoFlavor { TI_CLASS, TI_SI, TI_VMI };
    TypeInfoFlavor typeinfo_flavor() const;
    // True iff `b` is reachable from this class as a UNIQUE public non-virtual
    // base; if so writes its subobject offset to *off. Used for the
    // __si_class_type_info choice and the dynamic_cast src2dst hint.
    bool is_unique_public_nonvirtual_base(DataDefCLASS *b, size_t *off) const;
```
In `src/parser.cpp` (near `base_offset_of`):

```cpp
DataDefCLASS::TypeInfoFlavor DataDefCLASS::typeinfo_flavor() const
{
    if (bases.empty())
        return TI_CLASS;
    // __si only when there is exactly one base, public, non-virtual.
    if (bases.size() == 1 && !bases[0].is_virtual && bases[0].access == 0)
        return TI_SI;
    return TI_VMI;
}

bool DataDefCLASS::is_unique_public_nonvirtual_base(DataDefCLASS *b, size_t *off) const
{
    size_t found_off = 0; int count = 0;
    for (const BaseSpec &bs : bases) {
        if (bs.is_virtual || bs.access != 0) continue;
        if (bs.base == b) { found_off = bs.offset; count++; }
        else if (DataDefCLASS *bc = dynamic_cast<DataDefCLASS *>((DataDef *)bs.base)) {
            size_t sub = 0;
            if (bc->is_unique_public_nonvirtual_base(b, &sub)) { found_off = bs.offset + sub; count++; }
        }
    }
    if (count == 1) { if (off) *off = found_off; return true; }
    return false;
}
```
(`access == 0` is public per the `vf*` convention noted in the spec §1. `BaseSpec::base` is already `DataDefCLASS*`; the cast is defensive.)

- [ ] **Step 4: Run unit test, verify pass.**
Run: `( ulimit -t 200; timeout 300 make -C src test 2>&1 | tail -5 )`
Expected: all assertions pass.

- [ ] **Step 5: Commit.**
```bash
git add include/datadef.h src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(class): type_info flavor + unique-public-base predicate (S5b.2)"
```

### Task S5b.3: emit type_info objects + wire the vtable RTTI slot

**Files:**
- Modify: `src/cir_builder.cpp` (new `class_typeinfo_def`; wire into `class_vtable_def` + the vtable-emitting pass; extern decls)
- Modify: `src/cir_builder.h` (declare `class_typeinfo_def`)
- Test: `tests/test_rtti_typeinfo.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing integration test.** This proves the type_info object exists, is libstdc++-compatible, and is reachable via `vptr[-1]`. Create `tests/test_rtti_typeinfo.mad`:

```cpp
#include <iostream>
#include <typeinfo>
using namespace std;
class Shape { public: virtual int kind() { return 0; } };
class Circle : public Shape { public: int kind() { return 1; } };
int main() {
    Circle c;
    Shape *s = &c;
    // typeid via the real type_info read from the vtable RTTI slot:
    cout << (typeid(*s) == typeid(Circle)) << endl;  // 1
    cout << (typeid(*s) == typeid(Shape)) << endl;   // 0
    return 0;
}
```
Create `tests/test_rtti_typeinfo.expect`:
```
1
0
```
**Note:** this test also exercises `typeid` (S5d). If running S5b strictly before S5d, instead use a temporary probe that only checks emission (e.g. compile with `--emit=c11` and grep for `_ZTI5Shape`), and promote this `.mad` to the suite at S5d. The plan keeps it here because S5b+S5d land together in practice; if your executor enforces per-task green, gate this `.mad` at S5d.4 and use the `--emit=c11` grep as the S5b.3 test.

- [ ] **Step 2: Run it, verify it fails** (no type_info emitted; `typeid` unparsed).
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_typeinfo.mad 2>&1 | grep -v setrlimit | tail -3 )`
Expected: parse error on `typeid` or missing `_ZTI` symbol.

- [ ] **Step 3: Declare `class_typeinfo_def` in `src/cir_builder.h`** next to `class_vtable_def`:

```cpp
	node_t class_typeinfo_def(DataDefCLASS *cdd);   // emits _ZTI<cls> + _ZTS<cls>; NULL if non-polymorphic
```

- [ ] **Step 4: Implement `class_typeinfo_def` in `src/cir_builder.cpp`** (place it right before `class_vtable_def`). It returns a single `N_LIST` of definitions: the `_ZTS` char array, the extern decl for the needed libsupc++ vtable symbol, and the `_ZTI` `void*[]` object.

```cpp
// Emit the Itanium type_info object(s) for a polymorphic user class:
//   _ZTS<cls> : the bare mangled name string
//   _ZTI<cls> : a void*[] whose [0] points at the real libsupc++ type_info-class
//               vtable (+16 address point) so libstdc++'s __dynamic_cast accepts it.
// std:: classes are NOT emitted here (they reference libstdc++'s own _ZTI*).
node_t CirBuilder::class_typeinfo_def(DataDefCLASS *cdd)
{
	if (!cdd || !cdd->has_vtable) return NULL;

	std::string ti  = itanium_typeinfo_sym(cdd->name);          // _ZTI<cls>
	std::string ts  = itanium_typeinfo_name_sym(cdd->name);     // _ZTS<cls>
	std::string nm  = itanium_typeinfo_name_string(cdd->name);  // "<len><name>"

	auto vptr_t = [&]() {
		return node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
			     node2(N_DECL, ignore(), node1(N_LIST, pointer())));
	};
	auto void_ptr_to = [&](node_t expr) {            // (void*)expr
		return node2(N_CAST, vptr_t(), expr);
	};
	auto void_ptr_int = [&](long v) {                // (void*)(long)v
		return node2(N_CAST, vptr_t(), integer(v));
	};

	node_t out = list();

	// --- _ZTS<cls>: static const char _ZTS...[] = "<len><name>"; ---
	{
		node_t spec = list();
		append(spec, simple(N_CHAR));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore()));   // [] sized by init
		node_t decl = node2(N_DECL, id(ts.c_str()), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, node2(N_INIT, list(), str(nm.c_str())));     // string literal initializer
		append(out, sd);
	}

	// --- which libsupc++ type_info-class vtable symbol + address point (+16) ---
	const char *abi_vt;
	switch (cdd->typeinfo_flavor()) {
	case DataDefCLASS::TI_CLASS: abi_vt = "_ZTVN10__cxxabiv117__class_type_infoE"; break;
	case DataDefCLASS::TI_SI:    abi_vt = "_ZTVN10__cxxabiv120__si_class_type_infoE"; break;
	default:                     abi_vt = "_ZTVN10__cxxabiv121__vmi_class_type_infoE"; break;
	}
	// extern char <abi_vt>[];   (so (abi_vt + 16) is address+16 bytes = the address point)
	{
		node_t spec = list();
		append(spec, simple(N_EXTERN));
		append(spec, simple(N_CHAR));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore()));
		node_t decl = node2(N_DECL, id(abi_vt), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, ignore());                                   // no initializer (extern)
		append(out, sd);
	}
	referenced_funcs.insert(abi_vt);   // ensure MIR keeps the import live

	// --- _ZTI<cls>: void *_ZTI...[] = { ... }; ---
	node_t inits = list();
	// [0] = (void*)(abi_vt + 16)
	append(inits, node2(N_INIT, list(),
		void_ptr_to(node2(N_ADD, id(abi_vt), integer(16)))));
	// [1] = (void*)_ZTS<cls>
	append(inits, node2(N_INIT, list(), void_ptr_to(id(ts.c_str()))));

	auto emit_base_ti_extern_and_ref = [&](DataDefCLASS *b) -> node_t {
		std::string bti = itanium_typeinfo_sym(b->name);
		// extern char _ZTI<base>[];  -- declared once; harmless if repeated, c2mir
		// tolerates duplicate extern decls. Reference as &_ZTI<base>.
		node_t spec = list();
		append(spec, simple(N_EXTERN));
		append(spec, simple(N_CHAR));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore()));
		node_t decl = node2(N_DECL, id(bti.c_str()), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, ignore());
		append(out, sd);
		referenced_funcs.insert(bti);
		return void_ptr_to(id(bti.c_str()));
	};

	if (cdd->typeinfo_flavor() == DataDefCLASS::TI_SI) {
		// [2] = (void*)&_ZTI<base>
		append(inits, node2(N_INIT, list(),
			emit_base_ti_extern_and_ref(cdd->bases[0].base)));
	} else if (cdd->typeinfo_flavor() == DataDefCLASS::TI_VMI) {
		// flags + count packed: (void*)( flags | (count<<32) )
		unsigned long flags = 0;     // conservative; 0 is always runtime-safe
		unsigned long count = cdd->bases.size();
		append(inits, node2(N_INIT, list(),
			void_ptr_int((long)(flags | (count << 32)))));
		for (const DataDefCLASS::BaseSpec &bs : cdd->bases) {
			append(inits, node2(N_INIT, list(),
				emit_base_ti_extern_and_ref(bs.base)));
			long offflags = ((long)bs.offset << 8)
				| (bs.access == 0 ? 0x2L : 0L)
				| (bs.is_virtual ? 0x1L : 0L);
			append(inits, node2(N_INIT, list(), void_ptr_int(offflags)));
		}
	}

	{
		node_t spec = list();
		append(spec, simple(N_VOID));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore()));
		append(dl, pointer());
		node_t decl = node2(N_DECL, id(ti.c_str()), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, inits);
		append(out, sd);
	}

	return out;
}
```
**c2mir-risk checkpoint:** static initializers contain (a) an external-symbol address `+16` and (b) integer-to-`void*` casts. If `make` or a run reports c2mir rejecting either, the fallback is: emit `_ZTI` as a real `struct` with a `long` field for the packed flags/count and `long`/`void*` base-entry fields (a named struct type, same bytes) instead of a `void*[]`. Verify the `void*[]` form first — it is the lowest-friction and mirrors g++.

- [ ] **Step 5: Wire the RTTI slot in `class_vtable_def`.** Replace the S5a.2 placeholder (`rtti = ... integer(0)`) with a reference to `_ZTI<cls>`:

```cpp
		// RTTI slot: &_ZTI<cls> (the most-derived class's type_info; the SAME
		// object for every group — offset_to_top tells the runtime how far back).
		std::string ti = itanium_typeinfo_sym(cdd->name);
		referenced_funcs.insert(ti);
		node_t vtype2 = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t rtti = node2(N_CAST, vtype2, id(ti.c_str()));
		append(inits, node2(N_INIT, list(), rtti));
```

- [ ] **Step 6: Emit one type_info per polymorphic class.** Find the pass that emits vtables (cir_builder.cpp:~7788-7797, "Pass 1.5"; `grep -n "class_vtable_def(cdd" src/cir_builder.cpp`). Immediately before each `class_vtable_def(cdd, thunks)` emission, emit its type_info into the same output list:

```cpp
		if (node_t ti = class_typeinfo_def(cdd))
			append(/* the same definitions list the vtable is appended to */, ti);
```
(Match the exact append target used for the vtable `vt` node a few lines down — read the surrounding code and mirror it. The type_info must be emitted before or alongside the vtable since the vtable references `_ZTI<cls>`; c2mir tolerates forward references among file-scope decls, but emitting first is cleanest.)

- [ ] **Step 7: Add `#include "madc_mangle.h"` to cir_builder.cpp if not already present** (`grep -n madc_mangle src/cir_builder.cpp`).

- [ ] **Step 8: Build, verify the type_info symbol appears.**
Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → 0
Run: `( ulimit -t 60; ./bin/madc --emit=c11 tests/test_rtti_typeinfo.mad 2>&1 | grep -c "_ZTI5Shape" )` → ≥1 (the type_info object + vtable slot reference it)

- [ ] **Step 9: Commit** (the `.mad` end-to-end run is gated at S5d, but emission is now in place).
```bash
git add src/cir_builder.cpp src/cir_builder.h
git commit -m "feat(cir): emit ABI-faithful type_info objects + wire vtable RTTI slot (S5b.3)"
```

---

# Stage S5c — dynamic_cast<T*>(e)

### Task S5c.1: TokenDynamicCast + parse

**Files:**
- Modify: `include/tokens.h` (new `TokenDynamicCast`)
- Modify: `src/parser.cpp` (expression parser, ~9297, add a `dynamic_cast` branch)
- Test: `tests/test_rtti_dyncast.mad` (+ `.expect`)

- [ ] **Step 1: Write the failing integration test.** Create `tests/test_rtti_dyncast.mad`:

```cpp
#include <iostream>
using namespace std;
class Base { public: virtual ~Base() {} virtual int who() { return 0; } };
class Derived : public Base { public: int who() { return 1; } int extra() { return 42; } };
class Other  : public Base { public: int who() { return 2; } };
int main() {
    Base *b = new Derived();
    Derived *d = dynamic_cast<Derived*>(b);
    cout << (d != 0) << endl;            // 1 (succeeds)
    cout << d->extra() << endl;          // 42
    Other *o = dynamic_cast<Other*>(b);
    cout << (o == 0) << endl;            // 1 (fails -> null)
    delete b;
    return 0;
}
```
Create `tests/test_rtti_dyncast.expect`:
```
1
42
1
```
Verify the oracle: `g++ -std=c++17 tests/test_rtti_dyncast.mad -o tmp/dc && tmp/dc` prints `1 / 42 / 1`.

- [ ] **Step 2: Run it, verify it fails** (`dynamic_cast` unparsed → parse error / treated as a call to an undefined identifier).
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_dyncast.mad 2>&1 | grep -v setrlimit | tail -3 )`

- [ ] **Step 3: Add `TokenDynamicCast` to `include/tokens.h`** (near the other expression tokens; it holds the target type and the operand sub-expression). Follow the existing token-class shape (a `TokenBase` subclass with `compile()`/`operand()` implemented in cir_builder via the visitor, OR an inline cir lowering — match how `TokenNullptr`/`TokenMember` are structured). Minimal field set:

```cpp
class TokenDynamicCast : public TokenBase {
public:
    DataDef    *target_type;   // the T in dynamic_cast<T*> (the pointee class)
    bool        target_is_ptr; // true for T* form (the only supported form; see S5c.4)
    TokenBase  *operand;       // the (e) sub-expression
    TokenDynamicCast() : target_type(NULL), target_is_ptr(false), operand(NULL) {}
    virtual TokenID id() const { return TokenID::tkDynamicCast; }
    virtual TokenBase *clone() { return new TokenDynamicCast(*this); }
    // compile()/operand() — see cir_builder lowering task S5c.2
};
```
Add `tkDynamicCast` to the `TokenID` enum in `include/tokens.h` (find the enum, add a new value at the end of the appropriate group).

- [ ] **Step 4: Parse it** in the expression parser. In `src/parser.cpp` after the `sizeof` branch (~9312), add:

```cpp
		if ( ident_tb->str == "dynamic_cast" )
		{
		    // dynamic_cast < TYPE * > ( EXPR )
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkLT )
			Throw(tb) << "Expecting '<' after dynamic_cast" << flush;
		    nextToken(); // consume '<'
		    skip_expression_whitespace(*this);
		    // Parse the target type name (a class) + trailing '*'.
		    TokenBase *type_tb = nextToken();
		    DataDef *tgt = resolve_type_token(*this, type_tb);   // see note below
		    if ( !tgt )
			Throw(type_tb ? type_tb : tb) << "dynamic_cast target is not a type" << flush;
		    skip_expression_whitespace(*this);
		    bool is_ptr = false;
		    if ( peekToken() && peekToken()->id() == TokenID::tkMul )
		    { nextToken(); is_ptr = true; }
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkGT )
			Throw(tb) << "Expecting '>' to close dynamic_cast<...>" << flush;
		    nextToken(); // consume '>'
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after dynamic_cast<...>" << flush;
		    nextToken(); // consume '('
		    TokenBase *inner_first = nextToken();
		    TokenBase *inner = parseExpression(inner_first, false, false, false, 0, true);
		    skip_expression_whitespace(*this);
		    TokenBase *close_tb = nextToken();
		    if ( !close_tb || close_tb->id() != TokenID::tkClBrk )
			Throw(close_tb ? close_tb : tb) << "Expecting ')' to close dynamic_cast" << flush;
		    TokenDynamicCast *dc = new TokenDynamicCast();
		    dc->target_type = tgt;
		    dc->target_is_ptr = is_ptr;
		    dc->operand = inner;
		    dc->file = tb->file; dc->line = tb->line; dc->column = tb->column;
		    exStack.push(dc);
		    break;
		}
```
**Note on `resolve_type_token`:** there is existing type-name resolution used by `sizeof`/casts — find it with `grep -n "TokenDataType\|resolve.*type\|datatype" src/parser.cpp` around the `sizeof`/`evaluate_type_query` path and reuse that helper (the `sizeof(Type)` path already turns a type token into a `DataDef*`). Do not write a new resolver; reuse the one `evaluate_type_query` uses. If it is a static free function, call it the same way `sizeof` does. The token after `<` is a `TokenDataType` for a known class.

- [ ] **Step 5: Build, run — verify it now reaches lowering** (will fail at compile/lower until S5c.2, but the parse error is gone).
Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → 0
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_dyncast.mad 2>&1 | grep -v setrlimit | tail -3 )` (expect a lowering error / unimplemented, NOT a parse error).

- [ ] **Step 6: Commit.**
```bash
git add include/tokens.h src/parser.cpp
git commit -m "feat(parser): parse dynamic_cast<T*>(e) into TokenDynamicCast (S5c.1)"
```

### Task S5c.2: lower TokenDynamicCast to __dynamic_cast

**Files:**
- Modify: `src/cir_builder.cpp` (the token→cir_node lowering dispatch; extern decl for `__dynamic_cast`)
- Test: `tests/test_rtti_dyncast.mad` (from S5c.1)

- [ ] **Step 1: Find the token-lowering dispatch.** `grep -n "tkNullptr\|TokenNullptr\|case TokenID::tk" src/cir_builder.cpp` to locate where expression tokens are translated to cir_node (the big visitor / switch). Add a `TokenDynamicCast` case there.

- [ ] **Step 2: Implement the lowering.** The case builds:
`(Tgt*)__dynamic_cast((void*)operand, (void*)&_ZTI<srcCls>, (void*)&_ZTI<dstCls>, (long)hint)`
where `srcCls` = the static class of `operand`'s pointee, `dstCls` = `target_type`, and `hint` = `is_unique_public_nonvirtual_base` offset (dst derives from src) else `-1`.

```cpp
case TokenID::tkDynamicCast: {
    TokenDynamicCast *dc = static_cast<TokenDynamicCast *>(tb);
    node_t operand_node = translate(dc->operand);             // existing recursive entry
    DataDefCLASS *dstC = class_behind(dc->target_type);
    DataDef *src_dd = dc->operand->datatype();                // pointer-to-src
    DataDefCLASS *srcC = class_behind(src_dd);
    if (!dstC || !srcC)
        Throw(...) << "dynamic_cast requires polymorphic class pointers" << flush;
    std::string src_ti = itanium_typeinfo_sym(srcC->name);
    std::string dst_ti = itanium_typeinfo_sym(dstC->name);
    referenced_funcs.insert(src_ti);
    referenced_funcs.insert(dst_ti);
    // hint: offset of src within dst if unique public non-virtual base, else -1.
    long hint = -1; size_t off = 0;
    if (dstC->is_unique_public_nonvirtual_base(srcC, &off)) hint = (long)off;

    auto vptr_t = [&]() {
        return node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
                     node2(N_DECL, ignore(), node1(N_LIST, pointer())));
    };
    node_t args = list();
    append(args, node2(N_CAST, vptr_t(), operand_node));
    append(args, node2(N_CAST, vptr_t(), id(src_ti.c_str())));   // &_ZTI<src> (array decays)
    append(args, node2(N_CAST, vptr_t(), id(dst_ti.c_str())));
    append(args, integer(hint));
    node_t call = node2(N_CALL, id("__dynamic_cast"), args);
    referenced_funcs.insert("__dynamic_cast");
    // cast the void* result to Tgt*
    node_t tgt_t = node2(N_TYPE,
        node1(N_LIST, node2(N_STRUCT, id(dstC->name.c_str()), ignore())),
        node2(N_DECL, ignore(), node1(N_LIST, pointer())));
    return node2(N_CAST, tgt_t, call);
}
```
(Adapt `translate`/`datatype()`/`class_behind` to the exact names already in cir_builder — `class_behind` exists per S3; the recursive token-lowering entry and the operand's datatype accessor exist for every other case; mirror an existing pointer-returning case.)

- [ ] **Step 3: Emit the `__dynamic_cast` extern decl once.** In the same place type_info externs are emitted (or a one-time prologue emission), declare:
`extern void *__dynamic_cast(const void *, const void *, const void *, long);`
Build it as an `N_SPEC_DECL` with `N_EXTERN` + a function declarator, OR rely on the implicit-declaration / dlsym fallback if cir_builder already permits undeclared external calls (check how other runtime helpers like `__madc_vla_free` are declared — `grep -n "__madc_vla_free\|N_EXTERN" src/cir_builder.cpp` — and mirror that exact mechanism). `__dynamic_cast` is exported by libstdc++ and resolves via `dlsym(RTLD_DEFAULT)` at MIR link time.

- [ ] **Step 4: Build + run, verify pass.**
Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )` → 0
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_dyncast.mad 2>&1 | grep -v setrlimit )`
Expected: `1` / `42` / `1`.

- [ ] **Step 5: Full regression gate** (build 0-warn; run_tests known-6 + flaky only, +new test passes; SMAUG boots; no-std gate not increased).

- [ ] **Step 6: Commit.**
```bash
git add src/cir_builder.cpp tests/test_rtti_dyncast.mad tests/test_rtti_dyncast.expect
git commit -m "feat(cir): lower dynamic_cast<T*> to libstdc++ __dynamic_cast (S5c.2)"
```

### Task S5c.3: MI / cross-cast dynamic_cast test (offset-adjusting)

**Files:**
- Test: `tests/test_rtti_dyncast_mi.mad` (+ `.expect`)

- [ ] **Step 1: Write the test** — a dynamic_cast that must adjust `this` (downcast to a class where the source is a non-primary base, and a cross-cast between siblings). Create `tests/test_rtti_dyncast_mi.mad`:

```cpp
#include <iostream>
using namespace std;
class I1 { public: virtual int a() { return 1; } };
class I2 { public: virtual int b() { return 2; } };
class Impl : public I1, public I2 { public: int a() { return 10; } int b() { return 20; } };
int main() {
    Impl im;
    I1 *p1 = &im;
    // cross-cast I1* -> I2* via the most-derived Impl:
    I2 *p2 = dynamic_cast<I2*>(p1);
    cout << (p2 != 0) << endl;     // 1
    cout << p2->b() << endl;       // 20
    Impl *back = dynamic_cast<Impl*>(p1);
    cout << back->a() << endl;     // 10
    return 0;
}
```
Create `tests/test_rtti_dyncast_mi.expect`:
```
1
20
10
```
Verify the g++ oracle prints `1 / 20 / 10`.

- [ ] **Step 2: Run, verify it passes** (the runtime `__dynamic_cast` does the offset adjustment using offset_to_top + the type_info graph — no madc-side offset math needed; this test confirms the prologue + type_info base list are correct).
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_dyncast_mi.mad 2>&1 | grep -v setrlimit )`
Expected: `1` / `20` / `10`. If it fails, the `__vmi` base offset_flags or offset_to_top is wrong — re-probe g++'s `_ZTI4Impl` and the vtable prologue and compare.

- [ ] **Step 3: Commit.**
```bash
git add tests/test_rtti_dyncast_mi.mad tests/test_rtti_dyncast_mi.expect
git commit -m "test(rtti): MI cross-cast + downcast via __dynamic_cast (S5c.3)"
```

### Task S5c.4: reject the unsupported reference form with a clear diagnostic (YAGNI guard)

**Files:**
- Modify: `src/parser.cpp` (the dynamic_cast parse branch)

- [ ] **Step 1:** The reference form `dynamic_cast<T&>(e)` must throw `bad_cast` on failure (needs exception integration) — out of scope (spec Non-goals). In the parse branch, after detecting the type, if the next token is `tkBand` (`&`) instead of `*`, `Throw(tb) << "dynamic_cast to a reference type is not yet supported (use the pointer form)" << flush;`. This prevents silent miscompilation.

- [ ] **Step 2: Build, commit.**
```bash
( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )   # 0
git add src/parser.cpp
git commit -m "feat(parser): diagnose unsupported dynamic_cast reference form (S5c.4)"
```

---

# Stage S5d — typeid + type_info::name()

### Task S5d.1: minimal `<typeinfo>` header binding std::type_info

**Files:**
- Create: `include/madc/typeinfo`
- (Regenerate embedded headers via `make -C src` per `.claude/rules/embedded-headers.md`.)

- [ ] **Step 1:** Create `include/madc/typeinfo` declaring `std::type_info` as a header-defined class whose methods bind to the real libstdc++ symbols (the campaign keystone: bodyless std:: methods bind via the mangler). Minimal surface the tests need — `name()`, `operator==`, `operator!=`:

```cpp
// Minimal std::type_info — binds to libstdc++'s real type_info. The objects
// madc emits (_ZTI<cls>) are ABI-compatible, so equality and name() work
// against the real runtime. (Keystone: bodyless std:: methods bind to the
// libstdc++ symbol via the Itanium mangler — no hardcoded literal.)
namespace std {
    class type_info {
    public:
        const char *name() const;                 // _ZNKSt9type_info4nameEv
        bool operator==(const type_info &o) const; // _ZNKSt9type_infoeqERKS_
        bool operator!=(const type_info &o) const; // _ZNKSt9type_infoneERKS_
        virtual ~type_info();
    };
}
```
Confirm the symbol spellings: `echo _ZNKSt9type_info4nameEv | c++filt` → `std::type_info::name() const`. The keystone binds these automatically from the parsed declaration — do NOT hardcode the mangled strings in madc source (they belong only in comments / the mangler's output).

- [ ] **Step 2:** Run `make -C src` so `scripts/gen_embedded_headers.sh` bakes the header in. Verify a program can `#include <typeinfo>` and name the type:
Run: `( ulimit -t 60; printf '#include <typeinfo>\nint main(){ return 0; }\n' > tmp/ti_inc.mad; ./bin/madc tmp/ti_inc.mad 2>&1 | grep -v setrlimit | tail -2 )`
Expected: no error.

- [ ] **Step 3: Commit.**
```bash
git add include/madc/typeinfo
git commit -m "feat(headers): minimal <typeinfo> binding std::type_info to libstdc++ (S5d.1)"
```

### Task S5d.2: TokenTypeid + parse

**Files:**
- Modify: `include/tokens.h` (`TokenTypeid`, `tkTypeid`)
- Modify: `src/parser.cpp` (expression parser, after the `dynamic_cast` branch)
- Test: `tests/test_rtti_typeinfo.mad` (from S5b.3; promote to the suite here)

- [ ] **Step 1: Add `TokenTypeid` to `include/tokens.h`** (+ `tkTypeid` in the enum):

```cpp
class TokenTypeid : public TokenBase {
public:
    DataDef   *static_type; // non-NULL for typeid(Type)
    TokenBase *operand;     // non-NULL for typeid(expr)
    TokenTypeid() : static_type(NULL), operand(NULL) {}
    virtual TokenID id() const { return TokenID::tkTypeid; }
    virtual TokenBase *clone() { return new TokenTypeid(*this); }
};
```

- [ ] **Step 2: Parse it** in the expression parser (after the dynamic_cast branch). `typeid(` may be followed by a type or an expression — try a type first, fall back to an expression (mirror how `sizeof` distinguishes a type from an expression; reuse that classifier):

```cpp
		if ( ident_tb->str == "typeid" )
		{
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after typeid" << flush;
		    nextToken(); // '('
		    TokenTypeid *ttd = new TokenTypeid();
		    skip_expression_whitespace(*this);
		    TokenBase *first = nextToken();
		    if ( DataDef *t = try_resolve_type_token(*this, first) )  // type form (reuse sizeof's classifier)
			ttd->static_type = t;
		    else
			ttd->operand = parseExpression(first, false, false, false, 0, true);
		    skip_expression_whitespace(*this);
		    TokenBase *close_tb = nextToken();
		    if ( !close_tb || close_tb->id() != TokenID::tkClBrk )
			Throw(close_tb ? close_tb : tb) << "Expecting ')' after typeid(...)" << flush;
		    ttd->file = tb->file; ttd->line = tb->line; ttd->column = tb->column;
		    exStack.push(ttd);
		    break;
		}
```
(Use whatever helper `sizeof` uses to decide type-vs-expression; `try_resolve_type_token` is a placeholder for that existing classifier — find it near `evaluate_type_query` / `try_parse_dynamic_type_query` and reuse it.)

- [ ] **Step 3: Build, commit** (lowering follows in S5d.3; parse must be reachable).
```bash
( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )   # 0
git add include/tokens.h src/parser.cpp
git commit -m "feat(parser): parse typeid(expr|type) into TokenTypeid (S5d.2)"
```

### Task S5d.3: lower TokenTypeid

**Files:**
- Modify: `src/cir_builder.cpp` (token-lowering dispatch)
- Test: `tests/test_rtti_typeinfo.mad`

- [ ] **Step 1: Implement the lowering** in the same switch as `TokenDynamicCast`. Result is a `const std::type_info&` — represent as a `std::type_info*` that downstream member-call code dereferences (match how madc represents reference-returning expressions; references are pointers per `.claude/rules/c11-transpiler.md`).

```cpp
case TokenID::tkTypeid: {
    TokenTypeid *ti = static_cast<TokenTypeid *>(tb);
    DataDefCLASS *typeinfo_cls = /* lookup std::type_info from the type table */;
    auto ti_ptr_t = [&]() { /* std::type_info* type node */ ... };
    if (ti->static_type) {
        // static: &_ZTI<T>
        DataDefCLASS *c = class_behind(ti->static_type);
        std::string sym = itanium_typeinfo_sym(c->name);
        referenced_funcs.insert(sym);
        return node2(N_CAST, ti_ptr_t(), id(sym.c_str()));
    }
    // expr form
    DataDefCLASS *ec = class_behind(ti->operand->datatype());
    if (ec && ec->is_polymorphic()) {
        // runtime: (std::type_info*) ((void**) ((char*)&expr_vptr_load))[-1]
        // i.e. load the object's vptr, index [-1] for the RTTI slot.
        node_t obj = translate(ti->operand);                       // glvalue of the object
        // vptr = *(void***)obj  (vptr is at offset 0 of a polymorphic object)
        node_t vpp = node2(N_CAST,
            /* void*** */ ptr3_void_t(), node2(N_ADDR, obj));      // &obj as void***
        node_t vptr = node2(N_DEREF, node2(N_DEREF, vpp));         // (*(void***)&obj) -> void**
        // rtti = ((void**)vptr)[-1]
        node_t rtti = node2(N_DEREF, node2(N_ADD, vptr, integer(-1)));
        return node2(N_CAST, ti_ptr_t(), rtti);
    }
    // non-polymorphic expr: static type, like the type form.
    std::string sym = itanium_typeinfo_sym(ec ? ec->name : std::string());
    referenced_funcs.insert(sym);
    return node2(N_CAST, ti_ptr_t(), id(sym.c_str()));
}
```
**Pointer-arithmetic note:** `((void**)vptr)[-1]` indexes one pointer slot *before* the address point — exactly where S5a/S5b put `&_ZTI`. Verify the `N_ADD … integer(-1)` is scaled by `void*` (8 bytes) — if cir_builder's `N_ADD` on a `void**` does pointer scaling, `-1` = −8 bytes (correct); if it is raw byte arithmetic, use `integer(-8)` on a `char*` view. Mirror exactly how dispatch computes `[slot]` (cir_builder.cpp:~3160) which already does scaled `void**` indexing — copy that idiom and use index `-1`.

- [ ] **Step 2: Build + run `tests/test_rtti_typeinfo.mad`, verify pass.**
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_typeinfo.mad 2>&1 | grep -v setrlimit )`
Expected: `1` / `0`. (`typeid(*s)` reads the runtime type from the vtable RTTI slot = `&_ZTI Circle`; `typeid(Circle)` = `&_ZTI Circle`; `operator==` on real type_info compares to equal. `typeid(Shape)` differs.)

- [ ] **Step 3: Add the `.expect` to the suite** (already created in S5b.3). Full regression gate.

- [ ] **Step 4: Commit.**
```bash
git add src/cir_builder.cpp tests/test_rtti_typeinfo.mad tests/test_rtti_typeinfo.expect
git commit -m "feat(cir): lower typeid(expr|type) to the ABI type_info (S5d.3)"
```

### Task S5d.4: typeid name() end-to-end + final gate

**Files:**
- Test: `tests/test_rtti_name.mad` (+ `.expect`)

- [ ] **Step 1: Write the test** exercising `type_info::name()` (binds to libstdc++):

```cpp
#include <iostream>
#include <typeinfo>
using namespace std;
class Widget { public: virtual ~Widget() {} };
int main() {
    Widget w;
    // libstdc++ name() returns the mangled name; for Widget that is "6Widget".
    cout << typeid(w).name() << endl;     // 6Widget
    return 0;
}
```
Create `tests/test_rtti_name.expect`:
```
6Widget
```
Verify the g++ oracle: `g++ -std=c++17 tests/test_rtti_name.mad -o tmp/nm && tmp/nm` → `6Widget`.

- [ ] **Step 2: Run, verify pass.** (`typeid(w)` → `&_ZTI6Widget`; `.name()` binds to `_ZNKSt9type_info4nameEv`, returns the `_ZTS` string `"6Widget"`.)
Run: `( ulimit -t 60; ./bin/madc tests/test_rtti_name.mad 2>&1 | grep -v setrlimit )`
Expected: `6Widget`. If `name()` does not resolve, confirm `<typeinfo>` was included and the keystone bound the symbol (`./bin/madc --emit=c11 tests/test_rtti_name.mad | grep _ZNKSt9type_info4name`).

- [ ] **Step 3: Final full regression gate** — build 0-warn; `run_tests.sh` shows only the known-6 + flaky failing, all `tests/test_rtti_*.mad` + the 6 MI tests passing; unit `test_class_layout` + `test_mangle_rtti` green; SMAUG boots; no-std-hardcoding gate not increased.

- [ ] **Step 4: Commit.**
```bash
git add tests/test_rtti_name.mad tests/test_rtti_name.expect
git commit -m "test(rtti): typeid(x).name() end-to-end via libstdc++ (S5d.4)"
```

---

## Post-S5 wrap (not a code task — for the controller)

- Update `docs/superpowers/plans/2026-06-03-multiple-inheritance-HANDOFF.md`: S5 done, MI feature complete; the deferred items (construction vtables/VTT, runtime vbase_offset) remain logged.
- Update memory `project_multiple_inheritance` (S5 done) + the KG `Feature{Multiple and Virtual Inheritance}` node (status complete).
- Merge `feature/multiple-inheritance-claude` → `feature/retire-std-hardcoding-claude` (the stack promotes to develop together later) — do NOT promote to develop yet (parity gate).
- The std:: campaign resumes **string-first** on this foundation (handoff §RELATIONSHIP).

---

## Self-Review

**1. Spec coverage (spec §9-§13):**
- §9 type_info objects (3 flavors) → S5b.3. `dynamic_cast`→__dynamic_cast → S5c.2. `typeid`→S5d.3. RTTI slot in vtable → S5a + S5b.3. std:: REFERENCE libstdc++ type_info → S5b.3 (only `has_vtable` user classes emit; std:: classes bind via mangler, S5d.1).
- §10 mangling `_ZTI`/`_ZTS` → S5b.1. (`_ZTV`/`_ZTT`/`_ZThn`/`_ZTv`/`_ZTC`: madc uses its own `Cls__vtable`/`Cls__thunk` naming for user-class dispatch — real `_ZTV` etc. are only needed for libstdc++ vtable interop = the std:: campaign, NOT S5. Noted, deliberately out of scope here; the spec lists them under §10 but §9 only consumes `_ZTI`/`_ZTS`.)
- §11 portability: all emitted constructs are `void*[]`/`N_CAST`/`N_CALL`/extern decls — portable C11, no inline asm → `--emit=c11` stays valid (verified by the S5b.3 `--emit=c11` grep). ✓
- §12 testing: layout/mangling doctests (S5a.1, S5b.1, S5b.2) + integration matched to g++ (every S5c/S5d `.mad` verified against `g++ -std=c++17`) + regression gate each task. ✓
- §13 staging: S5 is the last MI stage; ends with "campaign resumes string-first" (post-S5 wrap). ✓
- Non-goals: reference-form dynamic_cast rejected with a diagnostic (S5c.4), not silently mis-lowered. ✓

**2. Placeholder scan:** The three helper names that defer to existing code — `resolve_type_token`/`try_resolve_type_token` (the type classifier `sizeof` uses) and the `ti_ptr_t()`/`ptr3_void_t()` node builders — are explicitly flagged as "reuse the existing X, find with `grep`" rather than invented APIs, because writing a parallel resolver would violate no-parallel-implementations. Every cir_node construction step shows complete builder code in the established `list()`/`node2(N_…)`/`integer()`/`id()` idiom. No "TBD"/"handle edge cases".

**3. Type consistency:** `itanium_typeinfo_sym`/`itanium_typeinfo_name_sym`/`itanium_typeinfo_name_string` (S5b.1) used identically in S5b.3/S5c.2/S5d.3. `typeinfo_flavor()`/`TI_CLASS|TI_SI|TI_VMI` (S5b.2) used in S5b.3. `is_unique_public_nonvirtual_base(b,&off)` (S5b.2) used in S5c.2. `class_typeinfo_def` declared (S5b.3 step 3) and defined (step 4) with matching signature. `TokenDynamicCast`/`tkDynamicCast` and `TokenTypeid`/`tkTypeid` consistent across parse + lower. `addr_point`-includes-prologue (S5a.1) is the invariant S5a.2/S5b.3 rely on.
