// SPDX-License-Identifier: MPL-2.0
///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Win64 POSIX supplement for the platform's real <stdlib.h>.            //
// Additive declarations only; the native header is always served first. //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_POSIX_STDLIB_H
#define __MADC_POSIX_STDLIB_H 1

#ifndef _WIN32
#error madc POSIX supplements are Win64-only
#endif

#ifdef __cplusplus
extern "C" {
#endif

// POSIX.1-2008 environment mutators. UCRT ships neither name; both are
// implemented in src/rt/rt_posix_env.c over the CRT's own _putenv_s, so a
// setenv() is visible to a later getenv() in the same process.
//
// One documented divergence: the Win32 environment block cannot hold a
// variable whose value is the empty string, so setenv(name, "", 1) REMOVES
// name instead of defining it empty. Representing it would need a shadow
// environment, which is the Cygwin-shaped cost this layer refuses.
int setenv(const char *__name, const char *__value, int __overwrite);
int unsetenv(const char *__name);

#ifdef __cplusplus
}
#endif

#endif
