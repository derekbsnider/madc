// madcide_plugin_host.inc — the plugin `host` transport (docs/plans/2026-09-
// 30-madcide-plugins.md §5.4, §6 G6, §7; plan §41.11a step 7): a plugin's
// code in a process of its own, a fork child of the running madcide
// (madc::fork_uri), speaking the plugin envelope ({"plugin": <verb>, ...},
// plugin_verb) over its stdin and stdout. A crash there ends the child, never
// madcide, and the child starts again.
//
// One owner each:
//   plugin_seat_request   the plugin envelope's run, command_id, get, set and
//                         show, under the asker's tier: a host's child's, or
//                         any seat client's (G6; madcide_seat.inc hands it
//                         the envelope)
//   plugin_host_start     a host's child forked, told what to open, and served
//                         until its code activated: its registrations become
//                         the world's rows, carrying the host's name and the
//                         child's handler ids (plugin_command_hosted & co.)
//   plugin_host_activate  plugin_activate's host arm
//   plugin_host_task      a host's connection served (plugin_host_serve), and
//                         its child started again when it ends, at most
//                         plugin_host_restarts() times a session
//   plugin_host_invoke    a host's row run: its invocation sent to its child
//   plugin_hosts_stop     a session's hosts ended as it closes
//   plugin_child          THE CHILD: its code opened by the one rule
//                         (plugin_code_open), activated through
//                         madcide_seat_api (the API table as seat requests),
//                         then its invocations run
//
// A world's hosts are the registry's "hosts" rows: { name, dir, manifest,
// conn (its connection's share id; 0 = none), state (plugin_host_state),
// restarts }.
//
// Thread contract: madcide's side runs on the session's thread — activation
// at session open, the serve task between events, as every seat client's.
// The child is its own process with one thread; its state (plugin_host_io,
// plugin_host_child) is written only there.

// How many times a session starts a host's child again after it ended.
long plugin_host_restarts()
{
    return 3;
}

// One line of the envelope on a channel.
void plugin_host_write(madc::channel &ch, var &msg)
{
    ch.write(format("{}\n", js::stringify(msg)));
}

long active_doc(long w, long es, long fallback);	// madcide_core.inc

// ---- the requests (G6) ------------------------------------------------------
// The plugin envelope's requests a running plugin makes (run, command_id,
// get, set, show), answered in `out` ({ok, ...} or {ok: false, error}) under
// `tier`, the asker's. The scope of get and set is an entity of the world (a
// session's, a document's); 0 = the session's. A registration is refused
// here: a plugin registers at its activation (plugin_host_start).
void plugin_seat_request(var &out, long w, long es, long tier, var &req)
{
    var verbw;
    keyed_get(verbw, req, "plugin");
    long verb = plugin_verb_of(verbw);
    long need = tierOBSERVER;
    if ( verb == pvSET || verb == pvSHOW )
	need = tierEDITOR;
    if ( verb == pvRUN )
	need = cmd_min_tier_w(w, req["code"].as_integer());
    if ( tier < need )
    {
	var why = format("'{}' requires {} (you are {})", verbw, tier_name(need),
			 tier_name(tier));
	out = { "ok": false, "error": why };
	return;
    }
    var scopev;
    keyed_get(scopev, req, "scope");
    long scope = es;
    if ( !scopev.is_null() && scopev.as_integer() != 0 )
	scope = scopev.as_integer();
    switch ( verb )
    {
	case pvRUN:
	{
	    long code = req["code"].as_integer();
	    var nm;
	    cmd_name_w(nm, w, code);
	    if ( strlen(nm.c_str()) == 0 )
	    {
		var why = format("no command {}", code);
		out = { "ok": false, "error": why };
		return;
	    }
	    var dv;
	    keyed_get(dv, req, "doc");
	    long doc = active_doc(w, es, 0);
	    if ( !dv.is_null() && dv.as_integer() != 0 )
		doc = dv.as_integer();
	    var arg;
	    keyed_get(arg, req, "arg");
	    var at = "";
	    if ( arg.is_string() )
		at = arg;
	    bool ran = ide_api_run(w, es, doc, code, at.c_str());
	    out = { "ok": true, "result": ran };
	    return;
	}
	case pvCOMMAND_ID:
	{
	    var nm = req["name"];
	    long code = cmd_of_w(w, nm.c_str());
	    out = { "ok": true, "code": code };
	    return;
	}
	case pvGET:
	{
	    var key = req["key"];
	    var v;
	    ui::get(v, w, scope, key.c_str());
	    out = { "ok": true };
	    out["value"] = v;
	    return;
	}
	case pvSET:
	{
	    var key = req["key"];
	    var v;
	    keyed_get(v, req, "value");
	    ui::set(w, scope, key.c_str(), v);
	    out = { "ok": true };
	    return;
	}
	case pvSHOW:
	{
	    bool shown = show_view(w, es, req["kind"].as_integer());
	    out = { "ok": shown };
	    return;
	}
	case pvCOMMAND:
	case pvVIEW:
	case pvEVENT:
	{
	    out = { "ok": false, "error": "a plugin registers at its activation" };
	    return;
	}
    }
    var why = format("unknown plugin request '{}'", verbw);
    out = { "ok": false, "error": why };
}

// ---- madcide's side: a host's child -----------------------------------------
// The index of host `name` among the world's hosts; -1 = none.
long plugin_host_index(long w, var &name)
{
    return registry_named_index(w, "hosts", name);
}

// Host `hi`'s row, with its connection and state set (conn 0 = none).
void plugin_host_mark(long w, long hi, long conn, long state)
{
    long reg = plugin_registry_make(w);
    var hosts;
    registry_rows(hosts, w, "hosts");
    hosts[hi]["conn"] = conn;
    hosts[hi]["state"] = state;
    ui::set(w, reg, "hosts", hosts);
}

// A line from host `host`'s child that is not the envelope: the plugin's own
// output (its stdout, its stderr folded in — a crash report), shown on
// madcide's stderr as a plugin's own output is in madcide's process.
void plugin_host_output(var &host, var &line)
{
    println(stderr, "plugin {}: {}", host, line);
}

// Serve a starting child until its code activated (`done`, its activated
// message): each registration becomes a row of host `host`, and the other
// requests are answered as a running host's are. False = the child ended
// first.
bool plugin_host_converse(var &done, madc::channel &ch, long w, long es,
			  var &host)
{
    var line;
    while ( ch.readline(line) )
    {
	var msg;
	if ( !js::parse(msg, line) || !msg.is_object() )
	{
	    plugin_host_output(host, line);
	    continue;
	}
	var verbw;
	keyed_get(verbw, msg, "plugin");
	long verb = plugin_verb_of(verbw);
	if ( verb == pvNONE )
	    continue;
	if ( verb == pvACTIVATED )
	{
	    done = msg;
	    return true;
	}
	var reply;
	if ( verb == pvCOMMAND )
	{
	    var nm = msg["name"];
	    var tt = msg["title"];
	    long code = plugin_command_hosted(w, nm.c_str(), tt.c_str(), host,
					      msg["hid"].as_integer());
	    reply = { "ok": code != 0, "code": code };
	}
	else if ( verb == pvVIEW )
	{
	    var nm = msg["name"];
	    var tt = msg["title"];
	    var key = msg["key"];
	    long kind = plugin_view_hosted(w, nm.c_str(), tt.c_str(), key.c_str(),
					   msg["show"].as_integer(), host);
	    reply = { "ok": kind != viewNONE, "kind": kind };
	}
	else if ( verb == pvEVENT )
	{
	    bool ok = plugin_event_hosted(w, msg["kind"].as_integer(), host,
					  msg["hid"].as_integer());
	    reply = { "ok": ok };
	}
	else
	    plugin_seat_request(reply, w, es, tierEDITOR, msg);
	reply["seq"] = msg["seq"];
	plugin_host_write(ch, reply);
    }
    return false;
}

// Start host `hi`'s child: forked, told what to open, and served until its
// code activated. The connection's handle (to adopt), 0 = refused (`why`),
// -1 = no plugin process on this platform (`why`). `from` = the code it
// opened, `note` = what the child said of it (a refused library's source in
// use). The host's earlier subscriptions are dropped first: a child starting
// again subscribes again.
int plugin_child();		// the child's entry (below)
int64_t plugin_host_start(long w, long es, long hi, var &from, var &why,
			  var &note)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    var hr = hosts[hi];
    var host = hr["name"];
    madc::channel ch(madc::fork_uri(plugin_child));
    if ( !ch.ok() )
    {
	why = ch.last_error();
	return -1;
    }
    plugin_host_forget_events(w, host);
    var start = { "plugin": plugin_verb_name(pvSTART), "name": host, "w": w };
    start["dir"] = hr["dir"];
    start["manifest"] = hr["manifest"];
    plugin_host_write(ch, start);
    var done;
    if ( !plugin_host_converse(done, ch, w, es, host) )
    {
	why = "its process ended before its code activated";
	ch.close_write();
	ch.close();
	return 0;
    }
    if ( !done["ok"].as_boolean() )
    {
	why = done["why"];
	ch.close_write();
	ch.close();
	return 0;
    }
    from = done["from"];
    note = done["note"];
    return ch.detach();
}

// Host `hi`'s row field `serving`: the request of its child's being
// answered (0 = none). An invocation it causes carries the number, so the
// child runs it within the call, as an in-process ide::run runs its command
// before it returns.
void plugin_host_serving(long w, long hi, long seq)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    hosts[hi]["serving"] = seq;
    ui::set(w, plugin_registry(w), "hosts", hosts);
}

// The invocations sent to host `hi` while its child was starting, written to
// its connection now that it is served.
void plugin_host_flush(madc::channel &ch, long w, long hi)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    var q;
    keyed_get(q, hosts[hi], "queued");
    if ( q.is_null() )
	return;
    for ( var line : q )
	ch.write(line.c_str());
    var none = {};
    hosts[hi]["queued"] = none;
    ui::set(w, plugin_registry(w), "hosts", hosts);
}

// A running host's connection (`id`, granted its tier): each request
// answered until the child ends.
void plugin_host_serve(madc::channel &ch, long w, long es, long hi, long id)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    var host = hosts[hi]["name"];
    var line;
    while ( ch.readline(line) )
    {
	var msg;
	if ( !js::parse(msg, line) || !msg.is_object() )
	{
	    plugin_host_output(host, line);
	    continue;
	}
	if ( envelope_of(msg) != envPLUGIN )
	    continue;
	var reply;
	plugin_host_serving(w, hi, msg["seq"].as_integer());
	plugin_seat_request(reply, w, es, eff_tier(id), msg);
	plugin_host_serving(w, hi, 0);
	reply["seq"] = msg["seq"];
	plugin_host_write(ch, reply);
    }
}

// Host `hi`'s life after its activation: its connection served, and its
// child started again when it ends (a crash, an exit) — unless the session is
// stopping it, or it has ended plugin_host_restarts() times, or it will not
// start; then it stops, and its commands say so.
void plugin_host_task(long w, long es, long hi, long handle)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    var host = hosts[hi]["name"];
    for ( ;; )
    {
	{
	    madc::channel ch;
	    if ( !ch.adopt(handle) )
		return;
	    registry_rows(hosts, w, "hosts");
	    if ( hosts[hi]["state"].as_integer() == hsSTOPPING )
	    {
		// The session closed before this connection was served: its child
		// sees its input end and returns.
		ch.close_write();
		ch.close();
		plugin_host_mark(w, hi, 0, hsSTOPPED);
		return;
	    }
	    long id = ch.share();
	    madc::conn_set_cookie(id, tierEDITOR);
	    plugin_host_mark(w, hi, id, hsRUNNING);
	    plugin_host_flush(ch, w, hi);
	    plugin_host_serve(ch, w, es, hi, id);
	    ch.unshare(id);
	    ch.close_write();
	    ch.close();
	}
	registry_rows(hosts, w, "hosts");
	if ( hosts[hi]["state"].as_integer() == hsSTOPPING )
	{
	    plugin_host_mark(w, hi, 0, hsSTOPPED);
	    return;
	}
	long n = hosts[hi]["restarts"].as_integer() + 1;
	if ( n > plugin_host_restarts() )
	{
	    plugin_host_mark(w, hi, 0, hsSTOPPED);
	    ui::set(w, es, "msg", format("Plugin '{}' stopped: its process ended {} times.",
					host, n));
	    return;
	}
	plugin_host_mark(w, hi, 0, hsRESTARTING);
	registry_rows(hosts, w, "hosts");
	hosts[hi]["restarts"] = n;
	ui::set(w, plugin_registry(w), "hosts", hosts);
	var from;
	var why;
	var note;
	int64_t h = plugin_host_start(w, es, hi, from, why, note);
	if ( h <= 0 )
	{
	    plugin_host_mark(w, hi, 0, hsSTOPPED);
	    ui::set(w, es, "msg", format("Plugin '{}' stopped: {}", host, why));
	    return;
	}
	ui::set(w, es, "msg", format("Plugin '{}' ended; it was started again.", host));
	handle = h;
    }
}

// plugin_activate's host arm: plugin `name` (its directory `dir`, its
// manifest `m`) activated in a child process. 1 = active, 0 = refused (`why`;
// nothing it registered remains), -1 = no plugin process on this platform.
long plugin_host_activate(long w, var &dir, var &name, var &m, var &why,
			  var &note)
{
    long reg = plugin_registry_make(w);
    long es = ui::entity_by_name(w, "editor-state");
    var hosts;
    registry_rows(hosts, w, "hosts");
    long hi = php::count(hosts);
    var row = { "name": name, "conn": 0, "state": hsRESTARTING, "restarts": 0 };
    row["dir"] = dir;
    row["manifest"] = m;
    hosts[] = row;
    ui::set(w, reg, "hosts", hosts);
    var mark;
    registry_mark(mark, w);
    var from;
    int64_t h = plugin_host_start(w, es, hi, from, why, note);
    if ( h <= 0 )
    {
	registry_rollback(w, mark);
	plugin_host_forget_events(w, name);
	var kept = {};
	for ( long i = 0; i < hi; i = i + 1 )
	    kept[] = hosts[i];
	ui::set(w, reg, "hosts", kept);
	if ( h < 0 )
	    return -1;
	return 0;
    }
    var active;
    registry_rows(active, w, "active");
    active[] = { "name": name, "transport": ptHOST, "from": from, "handle": 0 };
    ui::set(w, reg, "active", active);
    plugin_host_mark(w, hi, 0, hsRESTARTING);
    go plugin_host_task(w, es, hi, h);
    return 1;
}

// A host's row run (a command's, `event` null, or an event's): its
// invocation sent to the host's child, which runs the handler on its own; a
// child still starting (at session open, or again after it ended) gets it
// once it is served (plugin_host_flush). False = the host is stopped (a
// command's caller is told so).
bool plugin_host_invoke(long w, long es, long doc, var &host, long hid,
			const char *arg0, var &event)
{
    var arg = arg0;		// ring discipline: own the text first
    long hi = plugin_host_index(w, host);
    var hosts;
    registry_rows(hosts, w, "hosts");
    long state = hsSTOPPED;
    if ( hi >= 0 )
	state = hosts[hi]["state"].as_integer();
    if ( state != hsRUNNING && state != hsRESTARTING )
    {
	if ( event.is_null() )
	    ui::set(w, es, "msg", format("Plugin '{}' is not running.", host));
	return false;
    }
    var inv = { "plugin": plugin_verb_name(pvINVOKE), "hid": hid, "w": w,
		"es": es, "doc": doc, "arg": arg };
    if ( !event.is_null() )
	inv["event"] = event;
    var during;
    keyed_get(during, hosts[hi], "serving");
    if ( !during.is_null() && during.as_integer() != 0 )
	inv["during"] = during;
    var line = format("{}\n", js::stringify(inv));
    if ( state == hsRESTARTING )
    {
	var q;
	keyed_get(q, hosts[hi], "queued");
	if ( q.is_null() )
	    q = {};
	q[] = line;
	hosts[hi]["queued"] = q;
	ui::set(w, plugin_registry(w), "hosts", hosts);
	return true;
    }
    return madc::conn_send(hosts[hi]["conn"].as_integer(), line.c_str());
}

// A session's hosts ended as it closes: each running child's connection
// cancelled (its SIGTERM), so its serve task wakes to the end and stops.
void plugin_hosts_stop(long w)
{
    var hosts;
    registry_rows(hosts, w, "hosts");
    long n = php::count(hosts);
    for ( long i = 0; i < n; i = i + 1 )
    {
	long conn = hosts[i]["conn"].as_integer();
	long st = hosts[i]["state"].as_integer();
	if ( st == hsSTOPPED )
	    continue;
	plugin_host_mark(w, i, conn, hsSTOPPING);
	if ( conn != 0 )
	    madc::conn_cancel(conn);
    }
}

// ---- the child ----------------------------------------------------------------
// THE CHILD's state, in its own process only: its channel to madcide, and
// { handlers (an id is an index; the handler's full-width address),
//   pending (invocations that arrived while a call waited for its reply),
//   early (replies to an enclosing call that arrived while a call within
//   it waited for its own: kept, never dropped),
//   seq (the last request's number) }.
madc::channel *plugin_host_io = 0;
var plugin_host_child;

// Keep a handler the plugin registers; its id.
long plugin_host_keep(int64_t addr)
{
    var hs = plugin_host_child["handlers"];
    long hid = php::count(hs);
    hs[] = addr;
    plugin_host_child["handlers"] = hs;
    return hid;
}

// Run one invocation's handler (a command's, or an event's when it carries
// the event).
void plugin_host_run_invoke(var &msg)
{
    var hs = plugin_host_child["handlers"];
    long hid = msg["hid"].as_integer();
    if ( hid < 0 || hid >= php::count(hs) )
	return;
    int64_t addr = hs[hid].as_integer();
    long iw = msg["w"].as_integer();
    long ies = msg["es"].as_integer();
    long idoc = msg["doc"].as_integer();
    var ev;
    keyed_get(ev, msg, "event");
    if ( ev.is_null() )
    {
	var arg = msg["arg"];
	cmd_handler ch = (cmd_handler)addr;
	ch(iw, ies, idoc, arg.c_str());
	return;
    }
    event_handler eh = (event_handler)addr;
    eh(iw, ies, idoc, ev);
}

// A reply to request `seq` kept in early (an enclosing call's, arrived
// while a call within it waited): moved into `out`. False = none kept.
bool plugin_host_early(var &out, long seq)
{
    var early = plugin_host_child["early"];
    long n = php::count(early);
    for ( long i = 0; i < n; i = i + 1 )
	if ( early[i]["seq"].as_integer() == seq )
	{
	    out = early[i];
	    var kept = {};
	    for ( long j = 0; j < n; j = j + 1 )
		if ( j != i )
		    kept[] = early[j];
	    plugin_host_child["early"] = kept;
	    return true;
	}
    return false;
}

// One request to madcide and its reply (`out`). An invocation the request
// caused (its `during` is this request's number: the plugin ran its own
// command) runs now, within the call, as it would in madcide's process; any
// other waits in pending. A reply to an enclosing call that arrives while a
// call within it waits is kept in early for it. False = madcide is gone.
bool plugin_host_call(var &out, var &req)
{
    long seq = plugin_host_child["seq"].as_integer() + 1;
    plugin_host_child["seq"] = seq;
    req["seq"] = seq;
    plugin_host_write(*plugin_host_io, req);
    var line;
    for ( ;; )
    {
	if ( plugin_host_early(out, seq) )
	    return true;
	if ( !plugin_host_io->readline(line) )
	    break;
	var msg;
	if ( !js::parse(msg, line) || !msg.is_object() )
	    continue;
	var pv;
	keyed_get(pv, msg, "plugin");
	if ( !pv.is_null() )
	{
	    var dg;
	    keyed_get(dg, msg, "during");
	    if ( !dg.is_null() && dg.as_integer() == seq )
	    {
		plugin_host_run_invoke(msg);
		continue;
	    }
	    var p = plugin_host_child["pending"];
	    p[] = msg;
	    plugin_host_child["pending"] = p;
	    continue;
	}
	var sq;
	keyed_get(sq, msg, "seq");
	if ( sq.is_null() )
	    continue;
	if ( sq.as_integer() == seq )
	{
	    out = msg;
	    return true;
	}
	var early = plugin_host_child["early"];
	early[] = msg;
	plugin_host_child["early"] = early;
    }
    var none;
    out = none;
    return false;
}

// The API table as seat requests: <madcide/plugin>'s ide:: calls, the same
// for every transport.
long seat_api_command(long w, const char *name, const char *title, cmd_handler h)
{
    long hid = plugin_host_keep((int64_t)h);
    var req = { "plugin": plugin_verb_name(pvCOMMAND), "name": name, "title": title,
		"hid": hid };
    var r;
    if ( !plugin_host_call(r, req) )
	return 0;
    return r["code"].as_integer();
}

long seat_api_view(long w, const char *name, const char *title, const char *key,
		   long show)
{
    var req = { "plugin": plugin_verb_name(pvVIEW), "name": name, "title": title,
		"key": key, "show": show };
    var r;
    if ( !plugin_host_call(r, req) )
	return viewNONE;
    return r["kind"].as_integer();
}

bool seat_api_event(long w, long kind, event_handler h)
{
    long hid = plugin_host_keep((int64_t)h);
    var req = { "plugin": plugin_verb_name(pvEVENT), "kind": kind, "hid": hid };
    var r;
    return plugin_host_call(r, req) && r["ok"].as_boolean();
}

bool seat_api_run(long w, long es, long doc, long code, const char *arg)
{
    var req = { "plugin": plugin_verb_name(pvRUN), "es": es, "doc": doc,
		"code": code, "arg": arg };
    var r;
    return plugin_host_call(r, req) && r["ok"].as_boolean()
	   && r["result"].as_boolean();
}

long seat_api_command_id(long w, const char *name)
{
    var req = { "plugin": plugin_verb_name(pvCOMMAND_ID), "name": name };
    var r;
    if ( !plugin_host_call(r, req) )
	return cmdNONE;
    return r["code"].as_integer();
}

void seat_api_get(var &out, long w, long scope, const char *key)
{
    var req = { "plugin": plugin_verb_name(pvGET), "scope": scope, "key": key };
    var r;
    var none;
    out = none;
    if ( plugin_host_call(r, req) )
	keyed_get(out, r, "value");
}

void seat_api_set(long w, long scope, const char *key, const var &value)
{
    var req = { "plugin": plugin_verb_name(pvSET), "scope": scope, "key": key };
    req["value"] = value;
    var r;
    plugin_host_call(r, req);
}

bool seat_api_show(long w, long es, long kind)
{
    var req = { "plugin": plugin_verb_name(pvSHOW), "kind": kind };
    var r;
    return plugin_host_call(r, req) && r["ok"].as_boolean();
}

const ide_api madcide_seat_api = { MADCIDE_PLUGIN_API, seat_api_command,
				   seat_api_view, seat_api_event, seat_api_run,
				   seat_api_command_id, seat_api_get, seat_api_set,
				   seat_api_show };

// The child's next message: a pending invocation first, else the next
// envelope line. False = madcide is gone.
bool plugin_host_next(var &msg)
{
    var p = plugin_host_child["pending"];
    if ( php::count(p) > 0 )
    {
	php::array_shift(msg, p);
	plugin_host_child["pending"] = p;
	return true;
    }
    var line;
    while ( plugin_host_io->readline(line) )
	if ( js::parse(msg, line) && msg.is_object() )
	    return true;
    return false;
}

// THE CHILD (madc::fork_uri's entry): read what to open, open the plugin's
// code by the one rule, activate it through madcide_seat_api, say so, then
// run each invocation's handler until madcide is gone.
int plugin_child()
{
    madc::channel io("stdio://");
    plugin_host_io = &io;
    var none = {};
    var fresh = { "handlers": none, "pending": none, "early": none, "seq": 0 };
    plugin_host_child = fresh;
    var start;
    if ( !plugin_host_next(start) )
	return 1;
    var dir = start["dir"];
    var name = start["name"];
    var m = start["manifest"];
    long w = start["w"].as_integer();
    long transport = ptNONE;
    int64_t h = 0;
    int64_t aa = 0;
    var from;
    var why = "";
    var note;
    bool ok = plugin_code_open(dir, name, m, transport, h, aa, from, why, note);
    if ( ok )
    {
	plugin_activate_fn af = (plugin_activate_fn)aa;
	ok = af(&madcide_seat_api, w);
	if ( !ok )
	    why = format("its activation refused it ({})", from);
    }
    var done = { "plugin": plugin_verb_name(pvACTIVATED), "ok": ok, "why": why,
		 "note": note, "from": from };
    plugin_host_write(io, done);
    if ( !ok )
	return 1;
    var msg;
    while ( plugin_host_next(msg) )
    {
	var vw;
	keyed_get(vw, msg, "plugin");
	if ( plugin_verb_of(vw) == pvINVOKE )
	    plugin_host_run_invoke(msg);
    }
    return 0;
}
