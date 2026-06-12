# 32-byte `madc_value` ABI (Phase 2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline — user decision 2026-06-12: no implementation subagents under Fable). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Implement §3+§4 of `docs/plans/2026-06-12-type-table-value-abi-design.md`: replace the flat 40-byte `madc_value` in `include/madc_api.h` with the 32-byte typeid struct (SSO, refcounted cells, gradual-typing flags), keeping every existing test green. Eval package C builds on this.

**Architecture:** `madc_typeid.h` gains the dynamic-kind slots (TEXT/BYTES/OBJECT). `madc_api.h` gets the new struct + `MADC_VF_*` flags; the old `madc_value_kind` enum constants become *aliases for typeid slots* (one vocabulary — old comparisons still compile). A tiny cell runtime (`include/madc_value_cell.h` + `src/madc_value.cpp`) owns retain/release with saturating refcounts. `src/madc_c_api.cpp` helpers and the `madc::value` bridges are rewritten on the new layout; the C++ `madc::value` class is untouched (wrapper conversion is a later phase, per the design's A0 guardrail).

**Design refinement made here (sync to design doc in Task 5):** SSO threshold is **15 bytes + NUL** (not 16) so inline text is always a valid C string; cell-backed text is NUL-terminated too. `madc_value_text()` gives uniform read access.

**Branch:** `feature/value-abi-claude` off `develop`.

---

### Task 1: Dynamic-kind slots (TEXT=31, BYTES=32, OBJECT=33)

**Files:** Modify `include/madc_typeid.h`, `tests/unit/test_datadef.cpp`

- [ ] **1.1 Failing test** — extend the `"primitive slot constants are pinned ABI"` TEST_CASE (before the PRIMITIVE_LAST checks) and fix the LAST pin:

```cpp
        CHECK(MADC_TYPEID_TEXT == 31);
        CHECK(MADC_TYPEID_BYTES == 32);
        CHECK(MADC_TYPEID_OBJECT == 33);
        CHECK(MADC_TYPEID_PRIMITIVE_LAST == 33);   // was 30
```

Also extend the stamping TEST_CASE: dynamic kinds have no DataDef —

```cpp
        CHECK(madc_primitive_for_slot(MADC_TYPEID_TEXT) == (DataDef *)NULL);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_OBJECT) == (DataDef *)NULL);
```

- [ ] **1.2 Run** `( ulimit -t 600; timeout 900 make -C src test )` — expect compile error (undeclared).
- [ ] **1.3 Implement** — in `include/madc_typeid.h` after `MADC_TYPEID_AUTO = 30,`:

```c
    /* Dynamic value kinds (the madc_value ABI): payload kinds that have no
     * compiler DataDef — owned text, raw bytes, and the string-keyed object.
     * The dynamic array kind reuses MADC_TYPEID_ARRAY (ddARRAY, slot 29). */
    MADC_TYPEID_TEXT           = 31,
    MADC_TYPEID_BYTES          = 32,
    MADC_TYPEID_OBJECT         = 33,
    MADC_TYPEID_PRIMITIVE_LAST = 33,
```

(delete the old `MADC_TYPEID_PRIMITIVE_LAST = 30,` line)
- [ ] **1.4 Run** — expect PASS. **1.5 Commit** `feat(typeid): dynamic-kind slots TEXT/BYTES/OBJECT` (via `git commit -F tmp/msg.txt`).

---

### Task 2: New struct + flags in `madc_api.h`

**Files:** Modify `include/madc_api.h:21-38`; Test: `tests/unit/test_libmadc_value.cpp` (append)

- [ ] **2.1 Failing test** — append to `tests/unit/test_libmadc_value.cpp`:

```cpp
TEST_SUITE("madc_value 32-byte ABI") {
    TEST_CASE("layout is pinned ABI") {
        CHECK(sizeof(madc_value) == 32);
        CHECK(alignof(madc_value) == 16);
        CHECK(offsetof(madc_value, type_id) == 0);
        CHECK(offsetof(madc_value, flags) == 4);
        CHECK(offsetof(madc_value, size) == 8);
        CHECK(MADC_VALUE_NULL == MADC_TYPEID_INVALID);
        CHECK(MADC_VALUE_BOOLEAN == MADC_TYPEID_BOOL);
        CHECK(MADC_VALUE_INTEGER == MADC_TYPEID_INT64);
        CHECK(MADC_VALUE_REAL == MADC_TYPEID_DOUBLE);
        CHECK(MADC_VALUE_STRING == MADC_TYPEID_TEXT);
        CHECK(MADC_VF_HEAP == 1u);
        CHECK(MADC_VF_INLINE_TEXT == 2u);
        CHECK(MADC_VF_TYPE_LOCKED == 4u);
        CHECK(MADC_VF_TYPE_COERCE == 8u);
        CHECK(MADC_VF_NULLABLE == 16u);
        CHECK(MADC_VF_CONST == 32u);
    }
}
```

(add `#include <cstddef>` for `offsetof` if missing)
- [ ] **2.2 Run** — expect compile error. 
- [ ] **2.3 Implement** — in `include/madc_api.h`: add `#include "madc_typeid.h"` next to the existing includes; REPLACE the `madc_value_kind` enum and `madc_value` struct (lines 21-38) with:

```c
/* Value kinds are typeid slots (madc_typeid.h) — one vocabulary. The old
 * MADC_VALUE_* names are aliases kept for readability at call sites. */
typedef uint32_t madc_value_kind;
#define MADC_VALUE_NULL    ((madc_value_kind)MADC_TYPEID_INVALID)
#define MADC_VALUE_BOOLEAN ((madc_value_kind)MADC_TYPEID_BOOL)
#define MADC_VALUE_INTEGER ((madc_value_kind)MADC_TYPEID_INT64)
#define MADC_VALUE_REAL    ((madc_value_kind)MADC_TYPEID_DOUBLE)
#define MADC_VALUE_STRING  ((madc_value_kind)MADC_TYPEID_TEXT)

/* madc_value.flags bits. Storage discriminators + gradual typing
 * (design doc §4). All other bits reserved-zero. */
enum
{
    MADC_VF_HEAP        = 1u << 0,  /* payload is a refcounted cell */
    MADC_VF_INLINE_TEXT = 1u << 1,  /* payload is SSO inline_text */
    MADC_VF_TYPE_LOCKED = 1u << 2,  /* re-tag is an error */
    MADC_VF_TYPE_COERCE = 1u << 3,  /* assignments convert to current type_id */
    MADC_VF_NULLABLE    = 1u << 4,  /* null ok even when LOCKED/COERCE */
    MADC_VF_CONST       = 1u << 5   /* value is read-only */
};

/* The 32-byte interchange value (design doc §3). type_id is the canonical
 * type identity; size is per-kind (text/bytes length, array count, struct
 * byte size, sizeof for scalars); the 16-byte payload inlines every madc
 * primitive. Text: <= 15 bytes lives in inline_text NUL-terminated
 * (MADC_VF_INLINE_TEXT); longer text lives in a NUL-terminated refcounted
 * cell (MADC_VF_HEAP). Copy with madc_value_copy (retains), release with
 * madc_value_clear. */
typedef struct __attribute__((aligned(16))) madc_value
{
    uint32_t type_id;
    uint32_t flags;
    uint64_t size;
    union
    {
	int64_t  integer_value;   /* bool folds in; type_id distinguishes */
	double   real_value;
	char    *text_value;      /* cell payload when MADC_VF_HEAP */
	void    *data_ptr;        /* array/object/oversize-struct cell */
	char     inline_text[16]; /* SSO when MADC_VF_INLINE_TEXT */
	uint64_t wide_value[2];   /* __int128/_Complex/v128 (P0) */
    };
} madc_value;
```

- [ ] **2.4 Run** — `make -C src test` will now FAIL TO COMPILE `madc_c_api.cpp` (`.kind`, `.boolean_value`, `.text_length` gone). That is the Task 3+4 work — to keep this task commit-green, do Task 2 and Tasks 3-4 as ONE commit if needed; preferred order: proceed straight into Tasks 3-4 and commit when green. (Plan deviation note: struct + consumers are one atomic change set.)

---

### Task 3: Cell runtime (`madc_value_cell`)

**Files:** Create `include/madc_value_cell.h`; Modify `src/madc_value.cpp` (append); Test `tests/unit/test_libmadc_value.cpp`

- [ ] **3.1 Header** `include/madc_value_cell.h`:

```c
#ifndef __MADC_VALUE_CELL_H
#define __MADC_VALUE_CELL_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Refcounted heap cell backing madc_value pointer payloads (design doc §3,
 * shared header shape with madcdis value_header — pool-resident superset).
 * Refcounts are non-atomic (single-threaded script execution per engine)
 * and SATURATING: a cell that reaches MADC_CELL_PERMANENT is never
 * decremented or freed (literal/schema tier). The refcount lives in the
 * cell, never in madc_value — struct copies are independent. */
typedef struct madc_cell
{
    uint32_t refcount;
    uint32_t cell_flags;   /* reserved: permanent/interned/frozen/hash-present */
} madc_cell;

#define MADC_CELL_PERMANENT 0xFFFFFFFFu

/* Allocate a cell with payload_size payload bytes (zeroed), refcount 1.
 * Returns the PAYLOAD pointer (cell header sits immediately before it);
 * NULL on allocation failure. */
void *madc_cell_alloc(size_t payload_size);
/* Payload pointer -> its cell header. */
madc_cell *madc_cell_of(void *payload);
void madc_cell_retain(void *payload);
void madc_cell_release(void *payload);   /* frees at 0; saturated never frees */
uint32_t madc_cell_refcount(const void *payload);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __MADC_VALUE_CELL_H */
```

- [ ] **3.2 Implementation** — append to `src/madc_value.cpp`:

```cpp
// --- madc_value cell runtime (refcounted payload cells) -------------------
// docs/plans/2026-06-12-type-table-value-abi-design.md §3.
#include "madc_value_cell.h"
#include <cstdlib>
#include <cstring>

extern "C" {

void *madc_cell_alloc(size_t payload_size)
{
	madc_cell *cell = static_cast<madc_cell *>(std::malloc(sizeof(madc_cell) + payload_size));

	if ( cell == NULL )
		return NULL;
	cell->refcount = 1;
	cell->cell_flags = 0;
	std::memset(cell + 1, 0, payload_size);
	return cell + 1;
}

madc_cell *madc_cell_of(void *payload)
{
	if ( payload == NULL )
		return NULL;
	return static_cast<madc_cell *>(payload) - 1;
}

void madc_cell_retain(void *payload)
{
	madc_cell *cell = madc_cell_of(payload);

	if ( cell == NULL || cell->refcount == MADC_CELL_PERMANENT )
		return;
	if ( cell->refcount == MADC_CELL_PERMANENT - 1 )
		cell->refcount = MADC_CELL_PERMANENT;	// saturate
	else
		++cell->refcount;
}

void madc_cell_release(void *payload)
{
	madc_cell *cell = madc_cell_of(payload);

	if ( cell == NULL || cell->refcount == MADC_CELL_PERMANENT )
		return;
	if ( --cell->refcount == 0 )
		std::free(cell);
}

uint32_t madc_cell_refcount(const void *payload)
{
	if ( payload == NULL )
		return 0;
	return (static_cast<const madc_cell *>(payload) - 1)->refcount;
}

} // extern "C"
```

- [ ] **3.3 Tests** — append a TEST_CASE to the Task-2 suite:

```cpp
    TEST_CASE("cell runtime: retain/release/saturation") {
        void *p = madc_cell_alloc(8);
        REQUIRE(p != (void *)NULL);
        CHECK(madc_cell_refcount(p) == 1);
        madc_cell_retain(p);
        CHECK(madc_cell_refcount(p) == 2);
        madc_cell_release(p);
        CHECK(madc_cell_refcount(p) == 1);
        madc_cell_release(p);                    // frees; do not touch p after
        void *q = madc_cell_alloc(4);
        madc_cell_of(q)->refcount = MADC_CELL_PERMANENT - 1;
        madc_cell_retain(q);                     // saturates
        CHECK(madc_cell_refcount(q) == MADC_CELL_PERMANENT);
        madc_cell_release(q);                    // permanent: no-op, no free
        CHECK(madc_cell_refcount(q) == MADC_CELL_PERMANENT);
        std::free(madc_cell_of(q));              // test cleanup of permanent cell
    }
```

(test file needs `#include "madc_value_cell.h"` and `#include <cstdlib>`)

---

### Task 4: Rewrite C-API helpers + bridges on the new layout

**Files:** Modify `src/madc_c_api.cpp` (`clear_c_value` :28, `set_c_string` :42, `from_cpp_value` :76, `to_cpp_value` :111, the `madc_value_*` helpers :1117-1230, `madc_value_kind_name` :1397); `include/madc_api.h` (add `madc_value_copy` + `madc_value_text` decls); tests (`test_libmadc_program.cpp` `.kind` → `.type_id` at ~:1698/:1703; any `.text_length` → `.size`).

Core rules (each helper follows them):
- **clear** releases the cell iff `MADC_VF_HEAP`, then zeroes the struct (preserving NO flags — clear is full reset; lock state is the *variable owner's* to reapply… EXCEPT: `madc_value_clear` keeps the four semantic flag bits (LOCKED/COERCE/NULLABLE/CONST) and `type_id` when LOCKED/COERCE, so a locked variable stays locked with a null payload — `size=0`, union zeroed, HEAP/INLINE cleared).
- **set_X** with kind `k`: if CONST → `MADC_ERROR`. If LOCKED and `type_id != k`: for `set_null`, allowed iff NULLABLE; numeric COERCE converts int↔real↔bool toward the existing `type_id`; otherwise `MADC_ERROR`. Unflagged values re-tag freely.
- **set_string_n**: release old payload; `len <= 15` → `inline_text` + NUL, `flags |= MADC_VF_INLINE_TEXT`, `size = len`; else `madc_cell_alloc(len+1)`, copy + NUL, `text_value = payload`, `flags |= MADC_VF_HEAP`, `size = len`; `type_id = MADC_TYPEID_TEXT`.
- **madc_value_copy(dst, src)**: clear dst, struct-assign, then `madc_cell_retain` iff HEAP.
- **madc_value_text(v, &len)**: returns `inline_text` / cell text / NULL by flags; len out-param optional.
- **scalars**: `set_bool` → `type_id = MADC_TYPEID_BOOL`, `integer_value = !!b`, `size = 1`; `set_integer` → INT64/`size = 8`; `set_real` → DOUBLE/`size = 8`; `set_null` → `type_id = MADC_TYPEID_INVALID` (subject to flag rules), union zeroed.
- **bridges**: same five kinds as today (null/bool/int/real/string) — bytes/array/object stay package-C scope; `to_cpp_value` reads text via `madc_value_text`.
- **kind_name**: switch over MADC_VALUE_NULL/BOOLEAN/INTEGER/REAL/STRING returning the same strings as today, default `"unknown"`.

- [ ] **4.1** Implement all of the above (the failing state was established by Task 2.4).
- [ ] **4.2** Update test field refs (`.kind` → `.type_id`, `.text_length` → `.size`) — semantics of every existing CHECK preserved.
- [ ] **4.3** Add behavior tests to the Task-2 suite: SSO vs cell (16-byte string goes to cell, 15 inline), copy shares the cell (`refcount == 2`, text pointer equal), clear releases, LOCKED rejects cross-domain set (`MADC_ERROR`), LOCKED+NULLABLE allows `set_null`, COERCE converts `set_real` on an INT64-locked value to integer storage… concretely:

```cpp
    TEST_CASE("string SSO vs cell + copy/clear refcounts") {
        madc_value v; madc_value_init(&v);
        REQUIRE(madc_value_set_string(&v, "short") == MADC_OK);   // 5 -> SSO
        CHECK((v.flags & MADC_VF_INLINE_TEXT) != 0);
        CHECK(v.size == 5);
        REQUIRE(madc_value_set_string(&v, "exactly16bytes!!") == MADC_OK); // 16 -> cell
        CHECK((v.flags & MADC_VF_HEAP) != 0);
        CHECK(madc_cell_refcount(v.text_value) == 1);
        madc_value c; madc_value_init(&c);
        REQUIRE(madc_value_copy(&c, &v) == MADC_OK);
        CHECK(c.text_value == v.text_value);
        CHECK(madc_cell_refcount(v.text_value) == 2);
        madc_value_clear(&c);
        CHECK(madc_cell_refcount(v.text_value) == 1);
        madc_value_clear(&v);
    }
    TEST_CASE("gradual-typing flags on set helpers") {
        madc_value v; madc_value_init(&v);
        REQUIRE(madc_value_set_integer(&v, 5) == MADC_OK);
        v.flags |= MADC_VF_TYPE_LOCKED;
        CHECK(madc_value_set_string(&v, "no") == MADC_ERROR);     // cross-domain
        CHECK(madc_value_set_null(&v) == MADC_ERROR);             // not nullable
        v.flags |= MADC_VF_NULLABLE;
        CHECK(madc_value_set_null(&v) == MADC_OK);                // ?int accepts null
        CHECK(v.type_id == MADC_VALUE_INTEGER);                   // lock keeps domain
        madc_value w; madc_value_init(&w);
        REQUIRE(madc_value_set_integer(&w, 1) == MADC_OK);
        w.flags |= MADC_VF_TYPE_COERCE;
        CHECK(madc_value_set_real(&w, 2.9) == MADC_OK);           // converts toward INT64
        CHECK(w.type_id == MADC_VALUE_INTEGER);
        CHECK(w.integer_value == 2);
        madc_value_clear(&v); madc_value_clear(&w);
    }
```

- [ ] **4.4 Run** `( ulimit -t 600; timeout 900 make -C src test )` — expect 10/10 SUCCESS, zero warnings.
- [ ] **4.5 Commit** Tasks 2+3+4 as one atomic change: `feat(value-abi): 32-byte typeid madc_value — SSO, refcounted cells, gradual-typing flags`.

---

### Task 5: Full gates + doc sync

- [ ] **5.1** `make -C src clean` + capped `make -C src` → zero warnings.
- [ ] **5.2** `( ulimit -t 1200; timeout 1800 make -C src fulltest )` → **577/0/0/18 exit 0**, both gates GREEN.
- [ ] **5.3** Sync design doc (§3 SSO threshold refinement 15+NUL; status: phase 2 implemented), CHANGELOG [Unreleased] append, claude_status, KG, memory.
- [ ] **5.4** Hand off for user verification (no merge without sign-off).
