///////////////////////////////////////////////////////////////////////////
//								       //
// madc ui:: namespace — the data-hub projection surface	       //
// (Track 7 Phase 1, S4b)					       //
//								       //
// The namespace ui functions below are the single real	       //
// implementations behind the embedded <ns_ui> declarations; scripts  //
// resolve them mangled-direct (cpp-first-api.md). The Phase 1	       //
// session layer is deliberately bound to the pilot's adventure       //
// catalog (madcdis/adventure.h): it proves the seams — %verb data    //
// binding compiled handlers, projections as the security boundary,   //
// save-is-export — and generalizes into pluggable catalogs and       //
// stored views in later phases.				       //
//								       //
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): the       //
// session registry and every world reached through it are confined   //
// to one thread (the script's). The registry lives behind ONE	       //
// accessor function — the seam that becomes per-engine-context       //
// state in the F2 (programs-use-cores) arc without signature	       //
// changes.							       //
//								       //
///////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "libmadc/value.h"
#include "madcdis/hub.h"
#include "madcdis/verbs.h"
#include "madcdis/projection.h"
#include "madcdis/render_text.h"
#include "madcdis/world_text.h"
#include "madcdis/adventure.h"

using madc::hub::world;
using madc::hub::world_doc;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::credentials;
using madc::hub::requirement;
using madc::hub::verb_table;
using madc::hub::verb_outcome;
using madc::hub::verb_status;

namespace {

struct ui_session
{
    world w;
    madc::hub::roles r;
    verb_table verbs;
    // %require gates by name: (requirement, refusal prose from the data).
    std::map<std::string, std::pair<requirement, std::string> > gates;
    credentials session_creds;
    // The declarations the session owns, merged back into every save.
    std::vector<world_doc::verb_decl> verb_decls;
    std::vector<world_doc::verb_decl> require_decls;

    ui_session() { r = madc::hub::roles::standard(w); }
};

// The session registry: handle = slot index + 1; closed slots stay null so
// handles are never reused within a run. Confined to one thread per the
// contract above; this accessor is the future per-context seam.
std::vector<ui_session *> &ui_sessions()
{
    static std::vector<ui_session *> sessions;
    return sessions;
}

ui_session *ui_get(int64_t handle)
{
    std::vector<ui_session *> &s = ui_sessions();
    if ( handle < 1 || (size_t)handle > s.size() )
	return (ui_session *)0;
    return s[(size_t)handle - 1];
}

// Per-use actor credentials: session grants + carried grants + closure.
credentials ui_creds(ui_session *s, entity_id actor)
{
    return s->w.credentials_for(actor, s->session_creds,
				s->w.intern("in"), "grants");
}

madc::value &ui_text_out(madc::value &out, const std::string &text)
{
    out = madc::value(text);
    return out;
}

} // namespace

// The ONE line-input owner (src/ns_madc.cpp); no host header declares it —
// ui::prompt delegates reading to it rather than growing a second reader.
namespace madc { bool getline(value &out); }

namespace ui {

// ui::prompt — one prompt/read interaction on the process stdio streams:
// write `text`, FLUSH, read a line, return it. The flush is the point —
// stdio never flushes an unterminated line on its own (and glibc never
// flushes stdout on a stdin read), so an unflushed prompt is invisible.
//
// Scripted stdin (a pipe or file) adds exactly one thing: the returned
// line is echoed ("<line>\n") after the read, because no terminal exists
// to echo it — that makes a piped transcript read exactly like an
// interactive session, and at EOF the already-written prompt is the
// reference's trailing prompt-once shape, no special case.
//
// Lines beginning '#' are script comments in BOTH modes: consumed
// silently. Interactively the prompt is shown again for the next read;
// in a script it is not re-shown, so comments stay invisible in the
// transcript. Returns false at EOF (the std::getline contract, via
// madc::getline).
//
// THREAD CONTRACT (.claude/rules/thread-safety.md): operates on the
// process-global stdin/stdout under stdio's own locking; one prompting
// thread at a time is the supported shape — concurrent prompts
// interleave at line granularity.
bool prompt(madc::value &out, const char *text)
{
    const char *t = text ? text : "";
    const bool interactive = isatty(0) != 0;
    bool prompted = false;
    for (;;)
    {
	if ( !prompted || interactive )
	{
	    fputs(t, stdout);
	    fflush(stdout);
	    prompted = true;
	}
	madc::value line;
	if ( !madc::getline(line) )
	    return false;
	std::string s = line.as_string();
	if ( !s.empty() && s[0] == '#' )
	    continue;
	if ( !interactive )
	{
	    fputs(s.c_str(), stdout);
	    fputs("\n", stdout);
	}
	out = line;
	return true;
    }
}

int64_t world_open(const char *path)
{
    if ( !path || !*path )
    {
	fprintf(stderr, "ui::world_open: empty path\n");
	return 0;
    }
    std::string text;
    if ( !madc::hub::read_file_text(path, text) )
    {
	fprintf(stderr, "ui::world_open: cannot read `%s`\n", path);
	return 0;
    }
    world_doc doc;
    std::string err;
    if ( !madc::hub::world_doc_parse(text, doc, err) )
    {
	fprintf(stderr, "ui::world_open: %s: %s\n", path, err.c_str());
	return 0;
    }
    ui_session *s = new ui_session();
    if ( !madc::hub::world_doc_apply(doc, s->w, err) )
    {
	fprintf(stderr, "ui::world_open: %s: %s\n", path, err.c_str());
	delete s;
	return 0;
    }
    for ( size_t i = 0; i < doc.verbs.size(); ++i )
	if ( !madc::hub::adventure::register_catalog(s->verbs, s->w,
						     doc.verbs[i], err) )
	{
	    fprintf(stderr, "ui::world_open: %s: %%verb %s: %s\n", path,
		    doc.verbs[i].name.c_str(), err.c_str());
	    delete s;
	    return 0;
	}
    for ( size_t i = 0; i < doc.requires_.size(); ++i )
    {
	const world_doc::verb_decl &d = doc.requires_[i];
	s->gates[d.name] = std::make_pair(
	    madc::hub::requirement_from_decl(d, s->w), d.refusal);
    }
    s->verb_decls = doc.verbs;
    s->require_decls = doc.requires_;
    ui_sessions().push_back(s);
    return (int64_t)ui_sessions().size();
}

bool world_save(int64_t w, const char *path)
{
    ui_session *s = ui_get(w);
    if ( !s || !path || !*path )
	return false;
    world_doc doc = madc::hub::world_doc_extract(s->w);
    doc.verbs = s->verb_decls;
    doc.requires_ = s->require_decls;
    std::ofstream out(path);
    if ( !out )
    {
	fprintf(stderr, "ui::world_save: cannot write `%s`\n", path);
	return false;
    }
    out << madc::hub::world_doc_emit(doc);
    return (bool)out;
}

void world_close(int64_t w)
{
    std::vector<ui_session *> &s = ui_sessions();
    if ( w >= 1 && (size_t)w <= s.size() && s[(size_t)w - 1] )
    {
	delete s[(size_t)w - 1];
	s[(size_t)w - 1] = (ui_session *)0;
    }
}

int64_t entity_by_name(int64_t w, const char *name)
{
    ui_session *s = ui_get(w);
    if ( !s || !name )
	return 0;
    return (int64_t)s->w.find(name);
}

int64_t create(int64_t w, const char *name)
{
    ui_session *s = ui_get(w);
    if ( !s || !name || !*name )
	return 0;
    madc::hub::mutation_context mc(s->w);
    return (int64_t)mc.create(name);
}

int64_t location(int64_t w, int64_t entity)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return 0;
    return (int64_t)s->w.target((entity_id)entity, s->w.intern("in"),
				(name_id)0);
}

void session_grant(int64_t w, const char *key)
{
    ui_session *s = ui_get(w);
    if ( s && key && *key )
	s->session_creds.grant_key(s->w.intern(key));
}

void session_level(int64_t w, const char *domain, int64_t level)
{
    ui_session *s = ui_get(w);
    if ( s && domain && *domain )
	s->session_creds.set_level(s->w.intern(domain), (int32_t)level);
}

madc::value &render_look(madc::value &out, int64_t w, int64_t actor)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return ui_text_out(out, "");
    credentials creds = ui_creds(s, (entity_id)actor);
    madc::hub::uinode tree = madc::hub::adventure::room_view(
	s->w, s->r, creds, (entity_id)actor);
    return ui_text_out(out, madc::hub::render_text(s->r, tree));
}

madc::value &render_inspect(madc::value &out, int64_t w, int64_t target)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return ui_text_out(out, "");
    std::map<std::string, std::pair<requirement, std::string> >::iterator gate
	= s->gates.find("inspect");
    if ( gate != s->gates.end() )
    {
	credentials creds = s->session_creds;
	s->w.close_over_implications(creds);
	if ( !gate->second.first.satisfied_by(creds) )
	    return ui_text_out(out, gate->second.second.empty()
			       ? std::string("You may not inspect.\n")
			       : gate->second.second + "\n");
    }
    madc::hub::uinode tree = madc::hub::inspect(s->w, s->r,
						(entity_id)target);
    return ui_text_out(out, madc::hub::render_text(s->r, tree));
}

madc::value &act(madc::value &out, int64_t w, int64_t actor, const char *verb,
	   const char *rest)
{
    ui_session *s = ui_get(w);
    if ( !s || !verb || !*verb )
	return ui_text_out(out, "");
    std::string rest_line = rest ? rest : "";
    // First word of the rest may name the target; whatever remains is the
    // handler's argument. An unresolved first word stays in the argument
    // ("go north": north is a direction, not an entity).
    std::string first = rest_line, remainder;
    std::size_t sp = rest_line.find(' ');
    if ( sp != std::string::npos )
    {
	first = rest_line.substr(0, sp);
	remainder = madc::hub::detail::wt_trim(rest_line.substr(sp + 1));
    }
    credentials creds = ui_creds(s, (entity_id)actor);
    entity_id target = madc::hub::adventure::resolve(s->w, (entity_id)actor,
						     first);
    const std::string &arg = target != 0 ? remainder : rest_line;
    verb_outcome r = s->verbs.invoke(s->w, creds, s->w.intern(verb),
				     (entity_id)actor, target, arg);
    switch ( r.status )
    {
	case verb_status::unknown:
	    return ui_text_out(out, "");
	case verb_status::refused:
	    return ui_text_out(out, r.message.empty()
			       ? std::string("You may not do that.")
			       : r.message);
	case verb_status::ok:
	    madc::hub::adventure::tick(s->w);
	    return ui_text_out(out, r.message);
	case verb_status::failed:
	default:
	    return ui_text_out(out, r.message);
    }
}

int64_t turn_count(int64_t w)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return 0;
    entity_id state = s->w.find("world-state");
    return state ? madc::hub::adventure::bag_int(s->w.get(state), "turn", 0)
		 : 0;
}

// ---- entity bag access (E2, the Adventure feeder wave) ---------------------
// Reads copy OUT of the world and never vivify its bags; writes route
// through the hub's mutation_context — the one write surface (G4).

madc::value &get(madc::value &out, int64_t w, int64_t entity, const char *key)
{
    out = madc::value();
    ui_session *s = ui_get(w);
    const madc::hub::entity *e = s ? s->w.get((entity_id)entity)
				   : (const madc::hub::entity *)0;
    if ( !e || !key || !e->bag.is_object() )
	return out;
    const std::map<std::string, madc::value> &m = e->bag.as_object();
    std::map<std::string, madc::value>::const_iterator it = m.find(key);
    if ( it != m.end() )
	out = it->second;
    return out;
}

madc::value &name_of(madc::value &out, int64_t w, int64_t entity)
{
    ui_session *s = ui_get(w);
    const madc::hub::entity *e = s ? s->w.get((entity_id)entity)
				   : (const madc::hub::entity *)0;
    return ui_text_out(out,
		       e ? std::string(s->w.spelling(e->name))
			 : std::string());
}

madc::value &contents(madc::value &out, int64_t w, int64_t container)
{
    out = madc::value::make_array();
    ui_session *s = ui_get(w);
    if ( !s )
	return out;
    std::vector<entity_id> held =
	s->w.sources((entity_id)container, s->w.intern("in"));
    for ( entity_id id : held )
    {
	const madc::hub::entity *e = s->w.get(id);
	if ( e )
	    out.array().push_back(madc::value(s->w.spelling(e->name)));
    }
    return out;
}

void set(int64_t w, int64_t entity, const char *key, const madc::value &v)
{
    ui_session *s = ui_get(w);
    if ( !s || !key || !*key )
	return;
    madc::hub::mutation_context mc(s->w);
    madc::hub::entity *e = mc.edit((entity_id)entity);
    if ( e )
	e->bag.object()[key] = v;
}

void set(int64_t w, int64_t entity, const char *key, const char *v)
    { set(w, entity, key, madc::value(v ? v : "")); }
void set(int64_t w, int64_t entity, const char *key, int64_t v)
    { set(w, entity, key, madc::value(v)); }
void set(int64_t w, int64_t entity, const char *key, bool v)
    { set(w, entity, key, madc::value(v)); }
void set(int64_t w, int64_t entity, const char *key, double v)
    { set(w, entity, key, madc::value(v)); }

void move(int64_t w, int64_t entity, int64_t dest)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return;
    madc::hub::mutation_context mc(s->w);
    name_id rel_in = mc.intern("in");
    std::vector<entity_id> holders =
	s->w.targets((entity_id)entity, rel_in);
    for ( entity_id h : holders )
	mc.link_remove((entity_id)entity, rel_in, h);
    if ( dest )
	mc.link_add((entity_id)entity, rel_in, (entity_id)dest);
}

} // namespace ui
