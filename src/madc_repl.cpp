/* madc_repl.cpp — the command-line REPL (plan §41.5a, D20).
 *
 * The session decides what an entry is (the classifier inside its entry
 * transaction, §41.1a) and what it shows (D10); this loop only reads lines,
 * prompts, and prints. The line editor (D23), the commands (D13, D24) and
 * Ctrl-C (D8) are later owners.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "madc_repl.h"

// An entry the session took: print what it shows (D10). A refused one
// showed nothing; its diagnostics are already on the error stream.
static void print_shown(InteractiveSession &session, std::ostream &out)
{
    const std::string &shown = session.shown();
    if ( !shown.empty() )
	out << shown << '\n';
    out << std::flush;
}

static bool blank_line(const std::string &line)
{
    return line.find_first_not_of(" \t\r\f\v") == std::string::npos;
}

int madc_repl_run(InteractiveSession &session, std::istream &in,
		  std::ostream &out, bool terminal)
{
    Program &prog = session.program();
    // D22: the language prompt names the standard in force; continuation
    // lines are indented to its width (Julia).
    const std::string prompt =
	std::string(Program::standard_canonical_name(prog.language_std)) + "> ";
    const std::string continuation(prompt.size(), ' ');
    std::string pending;		// the entry typed so far
    bool extendable = false;		// pending is an if waiting for else
    std::string line;
    for (;;)
    {
	if ( terminal )
	    out << (pending.empty() ? prompt : continuation) << std::flush;
	if ( !std::getline(in, line) )
	    break;
	if ( extendable )
	{
	    // D11: an else continues the if; an empty line runs it; any other
	    // line runs it and starts the next entry.
	    extendable = false;
	    if ( !prog.entry_line_continues_if(line) )
	    {
		session.submit(pending);
		print_shown(session, out);
		pending.clear();
	    }
	}
	if ( pending.empty() && blank_line(line) )
	    continue;
	pending += line;
	pending += '\n';
	InteractiveSession::Offered r = session.offer(pending);
	switch ( r.state )
	{
	    case InteractiveSession::OfferState::Incomplete:
		break;
	    case InteractiveSession::OfferState::Extendable:
		extendable = true;
		break;
	    case InteractiveSession::OfferState::Taken:
		print_shown(session, out);
		pending.clear();
		break;
	}
    }
    // The end of input: a pending entry is final. An extendable if runs;
    // an incomplete entry is refused with its own end-of-input diagnostic.
    if ( !pending.empty() )
    {
	session.submit(pending);
	print_shown(session, out);
    }
    if ( terminal )
	out << std::endl;
    return 0;
}
