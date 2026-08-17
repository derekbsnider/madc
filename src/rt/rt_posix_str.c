// SPDX-License-Identifier: MPL-2.0
// Win64 POSIX string compatibility runtime.
//
// This is a strict-C11 dual-build source: the hosted compiler puts it in
// libmadc/libmadc_rt, and the same source can become an AOT-ledger module.
// Keep it free of compiler builtins and C++ runtime dependencies.

#if !defined(_WIN32) || !defined(_WIN64)
#error "rt_posix_str.c is a Win64-only runtime source"
#endif

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

char *strndup(const char *s, size_t size)
{
	size_t length = 0;
	size_t i;
	char *copy;

	while (length < size && s[length] != '\0')
		++length;

	/* No representable allocation can hold the terminator in this case. */
	if (length == (size_t)-1) {
		errno = ENOMEM;
		return NULL;
	}

	copy = (char *)malloc(length + 1);
	if (copy == NULL)
		return NULL;

	for (i = 0; i < length; ++i)
		copy[i] = s[i];
	copy[length] = '\0';
	return copy;
}
