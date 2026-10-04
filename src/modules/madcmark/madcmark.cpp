// modules/madcmark/madcmark.cpp — THE madcmark MODULE: madc::markdown_tree,
// the ONE cmark-gfm caller in madc (include/madcdis/markdown.h states the
// contract), and the module's C API (include/madc/madcmark.h — what
// `import madcmark;` binds). cmark-gfm is a dependency (owner 2026-10-04,
// plan docs/plans/2026-10-03-chthonia-windows-macos.md §7d), linked
// statically into this module by src/madcmark.mk and never into libmadc.
// The module binds libmadc's own symbols (value) at load, from the image
// that imported it.

#include "madcdis/markdown.h"

#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace madc {

namespace {

// Once per process: the core extensions join cmark-gfm's registry (its
// own refcounted global); C++11 makes the static's initialization
// thread-safe.
void ensure_extensions()
{
    static bool once = (cmark_gfm_core_extensions_ensure_registered(), true);
    (void)once;
}

// GitHub's extensions, the ones a README is written in. tagfilter is an
// HTML renderer's filter, not syntax.
const char *const extension_names[] = { "table", "strikethrough", "autolink", "tasklist" };

// cmark-gfm's node type -> markdown::node_kind, at the boundary, once. The
// extensions register their node types at run time, so those are known by
// the type string their extension gives them.
markdown::node_kind kind_of(cmark_node *n)
{
    switch ( cmark_node_get_type(n) )
    {
	case CMARK_NODE_DOCUMENT:		return markdown::node_kind::document;
	case CMARK_NODE_BLOCK_QUOTE:		return markdown::node_kind::block_quote;
	case CMARK_NODE_LIST:			return markdown::node_kind::list;
	case CMARK_NODE_ITEM:			return markdown::node_kind::item;
	case CMARK_NODE_CODE_BLOCK:		return markdown::node_kind::code_block;
	case CMARK_NODE_HTML_BLOCK:		return markdown::node_kind::html_block;
	case CMARK_NODE_PARAGRAPH:		return markdown::node_kind::paragraph;
	case CMARK_NODE_HEADING:		return markdown::node_kind::heading;
	case CMARK_NODE_THEMATIC_BREAK:		return markdown::node_kind::thematic_break;
	case CMARK_NODE_FOOTNOTE_DEFINITION:	return markdown::node_kind::footnote_definition;
	case CMARK_NODE_TEXT:			return markdown::node_kind::text;
	case CMARK_NODE_SOFTBREAK:		return markdown::node_kind::softbreak;
	case CMARK_NODE_LINEBREAK:		return markdown::node_kind::linebreak;
	case CMARK_NODE_CODE:			return markdown::node_kind::code;
	case CMARK_NODE_HTML_INLINE:		return markdown::node_kind::html_inline;
	case CMARK_NODE_EMPH:			return markdown::node_kind::emph;
	case CMARK_NODE_STRONG:			return markdown::node_kind::strong;
	case CMARK_NODE_LINK:			return markdown::node_kind::link;
	case CMARK_NODE_IMAGE:			return markdown::node_kind::image;
	case CMARK_NODE_FOOTNOTE_REFERENCE:	return markdown::node_kind::footnote_reference;
	default:
	    break;
    }
    const char *t = cmark_node_get_type_string(n);
    if ( !t )
	return markdown::node_kind::none;
    if ( strcmp(t, "table") == 0 )
	return markdown::node_kind::table;
    if ( strcmp(t, "table_row") == 0 || strcmp(t, "table_header") == 0 )
	return markdown::node_kind::table_row;	// the header row: `header` says so
    if ( strcmp(t, "table_cell") == 0 )
	return markdown::node_kind::table_cell;
    if ( strcmp(t, "strikethrough") == 0 )
	return markdown::node_kind::strikethrough;
    return markdown::node_kind::none;
}

markdown::column_align align_code(uint8_t a)
{
    switch ( a )
    {
	case 'l': return markdown::column_align::left;
	case 'c': return markdown::column_align::center;
	case 'r': return markdown::column_align::right;
	default:  return markdown::column_align::none;
    }
}

value code_value(markdown::node_kind k)	{ return value((int64_t)k); }

std::string text_of(const char *s)
{
    return s ? s : "";
}

// One node and its subtree, the shape markdown.h states.
value node_value(cmark_node *n)
{
    std::map<std::string, value> f;
    markdown::node_kind k = kind_of(n);
    f["kind"] = code_value(k);
    f["line"] = value((int64_t)cmark_node_get_start_line(n));
    f["col"] = value((int64_t)cmark_node_get_start_column(n));
    f["end_line"] = value((int64_t)cmark_node_get_end_line(n));
    f["end_col"] = value((int64_t)cmark_node_get_end_column(n));
    f["text"] = value(text_of(cmark_node_get_literal(n)));
    switch ( k )
    {
	case markdown::node_kind::heading:
	    f["level"] = value((int64_t)cmark_node_get_heading_level(n));
	    break;
	case markdown::node_kind::list:
	    f["list"] = value((int64_t)(cmark_node_get_list_type(n) == CMARK_ORDERED_LIST
				       ? markdown::list_kind::ordered
				       : markdown::list_kind::bullet));
	    f["start"] = value((int64_t)cmark_node_get_list_start(n));
	    f["tight"] = value(cmark_node_get_list_tight(n) != 0);
	    break;
	case markdown::node_kind::item:
	{
	    const char *t = cmark_node_get_type_string(n);
	    bool task = t && strcmp(t, "tasklist") == 0;
	    f["task"] = value(task);
	    f["checked"] = value(task && cmark_gfm_extensions_get_tasklist_item_checked(n));
	    break;
	}
	case markdown::node_kind::code_block:
	{
	    int length = 0, offset = 0;	// cmark-gfm writes all three
	    char fence = 0;
	    f["info"] = value(text_of(cmark_node_get_fence_info(n)));
	    f["fenced"] = value(cmark_node_get_fenced(n, &length, &offset, &fence) != 0);
	    break;
	}
	case markdown::node_kind::link:
	case markdown::node_kind::image:
	    f["url"] = value(text_of(cmark_node_get_url(n)));
	    f["title"] = value(text_of(cmark_node_get_title(n)));
	    break;
	case markdown::node_kind::table:
	{
	    uint16_t cols = cmark_gfm_extensions_get_table_columns(n);
	    uint8_t *al = cmark_gfm_extensions_get_table_alignments(n);
	    std::vector<value> aligns;
	    for ( uint16_t i = 0; i < cols; ++i )
		aligns.push_back(value((int64_t)align_code(al ? al[i] : 0)));
	    f["align"] = value::make_array(aligns);
	    break;
	}
	case markdown::node_kind::table_row:
	    f["header"] = value(cmark_gfm_extensions_get_table_row_is_header(n) != 0);
	    break;
	default:
	    break;
    }
    std::vector<value> kids;
    for ( cmark_node *c = cmark_node_first_child(n); c; c = cmark_node_next(c) )
	kids.push_back(node_value(c));
    f["children"] = value::make_array(kids);
    return value::make_object(f);
}

} // namespace

value markdown_tree(const std::string &text)
{
    ensure_extensions();
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS
				       | CMARK_OPT_FOOTNOTES);
    for ( size_t i = 0; i < sizeof extension_names / sizeof *extension_names; ++i )
    {
	cmark_syntax_extension *e = cmark_find_syntax_extension(extension_names[i]);
	if ( e )
	    cmark_parser_attach_syntax_extension(p, e);
    }
    cmark_parser_feed(p, text.data(), text.size());
    cmark_node *doc = cmark_parser_finish(p);
    value out = doc ? node_value(doc) : value::make_object();
    if ( doc )
	cmark_node_free(doc);
    cmark_parser_free(p);
    return out;
}

} // namespace madc

using madc::value;

// ------------------------------------------------------ the module's C API
extern "C" {

void *madcmark_parse(void *result, const char *text)
{
    value &out = *(value *)result;
    out = madc::markdown_tree(madc::text_of(text));
    return result;
}

} // extern "C"
