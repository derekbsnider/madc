//////////////////////////////////////////////////////////////////////////
//									//
// madc_memory.cpp — a project's memory (contract in			//
// include/madc_memory.h): the event stream, its folds, and the tools	//
// over them — a C++ port of madcide's change log, proposals, intent	//
// records and links and test runs (split arc 1C; madcide's own copies	//
// were deleted in 1D, its sessions read this through madc::tool_host).	//
// Every wire word converts once, here, to an enum.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ns_common.h"		// the madc_graph_* reads
#include "datadef.h"		// madc_self_exe_path: the running madc runs the tests
#include "madc_memory.h"
#include "madc_jsonrpc.h"	// a record's members; JSON lines
#include "madc_mcp.h"		// nexus.sync: a client of a source's server
#include "madc_config_file.h"	// nexus sources: the user config directory
#include "madc_lang.h"		// document_handle: the served document's parse
#include "madc_documents.h"	// the stream's file: document_read_file / _write_file
#include "madc_file_kinds.h"	// an asset's kind: lexed and parsed
#include "madc_posix_io.h"	// host_path_* : a test's directory and name
#include "madcdis/hub.h"
#include "madcdis/verbs.h"	// mutation_context: the text subscription
#include "madcdis/channel.h"	// test.run: the canonical runner, spawned
#include "libmadc/sysinfo.h"	// sys.platform: the node test.run runs on

namespace madc {
namespace memory {

using namespace jsonrpc;
using tools::context;
using tools::refusal;
using tools::refused;
using tools::served_path;
using tools::tier;

namespace {

typedef std::map<std::string, value> fields;

// ---- the vocabulary: a wire word converts once, to an enum ------------
// Each table's row 0 is "" (none); an enum's value is its row.

template <typename E, size_t N>
E word_of(const char *const (&names)[N], const std::string &word)
{
    for ( size_t i = 1; i < N; ++i )
	if ( word == names[i] )
	    return (E)i;
    return (E)0;
}

template <typename E, size_t N>
const char *name_of(const char *const (&names)[N], E e)
{
    return (size_t)e < N ? names[(size_t)e] : "";
}

enum class kind : unsigned char
{
    none, splice, checkpoint, nodeop, proposal, decision, record, link, testrun, buildrun
};
const char *const kind_names[] = {
    "", "splice", "checkpoint", "nodeop", "proposal", "decision", "record", "link",
    "testrun", "buildrun"
};

enum class status : unsigned char { none, open, accepted, rejected, withdrawn };
const char *const status_names[] = { "", "open", "accepted", "rejected", "withdrawn" };

enum class rkind : unsigned char
{
    none, task, requirement, decision, workset, milestone, test
};
const char *const rkind_names[] = {
    "", "task", "requirement", "decision", "workset", "milestone", "test"
};

enum class rstate : unsigned char { none, open, active, done, superseded, rejected };
const char *const rstate_names[] = {
    "", "open", "active", "done", "superseded", "rejected"
};

enum class rop : unsigned char { none, create, update, close };
const char *const rop_names[] = { "", "create", "update", "close" };

enum class lop : unsigned char { none, link, unlink };
const char *const lop_names[] = { "", "link", "unlink" };

// An external system's manifest: its transport, and its vocabulary's words
// (the MCP tool each maps to). A synced row's field lands in a record slot:
// the record's key, its state, or (every other name) a field.
enum class transport : unsigned char { none, stdio, http };
const char *const transport_names[] = { "", "stdio", "http" };

enum class nexus_op : unsigned char
{
    none, list_tasks, get_task, create_task, update_task, comment
};
const char *const nexus_op_names[] = {
    "", "list_tasks", "get_task", "create_task", "update_task", "comment"
};

enum class row_slot : unsigned char { field, key, state };
const char *const row_slot_names[] = { "", "key", "state" };

enum class ref_kind : unsigned char { none, record, symbol, commit, proposal, external };
const char *const ref_kind_names[] = {
    "", "record", "symbol", "commit", "proposal", "external"
};

enum class rel : unsigned char
{
    none, implements, tests, fixes, affects, requested_by, documents, supersedes,
    blocked_by, planned_for, reviewed_by, generated_by, introduced_by, modified_by
};
const char *const rel_names[] = {
    "", "implements", "tests", "fixes", "affects", "requested_by", "documents",
    "supersedes", "blocked_by", "planned_for", "reviewed_by", "generated_by",
    "introduced_by", "modified_by"
};

enum class provenance : unsigned char { none, compiler, git, event, record, inferred };
const char *const provenance_names[] = {
    "", "compiler", "git", "event", "record", "inferred"
};

enum class family : unsigned char { none, mad, unit, external };
const char *const family_names[] = { "", "mad", "unit", "external" };

enum class result_word : unsigned char { none, pass, fail, skip, timeout, error };
const char *const result_names[] = { "", "pass", "fail", "skip", "timeout", "error" };

enum class explain_level : unsigned char { none, summary, full };
const char *const explain_names[] = { "", "summary", "full" };

kind kind_of(const value &rec)
{
    return word_of<kind>(kind_names, text_field(rec, "kind"));
}

// A record's layers: what an asset can answer (madcide's asset layers,
// design §3.9), as bits.
enum layer : int64_t
{
    l_text = 1, l_versioned = 2, l_managed = 4, l_lexable = 8, l_parseable = 16,
    l_executable = 32, l_testable = 64
};
const char *const layer_names[] = {
    "text", "versioned", "managed", "lexable", "parseable", "executable", "testable"
};

value layer_words(int64_t bits)
{
    std::vector<value> out;
    for ( size_t i = 0; i < sizeof(layer_names) / sizeof(layer_names[0]); ++i )
	if ( bits & ((int64_t)1 << i) )
	    out.push_back(value(layer_names[i]));
    return value::make_array(out);
}

const char *layer_name(int64_t bit)
{
    for ( size_t i = 0; i < sizeof(layer_names) / sizeof(layer_names[0]); ++i )
	if ( bit == ((int64_t)1 << i) )
	    return layer_names[i];
    return "";
}

std::string key_of(int64_t n)
{
    return std::to_string(n);
}

value obj()
{
    return value::make_object();
}

value arr()
{
    return value::make_array();
}

// An array member's elements; none when it is not one (a restored record is
// data, never trusted to hold the shape it should).
const std::vector<value> &items(const value &v)
{
    static const std::vector<value> none;
    return v.is_array() ? v.as_array() : none;
}

} // namespace

// ---- the stream -------------------------------------------------------

project::project(hub::world &w) : _w(w), _head(0), _was_bound(false), _sources_loaded(false)
{
    hub::mutation_context(w).text_subscribe(this);
    // A document already open (a host whose world came first): its text
    // now is the baseline its first edit records.
    for ( hub::entity_id d : w.docs() )
	if ( const hub::text_buffer *b = w.text_of(d) )
	    _baseline[d] = b->text();
}

project::~project()
{
    if ( !_world_gone )
	hub::mutation_context(_w).text_unsubscribe(this);
}

std::string project::path_of(hub::entity_id doc) const
{
    const hub::document *d = doc ? _w.doc_of(doc) : 0;
    return d ? d->path() : std::string();
}

int64_t project::append(value rec, hub::entity_id doc)
{
    if ( !rec.is_object() )
	rec = obj();
    rec.object()["seq"] = value(++_head);
    rec.object()["ts"] = value((int64_t)std::time(0));
    if ( doc )
    {
	rec.object()["doc"] = value((int64_t)doc);
	const std::string path = path_of(doc);
	if ( !path.empty() )
	{
	    rec.object()["path"] = value(path);
	    _followed.insert(path);
	}
    }
    _records.push_back(rec);
    if ( !_path.empty() )
	document_write_file(_path, encode(rec) + "\n", true);
    return _head;
}

int64_t project::note(value rec, hub::entity_id doc)
{
    const kind k = kind_of(rec);
    if ( k == kind::none || k == kind::splice || k == kind::checkpoint )
	return 0;
    return append(rec, doc);
}

bool project::bind(const std::string &path, std::string &why)
{
    if ( !_path.empty() && path == _path )
	return true;				// already this stream's file
    std::string blob;
    const bool restoring = document_read_file(path, blob);
    if ( _head > 0 && restoring )
    {
	unbind();
	why = "the events were not restored: this session already logged events";
	return false;
    }
    if ( !restoring && !_records.empty() && !_was_bound )
    {
	// The FIRST events file, new (a project's manifest made mid-session):
	// the session's records so far are its first lines. A later project's
	// new file starts empty — another project's records are not its own.
	std::string lines;
	for ( const value &rec : _records )
	    lines += encode(rec) + "\n";
	if ( !document_write_file(path, lines, false) )
	{
	    unbind();
	    why = "the events file could not be written";
	    return false;
	}
    }
    if ( restoring )
    {
	size_t at = 0;
	while ( at < blob.size() )
	{
	    size_t nl = blob.find('\n', at);
	    if ( nl == std::string::npos )
		nl = blob.size();
	    value rec;
	    if ( nl > at && decode(blob.substr(at, nl - at), rec) && rec.is_object() )
	    {
		_records.push_back(rec);		// a torn line is skipped, never trusted
		_head = std::max(_head, int_field(rec, "seq"));
		if ( !text_field(rec, "path").empty() )
		    _followed.insert(text_field(rec, "path"));
	    }
	    at = nl + 1;
	}
	if ( !blob.empty() && blob[blob.size() - 1] != '\n' )
	    document_write_file(path, "\n", true);	// a torn tail ends: the next record
							// starts a line of its own
    }
    _path = path;
    _was_bound = true;
    return true;
}

void project::unbind()
{
    _path.clear();
}

std::string project::replay(hub::entity_id doc, int64_t upto) const
{
    const hub::text_buffer *live = _w.text_of(doc);
    const std::string path = path_of(doc);
    std::string text;
    bool any = false, seen = false;
    for ( const value &rec : _records )
    {
	if ( text_field(rec, "path") != path )
	    continue;
	const kind k = kind_of(rec);
	if ( upto > 0 && int_field(rec, "seq") > upto )
	{
	    // A target below the oldest surviving record clamps up to that
	    // checkpoint (the retention rule).
	    if ( k == kind::checkpoint && !seen )
	    {
		text = text_field(rec, "text");
		any = true;
	    }
	    break;
	}
	seen = true;
	if ( k == kind::checkpoint )
	{
	    text = text_field(rec, "text");
	    any = true;
	}
	else if ( k == kind::splice )
	{
	    const size_t at = (size_t)int_field(rec, "at"), del = (size_t)int_field(rec, "del");
	    if ( at <= text.size() )
		text.replace(at, std::min(del, text.size() - at), text_field(rec, "ins"));
	    any = true;
	}
    }
    return any ? text : (live ? live->text() : std::string());
}

void project::on_text_change(hub::world &w, const hub::text_change &c)
{
    if ( !w.doc_of(c.entity) )
	return;					// not a document's text
    const hub::text_buffer *b = w.text_of(c.entity);
    const std::string path = path_of(c.entity);
    const bool known = !path.empty() && _followed.count(path);
    if ( c.what != hub::text_change::kind::splice )
    {
	// A whole-buffer change (a load, an undo, a redo): the checkpoint
	// replay restarts from — once the stream follows the document;
	// before that, the baseline its first edit records.
	if ( known )
	{
	    fields f;
	    f["kind"] = value("checkpoint");
	    f["text"] = value(b ? b->text() : std::string());
	    append(value::make_object(f), c.entity);
	}
	else if ( b )
	    _baseline[c.entity] = b->text();
	return;
    }
    if ( !known )
    {
	std::map<hub::entity_id, std::string>::iterator base = _baseline.find(c.entity);
	fields cp;
	cp["kind"] = value("checkpoint");
	if ( base == _baseline.end() )
	{
	    // No baseline seen: the text after this edit starts the record.
	    cp["text"] = value(b ? b->text() : std::string());
	    append(value::make_object(cp), c.entity);
	    return;
	}
	cp["text"] = value(base->second);
	_baseline.erase(base);
	append(value::make_object(cp), c.entity);
    }
    fields f;
    f["kind"] = value("splice");
    f["at"] = value((int64_t)c.off);
    f["del"] = value((int64_t)c.old_len);
    f["ins"] = value(c.inserted ? *c.inserted : std::string());
    append(value::make_object(f), c.entity);
}

// ---- the folds and the tools ------------------------------------------

namespace {


// A tool's view of the memory: its records and writer, and the registry it
// serves (the edit core lands an accepted proposal through it).
struct seat
{
    project &m;
    const tools::registry &r;
};

// ---- proposals --------------------------------------------------------

struct proposal_fold
{
    std::vector<int64_t> seqs;
    std::map<int64_t, value> props;	// with status, decision, checks
};

proposal_fold fold_proposals(const std::vector<value> &recs, const std::string &path)
{
    proposal_fold f;
    for ( const value &rec : recs )
    {
	if ( text_field(rec, "path") != path )
	    continue;
	const kind k = kind_of(rec);
	if ( k == kind::proposal )
	{
	    const int64_t seq = int_field(rec, "seq");
	    value p = rec;
	    p.object()["status"] = value(name_of(status_names, status::open));
	    f.props[seq] = p;
	    f.seqs.push_back(seq);
	}
	else if ( k == kind::decision || k == kind::testrun )
	{
	    std::map<int64_t, value>::iterator it = f.props.find(int_field(rec, "proposal"));
	    if ( it == f.props.end() )
		continue;			// naming no proposal here: never trusted
	    if ( k == kind::decision )
	    {
		it->second.object()["status"] = object_field(rec, "decision");	// the last wins
		it->second.object()["decision"] = rec;
	    }
	    else
	    {
		value &checks = it->second.object()["checks"];
		if ( !checks.is_array() )
		    checks = arr();
		checks.array().push_back(rec);
	    }
	}
    }
    return f;
}

status status_of(const value &p)
{
    return word_of<status>(status_names, text_field(p, "status"));
}

// ---- records and links ------------------------------------------------

struct nexus_fold
{
    std::map<int64_t, value> records;
    std::vector<int64_t> ids;
    std::vector<value> links;
};

// The ONE canonical key links are matched by.
std::string ref_key(const value &ref)
{
    switch ( word_of<ref_kind>(ref_kind_names, text_field(ref, "ref")) )
    {
    case ref_kind::record:
	return "record:" + key_of(int_field(ref, "id"));
    case ref_kind::symbol:
	return "symbol:" + text_field(ref, "file") + "\t" + text_field(ref, "kind")
	    + "\t" + text_field(ref, "name");
    case ref_kind::commit:
	return "commit:" + text_field(ref, "sha");
    case ref_kind::proposal:
	return "proposal:" + key_of(int_field(ref, "seq"));
    case ref_kind::external:
	return "external:" + text_field(ref, "source") + "\t" + text_field(ref, "key");
    case ref_kind::none:
	break;
    }
    return std::string();
}

nexus_fold fold_nexus(const std::vector<value> &recs)
{
    nexus_fold f;
    for ( const value &rec : recs )
    {
	const kind k = kind_of(rec);
	if ( k == kind::record )
	{
	    const rop op = word_of<rop>(rop_names, text_field(rec, "op"));
	    if ( op == rop::create )
	    {
		const int64_t seq = int_field(rec, "seq");
		fields r;
		r["id"] = value(seq);
		r["rkind"] = object_field(rec, "rkind");
		r["state"] = word_of<rstate>(rstate_names, text_field(rec, "state")) == rstate::none
		    ? value(name_of(rstate_names, rstate::open)) : object_field(rec, "state");
		r["origin"] = object_field(rec, "origin");
		r["ext_key"] = object_field(rec, "ext_key");
		r["actor"] = object_field(rec, "actor");
		r["doc"] = object_field(rec, "doc");
		r["created_seq"] = value(seq);
		r["updated_seq"] = value(seq);
		r["fields"] = object_field(rec, "fields").is_object()
		    ? object_field(rec, "fields") : obj();
		f.records[seq] = value::make_object(r);
		f.ids.push_back(seq);
	    }
	    else if ( op == rop::update || op == rop::close )
	    {
		std::map<int64_t, value>::iterator it = f.records.find(int_field(rec, "id"));
		if ( it == f.records.end() )
		    continue;			// an op on an unknown record: never trusted
		const value &nf = object_field(rec, "fields");
		if ( nf.is_object() )
		{
		    value &have = it->second.object()["fields"];
		    if ( !have.is_object() )
			have = obj();
		    for ( const auto &kv : nf.as_object() )
			have.object()[kv.first] = kv.second;	// the update's keys win
		}
		if ( word_of<rstate>(rstate_names, text_field(rec, "state")) != rstate::none )
		    it->second.object()["state"] = object_field(rec, "state");
		it->second.object()["updated_seq"] = object_field(rec, "seq");
	    }
	}
	else if ( k == kind::link )
	{
	    const lop op = word_of<lop>(lop_names, text_field(rec, "op"));
	    const std::string fk = ref_key(object_field(rec, "from"));
	    const std::string tk = ref_key(object_field(rec, "to"));
	    const std::string rl = text_field(rec, "rel");
	    if ( op == lop::link )
	    {
		fields l;
		l["seq"] = object_field(rec, "seq");
		l["from"] = object_field(rec, "from");
		l["rel"] = value(rl);
		l["to"] = object_field(rec, "to");
		l["from_key"] = value(fk);
		l["to_key"] = value(tk);
		l["actor"] = object_field(rec, "actor");
		f.links.push_back(value::make_object(l));
	    }
	    else if ( op == lop::unlink )
	    {
		std::vector<value> kept;
		for ( const value &l : f.links )
		    if ( !(text_field(l, "from_key") == fk && text_field(l, "rel") == rl
			   && text_field(l, "to_key") == tk) )
			kept.push_back(l);
		f.links.swap(kept);
	    }
	}
    }
    return f;
}

bool live_state(rstate st)
{
    return st != rstate::superseded && st != rstate::rejected;
}

// A symbol ref for a node of the served document.
value symbol_ref(const context &c, const value &node)
{
    fields f;
    f["ref"] = value(name_of(ref_kind_names, ref_kind::symbol));
    f["file"] = value(served_path(c));
    f["kind"] = object_field(node, "kind");
    f["name"] = object_field(node, "name");
    return value::make_object(f);
}

// A symbol ref resolved against the served document's live tree.
bool symbol_node(const context &c, int64_t h, const value &ref, value &node)
{
    if ( !h || text_field(ref, "file") != served_path(c) )
	return false;
    std::string name = text_field(ref, "name");
    value d;
    madc_graph_definition(&d, h, &name);
    for ( const value &n : items(object_field(d, "nodes")) )
	if ( text_field(n, "kind") == text_field(ref, "kind") )
	{
	    node = n;
	    return true;
	}
    return false;
}

// The links touching `key`, as context rows (provenance record), each with
// the record at the other end when there is one.
void link_rows(const nexus_fold &f, const std::string &key, std::vector<value> &rows)
{
    for ( const value &l : f.links )
    {
	const bool from_me = text_field(l, "from_key") == key;
	if ( !from_me && text_field(l, "to_key") != key )
	    continue;
	const value &other = object_field(l, from_me ? "to" : "from");
	fields row;
	row["provenance"] = value(name_of(provenance_names, provenance::record));
	row["rel"] = object_field(l, "rel");
	row["direction"] = value(from_me ? "out" : "in");
	row["ref"] = other;
	row["seq"] = object_field(l, "seq");
	if ( word_of<ref_kind>(ref_kind_names, text_field(other, "ref")) == ref_kind::record )
	{
	    std::map<int64_t, value>::const_iterator r = f.records.find(int_field(other, "id"));
	    if ( r != f.records.end() )
		row["record"] = r->second;
	}
	rows.push_back(value::make_object(row));
    }
}

// The decisions that constrain a node: decision records linked by affects
// or documents to the node's symbol (informing, never blocking).
value constraints_of(const context &c, const nexus_fold &f, const value &target)
{
    const std::string key = ref_key(symbol_ref(c, target));
    std::vector<value> out;
    for ( const value &l : f.links )
    {
	const rel r = word_of<rel>(rel_names, text_field(l, "rel"));
	if ( r != rel::affects && r != rel::documents )
	    continue;
	const char *end = text_field(l, "to_key") == key ? "from"
	    : text_field(l, "from_key") == key ? "to" : 0;
	if ( !end )
	    continue;
	const value &other = object_field(l, end);
	if ( word_of<ref_kind>(ref_kind_names, text_field(other, "ref")) != ref_kind::record )
	    continue;
	std::map<int64_t, value>::const_iterator rec = f.records.find(int_field(other, "id"));
	if ( rec == f.records.end()
	     || word_of<rkind>(rkind_names, text_field(rec->second, "rkind")) != rkind::decision )
	    continue;
	fields row;
	row["id"] = object_field(rec->second, "id");
	row["state"] = object_field(rec->second, "state");
	row["title"] = object_field(object_field(rec->second, "fields"), "title");
	row["rel"] = object_field(l, "rel");
	out.push_back(value::make_object(row));
    }
    return value::make_array(out);
}

// ---- history ----------------------------------------------------------

// The EVENT rows of a node (madcide's graph_history, its event half): the
// node-ops whose target has the node's kind and name, and the most recent
// text edit intersecting its extent [a, b) — walked newest to oldest with
// the inverse shift; past that edit earlier coordinates are not derivable,
// so `complete` turns false.
void event_rows(const context &c, const std::vector<value> &recs, const value &span_answer,
		size_t a, size_t b, std::vector<value> &rows, bool &complete)
{
    const std::string path = served_path(c);
    std::vector<value> found;
    bool walking = true;
    for ( size_t i = recs.size(); i > 0; --i )
    {
	const value &rec = recs[i - 1];
	if ( text_field(rec, "path") != path )
	    continue;
	const kind k = kind_of(rec);
	if ( k == kind::splice && walking )
	{
	    const int64_t at = int_field(rec, "at"), del = int_field(rec, "del");
	    const int64_t ins = (int64_t)text_field(rec, "ins").size();
	    if ( at + ins <= (int64_t)a )
	    {
		a = (size_t)((int64_t)a - (ins - del));	// before the span: undo its shift
		b = (size_t)((int64_t)b - (ins - del));
	    }
	    else if ( at < (int64_t)b )
	    {
		fields row;
		row["provenance"] = value(name_of(provenance_names, provenance::event));
		row["seq"] = object_field(rec, "seq");
		row["ts"] = object_field(rec, "ts");
		row["verb"] = value("splice");
		row["at"] = value(at);
		row["del"] = value(del);
		row["ins"] = value(ins);
		found.push_back(value::make_object(row));
		walking = false;
		complete = false;
	    }
	}
	else if ( k == kind::nodeop )
	{
	    const value &tgt = object_field(rec, "target");
	    if ( text_field(tgt, "kind") == text_field(span_answer, "kind")
		 && text_field(tgt, "name") == text_field(span_answer, "name") )
	    {
		fields row;
		row["provenance"] = value(name_of(provenance_names, provenance::event));
		row["seq"] = object_field(rec, "seq");
		row["ts"] = object_field(rec, "ts");
		row["verb"] = object_field(rec, "verb");
		found.push_back(value::make_object(row));
	    }
	}
    }
    for ( size_t i = found.size(); i > 0; --i )
	rows.push_back(found[i - 1]);		// oldest first
}

// The GIT rows of a node (madcide's graph_history, its git half), with the
// version history: the live text blamed over the node's lines, folded to
// the distinct commits that last touched them — an uncommitted hunk (sha
// "") is the event rows' job.
void git_rows(const seat &s, const context &c, const value &span, std::vector<value> &rows)
{
    const hub::text_buffer *t = c.w.text_of(c.doc);
    const int64_t l0 = int_field(span, "line"), l1 = int_field(span, "end_line");
    value bl;
    if ( !s.r.history() || !t
	 || !s.r.history()->blame(served_path(c), t->text(), l0, l1 - l0 + 1, bl) )
	return;
    std::set<std::string> seen;
    for ( const value &hunk : items(object_field(bl, "rows")) )
    {
	const std::string sha = text_field(hunk, "sha");
	if ( sha.empty() || !seen.insert(sha).second )
	    continue;
	fields row;
	row["provenance"] = value(name_of(provenance_names, provenance::git));
	row["sha"] = value(sha);
	row["author"] = object_field(hunk, "author");
	row["when"] = object_field(hunk, "when");
	row["summary"] = object_field(hunk, "summary");
	rows.push_back(value::make_object(row));
    }
}

// graph.history over node `id`: the stream's rows.
bool node_history(const seat &s, const context &c, int64_t h, int64_t id, value &out)
{
    value sp;
    madc_graph_span(&sp, h, id);
    size_t a = 0, b = 0;
    const value *span = field(sp, "span");
    if ( !span || !c.w.text_of(c.doc) || !tools::span_extent(*c.w.text_of(c.doc), *span, a, b) )
    {
	out = refused(sp) ? sp : refusal("the node has no source extent");
	return false;
    }
    std::vector<value> rows;
    git_rows(s, c, *span, rows);
    bool complete = true;
    event_rows(c, s.m.records(), sp, a, b, rows, complete);
    fields f;
    f["node"] = sp;
    f["rows"] = value::make_array(rows);
    f["complete"] = value(complete);
    out = value::make_object(f);
    return true;
}

// ---- tests ------------------------------------------------------------

// The runner THIS node has for a family ("" = none): the canonical runner
// for the mad family — scripts/run_tests.sh under the project root (the
// working directory), POSIX only; an external record carries its own
// command; the unit family is `make -C src test`'s, no seat here.
std::string runner_of(family fam)
{
#ifdef _WIN32
    (void)fam;
    return std::string();
#else
    static const char runner[] = "scripts/run_tests.sh";
    return fam == family::mad && detail::host_path_type_of(runner) == detail::host_path_type::file
	? std::string(runner) : std::string();
#endif
}

// The live test records (family none = every family).
std::vector<value> test_records(const nexus_fold &f, family fam)
{
    std::vector<value> out;
    for ( int64_t id : f.ids )
    {
	const value &r = f.records.find(id)->second;
	if ( word_of<rkind>(rkind_names, text_field(r, "rkind")) != rkind::test
	     || !live_state(word_of<rstate>(rstate_names, text_field(r, "state"))) )
	    continue;
	const family rf = word_of<family>(family_names, text_field(object_field(r, "fields"), "family"));
	if ( fam != family::none && rf != fam )
	    continue;
	out.push_back(r);
    }
    return out;
}

bool tests_runnable_here(const nexus_fold &f)
{
    for ( const value &r : test_records(f, family::none) )
    {
	const value &flds = object_field(r, "fields");
	const family fam = word_of<family>(family_names, text_field(flds, "family"));
	if ( fam == family::external ? !text_field(flds, "command").empty()
				     : !runner_of(fam).empty() )
	    return true;
    }
    return false;
}

// The asset's layers (design §3.9): the bytes always; tracked when it is a
// file; lexed, parsed and run when the compiler parses its kind; tested
// when a live test record has a runner here; versioned, asked only when
// `versioned` (a repository read), when the version history tracks it.
int64_t asset_layers(const seat &s, const context &c, bool versioned)
{
    int64_t layers = l_text;
    const hub::document *d = c.w.doc_of(c.doc);
    value state;
    if ( d && !d->path().empty() )
	layers |= l_managed;
    if ( versioned && d && !d->path().empty() && s.r.history()
	 && s.r.history()->state(d->path(), state) )
	layers |= l_versioned;
    if ( d && file_kind_family(d->kind()) != fkUNKNOWN )
	layers |= l_lexable | l_parseable | l_executable;
    if ( tests_runnable_here(fold_nexus(s.m.records())) )
	layers |= l_testable;
    return layers;
}

// The layer gate: false, with the refusal, when the asset lacks a layer
// the tool needs.
bool has_layers(const seat &s, const context &c, const char *tool, int64_t need,
		value &result)
{
    const int64_t have = asset_layers(s, c, (need & l_versioned) != 0);
    for ( int64_t bit = l_text; bit <= l_testable; bit <<= 1 )
	if ( (need & bit) && !(have & bit) )
	{
	    const hub::document *d = c.w.doc_of(c.doc);
	    std::string words;
	    const value have_words = layer_words(have);
	    for ( const value &w : items(have_words) )
		words += (words.empty() ? "" : ", ") + w.as_string();
	    result = refusal(std::string("no ") + layer_name(bit) + " layer for kind "
			     + file_kind_name(d ? d->kind() : 0) + " ('" + tool
			     + "' needs it; this asset has: " + words + ")");
	    return false;
	}
    return true;
}

} // namespace

namespace {

// ---- the tools' plumbing ----------------------------------------------

const char *const REF_TEXT = "a ref: {ref: record|symbol|commit|proposal|external, ...}"
    " — record {id}, symbol {file, kind, name}, commit {sha}, proposal {seq},"
    " external {source, key}";

// The ref an argument carries: false, with the refusal, when it is not one.
bool ref_arg(const value &args, const char *key, value &ref, value &result)
{
    const value &wire = object_field(args, key);
    if ( wire.is_object()
	 && word_of<ref_kind>(ref_kind_names, text_field(wire, "ref")) != ref_kind::none )
    {
	ref = wire;
	return true;
    }
    result = refusal(std::string(key) + " needs a ref object: ref = record | symbol |"
		     " commit | proposal | external, plus its fields");
    return false;
}

// An optional enum argument: false, with the refusal, for a word no table
// names; `e` stays none when the argument is absent or empty.
template <typename E, size_t N>
bool word_arg(const value &args, const char *key, const char *const (&names)[N],
	      const char *what, E &e, value &result)
{
    const std::string w = text_field(args, key);
    e = (E)0;
    if ( w.empty() )
	return true;
    e = word_of<E>(names, w);
    if ( e != (E)0 )
	return true;
    result = refusal("unknown " + std::string(what) + " '" + w + "'");
    return false;
}

value proposal_row(const value &p)
{
    const value &ops = object_field(p, "ops");
    const value first = items(ops).empty() ? obj() : items(ops)[0];
    const value &tgt = object_field(first, "target");
    fields row;
    row["seq"] = object_field(p, "seq");
    row["status"] = object_field(p, "status");
    row["actor"] = object_field(p, "actor");
    row["base_seq"] = object_field(p, "base_seq");
    row["ts"] = object_field(p, "ts");
    row["ops"] = value((int64_t)items(ops).size());
    row["verb"] = object_field(first, "verb");
    row["kind"] = object_field(tgt, "kind");
    row["name"] = object_field(tgt, "name");
    return value::make_object(row);
}

// The open proposal `seq` of the served document; false, with the
// refusal, when there is none or it is decided.
bool open_proposal(const seat &s, const context &c, int64_t seq, value &p, value &result)
{
    proposal_fold f = fold_proposals(s.m.records(), served_path(c));
    std::map<int64_t, value>::const_iterator it = f.props.find(seq);
    if ( it == f.props.end() )
    {
	result = refusal("no proposal " + key_of(seq));
	return false;
    }
    if ( status_of(it->second) != status::open )
    {
	result = refusal("proposal " + key_of(seq) + " is " + text_field(it->second, "status"));
	return false;
    }
    p = it->second;
    return true;
}

int64_t decide(const seat &s, const context &c, int64_t seq, status st,
	       int64_t landed, const std::string &reason)
{
    fields d;
    d["kind"] = value(name_of(kind_names, kind::decision));
    d["actor"] = value(c.actor);
    d["proposal"] = value(seq);
    d["decision"] = value(name_of(status_names, st));
    d["landed_seq"] = value(landed);
    d["reason"] = value(reason);
    return s.m.append(value::make_object(d), c.doc);
}

bool run_tests(const seat &s, const context &c, const std::vector<int64_t> &ids,
	       int64_t proposal, value &out);

// A proposal's CHECKS (design §3.10): the tests linked `tests` to every
// op's target, run and recorded under the proposal; none linked names the
// mad family's suite as the fallback, never run inside an accept.
// Informing, never blocking.
value proposal_checks(const seat &s, const context &c, const value &ops, int64_t seq)
{
    const nexus_fold f = fold_nexus(s.m.records());
    std::vector<int64_t> ids;
    std::set<int64_t> seen;
    bool fallback = false;
    for ( const value &op : items(ops) )
    {
	const std::string key = ref_key(symbol_ref(c, object_field(op, "target")));
	bool linked = false;
	for ( const value &l : f.links )
	{
	    if ( word_of<rel>(rel_names, text_field(l, "rel")) != rel::tests
		 || text_field(l, "to_key") != key )
		continue;
	    const value &from = object_field(l, "from");
	    std::map<int64_t, value>::const_iterator r = f.records.find(int_field(from, "id"));
	    if ( r == f.records.end()
		 || word_of<rkind>(rkind_names, text_field(r->second, "rkind")) != rkind::test )
		continue;
	    linked = true;
	    if ( seen.insert(r->first).second )
		ids.push_back(r->first);
	}
	fallback = fallback || !linked;
    }
    fields out;
    out["fallback"] = value(fallback);
    if ( ids.empty() )
    {
	out["ran"] = value((int64_t)0);
	out["passed"] = value((int64_t)0);
	out["failed"] = value((int64_t)0);
	return value::make_object(out);
    }
    value run;
    if ( !run_tests(s, c, ids, seq, run) )
    {
	out["ran"] = value((int64_t)0);
	out["error"] = object_field(run, "error");
	return value::make_object(out);
    }
    out["ran"] = object_field(run, "ran");
    out["passed"] = object_field(run, "passed");
    out["failed"] = object_field(run, "failed");
    out["rows"] = object_field(run, "rows");
    return value::make_object(out);
}

// ---- graph.* over proposals, and graph.history ------------------------

bool t_proposals(const seat &s, const context &c, const value &args, value &out)
{
    status want;
    if ( !word_arg(args, "status", status_names, "status", want, out) )
	return false;
    proposal_fold f = fold_proposals(s.m.records(), served_path(c));
    std::vector<value> rows;
    for ( int64_t seq : f.seqs )
    {
	const value &p = f.props[seq];
	if ( want == status::none || status_of(p) == want )
	    rows.push_back(proposal_row(p));
    }
    fields r;
    r["rows"] = value::make_array(rows);
    out = value::make_object(r);
    return true;
}

bool t_proposal(const seat &s, const context &c, const value &args, value &out)
{
    proposal_fold f = fold_proposals(s.m.records(), served_path(c));
    std::map<int64_t, value>::const_iterator it = f.props.find(int_field(args, "seq"));
    if ( it == f.props.end() )
    {
	out = refusal("no proposal " + key_of(int_field(args, "seq")));
	return false;
    }
    out = it->second;
    return true;
}

// graph.accept: land an OPEN proposal after the re-checks per op (design
// §3.4) — the node at the target's span start has its kind and name, and
// its text is the proposal's byte-exact copy; the ops are RE-PLANNED
// against the fresh tree (a stored id is generation-bound) and land as one
// undo step through the edit core; then the decision, then the checks.
bool t_accept(const seat &s, const context &c, const value &args, value &out)
{
    const int64_t seq = int_field(args, "seq");
    value p;
    if ( !open_proposal(s, c, seq, p, out) )
	return false;
    const int64_t h = lang::document_handle(c.w, c.doc);
    const hub::text_buffer *b = c.w.text_of(c.doc);
    if ( !h || !b )
    {
	out = refusal("the compiler does not parse the served document");
	return false;
    }
    const std::string text = b->text();
    const value changed = refusal("proposal target changed since seq " + key_of(seq)
				  + "; re-propose against fresh ids");
    std::vector<value> fresh;
    for ( const value &op : items(object_field(p, "ops")) )
    {
	const value &tgt = object_field(op, "target");
	const value &span = object_field(tgt, "span");
	value node;
	madc_graph_at(&node, h, int_field(span, "line"), int_field(span, "column"));
	value nsp;
	size_t a = 0, e = 0;
	if ( !field(node, "id") || text_field(node, "kind") != text_field(tgt, "kind")
	     || text_field(node, "name") != text_field(tgt, "name") )
	{
	    out = changed;
	    return false;
	}
	madc_graph_span(&nsp, h, int_field(node, "id"));
	if ( !field(nsp, "span") || !tools::span_extent(*b, object_field(nsp, "span"), a, e)
	     || text.compare(a, e - a, text_field(op, "text")) != 0 )
	{
	    out = changed;
	    return false;
	}
	fields again;
	again["id"] = object_field(node, "id");
	again["src"] = object_field(op, "src");
	again["where"] = object_field(op, "where");
	value planned;
	std::string why;
	if ( !tools::plan_op(c, text_field(op, "verb"), value::make_object(again), planned, why) )
	{
	    out = refusal(why);
	    return false;
	}
	fresh.push_back(planned);
    }
    const int64_t landed_seq = s.m.head() + 1;	// the first node-op the landing records
    value landed;
    if ( !tools::land_ops(s.r, c, value::make_array(fresh), seq, landed) )
    {
	out = landed;
	return false;
    }
    decide(s, c, seq, status::accepted, landed_seq, std::string());
    const value checks = proposal_checks(s, c, object_field(p, "ops"), seq);
    fields r;
    r["ok"] = value(true);
    r["proposal"] = value(seq);
    r["node"] = object_field(landed, "node");
    r["seq"] = value(s.m.head());
    r["landed_seq"] = value(landed_seq);
    r["checks"] = checks;
    out = value::make_object(r);
    return true;
}

bool t_decide(const seat &s, const context &c, const value &args, status st, value &out)
{
    const int64_t seq = int_field(args, "seq");
    value p;
    if ( !open_proposal(s, c, seq, p, out) )
	return false;
    // The proposer tier withdraws only its own proposals.
    if ( st == status::withdrawn && c.caller == tier::proposer
	 && int_field(p, "actor") != c.actor )
    {
	out = refusal("proposal " + key_of(seq) + " is not yours (actor "
		      + key_of(int_field(p, "actor")) + ")");
	return false;
    }
    decide(s, c, seq, st, 0, text_field(args, "reason"));
    fields r;
    r["ok"] = value(true);
    r["proposal"] = value(seq);
    r["status"] = value(name_of(status_names, st));
    out = value::make_object(r);
    return true;
}

bool t_reject(const seat &s, const context &c, const value &args, value &out)
{
    return t_decide(s, c, args, status::rejected, out);
}

bool t_withdraw(const seat &s, const context &c, const value &args, value &out)
{
    return t_decide(s, c, args, status::withdrawn, out);
}

bool t_history(const seat &s, const context &c, const value &args, value &out)
{
    const int64_t h = lang::document_handle(c.w, c.doc);
    if ( !h )
    {
	out = refusal("the compiler does not parse the served document");
	return false;
    }
    return node_history(s, c, h, int_field(args, "id"), out);
}

// ---- nexus.* ----------------------------------------------------------

bool t_record(const seat &s, const context &c, const value &args, value &out)
{
    (void)c;
    const nexus_fold f = fold_nexus(s.m.records());
    std::map<int64_t, value>::const_iterator r = f.records.find(int_field(args, "id"));
    if ( r == f.records.end() )
    {
	out = refusal("no record " + key_of(int_field(args, "id")));
	return false;
    }
    out = r->second;
    return true;
}

// Without a state: the live records (a superseded or rejected one answers
// only when asked for).
bool t_records(const seat &s, const context &c, const value &args, value &out)
{
    (void)c;
    rkind rk;
    rstate st;
    if ( !word_arg(args, "rkind", rkind_names, "record kind", rk, out)
	 || !word_arg(args, "state", rstate_names, "record state", st, out) )
	return false;
    const nexus_fold f = fold_nexus(s.m.records());
    std::vector<value> rows;
    for ( int64_t id : f.ids )
    {
	const value &r = f.records.find(id)->second;
	if ( rk != rkind::none && word_of<rkind>(rkind_names, text_field(r, "rkind")) != rk )
	    continue;
	const rstate have = word_of<rstate>(rstate_names, text_field(r, "state"));
	if ( st == rstate::none ? !live_state(have) : have != st )
	    continue;
	rows.push_back(r);
    }
    fields res;
    res["rows"] = value::make_array(rows);
    out = value::make_object(res);
    return true;
}

int64_t record_write(const seat &s, const context &c, rop op, fields rec)
{
    rec["kind"] = value(name_of(kind_names, kind::record));
    rec["op"] = value(name_of(rop_names, op));
    rec["actor"] = value(c.actor);
    return s.m.append(value::make_object(rec), c.doc);
}

bool t_create(const seat &s, const context &c, const value &args, value &out)
{
    rkind rk;
    rstate st;
    if ( !word_arg(args, "rkind", rkind_names, "record kind", rk, out)
	 || !word_arg(args, "state", rstate_names, "record state", st, out) )
	return false;
    if ( rk == rkind::none )
    {
	out = refusal("unknown record kind '' (task | requirement | decision | workset"
		      " | milestone | test)");
	return false;
    }
    fields rec;
    rec["rkind"] = value(name_of(rkind_names, rk));
    rec["state"] = value(name_of(rstate_names, st == rstate::none ? rstate::open : st));
    rec["origin"] = value("");
    rec["ext_key"] = value("");
    rec["fields"] = object_field(args, "fields").is_object() ? object_field(args, "fields") : obj();
    fields r;
    r["ok"] = value(true);
    r["id"] = value(record_write(s, c, rop::create, rec));
    out = value::make_object(r);
    return true;
}

bool update_record(const seat &s, const context &c, const value &args, rop op, value &out)
{
    rstate st;
    if ( !word_arg(args, "state", rstate_names, "record state", st, out) )
	return false;
    const int64_t id = int_field(args, "id");
    const nexus_fold f = fold_nexus(s.m.records());
    if ( !f.records.count(id) )
    {
	out = refusal("no record " + key_of(id));
	return false;
    }
    if ( op == rop::close && st == rstate::none )
	st = rstate::done;
    fields rec;
    rec["id"] = value(id);
    rec["fields"] = object_field(args, "fields");
    if ( st != rstate::none )
	rec["state"] = value(name_of(rstate_names, st));
    fields r;
    r["ok"] = value(true);
    r["id"] = value(id);
    r["seq"] = value(record_write(s, c, op, rec));
    out = value::make_object(r);
    return true;
}

bool t_update(const seat &s, const context &c, const value &args, value &out)
{
    return update_record(s, c, args, rop::update, out);
}

bool t_close(const seat &s, const context &c, const value &args, value &out)
{
    return update_record(s, c, args, rop::close, out);
}

bool link_write(const seat &s, const context &c, const value &args, lop op, value &out)
{
    value from, to;
    rel r;
    if ( !ref_arg(args, "from", from, out) || !ref_arg(args, "to", to, out)
	 || !word_arg(args, "rel", rel_names, "relation", r, out) )
	return false;
    if ( r == rel::none )
    {
	out = refusal("unknown relation ''");
	return false;
    }
    fields rec;
    rec["kind"] = value(name_of(kind_names, kind::link));
    rec["op"] = value(name_of(lop_names, op));
    rec["actor"] = value(c.actor);
    rec["from"] = from;
    rec["rel"] = value(name_of(rel_names, r));
    rec["to"] = to;
    fields res;
    res["ok"] = value(true);
    res["seq"] = value(s.m.append(value::make_object(rec), c.doc));
    out = value::make_object(res);
    return true;
}

bool t_link(const seat &s, const context &c, const value &args, value &out)
{
    return link_write(s, c, args, lop::link, out);
}

bool t_unlink(const seat &s, const context &c, const value &args, value &out)
{
    return link_write(s, c, args, lop::unlink, out);
}

// nexus.context(ref): everything linked to a ref, provenance-tagged — its
// links and records; for a symbol, its node (compiler), its history rows
// and the proposals whose target it is.
value context_of(const seat &s, const context &c, int64_t h, const value &ref)
{
    const nexus_fold f = fold_nexus(s.m.records());
    const std::string key = ref_key(ref);
    std::vector<value> rows;
    link_rows(f, key, rows);
    if ( word_of<ref_kind>(ref_kind_names, text_field(ref, "ref")) == ref_kind::symbol )
    {
	value node;
	if ( symbol_node(c, h, ref, node) )
	{
	    fields nrow;
	    nrow["provenance"] = value(name_of(provenance_names, provenance::compiler));
	    nrow["node"] = node;
	    rows.push_back(value::make_object(nrow));
	    value hist;
	    if ( node_history(s, c, h, int_field(node, "id"), hist) )
		for ( const value &hr : items(object_field(hist, "rows")) )
		    rows.push_back(hr);
	}
	proposal_fold pf = fold_proposals(s.m.records(), served_path(c));
	for ( int64_t seq : pf.seqs )
	{
	    const value &p = pf.props[seq];
	    for ( const value &op : items(object_field(p, "ops")) )
	    {
		const value &tgt = object_field(op, "target");
		if ( text_field(tgt, "kind") == text_field(ref, "kind")
		     && text_field(tgt, "name") == text_field(ref, "name") )
		{
		    fields prow;
		    prow["provenance"] = value(name_of(provenance_names, provenance::record));
		    prow["proposal"] = object_field(p, "seq");
		    prow["status"] = object_field(p, "status");
		    prow["verb"] = object_field(op, "verb");
		    rows.push_back(value::make_object(prow));
		    break;
		}
	    }
	}
    }
    fields out;
    out["ref"] = ref;
    out["key"] = value(key);
    out["rows"] = value::make_array(rows);
    return value::make_object(out);
}

bool t_context(const seat &s, const context &c, const value &args, value &out)
{
    value ref;
    if ( !ref_arg(args, "ref", ref, out) )
	return false;
    out = context_of(s, c, lang::document_handle(c.w, c.doc), ref);
    return true;
}

// nexus.explain(ref, detail): ONE projection over the existing verbs
// (design §4.5) — what, depends, history, why, plan, constraints, layers.
bool t_explain(const seat &s, const context &c, const value &args, value &out)
{
    value ref;
    explain_level level;
    if ( !ref_arg(args, "ref", ref, out)
	 || !word_arg(args, "detail", explain_names, "detail", level, out) )
	return false;
    if ( word_of<ref_kind>(ref_kind_names, text_field(ref, "ref")) != ref_kind::symbol )
    {
	out = refusal("explain takes a symbol ref {ref: symbol, file, kind, name} (v1)");
	return false;
    }
    const int64_t h = lang::document_handle(c.w, c.doc);
    value node;
    if ( !symbol_node(c, h, ref, node) )
    {
	out = refusal("no " + text_field(ref, "kind") + " named " + text_field(ref, "name")
		      + " in the served document");
	return false;
    }
    const int64_t id = int_field(node, "id");
    const bool full = level == explain_level::full;
    value sp;
    madc_graph_span(&sp, h, id);
    fields what;
    what["provenance"] = value(name_of(provenance_names, provenance::compiler));
    what["node"] = node;
    what["span"] = object_field(sp, "span");
    if ( full )
    {
	value ty;
	madc_graph_type_of(&ty, h, id);
	what["type"] = ty;
    }
    // The call edges exist for a function only: the reads answer {nodes,
    // edges} for one and an error otherwise — ask them, never a kind word.
    fields depends;
    depends["provenance"] = value(name_of(provenance_names, provenance::compiler));
    value ce;
    madc_graph_callees(&ce, h, id);
    if ( object_field(ce, "edges").is_array() )
    {
	value cr, im;
	madc_graph_callers(&cr, h, id);
	madc_graph_impact(&im, h, id);
	depends["callees"] = value((int64_t)items(object_field(ce, "edges")).size());
	depends["callers"] = value((int64_t)items(object_field(cr, "edges")).size());
	depends["impact"] = im;
    }
    value hist;
    node_history(s, c, h, id, hist);
    std::vector<value> hrows = items(object_field(hist, "rows"));
    if ( !full && hrows.size() > 10 )
	hrows.resize(10);
    fields history;
    history["rows"] = value::make_array(hrows);
    history["complete"] = object_field(hist, "complete");
    // The record axis: why (decisions, requirements, the proposals that
    // touched it), plan (live tasks and milestones), constraints.
    const nexus_fold f = fold_nexus(s.m.records());
    std::vector<value> lrows, why, plan;
    link_rows(f, ref_key(ref), lrows);
    for ( const value &lr : lrows )
    {
	const value &r = object_field(lr, "record");
	if ( !r.is_object() )
	    continue;
	const rkind rk = word_of<rkind>(rkind_names, text_field(r, "rkind"));
	const rstate st = word_of<rstate>(rstate_names, text_field(r, "state"));
	const rel rl = word_of<rel>(rel_names, text_field(lr, "rel"));
	if ( rk == rkind::decision || rk == rkind::requirement )
	    why.push_back(lr);
	const bool live = st != rstate::done && live_state(st);
	const bool planned = rl == rel::planned_for || rl == rel::requested_by
	    || rl == rel::implements;
	if ( (rk == rkind::task || rk == rkind::milestone) && live && planned )
	    plan.push_back(lr);
    }
    const value ctx = context_of(s, c, h, ref);
    for ( const value &cr : items(object_field(ctx, "rows")) )
	if ( text_field(cr, "provenance") == name_of(provenance_names, provenance::record)
	     && field(cr, "proposal") )
	    why.push_back(cr);
    fields r;
    r["ref"] = ref;
    r["what"] = value::make_object(what);
    r["depends"] = value::make_object(depends);
    r["history"] = value::make_object(history);
    r["why"] = value::make_array(why);
    r["plan"] = value::make_array(plan);
    r["constraints"] = constraints_of(c, f, node);
    r["layers"] = layer_words(asset_layers(s, c, true));
    out = value::make_object(r);
    return true;
}

// ---- test.* -----------------------------------------------------------

// One origin's records by ext_key (the first record of a key wins).
std::map<std::string, int64_t> origin_index(const nexus_fold &f, const std::string &origin)
{
    std::map<std::string, int64_t> index;
    for ( int64_t id : f.ids )
    {
	const value &r = f.records.find(id)->second;
	if ( text_field(r, "origin") == origin )
	    index.insert(std::make_pair(text_field(r, "ext_key"), id));
    }
    return index;
}

// Upsert one record by (origin, ext_key) through `index` — that origin's
// records, kept current: true when created. An update merges the fields
// and sets `st` (none: unchanged); a new record starts open unless `st`.
bool upsert_record(const seat &s, const context &c, std::map<std::string, int64_t> &index,
		   rkind rk, const std::string &origin, const std::string &ext_key,
		   const fields &flds, rstate st)
{
    fields rec;
    rec["fields"] = value::make_object(flds);
    std::map<std::string, int64_t>::const_iterator it = index.find(ext_key);
    if ( it != index.end() )
    {
	rec["id"] = value(it->second);
	if ( st != rstate::none )
	    rec["state"] = value(name_of(rstate_names, st));
	record_write(s, c, rop::update, rec);
	return false;
    }
    rec["rkind"] = value(name_of(rkind_names, rk));
    rec["state"] = value(name_of(rstate_names, st == rstate::none ? rstate::open : st));
    rec["origin"] = value(origin);
    rec["ext_key"] = value(ext_key);
    index[ext_key] = record_write(s, c, rop::create, rec);
    return true;
}

// test.discover(dir?): the mad family (tests/<name>.mad; a .helper sibling
// marks a unit another test owns — skipped, the runner's rule) and the unit
// family (tests/unit/*.cpp), by the canonical runner's conventions.
bool t_discover(const seat &s, const context &c, const value &args, value &out)
{
    std::string dir = text_field(args, "dir");
    if ( dir.empty() )
	dir = "tests";
    int64_t created = 0, seen = 0;
    std::map<std::string, int64_t> index = origin_index(fold_nexus(s.m.records()), "convention");
    const struct { const char *glob; family fam; } families[] = {
	{ "/*.mad", family::mad }, { "/unit/*.cpp", family::unit }
    };
    for ( const auto &fam : families )
    {
	std::vector<std::string> paths;
	detail::glob_paths(dir + fam.glob, paths);
	std::sort(paths.begin(), paths.end());
	for ( const std::string &p : paths )
	{
	    if ( fam.fam == family::mad
		 && detail::host_path_type_of(detail::host_path_drop_extension(p) + ".helper")
		    == detail::host_path_type::file )
		continue;
	    ++seen;
	    fields flds;
	    flds["name"] = value(detail::host_path_stem(p));
	    flds["family"] = value(name_of(family_names, fam.fam));
	    flds["asset"] = value(p);
	    flds["command"] = value("");
	    if ( upsert_record(s, c, index, rkind::test, "convention", p, flds, rstate::none) )
		++created;
	}
    }
    fields r;
    r["ok"] = value(true);
    r["directory"] = value(dir);
    r["discovered"] = value(seen);
    r["created"] = value(created);
    r["updated"] = value(seen - created);
    out = value::make_object(r);
    return true;
}

// The testrun events passing the filters (test 0 = any, seq > since, node
// < 0 = any), oldest first, and the latest per test.
value testrun_fold(const seat &s, int64_t test, int64_t since, int64_t node)
{
    std::vector<value> rows;
    fields latest;
    for ( const value &rec : s.m.records() )
    {
	if ( kind_of(rec) != kind::testrun
	     || (test != 0 && int_field(rec, "test") != test)
	     || int_field(rec, "seq") <= since
	     || (node >= 0 && int_field(rec, "node") != node) )
	    continue;
	rows.push_back(rec);
	latest[key_of(int_field(rec, "test"))] = rec;
    }
    fields out;
    out["rows"] = value::make_array(rows);
    out["latest"] = value::make_object(latest);
    return value::make_object(out);
}

bool t_list(const seat &s, const context &c, const value &args, value &out)
{
    (void)c;
    family fam;
    if ( !word_arg(args, "family", family_names, "family", fam, out) )
	return false;
    const value runs = testrun_fold(s, 0, 0, -1);
    std::vector<value> rows;
    for ( const value &r : test_records(fold_nexus(s.m.records()), fam) )
    {
	const value &flds = object_field(r, "fields");
	fields row;
	row["id"] = object_field(r, "id");
	row["name"] = object_field(flds, "name");
	row["family"] = object_field(flds, "family");
	row["asset"] = object_field(flds, "asset");
	row["state"] = object_field(r, "state");
	row["origin"] = object_field(r, "origin");
	row["last"] = object_field(object_field(runs, "latest"), key_of(int_field(r, "id")).c_str());
	rows.push_back(value::make_object(row));
    }
    fields res;
    res["rows"] = value::make_array(rows);
    out = value::make_object(res);
    return true;
}

// test.candidates(ref): the test records linked `tests` to the ref; none
// linked: the mad family's whole suite NAMED as the fallback, never run.
bool t_candidates(const seat &s, const context &c, const value &args, value &out)
{
    (void)c;
    value ref;
    if ( !ref_arg(args, "ref", ref, out) )
	return false;
    const nexus_fold f = fold_nexus(s.m.records());
    const std::string key = ref_key(ref);
    std::vector<value> tests;
    std::set<int64_t> seen;
    for ( const value &l : f.links )
    {
	if ( word_of<rel>(rel_names, text_field(l, "rel")) != rel::tests
	     || text_field(l, "to_key") != key )
	    continue;
	std::map<int64_t, value>::const_iterator r
	    = f.records.find(int_field(object_field(l, "from"), "id"));
	if ( r == f.records.end()
	     || word_of<rkind>(rkind_names, text_field(r->second, "rkind")) != rkind::test
	     || !seen.insert(r->first).second )
	    continue;
	tests.push_back(r->second);
    }
    fields res;
    res["ref"] = ref;
    res["tests"] = value::make_array(tests);
    res["fallback"] = value(tests.empty());
    if ( tests.empty() )
    {
	fields suite;
	suite["family"] = value(name_of(family_names, family::mad));
	suite["count"] = value((int64_t)test_records(f, family::mad).size());
	res["suite"] = value::make_object(suite);
    }
    out = value::make_object(res);
    return true;
}

bool t_results(const seat &s, const context &c, const value &args, value &out)
{
    (void)c;
    out = testrun_fold(s, int_field(args, "test"), int_field(args, "since"),
		       int_field(args, "node", -1));
    return true;
}

// Every stdout line of a child until it drains, under an IDLE deadline: no
// line within idle_ms cancels it and answers false.
bool child_lines(channel &ch, int64_t idle_ms, std::vector<std::string> &lines)
{
    std::string line;
    for ( ;; )
    {
	const int got = next_line(ch, idle_ms, line);
	if ( got < 0 )
	    return true;			// drained
	if ( got == 0 )
	{
	    ch.cancel();
	    return false;
	}
	lines.push_back(line);
    }
}

// Is the served document's text what its file holds? The runner tests the
// DISK; a run against unsaved text is labelled, never blocked.
bool synced_to_disk(const context &c)
{
    std::string disk;
    const hub::text_buffer *b = c.w.text_of(c.doc);
    return b && document_read_file(served_path(c), disk) && disk == b->text();
}

// One testrun event and its answer row.
value verdict(const seat &s, const context &c, const value &r, result_word res,
	      int64_t exit, const value &seconds, const std::string &detail_text,
	      int64_t proposal, bool synced)
{
    const value &flds = object_field(r, "fields");
    fields rec;
    rec["kind"] = value(name_of(kind_names, kind::testrun));
    rec["actor"] = value(c.actor);
    rec["test"] = object_field(r, "id");
    rec["name"] = object_field(flds, "name");
    rec["family"] = object_field(flds, "family");
    rec["node"] = value((int64_t)0);
    rec["result"] = value(name_of(result_names, res));
    rec["exit"] = value(exit);
    rec["seconds"] = seconds;
    rec["detail"] = value(detail_text);
    rec["output_ref"] = value("");
    rec["synced"] = value(synced);
    if ( proposal > 0 )
	rec["proposal"] = value(proposal);
    fields row;
    row["seq"] = value(s.m.append(value::make_object(rec), c.doc));
    row["test"] = object_field(r, "id");
    row["name"] = object_field(flds, "name");
    row["result"] = value(name_of(result_names, res));
    row["exit"] = value(exit);
    row["seconds"] = seconds;
    row["detail"] = value(detail_text);
    return value::make_object(row);
}

// An external record's command with its placeholders: {path} the served
// file, {base} it without its extension, {project} the manifest, {madc} the
// running compiler.
std::string command_of(const seat &s, const context &c, const std::string &cmd)
{
    const std::string path = served_path(c);
    const struct { const char *key; std::string text; } subst[] = {
	{ "{path}", path },
	{ "{base}", detail::host_path_drop_extension(path) },
	{ "{project}", s.m.manifest() },
	{ "{madc}", madc_self_exe_path() },
    };
    std::string out = cmd;
    for ( const auto &sub : subst )
	for ( size_t at = out.find(sub.key); at != std::string::npos;
	      at = out.find(sub.key, at + sub.text.size()) )
	    out.replace(at, strlen(sub.key), sub.text);
    return out;
}

// test.run's body: the mad records grouped by DIRECTORY — one canonical
// runner spawn per directory (--report=json, read as data; MADC_BIN = the
// running madc) — and an external record's command (exit 0 = pass). A
// family with no runner here refuses the WHOLE call: nothing half-runs.
bool run_tests(const seat &s, const context &c, const std::vector<int64_t> &ids,
	       int64_t proposal, value &out)
{
    const nexus_fold f = fold_nexus(s.m.records());
    const std::string runner = runner_of(family::mad);
    std::vector<std::string> dirs;
    std::map<std::string, std::vector<value> > groups;
    std::vector<value> externals;
    for ( int64_t id : ids )
    {
	std::map<int64_t, value>::const_iterator r = f.records.find(id);
	if ( r == f.records.end()
	     || word_of<rkind>(rkind_names, text_field(r->second, "rkind")) != rkind::test )
	{
	    out = refusal("no test record " + key_of(id));
	    return false;
	}
	const value &flds = object_field(r->second, "fields");
	const family fam = word_of<family>(family_names, text_field(flds, "family"));
	if ( fam == family::mad && !runner.empty() )
	{
	    std::string dir = detail::host_path_dirname(text_field(flds, "asset"));
	    if ( dir.empty() )
		dir = ".";
	    else if ( dir.size() > 1 )
		dir.erase(dir.size() - 1);		// the separator dirname keeps
	    if ( !groups.count(dir) )
		dirs.push_back(dir);
	    groups[dir].push_back(r->second);
	}
	else if ( fam == family::external && !text_field(flds, "command").empty() )
	    externals.push_back(r->second);
	else
	{
	    out = refusal("no runner for family " + text_field(flds, "family")
			  + " on this node (the mad family's is scripts/run_tests.sh under"
			  " the project root, POSIX; an external record names its command)");
	    return false;
	}
    }
    const bool synced = synced_to_disk(c);
    const std::string self = madc_self_exe_path();
    std::vector<value> rows;
    int64_t passed = 0, failed = 0;
    auto tally = [&](const value &row) {
	rows.push_back(row);
	(text_field(row, "result") == name_of(result_names, result_word::pass) ? passed : failed)++;
    };
    for ( const std::string &dir : dirs )
    {
	std::string bases;
	for ( const value &r : groups[dir] )
	    bases += " " + detail::host_path_stem(text_field(object_field(r, "fields"), "asset"));
	const std::string uri = "exec://env MADC_BIN=" + self + " MADC_TEST_DIR=" + dir
	    + " bash " + runner + " --report=json" + bases;
	channel ch(uri.c_str());
	if ( !ch.ok() )
	{
	    out = refusal(std::string("cannot spawn the runner: ") + ch.last_error());
	    return false;
	}
	ch.close_write();
	std::vector<std::string> lines;
	const bool drained = child_lines(ch, 120000, lines);
	ch.close();
	const int64_t status_code = ch.exit_status();
	std::vector<value> reports;
	for ( const std::string &ln : lines )
	{
	    value rep;
	    if ( decode(ln, rep) && field(rep, "test") && !field(rep, "lane") )
		reports.push_back(rep);		// the verdict rows, not a lane's
	}
	for ( const value &r : groups[dir] )
	{
	    const std::string base = detail::host_path_stem(text_field(object_field(r, "fields"), "asset"));
	    result_word res = result_word::error;
	    int64_t exit = status_code;
	    value seconds((int64_t)0);
	    std::string text = drained ? "no report line from the runner"
				       : "the runner went silent (cancelled)";
	    for ( const value &rep : reports )
		if ( text_field(rep, "test") == base )
		{
		    res = word_of<result_word>(result_names, text_field(rep, "result"));
		    if ( res == result_word::none )
			res = result_word::error;
		    exit = int_field(rep, "exit", exit);
		    if ( field(rep, "seconds") )
			seconds = object_field(rep, "seconds");
		    text = text_field(rep, res == result_word::skip ? "reason" : "detail");
		    break;
		}
	    tally(verdict(s, c, r, res, exit, seconds, text, proposal, synced));
	}
    }
    for ( const value &r : externals )
    {
	const std::string cmd = command_of(s, c, text_field(object_field(r, "fields"), "command"));
	channel ch(("exec://" + cmd).c_str());
	result_word res = result_word::error;
	int64_t exit = -1;
	std::string text;
	if ( !ch.ok() )
	    text = ch.last_error();
	else
	{
	    ch.close_write();
	    std::vector<std::string> lines;
	    const bool drained = child_lines(ch, 60000, lines);
	    ch.close();
	    exit = ch.exit_status();
	    if ( !drained )
	    {
		res = result_word::timeout;
		text = "silent past the idle deadline (cancelled)";
	    }
	    else if ( exit == 0 )
		res = result_word::pass;
	    else
	    {
		res = result_word::fail;
		text = "exit " + key_of(exit);
	    }
	}
	tally(verdict(s, c, r, res, exit, value((int64_t)0), text, proposal, synced));
    }
    fields r;
    r["ok"] = value(true);
    r["node"] = value((int64_t)0);
    r["ran"] = value((int64_t)rows.size());
    r["passed"] = value(passed);
    r["failed"] = value(failed);
    r["synced"] = value(synced);
    r["rows"] = value::make_array(rows);
    out = value::make_object(r);
    return true;
}

// This node's offers (madcide's node_offers, design §3.11): node 0, its
// platform, its toolchains and what it serves.
value node_offers()
{
    fields f;
    f["node"] = value((int64_t)0);
    f["platform"] = value(sys.platform);
    f["toolchains"] = value::make_array(std::vector<value>(1, value("madc")));
    std::vector<value> serves;
    for ( const char *w : { "build", "run", "test" } )
	serves.push_back(value(w));
    f["serves"] = value::make_array(serves);
    return value::make_object(f);
}

// test.run(ids, target?, proposal?): `target` null, "", 0 or this node's
// platform is here; any other names a node no one offers — refused.
bool t_run(const seat &s, const context &c, const value &args, value &out)
{
    const value &target = object_field(args, "target");
    const bool here = target.is_null()
	|| (target.is_integer() && target.as_integer() == 0)
	|| (target.is_string() && (target.as_string().empty()
				   || target.as_string() == std::string(sys.platform)));
    if ( !here )
    {
	out = refusal("no node offers '" + (target.is_string() ? target.as_string()
					    : encode(target)) + "' (this node: "
		      + encode(node_offers()) + ")");
	return false;
    }
    std::vector<int64_t> ids;
    for ( const value &v : items(object_field(args, "ids")) )
	if ( v.is_integer() )
	    ids.push_back(v.as_integer());
    return run_tests(s, c, ids, int_field(args, "proposal"), out);
}

// graph.status: the served asset in one call — its parse (handle,
// generation, error count), the stream's head, its open revisions, its git
// state (with the version history; null without), its layers and this
// node's offers.
bool t_status(const seat &s, const context &c, const value &args, value &out)
{
    (void)args;
    const int64_t h = lang::document_handle(c.w, c.doc);
    std::vector<value> revs;
    for ( const tools::revisions::entry &e : s.r.past().of(c.doc) )
    {
	fields row;
	row["sha"] = value(e.sha);
	row["tag"] = value(e.tag);
	revs.push_back(value::make_object(row));
    }
    value git;
    if ( s.r.history() && !s.r.history()->state(served_path(c), git) )
	git = value();				// not tracked: null
    fields r;
    r["handle"] = value(h);
    r["generation"] = value(h ? madc_parse_generation(h) : (int64_t)-1);
    r["synced"] = value(true);			// document_handle parses the current text
    r["errors"] = value(h ? madc_parse_error_count(h) : (int64_t)0);
    r["log"] = value(s.m.bound_path());		// "" = memory-only
    r["log_head"] = value(s.m.head());
    r["revisions"] = value::make_array(revs);
    r["git"] = git;
    r["layers"] = layer_words(asset_layers(s, c, true));
    r["node"] = node_offers();
    out = value::make_object(r);
    return true;
}

// ---- nexus.sources / nexus.sync: an external system's MCP server -----

const int64_t mcp_reply_ms = 5000;	// initialize, tools/list
const int64_t mcp_call_ms = 10 * 1000;	// the list_tasks call

// One manifest read into `src`: false with `why` (the file's refusal).
bool read_source(const std::string &file, project::source &src, std::string &why)
{
    std::string text;
    value m;
    if ( !document_read_file(file, text) )
	why = "unreadable";
    else if ( !decode(text, m) || !m.is_object() )
	why = "not a JSON object";
    if ( !why.empty() )
	return false;
    src.name = text_field(m, "name");
    src.file = file;
    const value &tw = object_field(m, "transport");
    const transport tr = tw.is_null() ? transport::stdio
	: word_of<transport>(transport_names, text_field(m, "transport"));
    const value &cmd = object_field(m, "command");
    if ( src.name.empty() )
	why = "no name";
    else if ( tr == transport::none )
	why = "unknown transport " + encode(tw) + " (stdio | http)";
    else if ( tr != transport::stdio )
	why = "transport '" + text_field(m, "transport") + "' is not served yet (stdio only;"
	    " bridge with mcp-remote)";
    else if ( !cmd.is_array() || cmd.as_array().empty() )
	why = "no command";
    if ( !why.empty() )
	return false;
    src.command.clear();
    for ( const value &word : cmd.as_array() )
	src.command += (src.command.empty() ? "" : " ")
	    + (word.is_string() ? word.as_string() : encode(word));
    const value &env = object_field(m, "env");
    if ( env.is_object() && !env.as_object().empty() )
    {
#ifdef _WIN32
	why = "env: not supported on this platform";
	return false;
#else
	std::string prefix = "env";
	for ( const auto &kv : env.as_object() )
	    prefix += " " + kv.first + "="
		+ (kv.second.is_string() ? kv.second.as_string() : encode(kv.second));
	src.command = prefix + " " + src.command;
#endif
    }
    fields ops;
    const value &vocab = object_field(m, "vocabulary");
    if ( vocab.is_object() )
	for ( const auto &kv : vocab.as_object() )
	{
	    if ( word_of<nexus_op>(nexus_op_names, kv.first) == nexus_op::none )
		why = "unknown vocabulary key '" + kv.first + "'";
	    else if ( text_field(kv.second, "tool").empty() )
		why = "vocabulary '" + kv.first + "' names no tool";
	    if ( !why.empty() )
		return false;
	    ops[kv.first] = kv.second;
	}
    src.ops = value::make_object(ops);
    return true;
}

value source_row(const project::source &src)
{
    std::vector<value> tl;
    for ( size_t op = 1; op < sizeof(nexus_op_names) / sizeof(nexus_op_names[0]); ++op )
    {
	const value &e = object_field(src.ops, nexus_op_names[op]);
	if ( !e.is_object() )
	    continue;
	fields t;
	t["op"] = value(nexus_op_names[op]);
	t["tool"] = object_field(e, "tool");
	tl.push_back(value::make_object(t));
    }
    fields r;
    r["name"] = value(src.name);
    r["transport"] = value(name_of(transport_names, transport::stdio));
    r["file"] = value(src.file);
    r["tools"] = value::make_array(tl);
    return value::make_object(r);
}

// nexus.sources(dir?): the loaded manifests; `dir` (owner tier) loads that
// directory's in their place.
bool t_sources(const seat &s, const context &c, const value &args, value &out)
{
    const std::string dir = text_field(args, "dir");
    std::string why;
    if ( !dir.empty() && c.caller < tier::owner )
    {
	out = refusal(std::string("loading a directory of sources needs the owner tier (caller: ")
		      + tools::tier_name(c.caller) + ")");
	return false;
    }
    const std::vector<project::source> *srcs = 0;
    if ( (!dir.empty() && !s.m.load_sources(dir, why)) || !s.m.sources(srcs, why) )
    {
	out = refusal(why);
	return false;
    }
    std::vector<value> rows;
    for ( const project::source &src : *srcs )
	rows.push_back(source_row(src));
    fields r;
    r["ok"] = value(true);
    r["rows"] = value::make_array(rows);
    out = value::make_object(r);
    return true;
}

// A synced row's scalar as text: a key or a state word.
std::string scalar_text(const value &v)
{
    return v.is_string() ? v.as_string() : v.is_null() ? std::string() : encode(v);
}

// nexus.sync(source): ONE child per sync (the manifest's command over
// exec://): initialize, tools/list (every vocabulary tool advertised, or
// the sync refuses), the list_tasks call; each row renamed by its map and
// upserted as a task record with origin = the source, by its key.
bool t_sync(const seat &s, const context &c, const value &args, value &out)
{
    const std::string name = text_field(args, "source");
    const std::vector<project::source> *srcs = 0;
    std::string why;
    if ( !s.m.sources(srcs, why) )
    {
	out = refusal(why);
	return false;
    }
    const project::source *src = 0;
    for ( const project::source &x : *srcs )
	if ( x.name == name )
	    src = &x;
    if ( !src )
    {
	out = refusal("no source '" + name + "' (nexus.sources lists the loaded manifests)");
	return false;
    }
    const value &lt = object_field(src->ops, name_of(nexus_op_names, nexus_op::list_tasks));
    if ( !lt.is_object() )
    {
	out = refusal(name + ": the manifest maps no list_tasks");
	return false;
    }
    channel ch(("exec://" + command_of(s, c, src->command)).c_str());
    if ( !ch.ok() )
    {
	out = refusal(name + ": cannot spawn: " + ch.last_error());
	return false;
    }
    mcp::client cl(ch);
    value res;
    auto gone = [&]() {
	ch.close();			// reaps the child: its exit status is real after it
	fields f;
	f["error"] = value(name + ": " + text_field(res, "error"));
	f["exit_status"] = value(ch.exit_status());
	f["source"] = value(name);
	out = value::make_object(f);
	return false;
    };
    if ( !cl.start(res, mcp_reply_ms) || !cl.call("tools/list", obj(), res, mcp_reply_ms) )
	return gone();
    std::set<std::string> advertised;
    for ( const value &t : items(object_field(res, "tools")) )
	advertised.insert(text_field(t, "name"));
    for ( size_t op = 1; op < sizeof(nexus_op_names) / sizeof(nexus_op_names[0]); ++op )
    {
	const std::string tool = text_field(object_field(src->ops, nexus_op_names[op]), "tool");
	if ( !tool.empty() && !advertised.count(tool) )
	{
	    ch.close();
	    out = refusal(name + ": tool '" + tool + "' (vocabulary " + nexus_op_names[op]
			  + ") is not advertised by the server");
	    return false;
	}
    }
    fields cp;
    cp["name"] = object_field(lt, "tool");
    cp["arguments"] = object_field(lt, "args").is_object() ? object_field(lt, "args") : obj();
    if ( !cl.call("tools/call", value::make_object(cp), res, mcp_call_ms) )
	return gone();
    ch.close_write();
    ch.close();
    if ( object_field(res, "isError").is_boolean() && object_field(res, "isError").as_boolean() )
    {
	const std::vector<value> &content = items(object_field(res, "content"));
	out = refusal(name + ": " + text_field(lt, "tool") + " refused: "
		      + (content.empty() ? std::string() : text_field(content[0], "text")));
	return false;
    }
    const std::vector<value> rows = mcp::client::rows_of(res);
    const value &map = object_field(lt, "map");
    std::map<std::string, int64_t> index = origin_index(fold_nexus(s.m.records()), name);
    int64_t created = 0, updated = 0, skipped = 0;
    for ( const value &row : rows )
    {
	fields flds;
	std::string ext_key;
	rstate st = rstate::none;
	if ( row.is_object() )
	    for ( const auto &kv : row.as_object() )
	    {
		const std::string to = object_field(map, kv.first.c_str()).is_string()
		    ? object_field(map, kv.first.c_str()).as_string() : kv.first;
		switch ( word_of<row_slot>(row_slot_names, to) )
		{
		case row_slot::key:
		    ext_key = scalar_text(kv.second);
		    break;
		case row_slot::state:
		    if ( word_of<rstate>(rstate_names, scalar_text(kv.second)) != rstate::none )
			st = word_of<rstate>(rstate_names, scalar_text(kv.second));
		    break;
		case row_slot::field:
		    flds[to] = kv.second;
		    break;
		}
	    }
	if ( ext_key.empty() )
	{
	    ++skipped;			// a row with no key cannot be upserted
	    continue;
	}
	(upsert_record(s, c, index, rkind::task, name, ext_key, flds, st) ? created : updated)++;
    }
    fields r;
    r["ok"] = value(true);
    r["source"] = value(name);
    r["rows"] = value((int64_t)rows.size());
    r["created"] = value(created);
    r["updated"] = value(updated);
    r["skipped"] = value(skipped);
    out = value::make_object(r);
    return true;
}

// ---- the table --------------------------------------------------------

typedef bool (*tool_fn)(const seat &, const context &, const value &, value &);

struct tool_row
{
    const char *name;
    tier least;
    int64_t layers;
    tool_fn run;
    const char *description;
};

const tool_row tool_rows[] = {
    { "graph.status", tier::observer, l_text, t_status,
      "the served asset in one call: handle, generation, error count, the log's head and"
      " file (\"\" = memory-only), open revisions, git head/branch/dirty, layers, this"
      " node's offers" },
    { "graph.history", tier::observer, l_parseable, t_history,
      "who changed this node and when: the commits that last touched it (with the"
      " version history) and the session's events (node-ops, text edits),"
      " provenance-tagged" },
    { "graph.proposals", tier::observer, l_parseable, t_proposals,
      "the served file's proposals (compact rows; optional status filter)" },
    { "graph.proposal", tier::observer, l_parseable, t_proposal,
      "one proposal in full: ops, preview, diagnostics, status, decision" },
    { "graph.accept", tier::editor, l_parseable, t_accept,
      "land an open proposal (editor tier): re-checks the target byte-exact, applies,"
      " records the decision, runs the tests linked to the target as its checks"
      " (informing, never blocking)" },
    { "graph.reject", tier::editor, l_parseable, t_reject,
      "reject an open proposal with a reason (editor tier)" },
    { "graph.withdraw", tier::proposer, l_parseable, t_withdraw,
      "withdraw an open proposal (its proposer, or an editor)" },
    { "nexus.record", tier::observer, l_managed, t_record,
      "one intent record by id (task / requirement / decision / workset / milestone"
      " / test)" },
    { "nexus.records", tier::observer, l_managed, t_records,
      "the records (optional kind and state filters; without a state: the live ones"
      " — superseded / rejected answer only when asked)" },
    { "nexus.create", tier::proposer, l_managed, t_create,
      "create a record (proposer tier): {rkind, fields, state?} -> its id" },
    { "nexus.update", tier::proposer, l_managed, t_update,
      "merge fields into a record and/or set its state (proposer tier)" },
    { "nexus.close", tier::proposer, l_managed, t_close,
      "close a record: state done (default), superseded or rejected (proposer tier)" },
    { "nexus.link", tier::proposer, l_managed, t_link,
      "link two refs by a relation (proposer tier): from --rel--> to" },
    { "nexus.unlink", tier::proposer, l_managed, t_unlink,
      "remove exactly that link (proposer tier)" },
    { "nexus.context", tier::observer, l_managed, t_context,
      "everything linked to a ref, provenance-tagged: records and links, the"
      " proposals that touched a symbol, its event rows, its node" },
    { "nexus.explain", tier::observer, l_managed | l_parseable, t_explain,
      "one call for a symbol: what (compiler), depends, history (events), why /"
      " plan / constraints (records) — every row names its authority" },
    { "nexus.sources", tier::observer, l_managed, t_sources,
      "the external systems nexus.sync pulls from (their *.mcp.json manifests): name,"
      " transport, file and the tools each vocabulary word maps to; dir (owner tier)"
      " loads that directory's manifests instead" },
    { "nexus.sync", tier::editor, l_managed, t_sync,
      "pull a source's tasks (editor tier): spawn its MCP server, call the manifest's"
      " list_tasks tool, upsert each row as a task record with origin = the source,"
      " by its key -> {rows, created, updated, skipped}" },
    { "test.discover", tier::proposer, l_managed, t_discover,
      "fold the project's test families (tests/*.mad with fixtures,"
      " tests/unit/*.cpp) into test records by the canonical conventions"
      " (proposer tier)" },
    { "test.list", tier::observer, l_managed, t_list,
      "the live test records with each one's latest run (optional family filter)" },
    { "test.candidates", tier::observer, l_managed, t_candidates,
      "the tests linked `tests` to a ref (explicit links only); none linked = the"
      " mad family's whole suite NAMED as the fallback, never run" },
    { "test.run", tier::editor, l_managed | l_testable, t_run,
      "run test records on a node (editor tier): the canonical runner per directory"
      " (--report=json) or an external record's command; one testrun event per"
      " verdict" },
    { "test.results", tier::observer, l_managed, t_results,
      "the testrun events (optional test / since / node filters) and the latest run"
      " per test" },
};

value tool_schema(const std::string &name)
{
    fields p;
    const value idp = tools::property("integer", "a node id from a graph.* result");
    const value seqp = tools::property("integer", "a proposal seq (from a proposer's edit answer or"
			    " graph.proposals)");
    const value rid = tools::property("integer", "a record id (the seq of its creating event)");
    const value refp = tools::property("object", REF_TEXT);
    const value rkindp = tools::property("string", "task | requirement | decision | workset |"
			      " milestone | test");
    const value statep = tools::property("string", "open | active | done | superseded | rejected"
			      " (optional)");
    const value fieldsp = tools::property("object", "the record's fields (title, body, reason, ...)");
    if ( name == "graph.history" )
	p["id"] = idp;
    else if ( name == "graph.proposals" )
	p["status"] = tools::property("string", "open | accepted | rejected | withdrawn (optional;"
			   " default: every status)");
    else if ( name == "graph.proposal" || name == "graph.accept" || name == "graph.withdraw" )
	p["seq"] = seqp;
    else if ( name == "graph.reject" )
    {
	p["seq"] = seqp;
	p["reason"] = tools::property("string", "why (optional)");
    }
    else if ( name == "nexus.record" )
	p["id"] = rid;
    else if ( name == "nexus.records" )
    {
	p["rkind"] = rkindp;
	p["state"] = statep;
    }
    else if ( name == "nexus.create" )
    {
	p["rkind"] = rkindp;
	p["fields"] = fieldsp;
	p["state"] = statep;
    }
    else if ( name == "nexus.update" )
    {
	p["id"] = rid;
	p["fields"] = fieldsp;
	p["state"] = statep;
    }
    else if ( name == "nexus.close" )
    {
	p["id"] = rid;
	p["state"] = statep;
    }
    else if ( name == "nexus.link" || name == "nexus.unlink" )
    {
	p["from"] = refp;
	p["rel"] = tools::property("string", "implements | tests | fixes | affects | requested_by |"
			" documents | supersedes | blocked_by | planned_for | reviewed_by |"
			" generated_by | introduced_by | modified_by");
	p["to"] = refp;
    }
    else if ( name == "nexus.context" || name == "test.candidates" )
	p["ref"] = refp;
    else if ( name == "nexus.explain" )
    {
	p["ref"] = refp;
	p["detail"] = tools::property("string", "summary | full (optional; default summary)");
    }
    else if ( name == "nexus.sources" )
	p["dir"] = tools::property("string", "load this directory's *.mcp.json manifests (optional;"
			" owner tier; default: madc's user config directory's mcp/)");
    else if ( name == "nexus.sync" )
	p["source"] = tools::property("string", "a source's name (nexus.sources lists them)");
    else if ( name == "test.discover" )
	p["dir"] = tools::property("string", "the tests directory (optional; default tests)");
    else if ( name == "test.list" )
	p["family"] = tools::property("string", "mad | unit | external (optional; default every"
			   " family)");
    else if ( name == "test.run" )
    {
	p["ids"] = tools::property("array", "test record ids (test.list names them)");
	p["target"] = tools::property("string", "the node to run on: a node id or a platform word"
			   " (optional; null = here) — a node no one offers refuses");
	p["proposal"] = tools::property("integer", "record the runs under this proposal (optional)");
    }
    else if ( name == "test.results" )
    {
	p["test"] = tools::property("integer", "one test record id (optional)");
	p["since"] = tools::property("integer", "only runs after this seq (optional)");
	p["node"] = tools::property("integer", "only runs by this node (optional)");
    }
    return tools::object_schema(p);
}

} // namespace

void project::set_manifest(const std::string &path)
{
    _manifest = path;
}

bool project::load_sources(const std::string &dir, std::string &why)
{
    std::vector<std::string> files;
    detail::glob_paths(dir + "/*.mcp.json", files);
    std::sort(files.begin(), files.end());
    std::vector<source> loaded;
    std::set<std::string> names;
    for ( const std::string &file : files )
    {
	source src;
	std::string fwhy;
	if ( read_source(file, src, fwhy) && !names.insert(src.name).second )
	    fwhy = "the source '" + src.name + "' is already loaded";
	if ( !fwhy.empty() )
	{
	    why = file + ": " + fwhy;
	    return false;
	}
	loaded.push_back(src);
    }
    _sources.swap(loaded);
    _sources_loaded = true;
    return true;
}

bool project::sources(const std::vector<source> *&out, std::string &why)
{
    if ( !_sources_loaded )
    {
	const std::string user = cfg::user_config_dir("madc");
	if ( !user.empty() && !load_sources(user + "/mcp", why) )
	    return false;
	_sources_loaded = true;
    }
    out = &_sources;
    return true;
}

void project::add_tools(tools::registry &r)
{
    r.set_memory(this);
    for ( const tool_row &row : tool_rows )
    {
	const tool_row *tr = &row;
	project *self = this;
	const tools::registry *reg = &r;
	tools::tool t;
	t.name = row.name;
	t.description = row.description;
	t.input_schema = tool_schema(row.name);
	t.least = row.least;
	t.run = [tr, self, reg](const context &c, const value &args, value &result) {
	    const seat s = { *self, *reg };
	    if ( !has_layers(s, c, tr->name, tr->layers, result) )
		return false;
	    return tr->run(s, c, args, result);
	};
	r.add(t);
    }
}

bool project::propose(const context &c, const value &proposal, value &result)
{
    const nexus_fold f = fold_nexus(_records);
    std::vector<value> constraints;
    for ( const value &op : items(object_field(proposal, "ops")) )
    {
	const value rows = constraints_of(c, f, object_field(op, "target"));
	for ( const value &row : items(rows) )
	    constraints.push_back(row);
    }
    fields rec;
    rec["kind"] = value(name_of(kind_names, kind::proposal));
    rec["actor"] = value(c.actor);
    rec["base_seq"] = value(_head);
    rec["ops"] = object_field(proposal, "ops");
    rec["preview"] = object_field(proposal, "preview");
    rec["diagnostics"] = object_field(proposal, "diagnostics");
    rec["constraints"] = value::make_array(constraints);
    rec["checks"] = arr();
    const int64_t seq = append(value::make_object(rec), c.doc);
    fields r;
    r["ok"] = value(true);
    r["proposal"] = value(seq);
    r["preview"] = object_field(proposal, "preview");
    r["diagnostics"] = object_field(proposal, "diagnostics");
    r["constraints"] = value::make_array(constraints);
    result = value::make_object(r);
    return true;
}

void project::applied(const context &c, const value &nodeop)
{
    value rec = nodeop.is_object() ? nodeop : obj();
    rec.object()["kind"] = value(name_of(kind_names, kind::nodeop));
    rec.object()["actor"] = value(c.actor);
    append(rec, c.doc);
}

} // namespace memory
} // namespace madc
