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

void __madc_throw_int(int64_t val);
void __madc_throw_double(double val);
void __madc_throw_cstr(const char *val);
void __madc_throw_object(const void *obj);

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
