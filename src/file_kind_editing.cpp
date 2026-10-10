// file_kind_editing.cpp — the editing rules a file's kind answers (the
// contract is include/madc_file_kinds.h's "editing rules by kind"; ex core
// design decision 12): the line-comment opener, the block delimiters, the
// default indent, and the indent a text shows. The kinds are codes
// (include/madc/bits/file_kinds); a switch on the code, never a name.
//
// Thread contract: pure functions over their arguments.

#include <cstdint>
#include <string>

#include "madc_file_kinds.h"

namespace madc {

bool file_kind_c_syntax(int64_t kind)
{
    if ( file_kind_family(kind) != fkUNKNOWN )
	return true;
    switch ( kind )
    {
	case fkJS: case fkTYPESCRIPT: case fkRUST: case fkGO: case fkJAVA:
	case fkPHP: case fkSWIFT: case fkKOTLIN: case fkCSHARP:
	    return true;
	default:
	    return false;
    }
}

const char *file_kind_line_comment(int64_t kind)
{
    if ( file_kind_c_syntax(kind) )
	return "//";
    switch ( kind )
    {
	case fkSHELL: case fkMAKEFILE: case fkYAML: case fkTOML:
	case fkPYTHON: case fkPERL: case fkRUBY:
	    return "#";
	case fkINI:
	    return ";";
	case fkLUA:
	    return "--";
	default:
	    return "";
    }
}

const char *file_kind_block_open(int64_t kind)
{
    if ( file_kind_c_syntax(kind) )
	return "{";
    return kind == fkPYTHON ? ":" : "";
}

const char *file_kind_block_close(int64_t kind)
{
    return file_kind_c_syntax(kind) ? "}" : "";
}

const char *file_kind_indent_default(int64_t kind)
{
    switch ( kind )
    {
	case fkPYTHON:
	    return "    ";
	case fkYAML:
	    return "  ";
	default:
	    return "\t";
    }
}

std::string indent_detect(const std::string &text)
{
    size_t tabs = 0, spaces = 0;
    size_t prev = 0;		// the last non-blank line's space indent
    size_t steps[9] = { 0 };	// [2..8]: how often the indent stepped by it
    size_t at = 0;
    for ( size_t n = 0; n < 1000 && at < text.size(); ++n )
    {
	size_t nl = text.find('\n', at);
	if ( nl == std::string::npos )
	    nl = text.size();
	const std::string l = text.substr(at, nl - at);
	at = nl + 1;
	if ( l.find_first_not_of(" \t\r\f\v") == std::string::npos )
	    continue;			// blank: no vote
	if ( l[0] == '\t' )
	{
	    ++tabs;
	    continue;			// a tab-led line steps nothing
	}
	size_t k = 0;
	while ( k < l.size() && l[k] == ' ' )
	    ++k;
	if ( k < l.size() && l[k] == '\t' )
	    continue;			// mixed: neither vote
	if ( k > 0 )
	    ++spaces;
	const size_t d = k > prev ? k - prev : prev - k;
	if ( d >= 2 && d <= 8 )
	    ++steps[d];
	prev = k;
    }
    if ( tabs == 0 && spaces == 0 )
	return std::string();
    if ( tabs >= spaces )
	return "\t";
    size_t best = 4, most = 0;
    for ( size_t d = 2; d <= 8; ++d )
	if ( steps[d] > most )
	{
	    most = steps[d];
	    best = d;
	}
    return std::string(best, ' ');
}

} // namespace madc
