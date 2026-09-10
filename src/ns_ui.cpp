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
#include <deque>
#include <fstream>
#include <map>
#include <set>
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
#include "madcdis/term_screen.h"	// term_feed: the embedded Terminal's screen
#include "madcdis/tui_model.h"
#include "madcdis/tui_provider.h"
#include "madcdis/web_model.h"
#include "madcdis/world_text.h"
#include "rt/rt_task.h"	// the window's wait = the cooperative scheduler's (fire_due / runnable / yield / live)

// How long a window with LIVE cooperative tasks may park in its platform
// loop before the engine probes the scheduler again (ui_web_session::
// read_events): the Terminal/Output pumps' worst-case latency, and the
// only cost of an idle-but-live window.
static const int64_t WEB_TASK_TICK_MS = 25;

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
// The embedded include/madc/** files (scripts/gen_embedded_headers.sh):
// the DOM frontend reads its page from them (include/madc.h declares this
// for the lexer; ns_ui.cpp does not include the compiler's header).
const std::string *find_embedded_header(const std::string &name);

// A script-hosted ui TARGET (the web target is one, a test fake is
// another): a table of C function pointers a madc fragment fills and
// registers ONCE under a target name. LAYOUT CONTRACT with the script-side
// declaration in include/madc/ns_ui (namespace ui) — the same four
// pointers in the same order, append-only, keep both in sync. The engine
// calls them on the opening thread only (design §3.7).
namespace ui {
    typedef void *(*ui_host_open_fn)(const char *title, const char *page,
				     void *ctx);
    typedef void  (*ui_host_close_fn)(void *host);
    typedef int64_t (*ui_host_eval_fn)(void *host, const char *js);
    typedef int64_t (*ui_host_run_fn)(void *host);
    typedef int64_t (*ui_host_menu_fn)(void *host, const char *json);
    typedef int64_t (*ui_host_dialog_fn)(void *host, const char *json);
    typedef int64_t (*ui_host_tick_fn)(void *host, int64_t ms);
    struct ui_host_ops
    {
	ui_host_open_fn	 open;	// build the surface; the engine's `ctx` is
				// post_event's key; 0 = cannot serve here
	ui_host_close_fn close;
	ui_host_eval_fn	 eval;	// run script text in the page; 0 = ok
	ui_host_run_fn	 run;	// run the host's loop until ONE event was
				// posted, then return 0; nonzero = the host
				// ended (the window closed)
	ui_host_menu_fn	 menu;	// draw the native menu bar from the menu
				// JSON (S2); 0 = ok; optional (a host with
				// no chrome leaves it 0)
	ui_host_dialog_fn dialog; // open the native file dialog the request
				// JSON describes (S4); 0 = shown (the answer
				// arrives as a posted {"kind":"dialog"} event);
				// nonzero = unsupported here; optional
	ui_host_tick_fn	 tick;	// end the running/next loop after ms when no
				// event did (run() returns 0, nothing posted)
				// — the cooperative scheduler's bounded wait
				// while tasks are live; nonzero = no timer
				// here; optional
    };
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

// The target-generic ui session (slice 2 of the web-target arc): one
// FRONTEND per ui::open handle. A frontend pairs a model with the thing
// that shows it and owns the event queue one read batch fills:
//   - the GRID frontend (level 1, R5): tui_model (layout / focus / key
//     semantics / diffing) over a registered byte-moving tui_target (the
//     built-in one is the hand-rolled VT100/xterm target, src/ui_term.cpp);
//   - the DOM frontend (level 3): web_model over a script-hosted target
//     (the web target, <ns_ui_web>) — the next step of this slice.
// Independent of world sessions — an application holds both handles. Same
// handle discipline as ui_sessions (handle_table). The publics speak to
// this interface only; the tui_* names are the "term" target's spellings
// over the same handles.
struct ui_frontend
{
    std::vector<madc::hub::tui_event> queue;	// one read batch's events
    size_t next_event;
    size_t rows, cols;				// the surface, in text cells
    ui::level level;				// the UI level this frontend serves
						// (ui::level_of; the grid is ui::TUI,
						// a host declares its own)
    ui_frontend() : next_event(0), rows(0), cols(0), level(ui::NONE) {}
    virtual ~ui_frontend() {}
    // Enter the target (grid mode; a window); report the surface size.
    // False = this target cannot serve here (reason on stderr).
    virtual bool open(size_t &rows, size_t &cols) = 0;
    virtual void close() = 0;
    // Compose the value-shaped projection tree and present it.
    virtual void render(ui_session *s, madc::value &tree) = 0;
    // Refill `queue` with the next batch of semantic events (blocks);
    // false = the input source ended.
    virtual bool read_events() = 0;
    virtual void size(size_t &rows, size_t &cols) = 0;
    virtual void set_bindings(const madc::hub::tui_bindings &b) = 0;
    virtual const std::string &pending_chord() const = 0;
    // Terminal-only capabilities: refused by default (false).
    virtual bool suspend() { return false; }
    virtual bool resume() { return false; }
    // Forget what is on the surface so the NEXT render repaints all.
    virtual void refresh() {}
    // Script text into a page-hosted surface (the test seam); a grid has
    // no page: false.
    virtual bool eval_page(const char *) { return false; }
    // Native file dialogs (S4): can this surface show one, and show the
    // one the request JSON describes (the answer arrives as an event). A
    // grid has none: false.
    virtual bool dialogs() const { return false; }
    virtual bool dialog(const char *) { return false; }
};

struct ui_grid_frontend : ui_frontend
{
    madc::hub::tui_target *target;
    madc::hub::tui_model   model;
    madc::hub::tui_grid	   painted;	// the diff basis
    ui_grid_frontend() : target((madc::hub::tui_target *)0) { level = ui::TUI; }

    bool open(size_t &r, size_t &c)
    {
	madc::hub::register_builtin_tui_targets();
	target = madc::hub::create_tui_target("term");
	if ( !target )
	{
	    fprintf(stderr, "ui::open: no TUI target available\n");
	    return false;
	}
	if ( !target->open(r, c) )
	{
	    delete target;
	    target = (madc::hub::tui_target *)0;
	    return false;
	}
	return true;
    }
    void close()
    {
	// Target teardown is this consumer's own step (see handle_table.h);
	// the slot rule (delete + null, no reuse) is the table's.
	if ( !target )
	    return;
	target->close();
	delete target;
	target = (madc::hub::tui_target *)0;
    }
    // Only rows that changed since the last render repaint. The tree
    // arrives already access-filtered (typesetting only, the render_tree
    // contract).
    void render(ui_session *s, madc::value &tree)
    {
	const madc::hub::tui_grid &g =
	    model.compose(s->r, madc::hub::value_to_uinode(s->w, tree),
			  rows, cols);
	target->paint(painted, g);
	painted = g;
    }
    bool read_events()
    {
	std::vector<madc::hub::tui_keyev> keys;
	if ( !target->read_keys(keys) )
	    return false;
	queue = model.apply_keys(keys);
	next_event = 0;
	return true;
    }
    void size(size_t &r, size_t &c) { target->size(r, c); }
    void set_bindings(const madc::hub::tui_bindings &b) { model.set_bindings(b); }
    const std::string &pending_chord() const { return model.pending_chord(); }
    bool suspend() { return target->suspend(); }
    bool resume()
    {
	if ( !target->resume() )
	    return false;
	target->size(rows, cols);
	painted = madc::hub::tui_grid();
	return true;
    }
    void refresh() { painted = madc::hub::tui_grid(); }
};

// The LINE frontend (level ui::LINE): the ex / edlin client (client-server
// design §2.3b, slice V2.5). No addressable surface — it renders the
// composed projection tree through the level-0 sequential typesetter
// (render_text, the SAME linearizer ui::render_tree exposes and the
// headless one-shot uses) to stdout, and reads ONE line of stdin per
// event. It works over a pipe, in a dumb terminal, and as an MCP seat's
// transcript. It stays DUMB by design: a line becomes a `text` event
// carrying the raw line; the APPLICATION (madcide's run_line) owns the
// `:`-is-a-colon-command / bare-line-is-text classification — the engine
// ui:: layer never learns a tool's command syntax (separation of concerns,
// Rule #5). EOF ends the input, as a closed tty ends the grid's read_keys.
struct ui_line_frontend : ui_frontend
{
    std::string chord;			// no chords at this level (always "")
    ui_line_frontend() { level = ui::LINE; }
    bool open(size_t &r, size_t &c)
    {
	r = rows = 0;			// no addressable grid; the width is
	c = cols = 0;			// the typesetter's own (render_text)
	return true;
    }
    void close() {}
    // Typeset the composed tree sequentially to stdout — the level-0
    // renderer, byte-identical to ui::render_tree's linearization (the tree
    // arrives already access-filtered; a renderer never decides what may be
    // seen). No diff basis: the bottom of the ladder always reprints.
    void render(ui_session *s, madc::value &tree)
    {
	std::string txt = madc::hub::render_text(
	    s->r, madc::hub::value_to_uinode(s->w, tree));
	fputs(txt.c_str(), stdout);
	fflush(stdout);
    }
    // One line of stdin -> one `text` event (the raw line, a trailing CR of
    // a CRLF dropped). A bare EOF ends input; a final unterminated line is
    // still delivered, then the next call reports EOF. The stdin read is
    // this frontend's ONE blocking decision, exactly the grid's read_keys
    // (design §2.3b, the thread contract).
    bool read_events()
    {
	int c = getchar();
	if ( c == EOF )
	    return false;		// stdin ended: input is over
	std::string line;
	while ( c != EOF && c != '\n' )
	{
	    line.push_back((char)c);
	    c = getchar();
	}
	if ( !line.empty() && line[line.size() - 1] == '\r' )
	    line.resize(line.size() - 1);
	madc::hub::tui_event e;
	e.kind = madc::hub::tui_event_kind::text;
	e.text = line;
	queue.clear();
	queue.push_back(e);
	next_event = 0;
	return true;
    }
    void size(size_t &r, size_t &c) { r = rows; c = cols; }
    void set_bindings(const madc::hub::tui_bindings &) {}
    const std::string &pending_chord() const { return chord; }
};

// The script-hosted target registry: name -> the host's ops table (the
// fragment's static lives for the program; the engine never copies it).
// Populated by dynamic initialization before main, read by ui::open.
struct ui_host_reg
{
    std::string			 name;	// the target name (open(name); the window title)
    ui::level			 level;	// the UI level the host declares it serves
    const ui::ui_host_ops	*ops;
};
std::vector<ui_host_reg> &ui_hosts()
{
    static std::vector<ui_host_reg> hosts;
    return hosts;
}
const ui_host_reg *ui_host_named(const std::string &name)
{
    for ( size_t i = 0; i < ui_hosts().size(); ++i )
	if ( ui_hosts()[i].name == name )
	    return &ui_hosts()[i];
    return (const ui_host_reg *)0;
}
// The FIRST registered host declaring the level (registration order).
const ui_host_reg *ui_host_serving(ui::level lvl)
{
    for ( size_t i = 0; i < ui_hosts().size(); ++i )
	if ( ui_hosts()[i].level == lvl )
	    return &ui_hosts()[i];
    return (const ui_host_reg *)0;
}

// The DOM frontend (level 3): web_model over a script-hosted target. The
// host runs the platform loop; the page posts one JSON object per event
// through the host's bound callback -> ui::post_event(ctx, json), which
// lands in `inbound`; read_events drains one object through the model
// into the SAME semantic events the grid emits. The page text is the
// engine's (Task 6 embeds it); the host only shows it.
struct ui_dom_frontend : ui_frontend
{
    madc::hub::web_model	 model;
    const ui::ui_host_ops	*ops;
    void			*host;		// the host's own handle
    std::deque<std::string>	 inbound;	// posted, not yet applied
    std::string			 name;		// the target name (title)
    ui_dom_frontend(const ui_host_reg &r)
	: ops(r.ops), host((void *)0), name(r.name) { level = r.level; }

    // The ONE embedded page: the applier + input relay (ui_web/page.js) and
    // the default look (ui_web/page.css), baked in with the headers
    // (scripts/gen_embedded_headers.sh). The page knows the DOM ops and the
    // key spellings — never an editor or an action.
    static std::string page_html()
    {
	const std::string *js = find_embedded_header("ui_web/page.js");
	const std::string *css = find_embedded_header("ui_web/page.css");
	std::string html = "<!doctype html><html><head><meta charset=\"utf-8\">"
			   "<style>";
	if ( css )
	    html += *css;
	html += "</style></head><body><div id=\"root\"></div>"
		"<span id=\"measure\">M</span>"
		"<input id=\"kb\" autofocus autocomplete=\"off\" spellcheck=\"false\">"
		"<script>";
	if ( js )
	    html += *js;
	html += "</script></body></html>";
	return html;
    }
    bool open(size_t &r, size_t &c)
    {
	const std::string page = page_html();
	host = ops->open("madc", page.c_str(), (void *)this);
	if ( !host )
	{
	    fprintf(stderr, "ui::open: target '%s' cannot serve here\n",
		    name.c_str());
	    return false;
	}
	r = model.rows();
	c = model.cols();
	return true;
    }
    void close()
    {
	if ( !host )
	    return;
	if ( ops->close )
	    ops->close(host);
	host = (void *)0;
    }
    void render(ui_session *s, madc::value &tree)
    {
	std::string ops_json =
	    model.compose(s->r, madc::hub::value_to_uinode(s->w, tree));
	if ( ops->eval )
	    ops->eval(host, ("madcApply(" + ops_json + ")").c_str());
	// The native menu bar (S2): the composed root's `menu` hint, resolved
	// against the bindings, reaches the host only when it changed.
	if ( ops->menu && model.menu_changed() )
	    ops->menu(host, model.menu_json().c_str());
    }
    // The window's wait is the cooperative scheduler's ONE blocking
    // decision, as the terminal's is (src/ui_term.cpp): fire what is due
    // and hand runnable tasks the CPU BEFORE parking in the platform loop,
    // and when tasks ran, synthesize a `wake` — the application recomposes,
    // so a program's output streams into the Terminal tab and a build into
    // Output without a keystroke. While tasks are LIVE but parked (an fd
    // wait, a sleep) the platform wait is BOUNDED by the host's tick, so
    // the probe runs again shortly; with no live task the loop blocks as
    // before (zero cost). A host without a tick cannot bound the wait: the
    // pumps then progress only on input (the pre-tick shape).
    bool read_events()
    {
	while ( inbound.empty() )
	{
	    bool ran = false;
	    __madc_task_fire_due();
	    while ( inbound.empty() && __madc_task_runnable() > 0 )
	    {
		__madc_yield();
		ran = true;
	    }
	    if ( !inbound.empty() )
		break;
	    if ( ran )
	    {
		queue.clear();
		madc::hub::tui_event e;
		e.kind = madc::hub::tui_event_kind::wake;
		queue.push_back(e);
		next_event = 0;
		return true;
	    }
	    if ( ops->tick && __madc_task_live() > 0 )
		ops->tick(host, WEB_TASK_TICK_MS);
	    if ( ops->run(host) != 0 )
		return false;		// the host ended: input is over
	}
	std::string json = inbound.front();
	inbound.pop_front();
	queue = model.apply_input(json);
	next_event = 0;
	return true;
    }
    void size(size_t &r, size_t &c)
    {
	r = model.rows();
	c = model.cols();
    }
    void set_bindings(const madc::hub::tui_bindings &b) { model.set_bindings(b); }
    const std::string &pending_chord() const { return model.pending_chord(); }
    // Forget what the page holds so the next render paints every edit
    // node in full — the grid frontend's painted-grid reset.
    void refresh() { model.reset_surface(); }
    bool eval_page(const char *js)
    {
	return host && ops->eval && ops->eval(host, js ? js : "") == 0;
    }
    bool dialogs() const { return host && ops->dialog; }
    bool dialog(const char *json)
    {
	return host && ops->dialog && ops->dialog(host, json ? json : "") == 0;
    }
};

// The live DOM frontends — post_event's key is a `ctx` a host hands back,
// and a host is script code: an unknown key is refused, never followed.
std::set<ui_dom_frontend *> &ui_dom_live()
{
    static std::set<ui_dom_frontend *> live;
    return live;
}

handle_table<ui_frontend> &ui_frontends()
{
    static handle_table<ui_frontend> frontends;
    return frontends;
}

ui_frontend *ui_frontend_get(int64_t handle)
{
    return ui_frontends().get(handle);
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


// The ONE event -> value-object shaping (names at the boundary), shared by
// every target — a key on the terminal and the same key in a window arrive
// at the application as the same object:
//   { event:"text",   text:"..." }       a coalesced printable run
//   { event:"key",    key:"up"|"^s"|.. } a non-printable key; carries
//       option:N (1-based, the choose contract) when a focused choice
//       existed — the focused row for keys the widget does not consume
//   { event:"action", action:"name", seq:"^k s" }  a bound sequence
//   { event:"choose", option:N, action:"name" }  N is 1-based — the
//       same number the level-0 menu prints for that option
//   { event:"focus" } / { event:"resize" }  recompose and re-render
//   { event:"wake" }                     background tasks drained
//   { event:"snapshot", text:"..." }     a page reported its text (the
//       DOM frontend's test seam)
//   { event:"pointer", phase:"down"|"drag"|"up", offset:N, subject:E, tag:T }
//   Every object also carries event_code (ui::event_kind), a key event
//   key_code (ui::key), a pointer event phase_code (ui::pointer_phase) —
//   the enum values script code compares against (<bits/ui_enums>).
//       a pointing-device gesture on an edit node: N is the BYTE offset
//       in that node's text the pointer resolved to (-1 = the press named
//       no text position: the node itself, a window's header), E (absent
//       when the node projects nothing) the entity it projects — its
//       document — and T (absent when the node carried none) the `tag`
//       hint the composer stamped on it, echoed as data — its own identity
//       for the node (madcide: the window index, the one answer when two
//       windows show the same document)
madc::value ui_event_value(const madc::hub::tui_event &e, ui_session *s,
			   ui_frontend *f)
{
    std::map<std::string, madc::value> fields;
    // The kind as its ENUM value beside the name: script code compares
    // `ev["event_code"] == ui::event_kind::key` (a misspelt enumerator is a
    // compile error; a misspelt name was a silent miss). The names stay for
    // display, transport and the bindings tables.
    fields["event_code"] = madc::value((int64_t)e.kind);
    switch ( e.kind )
    {
	case madc::hub::tui_event_kind::text:
	    fields["event"] = madc::value(std::string("text"));
	    fields["text"] = madc::value(e.text);
	    break;
	case madc::hub::tui_event_kind::key:
	    fields["event"] = madc::value(std::string("key"));
	    fields["key"] = madc::value(ui_key_name(e.key, e.ch));
	    fields["key_code"] = madc::value((int64_t)e.key);	// ui::key
	    // A focused choice's live selection rides along (1-based, the
	    // choose contract) so the application can act on the focused
	    // row for keys the widget does not consume (ins/del); absent
	    // when nothing choice-shaped had focus.
	    if ( e.choice_focused )
		fields["option"] = madc::value((int64_t)(e.option + 1));
	    break;
	case madc::hub::tui_event_kind::choose:
	    fields["event"] = madc::value(std::string("choose"));
	    fields["option"] = madc::value((int64_t)(e.option + 1));
	    fields["action"] = madc::value(e.action
					   ? std::string(s->w.spelling(e.action))
					   : std::string());
	    fields["action_code"] = madc::value(e.action_code);	// the option's
						// `code` hint (0 = none)
	    break;
	case madc::hub::tui_event_kind::action:
	    fields["event"] = madc::value(std::string("action"));
	    fields["action"] = madc::value(e.action_name);
	    fields["action_code"] = madc::value(e.action_code);	// the bound
						// code, or the code the control
						// posting this name carried
	    fields["seq"] = madc::value(e.seq);
	    // The command's argument a native control carried (a buffer
	    // tab's ring index — polish P4); absent for a chord.
	    if ( !e.text.empty() )
		fields["arg"] = madc::value(e.text);
	    break;
	case madc::hub::tui_event_kind::resize:
	    // The surface changed: refresh the stored dimensions so the
	    // next render composes to the new size.
	    f->size(f->rows, f->cols);
	    fields["event"] = madc::value(std::string("resize"));
	    break;
	case madc::hub::tui_event_kind::wake:
	    // Cooperative background tasks drained while the loop waited
	    // for input (stage-2): the application re-checks its pending
	    // state (a spawned parse's completion) and recomposes.
	    fields["event"] = madc::value(std::string("wake"));
	    break;
	case madc::hub::tui_event_kind::snapshot:
	    // A DOM frontend's page reported its rendered text (the test
	    // seam); the grid target never emits it.
	    fields["event"] = madc::value(std::string("snapshot"));
	    fields["text"] = madc::value(e.text);
	    break;
	case madc::hub::tui_event_kind::pointer:
	    // A pointing-device gesture resolved to a byte offset in an
	    // edit node (the DOM model's hit test today; a terminal's mouse
	    // reporting later takes the same shape).
	    fields["event"] = madc::value(std::string("pointer"));
	    fields["phase"] = madc::value(
		std::string(madc::hub::pointer_phase_name(e.phase)));
	    fields["phase_code"] = madc::value((int64_t)e.phase);	// ui::pointer_phase
	    fields["offset"] = madc::value((int64_t)e.offset);
	    if ( e.subject != 0 )
		fields["subject"] = madc::value((int64_t)e.subject);
	    if ( e.tag >= 0 )
		fields["tag"] = madc::value((int64_t)e.tag);
	    break;
	case madc::hub::tui_event_kind::dialog:
	    // A native file dialog answered (S4): the request's mode and the
	    // chosen path ("" = cancelled).
	    fields["event"] = madc::value(std::string("dialog"));
	    fields["mode"] = madc::value(e.action_name);
	    fields["mode_code"] = madc::value(e.action_code);	// ui::dialog_mode
	    fields["path"] = madc::value(e.text);
	    break;
	case madc::hub::tui_event_kind::focus:
	default:
	    fields["event"] = madc::value(std::string("focus"));
	    break;
    }
    return madc::value::make_object(fields);
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

int64_t term_feed(int64_t w, int64_t entity, const char *bytes, int64_t n)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    if ( !b || !bytes || n < 0 )
	return -1;
    const std::string old_text = b->text();
    madc::hub::term_screen scr;
    madc::value col, esc, params;
    get(col, w, entity, "termcol");
    get(esc, w, entity, "termesc");
    get(params, w, entity, "termparams");
    // The first feed: the cursor sits at the end of whatever the text held.
    scr.load(old_text, col.is_integer() ? (size_t)col.as_integer()
					: old_text.size());
    if ( esc.is_integer() )
	scr.st = (madc::hub::term_screen::esc_state)(unsigned char)esc.as_integer();
    if ( params.is_string() )
	scr.params = params.as_string();
    scr.feed(bytes, (size_t)n);
    const std::string new_text = scr.text();
    text_replace(w, entity, 0, (int64_t)old_text.size(), new_text.c_str());
    set(w, entity, "termcol", (int64_t)scr.col);
    set(w, entity, "termesc", (int64_t)(unsigned char)scr.st);
    set(w, entity, "termparams", scr.params.c_str());
    return (int64_t)new_text.size();
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

// The 1-based line containing byte `off` (clamped into the document; a
// negative offset reads as 0). off == size after a terminated last line
// is line_count + 1 — the empty line past the content the editor calls
// the phantom line. One indexed lookup, so a caret's line is O(log lines)
// — never a walk over text_line_start.
int64_t text_line_of(int64_t w, int64_t entity, int64_t off)
{
    const madc::hub::text_buffer *b = ui_text_component(w, entity);
    if ( !b )
	return -1;
    return (int64_t)b->line_of(off < 0 ? 0 : (size_t)off);
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

// ---- the target-generic session surface (slice 2): open(target) ------
// The MODEL owns layout, focus, key semantics and diffing; a FRONTEND
// pairs it with the thing that shows it — the grid frontend behind the
// provider seam (level 1, R5: the registered TARGET moves the bytes; the
// built-in one is the hand-rolled VT100/xterm target, src/ui_term.cpp),
// the DOM frontend behind a script-hosted target (level 3, the web
// target). The application loop is compose-as-data -> render -> event ->
// apply on EVERY target: the same value-shaped projection tree render_tree
// typesets sequentially is presented on an addressable grid or in a page,
// choice menus becoming NAVIGABLE. The tui_* names are the "term" target's
// spellings over the same handles (the level-1 API, unchanged).

// Enter a frontend: register a DOM frontend as live, open the target (0 +
// the target's stderr reason when it cannot serve here), hand out the handle.
static int64_t open_frontend(ui_frontend *f, ui_dom_frontend *dom)
{
    if ( dom )
	ui_dom_live().insert(dom);
    if ( !f->open(f->rows, f->cols) )
    {
	if ( dom )
	    ui_dom_live().erase(dom);
	delete f;
	return 0;
    }
    return ui_frontends().open(f);
}

// Open a target by NAME: "term" (the grid frontend), or a registered
// script-hosted target. Returns a ui handle (> 0), or 0 with the reason on
// stderr (unknown target; the target cannot serve here — no tty, no
// display; one already open). An empty name is the terminal. The name is a
// target's registry key (the fake host of the tests, a second host of one
// level); a PROGRAM names the level it wants — open(level) below.
int64_t open(const char *target)
{
    std::string name = target ? target : "";
    if ( name.empty() )
	name = "term";
    if ( name == "term" )
	return open_frontend(new ui_grid_frontend(), (ui_dom_frontend *)0);
    const ui_host_reg *r = ui_host_named(name);
    if ( !r )
    {
	fprintf(stderr, "ui::open: unknown target '%s'\n", name.c_str());
	return 0;
    }
    ui_dom_frontend *dom = new ui_dom_frontend(*r);
    return open_frontend(dom, dom);
}

// Open the target that serves a UI LEVEL (OWNER 2026-09-09: a program names
// the RENDERING MODEL it wants — ui::level, ordered by requirement — never a
// target's spelling): ui::TUI is the grid frontend; any other level is the
// first registered host declaring it. 0 + stderr when no target serves the
// level here, or when that target cannot serve (as open(name)). ui::NONE has
// no frontend BY DESIGN: a client at that level has no surface and drives
// the session directly — the headless harness, `madcide -c` (the one-shot,
// client-server design §2.3b, V1.5), the api seat (V6) — so there is nothing
// to open. ui::LINE's frontend (stdin lines in, the level-0 typesetter out)
// is the ex / edlin client (slice V2.5, ui_line_frontend).
int64_t open(ui::level lvl)
{
    if ( lvl == ui::TUI )
	return open_frontend(new ui_grid_frontend(), (ui_dom_frontend *)0);
    if ( lvl == ui::LINE )
	return open_frontend(new ui_line_frontend(), (ui_dom_frontend *)0);
    const ui_host_reg *r = ui_host_serving(lvl);
    if ( !r )
    {
	fprintf(stderr, "ui::open: no target serves the %s level\n",
		madc::hub::ui_level_name(lvl));
	return 0;
    }
    ui_dom_frontend *dom = new ui_dom_frontend(*r);
    return open_frontend(dom, dom);
}

// The level the opened target serves (ui::level); -1 for a bad handle. "A
// real terminal exists" is level_of(t) <= ui::TUI — the ONE per-target fact a
// client loop needs.
int64_t level_of(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    return f ? (int64_t)f->level : (int64_t)-1;
}

void close(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    if ( !f )
	return;
    f->close();
    ui_dom_live().erase(dynamic_cast<ui_dom_frontend *>(f));
    ui_frontends().close(t);
}

// ---- script-hosted targets ---------------------------------------------
// Register a host under a target name (a fragment's dynamic initializer
// does this once, before main). False + stderr on a missing table, a
// table without open/run, or a name already taken — the first
// registration wins, so a program cannot swap the web target's host from
// under the engine.
bool register_host(const char *target, ui::level lvl, const ui_host_ops *ops)
{
    std::string name = target ? target : "";
    if ( name.empty() || !ops || !ops->open || !ops->run )
    {
	fprintf(stderr, "ui::register_host: '%s' needs a name and a table with open and run\n",
		name.c_str());
	return false;
    }
    if ( lvl == ui::NONE )
    {
	fprintf(stderr, "ui::register_host: target '%s' must declare the UI level it serves\n",
		name.c_str());
	return false;
    }
    if ( name == "term" || ui_host_named(name) )
    {
	fprintf(stderr, "ui::register_host: target '%s' is already registered\n",
		name.c_str());
	return false;
    }
    ui_host_reg r;
    r.name = name;
    r.level = lvl;
    r.ops = ops;
    ui_hosts().push_back(r);
    return true;
}

// The host's ONE inbound door: the page's event object (JSON text) for
// the session `ctx` its open() received. Callable from inside run(); the
// engine queues it and run() returns to let read_events drain it. An
// unknown `ctx` (a closed session, a stray pointer) is refused loudly.
void post_event(void *ctx, const char *json)
{
    ui_dom_frontend *f = (ui_dom_frontend *)ctx;
    if ( !f || !ui_dom_live().count(f) )
    {
	fprintf(stderr, "ui::post_event: not an open script-hosted ui session\n");
	return;
    }
    std::string text = json ? json : "";
    // A host may hand the page call's ARGUMENT ARRAY verbatim (the webview
    // bind convention — `["{...}"]`): the event object is its one string.
    if ( !text.empty() && text[0] == '[' )
    {
	nlohmann::json args = nlohmann::json::parse(text, nullptr, false);
	if ( args.is_array() && !args.empty() && args[0].is_string() )
	    text = args[0].get<std::string>();
    }
    f->inbound.push_back(text);
}

// Script text into a page-hosted target — the test seam (`madcSnapshot()`
// posts the rendered text back as a snapshot event). False on the grid
// frontend, a bad handle, or a host that refused the text.
bool eval_page(int64_t t, const char *js)
{
    ui_frontend *f = ui_frontend_get(t);
    return f && f->eval_page(js);
}

bool dialogs(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    return f && f->dialogs();
}

bool dialog(int64_t t, const char *json)
{
    ui_frontend *f = ui_frontend_get(t);
    return f && f->dialog(json);
}

int64_t rows(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    return f ? (int64_t)f->rows : -1;
}

int64_t cols(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    return f ? (int64_t)f->cols : -1;
}

// Compose a value-shaped projection tree (the render_tree schema) and
// present it on the target. The tree arrives already access-filtered
// (typesetting only, the render_tree contract).
void render(int64_t t, int64_t w, madc::value &tree)
{
    ui_frontend *f = ui_frontend_get(t);
    ui_session *s = ui_get(w);
    if ( !f || !s )
	return;
    f->render(s, tree);
}

// Hand the terminal back to run a child process (madcide v2, JOE ^K Z):
// suspend leaves grid mode restoring the screen and modes as found;
// resume re-enters and forces the NEXT render to repaint every row (the
// previous contents are gone — the diff basis resets). The size is
// re-read on resume (it may have changed while away); the application
// re-composes and renders as it would after a resize. False + stderr on
// a bad handle, a target that cannot suspend (a window answers false),
// or mismatched pairing.
bool suspend(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    if ( !f )
	return false;
    if ( !f->suspend() )
    {
	fprintf(stderr, "ui::suspend: the target cannot suspend here\n");
	return false;
    }
    return true;
}

bool resume(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    if ( !f )
	return false;
    if ( !f->resume() )
    {
	fprintf(stderr, "ui::resume: not suspended (or cannot re-enter)\n");
	return false;
    }
    return true;
}

// JOE's ^R retype (IDE-10a): the surface's contents can no longer be
// trusted (external writes on the tty, transmission junk) — a delta paint
// against the model's idea of the screen repairs nothing, because that
// idea IS what's wrong. Reset the diff basis so the NEXT render repaints
// every row from scratch (full-row spans + EL tails rewrite the whole
// viewport — the same guarantee resume relies on).
void refresh(int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    if ( f )
	f->refresh();
}

// The ONE table -> tui_bindings converter (bind_keys installs the result,
// validate_keys only verifies — valid here IS bindable there): a value
// object mapping key sequences to action names becomes a finalized
// bindings table. False = invalid table (unknown spelling, printable-
// headed sequence, a sequence shadowing a shorter binding); `err` names
// the offense for the installing caller's stderr.
static bool table_to_bindings(madc::value &table, madc::hub::tui_bindings &b,
			      std::string &err)
{
    if ( table.is_object() )
    {
	const std::map<std::string, madc::value> &o = table.as_object();
	for ( std::map<std::string, madc::value>::const_iterator it
		= o.begin(); it != o.end(); ++it )
	{
	    // The bound value: a string NAME (the tools' shape), an integer
	    // CODE (an application's own enum — madcide binds codes), or null.
	    std::string action;
	    int64_t code = 0;
	    if ( it->second.is_integer() )
		code = it->second.as_integer();
	    else if ( !it->second.is_null() )
		action = it->second.as_string();
	    if ( !b.bind(it->first, action, code) )
	    {
		err = "bad key sequence `" + it->first + "`";
		return false;
	    }
	}
    }
    return b.finalize(err);
}

// Install a keybinding PROFILE: a value object mapping key sequences
// ("^k s" — space-separated spellings, the same names key events carry)
// to action names. Bound sequences resolve ahead of every built-in key
// interpretation and arrive as { event:"action", action:"name",
// seq:"^k s" } (an unbound completion has an empty action and the seq —
// the app may report it). The whole table replaces the previous one — a
// profile swap is one call; an empty object clears. False + stderr on an
// invalid table, leaving the installed table unchanged.
bool bind_keys(int64_t t, madc::value &table)
{
    ui_frontend *f = ui_frontend_get(t);
    if ( !f )
	return false;
    madc::hub::tui_bindings b;
    std::string err;
    if ( !table_to_bindings(table, b, err) )
    {
	fprintf(stderr, "ui::bind_keys: %s\n", err.c_str());
	return false;
    }
    f->set_bindings(b);
    return true;
}

// Handle-free whole-table validation (the gateway seam): the SESSION
// layer validates keybinding-profile data — refusal before any state
// commits — while only the ui CLIENT holds a handle to bind into. The
// same converter as bind_keys, so a table this accepts binds. The verdict
// is SILENT by contract: the session composes its own refusal message,
// and a live tui's stderr is invisible under the alt screen anyway.
bool validate_keys(madc::value &table)
{
    madc::hub::tui_bindings b;
    std::string err;
    return table_to_bindings(table, b, err);
}

// The next SEMANTIC event as a value object (the shapes ui_event_value
// documents). Blocks until input arrives; false = the input source ended
// (out is a null value). Events are interpreted against the LAST render's
// tree (the model's focusables), so render before the first event.
bool event(madc::value &out, int64_t t, int64_t w)
{
    out = madc::value();
    ui_frontend *f = ui_frontend_get(t);
    ui_session *s = ui_get(w);
    if ( !f || !s )
	return false;
    while ( f->next_event >= f->queue.size() )
	if ( !f->read_events() )
	    return false;
    out = ui_event_value(f->queue[f->next_event++], s, f);
    return true;
}

// The chord entered so far (canonical spelling, e.g. "^k") — empty when
// no chord is pending or the handle is bad. A status line's chord-echo
// seat (JOE's %k) reads it at compose time; presentation state stays in
// the model, this is a read-only view of it.
void pending(madc::value &out, int64_t t)
{
    ui_frontend *f = ui_frontend_get(t);
    out = madc::value(std::string(f ? f->pending_chord() : std::string()));
}

// A key SPELLING (the bindings-table / action-name vocabulary: "left",
// "^k", "a") -> its ui::key enumerator value; ui::key::none for a spelling
// that is not a key. The ONE spelling owner (tui_key_from_name) answers, so
// an application that synthesizes a key event from an action name (the
// editor's "the action name IS the key spelling" rule) carries the same
// code the target would have.
int64_t key_code(const char *name)
{
    madc::hub::tui_keyev k;
    if ( !name || !madc::hub::tui_key_from_name(name, k) )
	return (int64_t)madc::hub::tui_key::none;
    return (int64_t)k.kind;
}

bool key_bytes(madc::value &out, const char *name)
{
    out = madc::value(std::string());
    madc::hub::tui_keyev k;
    if ( !name || !madc::hub::tui_key_from_name(name, k) )
	return false;
    std::string bytes = madc::hub::tui_key_bytes(k);
    out = madc::value(bytes);
    return !bytes.empty();
}

// The layout vocabulary at its boundaries (V2): the spellings are
// madcdis/ui_events.h's; unknown = none / "".
int64_t split_code(const char *name)
{
    madc::hub::ui_split d;
    if ( !name || !madc::hub::ui_split_from_name(name, d) )
	return (int64_t)ui::split::none;
    return (int64_t)d;
}

int64_t side_code(const char *name)
{
    madc::hub::ui_side sd;
    if ( !name || !madc::hub::ui_side_from_name(name, sd) )
	return (int64_t)ui::side::none;
    return (int64_t)sd;
}

const char *split_name(int64_t code)
{
    if ( code < (int64_t)ui::split::none || code > (int64_t)ui::split::horizontal )
	return "";
    return madc::hub::ui_split_name((ui::split)code);
}

const char *side_name(int64_t code)
{
    if ( code < (int64_t)ui::side::none || code > (int64_t)ui::side::bottom )
	return "";
    return madc::hub::ui_side_name((ui::side)code);
}

// ---- level-1 TUI (R5): the "term" target's spellings ------------------
// The original grid-frontend API, kept as the terminal target's names over
// the same handles: tui_open() IS open("term").
int64_t tui_open()			{ return ui::open("term"); }
void	tui_close(int64_t t)		{ ui::close(t); }
int64_t tui_rows(int64_t t)		{ return ui::rows(t); }
int64_t tui_cols(int64_t t)		{ return ui::cols(t); }
void	tui_render(int64_t t, int64_t w, madc::value &tree)
					{ ui::render(t, w, tree); }
bool	tui_suspend(int64_t t)		{ return ui::suspend(t); }
bool	tui_resume(int64_t t)		{ return ui::resume(t); }
void	tui_refresh(int64_t t)		{ ui::refresh(t); }
bool	tui_bind_keys(int64_t t, madc::value &table)
					{ return ui::bind_keys(t, table); }
bool	tui_validate_keys(madc::value &table)
					{ return ui::validate_keys(table); }
bool	tui_event(madc::value &out, int64_t t, int64_t w)
					{ return ui::event(out, t, w); }
void	tui_pending(madc::value &out, int64_t t)
					{ ui::pending(out, t); }

} // namespace ui
