/* madc_session.cpp — the persistent interactive session (plan §41.2a).
 *
 * The session is three owners composed, each doing what it already does for a
 * translation unit, once per entry instead of once per file:
 *   - the Program parses the entry on everything earlier entries declared
 *     (Program::begin_interactive_session / parse_entry);
 *   - the CIR builder translates the Program as it stands, DECLARING what an
 *     earlier module defines (Program::session_defined);
 *   - the JIT appends the module to one live MIR context
 *     (CirJitSession::begin_live / append).
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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
#include "ns_common.h"		// shell_words: %run's and %load's arguments
#include "madc_cir.h"
#include "madc_session.h"
#include "madc_type_spelling.h"
#include "madcdis/text_buffer.h"	// the one word rule (word_byte)
#include "madcdis/text_utf16.h"	// madc::line_width: %whos's columns
#include "madcdis/process.h"	// the terminal's editor (%open, %edit)

// The command registry (plan §41.8a, D13/D24): one row per command. A typed
// name becomes its code once, at input (command_named); what follows
// dispatches on the code.
namespace {

struct CommandRow
{
    const char *name;
    InteractiveSession::Command code;
    const char *usage;		// as %help prints it
    const char *summary;
};

const CommandRow command_rows[] = {
    { "help", InteractiveSession::Command::help, "%help",
      "list the session's commands" },
    { "type", InteractiveSession::Command::type, "%type EXPR",
      "the type of an expression, which is not run" },
    { "pinfo", InteractiveSession::Command::pinfo, "%pinfo NAME",
      "what the session knows of a name (also ?NAME)" },
    { "whos", InteractiveSession::Command::whos, "%whos",
      "the names the session defined: their types, values and origins" },
    { "load", InteractiveSession::Command::load, "%load FILE",
      "define FILE's names in this session; nothing runs" },
    { "run", InteractiveSession::Command::run, "%run [-i] FILE [ARGS]",
      "FILE's main with ARGS, in a fresh session (-i: this one); its names stay" },
    { "build", InteractiveSession::Command::build, "%build FILE [-o OUT]",
      "FILE built to a native executable, OUT (default: FILE without its extension)" },
    { "call", InteractiveSession::Command::call, "%call FILE[(ARGS)]",
      "FILE loaded, then the function named after it called with ARGS, else FILE's main" },
    { "open", InteractiveSession::Command::open, "%open FILE",
      "FILE in an editor: the IDE's, else $EDITOR" },
    { "edit", InteractiveSession::Command::edit, "%edit NAME",
      "the file that defines NAME, in an editor at its line" },
    { "quit", InteractiveSession::Command::quit, "%quit",
      "end the session" },
};

const size_t command_count = sizeof(command_rows) / sizeof(command_rows[0]);

// The other REPLs' own spellings (plan §7f): each an alias row naming one of
// the commands above, one meaning under every prefix (D24).
struct AliasRow
{
    const char *name;
    InteractiveSession::Command code;
};

const AliasRow alias_rows[] = {
    { "?", InteractiveSession::Command::help },		// cling's .?
    { "L", InteractiveSession::Command::load },		// cling's .L
    { "q", InteractiveSession::Command::quit },		// cling's .q
    { "exit", InteractiveSession::Command::quit },	// Node's .exit
    { "x", InteractiveSession::Command::call },		// cling's .x
};

const size_t alias_count = sizeof(alias_rows) / sizeof(alias_rows[0]);

const CommandRow *command_coded(InteractiveSession::Command code)
{
    for ( size_t i = 0; i < command_count; ++i )
	if ( command_rows[i].code == code )
	    return &command_rows[i];
    return NULL;
}

const CommandRow *command_named(const std::string &name)
{
    for ( size_t i = 0; i < command_count; ++i )
	if ( name == command_rows[i].name )
	    return &command_rows[i];
    for ( size_t i = 0; i < alias_count; ++i )
	if ( name == alias_rows[i].name )
	    return command_coded(alias_rows[i].code);
    return NULL;
}

// Where a command stands in an entry (D13): its first line, after blanks,
// starts with `%`, `:` or `.` and at once a name, or with `?` (D15). The name
// after a prefix may be `?` (cling's `.?`). So `::x`, the `%:` digraph and
// `.5` stay C, and so does every continuation line (a designator's `.x`); C
// starts no statement with `?`, or with `.` and a letter.
struct CommandText
{
    size_t prefix;	// the `%`, `:`, `.` or `?`; npos when the text is no command
    size_t name_end;	// past the name (past the `?`)
    size_t line_end;	// the first line's end
    bool query;		// `?NAME`: IPython's %pinfo NAME; `?` alone its help
    CommandText()
	: prefix(std::string::npos), name_end(0), line_end(0), query(false) {}
    bool is_command() const { return prefix != std::string::npos; }
    std::string name(const std::string &text) const
    {
	if ( query )
	    return text.find_first_not_of(" \t", name_end) < line_end
		 ? "pinfo" : "help";
	return text.substr(prefix + 1, name_end - prefix - 1);
    }
};

CommandText command_text(const std::string &text)
{
    CommandText c;
    size_t i = text.find_first_not_of(" \t");
    if ( i != std::string::npos && text[i] == '?' )
    {
	c.prefix = i;
	c.name_end = i + 1;
	c.query = true;
	c.line_end = text.find('\n', i);
	if ( c.line_end == std::string::npos )
	    c.line_end = text.size();
	return c;
    }
    if ( i == std::string::npos || i + 1 >= text.size()
	 || (text[i] != '%' && text[i] != ':' && text[i] != '.') )
	return c;
    // The name is the REPL's one word rule's run (text_buffer::word_byte,
    // as completion and word motion read a word), not starting with a digit,
    // or the one byte `?`.
    size_t e = i + 1;
    if ( text[e] == '?' )
	++e;
    else if ( !madc::hub::text_buffer::word_byte(text[e])
	      || isdigit((unsigned char)text[e]) )
	return c;
    else
	while ( e < text.size() && madc::hub::text_buffer::word_byte(text[e]) )
	    ++e;
    c.prefix = i;
    c.name_end = e;
    c.line_end = text.find('\n', e);
    if ( c.line_end == std::string::npos )
	c.line_end = text.size();
    return c;
}

// A command's argument, everything before it blanked, so a diagnostic cites
// the column the user typed.
std::string command_argument(const std::string &text, const CommandText &c)
{
    return std::string(c.name_end, ' ')
	 + text.substr(c.name_end, c.line_end - c.name_end);
}

} // namespace

bool InteractiveSession::command_of(const std::string &text, std::string &word,
				    Command &code, std::string &argument)
{
    CommandText c = command_text(text);
    if ( !c.is_command() )
	return false;
    word = c.name(text);
    const CommandRow *row = command_named(word);
    code = row ? row->code : Command::none;
    const std::string rest = text.substr(c.name_end, c.line_end - c.name_end);
    const size_t b = rest.find_first_not_of(" \t");
    argument = b == std::string::npos ? std::string() : rest.substr(b);
    return true;
}

InteractiveSession::InteractiveSession()
    : prog(new Program()), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false),
      payload_kind(madc::session_payload::none), payload_host(false),
      load_main(false), quit_asked(false), quiet_count(0)
{
}

InteractiveSession::InteractiveSession(std::unique_ptr<Program> configured)
    : prog(std::move(configured)), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false),
      payload_kind(madc::session_payload::none), payload_host(false),
      load_main(false), quit_asked(false), quiet_count(0)
{
}

// The JIT goes first: its modules reference the Program's token tree.
InteractiveSession::~InteractiveSession()
{
    jit.reset();
    prog.reset();
}

bool InteractiveSession::begin(const std::string &std_option)
{
    if ( !std_option.empty() && !prog->set_language_standard_option(std_option) )
    {
	// Its refusal is said, as the CLI says it (the host may have no
	// terminal: a backend's client reads the Program's error stream).
	const std::string prefix("--std=");
	prog->error() << "Unknown --std target: "
		      << (std_option.compare(0, prefix.size(), prefix) == 0
			  ? std_option.substr(prefix.size()) : std_option)
		      << std::endl;
	return false;
    }
    if ( !prog->begin_interactive_session("REPL") )
	return false;
    return jit->begin_live("REPL");
}

bool InteractiveSession::submit(const std::string &text, const TakenHook &taken)
{
    return enter(text, true, taken).ok;
}

InteractiveSession::Offered InteractiveSession::offer(const std::string &text,
						      const TakenHook &taken)
{
    return enter(text, false, taken);
}

// The one entry path. A FINAL entry is taken whatever its verdict: an
// extendable if runs, an incomplete entry is refused at its end.
// The half every unit shares once its parse is final (plan §41.2a): its
// module links into the live context, then its TU init runs, then an entry's
// run (D25: its statements, in source order, lowered into the entry function
// of its own module), each at the unit's boundary. Linked, the unit is kept
// whatever its init and its run do next: a use of a function no unit defines
// yet fails there, and the unit's definitions stay, as Julia keeps a failed
// input's earlier definitions (plan §42 D27). Running its init is a host-call
// boundary, like main() in madc_cir_execute: runtime services inherit the
// Program's policy.
static bool link_and_run(Program &prog, CirJitSession &jit,
			 Program::EntryTransaction &unit, const char *unit_file,
			 bool &linked)
{
    prog.push_runtime_scope();
    bool ok = false;
    try
    {
	linked = jit.append(&prog, unit_file);
	if ( linked )
	    unit.commit();
	const std::string &run = prog.entry_function_name;
	ok = linked && jit.run_entry_init(&prog, unit_file)
	    && (run.empty()
		|| jit.run_entry_function(&prog, unit_file, run.c_str()));
    }
    catch (...)
    {
	prog.pop_runtime_scope();
	throw;
    }
    prog.pop_runtime_scope();
    return ok;
}

// A unit's parse recorded its diagnostics without rendering them (an attempt
// still being typed must say nothing); the unit is final now. Its link and
// its run render theirs as they record them, and a command that loads a file
// has rendered the file's already: each record shows once.
void InteractiveSession::render_pending_diagnostics()
{
    for ( const Program::Diagnostic &d : prog->diagnostics )
	if ( !d.rendered )
	    prog->print_diagnostic(prog->error(), d);
}

InteractiveSession::Offered InteractiveSession::enter(const std::string &text,
						      bool final,
						      const TakenHook &taken)
{
    // Julia's spelling: the entry's diagnostics cite REPL[N]:line:column.
    // N counts every entry taken, refused ones too, as Julia's REPL[N] and
    // IPython's In [N] do, so a refused entry's number is never reused. An
    // attempt still being typed is not an entry: it cites the number it
    // would take, and takes it only when it is final.
    std::string name = "REPL[" + std::to_string(submit_count + 1) + "]";
    // The entry is one unit (plan §41.3): it is kept once its module links,
    // and a refusal (its parse, its translation or its link) rolls back
    // everything it did to the Program, as Julia leaves nothing of an input
    // that fails to parse. Its diagnostics stay. An attempt the client goes
    // on typing rolls back the same way.
    showed_command = false;
    // A payload is the entry's just taken, never a later one's.
    payload_kind = madc::session_payload::none;
    payload_args.clear();
    if ( command_text(text).is_command() )
    {
	// A command is one line, complete at its end (plan §41.8a): taken at
	// once, numbered and kept in history like any input (IPython's
	// magics), and it keeps no result (D12).
	if ( taken )
	    taken();
	++submit_count;
	bool ok = run_command(text, name);
	render_pending_diagnostics();
	return Offered{ OfferState::taken, ok };
    }
    Program::EntryTransaction entry(*prog);
    prog->entry_number = submit_count + 1;
    Program::EntryVerdict verdict = prog->parse_entry(text, name);
    if ( !final && verdict == Program::EntryVerdict::Incomplete )
	return Offered{ OfferState::incomplete, false };
    if ( !final && verdict == Program::EntryVerdict::CompleteExtendable )
	return Offered{ OfferState::extendable, false };
    if ( taken )
	taken();
    ++submit_count;
    render_pending_diagnostics();
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return Offered{ OfferState::taken, false };
    bool linked = false;
    bool ok = link_and_run(*prog, *jit, entry, prog->intern_file(name), linked);
    if ( linked )
    {
	++entry_count;
	prog->session_units.insert(name);	// its names are the session's
    }
    // D12: an entry whose show ran keeps its value as REPL[N]'s result, as
    // IPython's Out[N] (a run that stopped before its show keeps nothing).
    if ( linked && !prog->entry_shown.empty() )
	prog->keep_entry_result(submit_count);
    return Offered{ OfferState::taken, ok };
}


// The session API speaks std::string; the compiler's completion rows are
// interned names. One conversion, at the API edge.
static std::vector<std::string> entry_texts(const std::vector<madc::dis::istring> &rows)
{
    return std::vector<std::string>(rows.begin(), rows.end());
}

std::vector<std::string> InteractiveSession::complete(const std::string &text,
						      size_t caret, size_t &start)
{
    CommandText c = command_text(text);
    if ( c.is_command() && caret <= c.line_end )
    {
	// The command's own name: the registry's names and aliases that start
	// with it (`?` has none: what follows it is the name it asks about).
	if ( caret <= c.name_end && !c.query )
	{
	    std::vector<std::string> out;
	    start = c.prefix + 1;
	    if ( caret < start )
		return out;
	    const std::string typed = text.substr(start, caret - start);
	    for ( size_t i = 0; i < command_count; ++i )
		if ( strncmp(command_rows[i].name, typed.c_str(), typed.size()) == 0 )
		    out.push_back(command_rows[i].name);
	    for ( size_t i = 0; i < alias_count; ++i )
		if ( strncmp(alias_rows[i].name, typed.c_str(), typed.size()) == 0 )
		    out.push_back(alias_rows[i].name);
	    std::sort(out.begin(), out.end());
	    return out;
	}
	// Its argument completes as an entry, the command blanked.
	return entry_texts(prog->complete_entry(command_argument(text, c), caret, start));
    }
    return entry_texts(prog->complete_entry(text, caret, start));
}

bool InteractiveSession::load(const std::string &path)
{
    load_main = false;
    std::ifstream file(path.c_str(), std::ios::binary);
    if ( !file )
    {
	prog->record_frontend_error(Program::DiagnosticPhase::lexer,
				    "Failed to open file", path.c_str(), 0, 0);
	return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return load_text(text.str(), path);
}

bool InteractiveSession::load_text(const std::string &text, const std::string &path)
{
    // A file is read as gcc reads it, through its translation too: no entry
    // relaxation, bodies kept as roots, and an undefined reference refused
    // at its link (late binding, D27, is an entry's).
    load_main = false;
    const bool had_main = function("main") != NULL;
    Program::ParseModeScope mode(*prog, Program::ParseMode::TranslationUnit);
    Program::EntryTransaction unit(*prog);
    bool parsed = prog->parse_file_unit(text, path);
    render_pending_diagnostics();
    if ( !parsed )
	return false;
    bool linked = false;
    bool ok = link_and_run(*prog, *jit, unit, prog->intern_file(path), linked);
    if ( linked )
    {
	prog->session_units.insert(path);	// its names are the session's
	load_main = !had_main && function("main") != NULL;
    }
    return ok;
}

bool InteractiveSession::run_main(int argc, char **argv, int *status)
{
    const char *unit = prog->intern_file(argc > 0 && argv[0] ? argv[0] : "main");
    // main() is a host-call boundary, as in madc_cir_execute.
    prog->push_runtime_scope();
    bool ok = false;
    try
    {
	ok = jit->run_session_main(prog.get(), unit, argc, argv, status);
    }
    catch (...)
    {
	prog->pop_runtime_scope();
	throw;
    }
    prog->pop_runtime_scope();
    return ok;
}

bool InteractiveSession::run_file(int argc, char **argv)
{
    if ( argc < 1 || !load(argv[0]) )
	return false;
    int status = 0;
    if ( load_main )
	run_main(argc, argv, &status);	// its status is not the session's
    return true;
}

const std::string &InteractiveSession::shown() const
{
    return showed_command ? command_output : prog->entry_shown;
}

std::string InteractiveSession::standard_name()
{
    return Program::standard_canonical_name(prog->language_std);
}

bool InteractiveSession::continues_if(const std::string &line)
{
    return prog->entry_line_continues_if(line);
}

bool InteractiveSession::run_command(const std::string &text,
				     const std::string &name)
{
    CommandText c = command_text(text);
    command_output.clear();
    showed_command = true;
    prog->begin_entry();		// no earlier entry's diagnostics or display
    const CommandRow *row = command_named(c.name(text));
    if ( !row )
    {
	// IPython: "UsageError: Line magic function `%x` not found."
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     "unknown command '"
			     + text.substr(c.prefix, c.name_end - c.prefix)
			     + "' (%help lists them)",
			     prog->intern_file(name), 1, (int)c.prefix + 1);
	return false;
    }
    switch ( row->code )
    {
	case Command::help:
	{
	    // Each command's usage, what it does and its aliases; `:` and `.`
	    // reach them too.
	    size_t width = 0;
	    for ( size_t i = 0; i < command_count; ++i )
		width = std::max(width, strlen(command_rows[i].usage));
	    for ( size_t i = 0; i < command_count; ++i )
	    {
		command_output += std::string(command_rows[i].usage)
		    + std::string(width + 2 - strlen(command_rows[i].usage), ' ')
		    + command_rows[i].summary;
		std::string also;
		for ( size_t a = 0; a < alias_count; ++a )
		    if ( alias_rows[a].code == command_rows[i].code )
			also += (also.empty() ? " (also ." : ", .")
			      + std::string(alias_rows[a].name);
		if ( !also.empty() )
		    command_output += also + ")";
		command_output += "\n";
	    }
	    command_output += "Each name is also written with `:` or `.`"
			      " (`:type EXPR`, `.L FILE`).";
	    return true;
	}
	case Command::type:
	    return type_command(command_argument(text, c), name);
	case Command::pinfo:
	    return pinfo_command(command_argument(text, c), name);
	case Command::whos:
	    whos_command();
	    return true;
	case Command::load:
	    return load_command(command_argument(text, c), name);
	case Command::run:
	    return run_file_command(command_argument(text, c), name);
	case Command::build:
	    return build_command(command_argument(text, c), name);
	case Command::call:
	    return call_command(command_argument(text, c), name);
	case Command::open:
	    return open_command(command_argument(text, c), name);
	case Command::edit:
	    return edit_command(command_argument(text, c), name);
	case Command::quit:
	    return quit_command(command_argument(text, c), name);
	case Command::none:		// no row names it
	    break;
    }
    return false;
}

// The terminal's editor (its contract is madc_session.h's), run by the one
// spawn owner (Process): no shell.
bool madc::run_terminal_editor(const std::string &path, int line,
			       std::string &why)
{
    const char *env = getenv("EDITOR");
    std::vector<std::string> words;
    const bool chosen = env && *env;
    if ( !chosen )
#ifdef _WIN32
	words.push_back("notepad");
#else
	words.push_back("vi");
#endif
    else if ( !ns_common::shell_words(env, words) || words.empty() )
    {
	why = "$EDITOR is no command: " + std::string(env);
	return false;
    }
    ProcessOptions options;
    options.args.assign(words.begin() + 1, words.end());
#ifdef _WIN32
    const bool line_mark = line > 0 && chosen;
#else
    const bool line_mark = line > 0;
#endif
    if ( line_mark )
	options.args.push_back("+" + std::to_string(line));
    options.args.push_back(path);
    options.inherit_stdin = true;
    options.inherit_stdout = true;
    options.inherit_stderr = true;
    Process editor(DataSource("exec://" + words[0]), options);
    if ( !editor.start() )
    {
	why = "the editor '" + words[0] + "' did not start";
	return false;
    }
    editor.wait();
    return true;
}

// `%open FILE` (plan §7f, the IDE layer): FILE in an editor; it need not
// exist (an editor starts a new file). open_in_editor names the editor.
bool InteractiveSession::open_command(const std::string &argument,
				      const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) )
    {
	command_error("%open: no closing quotation, or a backslash at the end",
		      name);
	return false;
    }
    if ( words.size() != 1 )
    {
	command_error("%open takes one FILE", name);
	return false;
    }
    return open_in_editor(words[0], 0, name);
}

// `%edit NAME` (IPython's %edit; plan §7f): the file a session unit defined
// NAME in, at its line — the bindings' origin (%whos's). A name an entry
// defined has no file to open.
bool InteractiveSession::edit_command(const std::string &argument,
				      const std::string &name)
{
    size_t b = argument.find_first_not_of(" \t\r");
    size_t e = argument.find_last_not_of(" \t\r");
    const std::string word = b == std::string::npos
			   ? std::string() : argument.substr(b, e - b + 1);
    bool ok = !word.empty() && !isdigit((unsigned char)word[0]);
    for ( size_t i = 0; ok && i < word.size(); ++i )
	ok = madc::hub::text_buffer::word_byte(word[i]);
    if ( !ok )
    {
	command_error("%edit takes a name", name);
	return false;
    }
    std::vector<Program::SessionBinding> found;
    prog->session_bindings(found);
    for ( const Program::SessionBinding &r : found )
    {
	if ( r.name != word )
	    continue;
	const std::string file = r.file ? r.file : "";
	if ( file.empty() || !std::ifstream(file.c_str()) )
	{
	    command_error("%edit: '" + word + "' was defined in "
			  + (file.empty() ? std::string("no file") : file)
			  + ", which is no file to open", name);
	    return false;
	}
	return open_in_editor(file, r.line, name);
    }
    command_error("%edit: '" + word + "' is not a name the session defined",
		  name);
    return false;
}

// The editor is its host's: the `open` payload (argv: FILE, then the line
// when one is named) for a host that honors payloads — an IDE opens its
// editor, the terminal's client runs run_terminal_editor — else the session
// runs the terminal's editor itself.
bool InteractiveSession::open_in_editor(const std::string &path, int line,
					const std::string &name)
{
    if ( payload_host )
    {
	payload_kind = madc::session_payload::open;
	payload_args.assign(1, path);
	if ( line > 0 )
	    payload_args.push_back(std::to_string(line));
	return true;
    }
    std::string why;
    if ( madc::run_terminal_editor(path, line, why) )
	return true;
    command_error(why, name);
    return false;
}

// `%quit` (clang-repl's %quit, cling's .q, Node's .exit): the session ends.
// Ending is its host's (IPython's ask_exit payload): the `quit` payload for a
// host that honors payloads; any other reads it from ended().
bool InteractiveSession::quit_command(const std::string &argument,
				      const std::string &name)
{
    if ( argument.find_first_not_of(" \t\r") != std::string::npos )
    {
	command_error("%quit takes no argument", name);
	return false;
    }
    quit_asked = true;
    if ( payload_host )
	payload_kind = madc::session_payload::quit;
    return true;
}

bool InteractiveSession::ended(int &status) const
{
    if ( quit_asked )
	status = 0;
    return quit_asked;
}

void InteractiveSession::command_error(const std::string &message,
				       const std::string &name)
{
    prog->add_diagnostic(Program::DiagnosticSeverity::error,
			 Program::DiagnosticPhase::parser, message,
			 prog->intern_file(name), 1, 1);
}

// `%load FILE` (D25, Julia's include): FILE's names defined in this session,
// nothing run — load(), which leaves nothing of a refused file.
bool InteractiveSession::load_command(const std::string &argument,
				      const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) )
    {
	command_error("%load: no closing quotation, or a backslash at the end",
		      name);
	return false;
    }
    if ( words.size() != 1 )
    {
	command_error("%load takes one FILE", name);
	return false;
    }
    return file_command(madc::session_payload::load, words);
}

// `%run [-i] FILE [ARGS]` (D16, D25; IPython's %run, Thonny's F5): FILE's
// main with ARGS split by the shell's rules, its names left for the prompt.
// -i runs it in this session (run_file: load, then main). Without it the run
// is a FRESH session's, which only the host can start: the `run` payload,
// which the host honors as F5 does — restart, load, run.
bool InteractiveSession::run_file_command(const std::string &argument,
					  const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) )
    {
	command_error("%run: no closing quotation, or a backslash at the end",
		      name);
	return false;
    }
    const bool here = !words.empty() && words[0] == "-i";
    if ( here )
	words.erase(words.begin());
    if ( words.empty() )
    {
	command_error("%run needs a FILE", name);
	return false;
    }
    if ( !here && !payload_host )
    {
	command_error("%run starts a fresh session, which this host cannot;"
		      " %run -i FILE runs it in this one", name);
	return false;
    }
    return file_command(here ? madc::session_payload::run_here
			     : madc::session_payload::run, words);
}

// A file command's FILE (plan §7f) opens first, so a mistyped name is refused
// before anything is restarted or loaded and the session survives it. A host
// that honors payloads reads the file itself (an IDE's open buffer is its
// live text, as F5 runs it); with any other host the session loads it here.
bool InteractiveSession::file_command(madc::session_payload kind,
				      std::vector<std::string> &words)
{
    if ( !std::ifstream(words[0].c_str()) )
    {
	prog->record_frontend_error(Program::DiagnosticPhase::lexer,
				    "Failed to open file", words[0].c_str(),
				    0, 0);
	return false;
    }
    if ( payload_host )
    {
	payload_kind = kind;
	payload_args = words;
	return true;
    }
    if ( kind == madc::session_payload::load )
	return load(words[0]);
    if ( kind == madc::session_payload::call )
	return load(words[0]) && call_file(words[0], words[1], NULL);
    if ( kind == madc::session_payload::build )
	return build_file(words[0], std::string(), words[1]);
    std::vector<char *> argv;
    for ( std::string &w : words )
	argv.push_back(&w[0]);
    argv.push_back(NULL);
    return run_file((int)words.size(), argv.data());
}

// `%call FILE(ARGS)` (cling's .x; plan §7f): FILE is the argument up to its
// `(`, and the call's parenthesized ARGS end the line; without them the call
// takes none. The command's words are FILE and the call's text: the line with
// everything before its `(` blanked, so the call's diagnostics cite the
// columns typed. FILE loads, here or by a host that honors payloads (the
// `call` payload), and call_file makes the call.
bool InteractiveSession::call_command(const std::string &argument,
				      const std::string &name)
{
    static const char blanks[] = " \t\r";
    const size_t open = argument.find('(');
    size_t file_end = open == std::string::npos ? argument.size() : open;
    while ( file_end > 0 && strchr(blanks, argument[file_end - 1]) )
	--file_end;
    const size_t b = argument.find_first_not_of(blanks);
    if ( b == std::string::npos || b >= file_end )
    {
	command_error("%call needs a FILE", name);
	return false;
    }
    std::string call;
    if ( open == std::string::npos )
	call = std::string(file_end, ' ') + "()";
    else
    {
	const size_t close = argument.find_last_not_of(blanks);
	if ( argument[close] != ')' )
	{
	    command_error("%call's arguments end the line: %call FILE(ARGS)", name);
	    return false;
	}
	call = std::string(open, ' ') + argument.substr(open, close + 1 - open);
    }
    std::vector<std::string> words;
    words.push_back(argument.substr(b, file_end - b));
    words.push_back(call);
    return file_command(madc::session_payload::call, words);
}

// `%build FILE [-o OUT]` (plan §7f; madcide's Build): FILE compiled to a
// native executable, OUT by default FILE without its extension (the Build
// menu's {base}). FILE is the host's to read, as %load's is: the `build`
// payload for a host that honors payloads, else build_file here.
bool InteractiveSession::build_command(const std::string &argument,
				       const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) )
    {
	command_error("%build: no closing quotation, or a backslash at the end",
		      name);
	return false;
    }
    std::string file, out;
    for ( size_t i = 0; i < words.size(); ++i )
    {
	if ( words[i] == "-o" && i + 1 < words.size() && out.empty() )
	    out = words[++i];
	else if ( words[i] != "-o" && file.empty() )
	    file = words[i];
	else
	{
	    command_error("%build takes one FILE and -o OUT", name);
	    return false;
	}
    }
    if ( file.empty() )
    {
	command_error("%build needs a FILE", name);
	return false;
    }
    if ( out.empty() )
    {
	const size_t sep = file.find_last_of("/\\");
	const size_t base = sep == std::string::npos ? 0 : sep + 1;
	const size_t dot = file.rfind('.');
	if ( dot == std::string::npos || dot <= base )
	{
	    command_error("%build: FILE has no extension to drop; name the"
			  " executable: %build FILE -o OUT", name);
	    return false;
	}
	out = file.substr(0, dot);
    }
    std::vector<std::string> fw;
    fw.push_back(file);
    fw.push_back(out);
    return file_command(madc::session_payload::build, fw);
}

// A build's diagnostic rows (madc_parse_build's) recorded as the session's,
// so they render as every entry's do and a host's rows carry them.
static void record_diagnostic_rows(Program &prog, const madc::value &rows)
{
    if ( !rows.is_array() )
	return;
    for ( const madc::value &r : rows.as_array() )
    {
	if ( !r.is_object() )
	    continue;
	const std::map<std::string, madc::value> &f = r.as_object();
	std::map<std::string, madc::value>::const_iterator it;
	const int64_t sev = (it = f.find("severity_code")) == f.end()
			  ? 0 : it->second.as_integer();
	const int64_t phase = (it = f.find("phase_code")) == f.end()
			    ? 0 : it->second.as_integer();
	const std::string message = (it = f.find("message")) == f.end()
				  ? std::string() : std::string(it->second.as_string());
	const std::string file = (it = f.find("file")) == f.end()
			       ? std::string() : std::string(it->second.as_string());
	const int64_t line = (it = f.find("line")) == f.end()
			   ? 0 : it->second.as_integer();
	const int64_t column = (it = f.find("column")) == f.end()
			     ? 0 : it->second.as_integer();
	prog.add_diagnostic((Program::DiagnosticSeverity)sev,
			    (Program::DiagnosticPhase)phase, message,
			    file.empty() ? NULL : prog.intern_file(file),
			    (int)line, (int)column);
    }
}

bool InteractiveSession::build_file(const std::string &path,
				    const std::string &text,
				    const std::string &out)
{
    showed_command = true;
    command_output.clear();
    prog->clear_diagnostics();		// the build's rows only
    std::string source = text;
    if ( source.empty() )
    {
	std::ifstream file(path.c_str(), std::ios::binary);
	if ( !file )
	{
	    prog->record_frontend_error(Program::DiagnosticPhase::lexer,
					"Failed to open file", path.c_str(), 0, 0);
	    render_pending_diagnostics();
	    return false;
	}
	std::ostringstream read;
	read << file.rdbuf();
	source = read.str();
    }
    // The live-tree build (the Build menu's): a parse of FILE under the
    // session's standard, emitted from that tree.
    std::string display(path), kind("exe"), target(out);
    madc::value rows;
    bool ok = false;
    int64_t h = madc_parse_open(&source, &display, (int64_t)prog->language_std);
    if ( h != 0 )
    {
	ok = madc_parse_build(&rows, h, &kind, &target);
	madc_parse_close(h);
    }
    record_diagnostic_rows(*prog, rows);
    if ( h == 0 )
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::compiler,
			     "%build: no parse of " + path + " opened");
    render_pending_diagnostics();
    if ( ok )
	command_output = "built " + out;
    return ok;
}

// Does a session unit (an entry, a loaded file) define a function NAME?
// The bindings' rows (%whos's) are the one record.
bool InteractiveSession::defines_function(const std::string &name)
{
    std::vector<Program::SessionBinding> found;
    prog->session_bindings(found);
    for ( const Program::SessionBinding &b : found )
	if ( b.kind == madc::name_kind::function && b.name == name )
	    return true;
    return false;
}

bool InteractiveSession::call_file(const std::string &path,
				   const std::string &call, int *status)
{
    const std::string name = "REPL[" + std::to_string(submit_count) + "]";
    showed_command = true;
    command_output.clear();
    // The function is named after FILE: its name without the directory and
    // the last extension (cling's rule). Else FILE's own main (a main an
    // earlier unit defined is not FILE's, as for %run).
    const size_t sep = path.find_last_of("/\\");
    std::string stem = sep == std::string::npos ? path : path.substr(sep + 1);
    const size_t dot = stem.rfind('.');
    if ( dot != std::string::npos && dot > 0 )
	stem.erase(dot);
    std::string callee;
    if ( defines_function(stem) )
	callee = stem;
    else if ( load_main )
	callee = "main";
    else
    {
	command_error("%call: " + path + " defines no function " + stem
		      + ", and no main", name);
	render_pending_diagnostics();
	return false;
    }
    const size_t open = call.find('(');
    const size_t close = call.rfind(')');
    if ( open == std::string::npos || close == std::string::npos || close < open )
    {
	command_error("%call FILE(ARGS)", name);
	render_pending_diagnostics();
	return false;
    }
    if ( callee == "main"
      && call.find_first_not_of(" \t", open + 1) == close )
    {
	// main with no arguments runs as %run -i runs it: argv is FILE.
	std::string arg0(path);
	char *argv[] = { &arg0[0], NULL };
	int st = 0;
	const bool ok = run_main(1, argv, &st);
	if ( status )
	    *status = st;
	render_pending_diagnostics();
	return ok;
    }
    // The call is an entry under the command's REPL[N]: callee(ARGS), the
    // callee's name ending at the `(`, its value shown (D10) and kept by
    // none (a command keeps no result, D12).
    const std::string text = open >= callee.size()
	? std::string(open - callee.size(), ' ') + callee + call.substr(open)
	: callee + call.substr(open);
    Program::EntryTransaction entry(*prog);
    prog->entry_number = submit_count;
    Program::EntryVerdict verdict = prog->parse_entry(text, name);
    render_pending_diagnostics();
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return false;
    bool linked = false;
    const bool ok = link_and_run(*prog, *jit, entry, prog->intern_file(name), linked);
    if ( linked )
    {
	++entry_count;
	prog->session_units.insert(name);
    }
    command_output = prog->entry_shown;
    return ok;
}

// `%type EXPR` (plan §41.8a): the expression's type, never run. It is parsed
// as the session's next entry inside a transaction that rolls back, never
// translated or linked, and the parse's final-expression capture records its
// value type, which is spelled before the rollback (the type may be one the
// attempt made).
bool InteractiveSession::type_command(const std::string &expression,
				      const std::string &name)
{
    Program::EntryTransaction attempt(*prog);
    prog->entry_number = submit_count;
    Program::EntryVerdict verdict = prog->parse_entry(expression, name);
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return false;		// its diagnostics say why
    if ( !prog->entry_final_semicolon_omitted || !prog->entry_value_type )
    {
	size_t at = expression.find_first_not_of(' ');
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     "%type takes an expression, without a `;`",
			     prog->intern_file(name), 1,
			     at == std::string::npos ? 1 : (int)at + 1);
	return false;
    }
    command_output = TypeSpeller(prog.get()).shown(prog->entry_value_type);
    return true;
}

// `%pinfo NAME` / `?NAME` (plan §41.8a, slice 2): what the session knows of
// a name. The name is parsed first as the session's next entry, inside a
// transaction that rolls back, so what an included header or the madc
// dialect would register at the name's first use is registered, and the
// description reads it before the rollback. The attempt's own diagnostics go
// with it: a keyword or a type name is no expression, and is still described.
bool InteractiveSession::pinfo_command(const std::string &argument,
				       const std::string &name)
{
    size_t b = argument.find_first_not_of(" \t");
    size_t e = argument.find_last_not_of(" \t");
    const std::string word = b == std::string::npos
			   ? std::string() : argument.substr(b, e - b + 1);
    bool ok = !word.empty() && !isdigit((unsigned char)word[0]);
    for ( size_t i = 0; ok && i < word.size(); ++i )
	ok = madc::hub::text_buffer::word_byte(word[i]);
    if ( !ok )
    {
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     "%pinfo takes a name",
			     prog->intern_file(name), 1,
			     b == std::string::npos ? 1 : (int)b + 1);
	return false;
    }
    std::string described;
    bool known;
    {
	DiagnosticRenderMute mute;
	Program::EntryTransaction attempt(*prog);
	prog->entry_number = submit_count;
	prog->parse_entry(argument, name);
	known = prog->describe_name(word, described);
    }
    prog->begin_entry();		// the attempt's diagnostics go with it
    if ( !known )
    {
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser, described,
			     prog->intern_file(name), 1, (int)b + 1);
	return false;
    }
    command_output = described;
    return true;
}

void InteractiveSession::bindings(madc::value &out)
{
    std::vector<Program::SessionBinding> found;
    prog->session_bindings(found);
    std::vector<Variable *> objects;
    for ( size_t i = 0; i < found.size(); ++i )
	if ( found[i].var )
	    objects.push_back(found[i].var);
    std::vector<std::string> values;
    if ( !objects.empty() )
	show_rows(objects, values);
    std::vector<madc::value> rows;
    size_t shown = 0;		// the next object's text in `values`
    for ( size_t i = 0; i < found.size(); ++i )
    {
	const Program::SessionBinding &b = found[i];
	std::string value;
	if ( b.var )
	{
	    if ( shown < values.size() )
		value = values[shown];
	    ++shown;
	}
	std::map<std::string, madc::value> f;
	f["name"] = madc::value(b.name);
	f["kind"] = madc::value((int64_t)b.kind);	// madc::name_kind
	f["type"] = madc::value(b.type);
	f["value"] = madc::value(value);
	f["file"] = madc::value(std::string(b.file ? b.file : ""));
	f["line"] = madc::value((int64_t)b.line);
	rows.push_back(madc::value::make_object(f));
    }
    out = madc::value::make_array(rows);
}

// The quiet entry (bindings): parsed as the session's next entry, but its
// unit is its own (`<bindings N>`), never one of session_units, and nothing
// it says renders; the objects' show is appended to its run (Program::
// show_entry_rows). Its diagnostics go with it, as `%pinfo`'s attempt's do.
bool InteractiveSession::show_rows(const std::vector<Variable *> &objects,
				   std::vector<std::string> &texts)
{
    bool ok = false;
    texts.clear();
    {
	DiagnosticRenderMute mute;
	Program::EntryTransaction quiet(*prog);
	prog->entry_number = submit_count;
	prog->entry_show_rows = objects;
	const std::string unit = "<bindings " + std::to_string(++quiet_count) + ">";
	Program::EntryVerdict verdict = prog->parse_entry(";", unit);
	prog->entry_show_rows.clear();
	if ( verdict == Program::EntryVerdict::Complete
	  || verdict == Program::EntryVerdict::CompleteExtendable )
	{
	    bool linked = false;
	    ok = link_and_run(*prog, *jit, quiet, prog->intern_file(unit), linked);
	}
	texts = prog->entry_rows_shown;
    }
    prog->begin_entry();
    return ok;
}

// `%whos` (plan §41.11a step 3d; IPython's): the bindings as a Name / Type /
// Value / Origin table, each column as wide as its widest text in the one
// layout rule's columns. An empty session says so, in IPython's words.
void InteractiveSession::whos_command()
{
    madc::value rows;
    bindings(rows);
    if ( !rows.is_array() || rows.as_array().empty() )
    {
	command_output = "Interactive namespace is empty.";
	return;
    }
    const size_t ncol = 4;
    static const char *const heads[ncol] = { "Name", "Type", "Value", "Origin" };
    std::vector<std::vector<std::string> > table;
    table.push_back(std::vector<std::string>(heads, heads + ncol));
    for ( const madc::value &r : rows.as_array() )
    {
	const std::map<std::string, madc::value> &f = r.as_object();
	std::string origin = f.at("file").as_string();
	if ( f.at("line").as_integer() > 0 )
	    origin += ":" + std::to_string(f.at("line").as_integer());
	std::vector<std::string> cells;
	cells.push_back(f.at("name").as_string());
	cells.push_back(f.at("type").as_string());
	cells.push_back(f.at("value").as_string());
	cells.push_back(origin);
	table.push_back(cells);
    }
    size_t width[ncol] = { 0, 0, 0, 0 };
    for ( size_t r = 0; r < table.size(); ++r )
	for ( size_t c = 0; c < ncol; ++c )
	    width[c] = std::max(width[c], madc::line_width(table[r][c]));
    std::string out;
    for ( size_t r = 0; r < table.size(); ++r )
    {
	if ( r )
	    out += "\n";
	for ( size_t c = 0; c < ncol; ++c )
	{
	    out += table[r][c];
	    if ( c + 1 < ncol )
		out += std::string(width[c] + 2 - madc::line_width(table[r][c]), ' ');
	}
	if ( r == 0 )
	    out += "\n" + std::string(width[0] + width[1] + width[2] + width[3]
				      + 2 * (ncol - 1), '-');
    }
    command_output = out;
}

void *InteractiveSession::function(const char *name)
{
    return jit->function_code(name);
}

void *InteractiveSession::data(const char *name)
{
    return jit->data_address(name);
}
