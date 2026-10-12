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
#include <type_traits>
#include <stdint.h>
#if defined(_WIN32)
#include <io.h>		// isatty: the default editor runs only on a terminal
#else
#include <unistd.h>	// isatty: the default editor runs only on a terminal
#endif

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
#include "madc_history.h"	// %history's store (1B.7)
#include "madc_documents.h"	// document_buffer: host_documents' answer
#include "madc_posix_io.h"	// host_path_stem / drop_extension: %call's and %build's names
#include "madc_project.h"	// %project: the manifest's reader and writer
#include "madc_modules.h"	// a module's row: the commands it brings (1B.10)

// The command registry (plan §41.8a, D13/D24; the modes and the extension
// point: split arc 1A.2, docs/plans/2026-10-09-command-registry-design.md):
// one row per command. A typed name becomes its row once, at input
// (command_named); what follows dispatches on the row. The built-ins below
// are the table's; a module or the host adds REGISTERED rows to a session
// (InteractiveSession::registered), resolved by the same lookup.
namespace {

typedef madc::command_mode Mode;

struct CommandRow
{
    const char *name;
    InteractiveSession::Command code;
    const char *usage;		// as %help prints it
    const char *summary;
    // The pinfo command's rows: what each shows of a name (info unless set).
    InteractiveSession::Inspect detail;
};

const CommandRow command_rows[] = {
    { "help", InteractiveSession::Command::help, "%help",
      "list the session's commands" },
    { "type", InteractiveSession::Command::type, "%type EXPR",
      "the type of an expression, which is not run" },
    { "pinfo", InteractiveSession::Command::pinfo, "%pinfo NAME",
      "what the session knows of a name (also ?NAME)",
      InteractiveSession::Inspect::info },
    { "pinfo2", InteractiveSession::Command::pinfo, "%pinfo2 NAME",
      "%pinfo, and the source of a name the session defined (also ??NAME)",
      InteractiveSession::Inspect::info_source },
    { "psource", InteractiveSession::Command::pinfo, "%psource NAME",
      "the source of a name the session defined",
      InteractiveSession::Inspect::source },
    { "whos", InteractiveSession::Command::whos, "%whos",
      "the names the session defined: their types, values and origins" },
    { "load", InteractiveSession::Command::load, "%load FILE",
      "define FILE's names in this session; nothing runs" },
    { "run", InteractiveSession::Command::run, "%run [-i] FILE [ARGS]",
      "FILE's main with ARGS, in a fresh session (-i: this one); its names stay. No FILE: the current buffer's, with %args" },
    { "build", InteractiveSession::Command::build, "%build FILE [-o OUT]",
      "FILE built to a native executable, OUT (default: FILE without its extension). No FILE: the current buffer" },
    { "call", InteractiveSession::Command::call, "%call FILE[(ARGS)]",
      "FILE loaded, then the function named after it called with ARGS, else FILE's main" },
    { "open", InteractiveSession::Command::open, "%open FILE",
      "FILE in an editor: the IDE's, else $EDITOR" },
    { "edit", InteractiveSession::Command::edit, "%edit NAME",
      "the file that defines NAME, in an editor at its line" },
    { "quit", InteractiveSession::Command::quit, "%quit",
      "end the session" },
    { "history", InteractiveSession::Command::history,
      "%history [OPTIONS]",
      "this session's entries; -l [N] the last N kept, -g TEXT those holding TEXT, -n numbered" },
    { "reset", InteractiveSession::Command::reset, "%reset",
      "a fresh session under the same standard: every name is gone" },
    { "std", InteractiveSession::Command::standard, "%std [NAME]",
      "the standard in force; NAME (c17, gnu++20, madc): a fresh session under it" },
    { "check", InteractiveSession::Command::check, "%check [FILE]",
      "FILE's diagnostics (the current buffer's when none); nothing runs" },
    { "args", InteractiveSession::Command::args, "%args [ARGS]",
      "the arguments %run gives the current buffer's program; -- clears them" },
    { "project", InteractiveSession::Command::project, "%project [CMD ARGS]",
      "the project: MANIFEST opens it (created when missing); add FILE..., del FILE..., check, build [-o OUT], run [ARGS], close" },
};

const size_t command_count = sizeof(command_rows) / sizeof(command_rows[0]);

// The other REPLs' own spellings (plan §7f): each an alias row naming one of
// the commands above, in the MODE of the REPL it comes from (owner
// 2026-10-09): cling's and Node's are `.` only. A mode's alias may name a
// command of another mode (owner 2026-10-09; design §2.2a): every target
// today is a REPL command.
struct AliasRow
{
    const char *name;
    Mode mode;
    InteractiveSession::Command code;
};

const AliasRow alias_rows[] = {
    { "?", Mode::dot, InteractiveSession::Command::help },	// cling's .?
    { "L", Mode::dot, InteractiveSession::Command::load },	// cling's .L
    { "q", Mode::dot, InteractiveSession::Command::quit },	// cling's .q
    { "exit", Mode::dot, InteractiveSession::Command::quit },	// Node's .exit
    { "x", Mode::dot, InteractiveSession::Command::call },	// cling's .x
};

const size_t alias_count = sizeof(alias_rows) / sizeof(alias_rows[0]);

// A built-in command's name is the REPL modes' (`%` and `.`); an alias's
// only its own mode's.
bool repl_mode(Mode mode)
{
    return mode == Mode::percent || mode == Mode::dot;
}

const CommandRow *command_coded(InteractiveSession::Command code)
{
    for ( size_t i = 0; i < command_count; ++i )
	if ( command_rows[i].code == code )
	    return &command_rows[i];
    return NULL;
}

// The built-in a name names in `mode` (a command's or an alias's); NULL when
// none does.
const CommandRow *command_named(const std::string &name, Mode mode)
{
    if ( repl_mode(mode) )
	for ( size_t i = 0; i < command_count; ++i )
	    if ( name == command_rows[i].name )
		return &command_rows[i];
    for ( size_t i = 0; i < alias_count; ++i )
	if ( alias_rows[i].mode == mode && name == alias_rows[i].name )
	    return command_coded(alias_rows[i].code);
    return NULL;
}

// Any built-in's or alias's name, in any mode: a registration must not
// shadow one (one namespace).
bool builtin_name(const std::string &name)
{
    for ( size_t i = 0; i < command_count; ++i )
	if ( name == command_rows[i].name )
	    return true;
    for ( size_t i = 0; i < alias_count; ++i )
	if ( name == alias_rows[i].name )
	    return true;
    return false;
}

// Where a command stands in an entry (D13, D24 as the owner ruled it on
// 2026-10-09): its first line's first non-blank is the MODE — `%` or `.`
// (the REPL's commands: a name follows, or cling's `?`), `?` (the query:
// `?NAME` is %pinfo NAME, `??NAME` %pinfo2 NAME, `?` alone %help) or `:`
// (colon mode: the rest of the line is ex's, the host's) — except three
// spellings that stay C: `.` and a digit (`.5`), `::` (global scope) and
// `%:` (the `#` digraph). Only the first line is read, so every
// continuation line stays C (a designator's `.x`).
struct CommandText
{
    Mode mode;		// none: the text is no command
    size_t prefix;	// the mode's character
    size_t name_end;	// past the name (past the prefix when there is none)
    size_t line_end;	// the first line's end
    CommandText()
	: mode(Mode::none), prefix(std::string::npos), name_end(0), line_end(0) {}
    bool is_command() const { return mode != Mode::none; }
    std::string name(const std::string &text) const
    {
	if ( mode == Mode::query )
	    return name_end - prefix == 2 ? "pinfo2"	// `??NAME`
		 : text.find_first_not_of(" \t", name_end) < line_end
		 ? "pinfo" : "help";
	if ( mode == Mode::colon )
	    return std::string();
	return text.substr(prefix + 1, name_end - prefix - 1);
    }
    // The text a refusal cites: the prefix and its name, or, with no name,
    // the first word typed.
    std::string cited(const std::string &text) const
    {
	size_t end = name_end;
	if ( end == prefix + 1 )
	{
	    end = text.find_first_of(" \t\n", prefix + 1);
	    if ( end == std::string::npos || end > line_end )
		end = line_end;
	}
	return text.substr(prefix, end - prefix);
    }
};

CommandText command_text(const std::string &text)
{
    CommandText c;
    size_t i = text.find_first_not_of(" \t");
    if ( i == std::string::npos )
	return c;
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    switch ( text[i] )
    {
	case '?': c.mode = Mode::query; break;
	case ':': if ( next != ':' ) c.mode = Mode::colon; break;
	case '%': if ( next != ':' ) c.mode = Mode::percent; break;
	case '.': if ( !isdigit((unsigned char)next) ) c.mode = Mode::dot; break;
	default: break;
    }
    if ( !c.is_command() )
	return c;
    c.prefix = i;
    // The name is the REPL's one word rule's run (text_buffer::word_byte,
    // as completion and word motion read a word), not starting with a
    // digit, or the one byte `?` (cling's `.?`). The query and colon mode
    // read no name.
    size_t e = i + 1;
    if ( repl_mode(c.mode) )
    {
	if ( next == '?' )
	    ++e;
	else if ( !isdigit((unsigned char)next) )
	    while ( e < text.size() && madc::hub::text_buffer::word_byte(text[e]) )
		++e;
    }
    else if ( c.mode == Mode::query && next == '?' )
	++e;		// `??NAME`: %pinfo2 NAME
    c.name_end = e;
    c.line_end = text.find('\n', e);
    if ( c.line_end == std::string::npos )
	c.line_end = text.size();
    return c;
}

// The built-in a command text names: its name in its mode (the query's
// `pinfo` and `help` are the REPL's names); NULL when none does.
const CommandRow *command_row_of(const CommandText &c, const std::string &word)
{
    return command_named(word, c.mode == Mode::query ? Mode::percent : c.mode);
}

// A command's argument, everything before it blanked, so a diagnostic cites
// the column the user typed.
std::string command_argument(const std::string &text, const CommandText &c)
{
    return std::string(c.name_end, ' ')
	 + text.substr(c.name_end, c.line_end - c.name_end);
}

// The rest of a command's first line after its name, without the blanks
// before it.
std::string command_rest(const std::string &text, const CommandText &c)
{
    const std::string rest = text.substr(c.name_end, c.line_end - c.name_end);
    const size_t b = rest.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : rest.substr(b);
}

// A command's one NAME argument (%pinfo, %psource, %edit): the REPL's one
// word rule's run (text_buffer::word_byte), not starting with a digit. False
// when the argument is not one name; `at` is where its first non-blank
// stands (npos: none).
bool name_argument(const std::string &argument, std::string &word, size_t &at)
{
    at = argument.find_first_not_of(" \t\r");
    const size_t e = argument.find_last_not_of(" \t\r");
    word = at == std::string::npos ? std::string() : argument.substr(at, e - at + 1);
    bool ok = !word.empty() && !isdigit((unsigned char)word[0]);
    for ( size_t i = 0; ok && i < word.size(); ++i )
	ok = madc::hub::text_buffer::word_byte(word[i]);
    return ok;
}

} // namespace

bool InteractiveSession::command_of(const std::string &text, Mode &mode,
				    std::string &word, Command &code,
				    std::string &argument)
{
    CommandText c = command_text(text);
    if ( !c.is_command() )
	return false;
    mode = c.mode;
    word = c.name(text);
    const CommandRow *row = command_row_of(c, word);
    code = row ? row->code : Command::none;
    argument = command_rest(text, c);
    return true;
}

// A registered row (design §2.2): a whole command name under the
// recognition rule (a word run not starting with a digit), unique across
// the built-ins, the aliases and the rows registered before it.
bool InteractiveSession::registrable_name(const std::string &name)
{
    if ( name.empty() || isdigit((unsigned char)name[0]) || builtin_name(name) )
	return false;
    for ( char ch : name )
	if ( !madc::hub::text_buffer::word_byte(ch) )
	    return false;
    return true;
}

bool InteractiveSession::add_row(const std::string &name, const std::string &usage,
				 const std::string &summary, CommandHandler handler,
				 int64_t code)
{
    if ( !registrable_name(name) )
	return false;
    for ( const Registered &r : registered )
	if ( r.name == name )
	    return false;
    registered.push_back(Registered{ name, usage, summary, handler, code });
    return true;
}

bool InteractiveSession::add_command(const std::string &name, const std::string &usage,
				     const std::string &summary, CommandHandler handler)
{
    return handler && add_row(name, usage, summary, handler, 0);
}

static_assert(std::is_same<decltype(madc_module_command::handler),
			   InteractiveSession::CommandHandler>::value,
	      "a module's command handler is the registry's CommandHandler");

size_t InteractiveSession::add_module_commands(madc_module_commands_fn rows_of)
{
    size_t count = 0, added = 0;
    const madc_module_command *rows = rows_of ? rows_of(&count) : NULL;
    for ( size_t i = 0; rows && i < count; ++i )
	if ( rows[i].name && rows[i].handler
	  && add_command(rows[i].name, rows[i].usage ? rows[i].usage : "",
			 rows[i].summary ? rows[i].summary : "", rows[i].handler) )
	    ++added;
    return added;
}

void InteractiveSession::register_module_commands()
{
    if ( !prog->is_auto_library_loading_enabled() )
	return;
    for ( const madc::dis::istring &spelling : prog->loaded_modules() )
	if ( modules_seen.insert(spelling).second )
	    add_module_commands(reinterpret_cast<madc_module_commands_fn>(
				   madc_module_entry_point(spelling,
							   madc_module_entry::commands)));
}

bool InteractiveSession::add_host_command(const std::string &name,
					  const std::string &usage,
					  const std::string &summary, int64_t code)
{
    return add_row(name, usage, summary, NULL, code);
}

size_t InteractiveSession::set_host_commands(const madc::value &rows)
{
    std::vector<Registered> kept;
    for ( const Registered &r : registered )
	if ( r.handler )
	    kept.push_back(r);
    registered.swap(kept);
    size_t added = 0;
    if ( !rows.is_array() )
	return added;
    for ( const madc::value &row : rows.as_array() )
    {
	if ( !row.is_object() )
	    continue;
	const std::map<std::string, madc::value> &o = row.as_object();
	auto text = [&](const char *key) {
	    std::map<std::string, madc::value>::const_iterator it = o.find(key);
	    return it != o.end() && it->second.is_string() ? it->second.as_string()
							   : std::string();
	};
	std::map<std::string, madc::value>::const_iterator code = o.find("code");
	if ( code == o.end() || !code->second.is_integer() )
	    continue;
	if ( add_host_command(text("name"), text("usage"), text("summary"),
			      code->second.as_integer()) )
	    ++added;
    }
    return added;
}

InteractiveSession::InteractiveSession()
    : prog(new Program()), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false),
      payload_kind(madc::session_payload::none), payload_command(0),
      payload_host(false), load_main(false), quit_asked(false), quiet_count(0),
      history_file(madc::history_default_path())
{
}

InteractiveSession::InteractiveSession(std::unique_ptr<Program> configured)
    : prog(std::move(configured)), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false),
      payload_kind(madc::session_payload::none), payload_command(0),
      payload_host(false), load_main(false), quit_asked(false), quiet_count(0),
      history_file(madc::history_default_path())
{
}

// The JIT goes first: its modules reference the Program's token tree.
InteractiveSession::~InteractiveSession()
{
    if ( prog )
	prog->owning_session = NULL;
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
    prog->owning_session = this;	// madc::command_add's way in
    if ( !jit->begin_live("REPL") )
	return false;
    register_module_commands();		// the configured Program's modules (1B.10)
    return true;
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
			 bool &linked, bool run = true)
{
    prog.push_runtime_scope();
    bool ok = false;
    try
    {
	linked = jit.append(&prog, unit_file);
	if ( linked )
	    unit.commit();
	const std::string &entry_run = prog.entry_function_name;
	ok = linked && jit.run_entry_init(&prog, unit_file)
	    && (!run || entry_run.empty()
		|| jit.run_entry_function(&prog, unit_file, entry_run.c_str()));
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
    std::string name = madc::session_entry_unit(submit_count + 1);
    // The entry is one unit (plan §41.3): it is kept once its module links,
    // and a refusal (its parse, its translation or its link) rolls back
    // everything it did to the Program, as Julia leaves nothing of an input
    // that fails to parse. Its diagnostics stay. An attempt the client goes
    // on typing rolls back the same way.
    showed_command = false;
    // A payload is the entry's just taken, never a later one's.
    payload_kind = madc::session_payload::none;
    payload_args.clear();
    payload_command = 0;
    kept_source.clear();
    if ( command_text(text).is_command() )
    {
	// A command is one line, complete at its end (plan §41.8a): taken at
	// once, numbered and kept in history like any input (IPython's
	// magics), and it keeps no result (D12).
	if ( taken )
	    taken();
	++submit_count;
	inputs.push_back(text);
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
    inputs.push_back(text);
    render_pending_diagnostics();
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return Offered{ OfferState::taken, false };
    bool linked = false;
    bool ok = link_and_run(*prog, *jit, entry, prog->intern_file(name), linked);
    if ( linked )
    {
	unit_linked(name);
	kept_source = text;			// the host's REPL buffer keeps it
	buffer_entries.push_back(text);
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
	// Colon mode's text is the host's to complete (its ex core).
	if ( c.mode == Mode::colon )
	{
	    start = caret;
	    return std::vector<std::string>();
	}
	// The command's own name: the names valid in its mode — the
	// built-ins', the mode's aliases', the registered rows' (`?` has
	// none: what follows it is the name it asks about).
	if ( caret <= c.name_end && repl_mode(c.mode) )
	{
	    std::vector<std::string> out;
	    start = c.prefix + 1;
	    if ( caret < start )
		return out;
	    const std::string typed = text.substr(start, caret - start);
	    auto offer = [&](const std::string &name) {
		if ( name.compare(0, typed.size(), typed) == 0 )
		    out.push_back(name);
	    };
	    for ( size_t i = 0; i < command_count; ++i )
		offer(command_rows[i].name);
	    for ( size_t i = 0; i < alias_count; ++i )
		if ( alias_rows[i].mode == c.mode )
		    offer(alias_rows[i].name);
	    for ( const Registered &r : registered )
		offer(r.name);
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
    unit_text.clear();
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
	register_module_commands();		// its imports' (1B.10)
	unit_text = text;
    }
    return ok;
}

void InteractiveSession::unit_linked(const std::string &name)
{
    ++entry_count;
    prog->session_units.insert(name);	// its names are the session's
    register_module_commands();		// its imports' (1B.10)
}

namespace {

// TEXT's lines, the text buffer's (a final line break ends the last line).
std::vector<std::string> buffer_lines(const std::string &text)
{
    madc::hub::text_buffer b;
    b.load(text);
    std::vector<std::string> out;
    size_t off, len;
    for ( size_t n = 1; b.line_span(n, off, len); ++n )
	out.push_back(text.substr(off, len));
    return out;
}

std::string lines_text(const std::vector<std::string> &lines, size_t from, size_t to)
{
    std::string t;
    for ( size_t i = from; i < to; ++i )
	t += lines[i] + "\n";
    return t;
}

bool blank_text(const std::string &t)
{
    return t.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

// One entry read as definitions only (Program::entry_definitions_only).
struct DefinitionsOnly
{
    Program &prog;
    explicit DefinitionsOnly(Program &p) : prog(p) { p.entry_definitions_only = true; }
    ~DefinitionsOnly() { prog.entry_definitions_only = false; }
};

} // namespace

bool InteractiveSession::entry_whole(const std::string &text, bool &extendable)
{
    Program::EntryVerdict v;
    {
	Program::EntryTransaction trial(*prog);	// rolled back
	DefinitionsOnly only(*prog);
	v = prog->parse_entry(text, madc::session_entry_unit(submit_count + 1));
    }
    prog->clear_diagnostics();
    extendable = v == Program::EntryVerdict::CompleteExtendable;
    return v != Program::EntryVerdict::Incomplete;
}

// The REPL's grouping of typed lines (the cooked loop's): a line joins the
// entry until the entry is whole, and an if that ended one takes a following
// `else` line (D11). Blank lines before an entry are its; a remainder that
// never becomes whole is one entry.
std::vector<std::pair<size_t, size_t> >
InteractiveSession::buffer_split(const std::vector<std::string> &lines, size_t from, size_t to)
{
    std::vector<std::pair<size_t, size_t> > out;
    size_t start = from;
    std::string text;
    for ( size_t i = from; i < to; ++i )
    {
	text += lines[i] + "\n";
	if ( blank_text(text) )
	    continue;
	// A command line is an entry of its own, never read as code.
	bool extendable = false;
	if ( !command_text(text).is_command() && !entry_whole(text, extendable) )
	    continue;
	if ( extendable && i + 1 < to && continues_if(lines[i + 1]) )
	    continue;
	out.push_back(std::make_pair(start, i + 1));
	start = i + 1;
	text.clear();
    }
    if ( start < to )
	out.push_back(std::make_pair(start, to));
    return out;
}

bool InteractiveSession::redefine(const std::string &text)
{
    const std::string name = madc::session_entry_unit(submit_count + 1);
    Program::EntryTransaction entry(*prog);
    prog->entry_number = submit_count + 1;
    Program::EntryVerdict verdict;
    {
	DefinitionsOnly only(*prog);
	verdict = prog->parse_entry(text, name);
    }
    ++submit_count;
    inputs.push_back(text);
    render_pending_diagnostics();
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return false;
    bool linked = false;
    bool ok = link_and_run(*prog, *jit, entry, prog->intern_file(name), linked,
			   false);
    if ( linked )
	unit_linked(name);
    return ok;
}

// The live reparse (owner 2026-10-12: "if I edit a function in the REPL
// buffer, it should reparse instantly"). The edit is the lines the two texts
// do not share at either end; the entries it touched are read again from
// the edited text, and each that is not one of them unchanged is taken as
// definitions only (redefine). The entries are the ones the host was handed
// while they are its text, else the REPL's grouping of it.
bool InteractiveSession::buffer_edited(const std::string &before, const std::string &after)
{
    const std::vector<std::string> b = buffer_lines(before), a = buffer_lines(after);
    std::vector<std::pair<size_t, size_t> > old;
    size_t at = 0;
    for ( const std::string &e : buffer_entries )
    {
	const std::vector<std::string> el = buffer_lines(e);
	if ( at + el.size() > b.size()
	  || !std::equal(el.begin(), el.end(), b.begin() + at) )
	    break;
	old.push_back(std::make_pair(at, at + el.size()));
	at += el.size();
    }
    if ( old.size() != buffer_entries.size() || at != b.size() )
	old = buffer_split(b, 0, b.size());
    size_t p = 0;
    while ( p < b.size() && p < a.size() && b[p] == a[p] )
	++p;
    size_t s = 0;
    while ( s < b.size() - p && s < a.size() - p
	 && b[b.size() - 1 - s] == a[a.size() - 1 - s] )
	++s;
    if ( p == b.size() && p == a.size() )
	return true;
    // The entries the edit touched, [i, j): an inserted line touches only
    // the entry it lands inside.
    const size_t b_end = b.size() - s;
    const size_t limit = b_end > p ? b_end : p;
    auto touched = [&](size_t k) { return old[k].first < limit && old[k].second > p; };
    size_t i = 0;
    while ( i < old.size() && !touched(i) && old[i].first < p )
	++i;
    size_t j = i;
    while ( j < old.size() && touched(j) )
	++j;
    const size_t from = i < j ? old[i].first : p;
    const size_t to = (i < j ? old[j - 1].second : b_end) + a.size() - b.size();
    std::set<std::string> unchanged;
    for ( size_t k = i; k < j; ++k )
	unchanged.insert(lines_text(b, old[k].first, old[k].second));
    const std::vector<std::pair<size_t, size_t> > fresh = buffer_split(a, from, to);
    bool ok = true;
    std::vector<std::string> entries;
    for ( size_t k = 0; k < i; ++k )
	entries.push_back(lines_text(b, old[k].first, old[k].second));
    for ( const std::pair<size_t, size_t> &e : fresh )
    {
	const std::string t = lines_text(a, e.first, e.second);
	entries.push_back(t);
	if ( !blank_text(t) && !command_text(t).is_command() && !unchanged.count(t) )
	    ok = redefine(t) && ok;
    }
    for ( size_t k = j; k < old.size(); ++k )
	entries.push_back(lines_text(b, old[k].first, old[k].second));
    buffer_entries.swap(entries);
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

const std::vector<madc::code_range> &InteractiveSession::shown_code() const
{
    return showed_command ? command_code : no_code;
}

void InteractiveSession::begin_command_output()
{
    showed_command = true;
    command_output.clear();
    command_code.clear();
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
    begin_command_output();
    prog->begin_entry();		// no earlier entry's diagnostics or display
    if ( c.mode == Mode::colon )
    {
	// Colon mode is the host's (owner 2026-10-09): a host with one
	// answers `:` before the session sees the entry.
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     "'" + c.cited(text) + "' is a colon-mode command, and"
			     " there is no colon mode here (%help lists the"
			     " REPL's commands)",
			     prog->intern_file(name), 1, (int)c.prefix + 1);
	return false;
    }
    const std::string word = c.name(text);
    const CommandRow *row = command_row_of(c, word);
    if ( !row )
    {
	for ( const Registered &r : registered )
	    if ( r.name == word )
		return run_registered(r, command_rest(text, c), name);
	// IPython: "UsageError: Line magic function `%x` not found."
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     "unknown command '" + c.cited(text)
			     + "' (%help lists them)",
			     prog->intern_file(name), 1, (int)c.prefix + 1);
	return false;
    }
    switch ( row->code )
    {
	case Command::help:
	{
	    // ONE registry (design §2.6): each built-in's usage, what it does
	    // and its aliases in their modes, then every registered row, then
	    // how the modes reach them.
	    size_t width = 0;
	    for ( size_t i = 0; i < command_count; ++i )
		width = std::max(width, strlen(command_rows[i].usage));
	    for ( const Registered &r : registered )
		width = std::max(width, r.usage.size());
	    auto line = [&](const std::string &usage, const std::string &summary) {
		command_output += usage + std::string(width + 2 - usage.size(), ' ')
				+ summary;
	    };
	    for ( size_t i = 0; i < command_count; ++i )
	    {
		line(command_rows[i].usage, command_rows[i].summary);
		std::string also;
		for ( size_t a = 0; a < alias_count; ++a )
		    if ( alias_rows[a].code == command_rows[i].code )
			also += std::string(also.empty() ? " (also " : ", ")
			      + (alias_rows[a].mode == Mode::dot ? "." : "%")
			      + alias_rows[a].name;
		if ( !also.empty() )
		    command_output += also + ")";
		command_output += "\n";
	    }
	    for ( const Registered &r : registered )
	    {
		line(r.usage, r.summary);
		command_output += "\n";
	    }
	    command_output += "Each name is also written with `.` (`.type EXPR`);"
			      " `?NAME` asks about a name, `??NAME` shows its"
			      " source too; `:` starts a"
			      " colon-mode (ex) command.";
	    return true;
	}
	case Command::type:
	    return type_command(command_argument(text, c), name);
	case Command::pinfo:
	    return pinfo_command(command_argument(text, c), name, row->detail,
				 row->name);
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
	case Command::history:
	    return history_command(command_argument(text, c), name);
	case Command::reset:
	    return reset_command(command_argument(text, c), name);
	case Command::standard:
	    return std_command(command_argument(text, c), name);
	case Command::check:
	    return check_command(command_argument(text, c), name);
	case Command::args:
	    return args_command(command_argument(text, c), name);
	case Command::project:
	    return project_command(command_argument(text, c), name);
	case Command::none:		// no row names it
	    break;
    }
    return false;
}

bool madc::command_add(const char *name, const char *usage, const char *summary,
		       bool (*handler)(value &out, const char *argument))
{
    Program *p = Program::active_runtime_program();
    if ( !p || !p->owning_session || !name || !handler )
	return false;
    return p->owning_session->add_command(name, usage ? usage : "",
					  summary ? summary : "", handler);
}

// A registered row's run (design §2.3). A host row runs nowhere here: the
// session answers the `command` payload with its code and argument, and
// the host runs it (one with no host to ask is refused). A session row's
// handler is a host call into JIT code (or a native module's), inside the
// Program's runtime scope as run_main's main is.
bool InteractiveSession::run_registered(const Registered &row,
					const std::string &argument,
					const std::string &name)
{
    if ( !row.handler )
    {
	if ( !payload_host )
	{
	    command_error("%" + row.name + " is its host's command, and this"
			  " session has no host to run it", name);
	    return false;
	}
	payload_kind = madc::session_payload::command;
	payload_command = row.code;
	payload_args.assign(1, argument);
	return true;
    }
    madc::value out;
    bool ok = false;
    prog->push_runtime_scope();
    try
    {
	ok = row.handler(out, argument.c_str());
    }
    catch (...)
    {
	prog->pop_runtime_scope();
	throw;
    }
    prog->pop_runtime_scope();
    const std::string text = out.is_string() ? out.as_string() : std::string();
    if ( ok )
	command_output = text;
    else if ( !text.empty() )
	command_error(text, name);
    return ok;
}

bool madc::resolve_buffer(const ReplSession::BufferSource &source,
			  const dis::istring &name, dis::istring &path,
			  std::string &text, bool &has_text, std::string &why)
{
    path = name;
    text.clear();
    has_text = false;
    why.clear();
    if ( source && !source(name, path, text, has_text, why) )
	return false;
    if ( path.empty() )
    {
	why = "no FILE, and this host holds no buffer";
	return false;
    }
    return true;
}

bool InteractiveSession::buffer_text(const madc::dis::istring &name,
				     madc::dis::istring &path, std::string &text,
				     bool &has_text, std::string &why)
{
    return madc::resolve_buffer(buffers, name, path, text, has_text, why);
}

// The thread's host buffers (set_host_buffers): what session_buffer answers
// outside a session. Per thread — the program that installs it asks on it.
static thread_local ReplSession::BufferSource host_buffers;

void madc::set_host_buffers(const ReplSession::BufferSource &source)
{
    host_buffers = source;
}

bool madc::session_buffer(const std::string &name, std::string &path,
			  std::string &text, bool &has_text, std::string &why)
{
    Program *p = Program::active_runtime_program();
    InteractiveSession *s = p ? p->owning_session : NULL;
    dis::istring found;
    const bool ok = s ? s->buffer_text(dis::istring(name), found, text, has_text, why)
		      : resolve_buffer(host_buffers, dis::istring(name),
				       found, text, has_text, why);
    path = found;
    return ok;
}

bool madc::session_buffer(value &out, const char *name)
{
    std::string path, text, why;
    bool has_text = false;
    out = value::make_object();
    if ( !session_buffer(name ? name : "", path, text, has_text, why) )
    {
	out.object()["error"] = value(why);
	return false;
    }
    out.object()["path"] = value(path);
    out.object()["text"] = value(text);
    out.object()["has_text"] = value(has_text);
    return true;
}

bool madc::host_documents(int64_t w, int64_t current)
{
    if ( w == 0 )
    {
	set_host_buffers(ReplSession::BufferSource());
	return true;
    }
    if ( !madc_ui_world(w) )
	return false;
    set_host_buffers([w, current](const dis::istring &name, dis::istring &path,
				  std::string &text, bool &has_text,
				  std::string &why) -> bool {
	hub::world *world = madc_ui_world(w);	// asked now: the world may be gone
	if ( !world )
	{
	    why = "the host's documents are closed";
	    return false;
	}
	return document_buffer(*world, (hub::entity_id)current, name, path, text,
			       has_text, why);
    });
    return true;
}

std::string madc::session_entry_unit(size_t entry)
{
    return "REPL[" + std::to_string(entry) + "]";
}

size_t madc::session_entry_of_unit(const std::string &unit)
{
    static const char head[] = "REPL[";
    const size_t hl = sizeof(head) - 1;
    if ( unit.size() < hl + 2 || unit.compare(0, hl, head) != 0
      || unit[unit.size() - 1] != ']' )
	return 0;
    size_t n = 0;
    for ( size_t i = hl; i + 1 < unit.size(); ++i )
    {
	if ( unit[i] < '0' || unit[i] > '9' )
	    return 0;
	n = n * 10 + (size_t)(unit[i] - '0');
    }
    return n;
}

// The terminal's editor (its contract is madc_session.h's), run by the one
// spawn owner (Process): no shell.
bool madc::show_in_editor(const ReplSession::Editor &editor,
			  const std::string &path, int line, std::string &why)
{
    return editor ? editor(path, line, why)
		  : madc::run_terminal_editor(path, line, why);
}

bool madc::run_terminal_editor(const std::string &path, int line,
			       std::string &why)
{
    const char *env = getenv("EDITOR");
    std::vector<std::string> words;
    const bool chosen = env && *env;
    if ( !chosen )
    {
	// The default is a TERMINAL editor, so it runs only on one: with stdin
	// a pipe or a file, the input would become the editor's keystrokes
	// (`:!cmd` included). An $EDITOR the user set runs as given.
	if ( !isatty(0) )
	{
	    why = "no editor: set $EDITOR (input is not a terminal)";
	    return false;
	}
#ifdef _WIN32
	words.push_back("edit");	// Microsoft Edit (System32 on Windows 11)
#else
	words.push_back("vi");
#endif
    }
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
	why = chosen ? "the editor '" + words[0] + "' did not start"
		     : "no editor: set $EDITOR (the default '" + words[0]
		       + "' did not start)";
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
    std::string word;
    size_t at;
    if ( !name_argument(argument, word, at) )
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
    if ( madc::show_in_editor(editor, path, line, why) )
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

// `%history [-n] [-l [N]] [-g [TEXT]]` (split arc 1B.7, design decision 2;
// IPython's, which the kernel answers): this session's entries, REPL[1] on,
// the %history entry itself the last. -l: the store's last N entries in the
// session's language (10 without N); -g: the store's entries holding TEXT
// (the rest of the line, Ctrl-R's literal rule; bare -g: all of them),
// oldest first, the last N of them with -l. -n numbers each: REPL[N]'s N,
// or its place in the store.
bool InteractiveSession::history_command(const std::string &argument,
					 const std::string &name)
{
    static const char blanks[] = " \t\r";
    bool numbered = false, last = false, holding = false;
    size_t count = 10;
    std::string needle;
    size_t at = 0;
    for (;;)
    {
	at = argument.find_first_not_of(blanks, at);
	if ( at == std::string::npos )
	    break;
	size_t end = argument.find_first_of(blanks, at);
	if ( end == std::string::npos )
	    end = argument.size();
	const std::string word = argument.substr(at, end - at);
	at = end;
	if ( word == "-n" )
	    numbered = true;
	else if ( word == "-g" )
	{
	    holding = true;
	    size_t b = argument.find_first_not_of(blanks, at);
	    size_t e = argument.find_last_not_of(blanks);
	    if ( b != std::string::npos )
		needle = argument.substr(b, e + 1 - b);
	    break;
	}
	else if ( word == "-l" )
	{
	    last = true;
	    size_t b = argument.find_first_not_of(blanks, at);
	    size_t e = b == std::string::npos ? b : argument.find_first_of(blanks, b);
	    const std::string n = b == std::string::npos ? std::string()
		: argument.substr(b, (e == std::string::npos ? argument.size() : e) - b);
	    if ( !n.empty() && n.find_first_not_of("0123456789") == std::string::npos )
	    {
		count = (size_t)strtoul(n.c_str(), NULL, 10);
		at = e == std::string::npos ? argument.size() : e;
	    }
	}
	else
	{
	    command_error("%history: unknown option " + word
			  + " (-n, -l [N], -g [TEXT])", name);
	    return false;
	}
    }
    std::vector<std::string> entries;
    if ( last || holding )
    {
	if ( history_file.empty() )
	{
	    command_error("%history: this session keeps no history file", name);
	    return false;
	}
	entries = madc::history_entries(history_file, standard_name());
    }
    else
	entries = inputs;
    std::vector<size_t> shown;
    for ( size_t i = 0; i < entries.size(); ++i )
	if ( !holding || entries[i].find(needle) != std::string::npos )
	    shown.push_back(i);
    if ( last && shown.size() > count )
	shown.erase(shown.begin(), shown.end() - count);
    command_output.clear();
    for ( size_t i : shown )
    {
	std::string entry = entries[i];
	while ( !entry.empty()
		&& (entry[entry.size() - 1] == '\n' || entry[entry.size() - 1] == '\r') )
	    entry.erase(entry.size() - 1);
	if ( !command_output.empty() )
	    command_output += "\n";
	if ( numbered )
	{
	    char number[16];
	    snprintf(number, sizeof number, "%4u: ", (unsigned)(i + 1));
	    command_output += number;
	}
	for ( char ch : entry )
	{
	    command_output += ch;
	    if ( ch == '\n' && numbered )
		command_output += "      ";	// under the first line's text
	}
    }
    return true;
}

// `%reset` (split arc 1B.7, design decision 3; IPython's %reset -f): a
// fresh session under the same standard, which only the host can start —
// the `reset` payload, as %run's fresh session is (D16).
bool InteractiveSession::reset_command(const std::string &argument,
				       const std::string &name)
{
    if ( argument.find_first_not_of(" \t\r") != std::string::npos )
    {
	command_error("%reset takes no argument", name);
	return false;
    }
    if ( !payload_host )
    {
	command_error("%reset starts a fresh session, which this host cannot", name);
	return false;
    }
    payload_kind = madc::session_payload::reset;
    return true;
}

// `%std [NAME]` (design decision 4; plan D4: switching mid-session is
// unsafe): without NAME, the standard in force as `--std=` spells it; with
// one, a fresh session under it — the `standard` payload, argv NAME — when
// it differs, nothing when it does not. An unknown NAME is refused before
// anything restarts.
bool InteractiveSession::std_command(const std::string &argument,
				     const std::string &name)
{
    static const char blanks[] = " \t\r";
    const size_t b = argument.find_first_not_of(blanks);
    if ( b == std::string::npos )
    {
	command_output = Program::standard_spelling(prog->language_std,
						    prog->gnu_dialect);
	return true;
    }
    std::string spelling = argument.substr(b, argument.find_last_not_of(blanks) + 1 - b);
    const std::string option("--std=");
    if ( spelling.compare(0, option.size(), option) == 0 )
	spelling.erase(0, option.size());
    Program::LanguageStd std;
    bool gnu;
    if ( !Program::standard_of_spelling(spelling, std, gnu) )
    {
	command_error("%std: no standard is spelled '" + spelling
		      + "' (c17, gnu++20, madc: as --std= takes them)", name);
	return false;
    }
    if ( std == prog->language_std && gnu == prog->gnu_dialect )
	return true;
    if ( !payload_host )
    {
	command_error("%std NAME starts a fresh session, which this host cannot",
		      name);
	return false;
    }
    payload_kind = madc::session_payload::standard;
    payload_args.assign(1, spelling);
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
    // Loaded here, FILE's kept text is the host's REPL buffer's; a host
    // that honors the payload loads it, and keeps it, itself.
    unit_text.clear();
    const bool ok = file_command(madc::session_payload::load, words);
    kept_source = unit_text;
    if ( !kept_source.empty() )
	buffer_entries.push_back(kept_source);
    return ok;
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
	// No FILE: the host's current buffer, its live text, with the
	// arguments %args set (split arc 1B.8, design decision 3).
	if ( !payload_host )
	{
	    command_error("%run needs a FILE: this host holds no buffer", name);
	    return false;
	}
	payload_kind = here ? madc::session_payload::run_here
			    : madc::session_payload::run;
	payload_args.assign(1, std::string());
	return true;
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
// live text, as F5 runs it), so a FILE its buffer holds needs no file on disk;
// with any other host the session loads it here.
bool InteractiveSession::buffer_holds(const std::string &file)
{
    madc::dis::istring path;
    std::string text, why;
    bool has_text = false;
    return buffer_text(madc::dis::istring(file), path, text, has_text, why) && has_text;
}

bool InteractiveSession::file_command(madc::session_payload kind,
				      std::vector<std::string> &words)
{
    if ( !(payload_host && buffer_holds(words[0]))
	 && !std::ifstream(words[0].c_str()) )
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
	// No FILE: the host's current buffer (design decision 3); the host
	// names OUT from its path when -o does not.
	if ( !payload_host )
	{
	    command_error("%build needs a FILE: this host holds no buffer", name);
	    return false;
	}
	payload_kind = madc::session_payload::build;
	payload_args.clear();
	payload_args.push_back(std::string());
	payload_args.push_back(out);
	return true;
    }
    if ( out.empty() )
    {
	out = madc::detail::host_path_drop_extension(file);
	if ( out == file )
	{
	    command_error("%build: FILE has no extension to drop; name the"
			  " executable: %build FILE -o OUT", name);
	    return false;
	}
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

// A file command's source: the host's `text` when it handed one, else FILE
// read whole. False, its refusal recorded, when FILE does not open.
static bool command_source(Program &prog, const std::string &path,
			   const std::string *text, std::string &source)
{
    if ( text )
    {
	source = *text;
	return true;
    }
    std::ifstream file(path.c_str(), std::ios::binary);
    if ( !file )
    {
	prog.record_frontend_error(Program::DiagnosticPhase::lexer,
				   "Failed to open file", path.c_str(), 0, 0);
	return false;
    }
    std::ostringstream read;
    read << file.rdbuf();
    source = read.str();
    return true;
}

bool InteractiveSession::build_file(const std::string &path,
				    const std::string &text,
				    const std::string &out)
{
    return build_text(path, text.empty() ? NULL : &text, out);
}

bool InteractiveSession::build_text(const std::string &path,
				    const std::string *text,
				    const std::string &out)
{
    begin_command_output();
    prog->clear_diagnostics();		// the build's rows only
    std::string source;
    if ( !command_source(*prog, path, text, source) )
    {
	render_pending_diagnostics();
	return false;
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

// `%check [FILE]` (split arc 1B.8, design decision 2; gcc -fsyntax-only):
// FILE's diagnostics, nothing run. A host that honors payloads hands the
// backend the buffer's text (the `check` payload, argv FILE — "" for its
// current buffer); any other host's session reads FILE from disk.
bool InteractiveSession::check_command(const std::string &argument,
				       const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) || words.size() > 1 )
    {
	command_error("%check takes one FILE (none: the current buffer)", name);
	return false;
    }
    const std::string file = words.empty() ? std::string() : words[0];
    if ( payload_host )
    {
	payload_kind = madc::session_payload::check;
	payload_args.assign(1, file);
	return true;
    }
    if ( file.empty() )
    {
	command_error("%check needs a FILE: this host holds no buffer", name);
	return false;
    }
    return check_text(file, NULL);
}

// `%args [ARGS]` (split arc 1B.8, design decision 4; madcide's progargs at
// the prompt): the program's arguments a host's %run of its current buffer
// passes — the HOST's to keep across restarts: the `args` payload, argv the
// argument line ("" asks for them; `--` clears them).
bool InteractiveSession::args_command(const std::string &argument,
				      const std::string &name)
{
    if ( !payload_host )
    {
	command_error("%args sets what %run gives a host's buffer; this host"
		      " holds none (%run FILE ARGS)", name);
	return false;
    }
    static const char blanks[] = " \t\r";
    const size_t b = argument.find_first_not_of(blanks);
    payload_kind = madc::session_payload::args;
    payload_args.assign(1, b == std::string::npos ? std::string()
		: argument.substr(b, argument.find_last_not_of(blanks) + 1 - b));
    return true;
}

namespace {

// "1 TU", "N TUs" — the counts a summary prints.
std::string counted(size_t n, const char *one, const char *many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The diagnostics recorded from `first` on, counted: "N errors, M
// warnings" (%check's and %project check's), "" when there are none.
std::string problem_counts(const Program &prog, size_t first)
{
    size_t errors = 0, warnings = 0;
    for ( size_t i = first; i < prog.diagnostics.size(); ++i )
	if ( prog.diagnostics[i].is_error() )
	    ++errors;
	else
	    ++warnings;
    if ( errors == 0 && warnings == 0 )
	return std::string();
    return counted(errors, "error", "errors") + ", "
	+ counted(warnings, "warning", "warnings");
}

} // namespace

bool InteractiveSession::check_text(const std::string &path,
				    const std::string *text)
{
    begin_command_output();
    prog->clear_diagnostics();		// the check's rows only
    std::string source;
    if ( !command_source(*prog, path, text, source) )
    {
	render_pending_diagnostics();
	return false;
    }
    // A parse of its own under the session's standard (parse_check's
    // owner): nothing joins the session.
    std::string display(path);
    madc::value rows;
    int64_t h = madc_parse_open(&source, &display, (int64_t)prog->language_std);
    if ( h != 0 )
    {
	madc_parse_diagnostics(&rows, h);
	madc_parse_close(h);
    }
    record_diagnostic_rows(*prog, rows);
    if ( h == 0 )
    {
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::compiler,
			     "%check: no parse of " + path + " opened");
	render_pending_diagnostics();
	return false;
    }
    const std::string counts = problem_counts(*prog, 0);
    render_pending_diagnostics();
    command_output = path + ": " + (counts.empty() ? std::string("clean") : counts);
    return true;
}

// `%project [CMD ARGS]` (split arc 1B.8, design decision 6): the session's
// project — its manifest read and written through the manifest's owners
// (madc_project.h), checked, built and run through the project lanes
// (madc_project_open / _build / _run). The verb is converted once, here.
namespace {

enum class ProjectVerb : unsigned char { show, open, add, del, check, build, run, close };

struct ProjectVerbRow { const char *name; ProjectVerb verb; };
const ProjectVerbRow project_verb_rows[] = {
    { "add", ProjectVerb::add },
    { "del", ProjectVerb::del },
    { "check", ProjectVerb::check },
    { "build", ProjectVerb::build },
    { "run", ProjectVerb::run },
    { "close", ProjectVerb::close },
};

// The verb a command's first word names; any other word is a MANIFEST.
ProjectVerb project_verb_of(const std::vector<std::string> &words)
{
    if ( words.empty() )
	return ProjectVerb::show;
    for ( const ProjectVerbRow &r : project_verb_rows )
	if ( words[0] == r.name )
	    return r.verb;
    return ProjectVerb::open;
}

} // namespace

bool InteractiveSession::project_command(const std::string &argument,
					 const std::string &name)
{
    std::vector<std::string> words;
    if ( !ns_common::shell_words(argument, words) )
    {
	command_error("%project: no closing quotation, or a backslash at the end",
		      name);
	return false;
    }
    begin_command_output();
    const ProjectVerb verb = project_verb_of(words);
    if ( verb != ProjectVerb::show && verb != ProjectVerb::open
	 && verb != ProjectVerb::close && project_file.empty() )
    {
	command_error("%project " + words[0] + ": no project is open"
		      " (%project MANIFEST opens one)", name);
	return false;
    }
    std::string err;
    switch ( verb )
    {
	case ProjectVerb::show:
	    return project_show(name);
	case ProjectVerb::open:
	    if ( words.size() > 1 )
	    {
		command_error("%project takes one MANIFEST, or add, del, check,"
			      " build, run, close", name);
		return false;
	    }
	    if ( madc::detail::host_path_type_of(words[0])
		     == madc::detail::host_path_type::none
		 && !project_manifest_create(words[0], err) )
	    {
		command_error("%project: " + err, name);
		return false;
	    }
	    {
		ProjectManifest m;
		if ( !read_project_manifest(words[0], m, err) )
		{
		    command_error("%project: " + err, name);
		    return false;
		}
	    }
	    project_file = madc::detail::canonical_path_for_compare(words[0]);
	    project_changed();
	    return project_show(name);
	case ProjectVerb::close:
	    if ( words.size() > 1 )
	    {
		command_error("%project close takes nothing", name);
		return false;
	    }
	    project_file.clear();
	    project_changed();
	    return true;
	case ProjectVerb::add:
	case ProjectVerb::del:
	    if ( words.size() < 2 )
	    {
		command_error("%project " + words[0] + " takes FILE...", name);
		return false;
	    }
	    for ( size_t i = 1; i < words.size(); ++i )
	    {
		const bool ok = verb == ProjectVerb::add
		    ? project_manifest_add_tu(project_file, words[i], err)
		    : project_manifest_remove_tu(project_file, words[i], err);
		if ( !ok )
		{
		    if ( i > 1 )
			project_changed();	// the earlier FILEs stay
		    command_error("%project " + words[0] + ": " + err, name);
		    return false;
		}
	    }
	    project_changed();
	    return true;
	case ProjectVerb::check:
	    if ( words.size() > 1 )
	    {
		command_error("%project check takes nothing", name);
		return false;
	    }
	    return project_check(name);
	case ProjectVerb::build:
	{
	    std::string out;
	    if ( words.size() == 3 && words[1] == "-o" )
		out = words[2];
	    else if ( words.size() != 1 )
	    {
		command_error("%project build takes -o OUT", name);
		return false;
	    }
	    return project_build(out, name);
	}
	case ProjectVerb::run:
	    return project_run(std::vector<std::string>(words.begin() + 1,
							words.end()), name);
    }
    return false;
}

// The open project's manifest changed or closed: a host that honors
// payloads keeps it across restarts (and an IDE opens it), the `project`
// payload.
void InteractiveSession::project_changed()
{
    if ( !payload_host )
	return;
    payload_kind = madc::session_payload::project;
    payload_args.assign(1, project_file);
}

// `%project`: the manifest, its TUs, entry, kind and output.
bool InteractiveSession::project_show(const std::string &name)
{
    if ( project_file.empty() )
    {
	command_output = "no project";
	return true;
    }
    ProjectManifest m;
    std::string err;
    if ( !read_project_manifest(project_file, m, err) )
    {
	command_error("%project: " + err, name);
	return false;
    }
    std::string text = project_file + ": " + counted(m.tus.size(), "TU", "TUs")
	+ ", entry " + m.entry + ", " + project_kind_name(m.kind);
    if ( !m.output_name.empty() )
	text += ", output " + m.output_name;
    for ( const ProjectTU &tu : m.tus )
	text += "\n  " + tu.file;
    command_output = text;
    return true;
}

// `%project check`: every TU's diagnostics, each TU parsed with its
// manifest options (project_open's handles); shown, a count per TU with
// problems, else "MANIFEST: clean".
bool InteractiveSession::project_check(const std::string &name)
{
    prog->clear_diagnostics();		// the check's rows only
    int64_t h = madc_project_open(&project_file);
    if ( h == 0 )
    {
	ProjectManifest m;
	std::string err;
	read_project_manifest(project_file, m, err);
	command_error("%project check: " + (err.empty()
		      ? std::string("no parse of the project opened") : err), name);
	return false;
    }
    madc::value tus;
    madc_project_tus(&tus, h);
    std::string text;
    if ( tus.is_array() )
	for ( const madc::value &row : tus.as_array() )
	{
	    if ( !row.is_object() )
		continue;
	    const std::map<std::string, madc::value> &f = row.as_object();
	    std::map<std::string, madc::value>::const_iterator it;
	    const std::string file = (it = f.find("file")) == f.end()
				   ? std::string() : std::string(it->second.as_string());
	    const int64_t th = (it = f.find("handle")) == f.end()
			     ? 0 : it->second.as_integer();
	    const size_t first = prog->diagnostics.size();
	    if ( th == 0 )
		prog->add_diagnostic(Program::DiagnosticSeverity::error,
				     Program::DiagnosticPhase::lexer,
				     "Failed to open file", prog->intern_file(file),
				     0, 0);
	    else
	    {
		madc::value rows;
		madc_parse_diagnostics(&rows, th);
		record_diagnostic_rows(*prog, rows);
	    }
	    const std::string counts = problem_counts(*prog, first);
	    if ( !counts.empty() )
		text += (text.empty() ? "" : "\n") + file + ": " + counts;
	}
    madc_project_close(h);
    render_pending_diagnostics();
    command_output = text.empty() ? project_file + ": clean" : text;
    return true;
}

// `%project build [-o OUT]`: the --project native lane, to OUT (default:
// project_default_output).
bool InteractiveSession::project_build(const std::string &out_named,
				       const std::string &name)
{
    std::string out = out_named;
    if ( out.empty() )
    {
	ProjectManifest m;
	std::string err;
	if ( !read_project_manifest(project_file, m, err) )
	{
	    command_error("%project build: " + err, name);
	    return false;
	}
	out = project_default_output(project_file, m);
    }
    prog->clear_diagnostics();		// the build's rows only
    std::string kind("exe");
    madc::value rows;
    const bool ok = madc_project_build(&rows, &project_file, &kind, &out);
    record_diagnostic_rows(*prog, rows);
    render_pending_diagnostics();
    if ( ok )
	command_output = "built " + out;
    return ok;
}

// `%project run [ARGS]`: the --project JIT lane in a child, its main given
// ARGS; shown, a status other than 0.
bool InteractiveSession::project_run(const std::vector<std::string> &args,
				     const std::string &name)
{
    std::vector<std::string> argv(args);
    const int64_t rc = madc_project_run(&project_file, &argv);
    if ( rc < 0 )
    {
	command_error(rc == -2 ? "%project run: the project has no TUs"
		      : rc == -1 ? "%project run: the manifest cannot be read"
		      : "%project run: the program did not start", name);
	return false;
    }
    if ( rc != 0 )
	command_output = "exit " + std::to_string(rc);
    return true;
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
    const std::string name = madc::session_entry_unit(submit_count);
    begin_command_output();
    // The function is named after FILE: its name without the directory and
    // the last extension (cling's rule). Else FILE's own main (a main an
    // earlier unit defined is not FILE's, as for %run).
    const std::string stem = madc::detail::host_path_stem(path);
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
    command_code.assign(1, madc::code_range{ 0, command_output.size() });	// a type is code
    return true;
}

// `%pinfo NAME` / `?NAME` (plan §41.8a, slice 2): what the session knows of
// a name. The name is parsed first as the session's next entry, inside a
// transaction that rolls back, so what an included header or the madc
// dialect would register at the name's first use is registered, and the
// description reads it before the rollback. The attempt's own diagnostics go
// with it: a keyword or a type name is no expression, and is still described.
// `%pinfo2 NAME` / `??NAME` adds the source of the name's definitions the
// session holds, IPython's `Source:`; `%psource NAME` is that source alone,
// refused when the session holds none (a header's name, a type).
bool InteractiveSession::pinfo_command(const std::string &argument,
				       const std::string &name, Inspect detail,
				       const char *command)
{
    std::string word;
    size_t b;
    if ( !name_argument(argument, word, b) )
    {
	prog->add_diagnostic(Program::DiagnosticSeverity::error,
			     Program::DiagnosticPhase::parser,
			     std::string("%") + command + " takes a name",
			     prog->intern_file(name), 1,
			     b == std::string::npos ? 1 : (int)b + 1);
	return false;
    }
    std::string source;
    const bool has_source = detail != Inspect::info && binding_source(word, source);
    if ( detail == Inspect::source )
    {
	if ( !has_source )
	{
	    // IPython's: "No source found for x".
	    prog->add_diagnostic(Program::DiagnosticSeverity::error,
				 Program::DiagnosticPhase::parser,
				 "no source found for '" + word + "'",
				 prog->intern_file(name), 1, (int)b + 1);
	    return false;
	}
	command_output = source;
	command_code.assign(1, madc::code_range{ 0, command_output.size() });
	return true;
    }
    std::string described;
    std::vector<madc::code_range> described_code;
    bool known;
    {
	DiagnosticRenderMute mute;
	Program::EntryTransaction attempt(*prog);
	prog->entry_number = submit_count;
	prog->parse_entry(argument, name);
	known = prog->describe_name(word, described, &described_code);
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
    command_code = described_code;
    if ( has_source )
    {
	command_output += "\nSource:\n";
	const size_t at = command_output.size();
	command_output += source;
	command_code.push_back(madc::code_range{ at, command_output.size() });
    }
    return true;
}

// Where a session binding's definition ends, its last line (0: the Program
// recorded no end): a function's definition token's extent (the outline's
// end_line), an object's declaration statement's (parseStatement's).
static int definition_end_line(const Program &p, const Program::SessionBinding &b)
{
    if ( b.kind == Program::TopLevelName::Kind::function )
    {
	for ( size_t i = p.pending_funcs.size(); i-- > 0; )
	{
	    TokenFunc *tf = p.pending_funcs[i] ? p.pending_funcs[i]->as_func_tok() : NULL;
	    if ( tf && tf->line == b.line && tf->var.name == b.name
		 && tf->file && b.file && strcmp(tf->file, b.file) == 0 )
		return tf->end_line;
	}
	return 0;
    }
    for ( size_t i = p.top_decls.size(); i-- > 0; )
    {
	const Program::TopDecl &td = p.top_decls[i];
	if ( td.kind == Program::DeclKind::dkGlobalVar && td.var == b.var )
	    return td.decl ? td.decl->end_line : 0;
    }
    return 0;
}

// Lines `first` to `last` (1-based) of `text`, joined by '\n' without a
// CRLF text's '\r'. False when `first` is past its end.
static bool text_lines(const std::string &text, int first, int last,
		       std::string &out)
{
    out.clear();
    size_t at = 0;
    for ( int n = 1; n < first; ++n )
    {
	at = text.find('\n', at);
	if ( at == std::string::npos )
	    return false;
	++at;
    }
    if ( at >= text.size() )
	return false;
    for ( int n = first; n <= last && at < text.size(); ++n )
    {
	size_t e = text.find('\n', at);
	if ( e == std::string::npos )
	    e = text.size();
	std::string line = text.substr(at, e - at);
	if ( !line.empty() && line[line.size() - 1] == '\r' )
	    line.erase(line.size() - 1);
	out += (n > first ? "\n" : "") + line;
	at = e + 1;
    }
    return true;
}

bool InteractiveSession::binding_source(const std::string &word, std::string &out)
{
    out.clear();
    std::vector<Program::SessionBinding> found;
    prog->session_bindings(found);
    for ( const Program::SessionBinding &r : found )
    {
	if ( r.name != word || !r.file || r.line < 1 )
	    continue;
	std::string text;
	const size_t entry = madc::session_entry_of_unit(r.file);
	if ( entry )
	{
	    if ( entry > inputs.size() )
		continue;
	    text = inputs[entry - 1];
	}
	else
	{
	    madc::dis::istring path;
	    std::string why;
	    bool has_text = false;
	    if ( !buffer_text(madc::dis::istring(r.file), path, text, has_text, why) )
		continue;
	    if ( !has_text )
	    {
		std::ifstream file(path.c_str(), std::ios::binary);
		if ( !file )
		    continue;
		std::ostringstream read;
		read << file.rdbuf();
		text = read.str();
	    }
	}
	std::string lines;
	if ( !text_lines(text, r.line, std::max(r.line, definition_end_line(*prog, r)), lines) )
	    continue;
	out += (out.empty() ? "" : "\n\n") + lines;
    }
    return !out.empty();
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
	f["entry"] = madc::value((int64_t)madc::session_entry_of_unit(b.file ? b.file : ""));
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
