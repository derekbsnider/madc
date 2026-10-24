///////////////////////////////////////////////////////////////////////////
//                                                                       //
// The unwind personality — the routine every generated function with   //
// cleanup regions names: __madc_personality, the DWARF personality, or  //
// on Windows x64 __madc_seh_handler, the language-specific handler.     //
// Both run the frame's region table                                     //
// (docs/plans/2026-10-19-table-driven-unwinding.md).                    //
//                                                                       //
// It plays __gxx_personality_v0's role for g++, and like libgcc's       //
// personality it rides every image and object that names it:            //
// madc_crt.cpp carries this file's text (rt_unwind.h inlined,           //
// MADC_RT_IMAGE defined) as a startup member pulled for -o and -c       //
// output, so a program whose only runtime need is its destructors'      //
// unwinding needs no madc library and its object links with plain g++.  //
// The host build compiles it into libmadc, whose copy the JIT uses.     //
// Strict C11 with no system header but <stddef.h> / <stdint.h>: c2mir   //
// compiles it too.                                                      //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stddef.h>
#include <stdint.h>

#include "rt_unwind.h"

// c2mir's own <stddef.h> for Windows leaves NULL out.
#ifndef NULL
#define NULL ((void *)0)
#endif

void *memcpy(void *, const void *, size_t);

// What the personality reads from the throw side (rt_except.c). In an image
// a program that never throws carries no exception runtime, so there the
// references are weak and null without it (ld's undefined weak); a program
// that throws imports the same names, and they bind to its runtime. The
// image's definitions are linkonce, as every startup member's: each object
// carries one, and the link keeps one.
#ifdef MADC_RT_IMAGE
#define MADC_RT_THROW_SIDE __attribute__((weak))
#define MADC_RT_LINKED(fn) ((fn) != 0)
#define MADC_RT_IMAGE_DEF __attribute__((linkonce))
#else
#define MADC_RT_THROW_SIDE
#define MADC_RT_LINKED(fn) 1
#define MADC_RT_IMAGE_DEF
#endif
MADC_RT_THROW_SIDE extern void __madc_try_leave(void *guard);
#ifdef _WIN32
MADC_RT_THROW_SIDE extern struct MadcSehUnwind *__madc_seh_unwind_state(void);
#endif

// A generated frame's region table, as MIR writes it: the layout is
// MIR_UNWIND_TABLE_VERSION's in third_party/mir/mir.h. Its header names how
// the cleanup pointers are written: the address itself in the JIT, and in an
// image a pc-relative 32-bit offset to the pool slot holding the address
// (indirect | pcrel | sdata4, as gcc's personality pointer reaches DW.ref).
#define MADC_UNWIND_TABLE_VERSION 1
#define MADC_DW_EH_PE_absptr 0x00
#define MADC_DW_EH_PE_image 0x9b

static uint64_t madc_uleb(const uint8_t **p)
{
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do
    {
	b = *(*p)++;
	if ( shift < 64 )
	    v |= (uint64_t)(b & 0x7f) << shift;
	shift += 7;
    } while ( b & 0x80 );
    return v;
}

static int64_t madc_sleb(const uint8_t **p)
{
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b;
    do
    {
	b = *(*p)++;
	if ( shift < 64 )
	    v |= (uint64_t)(b & 0x7f) << shift;
	shift += 7;
    } while ( b & 0x80 );
    if ( shift < 64 && (b & 0x40) )
	v |= ~(uint64_t)0 << shift;
    return (int64_t)v;
}

// The cleanup pointer at P, written in ENC.
static uintptr_t madc_table_pointer(const uint8_t *p, uint8_t enc)
{
    if ( enc == MADC_DW_EH_PE_image )
    {
	int32_t rel;
	uintptr_t slot;
	memcpy(&rel, p, sizeof(rel));
	memcpy(&slot, p + rel, sizeof(slot));
	return slot;
    }
    {
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return (uintptr_t)v;
    }
}

// Region REGION (from 1) of the region list at P, its pointers written in
// ENC: its parent, its variable's offset from the frame base, and its cleanup.
static void madc_table_region(const uint8_t *p, uint8_t enc, uint64_t region,
			      uint64_t *parent, int64_t *offset, uintptr_t *cleanup)
{
    uint64_t i;
    for ( i = 1; ; i++ )
    {
	*parent = madc_uleb(&p);
	*offset = madc_sleb(&p);
	if ( i == region )
	    break;
	p += enc == MADC_DW_EH_PE_image ? 4 : 8;
    }
    *cleanup = madc_table_pointer(p, enc);
}

// The value of the frame's register REG (the table's base_reg), which the
// platform handler reads from its own frame description.
typedef uintptr_t (*madc_frame_reg_fn)(void *frame, int reg);

// The region walk every platform's handler runs over a generated frame: the
// region chain live where the frame's call returns to RA (an offset from the
// function's start) runs innermost first, each cleanup called with its
// variable's address (the frame base, REG_VALUE (FRAME, base_reg) + the
// table's base_offset, plus the region's offset) -- stopping at TARGET's guard
// (the region whose cleanup is __madc_try_leave and whose variable holds that
// try's context), so what the try body made goes and the rest of the frame
// stays. TARGET NULL runs every live region.
static void madc_run_regions(const uint8_t *p, uintptr_t ra, madc_frame_reg_fn reg_value,
			     void *frame, const struct MadcTryContext *target)
{
    const uint8_t *regions;
    uint64_t base_reg, base_align, ncalls, nregions, region = 0, i;
    int64_t base_offset;
    uintptr_t base;
    uint8_t enc;

    if ( !p )
	return;
    enc = p[1];
    if ( p[0] != MADC_UNWIND_TABLE_VERSION
	 || (enc != MADC_DW_EH_PE_absptr && enc != MADC_DW_EH_PE_image) )
	return;
    p += 2;
    base_reg = madc_uleb(&p);
    base_offset = madc_sleb(&p);
    base_align = madc_uleb(&p);
    ncalls = madc_uleb(&p);
    for ( i = 0; i < ncalls; i++ )
    {
	uint64_t start = madc_uleb(&p), len = madc_uleb(&p), r = madc_uleb(&p);
	if ( ra > start && ra <= start + len )
	    region = r;
    }
    nregions = madc_uleb(&p);
    regions = p;
    if ( region == 0 || region > nregions )
	return;
    base = reg_value(frame, (int)base_reg) + (uintptr_t)base_offset;
    if ( base_align > 1 )
	base = (base + (uintptr_t)base_align - 1) & ~((uintptr_t)base_align - 1);
    while ( region != 0 )
    {
	uint64_t parent;
	int64_t offset;
	uintptr_t cleanup;
	void *var;
	madc_table_region(regions, enc, region, &parent, &offset, &cleanup);
	var = (void *)(base + (uintptr_t)offset);
	if ( target && cleanup == (uintptr_t)__madc_try_leave && *(void **)var == (void *)target )
	    break;
	((void (*)(void *))cleanup)(var);
	region = parent;
    }
}

#ifdef MADC_RT_UNWIND
static uintptr_t madc_unwind_reg(void *uc, int reg)
{
    return _Unwind_GetGR((struct MadcUnwindContext *)uc, reg);
}

// The personality every generated function with regions names: in the cleanup
// phase, the frame's live regions (madc_run_regions), down to the throw's
// target try's guard. A madc try is no search-phase handler: the forced unwind
// lands it. A foreign exception runs every region, as g++ destroys a frame's
// objects for any exception.
MADC_RT_IMAGE_DEF int __madc_personality(int version, int actions, uint64_t exception_class,
					 struct MadcUnwindHeader *header,
					 struct MadcUnwindContext *uc)
{
    const struct MadcTryContext *target = NULL;

    if ( version != 1 || !(actions & MADC_UA_CLEANUP_PHASE) )
	return MADC_URC_CONTINUE_UNWIND;
    if ( exception_class == MADC_UNWIND_CLASS )
	target = ((struct MadcUnwind *)header)->target;
    madc_run_regions((const uint8_t *)_Unwind_GetLanguageSpecificData(uc),
		     _Unwind_GetIP(uc) - _Unwind_GetRegionStart(uc), madc_unwind_reg, uc, target);
    return MADC_URC_CONTINUE_UNWIND;
}
#endif

#ifdef _WIN32
// Windows (x64 SEH): a generated function with regions names
// __madc_seh_handler as its language-specific handler, with its region table
// as the handler data (MIR's UNWIND_INFO, EHANDLER | UHANDLER). A madc throw's
// longjmp is an unwinding one (the try passes its frame to _setjmp), so
// RtlUnwindEx calls the handler for every frame between the throw and the try,
// the try's own frame flagged as the target. The ABI's records are declared
// here under madc's names, with no <windows.h>.
struct MadcExceptionRecord {
    uint32_t code;
    uint32_t flags;
    struct MadcExceptionRecord *next;
    void *address;
    uint32_t nparams;
    uintptr_t info[15];
};

struct MadcRuntimeFunction {
    uint32_t begin, end, unwind;
};

struct MadcDispatcherContext {
    uint64_t control_pc;
    uint64_t image_base;
    const struct MadcRuntimeFunction *function_entry;
    uint64_t establisher_frame;
    uint64_t target_ip;
    void *context_record;
    void *language_handler;
    const void *handler_data;
};

enum {
    MADC_EXCEPTION_CONTINUE_SEARCH = 1,
    MADC_EH_UNWINDING = 0x02,
    MADC_EH_TARGET_UNWIND = 0x20,
};
#define MADC_STATUS_LONGJUMP 0x80000026u
// libgcc's SEH codes, GCC_EXCEPTION (TYPE): its throw, unwind, forced unwind
#define MADC_STATUS_GCC_MASK 0xfcffffffu
#define MADC_STATUS_GCC 0x20474343u

// The frame base on Windows: MIR writes the table's base_offset from the
// establisher frame, which the dispatcher hands the handler.
static uintptr_t madc_seh_frame_reg(void *frame, int reg)
{
    (void)reg;
    return (uintptr_t)frame;
}

MADC_RT_IMAGE_DEF int __madc_seh_handler(struct MadcExceptionRecord *rec, uint64_t frame,
					 void *context, struct MadcDispatcherContext *dc)
{
    struct MadcSehUnwind *unwind = MADC_RT_LINKED(__madc_seh_unwind_state)
				   ? __madc_seh_unwind_state() : NULL;
    struct MadcTryContext *target = unwind ? unwind->target : NULL;
    int madc_p;
    (void)context;
    if ( !(rec->flags & MADC_EH_UNWINDING) )
	return MADC_EXCEPTION_CONTINUE_SEARCH;
    madc_p = rec->code == MADC_STATUS_LONGJUMP && unwind && unwind->jbuf && rec->nparams >= 1
	     && rec->info[0] == (uintptr_t)unwind->jbuf;
    // A plain longjmp runs no cleanups (gcc's handler ignores
    // STATUS_LONGJUMP too); a host C++ exception crossing the frame runs
    // every live region, as g++ destroys a frame's objects for any exception.
    if ( !madc_p && (rec->code & MADC_STATUS_GCC_MASK) != MADC_STATUS_GCC )
	return MADC_EXCEPTION_CONTINUE_SEARCH;
    madc_run_regions((const uint8_t *)dc->handler_data,
		     (uintptr_t)(dc->control_pc - dc->image_base - dc->function_entry->begin),
		     madc_seh_frame_reg, (void *)(uintptr_t)frame, madc_p ? target : NULL);
    if ( madc_p && (rec->flags & MADC_EH_TARGET_UNWIND) )
    {
	unwind->jbuf = NULL;
	unwind->target = NULL;
    }
    return MADC_EXCEPTION_CONTINUE_SEARCH;
}
#endif
