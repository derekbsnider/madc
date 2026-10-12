#ifndef __MADCGIT_VALUES_H
#define __MADCGIT_VALUES_H 1

// modules/madcgit/madcgit_values.h — the module's value shapers
// (madcgit.cpp): a commit row and a blame row as madc values, the ONE shape
// the C interface (git::log / git::blame) and the history source
// (madcgit_history.cpp) both answer.
//
// Thread contract: pure functions.

#include "libmadc/value.h"
#include "madcdis/git_repo.h"

namespace madc {

value git_commit_value(const GitCommit &c);	// {sha, author, email, when, summary}
value git_blame_value(const GitBlameRow &b);	// {line, count, sha, author, when, summary}

} // namespace madc

#endif
