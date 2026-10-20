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
#include <functional>
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


#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "madc_ex.h"		// colon mode: the host's ex session (D24)
#include "madc_documents.h"	// the REPL buffer: a document in its world
#include "madcdis/hub.h"
#include "madc_history.h"	// the history store (1B.7)
#include "madc_repl.h"
#include "madc_posix_io.h"
#include "madcdis/line_edit.h"
#include "madcdis/ui_sgr.h"		// sgr / terminal_colour_depth: the entry's marks (1B.9)
#include "madc_console.h"		// colour_reaches: Phase 0.5's gate
#include "madc_theme.h"			// the prompt's colours (1B.9)
#include "madcdis/tui_provider.h"
#include "madcdis/ui_input.h"

using madc::hub::line_edit;
using madc::hub::line_history;
using madc::hub::line_painter;
using madc::hub::line_target;
using madc::hub::line_view;

// A standard's --std= spelling as the classifier's code (0: the default).
static int64_t standard_code(const std::string &name)
{
    Program::LanguageStd std = Program::LanguageStd();
    bool gnu = false;
    return Program::standard_of_spelling(madc::dis::istring(name), std, gnu)
	? (int64_t)std : 0;
}

// An entry the session took: print what it shows (D10), the code in it
// (`%type`, `%pinfo`: the session's shown_code) in `theme`'s colours where
// colour reaches `out` (split arc 1B.9, design decision 5). A refused one
// showed nothing; its diagnostics are already on the error stream.
static void print_shown(ReplSession &session, std::ostream &out,
			const madc::theme *theme)
{
    const std::string &shown = session.shown();
    if ( !shown.empty() )
    {
	if ( theme && !session.shown_code().empty() && madc::colour_reaches(out) )
	    out << madc::theme_paint(*theme, shown, session.shown_code(),
				     standard_code(session.standard_name()),
				     madc::hub::terminal_colour_depth());
	else
	    out << shown;
	out << '\n';
    }
    out << std::flush;
}

// ---- colon mode (D24; ex core design decision 11; madc_repl.h's
// repl_colon): what a colon entry leaves the loop to do.
enum class colon_step : unsigned char { done, wants_text, quit };

// `line` is a colon entry this loop takes (the host has an ex session):
// `argument` is what follows the `:`.
static bool colon_line(const repl_colon &colon, const std::string &line,
		       std::string &argument)
{
    InteractiveSession::Mode mode;
    InteractiveSession::Command code;
    std::string word;
    return colon.ex
	   && InteractiveSession::command_of(line, mode, word, code, argument)
	   && mode == InteractiveSession::Mode::colon;
}

// An ex outcome printed: its lines to `out`, its refusal to the colon
// error stream.
static colon_step colon_report(const madc::ex::outcome &o,
			       const repl_colon &colon, std::ostream &out,
			       ReplSession *session = 0,
			       const madc::theme *theme = 0)
{
    // The buffer's lines it prints (:p, :nu; outcome::code) in `theme`'s
    // colours where colour reaches `out`, as an entry's are as it is typed.
    if ( session && theme && !o.code.empty() && madc::colour_reaches(out) )
	out << madc::theme_paint(*theme, o.shown, o.code,
				 standard_code(session->standard_name()),
				 madc::hub::terminal_colour_depth());
    else
	out << o.shown;
    out << std::flush;
    if ( !o.ok && colon.err )
	*colon.err << o.error << std::endl;
    return o.quit ? colon_step::quit
	 : o.wants_text ? colon_step::wants_text : colon_step::done;
}

// A colon entry run: the host's ex session, but `:h` / `:help` is the
// session's `?` (`:h NAME` its `?NAME`).
static colon_step colon_run(ReplSession &session, const repl_colon &colon,
			    const std::string &argument, std::ostream &out,
			    const madc::theme *theme)
{
    madc::ex::line parsed;
    std::string why;
    if ( madc::ex::parse(argument, parsed, why)
	 && parsed.code == madc::ex::command::help )
    {
	session.submit("?" + parsed.argument + "\n");
	print_shown(session, out, theme);
	return colon_step::done;
    }
    return colon_report(colon.ex->run(argument), colon, out, &session, theme);
}

// The live reparse (owner 2026-10-12): an edit a colon command makes to the
// REPL buffer takes effect at once — the session reads the entries it
// changed again, their definitions only (ReplSession::buffer_edited).
static colon_step colon_apply(ReplSession &session, const repl_colon &colon,
			      madc::hub::entity_id repl,
			      const std::function<colon_step()> &command)
{
    const madc::hub::text_buffer *b = repl ? colon.ex->world().text_of(repl) : 0;
    const std::string before = b ? b->text() : std::string();
    const uint64_t version = b ? b->version() : 0;
    const colon_step c = command();
    b = repl ? colon.ex->world().text_of(repl) : 0;
    if ( b && b->version() != version && b->text() != before )
	session.buffer_edited(before, b->text());
    return c;
}

// Input mode on cooked lines: the lines up to one holding only `.` (or the
// end of input) are the text a / i / c waits for.
static colon_step colon_text(std::istream &in, std::ostream &out,
			     bool terminal, const std::string &prompt,
			     const repl_colon &colon)
{
    std::vector<std::string> body;
    std::string line;
    for (;;)
    {
	if ( terminal )
	    out << prompt << std::flush;
	if ( !std::getline(in, line) )
	    break;
	madc::drop_cr(line);
	if ( line == "." )
	    break;
	body.push_back(line);
    }
    return colon_report(colon.ex->text(body), colon, out);
}

// The REPL buffer (owner 2026-10-12: colon mode at the REPL edits what the
// session kept): a scratch buffer, `[REPL]`, in the ex session's world. At a
// bare `madc -i` (REPL mode) the host's untitled buffer IS it, and colon mode
// starts on it; `madc -i FILE` is line mode (split arc 1B.6 — `madcide
// --line`), so colon mode starts on FILE and `:b REPL` reaches the REPL
// buffer, which a new document holds. :ls, :b N / NAME, :bn, :bp and :e
// switch between them. The session's buffer question is the ex session's
// from here on (ex::session::buffer: the REPL buffer has no file to give). 0
// without colon mode.
// The REPL buffer's name (:ls, :b), and its main's argv[0] in a rebuild.
static const char repl_buffer_name[] = "[REPL]";

static madc::hub::entity_id repl_begin(ReplSession &session, const repl_colon &colon)
{
    if ( !colon.ex )
	return 0;
    madc::ex::session *ex = colon.ex;
    const madc::hub::entity_id cur = ex->current();
    const madc::hub::document *d = cur ? ex->world().doc_of(cur) : 0;
    const bool repl_mode = !d || d->untitled();
    const madc::hub::entity_id doc =
	repl_mode && cur ? cur : madc::document_new(ex->world(), "");
    if ( !doc || !ex->set_scratch(doc, madc::dis::istring(repl_buffer_name)) )
	return 0;
    if ( repl_mode )
	ex->use(doc);
    session.set_buffers([ex](const madc::dis::istring &name,
			     madc::dis::istring &path, std::string &text,
			     bool &has_text, std::string &why) -> bool {
	return ex->buffer(name, path, text, has_text, why);
    });
    return doc;
}

// An entry the session took: what it shows printed (print_shown), and the
// source it kept — a code entry's text, %load's file — at the end of the
// REPL buffer.
static void entry_taken(ReplSession &session, std::ostream &out,
			const madc::theme *theme, const repl_colon &colon,
			madc::hub::entity_id repl)
{
    print_shown(session, out, theme);
    if ( repl && !session.taken_source().empty() )
	colon.ex->append(repl, session.taken_source());
}

// An entry as history keeps it: without the whitespace after its last line
// (Julia's), so an extendable if taken at its blank line recalls as typed.
static std::string history_text(const std::string &entry)
{
    size_t end = entry.find_last_not_of(" \t\r\n\f\v");
    return end == std::string::npos ? std::string() : entry.substr(0, end + 1);
}

static int cooked_loop(ReplSession &session, std::istream &in,
		       std::istream *reclaim, std::ostream &out, bool terminal,
		       const repl_colon &colon, const madc::theme *theme,
		       madc::hub::entity_id repl);

// `%run` (no FILE) with the REPL buffer current (owner 2026-10-12): the REPL
// buffer is rebuilt; every other buffer runs as %run runs it.
static bool rebuild_line(const repl_colon &colon, madc::hub::entity_id repl,
			 const std::string &line)
{
    InteractiveSession::Mode mode;
    InteractiveSession::Command code;
    std::string word, argument;
    return repl && colon.ex && colon.ex->current() == repl
	   && InteractiveSession::command_of(line, mode, word, code, argument)
	   && code == InteractiveSession::Command::run && argument.empty();
}

// The rebuild: a fresh session (%reset), then the buffer's text read as
// typed lines, shown as any entry is and never added to the buffer again;
// then its main, when it defines one that no entry called (as %run runs a
// file's), with the buffer's name as argv[0].
static void rebuild_repl_buffer(ReplSession &session, std::istream *reclaim,
				std::ostream &out, const madc::theme *theme,
				const repl_colon &colon, madc::hub::entity_id repl)
{
    const madc::hub::text_buffer *b = colon.ex->world().text_of(repl);
    std::istringstream text(b ? b->text() : std::string());
    if ( !session.submit("%reset\n") )
	return;
    cooked_loop(session, text, reclaim, out, false, repl_colon(), theme, 0);
    int status = 0;
    if ( !session.ended(status) )
	session.run_rebuilt_main(repl_buffer_name);
}

// D22: the language prompt names the standard in force; continuation lines
// are indented to its width (Julia).
static std::string entry_prompt(ReplSession &session)
{
    return session.standard_name() + "> ";
}

// The entry's syntax colour (split arc 1B.9, design decision 5): the theme
// owner's marks over its lines, under the session's standard. A command
// entry (%, ., ?, :) is no code: it stays plain.
static void colour_entry(line_view &v, const madc::theme &theme,
			 int64_t standard, madc::hub::ui_colour_depth depth)
{
    std::string text;
    for ( size_t i = 0; i < v.lines.size(); ++i )
	text += (i ? "\n" : "") + v.lines[i];
    InteractiveSession::Mode mode;
    InteractiveSession::Command code;
    std::string word, argument;
    if ( !InteractiveSession::command_of(text, mode, word, code, argument) )
	madc::theme_marks(theme, v.lines, standard, depth, v.marks);
}

int madc_repl_edit(ReplSession &session, line_target &term,
		   std::ostream &out, const std::string &history_path,
		   const repl_colon &colon, const madc::theme *theme)
{
    // The entry's colour (split arc 1B.9): a theme, and Phase 0.5's gate on
    // the output (NO_COLOR, TERM=dumb, -fdiagnostics-color=never); the
    // terminal's depth read once.
    const madc::hub::entity_id repl = repl_begin(session, colon);
    const bool colour = theme && madc::colour_reaches(out);
    const madc::hub::ui_colour_depth depth = madc::hub::terminal_colour_depth();
    int64_t standard = 0;
    // History (§41.7a slice 2): the file's entries in this language, then
    // each entry the session takes, a refused one included, as Julia keeps
    // an input that failed. A record's mode is the prompt's standard (D22).
    // The store is madc_history.h's. A session under another standard
    // (%std, split arc 1B.7) re-keys the prompt and the ring: each language
    // recalls its own.
    std::string mode, prompt, continuation;
    line_history ring;
    auto rekey = [&]() {
	mode = session.standard_name();
	standard = standard_code(mode);
	prompt = entry_prompt(session);
	continuation.assign(prompt.size(), ' ');
	ring = line_history();
	const std::vector<std::string> kept = madc::history_entries(history_path, mode);
	for ( const std::string &entry : kept )
	    ring.add(entry);
    };
    rekey();
    line_edit ed(prompt);
    ed.set_history(&ring);
    auto remember = [&](const std::string &entry) {
	if ( ring.add(entry) )
	    madc::history_append(history_path, mode, entry);
    };
    line_painter paint;
    // What the painter shows: the editor's view, coloured when colour is on.
    auto shown = [&](line_view v) {
	if ( colour )
	    colour_entry(v, *theme, standard, depth);
	return v;
    };
    madc::hub::key_resolver keys;
    keys.set_bindings(madc::hub::line_edit_bindings());
    madc::hub::focus_state focus;	// nothing to focus: every key is the editor's
    std::vector<madc::hub::tui_keyev> kv;
    bool first = true;
    // Colon mode's input mode: a / i / c's text lines, read one entry each
    // at the continuation prompt until one holding only `.`.
    bool colon_text_mode = false;
    std::vector<std::string> colon_body;
    bool quit = false;			// :q
    for (;;)
    {
	// Output the entries wrote must reach the terminal before the prompt
	// the painter writes around the streams.
	out.flush();
	fflush(stdout);
	if ( session.standard_name() != mode )
	{
	    rekey();
	    ed.set_prompt(colon_text_mode ? continuation : prompt);
	}
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
	    term.write(paint.finish(shown(ed.view_at_end()), cols, tail));
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
			term.write(paint.finish(shown(ed.view()), cols, ""));
			term.write(line_painter::list(listed, cols));
		    }
		    term.write(paint.paint(shown(ed.view()), cols));
		    kv.clear();
		    if ( !term.read_keys(kv) )
		    {
			// The input ended mid-entry: a pending entry is
			// final, as at the end of cooked lines.
			std::string text = ed.text();
			release("");
			if ( !madc::blank_text(text) )
			{
			    session.submit(text + "\n");
			    remember(history_text(text));
			    entry_taken(session, out, theme, colon, repl);
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
		    std::string argument;
		    if ( colon_text_mode )
		    {
			release("");
			ed.entered(line_edit::verdict::taken);
			over = true;
			if ( ed.text() != "." )
			{
			    colon_body.push_back(ed.text());
			    break;
			}
			colon_text_mode = false;
			ed.set_prompt(prompt);
			quit = colon_apply(session, colon, repl, [&]() {
			    return colon_report(colon.ex->text(colon_body), colon, out);
			}) == colon_step::quit;
			colon_body.clear();
			break;
		    }
		    if ( colon_line(colon, ed.text(), argument) )
		    {
			release("");
			remember(history_text(text));
			ed.entered(line_edit::verdict::taken);
			over = true;
			const colon_step c = colon_apply(session, colon, repl, [&]() {
			    return colon_run(session, colon, argument, out, theme);
			});
			quit = c == colon_step::quit;
			if ( c == colon_step::wants_text )
			{
			    colon_text_mode = true;
			    ed.set_prompt(continuation);
			}
			break;
		    }
		    if ( rebuild_line(colon, repl, ed.text()) )
		    {
			release("");
			remember(history_text(text));
			ed.entered(line_edit::verdict::taken);
			over = true;
			rebuild_repl_buffer(session, NULL, out, theme, colon, repl);
			break;
		    }
		    if ( madc::blank_text(text) )
		    {
			release("");
			ed.entered(line_edit::verdict::taken);
			over = true;
			break;
		    }
		    ReplSession::TakenHook taken = [&]() { release(""); };
		    if ( ed.enter_is_final() )
		    {
			session.submit(text, taken);
			remember(history_text(text));
			ed.entered(line_edit::verdict::taken);
			entry_taken(session, out, theme, colon, repl);
			over = true;
			break;
		    }
		    ReplSession::Offered r = session.offer(text, taken);
		    switch ( r.state )
		    {
			case ReplSession::OfferState::taken:
			    remember(history_text(text));
			    ed.entered(line_edit::verdict::taken);
			    entry_taken(session, out, theme, colon, repl);
			    over = true;
			    break;
			case ReplSession::OfferState::incomplete:
			    ed.entered(line_edit::verdict::incomplete);
			    break;
			case ReplSession::OfferState::extendable:
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
	int status = 0;
	if ( session.ended(status) )
	    return status;		// exit(n) in an entry ends the REPL
	if ( quit )
	    return 0;			// :q
	if ( input_ended )
	{
	    // The end of input ends a / i / c's text, as `.` would.
	    if ( colon_text_mode )
		colon_apply(session, colon, repl, [&]() {
		    return colon_report(colon.ex->text(colon_body), colon, out);
		});
	    break;
	}
    }
    return 0;
}

// The bytes of a shared piped stdin the backend read past what its entry
// parsed (ReplSession::take_returned_input) go back to `in`, read next;
// NULL drops them.
static void reclaim_input(ReplSession &session, std::istream *in)
{
    const std::string back = session.take_returned_input();
    if ( !in )
	return;
    for ( std::string::const_reverse_iterator it = back.rbegin();
	  it != back.rend(); ++it )
	in->putback(*it);
}

// The cooked loop over `in`; returned stdin bytes go to `reclaim`. Its lines
// group into entries by the REPL's one rule (EntryLines); colon mode and the
// rebuild's line are read between entries.
static int cooked_loop(ReplSession &session, std::istream &in,
		       std::istream *reclaim, std::ostream &out, bool terminal,
		       const repl_colon &colon, const madc::theme *theme,
		       madc::hub::entity_id repl)
{
    std::string prompt, continuation;	// the standard's, entry by entry (%std)
    std::string line;
    int status = 0;			// a backend's exit(n)
    EntryLines lines(session);
    // An exit(n) in an entry ends the REPL.
    const EntryLines::Taken taken = [&](const std::string &, bool) {
	reclaim_input(session, reclaim);
	entry_taken(session, out, theme, colon, repl);
	return !session.ended(status);
    };
    for (;;)
    {
	if ( lines.idle() )
	{
	    prompt = entry_prompt(session);
	    continuation.assign(prompt.size(), ' ');
	}
	if ( terminal )
	    out << (lines.idle() ? prompt : continuation) << std::flush;
	if ( !std::getline(in, line) )
	    break;
	madc::drop_cr(line);			// a CRLF transcript's line
	if ( !lines.settle(line, taken) )
	    return status;
	std::string argument;
	if ( lines.idle() && colon_line(colon, line, argument) )
	{
	    const colon_step c = colon_apply(session, colon, repl, [&]() {
		colon_step step = colon_run(session, colon, argument, out, theme);
		if ( step == colon_step::wants_text )
		    step = colon_text(in, out, terminal, continuation, colon);
		return step;
	    });
	    if ( c == colon_step::quit )
		return 0;		// :q
	    continue;
	}
	if ( lines.idle() && rebuild_line(colon, repl, line) )
	{
	    rebuild_repl_buffer(session, reclaim, out, theme, colon, repl);
	    if ( session.ended(status) )
		return status;
	    continue;
	}
	if ( !lines.add(line, taken) )
	    return status;
    }
    // The end of input: a pending entry is final. An extendable if runs;
    // an incomplete entry is refused with its own end-of-input diagnostic.
    if ( !lines.finish(taken) )
	return status;
    if ( terminal )
	out << std::endl;
    return 0;
}

int madc_repl_run(ReplSession &session, std::istream &in,
		  std::ostream &out, bool terminal, const repl_colon &colon,
		  const madc::theme *theme)
{
    const madc::hub::entity_id repl = repl_begin(session, colon);
    return cooked_loop(session, in, &in, out, terminal, colon, theme, repl);
}
