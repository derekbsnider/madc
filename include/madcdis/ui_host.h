#ifndef __MADCDIS_UI_HOST_H
#define __MADCDIS_UI_HOST_H 1

#include "madc/bits/ui_enums"	// ui::level — the shared enum text

#include <stdint.h>

// A script-hosted ui TARGET (the web target is one, a test fake is
// another, the ws target is libmadc's own): a table of C function pointers
// a host fills and registers ONCE under a target name. LAYOUT CONTRACT with
// the script-side declaration in include/madc/ns_ui (namespace ui) — the
// same pointers in the same order, append-only, keep both in sync. The
// engine (src/ns_ui.cpp) calls them on the opening thread only (design
// §3.7).
namespace ui {
    typedef void *(*ui_host_open_fn)(const char *title, const char *page,
				     void *ctx);
    typedef void  (*ui_host_close_fn)(void *host);
    typedef int64_t (*ui_host_eval_fn)(void *host, const char *js);
    typedef int64_t (*ui_host_run_fn)(void *host);
    typedef int64_t (*ui_host_menu_fn)(void *host, const char *json);
    typedef int64_t (*ui_host_dialog_fn)(void *host, const char *json);
    typedef int64_t (*ui_host_tick_fn)(void *host, int64_t ms);
    typedef int64_t (*ui_host_clip_set_fn)(void *host, const char *text);
    typedef const char *(*ui_host_clip_get_fn)(void *host);
    typedef int64_t (*ui_host_title_fn)(void *host, const char *text);
    typedef int64_t (*ui_host_fullscreen_fn)(void *host, int64_t on);
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
	ui_host_clip_set_fn clip_set; // put plain text on the platform's
				// clipboard (plan §41.11a step 3e); 0 = taken;
				// nonzero = no clipboard here; optional
	ui_host_clip_get_fn clip_get; // the platform clipboard's text, read
				// now ("" = it holds none); the host owns it
				// until its next call; NULL = no clipboard
				// here; optional
	ui_host_title_fn title;	// show `text` as the window's title (the
				// composed root's `title` hint, sent when it
				// changes); 0 = shown; optional
	ui_host_fullscreen_fn fullscreen; // the window full screen: on = 1
				// enters, 0 leaves, -1 toggles its current
				// state; 0 = done; nonzero = not here;
				// optional
	const char *identity;	// the host's name for itself (its
				// module's name, "ui_web"): two
				// registrations stating one identity are one
				// host, from whichever image; NULL = none
				// (the entries say it); optional
    };

    // The engine's host doors (src/ns_ui.cpp): register a target, and post
    // the page's event for the session a host's open() received.
    bool register_host(const char *target, level lvl, const ui_host_ops *ops);
    void post_event(void *ctx, const char *json);
}

// The ws target's table (src/ui_ws.cpp): the web page over a WebSocket
// channel, libmadc's own host. The engine serves it by NAME ("ws") —
// never by level: ui::open(ui::WEB) is the program's declaration of need,
// served by a host the program registers.
const ui::ui_host_ops &ui_ws_host_ops();

#endif // __MADCDIS_UI_HOST_H
