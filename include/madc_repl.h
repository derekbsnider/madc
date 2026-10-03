/* madc_repl.h — the command-line REPL (plan §41.5a, D20).
 *
 * The loop between a line stream and an InteractiveSession: it reads cooked
 * lines, offers the text typed so far to the session until an entry is
 * final, and prints what the entry shows. `madc` with no program file on a
 * terminal, and `madc -i`, run it over stdin and stdout; the unit tests run
 * the same loop over string streams.
 *
 * Thread contract: one loop drives one session from one thread (D9); its
 * state is per call.
 */

#ifndef __MADC_REPL_H
#define __MADC_REPL_H 1

#include <iosfwd>
#include <string>

class ReplSession;		// madc_session.h: in this process or a backend

// Read entries from `in` to its end and run each in `session`, a session
// that has begun. Shown values (D10) go to `out`; diagnostics go to the
// Program's error stream, as their renderers put them. `terminal` asks for
// the prompts (D22); off a terminal there are none (D23), so a transcript's
// output is its values and diagnostics. A greeting is the host's (the CLI
// prints one when it starts a session with no file). A pending entry
// is submitted at the end of input. Returns the exit status: 0, as
// python -i exits at the end of its input.
int madc_repl_run(ReplSession &session, std::istream &in,
		  std::ostream &out, bool terminal);

namespace madc { namespace hub { class line_target; } }

// The REPL on a terminal (plan §41.7a, D23): each entry is read by the line
// editor on `term`, which holds the terminal only while it reads. Enter
// asks the session (offer) and the entry runs as a cooked line's would;
// shown values go to `out`. Up recalls, and Ctrl-R searches, the entries
// of the history file at `history_path` typed in the session's language,
// and each entry taken is appended there (Julia's records); an empty path
// keeps history for the session alone. Returns the exit status at the end
// of input (Ctrl-D on an empty entry), or -1 when `term` cannot begin the
// first entry, which is the host's cue for cooked lines. The unit tests
// give it a scripted target.
int madc_repl_edit(ReplSession &session, madc::hub::line_target &term,
		   std::ostream &out, const std::string &history_path);

// The history file's default place: $XDG_STATE_HOME/madc/history
// (~/.local/state/madc/history), or %LOCALAPPDATA%\madc\history on
// Windows. Empty when there is no home for it.
std::string madc_repl_history_path();

#endif
