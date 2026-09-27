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

class InteractiveSession;

// Read entries from `in` to its end and run each in `session`, a session
// that has begun. Shown values (D10) go to `out`; diagnostics go to the
// Program's error stream, as their renderers put them. `terminal` asks for
// the banner and the prompts (D22); off a terminal there are neither (D23),
// so a transcript's output is its values and diagnostics. A pending entry
// is submitted at the end of input. Returns the exit status: 0, as
// python -i exits at the end of its input.
int madc_repl_run(InteractiveSession &session, std::istream &in,
		  std::ostream &out, bool terminal);

#endif
