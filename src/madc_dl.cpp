// Host dynamic-library seam — platform backends. Contract in
// include/madc_dl.h; W1 of the Windows release lane
// (docs/plans/2026-08-12-windows-release-lane.md).
#include <stddef.h>
#include "madc_dl.h"

#ifndef _WIN32

// ---------------------------------------------------------------- POSIX ---
#include <dlfcn.h>

void *madcdl_open_global(const char *path, bool bind_now)
{
	return dlopen(path, (bind_now ? RTLD_NOW : RTLD_LAZY) | RTLD_GLOBAL);
}

void *madcdl_open_local(const char *path)
{
	return dlopen(path, RTLD_LAZY);
}

void *madcdl_open_self(void)
{
	return dlopen(NULL, RTLD_LAZY | RTLD_GLOBAL);
}

void *madcdl_probe_loaded(const char *path)
{
	return dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
}

void *madcdl_sym(void *handle, const char *name)
{
	return dlsym(handle, name);
}

void *madcdl_sym_default(const char *name)
{
	return dlsym(RTLD_DEFAULT, name);
}

void madcdl_close(void *handle)
{
	if (handle)
		dlclose(handle);
}

const char *madcdl_error(void)
{
	return dlerror();
}

bool madcdl_addr(const void *addr, MadcDlInfo &info)
{
	Dl_info di;
	if (!dladdr(addr, &di))
		return false;
	info.fname = di.dli_fname;
	info.fbase = di.dli_fbase;
	info.sname = di.dli_sname;
	info.saddr = di.dli_saddr;
	return true;
}

#else

// ---------------------------------------------------------------- Win32 ---
// PE has no default symbol scope: madcdl_open_global RECORDS each loaded
// module and madcdl_sym_default walks self-exe -> recorded modules ->
// libstdc++-6.dll -> libwinpthread-1.dll -> ucrtbase -> kernel32, in that
// order — the same resolution surface the W2 c2m driver binds against.
// madc's own statics need -Wl,--export-all-symbols to be visible to
// GetProcAddress (the hosted-windows MODE's job), but that flag CANNOT
// serve the C++ runtime: PE ld auto-excludes runtime archives from it, so
// a -static libstdc++ exposes no _Z* symbols and every script-side
// mangled-direct std:: import goes undefined. The hosted MODE links
// libstdc++ (and winpthread — one instance, pthread objects cross the
// exe<->DLL boundary via std::thread) SHARED from the UCRT stage, and the
// walk reads the DLLs' own export tables — the darwin flat-bind analogue.
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#include "madc_posix_io.h"	// win_error_text — the one Win32-error formatter
#include "mir-mingw-stdio.h"	// the ONE ANSI-stdio interposer map (fork-owned)

static std::vector<HMODULE> &madcdl_global_modules(void)
{
	static std::vector<HMODULE> mods;
	return mods;
}

static std::mutex &madcdl_lock(void)
{
	static std::mutex m;
	return m;
}

// dlerror() emulation: a consuming per-thread read, set by every failing
// open/sym, cleared by madcdl_error().
static thread_local char madcdl_errbuf[512];
static thread_local bool madcdl_err_pending = false;

static void madcdl_set_error(const char *op, const char *detail)
{
	DWORD code = GetLastError();
	std::string msg = madc::detail::win_error_text(code);
	snprintf(madcdl_errbuf, sizeof(madcdl_errbuf), "%s%s%s: %s (error %lu)",
		 op, detail ? " " : "", detail ? detail : "", msg.c_str(),
		 (unsigned long)code);
	madcdl_err_pending = true;
}

void *madcdl_open_global(const char *path, bool bind_now)
{
	(void)bind_now;	// PE binds imports at load; NOW vs LAZY has no analogue
	HMODULE h = LoadLibraryA(path);
	if (!h) {
		madcdl_set_error("LoadLibrary", path);
		return NULL;
	}
	std::lock_guard<std::mutex> g(madcdl_lock());
	madcdl_global_modules().push_back(h);
	return (void *)h;
}

void *madcdl_open_local(const char *path)
{
	HMODULE h = LoadLibraryA(path);
	if (!h)
		madcdl_set_error("LoadLibrary", path);
	return (void *)h;
}

void *madcdl_open_self(void)
{
	return (void *)GetModuleHandleA(NULL);
}

void *madcdl_probe_loaded(const char *path)
{
	// GET_MODULE_HANDLE_EX default (no flags) takes a reference, matching
	// the POSIX contract: a non-NULL result is madcdl_close()d by the
	// caller. Matches by the loader's name rules (best effort vs soname
	// spellings — a cover list naming an ELF soname simply never matches
	// here, which reads as "not loaded", the safe answer).
	HMODULE h = NULL;
	if (!GetModuleHandleExA(0, path, &h))
		return NULL;
	return (void *)h;
}

void *madcdl_sym(void *handle, const char *name)
{
	void *p = (void *)GetProcAddress((HMODULE)handle, name);
	if (!p)
		madcdl_set_error("GetProcAddress", name);
	return p;
}

void *madcdl_sym_default(const char *name)
{
	// gcc parity first: on this host flavor the process's printf/scanf
	// family IS the statically linked __mingw_* interposer set, and no
	// module walk can see it (UCRT doesn't export the names; PE ld
	// excludes the mingw runtime archives from --export-all-symbols).
	// mir-mingw-stdio.h is the one owner of that name list — both the
	// parse-time existence probe and JIT import resolution land here.
#if defined(__MINGW32__) && __USE_MINGW_ANSI_STDIO
	if (void *pre = mir_mingw_ansi_stdio_lookup(name))
		return pre;
#endif
	FARPROC p = GetProcAddress(GetModuleHandleA(NULL), name);
	if (p)
		return (void *)p;
	{
		std::lock_guard<std::mutex> g(madcdl_lock());
		for (HMODULE h : madcdl_global_modules())
			if ((p = GetProcAddress(h, name)) != NULL)
				return (void *)p;
	}
	// Runtime DLLs before the CRT pair: libstdc++-6.dll is the whole
	// mangled-direct std:: surface (its export table plays the role of
	// libstdc++.so's dynsym under Linux dlsym(RTLD_DEFAULT)); winpthread
	// gives script-side pthread_* the same way (the exe only IMPORTS
	// those names, and GetProcAddress on self cannot see imports). No
	// name overlaps with ucrtbase — the order is Linux load-order parity,
	// not a tie-break.
	static const char *const crt_mods[] = { "libstdc++-6.dll",
						"libwinpthread-1.dll",
						"ucrtbase.dll", "kernel32.dll",
						NULL };
	for (int i = 0; crt_mods[i]; i++) {
		HMODULE h = GetModuleHandleA(crt_mods[i]);
		if (h && (p = GetProcAddress(h, name)) != NULL)
			return (void *)p;
	}
	return NULL;
}

void madcdl_close(void *handle)
{
	if (!handle)
		return;
	if ((HMODULE)handle == GetModuleHandleA(NULL))
		return;	// the self pseudo-handle is not a LoadLibrary ref
	FreeLibrary((HMODULE)handle);
}

const char *madcdl_error(void)
{
	if (!madcdl_err_pending)
		return NULL;
	madcdl_err_pending = false;
	return madcdl_errbuf;
}

bool madcdl_addr(const void *addr, MadcDlInfo &info)
{
	// Owning module via the address's allocation base; no symbol lookup
	// without DbgHelp, so sname/saddr stay NULL (every consumer guards).
	HMODULE h = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
				  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)addr, &h))
		return false;
	static thread_local char fname[MAX_PATH];
	if (!GetModuleFileNameA(h, fname, sizeof(fname)))
		return false;
	info.fname = fname;
	info.fbase = (void *)h;	// HMODULE == the module's base address
	info.sname = NULL;
	info.saddr = NULL;
	return true;
}

#endif
