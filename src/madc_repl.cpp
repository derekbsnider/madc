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
#include <cstdlib>
#include <ctime>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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
using madc::hub::line_history;
using madc::hub::line_painter;
using madc::hub::line_target;

// ---------------------------------------------------------------- history
// The history file (plan §41.7a, slice 2): Julia's records, one per entry
// the session took, appended as it is taken, so a crash loses nothing.

// Where the file lives by default: the XDG state directory (history is
// state, not configuration: madc.ini's search takes the config one), or the
// local application data on Windows. Empty when there is no home for it.
std::string madc_repl_history_path()
{
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if ( local && *local )
	return std::string(local) + "\\madc\\history";
    return std::string();
#else
    const char *state = getenv("XDG_STATE_HOME");
    if ( state && *state )
	return std::string(state) + "/madc/history";
    const char *home = getenv("HOME");
    if ( home && *home )
	return std::string(home) + "/.local/state/madc/history";
    return std::string();
#endif
}

// Make every directory above `path` that is missing (mkdir -p of its
// parent). Failure shows at the first append, which then writes nothing.
static void make_parent_directories(const std::string &path)
{
    for ( size_t i = 1; i < path.size(); ++i )
    {
	if ( path[i] != '/' && path[i] != '\\' )
	    continue;
	std::string dir = path.substr(0, i);
#ifdef _WIN32
	if ( dir.size() == 2 && dir[1] == ':' )
	    continue;			// a drive, not a directory
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0700);
#endif
    }
}

static std::string utc_now()
{
    time_t t = time(NULL);
    struct tm parts;
#ifdef _WIN32
    gmtime_s(&parts, &t);
#else
    gmtime_r(&t, &parts);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%SZ", &parts);
    return buf;
}

// One record, in one write with the file opened for appending, so two
// sessions' records interleave whole. Owner-only (0600): an entry can hold
// anything typed.
static void append_record(const std::string &path, const std::string &record)
{
#ifdef _WIN32
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    f << record;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if ( fd < 0 )
	return;
    ssize_t n = ::write(fd, record.data(), record.size());
    (void)n;
    ::close(fd);
#endif
}

// The language a standard names: recall and search see the session's own
// (C, C++ or madc), as each of Julia's modes searches its own. The Program's
// is_c_mode / is_cpp_mode ask the same of the standard in force.
enum class entry_language : unsigned char { c, cpp, madc };
static entry_language language_of(Program::LanguageStd s)
{
    if ( s == Program::STD_MADC )
	return entry_language::madc;
    if ( s >= Program::STD_CPP98 && s <= Program::STD_CPP26 )
	return entry_language::cpp;
    return entry_language::c;
}

// Read the file's records into `ring`: the entries typed in the session's
// language, oldest first. A missing file is an empty history.
static void load_history(const std::string &path, Program::LanguageStd std,
			 line_history &ring)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if ( !f )
	return;
    std::ostringstream text;
    text << f.rdbuf();
    const entry_language want = language_of(std);
    std::vector<std::pair<std::string, std::string> > records =
	madc::hub::history_records(text.str());
    for ( size_t i = 0; i < records.size(); ++i )
    {
	Program::LanguageStd mode;
	if ( Program::standard_of_canonical_name(records[i].first.c_str(), mode)
	     && language_of(mode) == want )
	    ring.add(records[i].second);
    }
}

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

// An entry as history keeps it: without the whitespace after its last line
// (Julia's), so an extendable if taken at its blank line recalls as typed.
static std::string history_text(const std::string &entry)
{
    size_t end = entry.find_last_not_of(" \t\r\n\f\v");
    return end == std::string::npos ? std::string() : entry.substr(0, end + 1);
}

// D22: the language prompt names the standard in force; continuation lines
// are indented to its width (Julia).
static std::string entry_prompt(Program &prog)
{
    return std::string(Program::standard_canonical_name(prog.language_std))
	+ "> ";
}

int madc_repl_edit(InteractiveSession &session, line_target &term,
		   std::ostream &out, const std::string &history_path)
{
    Program &prog = session.program();
    line_edit ed(entry_prompt(prog));
    // History (§41.7a slice 2): the file's entries in this language, then
    // each entry the session takes, a refused one included, as Julia keeps
    // an input that failed. A record's mode is the prompt's standard (D22).
    line_history ring;
    const std::string mode = Program::standard_canonical_name(prog.language_std);
    if ( !history_path.empty() )
    {
	load_history(history_path, prog.language_std, ring);
	make_parent_directories(history_path);
    }
    ed.set_history(&ring);
    auto remember = [&](const std::string &entry) {
	if ( ring.add(entry) && !history_path.empty() )
	    append_record(history_path,
			  madc::hub::history_record(utc_now(), mode, entry));
    };
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
			    remember(history_text(text));
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
			remember(history_text(text));
			ed.entered(line_edit::verdict::taken);
			print_shown(session, out);
			over = true;
			break;
		    }
		    InteractiveSession::Offered r = session.offer(text, taken);
		    switch ( r.state )
		    {
			case InteractiveSession::OfferState::Taken:
			    remember(history_text(text));
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
		{
		    // The session answers; the editor inserts or lists.
		    size_t start = ed.caret();
		    std::vector<std::string> names =
			session.complete(ed.text(), ed.caret(), start);
		    ed.completed(start, names);
		    break;
		}
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
