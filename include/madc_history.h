/* madc_history.h — the REPL history store (split arc 1B.7; design
 * docs/plans/2026-10-10-history-std-reset-design.md, decision 1).
 *
 * One store: the history file. Its records are Julia's (madcdis/line_edit.h,
 * history_record / history_records): each names the standard its entry was
 * typed at, and a reader takes the entries of one LANGUAGE (C, C++ or madc),
 * as each of Julia's modes recalls its own. The terminal loop reads and
 * appends through these; the session's %history reads through them.
 *
 * Thread contract: the C++ standard library's. An append is one O_APPEND
 * write, so sessions in other threads or processes interleave whole
 * records; a read takes the file as it is then.
 */
#ifndef __MADC_HISTORY_H
#define __MADC_HISTORY_H 1
#include <string>
#include <vector>

namespace madc {

// The store's default place: $XDG_STATE_HOME/madc/history
// (~/.local/state/madc/history), or %LOCALAPPDATA%\madc\history on Windows
// (history is state, not configuration). Empty when there is no home for it.
std::string history_default_path();

// The entries the store at `path` holds in the language of the standard
// named `mode` (a canonical name: "c17", "c++20", "madc"), oldest first.
// None for a missing file or a name no standard has.
std::vector<std::string> history_entries(const std::string &path,
					 const std::string &mode);

// Append `entry`'s record (now, `mode`) to the store at `path` in one write,
// owner-only (an entry can hold anything typed), making the directories
// above it when they are missing. False when nothing was written.
bool history_append(const std::string &path, const std::string &mode,
		    const std::string &entry);

}

#endif
