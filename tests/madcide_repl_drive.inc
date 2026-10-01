// madcide_repl_drive.inc — driving an IdeSession's REPL pane from a test:
// the event builders a client sends (text, key, action) and the settle that
// lets the session's pump answer. Shared by tests/testmadcide_repl.mad and
// tests/testmadcide_chthonic.mad; included after madcide_core.inc.

void ev_text(var &e, const char *t)
{
    var f = { "event": "text", "event_code": ui::event_kind::text, "text": t };
    e = f;
}

void ev_key(var &e, const char *k)
{
    var f = { "event": "key", "event_code": ui::event_kind::key, "key": k,
	      "key_code": ui::key_code(k) };
    e = f;
}

void ev_action(var &e, const char *a, const char *seq)
{
    var f = { "event": "action", "event_code": ui::event_kind::action,
	      "action": a, "seq": seq };
    e = f;
}

// Let the pump run until nothing is outstanding (or `ms` passed): an offer
// answered, a completion answered — or, with `running`, until the offered
// entry has been taken and runs.
void settle(long w, long es, bool running)
{
    for ( long waited = 0; waited < 20000; waited = waited + 10 )
    {
	if ( running && es_int(w, es, "replrunning", 0) != 0 )
	    return;
	if ( !running && es_int(w, es, "replbusy", 0) == 0
	     && es_int(w, es, "replcseq", 0) == 0
	     && es_int(w, es, "replbseq", 0) == 0 )
	    return;
	madc::sleep_ms(10);
    }
    println("settle: timed out");
}

// Type a line into whatever has the keyboard, then press enter and settle.
void enter_line(IdeSession &S, long doc, long w, long es, const char *text)
{
    var e;
    if ( strlen(text) > 0 )
    {
	ev_text(e, text);
	S.apply_ide_event(doc, e);
    }
    ev_key(e, "enter");
    S.apply_ide_event(doc, e);
    settle(w, es, false);
}
