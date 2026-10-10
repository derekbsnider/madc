#ifndef __MADCGIT_VERBS_H
#define __MADCGIT_VERBS_H 1

// modules/madcgit/madcgit_verbs.h — the madcgit module's `%git` verbs
// (madcgit_verbs.cpp; split arc 1B.10): the module's own, and its unit
// test's (tests/unit/test_gitrepo.cpp compiles the module's sources).
//
// Thread contract: a verb runs on its caller's thread; its repository is
// opened and closed inside it.

#include <cstdint>
#include <string>

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

#endif
