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

// Win32 only (PE has no default symbol scope): the process's runtime DLLs
// the default-scope walk spans after the self-exe and the recorded modules,
// in resolution order, NULL-terminated — libstdc++-6, libwinpthread-1,
// ucrtbase, kernel32, ws2_32. ONE list: the JIT's symbol walk, the native
// runtime-need cover set and the PE writer's import-attribution order all
// read it (a second copy in the writer had drifted). Undefined on POSIX
// builds — nothing there names it.
const char *const *madcdl_default_scope_modules(void);

void madcdl_close(void *handle);

// Human-readable description of the last open/sym failure (POSIX dlerror():
// a consuming read; NULL when no error is pending).
const char *madcdl_error(void);

// Resolved image/symbol info for a code/data address (POSIX dladdr). The
// Win32 backend fills fname/fbase from the owning module (VirtualQuery +
// GetModuleFileName); sname/saddr may be NULL there — consumers already
// guard for absent symbol names.
struct MadcDlInfo {
	const char *fname;	// defining image path
	void       *fbase;	// image base address
	const char *sname;	// nearest symbol name (may be NULL)
	void       *saddr;	// nearest symbol address (may be NULL)
};

// False when no loaded image owns `addr`; fields are valid only on true.
// Async-signal-safe to the same degree as the platform call (the madc
// crash handler symbolizes through this).
bool madcdl_addr(const void *addr, MadcDlInfo &info);

#endif
