// madc_modules — module map + the ONE platform-spelling owner. Contract in
// include/madc_modules.h.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "madc_modules.h"
#include "madc_module.h"	// madc_module_entry_name
#include "madc_dl.h"
#include "madc_posix_io.h"	// resolve_real_path, host_path_dirname
#include "madc_guards.h"	// madc_lift_memory_guard: a GUI module in a module list

// The rows. `c` and `m` name the C runtime images by their REAL sonames /
// install names / module names — a dev-symlink spelling (libm.so) needs the
// -dev package and libc.so is a linker script on glibc, so the -l rule alone
// cannot open them. Darwin keeps both in libSystem; Windows keeps both in
// the UCRT. The interface column is the embedded header `import` tokenizes
// first (NULL = interface-less: the alias form's variadic member convention
// is all there is). A madc MODULE (madc_module.h) has no row: it is found by
// name on the module path and describes itself.
static const MadcModuleSpec madc_modules[] = {
	{ "c", NULL,     "libc.so.6", "libSystem.B.dylib", "ucrtbase.dll", 0 },
	{ "m", "math.h", "libm.so.6", "libSystem.B.dylib", "ucrtbase.dll", 0 },
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
bool madc_spelling_is_path(const std::string &name)
{
	return name.find_first_of("/\\") != std::string::npos;
}

std::string madc_module_library_spelling(const std::string &name, TargetOS os)
{
	const char *sfx = madc_target_dso_suffix(os);
	if (madc_spelling_is_path(name) || carries_dso_suffix(name, sfx))
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

// A madc MODULE's file (madc_module.h): its name and the target's library
// suffix, no lib prefix — modules live in their own directories, apart from
// the system's libraries, so a short name never meets a system lib<name>.
std::string madc_module_file_spelling(const std::string &module)
{
	return module + madc_target_dso_suffix();
}

std::string madc_module_dir()
{
	std::string libdir = madc_self_lib_dir();
	if (libdir.empty())
		return "";
	return madc::detail::host_path_join(madc::detail::host_path_join(libdir, "madc"), "modules");
}

std::string madc_module_find_file(const std::string &module, const std::vector<std::string> &dirs)
{
	std::string file = madc_module_file_spelling(module);
	std::vector<std::string> search(dirs);
	std::string own = madc_module_dir();
	if (!own.empty())
		search.push_back(own);
	for (size_t i = 0; i < search.size(); i++) {
		if (search[i].empty())
			continue;
		std::string path = madc::detail::host_path_join(search[i], file);
		if (madc::detail::host_path_type_of(path) == madc::detail::host_path_type::file)
			return path;
	}
	return "";
}

// The module list's entry spellings (madc_modules.h): a link library is its
// spelling; an optional one '?' and its spelling; a madc module '@', 'g' (a
// GUI module) or '-', its name, '=', its file.
void madc_module_deps_append(std::string &table, madc_module_dep kind,
			     const std::string &spelling, const std::string &name, bool gui)
{
	switch (kind) {
	case madc_module_dep::link:
		break;
	case madc_module_dep::optional:
		table += '?';
		break;
	case madc_module_dep::module:
		table += '@';
		table += gui ? 'g' : '-';
		table += name;
		table += '=';
		break;
	}
	table += spelling;
	table.push_back('\0');
}

bool madc_module_deps_open(const char *table, const char *end, std::string &error)
{
	error.clear();
	for (const char *p = table; p && (!end || p < end) && *p; ) {
		size_t n = end ? strnlen(p, (size_t)(end - p)) : strlen(p);
		std::string entry(p, n);
		p += n + 1;
		std::string why;
		if (entry[0] == '?') {
			madc_module_open(entry.substr(1), why);	// optional: absent is fine
			continue;
		}
		if (entry[0] != '@') {
			if (!madc_module_open(entry, why)) {
				error = "import: cannot load '" + entry + "': " + why;
				return false;
			}
			continue;
		}
		size_t eq = entry.find('=');
		std::string name = eq == std::string::npos ? std::string() : entry.substr(2, eq - 2);
		std::string file = eq == std::string::npos ? std::string() : entry.substr(eq + 1);
		// Its own load may reserve address space far beyond any program
		// budget (WebKit's): the guard lifts BEFORE it opens.
		if (entry.size() > 1 && entry[1] == 'g')
			madc_lift_memory_guard("a GUI module in the module list");
		if (file.empty() || !madc_module_open(file, why)) {
			error = "import: module '" + name + "': " + (file.empty() ? "no file recorded" : why);
			return false;
		}
	}
	return true;
}

std::string madc_module_entry_name(const std::string &module)
{
	std::string entry = "madc_module_" + module;
	for (size_t i = 0; i < entry.size(); i++)
		if (entry[i] == '.')
			entry[i] = '_';
	return entry;
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
	if (!madc_spelling_is_path(spelling)) {
		std::string libdir = madc_self_lib_dir();
		if (libdir.empty())
			return spelling;
		path = libdir + "/" + spelling;
	}
	std::string id = macho_dylib_id(path);
	return id.empty() ? spelling : id;
}

std::string madc_darwin_link_rpath(const std::string &spelling)
{
	if (!madc_spelling_is_path(spelling)
	    || macho_dylib_id(spelling).compare(0, 7, "@rpath/") != 0)
		return "";
	// The REAL directory: a versioned link (libx.dylib -> libx.1.dylib)
	// names the file the install name's <file> is, wherever it lives.
	std::string dir = madc::detail::host_path_dirname(
		madc::detail::resolve_real_path(spelling.c_str()));
	if (dir.size() > 1)
		dir.pop_back();	// the splitter keeps the trailing separator
	return dir;
}

// What ld's reading of a library comes to for a run: its shared objects
// opened in order (the first one's handle answers), an empty archive the
// process's own scope, a static archive refused.
static void *open_link_result(madc_link_kind kind, const std::vector<madc_link_image> &images,
			      const std::string &file, bool bind_now, std::string &error)
{
	switch (kind) {
	case madc_link_kind::images: {
		void *first = NULL;
		for (const madc_link_image &im : images) {
			void *h = madcdl_open_global(im.path.c_str(), bind_now);
			if (!h) {
				const char *e = madcdl_error();
				error = e ? e : "cannot open";
				return NULL;
			}
			if (!first)
				first = h;
		}
		return first;
	}
	case madc_link_kind::none:
		return madcdl_open_self();
	case madc_link_kind::archive:
		error = file + ": a static archive (madc links shared libraries)";
		return NULL;
	case madc_link_kind::unresolved:
		break;
	}
	return NULL;
}

void *madc_module_open(const std::string &spelling, std::string &error,
		       bool bind_now)
{
	error.clear();
	// ld's reading of the library, on an ELF host: a path's file at once (a
	// linker script or an archive is no dlopen input); a bare spelling only
	// when the loader cannot open it (a link-time name: no dev symlink, a
	// script, a stub archive).
	bool elf = madc_target_os == TargetOS::Posix;
	bool path = madc_spelling_is_path(spelling);
	std::vector<madc_link_image> images;
	std::string file;
	if (!path) {
		std::string libdir = madc_self_lib_dir();
		if (!libdir.empty()) {
			std::string beside = libdir + "/" + spelling;
			if (void *h = madcdl_open_global(beside.c_str(), bind_now))
				return h;
			(void)madcdl_error();	// consume; the loader's own search follows
		}
	} else if (elf) {
		madc_link_kind k = madc_module_link_images(spelling,
				madc_link_search_path(std::vector<std::string>()), images, file);
		if (k != madc_link_kind::unresolved)
			return open_link_result(k, images, file, bind_now, error);
	}
	void *h = madcdl_open_global(spelling.c_str(), bind_now);
	if (h)
		return h;
	const char *e = madcdl_error();
	std::string loader_error = e ? e : "cannot open";
	if (elf && !path) {
		madc_link_kind k = madc_module_link_images(spelling,
				madc_link_search_path(std::vector<std::string>()), images, file);
		if (k != madc_link_kind::unresolved)
			return open_link_result(k, images, file, bind_now, error);
	}
	error = loader_error;
	return NULL;
}
