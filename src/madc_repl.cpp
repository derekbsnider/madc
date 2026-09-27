/* madc_repl.cpp — the command-line REPL (plan §41.5a, D20; §41.7a, D23).
 *
 * The session decides what an entry is (the classifier inside its entry
 * transaction, §41.1a) and what it shows (D10); these loops only read,
 * prompt, and print. On a terminal the line editor (madcdis/line_edit.h)
 * reads the entry; anywhere else cooked lines do. The commands (D13, D24)
 * and Ctrl-C during a run (D8) are later owners.
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
#include "madcdis/line_edit.h"
#include "madcdis/tui_provider.h"
#include "madcdis/ui_input.h"

using madc::hub::line_edit;
using madc::hub::line_painter;
using madc::hub::line_target;

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
    return line.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

// D22: the language prompt names the standard in force; continuation lines
// are indented to its width (Julia).
static std::string entry_prompt(Program &prog)
{
    return std::string(Program::standard_canonical_name(prog.language_std))
	+ "> ";
}

int madc_repl_edit(InteractiveSession &session, line_target &term,
		   std::ostream &out)
{
    line_edit ed(entry_prompt(session.program()));
    line_painter paint;
    madc::hub::key_resolver keys;
    keys.set_bindings(madc::hub::line_edit_bindings());
    madc::hub::focus_state focus;	// nothing to focus: every key is the editor's
    std::vector<madc::hub::tui_keyev> kv;
    bool first = true;
    for (;;)
    {
	// Output the entries wrote must reach the terminal before the prompt
	// the painter writes around the streams.
	out.flush();
	fflush(stdout);
	size_t cols = 0;
	if ( !term.begin(cols) )
	    return first ? -1 : 1;
	first = false;
	ed.start();
	paint.reset();
	term.write(line_painter::fresh_row(cols));
	bool held = true;		// the terminal is the editor's
	bool over = false;		// this entry is over
	bool input_ended = false;
	// Finish the entry's display and hand the terminal back: before an
	// entry's diagnostics and its run (the session's taken hook), and at
	// every other end of an entry.
	auto release = [&](const std::string &tail) {
	    if ( !held )
		return;
	    term.write(paint.finish(ed.view_at_end(), cols, tail));
	    term.end();
	    held = false;
	};
	while ( !over )
	{
	    switch ( ed.step() )
	    {
		case line_edit::outcome::idle:
		{
		    cols = term.columns();
		    if ( ed.take_clear() )
		    {
			term.write("\x1b[H\x1b[2J");
			paint.reset();
		    }
		    std::vector<std::string> listed = ed.take_listed();
		    if ( !listed.empty() )
		    {
			term.write(paint.finish(ed.view(), cols, ""));
			term.write(line_painter::list(listed, cols));
		    }
		    term.write(paint.paint(ed.view(), cols));
		    kv.clear();
		    if ( !term.read_keys(kv) )
		    {
			// The input ended mid-entry: a pending entry is
			// final, as at the end of cooked lines.
			std::string text = ed.text();
			release("");
			if ( !blank_line(text) )
			{
			    session.submit(text + "\n");
			    print_shown(session, out);
			}
			input_ended = true;
			over = true;
			break;
		    }
		    ed.feed(madc::hub::ui_apply_keys(keys, focus, kv));
		    break;
		}
		case line_edit::outcome::enter:
		{
		    const std::string text = ed.text() + "\n";
		    if ( blank_line(text) )
		    {
			release("");
			ed.entered(line_edit::verdict::taken);
			over = true;
			break;
		    }
		    InteractiveSession::TakenHook taken = [&]() { release(""); };
		    if ( ed.enter_is_final() )
		    {
			session.submit(text, taken);
			ed.entered(line_edit::verdict::taken);
			print_shown(session, out);
			over = true;
			break;
		    }
		    InteractiveSession::Offered r = session.offer(text, taken);
		    switch ( r.state )
		    {
			case InteractiveSession::OfferState::Taken:
			    ed.entered(line_edit::verdict::taken);
			    print_shown(session, out);
			    over = true;
			    break;
			case InteractiveSession::OfferState::Incomplete:
			    ed.entered(line_edit::verdict::incomplete);
			    break;
			case InteractiveSession::OfferState::Extendable:
			    ed.entered(line_edit::verdict::extendable);
			    break;
		    }
		    break;
		}
		case line_edit::outcome::complete:
		    // Completion's provider is §41.7a's slice 3.
		    ed.completed(ed.caret(), std::vector<std::string>());
		    break;
		case line_edit::outcome::dropped:
		    release("^C");
		    over = true;
		    break;
		case line_edit::outcome::ended:
		    release("");
		    input_ended = true;
		    over = true;
		    break;
	    }
	}
	release("");
	if ( input_ended )
	    break;
    }
    return 0;
}

int madc_repl_run(InteractiveSession &session, std::istream &in,
		  std::ostream &out, bool terminal)
{
    Program &prog = session.program();
    const std::string prompt = entry_prompt(prog);
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
