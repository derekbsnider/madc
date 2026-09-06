// madc_modules — module map + the ONE platform-spelling owner. Contract in
// include/madc_modules.h.
#include <string.h>
#include "madc_modules.h"
#include "madc_dl.h"

// The rows. `c` and `m` name the C runtime images by their REAL sonames /
// install names / module names — a dev-symlink spelling (libm.so) needs the
// -dev package and libc.so is a linker script on glibc, so the -l rule alone
// cannot open them. Darwin keeps both in libSystem; Windows keeps both in
// the UCRT. The interface column is the embedded header `import` tokenizes
// first (NULL = interface-less: the alias form's variadic member convention
// is all there is).
static const MadcModuleSpec madc_modules[] = {
	{ "c", NULL,     "libc.so.6", "libSystem.B.dylib", "ucrtbase.dll" },
	{ "m", "math.h", "libm.so.6", "libSystem.B.dylib", "ucrtbase.dll" },
	{ NULL, NULL, NULL, NULL, NULL }
};

const MadcModuleSpec *madc_module_find(const std::string &name)
{
	for (int i = 0; madc_modules[i].name; i++)
		if (name == madc_modules[i].name)
			return &madc_modules[i];
	return NULL;
}

const char *madc_target_dso_suffix(TargetOS os)
{
	switch (os) {
	case TargetOS::Darwin:  return ".dylib";
	case TargetOS::Windows: return ".dll";
	case TargetOS::Posix:   return ".so";
	}
	return ".so";
}

// "Already spelled": the name ends with the target's suffix (libfoo.so) or
// carries it as an inner component (a versioned soname, libfoo.so.2) — ld's
// -l rule wraps neither.
static bool carries_dso_suffix(const std::string &name, const char *sfx)
{
	size_t n = strlen(sfx);
	if (name.size() >= n && name.compare(name.size() - n, n, sfx) == 0)
		return true;
	return name.find(std::string(sfx) + ".") != std::string::npos;
}

bool madc_spelled_library_p(const std::string &name)
{
	static const TargetOS all[] = { TargetOS::Posix, TargetOS::Darwin, TargetOS::Windows };
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (carries_dso_suffix(name, madc_target_dso_suffix(all[i])))
			return true;
	return false;
}

// Char-level twin of host_path_last_separator (madc_posix_io.cpp): a TARGET
// spelling may carry either separator whatever the host, so both mark a
// path here.
static bool is_path_spelling(const std::string &name)
{
	return name.find_first_of("/\\") != std::string::npos;
}

std::string madc_module_library_spelling(const std::string &name, TargetOS os)
{
	const char *sfx = madc_target_dso_suffix(os);
	if (is_path_spelling(name) || carries_dso_suffix(name, sfx))
		return name;
	if (const MadcModuleSpec *row = madc_module_find(name)) {
		switch (os) {
		case TargetOS::Darwin:  return row->darwin;
		case TargetOS::Windows: return row->windows;
		case TargetOS::Posix:   return row->posix;
		}
	}
	if (os == TargetOS::Windows)
		return name + sfx;
	return "lib" + name + sfx;
}

std::string madc_module_library_spelling(const std::string &name)
{
	return madc_module_library_spelling(name, madc_target_os);
}

void *madc_module_open(const std::string &spelling, std::string &error)
{
	error.clear();
	if (!is_path_spelling(spelling)) {
		std::string libdir = madc_self_lib_dir();
		if (!libdir.empty()) {
			std::string beside = libdir + "/" + spelling;
			if (void *h = madcdl_open_global(beside.c_str()))
				return h;
			(void)madcdl_error();	// consume; the loader's own search follows
		}
	}
	void *h = madcdl_open_global(spelling.c_str());
	if (!h) {
		const char *e = madcdl_error();
		error = e ? e : "cannot open";
	}
	return h;
}
