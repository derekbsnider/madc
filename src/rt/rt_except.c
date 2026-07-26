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

// Storage class of the per-thread exception state. Real TLS in the host
// build; the ledger build defines it EMPTY (-DMADC_RT_TLS=) because MIR has
// no thread-local storage, so a `-static-libmadc` image carries PROCESS-GLOBAL
// exception state. Single-threaded programs are unaffected; the honest fix is
// TLS in MIR (a Tier-3 floor gap, .claude/rules/lowering-vs-raising.md).
#ifndef MADC_RT_TLS
#define MADC_RT_TLS _Thread_local
#endif

// Exception type tags (matches madc DataType values where possible)
#define MADC_EXCEPT_NONE    0
#define MADC_EXCEPT_INT     1
#define MADC_EXCEPT_DOUBLE  2
#define MADC_EXCEPT_CSTR    3
#define MADC_EXCEPT_ANY     99  // catch(...)

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
};

// Thread-local exception state
static MADC_RT_TLS struct MadcTryContext *madc_try_stack = NULL;
static MADC_RT_TLS struct MadcException madc_current_exception = {0};
static MADC_RT_TLS struct MadcCleanupEntry *madc_cleanup_stack = NULL;

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

// Current cleanup-stack top — a mark the try lowering captures before the body
// so it can discard exactly the try-body entries on the NORMAL exit path (the
// cleanup attribute already ran their dtors at C-block scope exit). On the
// exception path __madc_throw_* unwinds to the same mark instead. P1.1c.
void *__madc_cleanup_top(void)
{
    return (void *)madc_cleanup_stack;
}

// Push a HEAP-ALLOCATED cleanup entry naming a destructor by VALUE (not via a
// double-indirect fn slot). The lowering passes `(void*)Cls___dtor` and
// `(void*)&obj` directly — no caller-provided entry storage and no per-object
// stack locals in the try body. This matters for the JIT path: adding stack
// arrays/locals inside a try body perturbs the MIR frame allocation and can make
// the try-context overlap an object (observed 2026-05-31, P1.1c) — passing only
// immediate call arguments avoids that. The entry is freed by unwind/discard/pop.
// `dtor` has the shape `void (*)(void *this)` — the same single-`this` shape the
// cleanup attribute and the class dtor symbol use. P1.1c.
void __madc_cleanup_push_dtor(void *dtor, void *obj)
{
    struct MadcCleanupEntry *entry
	= (struct MadcCleanupEntry *)malloc(sizeof(struct MadcCleanupEntry));
    if ( !entry )
	return;
    entry->fn_indirect = NULL;
    entry->obj_ptr = obj;
    entry->guard = NULL;
    entry->is_chain_tail = 0;
    entry->dtor_direct = dtor;   // direct function pointer (fn_indirect == NULL)
    entry->heap_alloc = 1;
    entry->prev = madc_cleanup_stack;
    madc_cleanup_stack = entry;
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

// Throw an integer exception
void __madc_throw_int(int64_t val)
{
    struct MadcTryContext *ctx;
    madc_current_exception.type = MADC_EXCEPT_INT;
    madc_current_exception.int_val = val;
    if ( !madc_try_stack )
    {
	__madc_cleanup_unwind_to(NULL);
	fprintf(stderr, "Unhandled exception: %ld\n", (long)val);
	abort();
    }
    ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    __madc_cleanup_unwind_to(ctx->cleanup_mark);
    longjmp(ctx->jbuf, 1);
}

// Throw a double exception
void __madc_throw_double(double val)
{
    struct MadcTryContext *ctx;
    madc_current_exception.type = MADC_EXCEPT_DOUBLE;
    madc_current_exception.double_val = val;
    if ( !madc_try_stack )
    {
	__madc_cleanup_unwind_to(NULL);
	fprintf(stderr, "Unhandled exception: %f\n", val);
	abort();
    }
    ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    __madc_cleanup_unwind_to(ctx->cleanup_mark);
    longjmp(ctx->jbuf, 1);
}

// Throw a string exception
void __madc_throw_cstr(const char *val)
{
    struct MadcTryContext *ctx;
    madc_current_exception.type = MADC_EXCEPT_CSTR;
    madc_current_exception.str_val = val;
    if ( !madc_try_stack )
    {
	__madc_cleanup_unwind_to(NULL);
	fprintf(stderr, "Unhandled exception: %s\n", val ? val : "(null)");
	abort();
    }
    ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    __madc_cleanup_unwind_to(ctx->cleanup_mark);
    longjmp(ctx->jbuf, 1);
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

// Clear the current exception (after catch completes)
void __madc_exception_clear(void)
{
    madc_current_exception.type = MADC_EXCEPT_NONE;
    madc_current_exception.int_val = 0;
    madc_current_exception.double_val = 0;
    madc_current_exception.str_val = NULL;
}

// Rethrow the current exception (throw; with no expression)
void __madc_rethrow(void)
{
    struct MadcTryContext *ctx;
    if ( madc_current_exception.type == MADC_EXCEPT_NONE )
    {
	fprintf(stderr, "rethrow with no active exception\n");
	abort();
    }
    if ( !madc_try_stack )
    {
	__madc_cleanup_unwind_to(NULL);
	fprintf(stderr, "Unhandled rethrown exception\n");
	abort();
    }
    // Don't modify the exception state — just longjmp to the next try
    ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    __madc_cleanup_unwind_to(ctx->cleanup_mark);
    longjmp(ctx->jbuf, 1);
}
