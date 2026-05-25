///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Exception handling runtime — SJLJ (setjmp/longjmp) model.             //
// Thread-local exception state and try/throw/catch support functions.    //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>

// Exception type tags (matches madc DataType values where possible)
#define MADC_EXCEPT_NONE    0
#define MADC_EXCEPT_INT     1
#define MADC_EXCEPT_DOUBLE  2
#define MADC_EXCEPT_CSTR    3
#define MADC_EXCEPT_ANY     99  // catch(...)

// Per-try-block context — linked list, one per active try
struct MadcTryContext {
    jmp_buf jbuf;
    MadcTryContext *prev;   // outer try block
};

// Current exception value
struct MadcException {
    int type;               // MADC_EXCEPT_* tag
    int64_t int_val;        // for throw int
    double double_val;      // for throw double
    const char *str_val;    // for throw "string"
};

// Thread-local exception state
static thread_local MadcTryContext *madc_try_stack = nullptr;
static thread_local MadcException madc_current_exception = {};

// --- Runtime functions called from JIT code ---

extern "C" {

// Push a try context onto the stack, return pointer to its jmp_buf
void *__madc_try_push(MadcTryContext *ctx)
{
    ctx->prev = madc_try_stack;
    madc_try_stack = ctx;
    return (void *)ctx->jbuf;
}

// Pop the current try context (normal exit from try block)
void __madc_try_pop()
{
    if ( madc_try_stack )
	madc_try_stack = madc_try_stack->prev;
}

// Throw an integer exception
void __madc_throw_int(int64_t val)
{
    madc_current_exception.type = MADC_EXCEPT_INT;
    madc_current_exception.int_val = val;
    if ( !madc_try_stack )
    {
	fprintf(stderr, "Unhandled exception: %ld\n", (long)val);
	abort();
    }
    MadcTryContext *ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    longjmp(ctx->jbuf, 1);
}

// Throw a double exception
void __madc_throw_double(double val)
{
    madc_current_exception.type = MADC_EXCEPT_DOUBLE;
    madc_current_exception.double_val = val;
    if ( !madc_try_stack )
    {
	fprintf(stderr, "Unhandled exception: %f\n", val);
	abort();
    }
    MadcTryContext *ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    longjmp(ctx->jbuf, 1);
}

// Throw a string exception
void __madc_throw_cstr(const char *val)
{
    madc_current_exception.type = MADC_EXCEPT_CSTR;
    madc_current_exception.str_val = val;
    if ( !madc_try_stack )
    {
	fprintf(stderr, "Unhandled exception: %s\n", val ? val : "(null)");
	abort();
    }
    MadcTryContext *ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    longjmp(ctx->jbuf, 1);
}

// Get exception type tag
int __madc_exception_type()
{
    return madc_current_exception.type;
}

// Get exception int value
int64_t __madc_exception_int()
{
    return madc_current_exception.int_val;
}

// Get exception double value
double __madc_exception_double()
{
    return madc_current_exception.double_val;
}

// Get exception string value
const char *__madc_exception_cstr()
{
    return madc_current_exception.str_val;
}

// Clear the current exception (after catch completes)
void __madc_exception_clear()
{
    madc_current_exception.type = MADC_EXCEPT_NONE;
    madc_current_exception.int_val = 0;
    madc_current_exception.double_val = 0;
    madc_current_exception.str_val = nullptr;
}

// Rethrow the current exception (throw; with no expression)
void __madc_rethrow()
{
    if ( madc_current_exception.type == MADC_EXCEPT_NONE )
    {
	fprintf(stderr, "rethrow with no active exception\n");
	abort();
    }
    if ( !madc_try_stack )
    {
	fprintf(stderr, "Unhandled rethrown exception\n");
	abort();
    }
    // Don't modify the exception state — just longjmp to the next try
    MadcTryContext *ctx = madc_try_stack;
    madc_try_stack = ctx->prev;
    longjmp(ctx->jbuf, 1);
}

} // extern "C"
