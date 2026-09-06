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
// pure except madc_module_open, which is the madc_dl seam's contract.

#include <string>
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
std::string madc_module_library_spelling(const std::string &name, TargetOS os);
std::string madc_module_library_spelling(const std::string &name);	// for madc_target_os

// Open a spelled library through the madc_dl seam into the default symbol
// scope: a bare spelling is tried beside the running binary first
// (<exe dir>/../lib/<spelling>, the relocatable install shape the runpath
// uses), then as the loader itself searches. NULL + `error` on failure.
void *madc_module_open(const std::string &spelling, std::string &error);

#endif
