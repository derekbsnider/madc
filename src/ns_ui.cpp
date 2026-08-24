///////////////////////////////////////////////////////////////////////////
//								       //
// madc ui:: namespace — the GENERIC interaction/session surface      //
// (Track 7.2 R1)						       //
//								       //
// The namespace ui functions below are the single real	       //
// implementations behind the embedded <ns_ui> declarations; scripts  //
// resolve them mangled-direct (cpp-first-api.md).		       //
//								       //
// RULE #7 GOVERNS THIS FILE: no application vocabulary, no compiled   //
// application verbs, no application projections. Applications supply //
// their verbs as DATA (%verb declarations gate; ui::bind_verb	       //
// attaches madc-source bodies — the script-entity binding kind) and   //
// their vocabulary as ARGUMENTS (relation and property names are      //
// parameters of the generic reads below). The engine ships ZERO       //
// verbs. The one-time eviction of the Phase 1 compiled pilot catalog  //
// is gated by scripts/check-engine-app-purity.sh.		       //
//								       //
// Substrate conventions (documented data-model conventions of this    //
// session layer, uniform across applications — the Rule #7 sense of   //
// a filename convention, not per-application special-casing):	       //
//   `in`     — the containment relation behind location/contents/     //
//              move and the context/credential closures.	       //
//   `grants` — the bag property through which a carried entity        //
//              confers a key (the hub access model's data-derived     //
//              credential contract).				       //
//								       //
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): the       //
// session registry and every world reached through it are confined   //
// to one thread (the script's). The registry lives behind ONE	       //
// accessor function — the seam that becomes per-engine-context       //
// state in the F2 (programs-use-cores) arc without signature	       //
// changes. Script verb bodies run on that same thread, inside the    //
// dispatching act(); they must not re-enter ui::act and must not     //
// open or close worlds (the Phase 1 re-entrancy policy).	       //
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
#include "madcdis/interaction.h"
#include "madcdis/verbs.h"
#include "madcdis/projection.h"
#include "madcdis/render_text.h"
#include "madcdis/world_text.h"

using madc::hub::world;
using madc::hub::world_doc;
using madc::hub::entity_id;
using madc::hub::name_id;
using madc::hub::credentials;
using madc::hub::requirement;
using madc::hub::verb_table;
using madc::hub::verb_outcome;
using madc::hub::verb_status;
using madc::hub::interaction_context;
using madc::hub::invocation;
using madc::hub::action_env;
using madc::hub::affordance;
using madc::hub::affordance_set;
using madc::hub::affordance_gatherer;

// Host-side prototypes for engine services implemented elsewhere in this
// binary (no host header declares them; scripts see them via <ns_madc>):
// the ONE line-input owner and the runtime-eval seam (src/ns_madc.cpp).
namespace madc {
    bool getline(value &out);
    value &eval_string_ctx(value &out, const char *source, value &ctx);
}

namespace {

struct ui_session;
bool ui_script_executor(action_env &env, const invocation &inv,
			const std::string &source, madc::value &out);

struct ui_session
{
    world w;
    madc::hub::roles r;
    verb_table verbs;
    // %require gates by name: (requirement, refusal prose from the data).
    std::map<std::string, std::pair<requirement, std::string> > gates;
    credentials session_creds;
    // The declarations the session owns: gating data for bind_verb and
    // the %verb/%require lines merged back into every save.
    std::vector<world_doc::verb_decl> verb_decls;
    std::vector<world_doc::verb_decl> require_decls;

    ui_session()
    {
	r = madc::hub::roles::standard(w);
	verbs.set_script_executor(ui_script_executor);
    }
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

// Per-use actor credentials: session grants + carried grants + closure
// (the `grants` bag-property convention, see the header comment).
credentials ui_creds(ui_session *s, entity_id actor)
{
    return s->w.credentials_for(actor, s->session_creds,
				s->w.intern("in"), "grants");
}

// The actor's current interaction context: focus = the containing entity,
// scope = carried + co-located + the focus (the resolution order).
interaction_context ui_context(ui_session *s, entity_id actor)
{
    return madc::hub::containment_context(s->w, actor, s->w.intern("in"));
}

madc::value &ui_text_out(madc::value &out, const std::string &text)
{
    out = madc::value(text);
    return out;
}

// interpret (design §5): raw driver text -> a structured invocation over
// an explicit context. Interpretation here is generic — verb word plus
// the raw argument line; semantic target binding is the application's
// (its verb bodies resolve words with their own vocabulary, e.g. via
// ui::resolve with their alias property).
invocation ui_interpret(ui_session *s, entity_id actor, const char *verb,
			const char *rest)
{
    invocation inv;
    inv.context = ui_context(s, actor);
    inv.actor = actor;
    inv.action = s->w.intern(verb);
    inv.arguments[madc::hub::arg_key(s->w)]
	= madc::value(madc::hub::detail::wt_trim(rest ? rest : ""));
    return inv;
}

// The script-entity executor (the eval seam injected into every session's
// registry): the invocation arrives as the eval context — its fields are
// top-level names in the body — and the body's returned text is the
// value-shaped result. An eval failure prints its own diagnostic and
// comes back empty; empty = failed (a verb that ran says something).
bool ui_script_executor(action_env &env, const invocation &inv,
			const std::string &source, madc::value &out)
{
    const world &w = env.mc.view();
    const madc::value *arg = inv.argument(madc::hub::arg_key(w));
    std::map<std::string, madc::value> c;
    c["w"] = madc::value(env.session);
    c["actor"] = madc::value((int64_t)inv.actor);
    c["target"] = madc::value((int64_t)inv.target);
    c["arg"] = arg ? *arg : madc::value(std::string());
    c["verb"] = madc::value(std::string(w.spelling(inv.action)));
    madc::value ctx = madc::value::make_object(c);
    madc::eval_string_ctx(out, source.c_str(), ctx);
    return out.is_string() && !out.as_string().empty();
}

} // namespace

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
    // %verb lines are DECLARATIONS — name + gating data. Bodies arrive
    // from the application via bind_verb; a declared verb with no bound
    // body dispatches as unknown.
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

// ui::bind_verb — attach a madc-source body (the script-entity binding
// kind) to a verb name. Gating (keys/levels/refusal) comes from the
// world's %verb declaration when one names this verb; an undeclared name
// binds ungated. Binding order is enumeration order in ui::affordances.
void bind_verb(int64_t w, const char *name, const char *source)
{
    ui_session *s = ui_get(w);
    if ( !s || !name || !*name || !source )
	return;
    requirement req;
    std::string refusal;
    for ( size_t i = 0; i < s->verb_decls.size(); ++i )
	if ( s->verb_decls[i].name == name )
	{
	    req = madc::hub::requirement_from_decl(s->verb_decls[i], s->w);
	    refusal = s->verb_decls[i].refusal;
	    break;
	}
    s->verbs.register_script_verb(s->w.intern(name), req, refusal, source);
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

// ui::has_key — the keys+levels evaluator, surfaced: does the actor's
// effective credential set (session grants + carried grants + key
// implications) hold this key? Entity-attached conditions in application
// verbs check through here.
bool has_key(int64_t w, int64_t actor, const char *key)
{
    ui_session *s = ui_get(w);
    if ( !s || !key || !*key )
	return false;
    return ui_creds(s, (entity_id)actor).has_key(s->w.intern(key));
}

// The ONE inspect projection both publics read: the generic entity
// browser tree, or — when the world's `%require inspect` gate refuses
// these credentials — a status-role node carrying the refusal prose.
// Projection selection IS the access decision (projection is the
// security boundary), so the refusal is itself a projection: text and
// tree consumers handle it with the same machinery.
static madc::hub::uinode ui_inspect_projection(ui_session *s, entity_id target)
{
    std::map<std::string, std::pair<requirement, std::string> >::iterator gate
	= s->gates.find("inspect");
    if ( gate != s->gates.end() )
    {
	credentials creds = s->session_creds;
	s->w.close_over_implications(creds);
	if ( !gate->second.first.satisfied_by(creds) )
	{
	    madc::hub::uinode refused(s->r.status);
	    refused.content = madc::value(gate->second.second.empty()
					  ? std::string("You may not inspect.")
					  : gate->second.second);
	    return refused;
	}
    }
    return madc::hub::inspect(s->w, s->r, target);
}

madc::value &render_inspect(madc::value &out, int64_t w, int64_t target)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return ui_text_out(out, "");
    return ui_text_out(out, madc::hub::render_text(
			s->r, ui_inspect_projection(s, (entity_id)target)));
}

// ui::inspect_tree — the SAME projection as hub DATA: the value tree
// uinode_to_value spells (role/states/actions by NAME, subject as the
// entity handle, children nested). Demand 3: the projection tree is
// itself inspectable/walkable data, not a rendering side effect.
madc::value &inspect_tree(madc::value &out, int64_t w, int64_t target)
{
    ui_session *s = ui_get(w);
    if ( !s )
    {
	out = madc::value::make_object();
	return out;
    }
    out = madc::hub::uinode_to_value(
		s->w, ui_inspect_projection(s, (entity_id)target));
    return out;
}

// ui::render_tree — typeset ANY value-shaped projection tree (the
// inspect_tree schema; every field optional) through the level-0
// renderer: an application COMPOSES its projection as ordinary data and
// hands it here (projection-as-data). Typesetting only — the tree
// arrives already access-filtered, so this public makes no gate
// decision; a `choice` node's children render as a numbered menu.
madc::value &render_tree(madc::value &out, int64_t w, madc::value &tree)
{
    ui_session *s = ui_get(w);
    if ( !s )
	return ui_text_out(out, "");
    return ui_text_out(out, madc::hub::render_text(
			s->r, madc::hub::value_to_uinode(s->w, tree)));
}

madc::value &act(madc::value &out, int64_t w, int64_t actor, const char *verb,
	   const char *rest)
{
    ui_session *s = ui_get(w);
    if ( !s || !verb || !*verb )
	return ui_text_out(out, "");
    // The universal cycle (design §5): interpret the physical text into a
    // structured invocation, then validate + execute through the registry.
    invocation inv = ui_interpret(s, (entity_id)actor, verb, rest);
    credentials creds = ui_creds(s, (entity_id)actor);
    verb_outcome r = s->verbs.invoke(s->w, creds, inv, w);
    // Display coercion of the value-shaped content is projection-side
    // (prose::text_of): strings verbatim, scalars natural, null empty.
    std::string text = madc::hub::prose::text_of(r.content);
    switch ( r.status )
    {
	case verb_status::unknown:
	    return ui_text_out(out, "");
	case verb_status::refused:
	    return ui_text_out(out, text.empty()
			       ? std::string("You may not do that.")
			       : text);
	case verb_status::ok:
	case verb_status::failed:
	default:
	    return ui_text_out(out, text);
    }
}

// ui::affordances — enumerate what the actor can presently do: each
// registered action with its truthful visible/enabled/reason state from
// the same keys+levels evaluator that gates execution. Ids surface as
// NAMES; an empty label defaults to the action's spelling.
madc::value &affordances(madc::value &out, int64_t w, int64_t actor)
{
    out = madc::value::make_array();
    ui_session *s = ui_get(w);
    if ( !s )
	return out;
    credentials creds = ui_creds(s, (entity_id)actor);
    interaction_context ctx = ui_context(s, (entity_id)actor);
    std::vector<affordance_gatherer> gatherers;	// application gatherers:
						// a later, script-shaped seam
    affordance_set set = madc::hub::resolve_affordances(s->w, s->verbs, creds,
							ctx, gatherers);
    for ( size_t i = 0; i < set.size(); ++i )
    {
	const affordance &a = set[i];
	const madc::hub::entity *t = s->w.get(a.target);
	const madc::hub::entity *p = s->w.get(a.provider);
	std::map<std::string, madc::value> f;
	std::string action = s->w.spelling(a.action);
	f["action"] = madc::value(action);
	f["target"] = madc::value(t ? std::string(s->w.spelling(t->name))
				     : std::string());
	f["provider"] = madc::value(p ? std::string(s->w.spelling(p->name))
				      : std::string());
	f["label"] = madc::value(a.label.empty() ? action : a.label);
	f["visible"] = madc::value(a.avail.visible);
	f["enabled"] = madc::value(a.avail.enabled);
	f["reason"] = madc::value(a.avail.reason);
	out.array().push_back(madc::value::make_object(f));
    }
    return out;
}

// ---- generic graph/bag reads (relation and property names are DATA —
// arguments, never engine spellings) ---------------------------------------

// Keyed-link enumeration: out = array of {key, target} objects for the
// `rel` links FROM `from`, in link order; key is "" for an unkeyed link,
// target is the linked entity's canonical name.
madc::value &links(madc::value &out, int64_t w, int64_t from, const char *rel)
{
    out = madc::value::make_array();
    ui_session *s = ui_get(w);
    if ( !s || !rel )
	return out;
    name_id r = s->w.intern(rel);
    std::vector<madc::hub::link> ls = s->w.links_of((entity_id)from);
    for ( size_t i = 0; i < ls.size(); ++i )
    {
	if ( ls[i].from != (entity_id)from || ls[i].rel != r )
	    continue;
	const madc::hub::entity *to = s->w.get(ls[i].to);
	std::map<std::string, madc::value> f;
	f["key"] = madc::value(ls[i].key != 0
			       ? std::string(s->w.spelling(ls[i].key))
			       : std::string());
	f["target"] = madc::value(to ? std::string(s->w.spelling(to->name))
				     : std::string());
	out.array().push_back(madc::value::make_object(f));
    }
    return out;
}

// Word -> entity over the actor's current scope (carried, co-located,
// focus — the interaction context's resolution order): a word matches an
// entity's canonical name or its `alias_prop` bag property. The alias
// property is the APPLICATION's vocabulary, passed as data. 0 = no match.
int64_t resolve(int64_t w, int64_t actor, const char *word,
		const char *alias_prop)
{
    ui_session *s = ui_get(w);
    if ( !s || !word || !*word )
	return 0;
    interaction_context ctx = ui_context(s, (entity_id)actor);
    for ( size_t i = 0; i < ctx.scope.size(); ++i )
    {
	const madc::hub::entity *e = s->w.get(ctx.scope[i]);
	if ( !e )
	    continue;
	if ( std::string(word) == s->w.spelling(e->name) )
	    return (int64_t)ctx.scope[i];
	if ( alias_prop && *alias_prop && e->bag.is_object() )
	{
	    const std::map<std::string, madc::value> &m = e->bag.as_object();
	    std::map<std::string, madc::value>::const_iterator it
		= m.find(alias_prop);
	    if ( it != m.end() && it->second.is_string()
	      && it->second.as_string() == word )
		return (int64_t)ctx.scope[i];
	}
    }
    return 0;
}

// ---- entity bag access (E2) ------------------------------------------------
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
