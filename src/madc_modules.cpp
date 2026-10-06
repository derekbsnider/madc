// madc_modules — module map + the ONE platform-spelling owner. Contract in
// include/madc_modules.h.
#include <stdio.h>
#include <stdint.h>
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
	{ "c", NULL,     "libc.so.6", "libSystem.B.dylib", "ucrtbase.dll", 0 },
	{ "m", "math.h", "libm.so.6", "libSystem.B.dylib", "ucrtbase.dll", 0 },
	{ "madcwebview", "webview.h", "libmadcwebview.so", "libmadcwebview.dylib", "madcwebview.dll", MADC_MODULE_GUI | MADC_MODULE_LAZY },
	// The git substrate (Nexus L4a → the V6 seam): a READ-ONLY view of a
	// local repository over the SYSTEM libgit2 — a dependency of the IDE's
	// nexus, never part of madc (owner ruling 2026-09-15). Built by
	// src/madcgit.mk where pkg-config finds libgit2; the <ns_git> fragment
	// imports it and asks madc::module_available before the first call.
	{ "madcgit", "madcgit.h", "libmadcgit.so", "libmadcgit.dylib", "madcgit.dll", MADC_MODULE_LAZY },
	// GitHub-flavoured Markdown (plan 2026-10-03-chthonia-windows-macos.md
	// §7d): cmark-gfm, a dependency linked statically into the module
	// (src/madcmark.mk); the <ns_markdown> fragment imports it and asks
	// madc::module_available before the first call.
	{ "madcmark", "madcmark.h", "libmadcmark.so", "libmadcmark.dylib", "madcmark.dll", MADC_MODULE_LAZY },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

const MadcModuleSpec *madc_module_find(const std::string &name)
{
	for (int i = 0; madc_modules[i].name; i++)
		if (name == madc_modules[i].name)
			return &madc_modules[i];
	return NULL;
}

const MadcModuleSpec *madc_module_find_spelled(const std::string &spelling)
{
	for (int i = 0; madc_modules[i].name; i++)
		if (spelling == madc_module_library_spelling(madc_modules[i].name))
			return &madc_modules[i];
	return NULL;
}

bool madc_module_any_flagged(unsigned flags)
{
	for (int i = 0; madc_modules[i].name; i++)
		if (madc_modules[i].flags & flags)
			return true;
	return false;
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

const char *madc_target_dso_suffix()
{
	return madc_target_dso_suffix(madc_target_os);
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

bool madc_spelled_library_p(const std::string &name, TargetOS os)
{
	return carries_dso_suffix(name, madc_target_dso_suffix(os));
}

bool madc_spelled_library_p(const std::string &name)
{
	static const TargetOS all[] = { TargetOS::Posix, TargetOS::Darwin, TargetOS::Windows };
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (madc_spelled_library_p(name, all[i]))
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

// A Mach-O dylib's LC_ID_DYLIB, read from the file at `path` ("" = not a
// readable 64-bit little-endian Mach-O dylib). A universal file answers from
// its first slice: a library's slices share one install name.
static uint32_t macho_u32(const unsigned char *p, bool big)
{
	return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
		   : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static std::string macho_dylib_id(const std::string &path)
{
	const uint32_t mh_magic_64 = 0xfeedfacfu, fat_magic = 0xcafebabeu;
	const uint32_t mh_dylib = 6, lc_id_dylib = 0xd;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return "";
	std::string id;
	unsigned char h[32];
	long base = 0;
	if (fread(h, 1, 8, f) == 8 && macho_u32(h, true) == fat_magic
	    && macho_u32(h + 4, true) > 0) {
		unsigned char arch[20];	// the first fat_arch: cputype .. align
		if (fread(arch, 1, sizeof arch, f) == sizeof arch)
			base = (long)macho_u32(arch + 8, true);
	}
	if (fseek(f, base, SEEK_SET) == 0 && fread(h, 1, sizeof h, f) == sizeof h
	    && macho_u32(h, false) == mh_magic_64 && macho_u32(h + 12, false) == mh_dylib) {
		uint32_t ncmds = macho_u32(h + 16, false), sizeofcmds = macho_u32(h + 20, false);
		std::string cmds(sizeofcmds, '\0');
		if (sizeofcmds && fread(&cmds[0], 1, sizeofcmds, f) == sizeofcmds) {
			const unsigned char *c = (const unsigned char *)cmds.data();
			uint32_t off = 0;
			for (uint32_t i = 0; i < ncmds && off + 8 <= sizeofcmds; i++) {
				uint32_t cmd = macho_u32(c + off, false), size = macho_u32(c + off + 4, false);
				if (size < 8 || off + size > sizeofcmds)
					break;
				if (cmd == lc_id_dylib && size >= 24) {
					uint32_t name = macho_u32(c + off + 8, false);
					if (name < size)
						id = std::string((const char *)c + off + name,
								 strnlen((const char *)c + off + name, size - name));
					break;
				}
				off += size;
			}
		}
	}
	fclose(f);
	return id;
}

std::string madc_darwin_install_name(const std::string &spelling)
{
	std::string path = spelling;
	if (!is_path_spelling(spelling)) {
		std::string libdir = madc_self_lib_dir();
		if (libdir.empty())
			return spelling;
		path = libdir + "/" + spelling;
	}
	std::string id = macho_dylib_id(path);
	return id.empty() ? spelling : id;
}

void *madc_module_open(const std::string &spelling, std::string &error,
		       bool bind_now)
{
	error.clear();
	if (!is_path_spelling(spelling)) {
		std::string libdir = madc_self_lib_dir();
		if (!libdir.empty()) {
			std::string beside = libdir + "/" + spelling;
			if (void *h = madcdl_open_global(beside.c_str(), bind_now))
				return h;
			(void)madcdl_error();	// consume; the loader's own search follows
		}
	}
	void *h = madcdl_open_global(spelling.c_str(), bind_now);
	if (!h) {
		const char *e = madcdl_error();
		error = e ? e : "cannot open";
	}
	return h;
}
