// madcmark.h — the madcmark MODULE's C interface: what `import madcmark;`
// (the <ns_markdown> fragment's) tokenizes and binds (the module row in
// src/madc_modules.cpp is LAZY). GitHub-flavoured Markdown parsed by
// cmark-gfm, a dependency linked statically into the module
// (src/modules/madcmark/madcmark.cpp is its one caller).
//
// `result` is a madc::value* (the dialect passes &out); text arguments are C
// strings.
//   madcmark_parse(out, text) -> the document node: {kind, line, col,
//                                end_line, end_col, text, children, ...}
//                                (include/madcdis/markdown.h states the shape;
//                                kinds are markdown::node_kind codes,
//                                <bits/markdown_enums>)
// Thread contract: a pure function of its text.
void   *madcmark_parse(void *result, const char *text);
