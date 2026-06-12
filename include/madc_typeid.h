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
 * frozen (pinned in tests/unit/test_datadef.cpp). Append-only — a new
 * primitive takes the next free slot below MADC_TYPEID_PRIMITIVE_END;
 * never renumber (same discipline as the token-kind enum tail rule).
 * Slots 18-22 are reserved ahead for the P0 wide-value work
 * (__int128 / _BitInt / long double / _Complex); they have no backing
 * DataDef yet and madc_primitive_for_slot() returns NULL for them.
 *
 * Pure C header: consumed by the compiler core (datadef.h) and, with
 * the value ABI, by the C embedding API (madc_api.h).
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
