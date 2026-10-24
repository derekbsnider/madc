#ifndef __RT_UNWIND_H
#define __RT_UNWIND_H 1

/* rt_unwind.h — what the exception runtime (rt_except.c, the throw side) and
 * the unwind personality (rt_personality.c) share: the platform unwinder's
 * ABI under madc's names and types, and the records of a madc unwind in
 * flight (docs/plans/2026-10-19-table-driven-unwinding.md).
 *
 * Strict C11 with no system header but <stdint.h>: rt_personality.c's text,
 * this file inlined, is also a startup member (madc_crt.cpp) that c2mir
 * compiles with no include path. */

#include <stdint.h>

struct MadcTryContext;

#ifndef _WIN32
// A throw reaches its try through the unwinder's FORCED unwind (libgcc_s;
// libunwind on macOS), glibc's thread-cancellation shape: the unwinder calls
// each frame's personality in its cleanup phase. The Itanium ABI's functions
// are declared here under their own names, with no <unwind.h>.
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

// A madc unwind: the header first, so a personality reaches the target try
// through the header's address; the landing, a jump buffer in the frame that
// holds LIMIT (a throw's: its try context; a boundary's: the boundary). It
// lives on the heap, never on the stack being unwound: a g++ frame's landing
// pad resets SP to its own frame, and its _Unwind_Resume reads the header
// back from storage below (g++ heap-allocates its exception, glibc keeps its
// in the thread).
struct MadcUnwind {
    struct MadcUnwindHeader header;
    struct MadcTryContext *target;
    uintptr_t limit;
    void *jbuf;
};

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
#else
// The madc unwind in flight on Windows, set before its unwinding longjmp:
// the jump buffer it lands in, and the try whose guard the walk stops at
// (NULL for a boundary's). __madc_seh_handler runs regions for that longjmp
// alone, and clears it in the target frame; a boundary's frame is a host
// one, and the next madc unwind overwrites the stale value. Per thread: the
// throw side owns it (rt_except.c), the handler reads it through
// __madc_seh_unwind_state.
struct MadcSehUnwind {
    void *jbuf;
    struct MadcTryContext *target;
};
struct MadcSehUnwind *__madc_seh_unwind_state(void);
#endif

#endif
