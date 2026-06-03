# Multiple/Virtual Inheritance — S1: Layout Engine (Tier A core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the Itanium-faithful class **layout engine** — `DataDefCLASS::compute_layout()` — plus the base-list data structures it fills, proven by unit tests against g++/clang++-verified offsets. No change to live parsing/codegen yet (the engine is dormant until S2 wires it in).

**Architecture:** New data members on `DataDefCLASS` (a `BaseSpec` list, `vbase_offset` map, `nvsize`, `secondary_vptr_owners`) and a `compute_layout()` method that implements the Itanium record-layout algorithm: primary base @0 sharing the vptr, other non-virtual bases placed by their *non-virtual* size, own members after them, virtual bases appended once at the end (deduped across diamonds). It is tested standalone via a new doctest that builds the probe hierarchies and asserts the exact offsets `tmp/mi_probe.cpp` produced under clang++/g++.

**Tech Stack:** C++11, madc `DataDef` type system (`include/datadef.h`), `compute_layout()` defined in `src/parser.cpp` (where other `DataDefCLASS` methods like `findMethod` live, linked into unit tests via `TESTOBJ`), doctest (`tests/unit/`).

**Spec:** `docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md` (§0 ground truth, §1 data structures, §2 layout engine). **ABI numbers:** `tmp/mi-abi-findings.md`.

**Ground-truth offsets this plan must reproduce (from `tmp/mi_probe.cpp`, clang++/g++):**
| Class | Shape | size | base offsets | nvsize |
|---|---|---|---|---|
| `A` | `{long a}` non-poly | 8 | — | 8 |
| `B : A` | non-virtual single | 16 | A@0 | 16 |
| `Vbase` | `{vptr; long v}` poly | 16 | — | 16 |
| `Mid : virtual Vbase` | poly, 1 vbase | 32 | Vbase@16 | 16 |
| `Leaf : Mid` | poly | 40 | Mid@0, Vbase@24 | 24 |
| `P1`,`P2` | `{vptr; long}` poly | 16 | — | 16 |
| `MIc : P1, P2` | MI non-virtual | 40 | P1@0, P2@16 | 40 |
| `Top` | `{vptr; long}` poly | 16 | — | 16 |
| `L : virtual Top`,`R : virtual Top` | poly, 1 vbase | 32 | Top@16 | 16 |
| `Diamond : L, R` | diamond | 56 | L@0, R@16, Top@40 | 40 |

---

## File Structure

- **`include/datadef.h`** (modify, `DataDefCLASS` ~689-740): add `BaseSpec`, `bases`, `vbase_offset`, `nvsize`, `secondary_vptr_owners`, ctor inits, and the `compute_layout()` declaration + small inline helpers (`is_polymorphic()`, `own_data_size()`). Keep the existing `base_class` field unchanged (S1 introduces `bases` alongside it; no existing reader is touched).
- **`src/parser.cpp`** (modify): add the out-of-line `DataDefCLASS::compute_layout()` definition near the existing `DataDefCLASS::findMethod` (~3699). No call site added in S1.
- **`tests/unit/test_class_layout.cpp`** (create): doctest building the probe hierarchies and asserting the table above. Auto-discovered by the Makefile `wildcard` (no Makefile edit).

**Contract of `compute_layout()` (precise):** On entry, `members`/`member_offsets` hold ONLY this class's *own* data members at natural offsets starting at 0 (as `addMember` produced them), `size` is their packed size, `has_vtable` is set iff the class is polymorphic, and `bases` is populated (each `BaseSpec` has `base` + `is_virtual`; `offset`/`is_primary` are output fields). On exit: every `BaseSpec.offset` and `is_primary` is set; `vbase_offset` maps each (transitive, deduped) virtual base to its offset; `secondary_vptr_owners` lists non-primary polymorphic direct bases; own members in `member_offsets` are shifted to sit after the vptr+non-virtual-bases block; `nvsize` and `size` are final. Base *members* are NOT yet copied into `members` (that is S2/S3).

---

## Task 1: Base-list data structures on DataDefCLASS

**Files:**
- Modify: `include/datadef.h` (`DataDefCLASS`, after `base_class` at ~706 and in the ctor ~731-734)
- Test: `tests/unit/test_class_layout.cpp` (create)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_class_layout.cpp`:
```cpp
// Unit tests for DataDefCLASS::compute_layout() — Itanium MI/virtual layout.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <vector>
#include <map>
#include <string>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
// Global dd* instances come from parser.o via TESTOBJ.

// Build a polymorphic-or-not class with `ndata` 8-byte (ddINT64) own members.
// ddINT64 is the 8-byte primitive global (there is no ddLONG); declared extern
// in datadef.h:1027, defined in parser.cpp, linked via TESTOBJ.
static DataDefCLASS *mkclass(const char *name, int ndata, bool poly) {
    DataDefCLASS *c = new DataDefCLASS(name, 0, DataType::dtRESERVED);
    c->has_vtable = poly;
    for (int i = 0; i < ndata; i++) {
        char m[8]; m[0]='m'; m[1]=char('0'+i); m[2]=0;
        c->addMember(m, ddINT64, 1);   // 8-byte member; addMember sets offset+size+max_align
    }
    return c;
}

TEST_SUITE("class layout — data structures") {
    TEST_CASE("bases vector starts empty; BaseSpec records virtuality") {
        DataDefCLASS *a = mkclass("A", 1, false);
        CHECK(a->bases.empty());
        DataDefCLASS *b = mkclass("B", 1, false);
        b->bases.push_back(BaseSpec{a, 0, false, 0u, false});
        CHECK(b->bases.size() == 1);
        CHECK(b->bases[0].base == a);
        CHECK(b->bases[0].is_virtual == false);
    }
}
```

- [ ] **Step 2: Run to verify it fails (BaseSpec / `bases` undefined)**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests; the new binary will fail to build)
Expected: COMPILE FAIL — `BaseSpec` and `DataDefCLASS::bases` not declared.

- [ ] **Step 3: Add the data structures**

In `include/datadef.h`, immediately before `class DataDefCLASS` (~689) add:
```cpp
struct BaseSpec {
    DataDefCLASS *base;
    size_t        offset;     // subobject offset within the derived class
    bool          is_virtual; // `: virtual public B`
    uint32_t      access;     // existing vf* flags (0=public/vfPRIVATE/vfPROTECTED)
    bool          is_primary; // shares the most-derived vptr (offset 0)
};
```
(`DataDefCLASS` is forward-declared earlier in the file; `BaseSpec` referencing it as a pointer is fine.)

Inside `DataDefCLASS` (after `base_class;` at ~706) add:
```cpp
    std::vector<BaseSpec> bases;                       // direct bases (replaces base_class going forward)
    std::map<DataDefCLASS *, size_t> vbase_offset;     // virtual base -> offset (shared, at the end)
    std::vector<DataDefCLASS *> secondary_vptr_owners; // non-primary polymorphic direct bases
    size_t nvsize;                                     // non-virtual size (where vbases begin)
    bool is_polymorphic() const { return has_vtable; }
    void compute_layout();                             // Itanium layout engine (defined in parser.cpp)
```
In the ctor (~731-734) add `nvsize(0)` to the init list (e.g. after `has_vtable(false)`).

- [ ] **Step 4: Run to verify it passes**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (1 test case).

- [ ] **Step 5: Commit**

```bash
git add include/datadef.h tests/unit/test_class_layout.cpp
git commit -m "feat(class): BaseSpec + base-list data structures on DataDefCLASS (S1 task 1)"
```

---

## Task 2: compute_layout — non-virtual single base (B:A)

**Files:**
- Modify: `src/parser.cpp` (new `DataDefCLASS::compute_layout()` near `findMethod` ~3699)
- Test: `tests/unit/test_class_layout.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test_class_layout.cpp`:
```cpp
TEST_SUITE("class layout — single non-virtual base") {
    TEST_CASE("B : A  -> A@0, size 16") {
        DataDefCLASS *a = mkclass("A", 1, false);   // {long a} size 8
        a->compute_layout();
        CHECK(a->size == 8);
        CHECK(a->nvsize == 8);

        DataDefCLASS *b = mkclass("B", 1, false);   // own {long b}
        b->bases.push_back(BaseSpec{a, 0, false, 0u, false});
        b->compute_layout();
        CHECK(b->bases[0].offset == 0);
        CHECK(b->size == 16);
        CHECK(b->nvsize == 16);
        // own member b shifted to sit after base A (offset 8)
        CHECK(b->member_offsets[0] == 8);
    }
}
```

- [ ] **Step 2: Run to verify it fails (link error: compute_layout undefined)**

Run: `( ulimit -t 300; timeout 360 make -C src test )`
Expected: LINK FAIL — `undefined reference to DataDefCLASS::compute_layout()`.

- [ ] **Step 3: Implement compute_layout (non-virtual, no vbases yet)**

In `src/parser.cpp`, near `DataDefCLASS::findMethod` (~3699), add:
```cpp
// Round `sz` up to alignment `a` (a is a power of two).
static inline size_t mi_align_up(size_t sz, size_t a) { return (sz + a - 1) & ~(a - 1); }

// Itanium-faithful record layout. See docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md §2.
// Precondition: `members`/`member_offsets` hold OWN data members from offset 0; `has_vtable` set iff
// polymorphic; `bases` populated (base, is_virtual). Postcondition per the plan contract.
void DataDefCLASS::compute_layout()
{
    size_t cur = 0;
    size_t maxalign = 8; // class alignment accumulator (>= pointer align)

    // 1. vptr: a polymorphic class introduces a vptr at offset 0 unless a primary
    //    (polymorphic, non-virtual) base already provides one.
    bool have_primary = false;
    for (auto &bs : bases) {
        if (!bs.is_virtual && bs.base->is_polymorphic()) {
            bs.is_primary = true; have_primary = true; break;
        }
    }
    bool own_vptr = is_polymorphic() && !have_primary;
    if (own_vptr) cur += 8;   // __vptr at 0

    // 2. non-virtual bases in declaration order (primary first, at 0).
    //    A base contributes its NON-VIRTUAL size (nvsize); its vbases are hoisted.
    for (auto &bs : bases) {
        if (bs.is_virtual) continue;
        size_t balign = 8;
        cur = mi_align_up(cur, balign);
        bs.offset = bs.is_primary ? 0 : cur;
        if (bs.is_primary) {
            // primary base sits at 0 and shares the vptr; advance past its nvsize
            cur = bs.base->nvsize;
        } else {
            if (bs.base->is_polymorphic()) secondary_vptr_owners.push_back(bs.base);
            cur += bs.base->nvsize;
        }
        if (balign > maxalign) maxalign = balign;
    }

    // 3. own data members after the non-virtual bases (shift their offsets by the
    //    base block). addMember already set member_offsets (own, from 0), the packed
    //    own `size`, and max_align (the strongest own-member alignment).
    if (max_align > maxalign) maxalign = max_align;
    size_t own_block = mi_align_up(cur, max_align ? max_align : 1);
    for (size_t i = 0; i < member_offsets.size(); i++)
        member_offsets[i] = own_block + member_offsets[i];
    if (!member_offsets.empty())
        cur = own_block + size;   // `size` held the packed own-members size on entry
    else
        cur = own_block;

    // 4. nvsize = end of the non-virtual portion.
    nvsize = mi_align_up(cur, maxalign);

    // 5. (virtual bases appended in Task 4)
    size = mi_align_up(nvsize, maxalign);
}
```
(Note: own-member offsets/size/alignment come straight from `addMember` (real per-type values via `field_align`/`max_align`). The remaining S1 simplification is **base alignment** — bases are assumed 8-aligned (`balign = 8`), true for every probe and std:: stream class; S2 derives base alignment from the base's own `max_align` when a test needs sub-8 or over-8 base alignment.)

- [ ] **Step 4: Run to verify it passes**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (single-non-virtual-base suite + task-1 suite).

- [ ] **Step 5: Commit**

```bash
git add include/datadef.h src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(class): compute_layout for non-virtual single base (S1 task 2)"
```

---

## Task 3: compute_layout — polymorphic vptr reservation (Vbase, P1, Top standalone)

**Files:**
- Test: `tests/unit/test_class_layout.cpp`
- (No code change expected — verifies Task 2's vptr branch; if it fails, fix the vptr reservation in `compute_layout`.)

- [ ] **Step 1: Write the failing test**

```cpp
TEST_SUITE("class layout — polymorphic vptr") {
    TEST_CASE("polymorphic leaf reserves vptr at 0") {
        DataDefCLASS *v = mkclass("Vbase", 1, true); // {vptr; long v}
        v->compute_layout();
        CHECK(v->size == 16);
        CHECK(v->nvsize == 16);
        CHECK(v->member_offsets[0] == 8); // long v after vptr
    }
}
```

- [ ] **Step 2: Run to verify it passes or fails**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (Task 2 already reserves the own-vptr). If FAIL, the `own_vptr` branch is wrong — fix so `cur` starts at 8 and own members shift past it.

- [ ] **Step 3: (only if step 2 failed) fix the vptr branch in compute_layout**

Ensure the `if (own_vptr) cur += 8;` precedes the own-member `own_block` computation so members land at 8.

- [ ] **Step 4: Re-run to confirm PASS**

Run: `( ulimit -t 300; timeout 360 make -C src test )`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_class_layout.cpp src/parser.cpp
git commit -m "test(class): polymorphic vptr reservation (S1 task 3)"
```

---

## Task 4: compute_layout — single virtual base (Mid, Leaf)

**Files:**
- Modify: `src/parser.cpp` (`compute_layout` step 5 — append virtual bases)
- Test: `tests/unit/test_class_layout.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_SUITE("class layout — single virtual base") {
    TEST_CASE("Mid : virtual Vbase -> Vbase@16, size 32, nvsize 16") {
        DataDefCLASS *v = mkclass("Vbase", 1, true);
        v->compute_layout();                 // size 16, nvsize 16
        DataDefCLASS *mid = mkclass("Mid", 1, true); // own {long m}
        mid->bases.push_back(BaseSpec{v, 0, /*virtual*/true, 0u, false});
        mid->compute_layout();
        CHECK(mid->nvsize == 16);            // vptr + m
        CHECK(mid->size == 32);              // + Vbase(16) at end
        CHECK(mid->vbase_offset[v] == 16);
    }
    TEST_CASE("Leaf : Mid (primary) -> Mid@0, Vbase@24, size 40, nvsize 24") {
        DataDefCLASS *v = mkclass("Vbase", 1, true); v->compute_layout();
        DataDefCLASS *mid = mkclass("Mid", 1, true);
        mid->bases.push_back(BaseSpec{v, 0, true, 0u, false}); mid->compute_layout();
        DataDefCLASS *leaf = mkclass("Leaf", 1, true); // own {long l}
        leaf->bases.push_back(BaseSpec{mid, 0, false, 0u, false}); // non-virtual primary
        leaf->compute_layout();
        CHECK(leaf->bases[0].is_primary == true);
        CHECK(leaf->bases[0].offset == 0);
        CHECK(leaf->nvsize == 24);          // shares Mid vptr@0, m@8, l@16
        CHECK(leaf->vbase_offset[v] == 24); // Vbase hoisted once to the end
        CHECK(leaf->size == 40);
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: FAIL — virtual bases not yet placed (`vbase_offset` empty, sizes short).

- [ ] **Step 3: Implement transitive vbase collection + placement**

Add a helper declaration in `include/datadef.h` inside `DataDefCLASS`:
```cpp
    void collect_vbases(std::vector<DataDefCLASS *> &out,
                        std::set<DataDefCLASS *> &seen) const;
```
Add `#include <set>` to `datadef.h` if not present.

In `src/parser.cpp` add the helper and replace `compute_layout` step 5 (`size = mi_align_up(...)`) with vbase placement:
```cpp
void DataDefCLASS::collect_vbases(std::vector<DataDefCLASS *> &out,
                                  std::set<DataDefCLASS *> &seen) const
{
    for (const auto &bs : bases) {
        // a base's own (transitive) virtual bases come first, then the base itself if virtual
        bs.base->collect_vbases(out, seen);
        if (bs.is_virtual && seen.insert(bs.base).second)
            out.push_back(bs.base);
    }
}
```
Replace step 5 with:
```cpp
    // 5. virtual bases: appended once at the end, in canonical (collected) order.
    std::vector<DataDefCLASS *> vbs;
    std::set<DataDefCLASS *> seen;
    collect_vbases(vbs, seen);
    size_t end = nvsize;
    for (DataDefCLASS *vb : vbs) {
        end = mi_align_up(end, 8);
        vbase_offset[vb] = end;
        end += vb->nvsize;        // vbase contributes its non-virtual size
        if (8 > maxalign) maxalign = 8;
    }
    size = mi_align_up(end, maxalign);
```

- [ ] **Step 4: Run to verify it passes**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (Mid + Leaf cases match the ground-truth table).

- [ ] **Step 5: Commit**

```bash
git add include/datadef.h src/parser.cpp tests/unit/test_class_layout.cpp
git commit -m "feat(class): virtual-base placement in compute_layout (S1 task 4)"
```

---

## Task 5: compute_layout — multiple non-virtual bases (MIc:P1,P2)

**Files:**
- Test: `tests/unit/test_class_layout.cpp`
- (Verifies the secondary-base branch from Task 2; fix only if it fails.)

- [ ] **Step 1: Write the failing test**

```cpp
TEST_SUITE("class layout — MI non-virtual") {
    TEST_CASE("MIc : P1, P2 -> P1@0, P2@16, size 40") {
        DataDefCLASS *p1 = mkclass("P1", 1, true); p1->compute_layout(); // nvsize 16
        DataDefCLASS *p2 = mkclass("P2", 1, true); p2->compute_layout(); // nvsize 16
        DataDefCLASS *mic = mkclass("MIc", 1, true); // own {long c}
        mic->bases.push_back(BaseSpec{p1, 0, false, 0u, false});
        mic->bases.push_back(BaseSpec{p2, 0, false, 0u, false});
        mic->compute_layout();
        CHECK(mic->bases[0].is_primary == true);
        CHECK(mic->bases[0].offset == 0);
        CHECK(mic->bases[1].offset == 16);            // P2 with its own vptr
        CHECK(mic->secondary_vptr_owners.size() == 1);
        CHECK(mic->secondary_vptr_owners[0] == p2);
        CHECK(mic->member_offsets[0] == 32);          // c after both bases
        CHECK(mic->size == 40);
    }
}
```

- [ ] **Step 2: Run**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (Task 2's non-primary branch places P2@16 and records the secondary vptr owner). If FAIL, fix the non-primary branch so `bs.offset = cur` then `cur += bs.base->nvsize`.

- [ ] **Step 3: (only if failed) fix the secondary-base branch**

Ensure non-primary polymorphic bases push to `secondary_vptr_owners` and advance `cur` by `nvsize`.

- [ ] **Step 4: Re-run to confirm PASS**

Run: `( ulimit -t 300; timeout 360 make -C src test )`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_class_layout.cpp src/parser.cpp
git commit -m "test(class): MI non-virtual layout, secondary vptr (S1 task 5)"
```

---

## Task 6: compute_layout — diamond with shared virtual base (Diamond:L,R)

**Files:**
- Test: `tests/unit/test_class_layout.cpp`
- (Verifies the dedup in `collect_vbases`; fix only if it fails.)

- [ ] **Step 1: Write the failing test**

```cpp
TEST_SUITE("class layout — diamond") {
    TEST_CASE("Diamond : L,R (both : virtual Top) -> L@0,R@16,Top@40 shared, size 56") {
        DataDefCLASS *top = mkclass("Top", 1, true); top->compute_layout(); // nvsize 16
        DataDefCLASS *l = mkclass("L", 1, true);
        l->bases.push_back(BaseSpec{top, 0, true, 0u, false}); l->compute_layout(); // nvsize 16, size 32
        DataDefCLASS *r = mkclass("R", 1, true);
        r->bases.push_back(BaseSpec{top, 0, true, 0u, false}); r->compute_layout();
        DataDefCLASS *dia = mkclass("Diamond", 1, true); // own {long d}
        dia->bases.push_back(BaseSpec{l, 0, false, 0u, false});
        dia->bases.push_back(BaseSpec{r, 0, false, 0u, false});
        dia->compute_layout();
        CHECK(dia->bases[0].offset == 0);
        CHECK(dia->bases[1].offset == 16);
        CHECK(dia->member_offsets[0] == 32);          // d
        CHECK(dia->vbase_offset[top] == 40);          // Top appears ONCE, at the end
        CHECK(dia->size == 56);
    }
}
```

- [ ] **Step 2: Run**

Run: `( ulimit -t 300; timeout 360 make -C src test )`  (builds + runs all unit tests with the right LD_LIBRARY_PATH/caps; find the `test_class_layout` lines in the output)
Expected: PASS (the `seen` set in `collect_vbases` dedups Top so it is placed once at 40). If FAIL (Top placed twice / size 72), the dedup is wrong — ensure `seen.insert(...).second` guards the push.

- [ ] **Step 3: (only if failed) fix the dedup in collect_vbases**

Ensure each virtual base is pushed at most once across the whole graph.

- [ ] **Step 4: Re-run to confirm PASS**

Run: `( ulimit -t 300; timeout 360 make -C src test )`
Expected: PASS (all six suites green).

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_class_layout.cpp src/parser.cpp
git commit -m "feat(class): diamond shared-vbase dedup in compute_layout (S1 task 6)"
```

---

## Task 7: Full S1 gate

**Files:** none (verification only)

- [ ] **Step 1: Build clean (0 warnings)**

Run: `( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )`
Expected: `0`.

- [ ] **Step 2: Full test suite green**

Run: `( ulimit -t 600; timeout 700 make -C src fulltest > tmp/s1.log 2>&1 ); tail -5 tmp/s1.log`
Expected: unit tests pass (incl. `test_class_layout`), integration **457 passed** (no regression — S1 changes nothing in the live path), and the no-std-hardcoding gate count UNCHANGED at 469 (S1 adds machinery, removes nothing).

- [ ] **Step 3: Confirm no behavior change**

Run: `git diff --stat develop..HEAD -- src/cir_builder.cpp` (expect: empty — S1 touches no codegen) and confirm `compute_layout` has **no call site** yet: `grep -rn "compute_layout()" src/ | grep -v "void DataDefCLASS::compute_layout"` → expect only the declaration in `datadef.h`.
Expected: engine is dormant; live parsing/codegen untouched.

- [ ] **Step 4: Push**

```bash
git push -u origin feature/multiple-inheritance-claude
```

---

## Self-review notes (author)
- **Spec coverage:** S1 covers §1 (data structures) + §2 (`compute_layout`, sizes/offsets/nvsize/vbase_offset/secondary_vptr_owners, replacing the *mechanism* that S2 will use to drop `sizeof(std::)`). The live `sizeof(std::)` removal, parser virtual/comma-base parsing, flatten rework, C11 lowering, vtables/VTT/thunks/RTTI are explicitly S2–S5 — not in scope here.
- **Base-alignment proxy:** own-member offsets/size/align are real (from `addMember`/`max_align`). The one S1 simplification is `balign = 8` for bases (true for every probe + std:: stream); S2 derives base alignment from the base's `max_align` and adds the layout doctest that `#include`s real `<fstream>`/`<string>`. Flagged in Task 2 step 3 so it is not mistaken for a finished general sizer.
- **No behavior change:** `base_class` field and all ~16 readers are untouched; `compute_layout` has no live call site (Task 7 step 3 enforces). Existing 457 integration + gate 469 must be unchanged.
- **Type consistency:** `BaseSpec{base, offset, is_virtual, access, is_primary}` field order is used identically in every test push_back and in `compute_layout`. `nvsize`, `vbase_offset`, `secondary_vptr_owners`, `is_polymorphic()`, `collect_vbases()` names are consistent across tasks.
