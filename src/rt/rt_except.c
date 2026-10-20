///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Exception handling runtime — SJLJ (setjmp/longjmp) model.             //
// Thread-local exception state and try/throw/catch support functions.    //
//                                                                       //
// A DUAL-BUILD source (forest-carriers S5): the host build compiles it   //
// into libmadc, and madc itself compiles it through c2mir at pack time   //
// into an AOT LEDGER module, so a `-static-libmadc` binary carries this  //
// machinery inside its own image. Keep it strict C11 with no compiler    //
// builtins — c2mir has to be able to compile it too. See                 //
// docs/plans/2026-07-25-forest-carriers-plan.md.                         //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>

#include "rt_except.h"	/* host-callable throw prototypes, CHECKED here */

// Storage class of the per-thread exception state. Real TLS in the host
// build; the ledger build defines it EMPTY (-DMADC_RT_TLS=) because MIR has
// no thread-local storage, so a `-static-libmadc` image carries PROCESS-GLOBAL
// exception state. Single-threaded programs are unaffected; the honest fix is
// TLS in MIR (a Tier-3 floor gap, .claude/rules/lowering-vs-raising.md).
#ifndef MADC_RT_TLS
#define MADC_RT_TLS _Thread_local
#endif

// Exception type tags live in rt_except.h (the one authoritative home).
// MADC_EXCEPT_CLASS: a thrown class object, or a thrown pointer that is not
// text. The payload (obj_val) is a MadcClassException: the object the throw
// constructed in __madc_exception_alloc storage, which the exception OWNS
// until its last handler ends ([except.throw]/4), and its match table. A
// class handler matches by type NAME through __madc_exception_match; the tag
// alone never selects one.

// Per-object cleanup entry — linked list, pushed at construction time
struct MadcCleanupEntry {
    void **fn_indirect;          // pointer to function pointer (double deref for JIT dtors)
    void *obj_ptr;               // object (this) pointer
    uint8_t *guard;              // guard byte on JIT stack (NULL if unguarded)
    uint8_t is_chain_tail;       // 1 = last in dtor chain for this object, clear guard
    void *dtor_direct;           // direct dtor fn pointer (used when fn_indirect == NULL)
    uint8_t heap_alloc;          // 1 = malloc'd by __madc_cleanup_push_dtor, free on remove
    struct MadcCleanupEntry *prev;  // toward older entries
};

// Per-try-block context — linked list, one per active try
struct MadcTryContext {
    jmp_buf jbuf;
    struct MadcTryContext *prev;           // outer try block
    struct MadcCleanupEntry *cleanup_mark; // cleanup stack depth at try entry
};

// Current exception value
struct MadcException {
    int type;               // MADC_EXCEPT_* tag
    int64_t int_val;        // for throw int
    double double_val;      // for throw double
    const char *str_val;    // for throw "string"
    const void *obj_val;    // MADC_EXCEPT_CLASS: its MadcClassException
};

// A class (or pointer) exception: the object the throw constructed, its
// complete-object destructor (NULL when trivial), and its match table — the
// Itanium type-name string of the object's type and of every unambiguous
// public base, each with that subobject's offset ([except.handle]/3). A
// pointer throw's object is the pointer VALUE; its offsets adjust the value.
// The table is the throw site's static data; the object is owned.
struct MadcClassException {
    void *obj;
    void (*dtor)(void *);
    uint64_t count;
    const char *const *names;
    const int64_t *offsets;
    int is_pointer;
};

// A handled exception (Itanium __cxa_begin_catch / __cxa_end_catch): one per
// active handler, innermost on top — what `throw;` rethrows. `cleanup` is the
// runtime cleanup entry that ends the catch when a throw leaves the handler.
struct MadcCaught {
    struct MadcException ex;
    struct MadcCaught *prev;
    struct MadcCleanupEntry *cleanup;
    int rethrown;
};

// Thread-local exception state
static MADC_RT_TLS struct MadcTryContext *madc_try_stack = NULL;
static MADC_RT_TLS struct MadcException madc_current_exception = {0};
static MADC_RT_TLS struct MadcCleanupEntry *madc_cleanup_stack = NULL;
static MADC_RT_TLS struct MadcCaught *madc_caught_stack = NULL;

// --- Per-execution-context state switch (rt_except.h documents the design
// decision). The four statics above are per-CONTEXT, not merely per-thread:
// the cooperative task runtime saves/restores them at every switch so a try
// held across a yield() keeps its own chain. A zero-filled buffer restores
// as the empty state (fresh task). Plain memcpy — ledger-safe C11.

struct MadcExceptState {
    struct MadcTryContext *try_stack;
    struct MadcException exception;
    struct MadcCleanupEntry *cleanup_stack;
    struct MadcCaught *caught_stack;
};

unsigned long __madc_except_state_size(void)
{
    return (unsigned long)sizeof(struct MadcExceptState);
}

void __madc_except_state_save(void *buf)
{
    struct MadcExceptState *s = (struct MadcExceptState *)buf;
    s->try_stack = madc_try_stack;
    s->exception = madc_current_exception;
    s->cleanup_stack = madc_cleanup_stack;
    s->caught_stack = madc_caught_stack;
}

void __madc_except_state_restore(const void *buf)
{
    const struct MadcExceptState *s = (const struct MadcExceptState *)buf;
    madc_try_stack = s->try_stack;
    madc_current_exception = s->exception;
    madc_cleanup_stack = s->cleanup_stack;
    madc_caught_stack = s->caught_stack;
}

// --- Runtime functions called from JIT code ---

// Byte size of MadcTryContext, so the CIR lowering can allocate an opaque,
// correctly-sized local for a try block without hard-coding jmp_buf internals
// (the struct is private to this TU). The lowering rounds up to a long[] count.
unsigned long __madc_try_context_size(void)
{
    return (unsigned long)sizeof(struct MadcTryContext);
}

// Push a try context onto the stack, return pointer to its jmp_buf
void *__madc_try_push(struct MadcTryContext *ctx)
{
    ctx->prev = madc_try_stack;
    ctx->cleanup_mark = madc_cleanup_stack;
    madc_try_stack = ctx;
    return (void *)ctx->jbuf;
}

// Pop the current try context (normal exit from try block)
void __madc_try_pop(void)
{
    if ( madc_try_stack )
	madc_try_stack = madc_try_stack->prev;
}

// Unwind cleanup entries down to mark, calling destructors
void __madc_cleanup_unwind_to(void *mark)
{
    while ( madc_cleanup_stack != (struct MadcCleanupEntry *)mark )
    {
	struct MadcCleanupEntry *e = madc_cleanup_stack;
	madc_cleanup_stack = e->prev;
	if ( !e->guard || *e->guard )
	{
	    void (*fn)(void *) = e->fn_indirect
		? (void (*)(void *))*e->fn_indirect
		: (void (*)(void *))e->dtor_direct;
	    if ( fn )
		fn(e->obj_ptr);
	    if ( e->guard && e->is_chain_tail )
		*e->guard = 0;
	}
	if ( e->heap_alloc )
	    free(e);
    }
}

// Discard cleanup entries down to mark WITHOUT calling destructors (freeing any
// heap-allocated entries — the dtors already ran via the cleanup attribute on
// the normal scope-exit path).
void __madc_cleanup_discard_to(void *mark)
{
    while ( madc_cleanup_stack != (struct MadcCleanupEntry *)mark )
    {
	struct MadcCleanupEntry *e = madc_cleanup_stack;
	madc_cleanup_stack = e->prev;
	if ( e->heap_alloc )
	    free(e);
    }
}

// A try block's normal exit — the end of its body, a return, break, continue
// or goto out of it: the cleanup attribute on the try's frame guard (a local
// holding the MadcTryContext's address) calls this with the guard's address.
// Ends that frame and drops any cleanup entry its body left (none, when every
// object's guard removed its own). A throw never gets here: the raise popped
// the frame before its longjmp, and a longjmp runs no cleanup attribute.
void __madc_try_leave(void *guard)
{
    struct MadcTryContext *ctx = guard ? *(struct MadcTryContext **)guard : NULL;
    if ( !ctx )
	return;
    __madc_cleanup_discard_to(ctx->cleanup_mark);
    madc_try_stack = ctx->prev;
}

// Current cleanup-stack top — a mark the try lowering captures before the body
// so it can discard exactly the try-body entries on the NORMAL exit path (the
// cleanup attribute already ran their dtors at C-block scope exit). On the
// exception path __madc_throw_* unwinds to the same mark instead. P1.1c.
void *__madc_cleanup_top(void)
{
    return (void *)madc_cleanup_stack;
}

// Push a HEAP-ALLOCATED cleanup entry naming a destructor by VALUE (not via a
// double-indirect fn slot): the lowering passes `(void*)Cls___dtor` and
// `(void*)&obj`. Returns the entry — the lowering keeps it in a guard local
// whose cleanup attribute (__madc_cleanup_leave) removes it when the object's
// scope exits normally. The entry is freed by unwind/discard/remove.
// `dtor` has the shape `void (*)(void *this)` — the same single-`this` shape the
// cleanup attribute and the class dtor symbol use. P1.1c.
void *__madc_cleanup_push_dtor(void *dtor, void *obj)
{
    struct MadcCleanupEntry *entry
	= (struct MadcCleanupEntry *)malloc(sizeof(struct MadcCleanupEntry));
    if ( !entry )
    {
	fprintf(stderr, "out of memory registering a cleanup\n");
	abort();
    }
    entry->fn_indirect = NULL;
    entry->obj_ptr = obj;
    entry->guard = NULL;
    entry->is_chain_tail = 0;
    entry->dtor_direct = dtor;   // direct function pointer (fn_indirect == NULL)
    entry->heap_alloc = 1;
    entry->prev = madc_cleanup_stack;
    madc_cleanup_stack = entry;
    return entry;
}

// Push a cleanup entry onto the cleanup stack
void __madc_cleanup_push(struct MadcCleanupEntry *entry, void **fn_indirect,
			 void *obj, uint8_t *guard, uint8_t is_chain_tail)
{
    entry->dtor_direct = NULL;
    entry->heap_alloc = 0;
    entry->fn_indirect = fn_indirect;
    entry->obj_ptr = obj;
    entry->guard = guard;
    entry->is_chain_tail = is_chain_tail;
    entry->prev = madc_cleanup_stack;
    madc_cleanup_stack = entry;
}

// Pop the top cleanup entry (no destructor call)
void __madc_cleanup_pop(void)
{
    if ( madc_cleanup_stack )
    {
	struct MadcCleanupEntry *e = madc_cleanup_stack;
	madc_cleanup_stack = e->prev;
	if ( e->heap_alloc )
	    free(e);
    }
}

// Remove ONE entry wherever it sits in the stack, no destructor call
// (rt_except.h documents the widening). Top-pop is not enough for a host
// frame whose registration outlives entries pushed BELOW it and is exited
// while entries pushed ABOVE it are still live — the MT-5 scope block's
// normal exit is exactly that shape. O(depth) walk; a scope-bound entry is
// on top when its scope exits, so the walk is one step there.
void __madc_cleanup_remove(void *entry)
{
    struct MadcCleanupEntry *e = (struct MadcCleanupEntry *)entry;
    struct MadcCleanupEntry **link = &madc_cleanup_stack;
    while ( *link )
    {
	if ( *link == e )
	{
	    *link = e->prev;
	    if ( e->heap_alloc )
		free(e);
	    return;
	}
	link = &(*link)->prev;
    }
}

// An object's scope exits normally — its end, a return, break, continue or
// goto: the cleanup attribute on the guard local that holds the object's
// entry (__madc_cleanup_push_dtor's result) calls this with the guard's
// address, BEFORE the object's own cleanup destroys it (the guard is declared
// after the object). The entry goes; a later throw cannot run it again.
void __madc_cleanup_leave(void *guard)
{
    void *entry = guard ? *(void **)guard : NULL;
    if ( entry )
	__madc_cleanup_remove(entry);
}

// --- The platform unwinder (docs/plans/2026-10-19-table-driven-unwinding.md).
// A throw reaches its try through the unwinder's FORCED unwind (libgcc_s;
// libunwind on macOS), glibc's thread-cancellation shape: the unwinder calls
// each frame's personality in its cleanup phase -- a generated frame's
// __madc_personality runs the cleanups its region table lists, a g++ frame's
// own personality its destructors -- and madc_unwind_stop longjmps into the
// try once the walk has passed the try's frame. A frame the unwinder cannot
// describe ends the walk, and the stop function lands there. The Itanium ABI's
// functions are declared here under their own names and madc's types: this
// file is strict C11, compiled by c2mir too, with no <unwind.h>. Windows keeps
// the plain longjmp until generated frames carry its unwind tables.
#ifndef _WIN32
#define MADC_RT_UNWIND 1

enum {
    MADC_URC_NO_REASON = 0,
    MADC_URC_CONTINUE_UNWIND = 8,
    MADC_UA_CLEANUP_PHASE = 2,
    MADC_UA_END_OF_STACK = 16,
};

// The ABI's struct _Unwind_Exception on the 64-bit targets.
struct MadcUnwindHeader {
    uint64_t exception_class;
    void (*exception_cleanup)(int, struct MadcUnwindHeader *);
    uintptr_t private_1, private_2;
};

// A madc throw's unwind: the header first, so a personality reaches the
// target try through the header's address. It lives on the heap, never on
// the stack being unwound: a g++ frame's landing pad resets SP to its own
// frame, and its _Unwind_Resume reads the header back from storage below
// (g++ heap-allocates its exception, glibc keeps its in the thread).
struct MadcUnwind {
    struct MadcUnwindHeader header;
    struct MadcTryContext *target;
};

// The header's exception_cleanup: a host `catch (...)` consumed the unwind.
static void madc_unwind_delete(int reason, struct MadcUnwindHeader *header)
{
    (void)reason;
    free(header);
}

#define MADC_UNWIND_CLASS 0x4d4144435354524bULL	/* "MADCSTRK" */

struct MadcUnwindContext;
typedef int (*madc_unwind_stop_fn)(int, int, uint64_t, struct MadcUnwindHeader *,
				   struct MadcUnwindContext *, void *);
extern int _Unwind_ForcedUnwind(struct MadcUnwindHeader *, madc_unwind_stop_fn, void *);
extern uintptr_t _Unwind_GetCFA(struct MadcUnwindContext *);
extern uintptr_t _Unwind_GetIP(struct MadcUnwindContext *);
extern uintptr_t _Unwind_GetRegionStart(struct MadcUnwindContext *);
extern uintptr_t _Unwind_GetGR(struct MadcUnwindContext *, int);
extern void *_Unwind_GetLanguageSpecificData(struct MadcUnwindContext *);

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

// The personality every generated function with regions names: in the cleanup
// phase, the region chain live at the frame's call runs innermost first, each
// cleanup called with its variable's address -- stopping at the throw's target
// try's guard (the region whose cleanup is __madc_try_leave and whose variable
// holds that try's context), so what the try body made goes and the rest of
// the frame stays. A madc try is no search-phase handler: the forced unwind
// lands it. A foreign exception runs every region, as g++ destroys a frame's
// objects for any exception.
int __madc_personality(int version, int actions, uint64_t exception_class,
		       struct MadcUnwindHeader *header, struct MadcUnwindContext *uc)
{
    const uint8_t *p = (const uint8_t *)_Unwind_GetLanguageSpecificData(uc);
    const uint8_t *regions;
    struct MadcTryContext *target = NULL;
    uint64_t base_reg, base_align, ncalls, nregions, region = 0, i;
    int64_t base_offset;
    uintptr_t ra, base;
    uint8_t enc;

    if ( version != 1 || !p || !(actions & MADC_UA_CLEANUP_PHASE) )
	return MADC_URC_CONTINUE_UNWIND;
    enc = p[1];
    if ( p[0] != MADC_UNWIND_TABLE_VERSION
	 || (enc != MADC_DW_EH_PE_absptr && enc != MADC_DW_EH_PE_image) )
	return MADC_URC_CONTINUE_UNWIND;
    p += 2;
    if ( exception_class == MADC_UNWIND_CLASS )
	target = ((struct MadcUnwind *)header)->target;
    base_reg = madc_uleb(&p);
    base_offset = madc_sleb(&p);
    base_align = madc_uleb(&p);
    ra = _Unwind_GetIP(uc) - _Unwind_GetRegionStart(uc);
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
	return MADC_URC_CONTINUE_UNWIND;
    base = _Unwind_GetGR(uc, (int)base_reg) + (uintptr_t)base_offset;
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
    return MADC_URC_CONTINUE_UNWIND;
}

// The forced unwind's stop function: called for every frame before its
// personality, it lands the throw in its try once the walk has passed the
// try's frame -- the context's CFA is the SP of the frame it describes, above
// every local of the frames below, the try context among them -- or when the
// walk ends (glibc's unwind_stop, with the try context's own address in place
// of the jmp_buf's saved SP).
static int madc_unwind_stop(int version, int actions, uint64_t exception_class,
			    struct MadcUnwindHeader *header, struct MadcUnwindContext *uc,
			    void *param)
{
    struct MadcTryContext *target = (struct MadcTryContext *)param;
    (void)version;
    (void)exception_class;
    if ( (actions & MADC_UA_END_OF_STACK) || _Unwind_GetCFA(uc) > (uintptr_t)target )
    {
	free(header);
	longjmp(target->jbuf, 1);
    }
    return MADC_URC_NO_REASON;
}
#endif

// Print the unhandled-exception line. The ONE formatting owner is
// __madc_exception_text (below) — the same renderer MT-3's scope error
// capture reads, so an aborted print and a captured scope error always
// agree on the text.
static void madc_print_unhandled(void)
{
    char buf[256];
    __madc_exception_text(buf, sizeof(buf));
    fprintf(stderr, "Unhandled exception: %s\n", buf);
}

// THE raise tail every throw shares: unwind the cleanup stack to the
// innermost try's mark and land in that try -- through the platform
// unwinder's forced unwind, which runs every frame's cleanups on the way
// (madc_unwind_stop lands it), or by a plain longjmp where there is no
// unwinder, it returned, or the unwind's header could not be allocated -- or,
// with no try active, run every cleanup, print the exception
// (`unhandled` names the case) and abort.
static void madc_raise(const char *unhandled)
{
    struct MadcTryContext *ctx;
#ifdef MADC_RT_UNWIND
    struct MadcUnwind *unwind;
#endif
    if ( !madc_try_stack )
    {
	__madc_cleanup_unwind_to(NULL);
	if ( unhandled )
	    fprintf(stderr, "%s\n", unhandled);
	else
	    madc_print_unhandled();
	abort();
    }
    ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    __madc_cleanup_unwind_to(ctx->cleanup_mark);
#ifdef MADC_RT_UNWIND
    unwind = (struct MadcUnwind *)calloc(1, sizeof(*unwind));
    if ( unwind )
    {
	unwind->header.exception_class = MADC_UNWIND_CLASS;
	unwind->header.exception_cleanup = madc_unwind_delete;
	unwind->target = ctx;
	_Unwind_ForcedUnwind(&unwind->header, madc_unwind_stop, ctx);
	free(unwind);
    }
#endif
    longjmp(ctx->jbuf, 1);
}

// Release what an exception owns: a class exception's object (destroyed,
// then freed) and its record. Scalar and text payloads own nothing.
static void madc_release(struct MadcException *ex)
{
    struct MadcClassException *r;
    if ( ex->type == MADC_EXCEPT_CLASS && ex->obj_val )
    {
	r = (struct MadcClassException *)ex->obj_val;
	if ( !r->is_pointer && r->dtor )
	    r->dtor(r->obj);
	free(r->obj);
	free(r);
    }
    memset(ex, 0, sizeof(*ex));
}

// Throw an integer exception
void __madc_throw_int(int64_t val)
{
    madc_current_exception.type = MADC_EXCEPT_INT;
    madc_current_exception.int_val = val;
    madc_raise(NULL);
}

// Throw a double exception
void __madc_throw_double(double val)
{
    madc_current_exception.type = MADC_EXCEPT_DOUBLE;
    madc_current_exception.double_val = val;
    madc_raise(NULL);
}

// Throw a string exception
void __madc_throw_cstr(const char *val)
{
    madc_current_exception.type = MADC_EXCEPT_CSTR;
    madc_current_exception.str_val = val;
    madc_raise(NULL);
}

// Storage for a class exception's object: the throw lowering constructs the
// operand into it, then hands it to __madc_throw_class, which owns it from
// there ([except.throw]/4: the exception object is in unspecified storage).
void *__madc_exception_alloc(uint64_t size)
{
    void *p = calloc(1, size ? size : 1);
    if ( !p )
    {
	fprintf(stderr, "out of memory allocating an exception object\n");
	abort();
    }
    return p;
}

// Throw a class object (or a pointer that is not text): `obj` is the
// __madc_exception_alloc storage holding it, `dtor` its complete-object
// destructor (NULL when trivial), and names[i] / offsets[i] its match table
// (struct MadcClassException) — the throw site's static data.
void __madc_throw_class(void *obj, void *dtor, uint64_t count,
			const char *const *names, const int64_t *offsets,
			int is_pointer)
{
    struct MadcClassException *r =
	(struct MadcClassException *)malloc(sizeof(struct MadcClassException));
    if ( !r )
    {
	fprintf(stderr, "out of memory throwing an exception\n");
	abort();
    }
    r->obj = obj;
    r->dtor = (void (*)(void *))dtor;
    r->count = count;
    r->names = names;
    r->offsets = offsets;
    r->is_pointer = is_pointer;
    madc_current_exception.type = MADC_EXCEPT_CLASS;
    madc_current_exception.obj_val = r;
    madc_raise(NULL);
}

// Does the in-flight exception match a handler for the type whose Itanium
// type-name string is `name`? On a match, *out (when out is non-NULL) is the
// matched subobject's address — or, for a pointer, the pointer value adjusted
// to the matched type (a null pointer stays null).
int __madc_exception_match(const char *name, void **out)
{
    const struct MadcClassException *r;
    uint64_t i;
    if ( madc_current_exception.type != MADC_EXCEPT_CLASS
      || !madc_current_exception.obj_val || !name )
	return 0;
    r = (const struct MadcClassException *)madc_current_exception.obj_val;
    for ( i = 0; i < r->count; i++ )
    {
	if ( !r->names[i] || strcmp(r->names[i], name) != 0 )
	    continue;
	if ( out )
	{
	    if ( r->is_pointer )
	    {
		char *pv = *(char **)r->obj;
		*out = pv ? pv + r->offsets[i] : NULL;
	    }
	    else
		*out = (char *)r->obj + r->offsets[i];
	}
	return 1;
    }
    return 0;
}

// End a catch: pop it from the caught stack and release its exception —
// unless `throw;` rethrew it, in which case the in-flight record owns it.
static void madc_end_catch(struct MadcCaught *c)
{
    struct MadcCaught **link = &madc_caught_stack;
    while ( *link && *link != c )
	link = &(*link)->prev;
    if ( *link )
	*link = c->prev;
    if ( !c->rethrown )
	madc_release(&c->ex);
    free(c);
}

// The runtime cleanup entry's handler: a throw left the handler, so the
// unwind ends its catch (the entry is consumed by the unwind itself).
static void madc_end_catch_unwound(void *c)
{
    madc_end_catch((struct MadcCaught *)c);
}

// A handler starts: the in-flight exception becomes the handled one (pushed
// on the caught stack), and a runtime cleanup entry ends the catch if a throw
// leaves the handler. The returned handle is the catch's guard; the handler
// ends it on every other exit through __madc_exception_end_catch.
void *__madc_exception_begin_catch(void)
{
    struct MadcCaught *c = (struct MadcCaught *)malloc(sizeof(struct MadcCaught));
    if ( !c )
    {
	fprintf(stderr, "out of memory catching an exception\n");
	abort();
    }
    c->ex = madc_current_exception;
    c->rethrown = 0;
    c->prev = madc_caught_stack;
    madc_caught_stack = c;
    memset(&madc_current_exception, 0, sizeof(madc_current_exception));
    c->cleanup = (struct MadcCleanupEntry *)
	__madc_cleanup_push_dtor((void *)madc_end_catch_unwound, c);
    return c;
}

// A handler's normal exit — the end of its block, a return, break, continue
// or goto: the cleanup attribute on its guard calls this with the guard's
// address. Removes the catch's runtime cleanup entry and ends the catch.
void __madc_exception_end_catch(void *guard)
{
    struct MadcCaught *c = guard ? *(struct MadcCaught **)guard : NULL;
    if ( !c )
	return;
    __madc_cleanup_remove(c->cleanup);
    madc_end_catch(c);
}

// Render the in-flight exception as one line of text (MT-3: the scope
// error capture; also THE renderer behind every "Unhandled exception"
// print — one formatting owner). Returns the byte length written
// (truncated to cap-1), 0 when no exception is in flight.
unsigned long __madc_exception_text(char *buf, unsigned long cap)
{
    int n = 0;
    if ( !buf || cap == 0 )
	return 0;
    switch ( madc_current_exception.type )
    {
    case MADC_EXCEPT_INT:
	n = snprintf(buf, cap, "%ld", (long)madc_current_exception.int_val);
	break;
    case MADC_EXCEPT_DOUBLE:
	n = snprintf(buf, cap, "%f", madc_current_exception.double_val);
	break;
    case MADC_EXCEPT_CSTR:
	n = snprintf(buf, cap, "%s", madc_current_exception.str_val
					 ? madc_current_exception.str_val
					 : "(null)");
	break;
    case MADC_EXCEPT_CLASS:
    {
	const struct MadcClassException *r =
	    (const struct MadcClassException *)madc_current_exception.obj_val;
	n = snprintf(buf, cap, "class object of type %s at %p",
		     r && r->count && r->names[0] ? r->names[0] : "?",
		     r ? r->obj : NULL);
	break;
    }
    default:
	buf[0] = '\0';
	return 0;
    }
    if ( n < 0 )
	n = 0;
    if ( (unsigned long)n >= cap )
	n = (int)(cap - 1);
    return (unsigned long)n;
}

// Get exception type tag
int __madc_exception_type(void)
{
    return madc_current_exception.type;
}

// Get exception int value
int64_t __madc_exception_int(void)
{
    return madc_current_exception.int_val;
}

// Get exception double value
double __madc_exception_double(void)
{
    return madc_current_exception.double_val;
}

// Get exception string value
const char *__madc_exception_cstr(void)
{
    return madc_current_exception.str_val;
}

// Discard the in-flight exception, releasing what it owns (a host frame that
// captured it — MT-3's scope error capture).
void __madc_exception_clear(void)
{
    madc_release(&madc_current_exception);
}

// `throw;` ([expr.throw]/3): rethrow the exception the innermost active
// handler is handling. Its catch is marked rethrown, so the unwind that ends
// it hands the object over instead of destroying it.
void __madc_rethrow(void)
{
    struct MadcCaught *c = madc_caught_stack;
    if ( !c )
    {
	fprintf(stderr, "rethrow with no active exception\n");
	abort();
    }
    c->rethrown = 1;
    madc_current_exception = c->ex;
    madc_raise("Unhandled rethrown exception");
}

// No handler of a try matched: the in-flight exception continues to the next
// enclosing try, unchanged (it was never caught).
void __madc_exception_resume(void)
{
    if ( madc_current_exception.type == MADC_EXCEPT_NONE )
    {
	fprintf(stderr, "resume with no exception in flight\n");
	abort();
    }
    madc_raise(NULL);
}
