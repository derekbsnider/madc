#ifndef __MADCDIS_MARKDOWN_H
#define __MADCDIS_MARKDOWN_H 1

// madcdis/markdown.h — the ONE Markdown parser in madc (plan
// docs/plans/2026-10-03-chthonia-windows-macos.md §7d): cmark-gfm, the
// CommonMark reference implementation with GitHub's extensions (tables,
// task lists, strikethrough, autolinks, footnotes). Its only caller is the
// madcmark MODULE (src/modules/madcmark/madcmark.cpp, built by
// src/madcmark.mk, never into libmadc); <cmark-gfm.h> is included there only.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): markdown_tree is
// a pure function of its text — each call owns its parser; the extension
// registry is filled once per process behind a static guard.

#include "libmadc/value.h"
#include "madc/bits/markdown_enums"	// markdown::node_kind — a node's one enum text

#include <string>

namespace madc {

// `text` parsed as GitHub-flavoured Markdown into a value tree, the document
// node at its root. Every node:
//   {kind: markdown::node_kind code, line, col, end_line, end_col (1-based source
//    positions; 0 where cmark-gfm reports none, a soft break's), text (the
//    literal of text, code, code_block, html_block, html_inline: decoded,
//    entities and escapes resolved), children: [...]}
// and by kind:
//   heading:    level (1-6)
//   list:       list (markdown::list_kind code), start (an ordered list's
//               first number), tight (bool)
//   item:       task (bool: a task list item), checked (bool)
//   code_block: info (the fence's info string, "" for an indented block),
//               fenced (bool)
//   link/image: url, title
//   table:      align: [markdown::column_align code per column]
//   table_row:  header (bool)
value markdown_tree(const std::string &text);

} // namespace madc

#endif // __MADCDIS_MARKDOWN_H
