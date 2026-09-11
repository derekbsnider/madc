// madcide_api.inc — the trAPI CLIENT SEAT (client-server design 2026-09-09
// §2.7, slice V6a). A madcide session with ZERO windows serving remote
// clients over a byte channel: one JSON line per message. A request is
//   {"cmd":"<name>", "args":"<arg>", "seq":<n>}
// and a reply is
//   {"seq":<n>, "ok":<bool>, "errors":<n>, "text":"<projection>"}
// or, on refusal,
//   {"seq":<n>, "ok":false, "error":"<why>"}.
// The command NAME resolves ONCE here, the channel boundary, through the
// registry's one table (cmd_of — the SAME table the profiles, menus and the
// -c one-shot load through: separation of concerns, Rule #5); the rest is
// the command's ARGUMENT, exactly as IdeSession::command takes it. The seat
// never learns a command's grammar — it drives the session directly, as the
// headless harness and run_once do. The MCP seat and the LSP endpoint (V6c)
// are adapters over THIS protocol.
//
// Transport: the accepted connection is an ordinary madc::channel byte
// stream (the listen:// acceptor, V6a step 1/2a). This first slice serves
// accepted clients SEQUENTIALLY (request/reply until a client hangs up);
// cooperative multi-client concurrency, the {"event":…} push feed off the
// V4 change-event log (a client's last_seq cursor), and the permission
// tiers are the follow-on V6a slices. A remote ":q" ends only that client's
// connection, never the server (cont is not honored here — a client leaves
// by disconnecting).

// Read the request's optional text ARGUMENT as a C string ("" when absent).
const char *api_arg(var &req)
{
    var a = req["args"];
    if ( a.is_null() )
	return "";
    return a.c_str();
}

// Write one JSON line (the JSONL framing the client reads with a line read).
void api_write(madc::channel &client, var &reply)
{
    var js = js::stringify(reply);
    var line = format("{}\n", js);
    client.write(line.c_str());
}

// A refusal reply — never a silent drop (the -c seat's contract over a
// channel): the request's seq rides back so the client can correlate.
void api_refuse(madc::channel &client, long seq, const char *why)
{
    var reply = { "seq": seq, "ok": false, "error": why };
    api_write(client, reply);
}

// Serve ONE connected client: read JSON-line requests until EOF, running
// each on the shared session and replying with the verdict + the composed
// projection (the same tree ui::render_tree typesets for the -c seat).
void serve_api_client(IdeSession &S, long doc, madc::channel &client)
{
    var line;
    while ( client.readline(line) )
    {
	var req;
	if ( !js::parse(req, line) )
	{
	    api_refuse(client, 0, "malformed JSON request");
	    continue;
	}
	long seq = req["seq"].as_integer();
	var name = req["cmd"];
	if ( name.is_null() )
	{
	    api_refuse(client, seq, "request has no 'cmd'");
	    continue;
	}
	long code = cmd_of(name.c_str());
	if ( code == cmdNONE )
	{
	    var why = format("unknown command '{}'", name);
	    api_refuse(client, seq, why.c_str());
	    continue;
	}
	long adoc = S.active(doc);
	bool cont = true;
	bool ok = S.command(adoc, code, api_arg(req), cont);
	long errors = S.error_count();
	var tree;
	S.compose_ide_tree(tree, S.active(adoc));
	var text;
	ui::render_tree(text, S.world(), tree);
	var reply = { "seq": seq, "ok": ok, "errors": errors, "text": text };
	api_write(client, reply);
    }
}

// The trAPI SERVER: open the session headless (no window), bind a listener,
// and accept clients as they arrive, serving each in turn. `addr` is the
// listen authority (host:port; :0 asks the OS for an ephemeral port, which
// the startup line reports). Returns 1 only when the file or the listener
// could not open (the reason on stderr); the serve loop itself runs until
// the listener dies (the process is stopped).
long run_serve(const char *path, bool ro, const char *addr)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    S.terminal(false);		// headless: no interactive terminal

    var uri = format("listen://{}", addr);
    madc::channel listener(uri.c_str());
    if ( !listener.ok() )
    {
	println(stderr, "madcide: cannot serve on {}: {}",
		addr, listener.last_error());
	S.close();
	return 1;
    }
    println(stderr, "madcide: serving {} on {} (api)",
	    path, listener.local_endpoint());

    bool serving = true;
    while ( serving )
    {
	if ( !listener.wait_readable() )
	    break;			// the listener died
	madc::channel client;
	long got = listener.accept(client);
	if ( got < 0 )
	    break;			// accept failed
	if ( got == 0 )
	    continue;			// spurious wake — nothing pending
	serve_api_client(S, doc, client);
	client.close();
    }
    S.close();
    return 0;
}
