//////////////////////////////////////////////////////////////////////////
//									//
// madc:: namespace — script-level runtime eval + expression eval.	//
//									//
// The namespace madc functions below are the single real		//
// implementation and the symbols scripts bind mangled-direct		//
// (cpp-first-api.md): the embedded <ns_madc> header declares them	//
// declaration-only and the parser resolves the Itanium symbols		//
// straight to these definitions via -rdynamic. The extern "C"		//
// exports at the bottom are the C-linkage API for C hosts consuming	//
// libmadc.a/.so — never the script-side resolution path.		//
//									//
// Both layers delegate to the madc_runtime_eval_* internals in		//
// src/parser.cpp (declared in ns_common.h), which drive the active	//
// Program's runtime_eval_source / runtime_eval_expression machinery	//
// and honour the engine's madc_runtime_eval_policy.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "ns_common.h"
#include "libmadc/sysinfo.h"
#include "madc_lang.h"		// doc_parse + doc_*: the language services over a document
#include "madc_theme.h"		// theme_styles + theme_paint: the CLI's reader and painter
#include "madc_diag_names.h"	// doc_diagnostics: the severity / phase names
#include "madc_posix_io.h"	// get_host_name (host-facts seam)
#include "madc_config_file.h"	// cfg::user_config_dir: the user configuration directory
#include "madc_project.h"	// the project manifest's one reader and writer
#include "madc_documents.h"	// document_read_file: a manifest's own JSON
#include "madcdis/world_text.h"	// hub::detail::wt_json_to_value: JSON -> value
#if defined(_WIN32)
#include <process.h>		// _getpid — sys.pid's UCRT spelling (never <windows.h>
				// here: its `interface` macro collides with the module map)
#else
#include <unistd.h>		// getpid — sys.pid's POSIX spelling
#endif
#include "madc_modules.h"	// module_available: the module map + the one dl seam
#include "madc_dl.h"		// library_open: the one dl seam
#include "madc_run_child.h"	// serve_run_child: a Run child of self
#include "madcdis/process.h"	// Process::run_and_wait: compiler_run
#include "madc_regex.h"		// regex_*: madc's one regex engine (PCRE2)

// ---- madc::sys — the system object (task #91) ----------------------------
namespace madc {

static const char *sys_detect_platform()
{
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char *sys_detect_hostname()
{
    static std::string name = madc::detail::get_host_name();
    return name.c_str();
}

// The process id as a portable fact (V6 seam): the dialect's bare getpid()
// resolved through the dlsym fallback on POSIX only — the UCRT exports
// _getpid, so on the win64 madc every program naming getpid failed to
// compile (the madcide family under wine). sys.pid says it on every platform.
static int64_t sys_detect_pid()
{
#if defined(_WIN32)
    return (int64_t)_getpid();
#else
    return (int64_t)getpid();
#endif
}

// The facts initialize once at load (dynamic init of this TU); argv/path
// are filled by sys_populate_args from the injected __madc_sys_init call.
SysInfo sys = { value(), value(), sys_detect_platform(), MADC_VERSION_STR,
		sys_detect_hostname(), sys_detect_pid() };

void sys_populate_args(int argc, char **argv)
{
    sys.argv = value::make_array();
    for ( int i = 0; i < argc && argv; i++ )
	sys.argv.array().push_back(value(argv[i] ? argv[i] : ""));
    // Initial search-path seed (future #load/eval honor mutations — the
    // Python one-way semantics): the script's directory, then cwd.
    sys.path = value::make_array();
    if ( argc > 0 && argv && argv[0] )
    {
	std::string dir = madc::detail::host_path_dirname(argv[0]);
	if ( dir.size() > 1 )	// ">1": a bare root separator stays off the path
	    sys.path.array().push_back(value(dir.substr(0, dir.size() - 1)));
    }
    sys.path.array().push_back(value("."));
}

} // namespace madc

namespace madc {

// Full-program eval: the source must define (or be wrapped into)
// `__madc_eval()`; the rendered result text lands in `out`.
std::string &eval_unit(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval(&out, &source); }
bool eval_bool(std::string &source)
	{ return madc_runtime_eval_bool(&source); }
int64_t eval_int(std::string &source)
	{ return madc_runtime_eval_int(&source); }
int64_t eval_int(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int(&s); }
double eval_double(std::string &source)
	{ return madc_runtime_eval_double(&source); }
std::string &eval_string(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval_string(&out, &source); }

// Value-first primaries (slice V2): const char* sources, value
// destinations. The value-destination forms carry the rendered result
// text as a string-kind value — the same convention as the channel
// value carriers (typed-kind eval results are a future runtime-core
// entry, not a header change).
value &eval_unit(value &out, const char *source)
	{ std::string s = source ? source : "", r; madc_runtime_eval(&r, &s); out = value(r); return out; }
bool eval_bool(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_bool(&s); }
double eval_double(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_double(&s); }
value &eval_string(value &out, const char *source)
	{ std::string s = source ? source : "", r; madc_runtime_eval_string(&r, &s); out = value(r); return out; }

// Expression eval: a single expression, no function calls unless the
// engine's expression policy allows them.
bool eval_expression_bool(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool(&e); }
int64_t eval_expression_int(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int(&e); }
double eval_expression_double(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double(&e); }
std::string &eval_expression_string(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string(&out, &e); }
value &eval_expression_string(value &out, const char *expr)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_string(&r, &e); out = value(r); return out; }

// Typed out-parameter forms (overloaded on the destination). The
// string-destination form RENDERS any result type ("42", "4.000000",
// "echo") via the untyped runtime; eval_expression_string above is the
// strict string-typed coercion.
void eval_expression(int64_t &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int(&e); }
void eval_expression(double &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double(&e); }
void eval_expression(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; madc_runtime_eval_expression(&out, &e); }
void eval_expression(value &out, const char *expr)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression(&r, &e); out = value(r); }

// Context-carrying expression forms: `ctx` IS a madc::value (the unified
// script array, package A0) of nested key/value entries built with the
// context_set_* helpers below.
std::string &eval_expression_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_ctx(&out, &e, &ctx); }
bool eval_expression_bool_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool_ctx(&e, &ctx); }
int64_t eval_expression_int_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int_ctx(&e, &ctx); }
double eval_expression_double_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double_ctx(&e, &ctx); }
std::string &eval_expression_string_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string_ctx(&out, &e, &ctx); }
value &eval_expression_string_ctx(value &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_string_ctx(&r, &e, &ctx); out = value(r); return out; }

// Typed out-parameter ctx forms — every eval_expression overload needs a
// _ctx sibling or the call-site scope-capture rebind has no target (the
// std::string& destination is covered by the render form above, which
// the rebind's overload re-rank already selects).
void eval_expression_ctx(int64_t &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int_ctx(&e, &ctx); }
void eval_expression_ctx(double &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double_ctx(&e, &ctx); }
void eval_expression_ctx(value &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_ctx(&r, &e, &ctx); out = value(r); }

// Context-carrying full-eval forms — the rebind targets for call-site
// scope capture (the parser rebinds a madc::eval_* call to its _ctx
// sibling and appends the captured scope when scope access is enabled).
std::string &eval_unit_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_ctx(&out, &source, &ctx); }
bool eval_bool_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_bool_ctx(&source, &ctx); }
int64_t eval_int_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_int_ctx(&source, &ctx); }
int64_t eval_int_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int_ctx(&s, &ctx); }
double eval_double_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_double_ctx(&source, &ctx); }
std::string &eval_string_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_string_ctx(&out, &source, &ctx); }

// Value-first _ctx siblings — every value-first eval overload above needs
// one, or the call-site scope-capture rebind has no target.
value &eval_unit_ctx(value &out, const char *source, value &ctx)
	{ std::string s = source ? source : "", r; madc_runtime_eval_ctx(&r, &s, &ctx); out = value(r); return out; }
bool eval_bool_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_bool_ctx(&s, &ctx); }
double eval_double_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_double_ctx(&s, &ctx); }
value &eval_string_ctx(value &out, const char *source, value &ctx)
	{ std::string s = source ? source : "", r; madc_runtime_eval_string_ctx(&r, &s, &ctx); out = value(r); return out; }

// Compiler data as data (madcide IDE-3): compile-NEVER-execute a source
// buffer in a child Program and return the compiler's own structured
// data as a value array — the meta-level surface an IDE's diagnostics
// pane and outline project. The <ns_madc> declarations carry the row
// shapes and the thread contract.
value &diagnostics(value &out, const char *source)
	{ std::string s = source ? source : "", f; madc_source_diagnostics(&out, &s, &f); return out; }
value &diagnostics(value &out, const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; madc_source_diagnostics(&out, &s, &f); return out; }
value &outline(value &out, const char *source)
	{ std::string s = source ? source : "", f; madc_source_outline(&out, &s, &f); return out; }
value &outline(value &out, const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; madc_source_outline(&out, &s, &f); return out; }
// The render query (the view seam's code views): parse the buffer in the
// same child and render its cir_node tree as `target` (the --emit=
// vocabulary). The <ns_madc> declaration carries the contract.
bool emit(value &out, const char *source, const char *filename,
	  const char *target)
	{ std::string s = source ? source : "", f = filename ? filename : "", t = target ? target : ""; return madc_source_emit(&out, &s, &f, &t); }
// The same render by the target's KIND (madc::file_kind, <bits/file_kinds>)
// — an IDE carries the enumerator, never the spelling. The converters
// file_kind_name / file_kind_of / file_kind_of_path the fragment declares
// beside this live in src/file_kinds.cpp (the vocabulary's one text owner).
bool emit(value &out, const char *source, const char *filename,
	  int64_t target_kind)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_source_emit_kind(&out, &s, &f, target_kind); }
// The same KIND render PLUS the source↔display COORDINATE MAP beside the text
// (V5): out_map arrives as an array of {disp, stored, len} rows (empty =
// nothing corresponds — park at 0), the input to ui::lens_to_display /
// ui::lens_to_stored. An IDE stores it on the code View. Statement/line-level
// today; column/expression precision is the named later refinement.
bool emit(value &out, value &out_map, const char *source, const char *filename,
	  int64_t target_kind)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_source_emit_kind_map(&out, &out_map, &s, &f, target_kind); }

// Persistent parse handles (madcide AST-1): the compiler-data surfaces
// above given a LIFETIME — open a live parse per TU, refresh it whole
// on save/check, query outline / check / enclosing from the RETAINED
// state without re-running the front end. The <ns_madc> declarations
// carry the row shapes and the thread contract.
int64_t parse_open(const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_parse_open(&s, &f, 0); }
// The same under `standard` (a <bits/file_kinds> standard code; 0 = the default).
int64_t parse_open(const char *source, const char *filename, int64_t standard)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_parse_open(&s, &f, standard); }
int64_t parse_open_file(const char *path)
	{ std::string p = path ? path : ""; return madc_parse_open_file(&p); }
bool parse_refresh(int64_t handle, const char *source)
	{ std::string s = source ? source : ""; return madc_parse_refresh(handle, &s); }
bool parse_close(int64_t handle)
	{ return madc_parse_close(handle); }
value &parse_outline(value &out, int64_t handle)
	{ madc_parse_outline(&out, handle); return out; }
value &parse_check(value &out, int64_t handle)
	{ madc_parse_diagnostics(&out, handle); return out; }
value &parse_enclosing(value &out, int64_t handle, int64_t line,
		       int64_t column)
	{ madc_parse_enclosing(&out, handle, line, column); return out; }
value &parse_spans(value &out, int64_t handle)
	{ madc_parse_spans(&out, handle); return out; }

// A document's parse handle (split arc 1A.1, design §2.5): the ONE door is
// madc::lang::document_handle (split arc 1C.11) — the document's version
// against the version its slot was parsed from. These are its dialect face.
static int64_t doc_parse_impl(value *diags, int64_t w, int64_t entity)
{
    madc::hub::world *world = madc_ui_world(w);
    return world ? lang::document_handle(*world, (madc::hub::entity_id)entity,
					 diags)
		 : 0;
}
int64_t doc_parse(int64_t w, int64_t entity)
	{ return doc_parse_impl(NULL, w, entity); }
int64_t doc_parse_checked(value &diags, int64_t w, int64_t entity)
	{ diags = value(); return doc_parse_impl(&diags, w, entity); }
bool doc_set_standard(int64_t w, int64_t entity, int64_t standard)
{
    madc::hub::world *world = madc_ui_world(w);
    return world && lang::document_set_standard(*world, (madc::hub::entity_id)entity,
						 standard);
}
int64_t doc_parsed(int64_t w, int64_t entity)
{
    madc::hub::world *world = madc_ui_world(w);
    return world ? lang::document_parsed(*world, (madc::hub::entity_id)entity) : 0;
}

// The language services' dialect face (split arc 1C.11): madc::lang's answers
// as rows. A range is { line, column, end_line, end_column }; an enum is its
// code with its name beside it. The <ns_madc> declaration carries the shapes.
static value lang_range_value(const lang::range &r)
{
    std::map<std::string, value> f;
    f["line"] = value(r.from.line);
    f["column"] = value(r.from.column);
    f["end_line"] = value(r.to.line);
    f["end_column"] = value(r.to.column);
    return value::make_object(f);
}

static value lang_symbol_value(const lang::symbol &s)
{
    std::map<std::string, value> f;
    f["name"] = value(s.name);
    f["kind"] = value(std::string(lang::symbol_kind_name(s.kind)));
    f["kind_code"] = value((int64_t)s.kind);
    f["range"] = lang_range_value(s.extent);
    f["selection"] = lang_range_value(s.selection);
    return value::make_object(f);
}

static value lang_ranges_value(const std::vector<lang::range> &rs)
{
    std::vector<value> rows;
    for ( size_t i = 0; i < rs.size(); ++i )
	rows.push_back(lang_range_value(rs[i]));
    return value::make_array(rows);
}

static lang::point lang_point(int64_t line, int64_t column)
{
    lang::point p;
    p.line = line;
    p.column = column;
    return p;
}

value &doc_diagnostics(value &out, int64_t w, int64_t entity)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    std::vector<lang::diagnostic> ds;
    if ( !world || !lang::diagnostics(*world, (madc::hub::entity_id)entity, ds) )
	return out;
    std::vector<value> rows;
    for ( size_t i = 0; i < ds.size(); ++i )
    {
	std::map<std::string, value> f;
	f["range"] = lang_range_value(ds[i].at);
	f["severity"] = value(std::string(diag_severity_name(ds[i].severity)));
	f["severity_code"] = value((int64_t)ds[i].severity);
	f["phase"] = value(std::string(diag_phase_name(ds[i].phase)));
	f["phase_code"] = value((int64_t)ds[i].phase);
	f["message"] = value(ds[i].message);
	rows.push_back(value::make_object(f));
    }
    out = value::make_array(rows);
    return out;
}

value &doc_outline(value &out, int64_t w, int64_t entity)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    std::vector<lang::symbol> syms;
    if ( !world || !lang::outline(*world, (madc::hub::entity_id)entity, syms) )
	return out;
    std::vector<value> rows;
    for ( size_t i = 0; i < syms.size(); ++i )
	rows.push_back(lang_symbol_value(syms[i]));
    out = value::make_array(rows);
    return out;
}

value &doc_enclosing(value &out, int64_t w, int64_t entity,
		     int64_t line, int64_t column)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    lang::symbol s;
    if ( world && lang::enclosing(*world, (madc::hub::entity_id)entity,
				  lang_point(line, column), s) )
	out = lang_symbol_value(s);
    return out;
}

value &doc_definition(value &out, int64_t w, int64_t entity,
		      int64_t line, int64_t column)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    std::vector<lang::range> rs;
    if ( world && lang::definition(*world, (madc::hub::entity_id)entity,
				   lang_point(line, column), rs) )
	out = lang_ranges_value(rs);
    return out;
}

value &doc_references(value &out, int64_t w, int64_t entity,
		      int64_t line, int64_t column, bool include_declaration)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    std::vector<lang::range> rs;
    if ( world && lang::references(*world, (madc::hub::entity_id)entity,
				   lang_point(line, column),
				   include_declaration, rs) )
	out = lang_ranges_value(rs);
    return out;
}

value &doc_hover(value &out, int64_t w, int64_t entity,
		 int64_t line, int64_t column)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    lang::hover_info h;
    if ( !world || !lang::hover(*world, (madc::hub::entity_id)entity,
				lang_point(line, column), h) )
	return out;
    std::map<std::string, value> f;
    f["text"] = value(h.text);
    if ( h.has_range )
	f["range"] = lang_range_value(h.at);
    out = value::make_object(f);
    return out;
}

value &doc_spans(value &out, int64_t w, int64_t entity)
{
    out = value();
    madc::hub::world *world = madc_ui_world(w);
    std::vector<highlight_span> sp;
    if ( !world || !lang::spans(*world, (madc::hub::entity_id)entity, sp) )
	return out;
    std::vector<value> rows;
    for ( size_t i = 0; i < sp.size(); ++i )
    {
	std::map<std::string, value> f;
	f["line"] = value((int64_t)sp[i].line);
	f["column"] = value((int64_t)sp[i].column);
	f["length"] = value((int64_t)sp[i].length);
	f["class"] = value(std::string(highlight_class_name(sp[i].cls)));
	f["class_code"] = value((int64_t)sp[i].cls);
	rows.push_back(value::make_object(f));
    }
    out = value::make_array(rows);
    return out;
}

// madc's colour theme as a host reads it (<ns_madc_host>): the CLI's own
// reader and painter (madc_theme.h). The theme `text` spells; "" = the
// built-in default. False: the reader refused it (`why` the line).
static bool host_theme(const char *text, theme &out, std::string &why)
{
    if ( !text || !*text )
    {
	out = theme_default();
	return true;
    }
    return theme_read(text, out, why);
}

bool theme_styles(value &out, value &why, const char *theme_text)
{
    out = value();
    why = value();
    theme t;
    std::string err;
    if ( !host_theme(theme_text, t, err) )
    {
	why = value(err);
	return false;
    }
    std::map<std::string, value> syntax, chrome;
    for ( size_t i = 0; i < highlight_class_count; ++i )
	if ( !t.syntax[i].is_normal() )
	    syntax[highlight_class_name((HighlightClass)i)] = value(hub::ui_style_spec(t.syntax[i]));
    for ( size_t i = 0; i < (size_t)hub::tui_chrome::count; ++i )
	if ( t.chrome_named[i] )
	    chrome[hub::tui_chrome_name((hub::tui_chrome)i)] = value(hub::ui_style_spec(t.chrome[i]));
    std::map<std::string, value> f;
    f["syntax"] = value::make_object(syntax);
    f["chrome"] = value::make_object(chrome);
    out = value::make_object(f);
    return true;
}

value &theme_paint(value &out, const char *theme_text, const char *text,
		   value &code, int64_t standard)
{
    theme t;
    std::string err;
    if ( !host_theme(theme_text, t, err) )
	t = theme_default();
    std::vector<code_range> ranges;
    ns_common::code_ranges_of(code, ranges);
    out = value(theme_paint(t, std::string(text ? text : ""), ranges, standard,
			    hub::ui_colour_depth::truecolor));
    return out;
}

// Code-graph MCP L1 (design 2026-09-12): the live declaration/type graph
// as node-addressed reads over a parse handle. The <ns_madc> declaration
// carries the node/result shapes.
value &graph_symbols(value &out, int64_t handle)
	{ madc_graph_symbols(&out, handle); return out; }
value &graph_node(value &out, int64_t handle, int64_t node_id)
	{ madc_graph_node(&out, handle, node_id); return out; }
value &graph_type_of(value &out, int64_t handle, int64_t node_id)
	{ madc_graph_type_of(&out, handle, node_id); return out; }
value &graph_definition(value &out, int64_t handle, const char *name)
	{ std::string n = name ? name : ""; madc_graph_definition(&out, handle, &n); return out; }
value &graph_members(value &out, int64_t handle, int64_t type_id)
	{ madc_graph_members(&out, handle, type_id); return out; }
value &graph_bases(value &out, int64_t handle, int64_t type_id)
	{ madc_graph_bases(&out, handle, type_id); return out; }
value &graph_enclosing(value &out, int64_t handle, int64_t line, int64_t column)
	{ madc_graph_enclosing(&out, handle, line, column); return out; }

// Code-graph MCP L1b (design 2026-09-12): the body graph over the live
// parse-handle TokenBase AST. graph_body(func) is the function's own
// statement tree; graph_children(id) descends from any node (a body handle
// or a function type-id). depth < 0 = unbounded (server-side capped).
value &graph_body(value &out, int64_t handle, int64_t func_id, int64_t depth)
	{ madc_graph_body(&out, handle, func_id, depth); return out; }
value &graph_children(value &out, int64_t handle, int64_t id, int64_t depth)
	{ madc_graph_children(&out, handle, id, depth); return out; }

// Code-graph MCP L2 (design 2026-09-12): derived-edge verbs over the live
// parse-handle forest. func_id is a FUNCTION type-id; def_id/id may be a
// function type-id OR a global decl-id (from graph_search). Each returns
// { nodes, edges?, truncated? }.
value &graph_callees(value &out, int64_t handle, int64_t func_id)
	{ madc_graph_callees(&out, handle, func_id); return out; }
value &graph_callers(value &out, int64_t handle, int64_t func_id)
	{ madc_graph_callers(&out, handle, func_id); return out; }
value &graph_references(value &out, int64_t handle, int64_t def_id)
	{ madc_graph_references(&out, handle, def_id); return out; }
value &graph_search(value &out, int64_t handle, const char *kind, const char *name_sub)
	{ std::string k = kind ? kind : ""; std::string n = name_sub ? name_sub : "";
	  madc_graph_search(&out, handle, &k, &n); return out; }
value &graph_impact(value &out, int64_t handle, int64_t id)
	{ madc_graph_impact(&out, handle, id); return out; }

// Code-graph MCP L3 (design 2026-09-12): a node's exact source extent, the node
// starting at a position, and the validated whole-TU refresh (true = the
// candidate replaced the live tree; false = rejected, nothing changed;
// out_diags = the candidate's diagnostics either way).
value &graph_span(value &out, int64_t handle, int64_t id)
	{ madc_graph_span(&out, handle, id); return out; }
value &graph_at(value &out, int64_t handle, int64_t line, int64_t column)
	{ madc_graph_at(&out, handle, line, column); return out; }
bool parse_refresh_checked(value &out_diags, int64_t handle, const char *source)
	{ std::string s = source ? source : "";
	  return madc_parse_refresh_checked(&out_diags, handle, &s); }
bool parse_would_accept(value &out_diags, int64_t handle, const char *source)
	{ std::string s = source ? source : "";
	  return madc_parse_would_accept(&out_diags, handle, &s); }

// (The git substrate's dialect face lived here as madc::git_* until the V6
// seam; it is the git:: namespace of the madcgit MODULE now — <ns_git>.)

// The comparison spelling of a path (V6c-2): the standing canonicalizer
// (detail::canonical_path_for_compare — realpath when it resolves, the input
// spelling otherwise) given a dialect face. Two spellings of one file must
// compare EQUAL: a protocol client addresses a document by absolute URI while
// a session may have opened it by a relative path, and without this the two
// are different documents. Never answers empty — a caller compares the
// result, and an empty answer would make every unresolvable path equal to
// every other.
value &canonical_path(value &out, const char *path)
	{ out = detail::canonical_path_for_compare(path ? path : ""); return out; }

// A path's extension and stem, and its missing parent directories: the host
// path owners (madc_posix_io.h) given dialect faces, so a host names an
// artifact or makes a directory by madc's rule, never a copy of it.
value &path_drop_extension(value &out, const char *path)
	{ out = detail::host_path_drop_extension(path ? path : ""); return out; }
value &path_stem(value &out, const char *path)
	{ out = detail::host_path_stem(path ? path : ""); return out; }
value &path_generic(value &out, const char *path)
	{ out = detail::host_path_generic(path ? path : ""); return out; }
void path_make_parents(const char *path)
	{ detail::host_path_make_parents(path ? path : ""); }

// The user configuration directory of `app` (madc_config_file.h's one rule):
// the dialect face of what madc.ini's lookup reads, so a host keeps its
// settings where madc keeps its own.
value &user_config_dir(value &out, const char *app)
	{ out = cfg::user_config_dir(app ? app : ""); return out; }
value &user_config_dir_of(value &out, const char *app, const char *xdg,
			  const char *appdata, const char *home, bool windows)
	{ out = cfg::user_config_dir_of(app ? app : "", xdg, appdata, home, windows); return out; }

// L4b (design §3.3): revision handles by generation TAG.
int64_t parse_open_tagged(const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : "";
	  return madc_parse_open_tagged(&s, &f); }
int64_t parse_generation(int64_t handle) { return madc_parse_generation(handle); }
int64_t graph_route(int64_t handle, int64_t id) { return madc_graph_route(handle, id); }

// The live-tree build/run pair (OWNER RULING 2026-08-27 — the running
// madc IS the compiler; ^B never re-parses, never execs a madc):
// parse_build emits a native artifact from the handle's EXISTING parsed
// tree; parse_run runs that tree in a fork() child. The <ns_madc>
// declarations carry the contracts.
bool parse_build(value &out_diags, int64_t handle, const char *kind,
		 const char *outpath)
	{
	    std::string k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_parse_build(&out_diags, handle, &k, &o);
	}
int64_t parse_run(int64_t handle)
	{ return madc_parse_run(handle); }

// The build surface (madcide IDE-10c): the CLI's AOT lane run IN-PROCESS —
// the IDE lives inside the compiler and never shells out to a PATH madc.
// The <ns_madc> declaration carries the contract (kinds, row shape).
bool build_native(value &out_diags, const char *path, const char *kind,
		  const char *outpath)
	{
	    std::string p = path ? path : "", k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_build_native(&out_diags, &p, &k, &o, NULL);
	}
// The same build with the CLI's -I directories (an array of text, in
// order): a TU that includes a header outside its own directory (a
// madcide plugin's <madcide/plugin>).
bool build_native(value &out_diags, const char *path, const char *kind,
		  const char *outpath, const value &include_dirs)
	{
	    std::string p = path ? path : "", k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_build_native(&out_diags, &p, &k, &o, &include_dirs);
	}

// The running compiler's own resolved executable path (madcide IDE-10c:
// "run this program" spawns a child OF SELF — never a PATH madc). Stable
// for the process lifetime (C++11 static init; immutable after — safe
// for concurrent readers); empty string = the platform probe failed.
const char *compiler_path()
	{
	    static const std::string p = madc_self_exe_path();
	    return p.c_str();
	}
// The running compiler run on `args` with ALL stdio inherited, waited for
// (split arc 1D: madcide --line is `madc -i FILE`) — the spawn owner's
// run-and-wait arm, argv[0] the compiler's own path.
int64_t compiler_run(value &args)
	{
	    std::vector<std::string> argv(1, std::string(compiler_path()));
	    if ( argv[0].empty() )
		return -1;
	    if ( args.is_array() )
		for ( const value &a : args.as_array() )
		    argv.push_back(a.is_string() ? a.as_string() : std::string());
	    return madc::Process::run_and_wait(argv[0], argv);
	}
// A Run child of self (madc_run_child.h): a program's injected entry init
// (__madc_sys_init) serves it, so a program on the engine serves the run its
// own parent requested before its main runs; a host may call it too.
int64_t serve_run_child()
	{
	    return madc_serve_run_child();
	}
value &lex_spans(value &out, const char *text, const char *filename)
	{
	    std::string src = text ? text : "";
	    std::string disp = filename ? filename : "";
	    madc_lex_spans(&out, &src, &disp);
	    return out;
	}
int64_t project_open(const char *manifest)
	{ std::string m = manifest ? manifest : ""; return madc_project_open(&m); }
value &project_tus(value &out, int64_t handle)
	{ madc_project_tus(&out, handle); return out; }
bool project_close(int64_t handle)
	{ return madc_project_close(handle); }
// The project build/run pair (madcide ^B correlation, owner design doc
// 2026-08-31: with a manifest open, Build = the --project build). The
// <ns_madc> declarations carry the contracts.
bool project_build(value &out_diags, const char *manifest, const char *kind,
		   const char *outpath)
	{
	    std::string m = manifest ? manifest : "", k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_project_build(&out_diags, &m, &k, &o);
	}
int64_t project_run(const char *manifest)
	{ std::string m = manifest ? manifest : ""; return madc_project_run(&m); }

// A project manifest — madc's one reader and writer (madc_project.h) given
// dialect faces, so a host reads and edits <base>.prj.json by madc's rules
// (a TU resolved against the manifest's directory, a kind word refused when
// unknown, a path named relative to the manifest), never its own copy.
static value text_list_value(const std::vector<std::string> &items)
{
	std::vector<value> rows;
	for ( const std::string &s : items )
		rows.push_back(value(s));
	return value::make_array(rows);
}

static value project_tu_value(const ProjectTU &tu)
{
	std::map<std::string, value> f;
	f["file"] = value(tu.file);
	f["directory"] = value(tu.working_dir);
	f["defines"] = text_list_value(tu.defines);
	f["include_dirs"] = text_list_value(tu.include_dirs);
	f["std"] = value(tu.std_option);
	f["stdlib"] = value(tu.stdlib_option);
	return value::make_object(f);
}

static value project_manifest_value(const ProjectManifest &m)
{
	std::map<std::string, value> f;
	std::vector<value> tus;
	for ( const ProjectTU &tu : m.tus )
		tus.push_back(project_tu_value(tu));
	f["tus"] = value::make_array(tus);
	f["entry"] = value(m.entry);
	f["output"] = value(m.output_name);
	f["kind"] = value((int64_t)m.kind);
	f["icon"] = value(m.icon);
	f["libs"] = text_list_value(m.libs);
	f["madc"] = value(m.madc_min);
	return value::make_object(f);
}

// The verdict of a manifest verb: true, or false with its reason in out_err.
static bool manifest_verdict(bool ok, const std::string &err, value &out_err)
{
	out_err = value(ok ? std::string() : err);
	return ok;
}

bool project_manifest_read(value &out, value &out_err, const char *path)
{
	const std::string p = path ? path : "";
	ProjectManifest m;
	std::string err;
	out = value();
	if ( !manifest_verdict(read_project_manifest(p, m, err), err, out_err) )
		return false;
	out = project_manifest_value(m);
	// The file's own JSON as written: the keys madc does not model ride
	// here, parsed as the reader parses (comments skipped).
	std::string text;
	value doc;
	if ( document_read_file(p, text) )
	{
		nlohmann::json j = nlohmann::json::parse(text, nullptr,
			/*allow_exceptions=*/false, /*ignore_comments=*/true);
		if ( !j.is_discarded() )
			doc = hub::detail::wt_json_to_value(j);
	}
	out.object()["json"] = doc;
	return true;
}
bool project_manifest_create(value &out_err, const char *path)
{
	std::string err;
	return manifest_verdict(::project_manifest_create(path ? path : "", err),
				err, out_err);
}
bool project_manifest_add_tu(value &out_err, const char *path, const char *file)
{
	std::string err;
	return manifest_verdict(::project_manifest_add_tu(path ? path : "",
							  file ? file : "", err),
				err, out_err);
}
bool project_manifest_remove_tu(value &out_err, const char *path, const char *file)
{
	std::string err;
	return manifest_verdict(::project_manifest_remove_tu(path ? path : "",
							     file ? file : "", err),
				err, out_err);
}
bool project_manifest_set_kind(value &out_err, const char *path, project_kind kind)
{
	std::string err;
	return manifest_verdict(::project_manifest_set_kind(path ? path : "", kind, err),
				err, out_err);
}
bool project_default_output(value &out, value &out_err, const char *path)
{
	const std::string p = path ? path : "";
	ProjectManifest m;
	std::string err;
	out = value("");
	if ( !manifest_verdict(read_project_manifest(p, m, err), err, out_err) )
		return false;
	out = value(::project_default_output(p, m));
	return true;
}
value &project_events_path(value &out, const char *path)
	{ out = value(::project_events_path(path ? path : "")); return out; }
const char *project_kind_name(project_kind kind)
	{ return ::project_kind_name(kind); }

// Context builders. Kind-safe via value_object_for_write: a null ctx
// vivifies to kind::object; any other non-object kind degrades to a
// diagnosed no-op instead of throwing across the JIT boundary.
void context_set_int(value &ctx, std::string &key, int64_t v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_int")[key] = value(int64_t(v)); }
void context_set_real(value &ctx, std::string &key, double v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_real")[key] = value(v); }
void context_set_string(value &ctx, std::string &key, const char *v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_string")[key] = value(std::string(v ? v : "")); }
void context_set_array(value &ctx, std::string &key, value &v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_array")[key] = v; }

// Value-first context builders: const char* keys, so building a context
// never requires <string> in the script.
void context_set_int(value &ctx, const char *key, int64_t v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_int")[k] = value(int64_t(v)); }
void context_set_real(value &ctx, const char *key, double v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_real")[k] = value(v); }
void context_set_string(value &ctx, const char *key, const char *v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_string")[k] = value(std::string(v ? v : "")); }
void context_set_array(value &ctx, const char *key, value &v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_array")[k] = v; }

// madc::getline — line input, value-first (the <ns_madc> declaration
// carries the contract). std::getline(cin, string&) is the oracle:
// failbit means NOTHING was extracted (clean EOF -> false); a final
// unterminated line and an empty line both deliver (true). The value
// takes the line through its own assignment, so freeze rejection stays
// with the one owner.
bool getline(value &out)
{
	std::string line;
	bool got = (bool)std::getline(std::cin, line);
	out = value(line);
	return got;
}

} // namespace madc

// ---- C-linkage API for C hosts (libmadc.a/.so) --------------------------

extern "C" {

void *__madc_eval_runtime(void *result, void *source)
{
    return madc_runtime_eval(result, source);
}

bool __madc_eval_bool_runtime(void *source)
{
    return madc_runtime_eval_bool(source);
}

int64_t __madc_eval_int_runtime(void *source)
{
    return madc_runtime_eval_int(source);
}

double __madc_eval_double_runtime(void *source)
{
    return madc_runtime_eval_double(source);
}

void *__madc_eval_string_runtime(void *result, void *source)
{
    return madc_runtime_eval_string(result, source);
}

void *__madc_eval_ctx_runtime(void *result, void *source, void *ctx)
{
    return madc_runtime_eval_ctx(result, source, ctx);
}

bool __madc_eval_bool_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_bool_ctx(source, ctx);
}

int64_t __madc_eval_int_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_int_ctx(source, ctx);
}

double __madc_eval_double_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_double_ctx(source, ctx);
}

void *__madc_eval_string_ctx_runtime(void *result, void *source, void *ctx)
{
    return madc_runtime_eval_string_ctx(result, source, ctx);
}

void *__madc_eval_expression_runtime(void *result, void *expr)
{
    return madc_runtime_eval_expression(result, expr);
}

bool __madc_eval_expression_bool_runtime(void *expr)
{
    return madc_runtime_eval_expression_bool(expr);
}

int64_t __madc_eval_expression_int_runtime(void *expr)
{
    return madc_runtime_eval_expression_int(expr);
}

double __madc_eval_expression_double_runtime(void *expr)
{
    return madc_runtime_eval_expression_double(expr);
}

void *__madc_eval_expression_string_runtime(void *result, void *expr)
{
    return madc_runtime_eval_expression_string(result, expr);
}

void *__madc_eval_expression_ctx_runtime(void *result, void *expr, void *ctx)
{
    return madc_runtime_eval_expression_ctx(result, expr, ctx);
}

bool __madc_eval_expression_bool_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_bool_ctx(expr, ctx);
}

int64_t __madc_eval_expression_int_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_int_ctx(expr, ctx);
}

double __madc_eval_expression_double_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_double_ctx(expr, ctx);
}

void *__madc_eval_expression_string_ctx_runtime(void *result, void *expr, void *ctx)
{
    return madc_runtime_eval_expression_string_ctx(result, expr, ctx);
}

void *__madc_context_set_int_runtime(void *ctx, void *key, int64_t value)
{
    madc::context_set_int(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_real_runtime(void *ctx, void *key, double value)
{
    madc::context_set_real(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_string_runtime(void *ctx, void *key, const char *value)
{
    madc::context_set_string(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_array_runtime(void *ctx, void *key, void *value)
{
    madc::context_set_array(*(madc::value *)ctx, *(std::string *)key,
			    *(madc::value *)value);
    return ctx;
}

}

namespace madc {
// Is a module's library available on this system? Opens the module map's
// spelling through the one dl seam (madc_module_open — beside the running
// binary first, then the loader's search) and keeps it open; false when it
// cannot be opened. The lazy-row contract's question (madc_modules.h).
bool module_available(const char *name)
{
    if ( !name || !*name )
	return false;
    std::string err;
    return madc_module_open(madc_module_library_spelling(name), err) != NULL;
}

// A shared library opened at run time by PATH (a madcide plugin's, plan
// §41.11a step 4), through the one dl seam: every reference binds at open,
// so a library whose names cannot all bind is refused here with the
// loader's reason, never at its first call; its names stay local, so two
// libraries' same-named functions never meet. 0 = refused, `why` the reason.
int64_t library_open(const char *path, value &why)
{
    why = value();
    if ( !path || !*path )
    {
	why = value("no library path");
	return 0;
    }
    void *h = madcdl_open_local(path, true);
    if ( !h )
    {
	const char *e = madcdl_error();
	why = value(e ? e : "the library cannot be opened");
    }
    return (int64_t)h;
}

// The address of `name` in an opened library; 0 = it defines no such name.
int64_t library_symbol(int64_t lib, const char *name)
{
    if ( !lib || !name )
	return 0;
    void *p = madcdl_sym((void *)lib, name);
    if ( !p )
	(void)madcdl_error();	// consume the pending failure: 0 says it
    return (int64_t)p;
}

// Close an opened library (a refused one; a library whose functions are
// still registered stays open).
void library_close(int64_t lib)
{
    if ( lib )
	madcdl_close((void *)lib);
}

// A source file compiled into THIS process and kept (a madcide `source`
// plugin, plan §41.11a step 5): the source twin of library_open. Its
// imports bind what the process already loaded (the one engine), and its
// dynamic initializers have run once. 0 = refused, `out_diags` the rows
// (build_native's shape, its include directories too).
int64_t code_open(value &out_diags, const char *path, const value &include_dirs)
{
	std::string p = path ? path : "";
	return madc_code_open(&out_diags, &p, &include_dirs);
}

// The address of `name` (a function or an object, by its emitted name: an
// extern "C" name is its spelling) in opened code; 0 = no such name.
int64_t code_symbol(int64_t code, const char *name)
{
	std::string n = name ? name : "";
	return madc_code_symbol(code, &n);
}

// Free opened code: none of its addresses may be called after.
void code_close(int64_t code)
{
	madc_code_close(code);
}

// The URI a madc::channel opens to fork a child running `entry`, a function
// of this program (the madcfork:// scheme, madc_program.cpp, registered
// here at its first spelling). Ring text, the c_str() contract.
void register_fork_channel_factory();	// madc_program.cpp
const char *fork_uri(int (*entry)())
{
    register_fork_channel_factory();
    std::string &slot = ns_common::ring_slot();
    slot = "madcfork://" + std::to_string((unsigned long long)(uintptr_t)entry);
    return slot.c_str();
}

// This target's shared-library suffix, the library spelling owner's
// (madc_target_dso_suffix): a script names its library file with it, never
// with a spelled suffix.
const char *library_suffix()
{
    return madc_target_dso_suffix();
}

// ---- regex_match / regex_search / regex_replace (split arc 1A.3; design
// docs/plans/2026-10-10-pcre2-design.md): madc's one regex engine
// (madc::regex, PCRE2). A pattern that does not compile — and a malformed
// replacement — is REFUSED: one line on stderr naming the function, and a
// neutral answer (false; the subject unchanged), never an answer that
// pretends the pattern matched nothing. The lean forms are the primary
// ones; the std::string forms serve a TU that included <string>.

namespace {

// The pattern compiled, or its refusal said for `who`; false when refused.
bool regex_compiled(const regex &re, const char *who)
{
    if ( re.ok() )
	return true;
    std::cerr << re.refusal(who) << std::endl;
    return false;
}

std::string text_of(const char *s)
{
    return s ? std::string(s) : std::string();
}

bool regex_matched(const std::string &s, const std::string &pattern)
{
    regex re(pattern);
    return regex_compiled(re, "madc::regex_match") && re.match(s);
}

bool regex_found(const std::string &s, const std::string &pattern)
{
    regex re(pattern);
    return regex_compiled(re, "madc::regex_search") && re.search(s);
}

// `s` with every match of `pattern` replaced (the subject when refused).
std::string regex_replaced(const std::string &s, const std::string &pattern,
			   const std::string &replacement)
{
    regex re(pattern);
    if ( !regex_compiled(re, "madc::regex_replace") )
	return s;
    std::string out, why;
    if ( !re.replace(s, replacement, out, &why) )
	std::cerr << "madc::regex_replace: invalid replacement \"" << replacement
		  << "\": " << why << std::endl;
    return out;
}

} // namespace

bool regex_match(const char *s, const char *pattern)
{
    return regex_matched(text_of(s), text_of(pattern));
}

bool regex_search(const char *s, const char *pattern)
{
    return regex_found(text_of(s), text_of(pattern));
}

value &regex_replace(value &out, const char *s, const char *pattern,
		     const char *replacement)
{
    out = value(regex_replaced(text_of(s), text_of(pattern), text_of(replacement)));
    return out;
}

bool regex_match(const std::string &s, const std::string &pattern)
{
    return regex_matched(s, pattern);
}

bool regex_search(const std::string &s, const std::string &pattern)
{
    return regex_found(s, pattern);
}

std::string &regex_replace(std::string &out, const std::string &s,
			   const std::string &pattern, const std::string &replacement)
{
    out = regex_replaced(s, pattern, replacement);
    return out;
}
} // namespace madc
