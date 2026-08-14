// SPDX-License-Identifier: MPL-2.0
// Win64 POSIX dynamic-loading compatibility runtime.
//
// This is a strict-C11 dual-build source: the hosted compiler puts it in
// libmadc/libmadc_rt, and the same source can become an AOT-ledger module.
// Keep it free of compiler builtins and C++ runtime dependencies.
//
// This does NOT call madc's own madcdl_* seam. That seam is L0 — madc's host
// C++ calling the OS — while this is L3, the shim COMPILED USER PROGRAMS call
// (docs/plans/2026-08-13-posix-target-surface.md §3: "They may share
// technique; they never share code paths, because L3 must link into an
// emitted-C program that contains no madc C++ at all"). An emitted-C program
// links libmadc_rt.a ALONE, so a forward to madcdl_open_global would be an
// unresolved symbol there. Same technique, separate owner, by design.

#if !defined(_WIN32) || !defined(_WIN64)
#error "rt_posix_dl.c is a Win64-only runtime source"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <psapi.h>	/* EnumProcessModules -> K32EnumProcessModules (kernel32) */
#include <stddef.h>
// Its own public header, after windows.h: the RTLD_* values have ONE
// definition, and the compiler checks these definitions against the
// declarations every consumer sees.
#include <madc/posix/dlfcn.h>

/* dlopen(NULL) names the main program. POSIX requires a usable handle, and
 * GetModuleHandle(NULL) is exactly that, so no sentinel is needed. */

/* Per-PROCESS error state, not per-thread: a _Thread_local here would pull an
 * emutls dependency into an archive that must link with no extra libraries.
 * POSIX permits the error state to be process-wide; dlerror() still clears. */
static char dl_error_text[512];
static int dl_error_live;

static void dl_clear_error(void)
{
	dl_error_live = 0;
	dl_error_text[0] = '\0';
}

/* Compose "<op>[ '<detail>']: Win32 error <code>" without any string or memory
 * helper from libc, since ledger membership forbids anything madc lowers to a
 * builtin that would call straight back into this archive. */
static void dl_set_error(const char *op, const char *detail, DWORD code)
{
	size_t at = 0;
	unsigned long value;
	char digits[16];
	size_t ndigits = 0;
	const char *p;

	for (p = op; p != NULL && *p != '\0' && at + 1 < sizeof(dl_error_text); ++p)
		dl_error_text[at++] = *p;
	if (detail != NULL && *detail != '\0') {
		if (at + 2 < sizeof(dl_error_text)) {
			dl_error_text[at++] = ' ';
			dl_error_text[at++] = '\'';
		}
		for (p = detail; *p != '\0' && at + 1 < sizeof(dl_error_text); ++p)
			dl_error_text[at++] = *p;
		if (at + 1 < sizeof(dl_error_text))
			dl_error_text[at++] = '\'';
	}
	for (p = ": Win32 error "; *p != '\0' && at + 1 < sizeof(dl_error_text); ++p)
		dl_error_text[at++] = *p;

	value = (unsigned long)code;
	do {
		digits[ndigits++] = (char)('0' + (int)(value % 10UL));
		value /= 10UL;
	} while (value != 0UL && ndigits < sizeof(digits));
	while (ndigits > 0 && at + 1 < sizeof(dl_error_text))
		dl_error_text[at++] = digits[--ndigits];

	dl_error_text[at] = '\0';
	dl_error_live = 1;
}

void *dlopen(const char *file, int mode)
{
	HMODULE handle;

	(void)mode;	/* Win32 has no lazy/eager or global/local distinction. */
	dl_clear_error();
	if (file == NULL) {
		handle = GetModuleHandleA(NULL);
		if (handle == NULL)
			dl_set_error("dlopen", NULL, GetLastError());
		return (void *)handle;
	}
	handle = LoadLibraryA(file);
	if (handle == NULL)
		dl_set_error("dlopen", file, GetLastError());
	return (void *)handle;
}

/* RTLD_DEFAULT: search every module currently mapped into the process, main
 * image first. This is the real loaded-module set, not a subset this shim
 * happens to have opened. */
static void *dl_sym_default(const char *name)
{
	HMODULE modules[256];
	DWORD needed = 0;
	DWORD count;
	DWORD i;
	FARPROC address;

	address = GetProcAddress(GetModuleHandleA(NULL), name);
	if (address != NULL)
		return (void *)address;
	if (!EnumProcessModules(GetCurrentProcess(), modules,
				(DWORD)sizeof(modules), &needed))
		return NULL;
	count = needed / (DWORD)sizeof(HMODULE);
	if (count > (DWORD)(sizeof(modules) / sizeof(modules[0])))
		count = (DWORD)(sizeof(modules) / sizeof(modules[0]));
	for (i = 0; i < count; ++i) {
		address = GetProcAddress(modules[i], name);
		if (address != NULL)
			return (void *)address;
	}
	return NULL;
}

void *dlsym(void *handle, const char *name)
{
	FARPROC address;

	dl_clear_error();
	if (name == NULL) {
		dl_set_error("dlsym", NULL, (DWORD)ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (handle == RTLD_DEFAULT) {
		void *found = dl_sym_default(name);

		if (found == NULL)
			dl_set_error("dlsym", name, (DWORD)ERROR_PROC_NOT_FOUND);
		return found;
	}
	address = GetProcAddress((HMODULE)handle, name);
	if (address == NULL)
		dl_set_error("dlsym", name, GetLastError());
	return (void *)address;
}

int dlclose(void *handle)
{
	dl_clear_error();
	/* The main-program handle is not a reference this shim owns. */
	if (handle == NULL || handle == (void *)GetModuleHandleA(NULL))
		return 0;
	if (!FreeLibrary((HMODULE)handle)) {
		dl_set_error("dlclose", NULL, GetLastError());
		return -1;
	}
	return 0;
}

char *dlerror(void)
{
	if (!dl_error_live)
		return NULL;
	dl_error_live = 0;	/* POSIX: the error is consumed by the read. */
	return dl_error_text;
}
