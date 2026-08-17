// SPDX-License-Identifier: MPL-2.0
// Win64 POSIX time compatibility runtime.
//
// This is a strict-C11 dual-build source: the hosted compiler puts it in
// libmadc/libmadc_rt, and the same source can become an AOT-ledger module.
// Keep it free of compiler builtins and C++ runtime dependencies.

#if !defined(_WIN32) || !defined(_WIN64)
#error "rt_posix_time.c is a Win64-only runtime source"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

unsigned int sleep(unsigned int seconds)
{
	/* INFINITE is not a finite Sleep timeout.  Chunk before converting
	 * seconds to DWORD milliseconds so neither multiplication nor the
	 * reserved timeout value can change a finite POSIX request. */
	const unsigned int max_chunk_seconds
		= (unsigned int)((INFINITE - 1UL) / 1000UL);

	while (seconds > max_chunk_seconds) {
		Sleep((DWORD)(max_chunk_seconds * 1000U));
		seconds -= max_chunk_seconds;
	}
	if (seconds != 0U)
		Sleep((DWORD)(seconds * 1000U));

	/* Win32 Sleep has no interruption result.  Until the hosted runtime has
	 * a wakeable POSIX-signal bridge, a completed wait has no unslept tail. */
	return 0U;
}
