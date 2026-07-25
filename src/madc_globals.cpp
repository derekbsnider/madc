#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
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
	char real[PATH_MAX];
	if (realpath(buf, real))
		return std::string(real);
	return std::string(buf);
#else
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0)
		return std::string();
	buf[n] = '\0';
	return std::string(buf);
#endif
}

