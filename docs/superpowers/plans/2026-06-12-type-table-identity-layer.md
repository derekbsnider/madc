# Type Table Identity Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land phase 1 of `docs/plans/2026-06-12-type-table-value-abi-design.md` — the uint32 typeid identity layer: ABI-pinned primitive slots, `DataDef::type_id`, and the per-Program project segment with lazy stamping. Zero behavior change; compiler internals keep using `DataDef*`.

**Architecture:** A pure-C header (`include/madc_typeid.h`) pins the segmented id space (0 invalid · `[1,0x100)` primitives · `[0x100,0x01000000)` system-forest reserved · `[0x01000000,…)` project). *(Amended post-execution 2026-06-12: primitive segment widened from `[1,100)` to `[1,0x100)` — 255 usable slots, primitive ids fit in a byte — user decision; code blocks below predate the widening.)* One switch (`madc_primitive_for_slot`) is the single source of truth slot↔global-DataDef; `Program::type_id_for()` is the single lazy-registration chokepoint for everything else. Nothing consumes the ids yet — the value ABI (next plan) and eval package C are the consumers.

**Tech Stack:** C++11, tabs, doctest (`tests/unit/test_datadef.cpp`, linked with parser.o via `TESTOBJ`). Repo rules apply: capped test runs, `make -C src test` is the only target that relinks `bin/test_*`, commit messages via `git commit -F tmp/msg.txt`.

**Branch:** `feature/type-table-claude` off `develop`.

**Design-doc contract being implemented (§2, §6 phase 1):** primitive-slot enum as ABI constants; `DataDef::type_id` (0 = unregistered); project-segment `vector<DataDef*>` on `Program` (NOT by-value — DataDef is polymorphic); id↔`DataDef*` lookup; doctests pin slot values like `test_mangle` pins manglings. Reserved-but-unbacked slots (`__int128`, `long double`, `_Complex`) return NULL until P0 lands.

**Known wrinkle (document, don't fix here):** lazy stamping is per-Program. All extern `dd*` globals in `include/datadef.h:996-1039` get fixed slots so no *global* is ever lazily stamped into one Program's project segment. Legacy globals on the retirement path (e.g. `ddTESTSTRUCT`, test-only) deliberately get NO slot — `type_from_id` returns NULL defensively for foreign ids.

---

### Task 0: Branch + baseline

**Files:** none (git/build only)

- [ ] **Step 0.1: Create the feature branch**

```bash
git -C /workspace/madc checkout -b feature/type-table-claude
```

- [ ] **Step 0.2: Baseline build (bin/madc was stale at session start)**

Run: `( ulimit -t 600; timeout 600 make -C src )`
Expected: clean build, **zero warnings**, `bin/madc` relinked.

- [ ] **Step 0.3: Baseline unit tests**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: all doctest binaries green (10 binaries; `test_libmadc_program` reports 97 passed / 38 skipped).

---

### Task 1: `include/madc_typeid.h` — segmented id space + primitive slots (ABI)

**Files:**
- Create: `include/madc_typeid.h`
- Test: `tests/unit/test_datadef.cpp` (append a new TEST_SUITE at end of file)

- [ ] **Step 1.1: Write the failing test** — append to `tests/unit/test_datadef.cpp`:

```cpp
TEST_SUITE("type table (typeid) identity layer") {

// The slot numbers are ABI (design doc §7): once eval package C ships,
// these constants are frozen, exactly like the manglings in test_mangle.
TEST_CASE("primitive slot constants are pinned ABI") {
	CHECK(MADC_TYPEID_INVALID == 0);
	CHECK(MADC_TYPEID_VOID == 1);
	CHECK(MADC_TYPEID_VOID_REF == 2);
	CHECK(MADC_TYPEID_BOOL == 3);
	CHECK(MADC_TYPEID_CHAR == 4);
	CHECK(MADC_TYPEID_INT == 5);
	CHECK(MADC_TYPEID_INT8 == 6);
	CHECK(MADC_TYPEID_INT16 == 7);
	CHECK(MADC_TYPEID_INT24 == 8);
	CHECK(MADC_TYPEID_INT32 == 9);
	CHECK(MADC_TYPEID_INT64 == 10);
	CHECK(MADC_TYPEID_UINT8 == 11);
	CHECK(MADC_TYPEID_UINT16 == 12);
	CHECK(MADC_TYPEID_UINT24 == 13);
	CHECK(MADC_TYPEID_UINT32 == 14);
	CHECK(MADC_TYPEID_UINT64 == 15);
	CHECK(MADC_TYPEID_FLOAT == 16);
	CHECK(MADC_TYPEID_DOUBLE == 17);
	CHECK(MADC_TYPEID_LONG_DOUBLE == 18);
	CHECK(MADC_TYPEID_INT128 == 19);
	CHECK(MADC_TYPEID_UINT128 == 20);
	CHECK(MADC_TYPEID_COMPLEX_FLOAT == 21);
	CHECK(MADC_TYPEID_COMPLEX_DOUBLE == 22);
	CHECK(MADC_TYPEID_MAX_ALIGN_T == 23);
	CHECK(MADC_TYPEID_LPSTR == 24);
	CHECK(MADC_TYPEID_VOID_PTR == 25);
	CHECK(MADC_TYPEID_CHAR_PTR == 26);
	CHECK(MADC_TYPEID_INT_PTR == 27);
	CHECK(MADC_TYPEID_INT32_PTR == 28);
	CHECK(MADC_TYPEID_ARRAY == 29);
	CHECK(MADC_TYPEID_AUTO == 30);
	CHECK(MADC_TYPEID_PRIMITIVE_LAST == 30);
	CHECK(MADC_TYPEID_PRIMITIVE_LAST < MADC_TYPEID_PRIMITIVE_END);
	CHECK(MADC_TYPEID_PRIMITIVE_END == 100);
	CHECK(MADC_TYPEID_SYSTEM_BASE == 100);
	CHECK(MADC_TYPEID_PROJECT_BASE == 0x01000000u);
}

} // TEST_SUITE
```

- [ ] **Step 1.2: Run to verify it fails**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: COMPILE ERROR — `MADC_TYPEID_INVALID` undeclared (the constants don't exist yet).

- [ ] **Step 1.3: Create `include/madc_typeid.h`** (pure C — it will be consumed by `madc_api.h` in the value-ABI plan):

```c
#ifndef __MADC_TYPEID_H
#define __MADC_TYPEID_H 1

/*
 * Canonical type identity: uint32 typeid into the segmented type table.
 * Design: docs/plans/2026-06-12-type-table-value-abi-design.md §2.
 *
 * Segments:
 *   0                       invalid / "no type" sentinel
 *   [1, 100)                primitives — pinned ABI slots (this enum)
 *   [100, 0x01000000)       system segment — embedded-forest types,
 *                           frozen at forest build time (reserved; no
 *                           occupants until the forest lands)
 *   [0x01000000, 2^32)      project segment — per-Program user types,
 *                           lazy-stamped via Program::type_id_for()
 *
 * ABI: once eval package C ships, slot numbers and segment bases are
 * frozen (pinned in tests/unit/test_datadef.cpp). Append-only — new
 * primitives take the next free slot below MADC_TYPEID_PRIMITIVE_END;
 * never renumber (same discipline as the token-kind enum tail rule).
 * Slots 18-22 are reserved ahead for the P0 wide-value work
 * (__int128 / _BitInt / long double / _Complex); they have no backing
 * DataDef yet and madc_primitive_for_slot() returns NULL for them.
 */
enum
{
    MADC_TYPEID_INVALID        = 0,
    MADC_TYPEID_VOID           = 1,
    MADC_TYPEID_VOID_REF       = 2,
    MADC_TYPEID_BOOL           = 3,
    MADC_TYPEID_CHAR           = 4,
    MADC_TYPEID_INT            = 5,
    MADC_TYPEID_INT8           = 6,
    MADC_TYPEID_INT16          = 7,
    MADC_TYPEID_INT24          = 8,
    MADC_TYPEID_INT32          = 9,
    MADC_TYPEID_INT64          = 10,
    MADC_TYPEID_UINT8          = 11,
    MADC_TYPEID_UINT16         = 12,
    MADC_TYPEID_UINT24         = 13,
    MADC_TYPEID_UINT32         = 14,
    MADC_TYPEID_UINT64         = 15,
    MADC_TYPEID_FLOAT          = 16,
    MADC_TYPEID_DOUBLE         = 17,
    MADC_TYPEID_LONG_DOUBLE    = 18,  /* reserved: P0 wide-value work */
    MADC_TYPEID_INT128         = 19,  /* reserved: P0 */
    MADC_TYPEID_UINT128        = 20,  /* reserved: P0 */
    MADC_TYPEID_COMPLEX_FLOAT  = 21,  /* reserved: P0 */
    MADC_TYPEID_COMPLEX_DOUBLE = 22,  /* reserved: P0 */
    MADC_TYPEID_MAX_ALIGN_T    = 23,
    MADC_TYPEID_LPSTR          = 24,
    MADC_TYPEID_VOID_PTR       = 25,
    MADC_TYPEID_CHAR_PTR       = 26,
    MADC_TYPEID_INT_PTR        = 27,
    MADC_TYPEID_INT32_PTR      = 28,
    MADC_TYPEID_ARRAY          = 29,
    MADC_TYPEID_AUTO           = 30,
    MADC_TYPEID_PRIMITIVE_LAST = 30,

    MADC_TYPEID_PRIMITIVE_END  = 100,
    MADC_TYPEID_SYSTEM_BASE    = 100
};

#define MADC_TYPEID_PROJECT_BASE 0x01000000u

#endif /* __MADC_TYPEID_H */
```

- [ ] **Step 1.4: Include it from `include/datadef.h`** — add directly below the existing `#include` lines at the top of the file (datadef.h already uses `uint32_t`, so `<stdint.h>` ordering is safe):

```cpp
#include "madc_typeid.h"
```

- [ ] **Step 1.5: Run to verify it passes**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: PASS (all binaries green, new suite runs in `bin/test_datadef`).

- [ ] **Step 1.6: Commit**

```bash
git -C /workspace/madc add include/madc_typeid.h include/datadef.h tests/unit/test_datadef.cpp
printf 'feat(typeid): segmented typeid space + ABI-pinned primitive slots\n\nidentity layer task 1 of docs/superpowers/plans/2026-06-12-type-table-identity-layer.md\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>\n' > tmp/msg.txt
git -C /workspace/madc commit -F tmp/msg.txt
```

---

### Task 2: `DataDef::type_id` + primitive slot mapping + stamping

**Files:**
- Modify: `include/datadef.h` (DataDef class at :96-122; declarations after the extern `dd*` block at :996-1039)
- Modify: `src/parser.cpp` (implementations — place directly after `DataDef::same_representation`, find with `grep -n "bool DataDef::same_representation" src/parser.cpp`, ~:6339)
- Modify: `src/lexer.cpp` (`Program::add_datatypes()` at :1884)
- Test: `tests/unit/test_datadef.cpp`

- [ ] **Step 2.1: Write the failing test** — add inside the `TEST_SUITE` from Task 1:

```cpp
TEST_CASE("primitive stamping round-trips slot <-> global DataDef") {
	madc_stamp_primitive_type_ids();
	CHECK(ddVOID.type_id == MADC_TYPEID_VOID);
	CHECK(ddINT.type_id == MADC_TYPEID_INT);
	CHECK(ddUINT64.type_id == MADC_TYPEID_UINT64);
	CHECK(ddDOUBLE.type_id == MADC_TYPEID_DOUBLE);
	CHECK(ddCHARptr.type_id == MADC_TYPEID_CHAR_PTR);
	CHECK(madc_primitive_for_slot(MADC_TYPEID_INT) == &ddINT);
	CHECK(madc_primitive_for_slot(MADC_TYPEID_AUTO) == &ddAUTO);
	// every backed slot stamps to exactly its own number
	for ( uint32_t s = 1; s <= MADC_TYPEID_PRIMITIVE_LAST; ++s )
	{
		DataDef *dd = madc_primitive_for_slot(s);
		if ( dd )
			CHECK(dd->type_id == s);
	}
	// reserved-but-unbacked slots resolve NULL until P0 lands
	CHECK(madc_primitive_for_slot(MADC_TYPEID_INT128) == (DataDef *)NULL);
	CHECK(madc_primitive_for_slot(MADC_TYPEID_LONG_DOUBLE) == (DataDef *)NULL);
	// out-of-segment queries resolve NULL
	CHECK(madc_primitive_for_slot(MADC_TYPEID_INVALID) == (DataDef *)NULL);
	CHECK(madc_primitive_for_slot(MADC_TYPEID_PRIMITIVE_END) == (DataDef *)NULL);
}
```

- [ ] **Step 2.2: Run to verify it fails**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: COMPILE ERROR — `madc_stamp_primitive_type_ids` / `type_id` undeclared.

- [ ] **Step 2.3: Add the field to `DataDef`** in `include/datadef.h` — insert before the two constructors (currently `DataDef() { size = 0; _type = 0; }` / `DataDef(std::string n, size_t s, DataType d) {…}` at ~:121-122), and initialize it in BOTH:

```cpp
    // Canonical typeid: index into the segmented type table
    // (include/madc_typeid.h; design docs/plans/2026-06-12-type-table-
    // value-abi-design.md §2). 0 = not yet registered. Primitives carry
    // fixed ABI slots (stamped by madc_stamp_primitive_type_ids());
    // everything else is lazy-stamped per Program by type_id_for().
    uint32_t	 type_id;
    DataDef() { size = 0; _type = 0; type_id = 0; }
    DataDef(std::string n, size_t s, DataType d) { name = n; size = s; _type = (uint32_t)d; type_id = 0; }
```

(The two ctor lines REPLACE the existing ones — the only change is appending `type_id = 0;`.)

- [ ] **Step 2.4: Declare the free functions** in `include/datadef.h`, after the extern `dd*` block (below `extern DataDefAUTO ddAUTO;` at :1039):

```cpp
// Type table identity layer — slot <-> global-primitive mapping (the single
// source of truth; defined in src/parser.cpp next to same_representation).
DataDef *madc_primitive_for_slot(uint32_t slot);
void madc_stamp_primitive_type_ids();
```

- [ ] **Step 2.5: Implement in `src/parser.cpp`** directly after `DataDef::same_representation` (~:6339; tabs):

```cpp
// --- Type table (typeid) identity layer ----------------------------------
// docs/plans/2026-06-12-type-table-value-abi-design.md §2. Slot numbers are
// ABI (pinned in tests/unit/test_datadef.cpp). This switch is the single
// source of truth for slot -> global primitive; the stamping loop and
// Program::type_from_id() both derive from it. Reserved slots (P0 wide
// values: 18-22) return NULL until their DataDefs exist.
DataDef *madc_primitive_for_slot(uint32_t slot)
{
	switch ( slot )
	{
		case MADC_TYPEID_VOID:		return &ddVOID;
		case MADC_TYPEID_VOID_REF:	return &ddVOIDref;
		case MADC_TYPEID_BOOL:		return &ddBOOL;
		case MADC_TYPEID_CHAR:		return &ddCHAR;
		case MADC_TYPEID_INT:		return &ddINT;
		case MADC_TYPEID_INT8:		return &ddINT8;
		case MADC_TYPEID_INT16:		return &ddINT16;
		case MADC_TYPEID_INT24:		return &ddINT24;
		case MADC_TYPEID_INT32:		return &ddINT32;
		case MADC_TYPEID_INT64:		return &ddINT64;
		case MADC_TYPEID_UINT8:		return &ddUINT8;
		case MADC_TYPEID_UINT16:	return &ddUINT16;
		case MADC_TYPEID_UINT24:	return &ddUINT24;
		case MADC_TYPEID_UINT32:	return &ddUINT32;
		case MADC_TYPEID_UINT64:	return &ddUINT64;
		case MADC_TYPEID_FLOAT:		return &ddFLOAT;
		case MADC_TYPEID_DOUBLE:	return &ddDOUBLE;
		case MADC_TYPEID_MAX_ALIGN_T:	return &ddMAX_ALIGN_T;
		case MADC_TYPEID_LPSTR:		return &ddLPSTR;
		case MADC_TYPEID_VOID_PTR:	return &ddVOIDptr;
		case MADC_TYPEID_CHAR_PTR:	return &ddCHARptr;
		case MADC_TYPEID_INT_PTR:	return &ddINTptr;
		case MADC_TYPEID_INT32_PTR:	return &ddINT32ptr;
		case MADC_TYPEID_ARRAY:		return &ddARRAY;
		case MADC_TYPEID_AUTO:		return &ddAUTO;
		default:			return NULL;
	}
}

void madc_stamp_primitive_type_ids()
{
	for ( uint32_t slot = 1; slot <= MADC_TYPEID_PRIMITIVE_LAST; ++slot )
	{
		DataDef *dd = madc_primitive_for_slot(slot);

		if ( dd )
			dd->type_id = slot;
	}
}
```

- [ ] **Step 2.6: Stamp at startup** — in `src/lexer.cpp` `Program::add_datatypes()` (:1884), add as the FIRST line of the function body (idempotent — globals get the same constant every time, so multiple Programs per process are safe):

```cpp
    madc_stamp_primitive_type_ids();
```

- [ ] **Step 2.7: Run to verify it passes**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: PASS.

- [ ] **Step 2.8: Commit**

```bash
git -C /workspace/madc add include/datadef.h src/parser.cpp src/lexer.cpp tests/unit/test_datadef.cpp
printf 'feat(typeid): DataDef::type_id + primitive slot mapping and stamping\n\nidentity layer task 2 of docs/superpowers/plans/2026-06-12-type-table-identity-layer.md\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>\n' > tmp/msg.txt
git -C /workspace/madc commit -F tmp/msg.txt
```

---

### Task 3: `Program` project segment — `type_id_for()` / `type_from_id()`

**Files:**
- Modify: `include/madc.h` (Program class — add next to `datadef_map` at :1134)
- Modify: `src/parser.cpp` (implementations, directly after `madc_stamp_primitive_type_ids` from Task 2)
- Test: `tests/unit/test_datadef.cpp`

- [ ] **Step 3.1: Write the failing test** — add inside the same TEST_SUITE:

```cpp
TEST_CASE("project segment: lazy stamp, memoization, round trip") {
	Program pgm;
	DataDef a("UserTypeA", 4, DataType::dtINT);
	DataDef b("UserTypeB", 8, DataType::dtINT64);

	CHECK(a.type_id == 0);				// unregistered until asked
	uint32_t ida = pgm.type_id_for(&a);
	CHECK(ida == MADC_TYPEID_PROJECT_BASE);		// first project id
	CHECK(pgm.type_id_for(&a) == ida);		// memoized via the stamp
	CHECK(pgm.type_from_id(ida) == &a);		// round trip
	CHECK(pgm.type_id_for(&b) == MADC_TYPEID_PROJECT_BASE + 1);
	CHECK(pgm.type_from_id(MADC_TYPEID_PROJECT_BASE + 1) == &b);

	// primitives resolve through the slot table
	madc_stamp_primitive_type_ids();
	CHECK(pgm.type_from_id(MADC_TYPEID_CHAR) == &ddCHAR);
	CHECK(pgm.type_id_for(&ddCHAR) == MADC_TYPEID_CHAR);	// no re-stamp

	// defensive NULLs: invalid, reserved system segment, foreign/unknown ids
	CHECK(pgm.type_from_id(MADC_TYPEID_INVALID) == (DataDef *)NULL);
	CHECK(pgm.type_from_id(MADC_TYPEID_SYSTEM_BASE + 5) == (DataDef *)NULL);
	CHECK(pgm.type_from_id(MADC_TYPEID_PROJECT_BASE + 99) == (DataDef *)NULL);
	CHECK(pgm.type_id_for((DataDef *)NULL) == MADC_TYPEID_INVALID);
}
```

- [ ] **Step 3.2: Run to verify it fails**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: COMPILE ERROR — `type_id_for` is not a member of `Program`.
(If `Program pgm;` itself fails to construct standalone in this binary, fall back to the engine pattern already used in this file at :240: `MadcEngine engine; std::unique_ptr<Program> prog = engine.create_program();` and call through `prog->`.)

- [ ] **Step 3.3: Add the member + declarations** in `include/madc.h` next to `datadef_map` (:1134):

```cpp
    // Type table identity layer — project segment (design doc §2). Holds
    // every non-primitive DataDef this Program has been asked an id for;
    // index i <=> typeid MADC_TYPEID_PROJECT_BASE + i. Lazy registration
    // order is ask order (deterministic per compilation). Pointers, NOT
    // values: DataDef is polymorphic and ids must survive growth.
    std::vector<DataDef *> project_types;
    uint32_t type_id_for(DataDef *dd);	// THE lazy-stamp chokepoint
    DataDef *type_from_id(uint32_t id);	// segment-dispatching reverse lookup
```

- [ ] **Step 3.4: Implement in `src/parser.cpp`** after `madc_stamp_primitive_type_ids`:

```cpp
uint32_t Program::type_id_for(DataDef *dd)
{
	if ( !dd )
		return MADC_TYPEID_INVALID;
	if ( dd->type_id )
		return dd->type_id;
	project_types.push_back(dd);
	dd->type_id = MADC_TYPEID_PROJECT_BASE + (uint32_t)(project_types.size() - 1);
	return dd->type_id;
}

DataDef *Program::type_from_id(uint32_t id)
{
	if ( id == MADC_TYPEID_INVALID )
		return NULL;
	if ( id < MADC_TYPEID_PRIMITIVE_END )
		return madc_primitive_for_slot(id);
	if ( id >= MADC_TYPEID_PROJECT_BASE )
	{
		uint32_t idx = id - MADC_TYPEID_PROJECT_BASE;

		if ( idx < project_types.size() )
			return project_types[idx];
		return NULL;	// foreign Program's id, or never registered
	}
	return NULL;	// system segment: reserved for the embedded forest
}
```

- [ ] **Step 3.5: Run to verify it passes**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: PASS.

- [ ] **Step 3.6: Commit**

```bash
git -C /workspace/madc add include/madc.h src/parser.cpp tests/unit/test_datadef.cpp
printf 'feat(typeid): Program project segment — type_id_for/type_from_id\n\nidentity layer task 3 of docs/superpowers/plans/2026-06-12-type-table-identity-layer.md\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>\n' > tmp/msg.txt
git -C /workspace/madc commit -F tmp/msg.txt
```

---

### Task 4: Full gates + doc sync

**Files:**
- Modify: `docs/plans/2026-06-12-type-table-value-abi-design.md` (status line)

- [ ] **Step 4.1: Clean rebuild, zero warnings**

Run: `make -C src clean` then `( ulimit -t 600; timeout 600 make -C src )`
Expected: clean build, **zero warnings** (new-warning check is a PR gate).

- [ ] **Step 4.2: Unit tests**

Run: `( ulimit -t 600; timeout 900 make -C src test )`
Expected: all green.

- [ ] **Step 4.3: Full integration suite** (ONE heavy job at a time)

Run: `( ulimit -t 1200; timeout 1800 make -C src fulltest )`
Expected: **577 passed / 0 failed / 0 timed out / 18 skipped, exit 0**, both check gates GREEN (no-std-hardcoding, call-emit-symbol). This change adds dormant fields/functions — ANY deviation from the baseline is a regression; stop and investigate per systematic-debugging.

- [ ] **Step 4.4: Update the design doc status** — in `docs/plans/2026-06-12-type-table-value-abi-design.md`, change the `**Status:**` line to record: identity layer (§6 phase 1) LANDED on `feature/type-table-claude` with this plan; value ABI (§6 phase 2) is next.

- [ ] **Step 4.5: Commit doc sync**

```bash
git -C /workspace/madc add docs/plans/2026-06-12-type-table-value-abi-design.md
printf 'docs(typeid): identity layer landed — design doc status sync\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>\n' > tmp/msg.txt
git -C /workspace/madc commit -F tmp/msg.txt
```

- [ ] **Step 4.6: Hand off** — report branch, validation results, and stop for user verification before any merge to develop (repo custom: user verifies feature branches; then KG/status mirrors sync at merge time).

---

## Self-review notes

- **Spec coverage vs design doc §6 phase 1:** primitive-slot enum ✓ (Task 1), `DataDef::type_id` stamped at chokepoints ✓ (Task 2: globals at `add_datatypes`; Task 3: everything else via the single lazy chokepoint `type_id_for` — deliberately NOT eager edits at the ~15 `struct_map` sites, the lazy accessor IS the chokepoint), project-segment vector ✓ (Task 3), id↔DataDef lookup ✓ (Task 3), doctests pin slots ✓ (Task 1). **Derived-type memo API is deliberately deferred** to the tag-arithmetic campaign (design §6 bullet 4): until consumers exist, pointer/ref DataDefs that reach `type_id_for` get ordinary project ids — correct and sufficient for the value ABI + package C.
- **Zero behavior change:** nothing reads `type_id` yet; the only executed new code is the stamping loop in `add_datatypes`.
- **Type consistency:** `type_id_for(DataDef*) -> uint32_t`, `type_from_id(uint32_t) -> DataDef*`, `madc_primitive_for_slot(uint32_t) -> DataDef*` used identically across Tasks 2-4.
