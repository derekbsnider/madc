// SPDX-License-Identifier: MPL-2.0
///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Win64 POSIX supplement for the platform's real <string.h>.            //
// Additive declarations only; the native header is always served first. //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_POSIX_STRING_H
#define __MADC_POSIX_STRING_H 1

#ifndef _WIN32
#error madc POSIX supplements are Win64-only
#endif

#ifdef __cplusplus
extern "C" {
#endif

char *strndup(const char *s, size_t size);

#ifdef __cplusplus
}
#endif

#endif
