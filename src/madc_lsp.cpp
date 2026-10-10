//////////////////////////////////////////////////////////////////////////
//									//
// madc::lsp — the Language Server Protocol in madc's core (split arc	//
// 1C.12; include/madc_lsp.h, design docs/plans/2026-10-10-lsp-in-	//
// core-design.md). The lifecycle and the error codes are LSP 3.17's;	//
// every answer is madc::lang's. The protocol's text (a method name, a	//
// URI) converts ONCE at the boundary: a method to its enum, a URI to	//
// a path; positions convert through one owner pair (point_of /	//
// position_of).							//
//									//
//////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "ns_common.h"		// js_decodeURIComponent: the one percent-decoder
#include "madc_lsp.h"
#include "madc_lang.h"
#include "madc_documents.h"	// a document's file: open, find, new
#include "madc_posix_io.h"	// host_path_absolute: a URI's drive form
#include "madcdis/verbs.h"	// mutation_context: the client's edits
#include "madcdis/text_utf16.h"	// col16_to_byte / col16_of_byte
#include "libmadc/sysinfo.h"	// madc::sys.version: serverInfo
#include "madcdis/channel.h"	// serve: a Content-Length framed channel
#include "madcdis/world_text.h"	// the one value<->JSON bridge
#include "madc_session_client.h"	// backend_commands: madc -i's backend session

namespace madc {
namespace lsp {

namespace {

typedef std::map<std::string, value> fields;

// ---- the protocol's vocabulary, converted once ------------------------

enum class method : unsigned char
{
    initialize, initialized, shutdown, exit,
    did_open, did_change, did_save, did_close, will_save,
    did_change_configuration, did_change_watched_files,
    did_change_workspace_folders, cancel_request, set_trace,
    document_symbol, definition, references, hover, semantic_tokens_full,
    execute_command
};

struct method_row
{
    const char *name;
    method m;
    bool notification;
};

const method_row method_rows[] = {
    { "initialize", method::initialize, false },
    { "initialized", method::initialized, true },
    { "shutdown", method::shutdown, false },
    { "exit", method::exit, true },
    { "textDocument/didOpen", method::did_open, true },
    { "textDocument/didChange", method::did_change, true },
    { "textDocument/didSave", method::did_save, true },
    { "textDocument/didClose", method::did_close, true },
    { "textDocument/willSave", method::will_save, true },
    { "workspace/didChangeConfiguration", method::did_change_configuration, true },
    { "workspace/didChangeWatchedFiles", method::did_change_watched_files, true },
    { "workspace/didChangeWorkspaceFolders", method::did_change_workspace_folders, true },
    { "$/cancelRequest", method::cancel_request, true },
    { "$/setTrace", method::set_trace, true },
    { "textDocument/documentSymbol", method::document_symbol, false },
    { "textDocument/definition", method::definition, false },
    { "textDocument/references", method::references, false },
    { "textDocument/hover", method::hover, false },
    { "textDocument/semanticTokens/full", method::semantic_tokens_full, false },
    { "workspace/executeCommand", method::execute_command, false },
};

const method_row *method_of(const std::string &name)
{
    for ( size_t i = 0; i < sizeof(method_rows) / sizeof(method_rows[0]); ++i )
	if ( name == method_rows[i].name )
	    return &method_rows[i];
    return 0;
}

// JSON-RPC's and LSP's error codes.
enum class error_code : int64_t
{
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    server_not_initialized = -32002,
    request_failed = -32803
};

// The semantic-token legend, in its wire order.
enum class token_type : unsigned char
{
    keyword, variable, number, string, comment, type, function, none
};

const char *const token_type_names[] = {
    "keyword", "variable", "number", "string", "comment", "type", "function"
};

token_type token_of(HighlightClass c)
{
    switch ( c )
    {
    case HighlightClass::hcKeyword:
    case HighlightClass::hcControl:
    case HighlightClass::hcDirective:
	return token_type::keyword;
    case HighlightClass::hcIdent:
    case HighlightClass::hcVariable:
    case HighlightClass::hcParameter:
    case HighlightClass::hcMember:
    case HighlightClass::hcEnumerator:
	return token_type::variable;
    case HighlightClass::hcNumber:
	return token_type::number;
    case HighlightClass::hcString:
    case HighlightClass::hcIncludePath:
    case HighlightClass::hcEscape:
	return token_type::string;
    case HighlightClass::hcComment:
	return token_type::comment;
    case HighlightClass::hcType:
    case HighlightClass::hcTypeName:
    case HighlightClass::hcNamespace:
	return token_type::type;
    case HighlightClass::hcFunction:
	return token_type::function;
    case HighlightClass::hcNone:
	break;
    }
    return token_type::none;
}

// An LSP SymbolKind.
int64_t symbol_kind_number(lang::symbol_kind k)
{
    switch ( k )
    {
    case lang::symbol_kind::function:
	return 12;
    }
    return 13;
}

// An LSP DiagnosticSeverity.
int64_t severity_number(diag_severity s)
{
    switch ( s )
    {
    case diag_severity::error:
	return 1;
    case diag_severity::warning:
	return 2;
    }
    return 3;					// Information
}

// ---- reading a message ------------------------------------------------

const value *field(const value &v, const char *name)
{
    if ( !v.is_object() )
	return 0;
    const fields &f = v.as_object();
    fields::const_iterator i = f.find(name);
    return i == f.end() ? 0 : &i->second;
}

std::string text_field(const value &v, const char *name)
{
    const value *f = field(v, name);
    return f && f->is_string() ? f->as_string() : std::string();
}

int64_t int_field(const value &v, const char *name)
{
    const value *f = field(v, name);
    return f && f->is_integer() ? f->as_integer() : 0;
}

const value &object_field(const value &v, const char *name)
{
    static const value none;
    const value *f = field(v, name);
    return f ? *f : none;
}

// ---- writing one ------------------------------------------------------

value response(const value &id, const value &result)
{
    fields f;
    f["jsonrpc"] = value("2.0");
    f["id"] = id;
    f["result"] = result;
    return value::make_object(f);
}

value error_response(const value &id, error_code code, const std::string &message)
{
    fields e;
    e["code"] = value((int64_t)code);
    e["message"] = value(message);
    fields f;
    f["jsonrpc"] = value("2.0");
    f["id"] = id;
    f["error"] = value::make_object(e);
    return value::make_object(f);
}

value notification(const char *name, const value &params)
{
    fields f;
    f["jsonrpc"] = value("2.0");
    f["method"] = value(name);
    f["params"] = params;
    return value::make_object(f);
}

// A request the server sends; the client's response is dropped (handle).
value request(int64_t id, const char *name, const value &params)
{
    value r = notification(name, params);
    r.object()["id"] = value(id);
    return r;
}

// ---- URIs: a file:// URI names a path ----------------------------------

// The path a file:// URI names, percent-decoded. RFC 8089's DOS drive form
// `/C:/...` names `C:/...`: the URI path past its leading `/` is the host's
// path when that is absolute there (a drive on Windows; never on POSIX, where
// the leading `/` is the path's own). False for any other scheme.
bool uri_path(const std::string &uri, std::string &path)
{
    static const char scheme[] = "file://";
    const size_t n = sizeof(scheme) - 1;
    if ( uri.compare(0, n, scheme) != 0 )
	return false;
    js_decodeURIComponent(&path, uri.c_str() + n);
    if ( path.size() > 1 && detail::host_path_absolute(path.substr(1)) )
	path.erase(0, 1);
    return !path.empty();
}

// A path encoded for a URI one segment at a time: encodeURIComponent escapes
// the separators too, so they are joined outside it, as '/'.
std::string encoded_path(const std::string &path)
{
    std::string dir = detail::host_path_dirname(path), seg;
    js_encodeURIComponent(&seg, detail::host_path_basename(path).c_str());
    if ( dir.empty() )
	return seg;
    if ( dir.size() == 1 )
	return "/" + seg;			// the root's
    return encoded_path(dir.substr(0, dir.size() - 1)) + "/" + seg;
}

// ---- positions: the negotiated encoding against lang's byte columns -----

// A document's text, materialized once per request.
struct doc_text
{
    const hub::text_buffer &b;
    std::string t;
    explicit doc_text(const hub::text_buffer &buf) : b(buf), t(buf.text()) {}
    bool line(int64_t n, std::string &text, size_t &off) const
    {
	size_t len = 0;
	if ( n < 1 || !b.line_span((size_t)n, off, len) )
	    return false;
	text = t.substr(off, len);
	return true;
    }
};

// An LSP position as lang's point: the column converted to bytes on its line
// (clamped to the line); a line the text does not have keeps its column.
lang::point point_of(const doc_text &d, position_encoding enc, const value &pos)
{
    lang::point p;
    p.line = int_field(pos, "line") + 1;
    p.column = int_field(pos, "character");
    std::string line;
    size_t off = 0;
    if ( d.line(p.line, line, off) )
	p.column = enc == position_encoding::utf16
	    ? (int64_t)col16_to_byte(line, (long)p.column)
	    : std::min<int64_t>(std::max<int64_t>(p.column, 0), (int64_t)line.size());
    return p;
}

// An LSP position as a byte offset in the text: past the last line is the
// text's end.
size_t offset_of(const doc_text &d, position_encoding enc, const value &pos)
{
    lang::point p = point_of(d, enc, pos);
    std::string line;
    size_t off = 0;
    if ( !d.line(p.line, line, off) )
	return d.t.size();
    return off + (size_t)p.column;
}

value position_of(const doc_text &d, position_encoding enc, lang::point p)
{
    int64_t character = p.column;
    std::string line;
    size_t off = 0;
    if ( enc == position_encoding::utf16 && d.line(p.line, line, off) )
	character = col16_of_byte(line, (size_t)p.column);
    fields f;
    f["line"] = value((int64_t)(p.line - 1));
    f["character"] = value(character);
    return value::make_object(f);
}

value range_of(const doc_text &d, position_encoding enc, const lang::range &r)
{
    fields f;
    f["start"] = position_of(d, enc, r.from);
    f["end"] = position_of(d, enc, r.to);
    return value::make_object(f);
}

value location_of(const std::string &uri, const doc_text &d,
		  position_encoding enc, const lang::range &r)
{
    fields f;
    f["uri"] = value(uri);
    f["range"] = range_of(d, enc, r);
    return value::make_object(f);
}

} // namespace

struct server::state
{
    hub::world w;
    position_encoding enc;
    bool initialized;
    bool shut;
    bool exited;
    std::string root;			// the workspace's path (L0); "" = none
    std::map<hub::entity_id, std::string> uris;	// an open document's URI, as the client spells it
    command_host *host;			// executeCommand's session; none: no commands
    std::vector<std::string> commands;	// the advertised names, without "madc."
    bool shows_documents;		// the client answers window/showDocument
    int64_t next_request;		// the server's own request ids
    std::vector<value> *pending;	// a running command's notes; else none
    state() : enc(position_encoding::utf16), initialized(false), shut(false),
	      exited(false), host(0), shows_documents(false), next_request(0),
	      pending(0) {}
};

namespace {

// The document a request's textDocument names: the open one, else the file
// read from disk; 0 (and `why`) when the URI names neither.
hub::entity_id request_document(hub::world &w, const value &params,
				std::string &uri, std::string &why)
{
    uri = text_field(object_field(params, "textDocument"), "uri");
    std::string path;
    if ( !uri_path(uri, path) )
    {
	why = uri.empty() ? "no textDocument.uri" : "not a file:// URI: " + uri;
	return 0;
    }
    hub::entity_id e = document_find(w, path);
    if ( !e )
	e = document_open(w, path);
    if ( !e )
	why = "no such document: " + path;
    return e;
}

// textDocument/publishDiagnostics for one document.
value diagnostics_note(hub::world &w, hub::entity_id e, const std::string &uri,
		       position_encoding enc)
{
    std::vector<value> rows;
    std::vector<lang::diagnostic> ds;
    if ( e && lang::diagnostics(w, e, ds) )
    {
	doc_text d(*w.text_of(e));
	for ( size_t i = 0; i < ds.size(); ++i )
	{
	    fields f;
	    f["range"] = range_of(d, enc, ds[i].at);
	    f["severity"] = value(severity_number(ds[i].severity));
	    f["source"] = value("madc");
	    f["message"] = value(ds[i].message);
	    rows.push_back(value::make_object(f));
	}
    }
    fields p;
    p["uri"] = value(uri);
    p["diagnostics"] = value::make_array(rows);
    return notification("textDocument/publishDiagnostics", value::make_object(p));
}

value capabilities(position_encoding enc, const std::vector<std::string> &commands)
{
    fields sync;
    sync["openClose"] = value(true);
    sync["change"] = value((int64_t)2);		// incremental
    fields save;
    save["includeText"] = value(false);
    sync["save"] = value::make_object(save);

    std::vector<value> types;
    for ( size_t i = 0; i < sizeof(token_type_names) / sizeof(token_type_names[0]); ++i )
	types.push_back(value(token_type_names[i]));
    fields legend;
    legend["tokenTypes"] = value::make_array(types);
    legend["tokenModifiers"] = value::make_array();
    fields tokens;
    tokens["legend"] = value::make_object(legend);
    tokens["full"] = value(true);

    fields c;
    c["positionEncoding"] = value(enc == position_encoding::utf8 ? "utf-8" : "utf-16");
    c["textDocumentSync"] = value::make_object(sync);
    c["hoverProvider"] = value(true);
    c["definitionProvider"] = value(true);
    c["referencesProvider"] = value(true);
    c["documentSymbolProvider"] = value(true);
    c["semanticTokensProvider"] = value::make_object(tokens);
    if ( !commands.empty() )
    {
	std::vector<value> names;
	for ( size_t i = 0; i < commands.size(); ++i )
	    names.push_back(value("madc." + commands[i]));
	fields exec;
	exec["commands"] = value::make_array(names);
	c["executeCommandProvider"] = value::make_object(exec);
    }
    return value::make_object(c);
}

// The encoding the client offers that the server prefers: UTF-8 when it is
// offered (no conversion), else the protocol's UTF-16.
position_encoding negotiate(const value &params)
{
    const value &general = object_field(object_field(params, "capabilities"), "general");
    const value &offered = object_field(general, "positionEncodings");
    if ( offered.is_array() )
	for ( size_t i = 0; i < offered.as_array().size(); ++i )
	    if ( offered.as_array()[i].is_string()
		 && offered.as_array()[i].as_string() == "utf-8" )
		return position_encoding::utf8;
    return position_encoding::utf16;
}

// The workspace's root (L0): the first workspace folder, else rootUri.
std::string root_of(const value &params)
{
    std::string path;
    const value &folders = object_field(params, "workspaceFolders");
    if ( folders.is_array() && !folders.as_array().empty()
	 && uri_path(text_field(folders.as_array()[0], "uri"), path) )
	return path;
    if ( uri_path(text_field(params, "rootUri"), path) )
	return path;
    return std::string();
}

} // namespace

server::server() : _s(new state()) {}
server::~server() {}

void server::set_command_host(command_host *host) { _s->host = host; }
bool server::exited() const { return _s->exited; }
int server::exit_code() const { return _s->shut ? 0 : 1; }
position_encoding server::encoding() const { return _s->enc; }
hub::world &server::world() { return _s->w; }
const std::string &server::root() const { return _s->root; }

hub::entity_id server::open_file(const std::string &path)
{
    hub::entity_id e = document_find(_s->w, path);
    if ( !e )
	e = document_open(_s->w, path);
    if ( e )
	lang::document_handle(_s->w, e);	// parsed before the client asks
    return e;
}

namespace {

// One message on the wire. A string's bytes that are not UTF-8 are replaced,
// never a throw: JSON text is Unicode, and a document's bytes are not
// always.
void write_message(channel &io, const value &v)
{
    std::string text = hub::detail::wt_value_to_json(v).dump(
	-1, ' ', false, nlohmann::json::error_handler_t::replace);
    io.write(text);
}

} // namespace

int server::serve(channel &io)
{
    std::string body;
    while ( !_s->exited && io.readline(body) )
    {
	value reply;
	std::vector<value> notes;
	bool answered = true;
	nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
	if ( j.is_discarded() )
	    reply = error_response(value(), error_code::parse_error,
				   "the message is not JSON");
	else
	    answered = handle(hub::detail::wt_json_to_value(j), reply, notes);
	if ( answered )
	    write_message(io, reply);
	for ( size_t i = 0; i < notes.size(); ++i )
	    write_message(io, notes[i]);
    }
    return exit_code();
}

bool server::handle(const value &message, value &reply, std::vector<value> &notes)
{
    state &s = *_s;
    const value *idp = field(message, "id");
    const bool has_id = idp != 0;
    const value id = idp ? *idp : value();
    if ( !message.is_object() )
    {
	reply = error_response(value(), error_code::invalid_request,
			       "a message is a JSON object");
	return true;
    }
    const std::string name = text_field(message, "method");
    if ( name.empty() )
    {
	// A response to a request this server never sends.
	if ( has_id && (field(message, "result") || field(message, "error")) )
	    return false;
	reply = error_response(id, error_code::invalid_request,
			       "a message names its method");
	return true;
    }
    const method_row *row = method_of(name);
    if ( !s.initialized
	 && (!row || (row->m != method::initialize && row->m != method::exit)) )
    {
	if ( !has_id )
	    return false;			// dropped until initialize
	reply = error_response(id, error_code::server_not_initialized,
			       "the server is not initialized");
	return true;
    }
    if ( !row )
    {
	if ( !has_id )
	    return false;			// a notification nobody serves
	reply = error_response(id, error_code::method_not_found,
			       "Method not found: " + name);
	return true;
    }
    if ( row->notification && has_id )
    {
	reply = error_response(id, error_code::invalid_request,
			       name + " is a notification: it carries no id");
	return true;
    }
    if ( !row->notification && !has_id )
	return false;				// a request with no id has no answer
    if ( s.shut && row->m != method::exit )
    {
	if ( !has_id )
	    return false;
	reply = error_response(id, error_code::invalid_request,
			       "the server is shut down");
	return true;
    }
    const value &params = object_field(message, "params");

    switch ( row->m )
    {
    case method::initialize:
    {
	if ( s.initialized )
	{
	    reply = error_response(id, error_code::invalid_request,
				   "initialize is sent once");
	    return true;
	}
	s.initialized = true;
	s.enc = negotiate(params);
	s.root = root_of(params);
	const value *shows = field(object_field(object_field(object_field(
	    params, "capabilities"), "window"), "showDocument"), "support");
	s.shows_documents = shows && shows->is_boolean() && shows->as_boolean();
	std::string why;
	ReplSession *session = s.host ? s.host->session(why) : 0;
	if ( session )
	{
	    // The registry's names: the session's completion of `%`.
	    size_t start = 0;
	    s.commands = session->complete("%", 1, start);
	}
	fields info;
	info["name"] = value("madc");
	info["version"] = value(sys.version);
	fields r;
	r["capabilities"] = capabilities(s.enc, s.commands);
	r["serverInfo"] = value::make_object(info);
	reply = response(id, value::make_object(r));
	return true;
    }
    case method::shutdown:
	s.shut = true;
	reply = response(id, value());
	return true;
    case method::exit:
	s.exited = true;
	return false;
    case method::initialized:
    case method::will_save:
    case method::did_change_configuration:
    case method::did_change_watched_files:
    case method::did_change_workspace_folders:
    case method::cancel_request:
    case method::set_trace:
	return false;
    case method::did_open:
    {
	const value &td = object_field(params, "textDocument");
	std::string uri = text_field(td, "uri");
	std::string path;
	if ( !uri_path(uri, path) )
	    return false;
	hub::entity_id e = document_find(s.w, path);
	if ( !e )
	    e = document_new(s.w, path);
	if ( !e )
	    return false;
	hub::mutation_context mc(s.w);
	mc.text_load(e, text_field(td, "text"));
	s.uris[e] = uri;
	notes.push_back(diagnostics_note(s.w, e, uri, s.enc));
	return false;
    }
    case method::did_change:
    {
	std::string uri, why;
	hub::entity_id e = request_document(s.w, params, uri, why);
	if ( !e )
	    return false;
	const value &changes = object_field(params, "contentChanges");
	if ( changes.is_array() )
	    for ( size_t i = 0; i < changes.as_array().size(); ++i )
	    {
		const value &c = changes.as_array()[i];
		const value *r = field(c, "range");
		hub::mutation_context mc(s.w);
		if ( !r )
		{
		    mc.text_load(e, text_field(c, "text"));
		    continue;
		}
		doc_text d(*s.w.text_of(e));
		size_t from = offset_of(d, s.enc, object_field(*r, "start"));
		size_t to = offset_of(d, s.enc, object_field(*r, "end"));
		if ( to < from )
		    std::swap(from, to);
		mc.text_replace(e, from, to - from, text_field(c, "text"));
	    }
	notes.push_back(diagnostics_note(s.w, e, uri, s.enc));
	return false;
    }
    case method::did_save:
    {
	std::string uri, why;
	hub::entity_id e = request_document(s.w, params, uri, why);
	if ( e )
	    notes.push_back(diagnostics_note(s.w, e, uri, s.enc));
	return false;
    }
    case method::did_close:
    {
	std::string uri = text_field(object_field(params, "textDocument"), "uri");
	std::string path;
	if ( !uri_path(uri, path) )
	    return false;
	hub::entity_id e = document_find(s.w, path);
	if ( e )
	{
	    lang::document_close(s.w, e);
	    s.uris.erase(e);
	}
	notes.push_back(diagnostics_note(s.w, 0, uri, s.enc));	// clears them
	return false;
    }
    case method::execute_command:
	reply = run_command(id, params, notes);
	return true;
    case method::document_symbol:
    case method::definition:
    case method::references:
    case method::hover:
    case method::semantic_tokens_full:
	break;
    }

    // The reads: a document, then lang's answer, converted.
    std::string uri, why;
    hub::entity_id e = request_document(s.w, params, uri, why);
    if ( !e )
    {
	reply = error_response(id, error_code::invalid_params, why);
	return true;
    }
    if ( !lang::document_handle(s.w, e) )
    {
	reply = error_response(id, error_code::request_failed,
			       "the compiler does not parse " + uri);
	return true;
    }
    doc_text d(*s.w.text_of(e));
    lang::point at = point_of(d, s.enc, object_field(params, "position"));
    switch ( row->m )
    {
    case method::document_symbol:
    {
	std::vector<lang::symbol> syms;
	lang::outline(s.w, e, syms);
	std::vector<value> rows;
	for ( size_t i = 0; i < syms.size(); ++i )
	{
	    fields f;
	    f["name"] = value(syms[i].name);
	    f["kind"] = value(symbol_kind_number(syms[i].kind));
	    f["range"] = range_of(d, s.enc, syms[i].extent);
	    f["selectionRange"] = range_of(d, s.enc, syms[i].selection);
	    rows.push_back(value::make_object(f));
	}
	reply = response(id, value::make_array(rows));
	return true;
    }
    case method::definition:
    case method::references:
    {
	std::vector<lang::range> rs;
	if ( row->m == method::definition )
	    lang::definition(s.w, e, at, rs);
	else
	{
	    const value *decl = field(object_field(params, "context"),
				      "includeDeclaration");
	    lang::references(s.w, e, at, decl && decl->is_boolean()
				 && decl->as_boolean(), rs);
	}
	std::vector<value> rows;
	for ( size_t i = 0; i < rs.size(); ++i )
	    rows.push_back(location_of(uri, d, s.enc, rs[i]));
	reply = response(id, value::make_array(rows));
	return true;
    }
    case method::hover:
    {
	lang::hover_info h;
	if ( !lang::hover(s.w, e, at, h) )
	{
	    reply = response(id, value());		// nothing to say here
	    return true;
	}
	fields contents;
	contents["kind"] = value("plaintext");
	contents["value"] = value(h.text);
	fields r;
	r["contents"] = value::make_object(contents);
	if ( h.has_range )
	    r["range"] = range_of(d, s.enc, h.at);
	reply = response(id, value::make_object(r));
	return true;
    }
    case method::semantic_tokens_full:
    {
	std::vector<highlight_span> spans;
	lang::spans(s.w, e, spans);
	std::stable_sort(spans.begin(), spans.end(),
			 [](const highlight_span &a, const highlight_span &b) {
			     return a.line < b.line
				 || (a.line == b.line && a.column < b.column);
			 });
	// Five integers per token, each position relative to the previous
	// token's; a span inside the previous one (an escape inside its
	// string) is not a token of its own — tokens do not overlap.
	std::vector<value> data;
	int64_t prev_line = 0, prev_char = 0, prev_end = -1;
	for ( size_t i = 0; i < spans.size(); ++i )
	{
	    token_type t = token_of(spans[i].cls);
	    if ( t == token_type::none || spans[i].line < 1 || spans[i].length <= 0 )
		continue;
	    lang::point from, to;
	    from.line = to.line = spans[i].line;
	    from.column = spans[i].column;
	    to.column = spans[i].column + spans[i].length;
	    int64_t line0 = spans[i].line - 1;
	    int64_t c0 = int_field(position_of(d, s.enc, from), "character");
	    int64_t c1 = int_field(position_of(d, s.enc, to), "character");
	    if ( line0 == prev_line && prev_end >= 0 && c0 < prev_end )
		continue;
	    data.push_back(value(line0 - prev_line));
	    data.push_back(value(line0 == prev_line ? c0 - prev_char : c0));
	    data.push_back(value(c1 - c0));
	    data.push_back(value((int64_t)t));
	    data.push_back(value((int64_t)0));		// no modifiers
	    prev_line = line0;
	    prev_char = c0;
	    prev_end = c1;
	}
	fields r;
	r["data"] = value::make_array(data);
	reply = response(id, value::make_object(r));
	return true;
    }
    default:
	break;
    }
    return false;
}

// workspace/executeCommand (design decision 5): `madc.NAME` runs `%NAME ARG`
// in the host's session, ARG the path arguments[0] names (a file:// URI or a
// path), one shell word. The server's documents are the session's buffers, so a command
// reads a document's unsaved text; the client is its editor
// (window/showDocument). The result is what the command showed, what its
// program printed and its rendered diagnostics.
value server::run_command(const value &id, const value &params, std::vector<value> &notes)
{
    state &s = *_s;
    static const char prefix[] = "madc.";
    const size_t n = sizeof(prefix) - 1;
    const std::string command = text_field(params, "command");
    const std::string name = command.compare(0, n, prefix) == 0
	? command.substr(n) : std::string();
    if ( name.empty()
	 || std::find(s.commands.begin(), s.commands.end(), name) == s.commands.end() )
	return error_response(id, error_code::invalid_params,
			      "no such command: " + command);
    std::string why;
    ReplSession *session = s.host ? s.host->session(why) : 0;
    if ( !session )
	return error_response(id, error_code::request_failed,
			      "no session runs commands: " + why);
    std::string line = "%" + name;
    const value &args = object_field(params, "arguments");
    if ( args.is_array() && !args.as_array().empty()
	 && args.as_array()[0].is_string() )
    {
	const std::string &arg = args.as_array()[0].as_string();
	std::string path;
	line += " " + ns_common::shell_quote(uri_path(arg, path) ? path : arg);
    }

    hub::world &w = s.w;
    session->set_buffers([&w](const dis::istring &name, dis::istring &path,
			      std::string &text, bool &has_text, std::string &why) {
	if ( name.empty() )
	{
	    why = "an LSP command names its FILE: no document is current";
	    return false;
	}
	path = name;
	hub::entity_id e = document_find(w, name.str());
	has_text = e && w.text_of(e);
	if ( has_text )
	    text = w.text_of(e)->text();
	return true;
    });
    state *sp = &s;
    session->set_editor([sp](const std::string &path, int at, std::string &why) {
	if ( !sp->pending || !sp->shows_documents )
	{
	    why = "the client shows no documents (window/showDocument)";
	    return false;
	}
	hub::entity_id e = document_find(sp->w, path);
	std::map<hub::entity_id, std::string>::const_iterator u = sp->uris.find(e);
	std::string uri = u != sp->uris.end() ? u->second : std::string();
	if ( uri.empty() )
	{
	    std::string real = detail::resolve_real_path(path.c_str());
	    std::string enc = encoded_path(real.empty() ? path : real);
	    uri = std::string("file://") + (enc.compare(0, 1, "/") == 0 ? "" : "/") + enc;
	}
	fields p;
	p["uri"] = value(uri);
	p["takeFocus"] = value(true);
	if ( at > 0 )
	{
	    fields pos;
	    pos["line"] = value((int64_t)(at - 1));
	    pos["character"] = value((int64_t)0);
	    fields sel;
	    sel["start"] = value::make_object(pos);
	    sel["end"] = value::make_object(pos);
	    p["selection"] = value::make_object(sel);
	}
	sp->pending->push_back(request(++sp->next_request, "window/showDocument",
				       value::make_object(p)));
	return true;
    });

    s.pending = &notes;
    const bool ok = session->submit(line);
    s.pending = 0;
    std::string output, diagnostics;
    s.host->take(output, diagnostics);
    fields r;
    r["ok"] = value(ok);
    r["text"] = value(session->shown());
    r["output"] = value(output);
    r["diagnostics"] = value(diagnostics);
    return response(id, value::make_object(r));
}

// ---- madc --lsp's command host -----------------------------------------

struct backend_commands::state
{
    std::vector<std::string> options;
    ProgramFactory make_program;
    std::string std_option;
    std::ostringstream output, diagnostics;
    std::unique_ptr<SessionClient> client;
    std::unique_ptr<BackendSession> session;	// destroyed before its client
};

backend_commands::backend_commands(const std::vector<std::string> &options,
				   const ProgramFactory &make_program,
				   const std::string &std_option)
    : _s(new state())
{
    _s->options = options;
    _s->make_program = make_program;
    _s->std_option = std_option;
}

backend_commands::~backend_commands() {}

ReplSession *backend_commands::session(std::string &why)
{
    state &s = *_s;
    int status = 0;
    if ( s.session && !s.session->ended(status) )
	return s.session.get();
    s.session.reset();
    s.client.reset(new SessionClient());
    s.client->set_backend_arguments(s.options);
    s.client->set_history_file(std::string());	// %history: none
    s.client->set_diagnostic_colour(false);		// JSON text carries no escapes
    if ( !s.client->start(s.std_option, s.make_program) )
    {
	why = s.client->last_error();
	s.client.reset();
	return 0;
    }
    s.session.reset(new BackendSession(*s.client, s.diagnostics, &s.output));
    return s.session.get();
}

void backend_commands::take(std::string &output, std::string &diagnostics)
{
    output = _s->output.str();
    diagnostics = _s->diagnostics.str();
    _s->output.str(std::string());
    _s->diagnostics.str(std::string());
}

} // namespace lsp
} // namespace madc
