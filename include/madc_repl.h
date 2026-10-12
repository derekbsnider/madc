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
namespace madc { namespace ex { class session; } }

// Colon mode (D24; ex core design decision 11): the host's ex session, over
// the host's own world, and the stream its refusals go to. An entry whose
// first line starts with `:` is the host's — it never reaches `session`:
// shown lines go to `out`, a refusal to `err`; `a` / `i` / `c` read the
// lines after it as text up to one holding only `.`; `:q` ends the loop
// (refused while a buffer has changes, `:q!` ends it anyway); `:h` /
// `:help` is `?` and `:h NAME` is `?NAME`. Without one (`ex` null) a `:`
// entry goes to the session, which refuses it. The REPL buffer (owner
// 2026-10-12) is a scratch buffer `[REPL]`: every entry the session keeps,
// and every file %load keeps, is added to its end
// (ReplSession::taken_source). Colon mode starts on it at a bare `madc -i`
// (the host's untitled buffer is it); `madc -i FILE` is line mode, starting
// on FILE, with `:b REPL` reaching it. :ls, :b N / NAME, :bn, :bp and :e
// switch between the buffers. `%run` with the REPL buffer current rebuilds
// it: a fresh session, then its text as typed lines.
struct repl_colon
{
    madc::ex::session *ex;
    std::ostream      *err;
    repl_colon() : ex(0), err(0) {}
};

// Read entries from `in` to its end and run each in `session`, a session
// that has begun. Shown values (D10) go to `out`; diagnostics go to the
// Program's error stream, as their renderers put them. `terminal` asks for
// the prompts (D22); off a terminal there are none (D23), so a transcript's
// output is its values and diagnostics. A greeting is the host's (the CLI
// prints one when it starts a session with no file). A pending entry
// is submitted at the end of input. Returns the exit status: 0, as
// python -i exits at the end of its input. `theme` (split arc 1B.9) colours
// the code a command shows (`%type`, `%pinfo`) when Phase 0.5's gate lets
// colour reach `out`; the cooked lines themselves are never repainted.
namespace madc { namespace hub { class line_target; } struct theme; }
int madc_repl_run(ReplSession &session, std::istream &in,
		  std::ostream &out, bool terminal,
		  const repl_colon &colon = repl_colon(),
		  const madc::theme *theme = 0);

// The REPL on a terminal (plan §41.7a, D23): each entry is read by the line
// editor on `term`, which holds the terminal only while it reads. Enter
// asks the session (offer) and the entry runs as a cooked line's would;
// shown values go to `out`. Up recalls, and Ctrl-R searches, the entries
// of the history store at `history_path` (madc_history.h) typed in the
// session's language, and each entry taken is appended there; an empty
// path keeps history for the session alone. Returns the exit status at the end
// of input (Ctrl-D on an empty entry), or -1 when `term` cannot begin the
// first entry, which is the host's cue for cooked lines. The unit tests
// give it a scripted target. `theme` (split arc 1B.9) colours each entry
// as it is typed, and the code a command shows, when Phase 0.5's gate lets
// colour reach `out` (madc::colour_reaches); NULL paints it plain.
int madc_repl_edit(ReplSession &session, madc::hub::line_target &term,
		   std::ostream &out, const std::string &history_path,
		   const repl_colon &colon = repl_colon(),
		   const madc::theme *theme = 0);


#endif
