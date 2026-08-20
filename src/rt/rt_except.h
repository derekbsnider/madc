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

#ifdef __cplusplus
}
#endif

#endif /* __RT_EXCEPT_H */
