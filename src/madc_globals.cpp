#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include "madc_dl.h"
#include "madc_posix_io.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

thread_local bool madc_verbose __attribute__((weak)) = false;
thread_local int madc_opt_level __attribute__((weak)) = 1;
thread_local bool madc_debug_info __attribute__((weak)) = false;
thread_local bool madc_object_mode __attribute__((weak)) = false;

// Resolved absolute path of the running executable (see datadef.h).
std::string madc_self_exe_path()
{
#ifdef __APPLE__
	char buf[4096];
	uint32_t sz = sizeof(buf);
	if (_NSGetExecutablePath(buf, &sz) != 0)
		return std::string();
	std::string real = madc::detail::resolve_real_path(buf);
	return real.empty() ? std::string(buf) : real;
#else
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0)
		return std::string();
	buf[n] = '\0';
	return std::string(buf);
#endif
}

// Resolved absolute path of the image holding libmadc's code (see datadef.h).
// This function itself is the probe symbol: it is compiled into libmadc, so
// dladdr maps it to libmadc.so / madc.dylib in the shared shape and to the
// executable in the monolithic one (a static libmadc IS the exe's text).
std::string madc_self_lib_path()
{
	MadcDlInfo info;
	if (!madcdl_addr((void *)&madc_self_lib_path, info)
	    || !info.fname || !info.fname[0])
		return std::string();
	std::string real = madc::detail::resolve_real_path(info.fname);
	return real.empty() ? std::string(info.fname) : real;
}

