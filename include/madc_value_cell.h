#ifndef __MADC_VALUE_CELL_H
#define __MADC_VALUE_CELL_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Refcounted heap cell backing madc_value pointer payloads
 * (docs/plans/2026-06-12-type-table-value-abi-design.md §3; the header
 * shape is shared with madcdis's pool value_header — that is its
 * pool-resident superset). Refcounts are non-atomic (single-threaded
 * script execution per engine; fork isolation gives CoW pages) and
 * SATURATING: a cell that reaches MADC_CELL_PERMANENT is never
 * decremented or freed (the literal/schema tier — SMAUG hashstr
 * discipline). The refcount lives in the cell, never in madc_value —
 * struct copies are independent and inline counts would desync. */
typedef struct madc_cell
{
    uint32_t refcount;
    uint32_t cell_flags;   /* reserved: permanent/interned/frozen/hash-present */
    /* Payload finalizer, run exactly once when the refcount reaches 0,
     * before the cell is freed. NULL for plain byte payloads (text,
     * bytes). A typed-instance cell carries the instance type's own
     * destructor here (e.g. a JIT-resolved class dtor thunk) — the cell
     * never knows WHAT it holds, only how to let it go. */
    void (*destroy)(void *payload);
} madc_cell;

#define MADC_CELL_PERMANENT 0xFFFFFFFFu

/* Allocate a cell with payload_size payload bytes (zeroed), refcount 1.
 * Returns the PAYLOAD pointer (the cell header sits immediately before
 * it); NULL on allocation failure. */
void *madc_cell_alloc(size_t payload_size);
/* As madc_cell_alloc, with a payload finalizer (may be NULL). */
void *madc_cell_alloc_dtor(size_t payload_size, void (*destroy)(void *payload));
/* Payload pointer -> its cell header. */
madc_cell *madc_cell_of(void *payload);
void madc_cell_retain(void *payload);
void madc_cell_release(void *payload);   /* frees at 0; saturated never frees */
uint32_t madc_cell_refcount(const void *payload);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __MADC_VALUE_CELL_H */
