#ifndef __RT_EXCEPT_H
#define __RT_EXCEPT_H 1

/* rt_except.h — the HOST-CALLABLE surface of the exception runtime.
 *
 * rt_except.c had no header because its only caller was GENERATED code, whose
 * externs the CIR builder emits (need_output_extern). The first HOST caller —
 * a runtime thunk that must raise a script-catchable error (madarray_count,
 * src/madc_mir_backend.cpp) — needs a prototype, and a local `extern` at the
 * call site is the silent-mismatch trap rt_dump.h documents: it links whatever
 * was written and corrupts arguments if the two drift. So the prototypes live
 * here and rt_except.c includes this file, which makes the compiler CHECK them
 * against the definitions.
 *
 * Deliberately only the throw family: the try-stack / cleanup-stack primitives
 * (__madc_try_pop, __madc_cleanup_push, ...) are the generated code's own
 * machinery, meaningful only inside a translated try scope. A host frame has
 * no such scope — the ONLY correct host interaction with this runtime is to
 * RAISE, letting the script-side stack (or the unhandled-exception abort) do
 * the rest. Widening this header is a design decision, not an edit.
 *
 * A throw longjmps to the innermost script `try` (or prints "Unhandled
 * exception: ..." and aborts). The jump unwinds no C++ frames, so a host
 * caller must hold NO live destructors when it throws. A cstr's pointer is
 * STORED, not copied — pass a string literal or other immortal storage.
 *
 * Plain C, C linkage, ledger-safe (rt_except.c is `all` in
 * scripts/ledger_sources.txt).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exception type tags — THE authoritative home (MT-3 moved them here from
 * rt_except.c; parser.cpp's catch-clause lowering emits these values as
 * numeric literals with name-comments). */
#define MADC_EXCEPT_NONE    0
#define MADC_EXCEPT_INT     1
#define MADC_EXCEPT_DOUBLE  2
#define MADC_EXCEPT_CSTR    3
#define MADC_EXCEPT_CLASS   4
#define MADC_EXCEPT_ANY     99

void __madc_throw_int(int64_t val);
void __madc_throw_double(double val);
void __madc_throw_cstr(const char *val);
void __madc_throw_object(const void *obj);

/* Render the in-flight exception as one line of text (MT-3 scope error
 * capture; also the formatting owner behind every "Unhandled exception"
 * print). Returns bytes written (truncated to cap-1); 0 = none in flight. */
unsigned long __madc_exception_text(char *buf, unsigned long cap);

/* The C-side try frame (MT-3's task trampoline — the third legitimate host
 * consumer, widened consciously like the state switch below): a scoped
 * task's uncaught error must be CAPTURED by its scope, not abort the
 * process, so the trampoline arms one catch-all frame around the task body:
 * allocate __madc_try_context_size() bytes, setjmp on the jmp_buf
 * __madc_try_push returns (setjmp must run in the frame that stays live),
 * __madc_try_pop on the normal path, and read the exception through the
 * accessors on the longjmp path. Everything else about try frames remains
 * the generated code's own API. */
struct MadcTryContext;
unsigned long __madc_try_context_size(void);
void *__madc_try_push(struct MadcTryContext *ctx);
void __madc_try_pop(void);
int __madc_exception_type(void);
const char *__madc_exception_cstr(void);
void __madc_exception_clear(void);

/* Per-execution-context state switch (the MT arc's task runtime,
 * src/rt/rt_task.c — the second legitimate host consumer, and the widening
 * decision the note above demands made consciously): the try chain, the
 * cleanup chain, and the in-flight exception are PER-CONTEXT state, saved
 * and restored at every task switch exactly like CPU registers. Without
 * this, a `try` held across a `yield()` lands its throw in ANOTHER task's
 * jmp_buf. The buffer is opaque; its required size is
 * __madc_except_state_size() (checked loud by the task runtime). A
 * zero-filled buffer restores as the EMPTY state — a fresh task's start. */
unsigned long __madc_except_state_size(void);
void __madc_except_state_save(void *buf);
void __madc_except_state_restore(const void *buf);

#ifdef __cplusplus
}
#endif

#endif /* __RT_EXCEPT_H */
