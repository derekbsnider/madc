// modules/ui_web/ui_web.cpp — THE ui_web MODULE (include/madc_module.h;
// `import ui_web;`): the WEB target's host over the platform webview
// (WebKitGTK / WKWebView / WebView2: third_party/webview and madc's chrome,
// madcwebview_chrome.cc, compiled into this module). Loading the module
// registers the host as ui target "web" serving ui::WEB (plan 2026-10-17, D4):
// a program that imports it and opens ui::WEB gets the window; one that does
// not import it is refused by the engine like any target nobody serves. The
// engine (src/ns_ui.cpp, web_model) never names a platform library; the page
// it shows is the engine's (ui_web/page.js + page.css). The face (ui_web.madi)
// is the webview C API and the test seam ui_web::menu_activate.
//
// THREAD CONTRACT: a host is confined to the thread that opened it; the
// page's bound callback runs inside madcwebview_run on that same thread;
// webview_dispatch is the only cross-thread door and nothing here uses it.

#include "madcdis/ui_host.h"	// ui::ui_host_ops, register_host, post_event
#include "libmadc/value.h"
#include "madc_module.h"	// MadcModuleDescription: the entry point's answer
// The C API alone: the implementation (and the platform headers it pulls —
// Xlib's None / Bool macros) is webview.cc's, and stays out of this unit,
// which also holds the generated table (madc.h).
#define WEBVIEW_HEADER
#include "webview/webview.h"
#include "madcwebview_chrome.h"	// madcwebview_*: the native chrome

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

// libmadc's js:: (src/ns_js.cpp), bound at load from the image that imported
// the module.
namespace js {
bool parse(madc::value &out, const char *text);
const char *stringify(madc::value &values);
}

namespace ui_web {

namespace {

// The host state the engine sees only as an opaque handle.
struct host
{
    webview_t view;
    void *ctx;		// the engine session — ui::post_event's key
    int posted;		// events posted during the current run()
    int running;	// inside madcwebview_run (a stop is meaningful)
    int ticked;		// the tick ended the current run() (nothing posted)
    char *clip;		// the last clipboard read (clip_get's answer), the
			// host's until the next read
};

// The most recently opened host — the test seam's target (menu_activate);
// a GUI test opens one window.
host *last_host = 0;

// A field of a parsed request; NULL when the request is no object or has
// no such key.
const madc::value *field(const madc::value &o, const char *key)
{
    if ( !o.is_object() )
	return 0;
    const std::map<std::string, madc::value> &f = o.as_object();
    std::map<std::string, madc::value>::const_iterator it = f.find(key);
    return it == f.end() ? 0 : &it->second;
}

// A field's text; "" when it is absent or null.
std::string text_of(const madc::value *v)
{
    return v && !v->is_null() ? v->as_string() : std::string();
}

void post(host *h, std::map<std::string, madc::value> &fields)
{
    madc::value ev = madc::value::make_object(fields);
    ui::post_event(h->ctx, js::stringify(ev));
}

// An event posted from inside the platform loop stops it ONCE per run
// (madcwebview_stop): GTK delivers terminate through its own idle queue, so
// a second terminate for a second event in the same run would fire in a
// LATER run and end it with nothing posted — read as the window closing.
// (Win32's stop is a flag the loop reads, which a second call only sets
// again.) Posted OUTSIDE run (the test seam's activate), the event is queued
// and the engine reads it before running again: no terminate, or the same
// stale idle would end the next run empty.
void posted_event(host *h)
{
    if ( h->posted++ == 0 && h->running && !h->ticked )
	madcwebview_stop(h->view);	// run() returns; the engine drains the events
}

// The tick (the engine's `tick` op, madcwebview_tick): ends the running loop
// ONCE when no event ended it first — the same one-terminate-per-run rule as
// posted_event (an event landing after the tick in the same run rides the
// tick's terminate; the engine reads it next). A tick armed for a run that
// ended early fires outside any run, or inside a later one: outside it is a
// no-op; inside, it merely ends that run early (the engine probes its tasks
// and runs again).
void on_tick(void *arg)
{
    host *h = (host *)arg;
    if ( h->running && !h->posted && !h->ticked )
    {
	h->ticked = 1;
	madcwebview_stop(h->view);
    }
}

// The ONE bound callback: the page calls window.madc(json) -> here. The
// request is the call's argument array (["{...}"]); the engine's door
// unwraps the one string argument.
void on_event(const char *id, const char *req, void *arg)
{
    host *h = (host *)arg;
    ui::post_event(h->ctx, req);
    webview_return(h->view, id, 0, "null");
    posted_event(h);
}

// A native menu selection (madcwebview_menu_on_action): the SAME door the
// page uses, as the action event a bound chord would produce.
void on_menu_action(const char *id, void *arg)
{
    host *h = (host *)arg;
    std::map<std::string, madc::value> ev;
    ev["kind"] = madc::value("action");
    ev["action"] = madc::value(id ? id : "");
    post(h, ev);
    posted_event(h);
}

// GTK4 draws the window through its GL renderer, and a software GL (Mesa
// llvmpipe — every X server without a GPU: Xvfb, VcXsrv, a Docker-to-desktop
// forward) presents its frames over MIT-SHM. An X connection over TCP has no
// shared memory — DISPLAY names a host, as Docker → a Windows X server does —
// and there the GL path fails to attach its images and the window stays
// BLACK: entirely (measured 2026-09-08 against Xvfb -listen tcp: "MESA:
// error: Failed to attach to x11 shm", not one painted pixel) or from the
// first window grow on (the owner's VcXsrv: the exposed region never paints
// — S0, KG web_window_resize_fill). GTK's cairo renderer draws through plain
// XPutImage and is correct over any connection, so it is the DEFAULT for a
// remote display — chosen before GTK initializes (inside webview_create) and
// only when the user has not named a renderer. A local display (":0",
// "unix:0", Wayland) keeps GTK's own default. Runs once, on the opening
// thread, before any GTK thread exists. (No X display on Windows or macOS.)
void renderer_default_for_display()
{
#if !defined(_WIN32) && !defined(__APPLE__)
    const char *disp = getenv("DISPLAY");
    if ( !disp || disp[0] == 0 || disp[0] == ':' )
	return;
    if ( strncmp(disp, "unix:", 5) == 0 )
	return;
    if ( getenv("GSK_RENDERER") )
	return;
    setenv("GSK_RENDERER", "cairo", 1);
#endif
}

void *open(const char *title, const char *page, void *ctx)
{
    host *h = (host *)malloc(sizeof(host));
    if ( !h )
	return 0;
    renderer_default_for_display();
    h->view = webview_create(0, 0);
    if ( !h->view )
    {
	fprintf(stderr, "ui_web: webview_create failed (no display?)\n");
	free(h);
	return 0;
    }
    h->ctx = ctx;
    h->posted = 0;
    h->running = 0;
    h->ticked = 0;
    h->clip = 0;
    webview_set_title(h->view, title);
    webview_set_size(h->view, 960, 640, WEBVIEW_HINT_NONE);
    webview_bind(h->view, "madc", on_event, h);
    madcwebview_menu_on_action(h->view, on_menu_action, h);
    webview_set_html(h->view, page);
    last_host = h;
    return h;
}

// The native menu bar (S2): the engine's menu JSON — {"bar":[{"title",
// "items":[{"id","title","key"?,"enabled"}|{"sep"}]}]} — walked here and
// handed to the platform layer item by item (madcwebview_menu_*: JSON-free by
// design; GTK4 / Cocoa / Win32 each draw it natively). 0 = drawn; 1 = no
// native menu on this host or the text is not a menu — the page stands as it
// is.
int64_t menu(void *hv, const char *json)
{
    host *h = (host *)hv;
    madc::value m;
    const madc::value *bar = js::parse(m, json) ? field(m, "bar") : 0;
    if ( !bar || !bar->is_array() )
	return 1;
    if ( madcwebview_menu_begin(h->view) != 0 )
	return 1;
    for ( const madc::value &mn : bar->as_array() )
    {
	std::string title = text_of(field(mn, "title"));
	const madc::value *items = field(mn, "items");
	if ( !items || !items->is_array() )
	    continue;
	for ( const madc::value &it : items->as_array() )
	{
	    const madc::value *sep = field(it, "sep");
	    if ( sep && !sep->is_null() )
	    {
		madcwebview_menu_separator(h->view, title.c_str());
		continue;
	    }
	    const madc::value *enabled = field(it, "enabled");
	    bool en = !enabled || enabled->is_null() || enabled->as_boolean();
	    madcwebview_menu_add(h->view, title.c_str(), text_of(field(it, "id")).c_str(),
				 text_of(field(it, "title")).c_str(),
				 text_of(field(it, "key")).c_str(), en ? 1 : 0);
	}
    }
    return madcwebview_menu_end(h->view) == 0 ? 0 : 1;
}

// A native file dialog's answer (madcwebview_dialog_*): the request's mode
// rides the context so the event names it; "" = cancelled.
struct dialog_wait
{
    host *h;
    char mode[16];
};

void on_dialog_done(const char *path, void *arg)
{
    dialog_wait *dw = (dialog_wait *)arg;
    std::map<std::string, madc::value> ev;
    ev["kind"] = madc::value("dialog");
    ev["mode"] = madc::value(dw->mode);
    ev["path"] = madc::value(path ? path : "");
    post(dw->h, ev);
    posted_event(dw->h);
    free(dw);
}

// The native file dialog (S4): the engine's request — {"mode":"open"|
// "save","title":..,"path":<initial>} — shown over the window; the answer
// comes back through the host's one door as a dialog event. 0 = up; 1 = no
// native dialog on this host or not a request — the caller falls back to its
// prompt.
int64_t dialog(void *hv, const char *json)
{
    host *h = (host *)hv;
    madc::value rq;
    const madc::value *mode = js::parse(rq, json) ? field(rq, "mode") : 0;
    if ( !mode || mode->is_null() )
	return 1;
    std::string md = mode->as_string();
    std::string title = text_of(field(rq, "title"));
    std::string initial = text_of(field(rq, "path"));
    dialog_wait *dw = (dialog_wait *)malloc(sizeof(dialog_wait));
    if ( !dw )
	return 1;
    dw->h = h;
    strncpy(dw->mode, md.c_str(), 15);
    dw->mode[15] = 0;
    int rc = md == "save"
	? madcwebview_dialog_save(h->view, title.c_str(), initial.c_str(), on_dialog_done, dw)
	: madcwebview_dialog_open(h->view, title.c_str(), initial.c_str(), on_dialog_done, dw);
    if ( rc != 0 )
    {
	free(dw);
	return 1;
    }
    return 0;
}

// The platform clipboard (plan §41.11a step 3e, madcwebview_clipboard_*):
// clip_set puts plain text there, 0 = taken; clip_get reads what it holds
// now, kept on the host until the next read (the engine copies it at once);
// 0 = no clipboard here.
int64_t clip_set(void *hv, const char *text)
{
    host *h = (host *)hv;
    return madcwebview_clipboard_set(h->view, text) == 0 ? 0 : 1;
}

void on_clip_text(const char *text, void *arg)
{
    host *h = (host *)arg;
    free(h->clip);
    size_t n = strlen(text);
    h->clip = (char *)malloc(n + 1);
    if ( h->clip )
    {
	memcpy(h->clip, text, n);
	h->clip[n] = 0;
    }
}

const char *clip_get(void *hv)
{
    host *h = (host *)hv;
    if ( madcwebview_clipboard_get(h->view, on_clip_text, h) != 0 )
	return 0;
    return h->clip;
}

// The window's title (the engine's `title` op): the composed root's `title`
// hint, sent when it changes. 0 = shown.
int64_t title(void *hv, const char *text)
{
    host *h = (host *)hv;
    webview_set_title(h->view, text);
    return 0;
}

// The window full screen (the engine's `fullscreen` op,
// madcwebview_fullscreen): on = 1 enters, 0 leaves, -1 toggles. 0 = done.
int64_t fullscreen(void *hv, int64_t on)
{
    host *h = (host *)hv;
    return madcwebview_fullscreen(h->view, (int)on) == 0 ? 0 : 1;
}

void close(void *hv)
{
    host *h = (host *)hv;
    if ( last_host == h )
	last_host = 0;
    webview_unbind(h->view, "madc");
    webview_destroy(h->view);
    free(h->clip);
    free(h);
}

// Script text into the page; 0 = accepted. Evals before the first run() are
// the platform view's to drop (the page has not loaded yet): the first resize
// event is the page's ready signal — render after it.
int64_t eval(void *hv, const char *js)
{
    host *h = (host *)hv;
    return webview_eval(h->view, js) == WEBVIEW_ERROR_OK ? 0 : 1;
}

// Run the platform loop (madcwebview_run) until the page posts ONE event
// (on_event stops the loop) or the armed tick ends it, then return 0; the
// window closed: 1 — said by the loop where the platform can tell (Win32),
// else by the loop ending with nothing posted and no tick.
int64_t run(void *hv)
{
    host *h = (host *)hv;
    h->posted = 0;
    h->ticked = 0;
    h->running = 1;
    int rc = madcwebview_run(h->view);
    h->running = 0;
    if ( rc != 0 )
	return 1;
    return (h->posted || h->ticked) ? 0 : 1;
}

// The bounded wait (the engine's `tick` op): arm a one-shot UI-thread timer
// that ends the next run() after ms when no event did — the engine's
// cooperative tasks (a program in the Terminal tab, a build streaming into
// Output) get the CPU between loops. 0 = armed.
int64_t tick(void *hv, int64_t ms)
{
    host *h = (host *)hv;
    return madcwebview_tick(h->view, (unsigned)ms, on_tick, h) == 0 ? 0 : 1;
}

const ui::ui_host_ops ops = {
    open, close, eval, run, menu, dialog, tick, clip_set, clip_get, title, fullscreen,
    "ui_web"
};

// The module registers its host when it LOADS (D4) — at the import in the
// compiler, at program start in a native artifact (its module list).
const bool registered = ui::register_host("web", ui::WEB, &ops);

} // namespace

// The test seam (the face's one function): choose a menu item by its command
// id as the user would (the platform fires the action; the event reaches the
// engine). 0 = fired; 1 = no window, no menu, or no such item.
int64_t menu_activate(const char *id)
{
    if ( !last_host )
	return 1;
    return madcwebview_menu_activate(last_host->view, id) == 0 ? 0 : 1;
}

} // namespace ui_web

// The module's declaration table, generated from its face (ui_web.madi) by
// madc --emit-module-table (src/ui_web.mk), and its description: the entry
// point `import ui_web;` reads. GUI: a program that imports it lifts an armed
// memory guard at run start (WebKit reserves address space far beyond any
// program budget).
#include "ui_web.inc"

MADC_MODULE_EXPORT const MadcModuleDescription *madc_module_ui_web(void)
{
    static const MadcModuleDescription d = {
	MADC_MODULE_ABI, "ui_web", mdGui, madc_module_declarations,
	madc_module_ndeclarations, NULL, NULL
    };
    return &d;
}
