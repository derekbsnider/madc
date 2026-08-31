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

#include "handle_table.h"
#include "libmadc/value.h"
#include "madcdis/doc_lens.h"
#include "madcdis/hub.h"
#include "madcdis/interaction.h"
#include "madcdis/verbs.h"
#include "madcdis/projection.h"
#include "madcdis/render_text.h"
#include "madcdis/tui_model.h"
#include "madcdis/tui_provider.h"
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
    // Code-entity key-gating (hub doc Decided; the R3 sibling design):
    // when non-empty, DEFINING code entities — bind_verb / bind_check —
    // requires these credentials. Unset = open (every existing caller).
    requirement bind_req;
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

// The session registry: handle_table (slot+1, closed slots stay null, no
// reuse within a run — include/handle_table.h owns the rule). Confined to
// one thread per the contract above; this accessor is the future
// per-context seam.
handle_table<ui_session> &ui_sessions()
{
    static handle_table<ui_session> sessions;
    return sessions;
}

// A TUI session (R5): the model (layout/focus/key semantics) plus the
// registered byte-moving target, and the event queue one read batch
// fills. Independent of world sessions — an application holds both
// handles. Same handle discipline as ui_sessions (handle_table).
struct ui_tui
{
    madc::hub::tui_target *target;
    madc::hub::tui_model   model;
    madc::hub::tui_grid	   painted;	// the diff basis
    std::vector<madc::hub::tui_event> queue;
    size_t next_event;
    size_t rows, cols;
    ui_tui() : target((madc::hub::tui_target *)0), next_event(0),
	       rows(0), cols(0) {}
};

handle_table<ui_tui> &ui_tuis()
{
    static handle_table<ui_tui> tuis;
    return tuis;
}

ui_tui *ui_tui_get(int64_t handle)
{
    return ui_tuis().get(handle);
}

// Key spelling at the value boundary: the model's tui_key_name is the one
// spelling owner (both directions — the bindings tables parse with its
// inverse), adopted here.
std::string ui_key_name(madc::hub::tui_key k, char ch)
{
    return madc::hub::tui_key_name(madc::hub::tui_keyev(k, ch));
}

ui_session *ui_get(int64_t handle)
{
    return ui_sessions().get(handle);
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
    return ui_sessions().open(s);
}

// ui::world_new — an EMPTY session: no world file, no declarations. The
// home of applications whose data is not authored world content (the
// texteditor: documents are files it opens itself). Same handle space,
// same lifecycle, same registry — a %world file is authoring convenience,
// never a session requirement.
int64_t world_new()
{
    return ui_sessions().open(new ui_session());
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
    ui_sessions().close(w);
}

// Is this session allowed to DEFINE code entities? The hub's Decided
// text: "defining or editing code entities is itself key-gated" — the
// same keys+levels machinery as every other condition, evaluated over
// the session's effective credentials. An empty requirement (the
// default) is open. Refusals are loud and bind nothing.
static bool ui_bind_permitted(ui_session *s, const char *who,
			      const char *name)
{
    if ( s->bind_req.empty() )
	return true;
    credentials creds = s->session_creds;
    s->w.close_over_implications(creds);
    if ( s->bind_req.satisfied_by(creds) )
	return true;
    fprintf(stderr, "%s: binding `%s` refused — this session lacks the "
		    "required code-entity key\n", who, name);
    return false;
}

// ui::bind_require_key — arm the code-entity gate: every LATER bind_verb
// / bind_check on this session requires `key` (cumulative; keys layer
// through the world's implications like every credential check).
void bind_require_key(int64_t w, const char *key)
{
    ui_session *s = ui_get(w);
    if ( s && key && *key )
	s->bind_req.keys.push_back(s->w.intern(key));
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
    if ( !ui_bind_permitted(s, "ui::bind_verb", name) )
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

// ui::bind_check — attach a madc-source availability CHECK to a bound
// verb (the script kind of the state-conditional half of availability;
// design §2.9 — "read-only document: remove insert, delete, replace,
// save"). The body runs with the same context fields as a verb body and
// answers "ok" for available or the refusal reason otherwise; it is
// evaluated by the SAME machinery at enumeration (ui::affordances) and at
// dispatch (ui::act), so the two can never disagree. CONTRACT: check
// bodies are read-only — they must not mutate the world, act, or touch
// session lifecycle (the verb-body re-entrancy policy, plus no writes).
void bind_check(int64_t w, const char *name, const char *source)
{
    ui_session *s = ui_get(w);
    if ( !s || !name || !*name || !source )
	return;
    if ( !ui_bind_permitted(s, "ui::bind_check", name) )
	return;
    if ( !s->verbs.set_script_check(s->w.intern(name), source) )
	fprintf(stderr, "ui::bind_check: no verb `%s` bound\n", name);
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
							ctx, gatherers, w);
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

// ---- text component (R4): the piece-table buffer attached to an entity.
// Writes mirror through the mutation context (the one write surface);
// reads are const world reads. Offsets/lengths are BYTES; lines are
// 1-based, length excluding the '\n' (the buffer's documented model).
// Document PROPERTIES (path, modified, read_only) are application bag
// keys — these publics never touch a bag.

// The one session+component lookup every text READ public performs.
static const madc::hub::text_buffer *ui_text_component(int64_t w,
						       int64_t entity)
{
    ui_session *s = ui_get(w);
    return s ? s->w.text_of((entity_id)entity)
	     : (const madc::hub::text_buffer *)0;
}

void text_load(int64_t w, int64_t entity, const char *text)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return;
    madc::hub::mutation_context mc(s->w);
    mc.text_load((entity_id)entity, text ? text : "");
}

void text_insert(int64_t w, int64_t entity, int64_t off, const char *text)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity || off < 0 )
	return;
    madc::hub::mutation_context mc(s->w);
    mc.text_insert((entity_id)entity, (size_t)off, text ? text : "");
}

void text_erase(int64_t w, int64_t entity, int64_t off, int64_t len)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity || off < 0 || len <= 0 )
	return;
    madc::hub::mutation_context mc(s->w);
    mc.text_erase((entity_id)entity, (size_t)off, (size_t)len);
}

void text_replace(int64_t w, int64_t entity, int64_t off, int64_t len,
		  const char *text)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity || off < 0 || len < 0 )
	return;
    madc::hub::mutation_context mc(s->w);
    mc.text_replace((entity_id)entity, (size_t)off, (size_t)len,
		    text ? text : "");
}

// History (madcide IDE-2): checkpoint BEFORE mutating — one semantic
// edit, one step. `meta` is the application's opaque payload (the caret
// rides with the state it belongs to); undo restores the buffer and
// hands it back. False: no such component, or empty history.
void text_checkpoint(int64_t w, int64_t entity, madc::value &meta)
{
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return;
    madc::hub::mutation_context mc(s->w);
    mc.text_checkpoint((entity_id)entity, meta);
}

bool text_undo(madc::value &meta_out, int64_t w, int64_t entity)
{
    meta_out = madc::value();
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return false;
    madc::hub::mutation_context mc(s->w);
    return mc.text_undo((entity_id)entity, meta_out);
}

// The redo-preserving pair (madcide v2): now_meta = the payload live on
// the document being LEFT — it lands on the opposite stack, so walking
// back restores document + interaction state together. A checkpoint (a
// new edit) clears redo; the one-argument text_undo above is the legacy
// destructive form and clears redo too.
bool text_undo(madc::value &meta_out, int64_t w, int64_t entity,
	       madc::value &now_meta)
{
    meta_out = madc::value();
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return false;
    madc::hub::mutation_context mc(s->w);
    return mc.text_undo((entity_id)entity, meta_out, now_meta);
}

bool text_redo(madc::value &meta_out, int64_t w, int64_t entity,
	       madc::value &now_meta)
{
    meta_out = madc::value();
    ui_session *s = ui_get(w);
    if ( !s || !entity )
	return false;
    madc::hub::mutation_context mc(s->w);
    return mc.text_redo((entity_id)entity, meta_out, now_meta);
}

madc::value &text(madc::value &out, int64_t w, int64_t entity)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    return ui_text_out(out, b ? b->text() : std::string());
}

int64_t text_size(int64_t w, int64_t entity)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    return b ? (int64_t)b->size() : -1;	// -1 = no component
}

int64_t text_line_count(int64_t w, int64_t entity)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    return b ? (int64_t)b->line_count() : -1;
}

madc::value &text_line(madc::value &out, int64_t w, int64_t entity, int64_t n)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    size_t off = 0, len = 0;
    if ( b && n > 0 && b->line_span((size_t)n, off, len) )
	return ui_text_out(out, b->slice(off, len));
    return ui_text_out(out, std::string());
}

// The line's byte span, for composing range edits from line commands:
// start offset (or -1 when absent) and length EXCLUDING the newline.
int64_t text_line_start(int64_t w, int64_t entity, int64_t n)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    size_t off = 0, len = 0;
    if ( b && n > 0 && b->line_span((size_t)n, off, len) )
	return (int64_t)off;
    return -1;
}

int64_t text_line_len(int64_t w, int64_t entity, int64_t n)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    size_t off = 0, len = 0;
    if ( b && n > 0 && b->line_span((size_t)n, off, len) )
	return (int64_t)len;
    return -1;
}

int64_t text_find(int64_t w, int64_t entity, int64_t from, const char *needle)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    if ( !b || !needle || !*needle || from < 0 )
	return -1;
    size_t hit = b->find((size_t)from, needle);
    return hit == madc::hub::text_buffer::npos ? -1 : (int64_t)hit;
}

// Word motion (madcide v2 — JOE ^Z/^X): reads, like text_find. -1 when
// the entity has no text component or `from` is negative; otherwise the
// clamped destination offset (see text_buffer::word_left/word_right).
int64_t text_word_left(int64_t w, int64_t entity, int64_t from)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    if ( !b || from < 0 )
	return -1;
    return (int64_t)b->word_left((size_t)from);
}

int64_t text_word_right(int64_t w, int64_t entity, int64_t from)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    if ( !b || from < 0 )
	return -1;
    return (int64_t)b->word_right((size_t)from);
}

// ---- the view seam's coordinate map (madcide AST-3) --------------------
// A document lens's display<->stored map rides as DATA ({disp, stored,
// len} rows — madcdis/doc_lens.h's codec); these publics are the dialect
// face of the ONE projection owner, so caret math across concealed or
// synthetic ranges never becomes per-view arithmetic. -1 = a malformed
// map or a negative offset; a valid EMPTY map answers 0 (a wholly
// rendered view: nothing corresponds).
int64_t lens_to_display(madc::value &map, int64_t stored)
{
    madc::hub::doc_map m;
    if ( stored < 0 || !madc::hub::doc_map::from_value(map, m) )
	return -1;
    return (int64_t)m.to_display((size_t)stored);
}

int64_t lens_to_stored(madc::value &map, int64_t display)
{
    madc::hub::doc_map m;
    if ( display < 0 || !madc::hub::doc_map::from_value(map, m) )
	return -1;
    return (int64_t)m.to_stored((size_t)display);
}

// ---- level-1 TUI (R5): the grid frontend behind the provider seam.
// The MODEL (madcdis/tui_model.h) owns layout, focus, key semantics, and
// diffing; the registered TARGET moves the bytes (the built-in one is the
// hand-rolled VT100/xterm target — src/ui_term.cpp). The application
// loop is compose-as-data -> tui_render -> tui_event -> apply: the same
// value-shaped projection tree render_tree typesets sequentially is
// presented on an addressable grid, choice menus becoming NAVIGABLE.

// Open the grid frontend. Returns a TUI handle (> 0), or 0 with the
// reason on stderr (no target registered, no tty, one already open).
int64_t tui_open()
{
    madc::hub::register_builtin_tui_targets();
    madc::hub::tui_target *t = madc::hub::create_tui_target((const char *)0);
    if ( !t )
    {
	fprintf(stderr, "ui::tui_open: no TUI target available\n");
	return 0;
    }
    ui_tui *u = new ui_tui();
    u->target = t;
    if ( !t->open(u->rows, u->cols) )
    {
	delete t;
	delete u;
	return 0;
    }
    return ui_tuis().open(u);
}

void tui_close(int64_t t)
{
    // Target teardown is this consumer's own step (see handle_table.h);
    // the slot rule (delete + null, no reuse) is the table's.
    ui_tui *u = ui_tuis().get(t);
    if ( !u )
	return;
    u->target->close();
    delete u->target;
    ui_tuis().close(t);
}

int64_t tui_rows(int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    return u ? (int64_t)u->rows : -1;
}

int64_t tui_cols(int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    return u ? (int64_t)u->cols : -1;
}

// Compose a value-shaped projection tree (the render_tree schema) onto
// the grid and present it — only rows that changed since the last render
// repaint. The tree arrives already access-filtered (typesetting only,
// the render_tree contract).
void tui_render(int64_t t, int64_t w, madc::value &tree)
{
    ui_tui *u = ui_tui_get(t);
    ui_session *s = ui_get(w);
    if ( !u || !s )
	return;
    const madc::hub::tui_grid &g =
	u->model.compose(s->r, madc::hub::value_to_uinode(s->w, tree),
			 u->rows, u->cols);
    u->target->paint(u->painted, g);
    u->painted = g;
}

// Hand the terminal back to run a child process (madcide v2, JOE ^K Z):
// tui_suspend leaves grid mode restoring the screen and modes as found;
// tui_resume re-enters and forces the NEXT render to repaint every row
// (the previous contents are gone — the diff basis resets). The size is
// re-read on resume (it may have changed while away); the application
// re-composes and renders as it would after a resize. False + stderr on
// a bad handle, a target that cannot suspend, or mismatched pairing.
bool tui_suspend(int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    if ( !u )
	return false;
    if ( !u->target->suspend() )
    {
	fprintf(stderr, "ui::tui_suspend: the target cannot suspend here\n");
	return false;
    }
    return true;
}

bool tui_resume(int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    if ( !u )
	return false;
    if ( !u->target->resume() )
    {
	fprintf(stderr, "ui::tui_resume: not suspended (or cannot re-enter)\n");
	return false;
    }
    u->target->size(u->rows, u->cols);
    u->painted = madc::hub::tui_grid();
    return true;
}

// JOE's ^R retype (IDE-10a): the terminal's contents can no longer be
// trusted (external writes on the tty, transmission junk) — a delta paint
// against the model's idea of the screen repairs nothing, because that
// idea IS what's wrong. Reset the diff basis so the NEXT render repaints
// every row from scratch (full-row spans + EL tails rewrite the whole
// viewport — the same guarantee tui_resume relies on).
void tui_refresh(int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    if ( !u )
	return;
    u->painted = madc::hub::tui_grid();
}

// Install a keybinding PROFILE: a value object mapping key sequences
// ("^k s" — space-separated spellings, the same names key events carry)
// to action names. Bound sequences resolve ahead of every built-in key
// interpretation and arrive as { event:"action", action:"name",
// seq:"^k s" } (an unbound completion has an empty action and the seq —
// the app may report it). The whole table replaces the previous one — a
// profile swap is one call; an empty object clears. False + stderr on an
// invalid table (unknown spelling, printable-headed sequence, a sequence
// shadowing a shorter binding), leaving the installed table unchanged.
bool tui_bind_keys(int64_t t, madc::value &table)
{
    ui_tui *u = ui_tui_get(t);
    if ( !u )
	return false;
    madc::hub::tui_bindings b;
    if ( table.is_object() )
    {
	const std::map<std::string, madc::value> &o = table.as_object();
	for ( std::map<std::string, madc::value>::const_iterator it
		= o.begin(); it != o.end(); ++it )
	{
	    std::string action = it->second.is_null()
		? std::string() : it->second.as_string();
	    if ( !b.bind(it->first, action) )
	    {
		fprintf(stderr, "ui::tui_bind_keys: bad key sequence `%s`\n",
			it->first.c_str());
		return false;
	    }
	}
    }
    std::string err;
    if ( !b.finalize(err) )
    {
	fprintf(stderr, "ui::tui_bind_keys: %s\n", err.c_str());
	return false;
    }
    u->model.set_bindings(b);
    return true;
}

// The next SEMANTIC event as a value object (names at the boundary):
//   { event:"text",   text:"..." }       a coalesced printable run
//   { event:"key",    key:"up"|"^s"|.. } a non-printable key; carries
//       option:N (1-based, the choose contract) when a focused choice
//       existed — the focused row for keys the widget does not consume
//   { event:"action", action:"name", seq:"^k s" }  a bound sequence
//   { event:"choose", option:N, action:"name" }  N is 1-based — the
//       same number the level-0 menu prints for that option
//   { event:"focus" } / { event:"resize" }  recompose and re-render
// Blocks until input arrives; false = the input source ended (out is a
// null value). Events are interpreted against the LAST tui_render's
// tree (the model's focusables), so render before the first event.
bool tui_event(madc::value &out, int64_t t, int64_t w)
{
    out = madc::value();
    ui_tui *u = ui_tui_get(t);
    ui_session *s = ui_get(w);
    if ( !u || !s )
	return false;
    while ( u->next_event >= u->queue.size() )
    {
	std::vector<madc::hub::tui_keyev> keys;
	if ( !u->target->read_keys(keys) )
	    return false;
	u->queue = u->model.apply_keys(keys);
	u->next_event = 0;
    }
    const madc::hub::tui_event &e = u->queue[u->next_event++];
    std::map<std::string, madc::value> f;
    switch ( e.kind )
    {
	case madc::hub::tui_event_kind::text:
	    f["event"] = madc::value(std::string("text"));
	    f["text"] = madc::value(e.text);
	    break;
	case madc::hub::tui_event_kind::key:
	    f["event"] = madc::value(std::string("key"));
	    f["key"] = madc::value(ui_key_name(e.key, e.ch));
	    // A focused choice's live selection rides along (1-based, the
	    // choose contract) so the application can act on the focused
	    // row for keys the widget does not consume (ins/del); absent
	    // when nothing choice-shaped had focus.
	    if ( e.choice_focused )
		f["option"] = madc::value((int64_t)(e.option + 1));
	    break;
	case madc::hub::tui_event_kind::choose:
	    f["event"] = madc::value(std::string("choose"));
	    f["option"] = madc::value((int64_t)(e.option + 1));
	    f["action"] = madc::value(e.action
				      ? std::string(s->w.spelling(e.action))
				      : std::string());
	    break;
	case madc::hub::tui_event_kind::action:
	    f["event"] = madc::value(std::string("action"));
	    f["action"] = madc::value(e.action_name);
	    f["seq"] = madc::value(e.seq);
	    break;
	case madc::hub::tui_event_kind::resize:
	    // The surface changed: refresh the stored dimensions so the
	    // next render composes to the new size.
	    u->target->size(u->rows, u->cols);
	    f["event"] = madc::value(std::string("resize"));
	    break;
	case madc::hub::tui_event_kind::wake:
	    // Cooperative background tasks drained while the loop waited
	    // for input (stage-2): the application re-checks its pending
	    // state (a spawned parse's completion) and recomposes.
	    f["event"] = madc::value(std::string("wake"));
	    break;
	case madc::hub::tui_event_kind::focus:
	default:
	    f["event"] = madc::value(std::string("focus"));
	    break;
    }
    out = madc::value::make_object(f);
    return true;
}

// The chord entered so far (canonical spelling, e.g. "^k") — empty when
// no chord is pending or the handle is bad. A status line's chord-echo
// seat (JOE's %k) reads it at compose time; presentation state stays in
// the model, this is a read-only view of it.
void tui_pending(madc::value &out, int64_t t)
{
    ui_tui *u = ui_tui_get(t);
    out = madc::value(std::string(u ? u->model.pending_chord()
				    : std::string()));
}

} // namespace ui
