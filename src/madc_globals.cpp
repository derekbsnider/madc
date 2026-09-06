#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include "madc_dl.h"
#include "madc_posix_io.h"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

// The weak markers let a unit-test TU's own `bool madc_verbose` definition
// win the link (debug.md convention). PE-COFF weak externals do not compose
// with emutls-backed thread_local definitions — the __emutls_v control
// variable reference stays undefined at link — so the Win arm defines them
// plain (no unit-test binaries link on win64 yet; revisit at their wiring).
#ifdef _WIN32
#define MADC_GLOBAL_WEAK
#else
#define MADC_GLOBAL_WEAK __attribute__((weak))
#endif
thread_local bool madc_verbose MADC_GLOBAL_WEAK = false;
thread_local int madc_opt_level MADC_GLOBAL_WEAK = 1;
thread_local bool madc_debug_info MADC_GLOBAL_WEAK = false;
thread_local bool madc_no_warnings MADC_GLOBAL_WEAK = false;
thread_local bool madc_object_mode MADC_GLOBAL_WEAK = false;

// Resolved absolute path of the running executable (see datadef.h).
std::string madc_self_exe_path()
{
#if defined(_WIN32)
	// NULL module = the process's own executable. A truncated path comes
	// back as n == sizeof(buf) (no error), so treat it as a failure too.
	char buf[4096];
	DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf))
		return std::string();
	std::string real = madc::detail::resolve_real_path(buf);
	return real.empty() ? std::string(buf) : real;
#elif defined(__APPLE__)
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

// <exe dir>/../lib beside the running executable (see datadef.h). The
// separator rule is host_path_dirname's — the one owner, both separators on
// Windows.
std::string madc_self_lib_dir()
{
	std::string dir = madc::detail::host_path_dirname(madc_self_exe_path());
	if (dir.empty())
		return std::string();
	return dir + "../lib";
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

