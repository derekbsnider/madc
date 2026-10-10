#ifndef __HIGHLIGHT_CLASS_H
#define __HIGHLIGHT_CLASS_H 1

// highlight_class.h — what KIND of thing a lexed token is, for
// presentation: the classes, their names at the value boundary, and the
// classifier's C++ face (split arc 1B.9). Moved out of tokens.h so a
// renderer and a theme read the classes without the compiler's token
// header.
//
// Thread contract: constants and pure functions; lex_classes keeps the
// runtime-eval confinement (one Program per thread).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Highlight classification (madcide AST-2 / IDE-7): what KIND of thing a
// lexed token is, for presentation. The one classifier is
// madc_token_highlight_class (lexer.cpp — the token-vocabulary owner);
// names cross the value boundary via highlight_class_name, and come back
// through highlight_class_of. A theme maps each class to a style
// (madc_theme.h: madc's one built-in default, madc.ini's `theme`, an IDE's
// own files; split arc 1B.9).
enum class HighlightClass : unsigned char
{
    hcNone = 0,		// operators / punctuation — unstyled
    hcKeyword,
    hcIdent,
    hcNumber,
    hcString,		// string AND char literals
    hcComment,		// from leading trivia (keep_trivia mode)
    hcType,		// datatype spellings (tkDeclare)
    hcFunction,		// an identifier the tree knows as a function name
    hcControl,		// a control-flow keyword (return if for while switch ...)
    hcTypeName,		// an identifier the parse READ as a type-name (a user's
			// class, struct, enum, typedef — note_type_name_use)
    hcDirective,	// a preprocessor directive's `#name` (from the line text)
    hcIncludePath,	// the header-name an #include / #load names
    hcEscape,		// an escape sequence inside a string or char literal
    // A NAME, by what the parse tree resolved it to (the use node's
    // TokenVar::name_tok / TokenMember::object_tok):
    hcVariable,		// a variable (global or local)
    hcParameter,	// a function parameter
    hcMember,		// a data member after `.` / `->`
    hcEnumerator,	// an enumeration constant
    hcNamespace		// an identifier the parse READ as a namespace-name (a
			// qualifier, a definition, an alias, a using-directive —
			// note_namespace_name_use)
};

inline const char *highlight_class_name(HighlightClass c)
{
    switch ( c )
    {
	case HighlightClass::hcNone:	 return "none";
	case HighlightClass::hcKeyword:	 return "keyword";
	case HighlightClass::hcIdent:	 return "ident";
	case HighlightClass::hcNumber:	 return "number";
	case HighlightClass::hcString:	 return "string";
	case HighlightClass::hcComment:	 return "comment";
	case HighlightClass::hcType:	 return "type";
	case HighlightClass::hcFunction: return "function";
	case HighlightClass::hcControl:	 return "control";
	case HighlightClass::hcTypeName: return "typename";
	case HighlightClass::hcDirective: return "directive";
	case HighlightClass::hcIncludePath: return "include";
	case HighlightClass::hcEscape:	 return "escape";
	case HighlightClass::hcVariable: return "variable";
	case HighlightClass::hcParameter: return "parameter";
	case HighlightClass::hcMember:	 return "member";
	case HighlightClass::hcEnumerator: return "enumerator";
	case HighlightClass::hcNamespace: return "namespace";
    }
    return "none";
}

// The number of classes (the enum's values are 0 .. count - 1).
const std::size_t highlight_class_count = (std::size_t)HighlightClass::hcNamespace + 1;

// The class a NAME spells (highlight_class_name's inverse, the one table):
// false for a name no class has.
inline bool highlight_class_of(const std::string &name, HighlightClass &out)
{
    for ( std::size_t i = 0; i < highlight_class_count; ++i )
	if ( name == highlight_class_name((HighlightClass)i) )
	{
	    out = (HighlightClass)i;
	    return true;
	}
    return false;
}

namespace madc {

// One classified span: a 1-based line, its 0-based BYTE column and byte
// length, the class.
struct highlight_span
{
    long line;
    long column;
    long length;
    HighlightClass cls;
};

// A byte range [from, to) of a shown text that is code — `%type`'s type,
// `%pinfo`'s declarations (split arc 1B.9): a host lexes it and colours it.
// A shown value is not code.
struct code_range
{
    size_t from;
    size_t to;
};

// THE classifier's C++ face: `text` lexed alone (no header ingested, its
// macros unexpanded) under `standard` (a --std= code or a family head, as
// madc::parse_open takes it; 0 = the default), each token classified as
// madc::lex_spans classifies it, the classes converted ONCE to the enum;
// sorted by line, then column. Nothing runs, nothing is retained, and the
// calling thread's Program keeps its diagnostics. Defined in parser.cpp.
void lex_classes(const std::string &text, int64_t standard,
		 std::vector<highlight_span> &out);

// A parse handle's spans (madc::parse_spans' rows) as enums, through the
// same conversion: the parse's classes refine the lexical ones, and text the
// parse did not reach keeps them. False (and an empty `out`) = a bad handle.
// Defined in parser.cpp.
bool parse_classes(int64_t handle, std::vector<highlight_span> &out);

} // namespace madc

#endif // __HIGHLIGHT_CLASS_H
