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

InteractiveSession::InteractiveSession()
    : prog(new Program()), jit(new CirJitSession()), entry_count(0),
      submit_count(0)
{
}

InteractiveSession::InteractiveSession(std::unique_ptr<Program> configured)
    : prog(std::move(configured)), jit(new CirJitSession()), entry_count(0),
      submit_count(0)
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
    return prog->entry_shown;
}

void *InteractiveSession::function(const char *name)
{
    return jit->function_code(name);
}

void *InteractiveSession::data(const char *name)
{
    return jit->data_address(name);
}
