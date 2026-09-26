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

bool InteractiveSession::submit(const std::string &text)
{
    // Julia's spelling: the entry's diagnostics cite REPL[N]:line:column.
    // N counts every entry submitted, as Julia's REPL[N] and IPython's
    // In [N] do, so a refused entry's number is never reused.
    std::string name = "REPL[" + std::to_string(++submit_count) + "]";
    // The entry is one unit (plan §41.3): it is kept once its module links,
    // and a refusal (its parse, its translation or its link) rolls back
    // everything it did to the Program, as Julia leaves nothing of an input
    // that fails to parse. Its diagnostics stay.
    Program::EntryTransaction entry(*prog);
    if ( !prog->parse_entry(text, name) )
	return false;
    // Running the entry's init is a host-call boundary, like main() in
    // madc_cir_execute: runtime services inherit the Program's policy.
    prog->push_runtime_scope();
    bool ok = false;
    try
    {
	bool linked = jit->append(prog.get(), prog->intern_file(name));
	// Linked, its definitions are live whatever its run does next.
	if ( linked )
	    entry.commit();
	ok = linked && run_entry();
    }
    catch (...)
    {
	prog->pop_runtime_scope();
	throw;
    }
    prog->pop_runtime_scope();
    return ok;
}

// The entry's run (D25): its statements, in source order, lowered into the
// entry function of its own module. It runs once, after the module links and
// its init ran. The entry counts from the link: its definitions are live even
// when its run cannot be generated.
bool InteractiveSession::run_entry()
{
    ++entry_count;
    const std::string &run = prog->entry_function_name;
    if ( run.empty() )
	return true;
    void *code = jit->function_code(run.c_str());
    if ( !code )
	return false;
    ((void (*)(void))code)();
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
