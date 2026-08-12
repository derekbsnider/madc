#ifndef __MADC_DL_H
#define __MADC_DL_H 1

// madc_dl — the host dynamic-library seam (Windows lane W1).
// One owner for every dlopen/dlsym-family use in the host: call sites name
// the CONCEPT (open mode, symbol scope) and this seam owns the platform
// mapping. POSIX backend in src/madc_dl.cpp; the Win32 backend
// (LoadLibrary/GetProcAddress + a recorded-module walk for the default
// scope) lands behind these same signatures — call sites never test the
// platform. This is NOT the script-facing dlopen()/dlsym() builtin surface
// (the parser.cpp thunks with int64 handles); those adapt onto this seam.

// Load + publish to the default symbol scope (POSIX RTLD_GLOBAL); bind_now
// resolves everything at open (-l fail-fast). NULL on failure — see
// madcdl_error().
void *madcdl_open_global(const char *path, bool bind_now = false);

// Load WITHOUT publishing to the default scope (POSIX RTLD_LAZY, local) —
// the script-level dlopen() contract.
void *madcdl_open_local(const char *path);

// Handle onto the program's own default symbol scope (POSIX dlopen(NULL)).
void *madcdl_open_self(void);

// Handle iff `path` is ALREADY mapped (POSIX RTLD_NOLOAD) — interrogation
// only, never loads; caller madcdl_close()s a non-NULL result.
void *madcdl_probe_loaded(const char *path);

void *madcdl_sym(void *handle, const char *name);

// Default-scope lookup (POSIX dlsym(RTLD_DEFAULT)) — the import resolver's
// probe.
void *madcdl_sym_default(const char *name);

void madcdl_close(void *handle);

// Human-readable description of the last open/sym failure (POSIX dlerror():
// a consuming read; NULL when no error is pending).
const char *madcdl_error(void);

#endif
