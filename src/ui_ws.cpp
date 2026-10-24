///////////////////////////////////////////////////////////////////////////
//								       //
// ui_ws — the WEB target over a BYTE CHANNEL (client-server arc V6b)   //
//								       //
// The same engine page.js and the same web_model as the webview host, //
// but the DOM ops go down a madc::channel (an RFC6455 WebSocket	       //
// channel) and the page's event JSON comes back up it — instead of a  //
// platform webview. libmadc's own host: the engine serves it by NAME  //
// ("ws", madcdis/ui_host.h), so a serve seat opens it with	       //
// ui::open("ws") and it never collides with the webview host that     //
// also serves ui::WEB. The namespace ui_ws publics are the single     //
// real implementations behind the embedded <ns_ui_ws> declarations;   //
// scripts resolve them mangled-direct (cpp-first-api.md).	       //
//								       //
// The channel is the SEAT'S: this host borrows it for the frontend's  //
// lifetime and never closes it (the serve task that accepted and      //
// upgraded it owns that).					       //
//								       //
// THREAD-SAFETY CONTRACT: a host is confined to the cooperative	       //
// scheduler thread that opened it (the serve task). run()'s readline  //
// PARKS that task on the socket, so other clients' tasks run while    //
// this one idles — no OS-thread block. The pending slot is per	       //
// thread: a seat's attach() and its ui::open("ws") run on one	       //
// scheduler thread with no park between them.			       //
//								       //
///////////////////////////////////////////////////////////////////////////

#include "madcdis/channel.h"
#include "madcdis/ui_host.h"

#include <cstdio>
#include <string>

namespace ui_ws {

// The host state the engine sees only as an opaque handle: the upgraded
// ws channel (borrowed) and the engine frontend that ui::post_event routes
// an inbound event to.
struct host
{
    madc::channel	*ch;
    void		*ctx;
};

// The connection the NEXT open("ws") on this thread binds.
static thread_local madc::channel *pending = 0;

void attach(madc::channel *ch)
{
    pending = ch;
}

// open(title, page, ctx): bind the pending channel to a new frontend.
// `page` (the engine's HTML) is ignored — a ws client loaded it over HTTP
// before dialing the socket, so this host ships DOM ops, not the page.
void *open(const char *, const char *, void *ctx)
{
    if ( !pending )
    {
	fprintf(stderr, "ui_ws: open with no pending connection (attach first)\n");
	return 0;
    }
    host *h = new host;
    h->ch = pending;
    h->ctx = ctx;
    pending = 0;
    return h;
}

// close(host): drop the host struct only — the channel is the seat's.
void close(void *hv)
{
    delete (host *)hv;
}

// eval(host, js): send the script text (the engine's "madcApply([...])")
// as ONE ws text frame. 0 = sent, 1 = the socket failed.
int64_t eval(void *hv, const char *js)
{
    host *h = (host *)hv;
    return h->ch->write(js) ? 0 : 1;
}

// run(host): the engine's ONE-event pump. Read one message off the socket
// (readline PARKS the serve task cooperatively) and post it as the page's
// event, then 0; a closed / failed channel is the window ending: 1.
int64_t run(void *hv)
{
    host *h = (host *)hv;
    std::string msg;
    if ( !h->ch->readline(msg) )
	return 1;
    ui::post_event(h->ctx, msg.c_str());
    return 0;
}

} // namespace ui_ws

// No menu / dialog / tick / clipboard / title / full-screen op: a ws window
// has no native menu bar or file dialog, no platform loop to bound
// (readline is the cooperative wait), and a page asks the user before it
// reads the clipboard — so the application keeps its own copy (plan
// §41.11a step 3e); the page applies only madcApply(...) frames; the
// browser's own full screen is the browser's (F11).
const ui::ui_host_ops &ui_ws_host_ops()
{
    static const ui::ui_host_ops ops = {
	ui_ws::open, ui_ws::close, ui_ws::eval, ui_ws::run, 0, 0, 0, 0, 0, 0, 0,
	"ns_ui_ws"
    };
    return ops;
}
