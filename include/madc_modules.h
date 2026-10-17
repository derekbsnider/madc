#ifndef __MADC_MODULES_H
#define __MADC_MODULES_H 1

// madc_modules — the module map and the ONE platform-spelling owner
// (design: docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md §3.1).
// `import name;` (source), `-l<name>` (build line) and the native lanes'
// link closure all resolve a MODULE or bare library NAME here; no other
// file spells .so / .dylib / .dll or the lib prefix.
//
// A module row names an optional INTERFACE (an embedded header the import
// tokenizes before binding) and the runtime image per target OS. A name
// with no row follows the platform rule: lib<name>.so / lib<name>.dylib /
// <name>.dll. A name containing a path separator, or already carrying the
// target's suffix (libfoo.so, libfoo.so.2), is verbatim (the -l contract).
//
// THREAD-SAFETY CONTRACT: the map is a constant table; the functions are
// pure except madc_module_open, which is the madc_dl seam's contract, and
// madc_darwin_install_name / madc_darwin_link_rpath, which read a file and
// share no state.

#include <string>
#include <vector>
#include "datadef.h"	// TargetOS, madc_target_os

struct MadcModuleSpec {
	const char *name;
	const char *interface;	// embedded header name, or NULL (interface-less)
	const char *posix;	// ELF runtime image spelling (the real soname)
	const char *darwin;	// Mach-O install name (bare file name)
	const char *windows;	// PE module name
};

const MadcModuleSpec *madc_module_find(const std::string &name);
const char *madc_target_dso_suffix(TargetOS os);
const char *madc_target_dso_suffix();	// for madc_target_os (madc::library_suffix)
// True when `name` already carries SOME target's library suffix (libc.so.6,
// libfoo.so, libSystem.B.dylib, ucrtbase.dll) — as opposed to a bare stem
// (libc++, libsystem_: the darwin cover prefixes) or a module name. The one
// "is this spelled?" test for consumers that read recorded spellings of any
// platform (the native cover analysis).
bool madc_spelled_library_p(const std::string &name);
// The per-target form: spelled for THIS os (libfoo.dylib is a Darwin
// spelling; libm.so.6 is not). A Linux-hosted darwin cross madc runs its
// cover analysis over the HOST's ELF images, so a Mach-O writer asking
// "which of these are the target's user libraries" must ask per target.
bool madc_spelled_library_p(const std::string &name, TargetOS os);
std::string madc_module_library_spelling(const std::string &name, TargetOS os);
std::string madc_module_library_spelling(const std::string &name);	// for madc_target_os
// A madc MODULE (madc_module.h) — found by NAME, apart from the system's
// libraries: its file is the name and the target's suffix, no lib prefix
// (markdown.so / markdown.dylib / markdown.dll). madc's own module directory
// is <madc's lib directory>/madc/modules — the directory holding libmadc's
// image when libmadc is a library of its own (so a native program finds the
// modules of the madc it loads), else <the running binary's dir>/../lib; ""
// when madc cannot place it. madc_module_find_file: the first of `dirs`, then madc's own
// directory, that holds the module's file — its path, or "" (no module of
// that name: the import's "no module named").
std::string madc_module_file_spelling(const std::string &module);
std::string madc_module_dir();
std::string madc_module_find_file(const std::string &module, const std::vector<std::string> &dirs);

// The exit status of an import that fails before a program can run: `madc
// --import=NAME` with no source, and a native program whose module cannot
// open at start — its own, so a launcher tells "not installed" from any
// other failure (sysexits.h's EX_UNAVAILABLE; spelled here because not every
// target has <sysexits.h>).
const int madc_import_unavailable_status = 69;

// A madc MODULE's REFERENCE — what a program records of a module it imported
// (its module list's entry, a member call's library): '@', 'g' (a GUI module)
// or '-', the module's name, '=', the file its import opened (absolute).
std::string madc_module_reference(const std::string &name, const std::string &file,
				  bool gui);
// Open a module by its reference where the program RUNS: the file it was built
// against while that file is there, else the module of that name in the
// program's own directory, then madc's module directory (Python's sys.path) —
// its description checked (madc_module_describe). A GUI module lifts an armed
// memory guard before it opens. NULL + `error` (the import's error) when none
// opens.
void *madc_module_open_reference(const std::string &reference, std::string &error);
// The description `handle` (the file `path`) exports as the module `name`: its
// entry point found, its ABI this madc's, its name `name`. The import's check
// and a program's at start. NULL + `error` (the reason).
struct MadcModuleDescription;
const MadcModuleDescription *madc_module_describe(void *handle, const std::string &name,
						  const std::string &path, std::string &error);

// The module list a unit carries (__madc_module_deps — the CIR builder writes
// it; the single-object loader opens an object's before load, and a program's
// own init opens its own before its other initializers): entries
// NUL-separated, the list double-NUL-terminated. An entry is a link-form
// library's TARGET spelling (its DT_NEEDED twin) or a madc module's reference,
// opened at program start (madc_module_open_reference) so its initializer runs
// before main (a host registers); both are required.
void madc_module_deps_append(std::string &table, const std::string &entry);
// Open every entry of a module list, in order; `end` bounds it (NULL: its
// double NUL does). False + `error` (the import error's text) at the first
// required entry that cannot open.
bool madc_module_deps_open(const char *table, const char *end, std::string &error);

// Open a spelled library through the madc_dl seam into the default symbol
// scope: a bare spelling is tried beside the running binary first
// (<exe dir>/../lib/<spelling>, the relocatable install shape the runpath
// uses), then as the loader itself searches, then — on an ELF host, where a
// generic lib<name>.so is a link-time name the loader may not know — as ld
// searches (madc_module_link_images over the toolchain's directories). A
// library FILE opens as ld reads it (madc_module_link_file): a linker script
// opens its shared members; an archive with no members is the process's own
// scope. `bind_now` resolves every reference at open (the command line's -l:
// fail-fast). The one opener for `import`, a manifest's "libs", -l and a
// frozen forest's recorded libraries. NULL + `error` on failure.
void *madc_module_open(const std::string &spelling, std::string &error,
		       bool bind_now = false);

// Does a TARGET library spelling name a path (either separator, whatever the
// host) rather than a name a search finds?
bool madc_spelling_is_path(const std::string &spelling);

// What ld links for a library, on an ELF target (src/madc_link_files.cpp):
// the -l search and the files it reads. A library file is a shared object (DT_NEEDED records its
// DT_SONAME, else the name it was found by), a linker script (INPUT / GROUP:
// each shared member, -l members searched again; AS_NEEDED lists and archive
// members are not recorded), or an archive.
struct madc_link_image
{
	std::string path;	// the shared object's file
	std::string needed;	// what DT_NEEDED records for it
};
enum class madc_link_kind : unsigned char
{
	unresolved,	// no library file here: the caller keeps the spelling
	images,		// shared objects, in `images`
	none,		// an archive with no members (glibc's libdl.a, libpthread.a:
			// their symbols are libc's): nothing to record or open
	archive		// a static archive with members: madc links no archives
};

// The file at `path`, read as ld reads a library; `named` is what DT_NEEDED
// records for a shared object with no DT_SONAME (the path a program gave, or
// the name a search found it by). `dirs` serve a script's -l members.
madc_link_kind madc_module_link_file(const std::string &path, const std::string &named,
				     const std::vector<std::string> &dirs,
				     std::vector<madc_link_image> &images);

// What ld links for the library `spelling` names: a path, its file
// (madc_module_link_file, recording the path for a shared object with no
// DT_SONAME); the generic lib<name>.so (the -l rule for a name no module row
// spells), the -l search — each of `dirs` in order, lib<name>.so before
// lib<name>.a within one, an ELF file of another class passed over. Any other
// spelling (a module row's, a versioned name) is `unresolved`. `file` is the
// library file it settled on.
madc_link_kind madc_module_link_images(const std::string &spelling,
				       const std::vector<std::string> &dirs,
				       std::vector<madc_link_image> &images,
				       std::string &file);

// The directories `cc -l` searches: `cli_dirs` (the command line's -L, in
// order), then the toolchain's (madc_toolchain_library_dirs, probed at build).
std::vector<std::string> madc_link_search_path(const std::vector<std::string> &cli_dirs);

// The library file the -l search over `dirs` alone settles on for `spelling`
// (madc_module_link_images), else `spelling` unchanged. A path spelling is
// never searched. The command line's -L for a run: the file found opens
// through madc_module_open, which reads it as ld would.
std::string madc_module_locate(const std::string &spelling,
			       const std::vector<std::string> &search_dirs);

// The load command a Mach-O image linking `spelling` records — the dylib's
// own install name (LC_ID_DYLIB), as ld64 records it — read from the file
// madc_module_open opens first: beside the running binary for a bare
// spelling, the path itself for a path spelling. Every library madc builds
// or ships names itself "@rpath/<file>", so a program linking one loads it
// through the runpath's LC_RPATHs from a relocatable install. A file madc
// cannot read (a system dylib in the dyld shared cache, a cross host without
// it) answers the spelling: dyld's default search.
std::string madc_darwin_install_name(const std::string &spelling);

// The LC_RPATH a Mach-O image needs to load `spelling` where the link found
// it: a PATH spelling whose install name is "@rpath/<file>" (every library
// madc builds) answers its real directory — ld64 links such a library by
// path with `-rpath <dir>`, recording the install name. "" for a bare
// spelling (the runpath's madc lib dir serves it) and for a library whose
// install name is a fixed path.
std::string madc_darwin_link_rpath(const std::string &spelling);

#endif
