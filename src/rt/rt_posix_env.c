// SPDX-License-Identifier: MPL-2.0
// Win64 POSIX environment compatibility runtime.
//
// This is a strict-C11 dual-build source: the hosted compiler puts it in
// libmadc/libmadc_rt, and the same source can become an AOT-ledger module.
// Keep it free of compiler builtins and C++ runtime dependencies.
//
// _putenv_s is the CRT mutator, so a value set here is visible to the CRT's
// own getenv() in the same process. SetEnvironmentVariable() is deliberately
// NOT used: it updates the Win32 block without updating the CRT's view, so a
// setenv/getenv round-trip would silently read stale data.

#if !defined(_WIN32) || !defined(_WIN64)
#error "rt_posix_env.c is a Win64-only runtime source"
#endif

#include <errno.h>
#include <stdlib.h>	/* getenv, and _putenv_s via sec_api/stdlib_s.h */

/* POSIX.1-2008: the name must be non-NULL, non-empty and contain no '='. */
static int madc_env_name_rejected(const char *name)
{
	const char *c;

	if (name == NULL || name[0] == '\0')
		return 1;
	for (c = name; *c != '\0'; ++c)
		if (*c == '=')
			return 1;
	return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (madc_env_name_rejected(name) || value == NULL) {
		errno = EINVAL;
		return -1;
	}
	/* An existing name with overwrite == 0 succeeds and changes nothing. */
	if (overwrite == 0 && getenv(name) != NULL)
		return 0;
	if (_putenv_s(name, value) != 0) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

int unsetenv(const char *name)
{
	if (madc_env_name_rejected(name)) {
		errno = EINVAL;
		return -1;
	}
	/* Removing an absent name succeeds ([POSIX] unsetenv). */
	if (getenv(name) == NULL)
		return 0;
	/* On Windows an empty value IS the removal spelling. */
	if (_putenv_s(name, "") != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}
