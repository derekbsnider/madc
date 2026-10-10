// madc_theme.cpp — the colour theme's one owner (madc_theme.h).

#include "madc_theme.h"

#include <cstddef>

#include "madcdis/ui_sgr.h"	// sgr: a style's bytes at a depth

namespace madc {

namespace {

// madcide's tools/madcide/profiles/default.theme, its class lines: JOE 4.6's
// default scheme (Comment green; Constant cyan, which Number and String
// inherit; Keyword and Type bold; Preproc blue; Escape and the include path
// bold cyan).
const char default_theme_text[] =
    "keyword bold\n"
    "control bold\n"
    "type bold\n"
    "typename bold\n"
    "string cyan\n"
    "number cyan\n"
    "comment green\n"
    "directive blue\n"
    "include bold cyan\n"
    "escape bold cyan\n";

// A refined class a scheme leaves unnamed takes its nearest named
// ANCESTOR's style (madcide's load_theme rule): a scheme written before the
// class existed still colours `return` as a keyword, an escape as the
// string around it, a namespace as a user type or as a type.
struct ParentRow { HighlightClass child, parent; };
const ParentRow parent_rows[] = {
    { HighlightClass::hcControl, HighlightClass::hcKeyword },
    { HighlightClass::hcTypeName, HighlightClass::hcType },
    { HighlightClass::hcNamespace, HighlightClass::hcTypeName },
    { HighlightClass::hcDirective, HighlightClass::hcKeyword },
    { HighlightClass::hcIncludePath, HighlightClass::hcString },
    { HighlightClass::hcEscape, HighlightClass::hcString },
};

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

} // namespace

bool theme_read(const std::string &text, theme &out, std::string &err)
{
    theme t;
    bool named[highlight_class_count] = {};
    std::size_t line_no = 0, at = 0;
    while ( at <= text.size() )
    {
	std::size_t nl = text.find('\n', at);
	if ( nl == std::string::npos )
	    nl = text.size();
	std::string line = text.substr(at, nl - at);
	at = nl + 1;
	++line_no;
	std::size_t b = 0;
	while ( b < line.size() && blank(line[b]) )
	    ++b;
	if ( b == line.size() || line[b] == '#' || line[b] == '@' )
	    continue;
	std::size_t e = b;
	while ( e < line.size() && !blank(line[e]) )
	    ++e;
	const std::string name = line.substr(b, e - b);
	HighlightClass cls = HighlightClass::hcNone;
	hub::tui_chrome piece = hub::tui_chrome::divider;
	const bool syntax = highlight_class_of(name, cls);
	if ( !syntax && !hub::tui_chrome_of(name, piece) )
	{
	    err = "line " + std::to_string(line_no) + ": no class or chrome named '"
		+ name + "'";
	    return false;
	}
	hub::ui_style style;
	if ( !hub::ui_style_of(line.substr(e), style) )
	{
	    err = "line " + std::to_string(line_no) + ": '" + name
		+ "' has no style the theme can read";
	    return false;
	}
	if ( syntax )
	{
	    t.syntax[(std::size_t)cls] = style;
	    named[(std::size_t)cls] = true;
	}
	else
	{
	    t.chrome[(std::size_t)piece] = style;
	    t.chrome_named[(std::size_t)piece] = true;
	}
    }
    for ( const ParentRow &r : parent_rows )
	if ( !named[(std::size_t)r.child] && named[(std::size_t)r.parent] )
	{
	    t.syntax[(std::size_t)r.child] = t.syntax[(std::size_t)r.parent];
	    named[(std::size_t)r.child] = true;
	}
    out = t;
    return true;
}

const theme &theme_default()
{
    static const theme built_in = []() {
	theme t;
	std::string err;
	theme_read(default_theme_text, t, err);
	return t;
    }();
    return built_in;
}

void theme_marks(const theme &t, const std::vector<std::string> &lines,
		 int64_t standard, hub::ui_colour_depth depth,
		 std::vector<hub::line_mark> &out)
{
    std::string text;
    for ( std::size_t i = 0; i < lines.size(); ++i )
    {
	if ( i )
	    text += '\n';
	text += lines[i];
    }
    std::vector<highlight_span> spans;
    lex_classes(text, standard, spans);
    std::vector<std::vector<int> > cls(lines.size());
    for ( std::size_t i = 0; i < lines.size(); ++i )
	cls[i].assign(lines[i].size(), -1);
    for ( const highlight_span &sp : spans )
    {
	if ( sp.line < 1 || (std::size_t)sp.line > lines.size() || sp.column < 0 )
	    continue;
	std::vector<int> &row = cls[sp.line - 1];
	for ( std::size_t b = (std::size_t)sp.column;
	      b < (std::size_t)(sp.column + sp.length) && b < row.size(); ++b )
	    row[b] = (int)sp.cls;
    }
    for ( std::size_t i = 0; i < cls.size(); ++i )
	for ( std::size_t b = 0; b < cls[i].size(); )
	{
	    std::size_t e = b + 1;
	    while ( e < cls[i].size() && cls[i][e] == cls[i][b] )
		++e;
	    if ( cls[i][b] >= 0 )
	    {
		const hub::ui_style &st = t.of((HighlightClass)cls[i][b]);
		if ( !st.is_normal() )
		{
		    hub::line_mark m;
		    m.line = i;
		    m.from = b;
		    m.to = e;
		    hub::sgr(m.on, hub::ui_style(), st, depth);
		    out.push_back(m);
		}
	    }
	    b = e;
	}
}

} // namespace madc
