#ifndef __MODULES_GIT_VERBS_H
#define __MODULES_GIT_VERBS_H 1

// modules/git/git_verbs.h — the git module's `%git` verbs
// (git_verbs.cpp; split arc 1B.10): the module's own, and its unit
// test's (tests/unit/test_gitrepo.cpp compiles the module's sources) — and
// the two entries its description hands a host: its commands, its tools.
//
// Thread contract: a verb runs on its caller's thread; its repository is
// opened and closed inside it.

#include <cstdint>
#include <string>

#include "madc_module_commands.h"	// madc_module_command: the %git row
#include "madc_module_tools.h"	// madc::tools::registry: the history tools

namespace madc {

// `VERB ARGS` (the shell's word rules) to the text git prints for the same
// question — log, show, blame, status, diff. False = refused, with `why`.
// The repository is the one above FILE, else above the host's current
// buffer (madc::session_buffer), else the working directory's.
bool git_command(const std::string &argument, std::string &out, std::string &why);

// A time as git's --date=relative spells it, `now` the clock (git's date.c,
// show_date_relative: the same thresholds and roundings).
std::string git_relative_date(int64_t when, int64_t now);

} // namespace madc

// The entries the module's description hands a host: its command rows
// (`commands` — one, `%git`) and its tools (`tools` — the engine's history
// tools over the module's version history, git_history.cpp).
namespace git {
const madc_module_command *module_commands(std::size_t *count);
void module_tools(madc::tools::registry &r);
}

#endif
