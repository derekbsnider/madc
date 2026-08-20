#ifndef __MADCDIS_WORLD_TEXT_H
#define __MADCDIS_WORLD_TEXT_H 1

// madcdis/world_text.h — the tagged-text world format (Track 7 Phase 1,
// S4): the pilot's ingestion AND export shape — save IS export, load IS
// ingestion, and gate G2 is parse(emit(w)) ≡ w.
//
//   plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md
//
// The format, line-based, one declaration per line ('#' comments, blank
// lines ignored; prop values are single-line by design — display width is
// the renderer's problem, not the format's):
//
//   %world 1
//   %entity twisty-passage
//     title = Twisty Passage
//     desc = You are in a maze of twisty little passages, all alike.
//     fuel = 50            # integers and true/false infer their kind;
//     lit = false          # anything else is a string to end of line
//   %link brass-lantern in twisty-passage        # unkeyed (containment)
//   %link twisty-passage exit dead-end north     # keyed (an exit)
//   %verb take
//   %verb edit key=builder refusal=Only a builder may reshape the world.
//   %require inspect key=builder refusal=Only a builder sees the bones.
//
// world_doc is the parsed document; apply/extract bind it to a hub world.
// The %verb/%require declarations are carried in the doc for the session
// layer (ns_ui) to bind — the hub world itself stores entities and links
// only. world_text_adapter exposes the same parse through the
// madc::SourceAdapter contract (record families entity/link/verb/require,
// line-range locators back into the source).
//
// THREAD-SAFETY CONTRACT: pure functions of their arguments; the adapter
// is stateless.

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "madcdis/hub.h"
#include "madcdis/source_adapter.h"

namespace madc {
namespace hub {

struct world_doc
{
    struct entity_decl
    {
	std::string name;
	std::vector<std::pair<std::string, madc::value> > props;
	std::size_t line;
	entity_decl() : line(0) {}
    };
    struct link_decl
    {
	std::string from, rel, to, key;
	std::size_t line;
	link_decl() : line(0) {}
    };
    struct verb_decl
    {
	std::string name;
	std::vector<std::string> keys;
	std::string domain;
	long long min_level;
	std::string refusal;
	std::size_t line;
	verb_decl() : min_level(0), line(0) {}
    };

    int version;
    std::vector<entity_decl> entities;
    std::vector<link_decl> links;
    std::vector<verb_decl> verbs;
    std::vector<verb_decl> requires_;	// %require: same shape, named gates

    world_doc() : version(1) {}
};

namespace detail {

inline std::string wt_trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r");
    if ( b == std::string::npos )
	return std::string();
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

inline std::vector<std::string> wt_words(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while ( is >> w )
	out.push_back(w);
    return out;
}

// Kind inference: full-consume integer, true/false boolean, else string.
inline madc::value wt_value(const std::string &text)
{
    if ( text == "true" )
	return madc::value(true);
    if ( text == "false" )
	return madc::value(false);
    if ( !text.empty() )
    {
	char *end = (char *)0;
	long long n = strtoll(text.c_str(), &end, 10);
	if ( end && *end == '\0' && end != text.c_str() )
	    return madc::value((int64_t)n);
    }
    return madc::value(text);
}

// NOT prose::text_of. This is the SERIALIZATION twin of wt_value — its
// output must round-trip through wt_value unchanged (the format's law).
// prose::text_of is display coercion (%g reals, empty null) and may
// drift from this freely; consolidating the two would break one law or
// the other.
inline std::string wt_value_text(const madc::value &v)
{
    if ( v.is_boolean() )
	return v.as_boolean() ? "true" : "false";
    if ( v.is_integer() )
    {
	std::ostringstream os;
	os << v.as_integer();
	return os.str();
    }
    return v.as_string();
}

// Parse one "%verb"/"%require" argument tail: NAME [key=K]* [domain=D]
// [min=N] [refusal=rest of line].
inline bool wt_parse_verb(const std::string &rest, world_doc::verb_decl &out,
			  std::string &err)
{
    std::string tail = rest;
    size_t at = tail.find("refusal=");
    if ( at != std::string::npos )
    {
	out.refusal = wt_trim(tail.substr(at + 8));
	tail = tail.substr(0, at);
    }
    std::vector<std::string> words = wt_words(tail);
    if ( words.empty() )
    {
	err = "missing name";
	return false;
    }
    out.name = words[0];
    for ( size_t i = 1; i < words.size(); ++i )
    {
	if ( words[i].compare(0, 4, "key=") == 0 )
	    out.keys.push_back(words[i].substr(4));
	else if ( words[i].compare(0, 7, "domain=") == 0 )
	    out.domain = words[i].substr(7);
	else if ( words[i].compare(0, 4, "min=") == 0 )
	    out.min_level = strtoll(words[i].c_str() + 4, (char **)0, 10);
	else
	{
	    err = "unknown argument `" + words[i] + "`";
	    return false;
	}
    }
    return true;
}

} // namespace detail

// Slurp a file into a string; false on any failure. THE one owner of
// whole-file reads in the hub subsystem (the lexer and program loaders
// own SOURCE ingestion with their richer error paths — different
// concern, different owner).
inline bool read_file_text(const std::string &path, std::string &out)
{
    std::ifstream in(path.c_str());
    if ( !in )
	return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

// Bind one %verb / %require declaration's condition to hub types: intern
// the keys and the level domain. THE one owner of decl -> requirement
// (the verb catalog and the session gate table both consume it).
inline requirement requirement_from_decl(const world_doc::verb_decl &d,
					 const world &w)
{
    requirement req;
    for ( size_t i = 0; i < d.keys.size(); ++i )
	req.keys.push_back(w.intern(d.keys[i]));
    if ( !d.domain.empty() )
    {
	req.level_domain = w.intern(d.domain);
	req.min_level = (int32_t)d.min_level;
    }
    return req;
}

// Parse the document text. False = loud error naming the 1-based line.
inline bool world_doc_parse(const std::string &text, world_doc &out,
			    std::string &err)
{
    std::istringstream in(text);
    std::string raw;
    std::size_t lineno = 0;
    world_doc::entity_decl *open_entity = (world_doc::entity_decl *)0;
    bool saw_version = false;

    while ( std::getline(in, raw) )
    {
	++lineno;
	std::string line = detail::wt_trim(raw);
	if ( line.empty() || line[0] == '#' )
	    continue;
	if ( line[0] == '%' )
	{
	    open_entity = (world_doc::entity_decl *)0;
	    std::size_t sp = line.find_first_of(" \t");
	    std::string directive = sp == std::string::npos
		? line : line.substr(0, sp);
	    std::string rest = sp == std::string::npos
		? std::string() : detail::wt_trim(line.substr(sp + 1));
	    if ( directive == "%world" )
	    {
		out.version = (int)strtoll(rest.c_str(), (char **)0, 10);
		saw_version = true;
	    }
	    else if ( directive == "%entity" )
	    {
		if ( rest.empty() )
		{
		    std::ostringstream os;
		    os << "line " << lineno << ": %entity needs a name";
		    err = os.str();
		    return false;
		}
		world_doc::entity_decl d;
		d.name = rest;
		d.line = lineno;
		out.entities.push_back(d);
		open_entity = &out.entities.back();
	    }
	    else if ( directive == "%link" )
	    {
		std::vector<std::string> words = detail::wt_words(rest);
		if ( words.size() != 3 && words.size() != 4 )
		{
		    std::ostringstream os;
		    os << "line " << lineno
		       << ": %link needs FROM REL TO [KEY]";
		    err = os.str();
		    return false;
		}
		world_doc::link_decl d;
		d.from = words[0];
		d.rel = words[1];
		d.to = words[2];
		if ( words.size() == 4 )
		    d.key = words[3];
		d.line = lineno;
		out.links.push_back(d);
	    }
	    else if ( directive == "%verb" || directive == "%require" )
	    {
		world_doc::verb_decl d;
		std::string why;
		if ( !detail::wt_parse_verb(rest, d, why) )
		{
		    std::ostringstream os;
		    os << "line " << lineno << ": " << directive << ": " << why;
		    err = os.str();
		    return false;
		}
		d.line = lineno;
		if ( directive == "%verb" )
		    out.verbs.push_back(d);
		else
		    out.requires_.push_back(d);
	    }
	    else
	    {
		std::ostringstream os;
		os << "line " << lineno << ": unknown directive `"
		   << directive << "`";
		err = os.str();
		return false;
	    }
	    continue;
	}
	// A property line: `key = value`, only inside an open %entity.
	std::size_t eq = line.find('=');
	if ( !open_entity || eq == std::string::npos )
	{
	    std::ostringstream os;
	    os << "line " << lineno << ": expected a directive or `key = value`"
	       << (open_entity ? "" : " (no open %entity)");
	    err = os.str();
	    return false;
	}
	std::string key = detail::wt_trim(line.substr(0, eq));
	std::string val = detail::wt_trim(line.substr(eq + 1));
	if ( key.empty() )
	{
	    std::ostringstream os;
	    os << "line " << lineno << ": empty property name";
	    err = os.str();
	    return false;
	}
	open_entity->props.push_back(
	    std::make_pair(key, detail::wt_value(val)));
    }
    if ( !saw_version )
    {
	err = "missing %world version line";
	return false;
    }
    return true;
}

// Emit a document. Deterministic: entities and links in vector order,
// props in given order (extract sorts them via the bag's map).
inline std::string world_doc_emit(const world_doc &d)
{
    std::ostringstream os;
    os << "%world " << d.version << "\n";
    for ( size_t i = 0; i < d.entities.size(); ++i )
    {
	const world_doc::entity_decl &e = d.entities[i];
	os << "%entity " << e.name << "\n";
	for ( size_t p = 0; p < e.props.size(); ++p )
	    os << "  " << e.props[p].first << " = "
	       << detail::wt_value_text(e.props[p].second) << "\n";
    }
    for ( size_t i = 0; i < d.links.size(); ++i )
    {
	const world_doc::link_decl &l = d.links[i];
	os << "%link " << l.from << " " << l.rel << " " << l.to;
	if ( !l.key.empty() )
	    os << " " << l.key;
	os << "\n";
    }
    for ( int pass = 0; pass < 2; ++pass )
    {
	const std::vector<world_doc::verb_decl> &decls
	    = pass == 0 ? d.verbs : d.requires_;
	for ( size_t i = 0; i < decls.size(); ++i )
	{
	    const world_doc::verb_decl &v = decls[i];
	    os << (pass == 0 ? "%verb " : "%require ") << v.name;
	    for ( size_t k = 0; k < v.keys.size(); ++k )
		os << " key=" << v.keys[k];
	    if ( !v.domain.empty() )
		os << " domain=" << v.domain << " min=" << v.min_level;
	    if ( !v.refusal.empty() )
		os << " refusal=" << v.refusal;
	    os << "\n";
	}
    }
    return os.str();
}

// Bind a parsed document into a world: two passes (create every entity,
// then props + links), so forward references in links just work. False =
// loud error (unknown link endpoint).
inline bool world_doc_apply(const world_doc &d, world &w, std::string &err)
{
    for ( size_t i = 0; i < d.entities.size(); ++i )
    {
	const world_doc::entity_decl &e = d.entities[i];
	if ( w.find(e.name) != 0 )
	{
	    err = "duplicate entity `" + e.name + "`";
	    return false;
	}
	entity_id id = w.create(e.name);
	entity *ent = w.get(id);
	for ( size_t p = 0; p < e.props.size(); ++p )
	    ent->bag.object()[e.props[p].first] = e.props[p].second;
    }
    for ( size_t i = 0; i < d.links.size(); ++i )
    {
	const world_doc::link_decl &l = d.links[i];
	entity_id from = w.find(l.from);
	entity_id to = w.find(l.to);
	if ( from == 0 || to == 0 )
	{
	    err = "link endpoint `" + (from == 0 ? l.from : l.to)
		+ "` is not an entity";
	    return false;
	}
	w.link_add(from, w.intern(l.rel), to,
		   l.key.empty() ? 0 : w.intern(l.key));
    }
    return true;
}

// Extract entities + links from a live world (bag props in map order —
// deterministic). The session layer owns %verb/%require and merges its
// declarations before emitting a save.
inline world_doc world_doc_extract(const world &w)
{
    world_doc d;
    for ( entity_id id = 1; id <= (entity_id)w.entity_count(); ++id )
    {
	const entity *e = w.get(id);
	if ( !e )
	    continue;
	world_doc::entity_decl decl;
	decl.name = w.spelling(e->name);
	if ( e->bag.is_object() )
	{
	    const std::map<std::string, madc::value> &bag = e->bag.as_object();
	    for ( std::map<std::string, madc::value>::const_iterator it
		    = bag.begin(); it != bag.end(); ++it )
		decl.props.push_back(std::make_pair(it->first, it->second));
	}
	d.entities.push_back(decl);
    }
    std::vector<link> ls = w.all_links();
    for ( size_t i = 0; i < ls.size(); ++i )
    {
	world_doc::link_decl decl;
	const entity *from = w.get(ls[i].from);
	const entity *to = w.get(ls[i].to);
	decl.from = from ? w.spelling(from->name) : "?";
	decl.rel = w.spelling(ls[i].rel);
	decl.to = to ? w.spelling(to->name) : "?";
	if ( ls[i].key != 0 )
	    decl.key = w.spelling(ls[i].key);
	d.links.push_back(decl);
    }
    return d;
}

// ------------------------------------------------------- world_text_adapter
// The same parse through the madc::SourceAdapter contract: four record
// families, each record a value object, each with a line locator back into
// the source. Accepts file-like sources (plain path or file://).
class world_text_adapter : public SourceAdapter
{
    static std::string source_path(const DataSource &source)
    {
	return source.path().empty() ? source.location() : source.path();
    }
public:
    const char *name() const { return "world_text"; }
    bool can_read(const DataSource &source) const
    {
	return source.scheme().empty() || source.scheme() == "file";
    }
    bool discover_types(const DataSource &, std::vector<ExtractedRecordType> &out,
			error * = (error *)0) const
    {
	out.push_back(ExtractedRecordType("entity"));
	out.push_back(ExtractedRecordType("link"));
	out.push_back(ExtractedRecordType("verb"));
	out.push_back(ExtractedRecordType("require"));
	return true;
    }
    bool extract(const DataSource &source, const std::string &type_name,
		 std::vector<ExtractedRecord> &out,
		 error *err = (error *)0) const
    {
	std::string text;
	if ( !read_file_text(source_path(source), text) )
	{
	    if ( err )
		*err = error(error::severity::error, error::phase::runtime,
			     "world_text: cannot read `"
			     + source_path(source) + "`");
	    return false;
	}
	world_doc d;
	std::string why;
	if ( !world_doc_parse(text, d, why) )
	{
	    if ( err )
		*err = error(error::severity::error, error::phase::runtime,
			     "world_text: " + why);
	    return false;
	}
	if ( type_name == "entity" )
	{
	    for ( size_t i = 0; i < d.entities.size(); ++i )
	    {
		ExtractedRecord r;
		r.type_name = "entity";
		std::map<std::string, madc::value> props(
		    d.entities[i].props.begin(), d.entities[i].props.end());
		std::map<std::string, madc::value> fields;
		fields["name"] = madc::value(d.entities[i].name);
		fields["props"] = madc::value::make_object(props);
		r.record = madc::value::make_object(fields);
		r.locator = SourceLocator::at_line_range(
		    d.entities[i].line, 1 + d.entities[i].props.size());
		out.push_back(r);
	    }
	    return true;
	}
	if ( type_name == "link" )
	{
	    for ( size_t i = 0; i < d.links.size(); ++i )
	    {
		ExtractedRecord r;
		r.type_name = "link";
		std::map<std::string, madc::value> fields;
		fields["from"] = madc::value(d.links[i].from);
		fields["rel"] = madc::value(d.links[i].rel);
		fields["to"] = madc::value(d.links[i].to);
		if ( !d.links[i].key.empty() )
		    fields["key"] = madc::value(d.links[i].key);
		r.record = madc::value::make_object(fields);
		r.locator = SourceLocator::at_line_range(d.links[i].line, 1);
		out.push_back(r);
	    }
	    return true;
	}
	if ( type_name == "verb" || type_name == "require" )
	{
	    const std::vector<world_doc::verb_decl> &decls
		= type_name == "verb" ? d.verbs : d.requires_;
	    for ( size_t i = 0; i < decls.size(); ++i )
	    {
		ExtractedRecord r;
		r.type_name = type_name;
		std::map<std::string, madc::value> fields;
		fields["name"] = madc::value(decls[i].name);
		if ( !decls[i].refusal.empty() )
		    fields["refusal"] = madc::value(decls[i].refusal);
		r.record = madc::value::make_object(fields);
		r.locator = SourceLocator::at_line_range(decls[i].line, 1);
		out.push_back(r);
	    }
	    return true;
	}
	if ( err )
	    *err = error(error::severity::error, error::phase::runtime,
			 "world_text: unknown record family `" + type_name + "`");
	return false;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_WORLD_TEXT_H
