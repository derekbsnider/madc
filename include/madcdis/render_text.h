#ifndef __MADCDIS_RENDER_TEXT_H
#define __MADCDIS_RENDER_TEXT_H 1

// madcdis/render_text.h — the level-0 renderer (Track 7 Phase 1, S3):
// sequential text, the bottom of the capability ladder and the one that
// always works. TYPESETTING ONLY (design, Decided): headings, item
// bullets, action brackets, word wrap. It composes no sentences — prose
// arrives finished from the projection library (prose.h). A text MUD
// interface is a screen reader for the world; this is the screen reader.
//
// THREAD-SAFETY CONTRACT: pure function of its arguments.

#include <string>

#include "madcdis/uinode.h"
#include "madcdis/prose.h"

namespace madc {
namespace hub {

// Greedy word wrap at `width` columns; explicit newlines are respected.
inline std::string wrap_text(const std::string &text, size_t width)
{
    std::string out;
    size_t line_len = 0;
    size_t i = 0;
    while ( i < text.size() )
    {
	if ( text[i] == '\n' )
	{
	    out += '\n';
	    line_len = 0;
	    ++i;
	    continue;
	}
	size_t end = i;
	while ( end < text.size() && text[end] != ' ' && text[end] != '\n' )
	    ++end;
	size_t word_len = end - i;
	if ( line_len > 0 && line_len + 1 + word_len > width )
	{
	    out += '\n';
	    line_len = 0;
	}
	else if ( line_len > 0 )
	{
	    out += ' ';
	    ++line_len;
	}
	out.append(text, i, word_len);
	line_len += word_len;
	i = end;
	while ( i < text.size() && text[i] == ' ' )
	    ++i;
    }
    return out;
}

namespace detail {

inline void render_text_node(const roles &r, const uinode &n,
			     size_t width, std::string &out)
{
    std::string label = prose::text_of(n.label);

    if ( n.role == r.heading )
    {
	out += "=== " + label + " ===\n";
    }
    else if ( n.role == r.content || n.role == r.status || n.role == r.edit )
    {
	// An edit region linearizes as its text — level 0 has no cursor;
	// the document simply prints (the same tree a grid renderer
	// presents as an editable window).
	std::string text = node_text(n);
	if ( !text.empty() )
	    out += wrap_text(text, width) + "\n";
    }
    else if ( n.role == r.item )
    {
	out += "  " + node_text(n) + "\n";
    }
    else if ( n.role == r.action )
    {
	out += "[" + label + "]\n";
    }
    else if ( n.role == r.separator )
    {
	out += "\n";
    }
    else if ( n.role == r.choice )
    {
	// A menu: the node's children ARE the options, numbered in line
	// mode — the same tree a selection-capable renderer (TUI) presents
	// as a movable choice. Each option's own children (detail under an
	// option) still render generically.
	if ( !label.empty() )
	    out += label + "\n";
	for ( size_t i = 0; i < n.children.size(); ++i )
	{
	    const uinode &opt = n.children[i];
	    out += "  " + std::to_string(i + 1) + ". " + node_text(opt) + "\n";
	    for ( size_t k = 0; k < opt.children.size(); ++k )
		render_text_node(r, opt.children[k], width, out);
	}
	return;	// options consumed — no generic child recursion
    }
    else if ( n.role == r.list && !label.empty() )
    {
	out += label + ":\n";
    }
    // group / list / unknown roles: structure only — children carry it.

    for ( size_t i = 0; i < n.children.size(); ++i )
	render_text_node(r, n.children[i], width, out);
}

} // namespace detail

// Linearize a semantic tree to sequential text. The tree arrives already
// access-filtered (projection is the security boundary — a renderer never
// decides what may be seen).
inline std::string render_text(const roles &r, const uinode &tree,
			       size_t width = 78)
{
    std::string out;
    detail::render_text_node(r, tree, width, out);
    return out;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_RENDER_TEXT_H
