// madcide_discover.inc — SESSION DISCOVERY (V6c-4): the ADVERTISEMENT a
// client finds a running session by, so nobody has to be TOLD an address.
//
// ⚠️ IT IS AN ADVERTISEMENT, NOT A LOCK. madcide refuses no file — there is
// no swapfile mechanism here and never was, so there is nothing to exclude
// with. This publishes how to REACH a session; two sessions in one root both
// advertise, and a client picks. The V6c-3c relay already lets an editor, an
// agent and a browser drive ONE running session; this is what removes the
// human typing the port into three places.
//
// WHERE (ruling R1, plan 2026-09-15-v6c4): a STATE DIR, not the project root.
// The owner's analogy was neovim, whose own state (swapfiles, server sockets)
// lives under $XDG_STATE_HOME — and a `.madcide.lock` in a project root lands
// in the git status of every project madcide ever opens. $MADCIDE_SESSION_DIR
// overrides, which is also how the gates stay out of a developer's real state
// directory.
//
// STALENESS IS A CONNECT TEST, never a pid test: a crashed session leaves its
// file behind and a pid is reused. A file whose endpoint refuses connection is
// stale — remove it and carry on.
//
// Every failure here is NON-FATAL: no state dir, no writable file, no port —
// the session opens anyway, unadvertised. An editor that refuses to start
// because it could not advertise is a worse editor.
//
// The dialect surface this needs is ALL already there (probed at live HEAD in
// the JIT pass and the -o exe pass, plan §recon): getenv is a registered
// builtin with the real C shape, getpid resolves through the dlsym fallback,
// madc::canonical_path answers ".", perl::glob scans a directory and php::time
// is the wall clock. No engine change.

// The advertisement directory (R1's resolution order). "" = nowhere to
// publish (no HOME): the caller degrades to an unadvertised session.
void session_dir(var &out)
{
    out = "";
    const char *over = getenv("MADCIDE_SESSION_DIR");
    if ( over != 0 && strlen(over) > 0 )
    {
	out = over;
	return;
    }
    const char *xdg = getenv("XDG_STATE_HOME");
    if ( xdg != 0 && strlen(xdg) > 0 )
    {
	out = format("{}/madcide/sessions", xdg);
	return;
    }
    const char *home = getenv("HOME");
    if ( home != 0 && strlen(home) > 0 )
	out = format("{}/.local/state/madcide/sessions", home);
}

// Create the directory and every ancestor. php::mkdir is PHP's ONE level
// (there is no recursive flag), so walk the components; an already-existing
// level is not an error. False = it does not exist afterwards.
bool session_dir_ensure(var &dir)
{
    const char *d = dir.c_str();
    if ( strlen(d) == 0 )
	return false;
    if ( php::file_exists(d) )
	return true;
    array parts;
    php::explode(parts, "/", d);
    long n = php::count(parts);
    // An absolute path's leading empty component IS the root; a relative one
    // grows from the working directory.
    var grown = ".";
    if ( d[0] == '/' )
	grown = "";
    for ( long i = 0; i < n; i = i + 1 )
    {
	const char *seg = php::array_get_cstr(parts, i);
	if ( strlen(seg) == 0 )
	    continue;
	var next = format("{}/{}", grown, seg);
	grown = next;
	if ( !php::file_exists(grown.c_str()) )
	    php::mkdir(grown.c_str());
    }
    return php::file_exists(d);
}

// A filesystem-safe rendering of a root path, so `ls` of the directory reads
// as the sessions it holds.
void session_slug(var &out, var &root)
{
    var slashed = php::str_replace("/", "_", root);
    var spaced = php::str_replace(" ", "_", slashed);
    out = spaced;
    const char *p = spaced.c_str();
    if ( p[0] == '_' && strlen(p) > 1 )
    {
	var trimmed = perl::substr(spaced, 1, strlen(p) - 1);
	out = trimmed;
    }
}

// THE advertisement's path: one file per running session. The pid keeps two
// sessions in one root distinct — it advertises, it never excludes.
void session_file(var &out, var &dir, var &root, long pid)
{
    var slug;
    session_slug(slug, root);
    out = format("{}/{}-{}.json", dir, slug, pid);
}

// The documents this session holds, canonically. The buffer table is the
// truth once seeded; a headless session that has not opened a second buffer
// still has its launch document.
void session_documents(var &out, IdeSession &S, long doc)
{
    long w = S.world();
    long es = S.es;
    var list = {};
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() || php::count(bufs) == 0 )
    {
	var p;
	ui::get(p, w, doc, "path");
	if ( !p.is_null() )
	{
	    var c;
	    madc::canonical_path(c, p.c_str());
	    list.push(c);
	}
	out = list;
	return;
    }
    long n = php::count(bufs);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var r = bufs[i];
	var rp = r["path"];
	if ( rp.is_null() )
	    continue;
	var c;
	madc::canonical_path(c, rp.c_str());
	list.push(c);
    }
    out = list;
}

// The record itself — everything a client needs to connect, which is what the
// owner asked for. `serves` is what the connection seat accepts on this port:
// the three web seats always (serve_web classifies each connection), plus the
// two JSON-RPC dialects through the V6c-3c declared-dialect hello.
// ⚠️ The endpoint travels as a CARRIER, never as a `const char *`. A var's
// c_str() is a RING-lifetime rendering, and this function calls out four times
// (canonical_path, the document scan, format) before it places the value — long
// enough for the ring to cycle and the record to advertise some other string as
// the address. Cost the first run of this slice: `"endpoint":"tmp/discdoc.mad"`.
void session_record(var &out, IdeSession &S, long doc, var &endpoint)
{
    var root;
    madc::canonical_path(root, ".");
    var docs;
    session_documents(docs, S, doc);
    var url = format("http://{}/", endpoint);
    var serves = { "api", "ws", "http", "lsp", "mcp" };
    long pid = getpid();
    long started = php::time();
    var version = madc::sys.version;
    var host = madc::sys.hostname;
    out = { "madcide": 1,
	    "pid": pid,
	    "started": started,
	    "version": version,
	    "host": host,
	    "root": root,
	    "endpoint": endpoint,
	    "url": url,
	    "serves": serves,
	    "documents": docs };
}

// Publish. The file this session advertised through — and the endpoint it
// advertised — are the ADVERTISEMENT's own state, so they live on the session
// bag (`advert` / `advertep`) rather than being carried through every serving
// entry point: refresh and withdraw then need nothing but the session.
// Advertising is best-effort; a session that could not publish simply runs
// unadvertised.
void session_advertise(IdeSession &S, long doc, var &endpoint)
{
    long w = S.world();
    long es = S.es;
    ui::set(w, es, "advert", "");
    ui::set(w, es, "advertep", endpoint);
    var dir;
    session_dir(dir);
    if ( !session_dir_ensure(dir) )
	return;
    var root;
    madc::canonical_path(root, ".");
    long pid = getpid();
    var path;
    session_file(path, dir, root, pid);
    var rec;
    session_record(rec, S, doc, endpoint);
    var js = js::stringify(rec);		// ring lifetime: owned here
    if ( php::file_put_contents(path.c_str(), js.c_str()) <= 0 )
	return;
    ui::set(w, es, "advert", path);
}

// Re-publish an already-advertised session (its document set moved on). Silent
// when this session never advertised.
void session_refresh(IdeSession &S, long doc)
{
    long w = S.world();
    long es = S.es;
    var path;
    ui::get(path, w, es, "advert");
    if ( path.is_null() || strlen(path.c_str()) == 0 )
	return;
    var ep;
    ui::get(ep, w, es, "advertep");
    if ( ep.is_null() )
	return;
    var rec;
    session_record(rec, S, doc, ep);
    var js = js::stringify(rec);
    php::file_put_contents(path.c_str(), js.c_str());
}

// Withdraw. Idempotent, and never an error: a session that could not advertise
// has nothing to remove.
void session_withdraw(IdeSession &S)
{
    long w = S.world();
    long es = S.es;
    var path;
    ui::get(path, w, es, "advert");
    if ( path.is_null() || strlen(path.c_str()) == 0 )
	return;
    php::unlink(path.c_str());
    ui::set(w, es, "advert", "");
}

// THE liveness test: can anyone still reach this endpoint? A pid test would
// lie (pids are reused, and a live pid may have stopped serving); a refused
// connection is the honest answer to the only question a client has.
bool session_alive(const char *endpoint)
{
    var uri = format("tcp://{}", endpoint);
    madc::channel probe(uri.c_str());
    bool up = probe.ok();
    probe.close();
    return up;
}

// Every session advertised AND still reachable. A record whose endpoint is
// gone is stale: its file is removed here, so the directory self-heals after a
// crash without anyone running a cleanup. A file that does not parse is left
// alone — it may not be ours.
void session_scan(var &out)
{
    out = {};
    var dir;
    session_dir(dir);
    if ( strlen(dir.c_str()) == 0 )
	return;
    var pattern = format("{}/*.json", dir);
    array hits;
    perl::glob(hits, pattern.c_str());
    long n = php::count(hits);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var f = php::array_get_cstr(hits, i);
	var text;
	if ( !php::file_get_contents(text, f.c_str()) )
	    continue;
	var rec;
	if ( !js::parse(rec, text.c_str()) )
	    continue;
	var ep = rec["endpoint"];
	if ( ep.is_null() )
	    continue;
	if ( !session_alive(ep.c_str()) )
	{
	    php::unlink(f.c_str());		// stale: the connect test is the authority
	    continue;
	}
	rec["file"] = f;
	out.push(rec);
    }
}

// Does `root` COVER `file`? A path prefix at a COMPONENT BOUNDARY — "/a/b"
// covers "/a/b/c" and "/a/b" itself, but never "/a/bc". A bare strncmp would
// match the last, which is the bug this shape exists to refuse.
bool session_covers(var &root, var &file)
{
    const char *r = root.c_str();
    const char *f = file.c_str();
    long rl = strlen(r);
    if ( rl == 0 )
	return false;
    if ( rl > 1 && r[rl - 1] == '/' )
	rl = rl - 1;			// a trailing separator is not part of it
    if ( strncmp(f, r, rl) != 0 )
	return false;
    return f[rl] == '/' || f[rl] == 0;
}

// The session that covers `file`, best first: a session that HOLDS the file
// beats one that merely covers its root. False = nothing running matches.
bool session_find(var &out, const char *file)
{
    var want;
    madc::canonical_path(want, file);
    var all;
    session_scan(all);
    long n = php::count(all);
    long covering = -1;
    for ( long i = 0; i < n; i = i + 1 )
    {
	var rec = all[i];
	var docs = rec["documents"];
	long dn = php::count(docs);
	for ( long k = 0; k < dn; k = k + 1 )
	{
	    var d = docs[k];
	    if ( d == want )
	    {
		out = rec;
		return true;		// an exact holder wins outright
	    }
	}
	if ( covering < 0 )
	{
	    var root = rec["root"];
	    if ( !root.is_null() && session_covers(root, want) )
		covering = i;
	}
    }
    if ( covering < 0 )
	return false;
    out = all[covering];
    return true;
}

// ONE rendering of a session for a human — `--sessions`, the editor's status
// line and the attach hint all read it, so they never drift apart.
void session_report(var &out, var &rec)
{
    var ep = rec["endpoint"];
    var pid = rec["pid"];
    var root = rec["root"];
    var docs = rec["documents"];
    long dn = php::count(docs);
    out = format("{}  pid {}  {}  {} doc(s)", ep, pid, root, dn);
}
