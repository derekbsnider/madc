#ifndef __MADC_MODULE_H
#define __MADC_MODULE_H 1

// madc_module.h — a madc MODULE describes itself (docs/plans/2026-10-17-
// dialect-builtin-registration.md, "Modules before R4"). A module is a library
// found by NAME on the module path; it exports ONE entry point, named after
// the module, that returns its description: its declarations (a builder table
// the build generates from the module's face — the dialect table's shape),
// and what it brings a session (commands, tools). Nothing about a module
// lives in the core: `import NAME;` opens it, checks its ABI and registers its
// declarations, each function bound through the module (the slot binding,
// CirBuilder::dyn_module_callee). A failed import is the answer to "is it
// there". A madc module is madc's BUILT form of a C++20 named module.
//
// THREAD-SAFETY CONTRACT: a description is the module's constant data;
// registering it writes only the importing Program.

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "madc_module_commands.h"
#include "madc_module_tools.h"

struct MadcBuiltinEntry;	// madc_builtin_registry.h: (ns, name) -> builder

// The builders' contract with libmadc: the registration API and the DataDef
// layout a generated builder relies on. A module built against another ABI is
// refused at import. Bump it when either changes.
#define MADC_MODULE_ABI 1u

enum MadcModuleDescFlag : unsigned
{
	mdGui = 1u,	// a GUI module: the driver lifts the memory guard at run start
};

// The description (append-only: a field added later is read only from a
// module whose `abi` says it has it).
struct MadcModuleDescription
{
	uint32_t			 abi;		// MADC_MODULE_ABI it was built against
	const char			*name;		// the module's name ("markdown")
	unsigned			 flags;		// MadcModuleDescFlag bits
	const MadcBuiltinEntry		*declarations;	// sorted by (ns, name)
	size_t				 ndeclarations;
	madc_module_commands_fn		 commands;	// the session's commands; NULL = none
	madc_module_tools_fn		 tools;		// a tool server's tools; NULL = none
};

// The entry point: C linkage so its name is the same on every target, a
// C++ payload (the commands/tools precedent).
typedef const MadcModuleDescription *(*madc_module_describe_fn)(void);

// The entry point's name: "madc_module_" and the module's name, each '.' of a
// dotted name as '_' ("a.b" -> "madc_module_a_b").
std::string madc_module_entry_name(const std::string &module);

#ifdef _WIN32
#define MADC_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define MADC_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#endif // __MADC_MODULE_H
