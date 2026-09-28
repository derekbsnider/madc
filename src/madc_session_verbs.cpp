/* madc_session_verbs.cpp — the script surface of the session backend (plan
 * §41.9a, slice 2): <ns_madc>'s session_* verbs over SessionClient.
 *
 * A handle is a handle_table slot holding a client, the output a poll took
 * before the script asked for it, and the handle's select case. Every verb
 * returns at once; replies come through session_poll, and a pump task waits
 * on session_readable's case in chan_select.
 *
 * Thread contract: the table is per thread (thread_local), so a handle is
 * confined to the thread that opened it, as the parse handles are; the
 * select case is read on the scheduler thread, the same one.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "handle_table.h"
#include "madc_session_client.h"
#include "madc_task_io.h"	// readiness_source, chan_readiness
#include "ns_common.h"		// ring_text: the lean const char* return

namespace {

struct SessionHandle : public madc::taskio::readiness_source
{
    SessionClient client;
    std::string output;		// taken by a poll, not yet by session_output
    int64_t select_case;	// session_readable's chan handle, made once

    SessionHandle() : select_case(0) {}

    int64_t poll_state() override
    {
	if ( !output.empty() )
	    return 1;
	return client.pending();
    }
    void wait_handles(std::vector<madc::poll_handle> &out) override
    {
	client.wait_handles(out);
    }
};

handle_table<SessionHandle> &session_handles()
{
    thread_local handle_table<SessionHandle> handles;
    return handles;
}

SessionHandle *session_of(int64_t handle)
{
    return session_handles().get(handle);
}

madc::taskio::readiness_source *session_source(int64_t handle)
{
    return session_of(handle);
}

// A reply as a row: named fields, the kind and the verdict as their enum
// codes (<bits/session_enums>).
madc::value reply_row(const SessionClient::Reply &r)
{
    std::map<std::string, madc::value> f;
    f["kind"] = madc::value((int64_t)r.kind);
    f["seq"] = madc::value((int64_t)r.seq);
    switch ( r.kind )
    {
	case madc::session_reply::running:	// the kind and the seq say it all
	    break;
	case madc::session_reply::offer:
	    f["state"] = madc::value((int64_t)r.state);
	    f["ok"] = madc::value(r.ok);
	    f["shown"] = madc::value(r.shown);
	    f["rendered"] = madc::value(r.rendered);
	    f["submitted"] = madc::value((int64_t)r.submitted);
	    f["diagnostics"] = r.diagnostics.is_array() ? r.diagnostics : madc::value::make_array();
	    break;
	case madc::session_reply::complete:
	{
	    std::vector<madc::value> names;
	    for ( const std::string &n : r.names )
		names.push_back(madc::value(n));
	    f["start"] = madc::value((int64_t)r.start);
	    f["names"] = madc::value::make_array(names);
	    break;
	}
	case madc::session_reply::stopped:
	    f["exit_status"] = madc::value((int64_t)r.exit_status);
	    f["signal"] = madc::value((int64_t)r.signal);
	    break;
	case madc::session_reply::load:
	case madc::session_reply::run:
	    f["ok"] = madc::value(r.ok);
	    f["rendered"] = madc::value(r.rendered);
	    if ( r.kind == madc::session_reply::run )
		f["status"] = madc::value((int64_t)r.status);
	    break;
	case madc::session_reply::continues:
	    f["continues"] = madc::value(r.continues);
	    break;
    }
    return madc::value::make_object(f);
}

} // namespace

namespace madc {

int64_t session_open(const char *std_spelling)
{
    SessionHandle *s = new SessionHandle();
    // `c17` or `--std=c17`; empty keeps the default standard.
    const std::string prefix("--std=");
    std::string opt = std_spelling ? std_spelling : "";
    if ( !opt.empty() && opt.compare(0, prefix.size(), prefix) != 0 )
	opt = prefix + opt;
    s->client.start(opt);		// a refusal is the handle's state
    return session_handles().open(s);
}

bool session_running(int64_t handle)
{
    SessionHandle *s = session_of(handle);
    return s && s->client.running();
}

const char *session_error(int64_t handle)
{
    SessionHandle *s = session_of(handle);
    return ns_common::ring_text(s ? s->client.last_error()
				  : std::string("session: no such handle"));
}

const char *session_standard(int64_t handle)
{
    SessionHandle *s = session_of(handle);
    return ns_common::ring_text(s ? s->client.standard() : std::string());
}

int64_t session_offer(int64_t handle, const char *text, bool final)
{
    SessionHandle *s = session_of(handle);
    return s ? (int64_t)s->client.offer(text ? text : "", final) : 0;
}

int64_t session_complete(int64_t handle, const char *text, int64_t caret)
{
    SessionHandle *s = session_of(handle);
    if ( !s || caret < 0 )
	return 0;
    return (int64_t)s->client.complete(text ? text : "", (size_t)caret);
}

int64_t session_load(int64_t handle, const char *path, const char *text)
{
    SessionHandle *s = session_of(handle);
    if ( !s || !path )
	return 0;
    if ( text && *text )
	return (int64_t)s->client.load_text(text, path);
    return (int64_t)s->client.load(path);
}

int64_t session_run(int64_t handle, value &argv)
{
    SessionHandle *s = session_of(handle);
    if ( !s || !argv.is_array() )
	return 0;
    std::vector<std::string> args;
    for ( const value &a : argv.as_array() )
	args.push_back(a.is_string() ? a.as_string() : std::string());
    return (int64_t)s->client.run(args);
}

int64_t session_poll(value &reply, int64_t handle)
{
    SessionHandle *s = session_of(handle);
    if ( !s )
    {
	reply = value();
	return -1;
    }
    // A backend that ends is restarted here, as Thonny's and Jupyter's are:
    // its state is gone, the handle lives on (the row says whether the new
    // one started). One that was not running is not retried.
    const bool was_running = s->client.running();
    SessionClient::Reply r;
    const int got = s->client.poll(r, s->output, 0);
    if ( got == 0 )
    {
	reply = value();
	return 0;
    }
    reply = reply_row(r);
    if ( got < 0 )
	reply.object()["restarted"] = value(was_running && s->client.restart());
    return got;
}

value &session_output(value &out, int64_t handle)
{
    SessionHandle *s = session_of(handle);
    std::string text;
    if ( s )
    {
	s->client.take_output(s->output);
	text.swap(s->output);
    }
    out = value(text);
    return out;
}

bool session_input(int64_t handle, const char *text)
{
    SessionHandle *s = session_of(handle);
    return s && s->client.input(text ? text : "");
}

bool session_restart(int64_t handle)
{
    SessionHandle *s = session_of(handle);
    if ( !s )
	return false;
    s->output.clear();
    return s->client.restart();
}

int64_t session_readable(int64_t handle)
{
    SessionHandle *s = session_of(handle);
    if ( !s )
	return 0;
    if ( !s->select_case )
	s->select_case = taskio::chan_readiness(session_source, handle);
    return s->select_case;
}

bool session_close(int64_t handle)
{
    return session_handles().close(handle);	// the client's destructor stops it
}

} // namespace madc
