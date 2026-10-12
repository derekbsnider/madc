#ifndef __MADC_MODULE_COMMANDS_H
#define __MADC_MODULE_COMMANDS_H 1

// madc_module_commands.h — the REPL commands a module brings to a session
// (split arc 1B.10; design docs/plans/2026-10-10-git-module-command-design.md
// §2.2-§2.3). A module's map row (src/madc_modules.cpp) names the function it
// exports; the session calls it when the module enters its Program and adds
// each row as a SESSION row (InteractiveSession::add_command): its handler
// runs in the session, inside the Program's runtime scope, and reaches the
// session through active_runtime_program()->owning_session.
//
// THREAD-SAFETY CONTRACT: the rows are the module's constants; a handler
// runs on its session's thread.

#include <cstddef>

namespace madc { class value; }

struct madc_module_command
{
    const char *name;		// the command's name, without its `%`
    const char *usage;		// %help's usage column (`%git VERB [ARGS]`)
    const char *summary;	// %help's summary
    // The registry's CommandHandler: true, `out` (a string) is what it shows;
    // false, `out` says why it was refused.
    bool (*handler)(madc::value &out, const char *argument);
};

// The function a row's `commands` names (extern "C", the module's C
// interface): its rows, `*count` of them.
typedef const madc_module_command *(*madc_module_commands_fn)(std::size_t *count);

#endif
