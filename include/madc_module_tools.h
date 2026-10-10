#ifndef __MADC_MODULE_TOOLS_H
#define __MADC_MODULE_TOOLS_H 1

// madc_module_tools.h — the agent tools a module brings to a tool registry
// (split arc 1C.14; include/madc_tools.h). A module's map row
// (src/madc_modules.cpp) names the function it exports; a tool server
// (madc --mcp) calls it for each module its Program loaded, and the module
// registers into the registry — its own rows, or the engine's own tool
// family over the dependency the module isolates (madcgit hands
// tools::add_history_tools its history_source).
//
// THREAD-SAFETY CONTRACT: the function runs on the registry's thread; what
// it registers keeps the registry's contract (include/madc_tools.h).

namespace madc { namespace tools { class registry; } }

// The function a row's `tools` names (extern "C", the module's C interface).
typedef void (*madc_module_tools_fn)(madc::tools::registry &r);

#endif
