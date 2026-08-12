// Host dynamic-library seam — POSIX backend. Contract in include/madc_dl.h;
// the Win32 backend is a W1 deliverable of the Windows release lane
// (docs/plans/2026-08-12-windows-release-lane.md).
#include <stddef.h>
#include <dlfcn.h>
#include "madc_dl.h"

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
