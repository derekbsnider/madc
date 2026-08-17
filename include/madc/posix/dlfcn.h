// SPDX-License-Identifier: MPL-2.0
///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Win64 POSIX <dlfcn.h>. Unlike the other members of this directory this //
// is a WHOLE provider, not a supplement: mingw-w64 ships no <dlfcn.h> at  //
// all, so there is no native header to augment and nothing to shadow.    //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_POSIX_DLFCN_H
#define __MADC_POSIX_DLFCN_H 1

#ifndef _WIN32
#error madc POSIX supplements are Win64-only
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Win32's loader has no lazy/eager distinction and no per-object symbol
// visibility, so these are accepted and documented rather than emulated:
// LAZY/NOW are equivalent, and GLOBAL/LOCAL do not change what dlsym() can
// reach. They exist so POSIX source compiles unchanged.
#define RTLD_LAZY	0x0001
#define RTLD_NOW	0x0002
#define RTLD_GLOBAL	0x0100
#define RTLD_LOCAL	0x0000

// dlsym()'s pseudo-handles. RTLD_DEFAULT is a null handle, as on glibc, so
// `dlsym(RTLD_DEFAULT, name)` searches every module loaded in the process.
#define RTLD_DEFAULT	((void *)0)

void *dlopen(const char *__file, int __mode);
void *dlsym(void *__handle, const char *__name);
int   dlclose(void *__handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
