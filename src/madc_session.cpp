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
#include "madc_cir.h"
#include "madc_session.h"
#include "madc_type_spelling.h"
#include "madcdis/text_buffer.h"	// the one word rule (word_byte)

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
};

const size_t command_count = sizeof(command_rows) / sizeof(command_rows[0]);

const CommandRow *command_named(const std::string &name)
{
    for ( size_t i = 0; i < command_count; ++i )
	if ( name == command_rows[i].name )
	    return &command_rows[i];
    return NULL;
}

// Where a command stands in an entry (D13): its first line, after blanks,
// starts with `%` or `:` and at once a name, or with `?` (D15). So `::x` and
// the `%:` digraph stay C, and so does every continuation line; C starts no
// statement with `?`.
struct CommandText
{
    size_t prefix;	// the `%`, `:` or `?`; npos when the text is no command
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
	 || (text[i] != '%' && text[i] != ':') )
	return c;
    // The name is the REPL's one word rule's run (text_buffer::word_byte,
    // as completion and word motion read a word), not starting with a digit.
    size_t e = i + 1;
    if ( !madc::hub::text_buffer::word_byte(text[e])
	 || isdigit((unsigned char)text[e]) )
	return c;
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

InteractiveSession::InteractiveSession()
    : prog(new Program()), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false)
{
}

InteractiveSession::InteractiveSession(std::unique_ptr<Program> configured)
    : prog(std::move(configured)), jit(new CirJitSession()), entry_count(0),
      submit_count(0), showed_command(false)
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
	return false;
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
// still being typed must say nothing); the unit is final now.
void InteractiveSession::render_parse_diagnostics()
{
    for ( const Program::Diagnostic &d : prog->diagnostics )
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
    if ( command_text(text).is_command() )
    {
	// A command is one line, complete at its end (plan §41.8a): taken at
	// once, numbered and kept in history like any input (IPython's
	// magics), and it keeps no result (D12).
	if ( taken )
	    taken();
	++submit_count;
	bool ok = run_command(text, name);
	render_parse_diagnostics();
	return Offered{ OfferState::Taken, ok };
    }
    Program::EntryTransaction entry(*prog);
    prog->entry_number = submit_count + 1;
    Program::EntryVerdict verdict = prog->parse_entry(text, name);
    if ( !final && verdict == Program::EntryVerdict::Incomplete )
	return Offered{ OfferState::Incomplete, false };
    if ( !final && verdict == Program::EntryVerdict::CompleteExtendable )
	return Offered{ OfferState::Extendable, false };
    if ( taken )
	taken();
    ++submit_count;
    render_parse_diagnostics();
    if ( verdict != Program::EntryVerdict::Complete
      && verdict != Program::EntryVerdict::CompleteExtendable )
	return Offered{ OfferState::Taken, false };
    bool linked = false;
    bool ok = link_and_run(*prog, *jit, entry, prog->intern_file(name), linked);
    if ( linked )
	++entry_count;
    // D12: an entry whose show ran keeps its value as REPL[N]'s result, as
    // IPython's Out[N] (a run that stopped before its show keeps nothing).
    if ( linked && !prog->entry_shown.empty() )
	prog->keep_entry_result(submit_count);
    return Offered{ OfferState::Taken, ok };
}

std::vector<std::string> InteractiveSession::complete(const std::string &text,
						      size_t caret, size_t &start)
{
    CommandText c = command_text(text);
    if ( c.is_command() && caret <= c.line_end )
    {
	// The command's own name: the registry's names that start with it
	// (`?` has none: what follows it is the name it asks about).
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
	    std::sort(out.begin(), out.end());
	    return out;
	}
	// Its argument completes as an entry, the command blanked.
	return prog->complete_entry(command_argument(text, c), caret, start);
    }
    return prog->complete_entry(text, caret, start);
}

bool InteractiveSession::load(const std::string &path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if ( !file )
    {
	prog->record_frontend_error(Program::DiagnosticPhase::lexer,
				    "Failed to open file", path.c_str(), 0, 0);
	return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    // A file is read as gcc reads it, through its translation too: no entry
    // relaxation, bodies kept as roots, and an undefined reference refused
    // at its link (late binding, D27, is an entry's).
    Program::ParseModeScope mode(*prog, Program::ParseMode::TranslationUnit);
    Program::EntryTransaction unit(*prog);
    bool parsed = prog->parse_file_unit(text.str(), path);
    render_parse_diagnostics();
    if ( !parsed )
	return false;
    bool linked = false;
    return link_and_run(*prog, *jit, unit, prog->intern_file(path), linked);
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

const std::string &InteractiveSession::shown() const
{
    return showed_command ? command_output : prog->entry_shown;
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
	    // Each command's usage and what it does; `:` reaches them too.
	    size_t width = 0;
	    for ( size_t i = 0; i < command_count; ++i )
		width = std::max(width, strlen(command_rows[i].usage));
	    for ( size_t i = 0; i < command_count; ++i )
		command_output += std::string(command_rows[i].usage)
		    + std::string(width + 2 - strlen(command_rows[i].usage), ' ')
		    + command_rows[i].summary + "\n";
	    command_output += "Each is also written with `:` (`:type EXPR`).";
	    return true;
	}
	case Command::type:
	    return type_command(command_argument(text, c), name);
	case Command::pinfo:
	    return pinfo_command(command_argument(text, c), name);
    }
    return false;
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

void *InteractiveSession::function(const char *name)
{
    return jit->function_code(name);
}

void *InteractiveSession::data(const char *name)
{
    return jit->data_address(name);
}
