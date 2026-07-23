// madc::error — see include/libmadc/error.h

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)


#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "libmadc/error.h"

#include <sstream>

namespace madc {

namespace {

error::severity severity_from_program(Program::DiagnosticSeverity sev)
{
    switch ( sev )
    {
	case Program::DiagnosticSeverity::warning: return error::severity::warning;
	case Program::DiagnosticSeverity::error:   return error::severity::error;
    }
    return error::severity::error;
}

error::phase phase_from_program(Program::DiagnosticPhase ph)
{
    switch ( ph )
    {
	case Program::DiagnosticPhase::lexer:    return error::phase::lexer;
	case Program::DiagnosticPhase::parser:   return error::phase::parser;
	case Program::DiagnosticPhase::compiler: return error::phase::compiler;
	case Program::DiagnosticPhase::runtime:  return error::phase::runtime;
	case Program::DiagnosticPhase::unknown:  return error::phase::unknown;
    }
    return error::phase::unknown;
}

} // namespace

error::error()
    : level(severity::error), stage(phase::unknown), line(0), column(0)
{
}

error::error(severity sev, phase ph, const std::string &msg,
	     const std::string &src_file, int src_line, int src_column)
    : level(sev), stage(ph), message(msg), file(src_file),
      line(src_line), column(src_column)
{
}

bool error::operator==(const error &other) const
{
    return level == other.level
	&& stage == other.stage
	&& message == other.message
	&& file == other.file
	&& line == other.line
	&& column == other.column;
}

const char *error::severity_name(severity sev)
{
    switch ( sev )
    {
	case severity::warning: return "warning";
	case severity::error:   return "error";
    }
    return "error";
}

const char *error::phase_name(phase ph)
{
    switch ( ph )
    {
	case phase::unknown:  return "unknown";
	case phase::lexer:    return "lexer";
	case phase::parser:   return "parser";
	case phase::compiler: return "compiler";
	case phase::runtime:  return "runtime";
    }
    return "unknown";
}

std::string error::to_string() const
{
    std::ostringstream os;
    if ( !file.empty() )
	os << file << ':' << line << ':' << column << ": ";
    os << severity_name(level) << ": " << message;
    return os.str();
}

std::vector<error> make_errors_from_program_diagnostics(const Program &pgm)
{
    std::vector<error> out;
    out.reserve(pgm.diagnostics.size());
    for ( const auto &diag : pgm.diagnostics )
    {
	out.push_back(error(severity_from_program(diag.severity),
			    phase_from_program(diag.phase),
			    diag.message,
			    diag.file,
			    diag.line,
			    diag.column));
    }
    return out;
}

} // namespace madc
