// madcide_base.inc — madcide's BASE (plugin design §9.3; docs/plans/
// 2026-10-03-chthonia-windows-macos.md §7 D2): every layer of the IDE, and
// madcide_main, the entry a product calls with its descriptor
// (<madcide/product>). madcide.mad is this plus madcide's own descriptor and
// main; madcide_base.mad is this alone, built as the library a product
// links (libmadcide).
//
// Thread contract: madcide_main runs on the main thread; it records the
// product (product_use) before any session or thread starts.
#include "../texteditor/lined_core.inc"
#include "../texteditor/editor_events.inc"
#include "madcide_core.inc"
#include "madcide_args.inc"	// THE command line (the one argv reader)
// SESSION DISCOVERY (V6c-4) sits directly on the session: the ADVERTISEMENT a
// client finds a running session by, so nobody has to be told an address. It
// is read by every face that listens (the serving processes below) and by the
// editor itself, which listens by default.
#include "madcide_discover.inc"
#include "madcide_client.inc"
#include "madcide_api.inc"
#include "madcide_once.inc"
#include "madcide_line.inc"
#include "madcide_mcp.inc"
// The LSP face sits BEFORE the connection seat, which routes LSP traffic to
// lsp_handle (V6c-3c), and forward-declares the one thing it needs from the
// serve seat below it (serve_conn_task — `--lsp --serve` is ONE process
// wearing both faces, V6c-3b).
#include "madcide_lsp.inc"
#include "madcide_seat.inc"
#include "madcide_ws.inc"
#include "madcide_serve.inc"

// The base's entry (<madcide/product>): the product recorded, then the
// command line read and the face it asks for opened.
int madcide_main(int argc, char **argv, const ide_product &product)
{
    product_use(product);
    // THE command line (madcide_args.inc): flags anywhere, the first word
    // that is not a flag is the file, an unknown flag refused with the usage.
    ide_args a;
    var why;
    bool parsed = ide_args_parse(a, why, argc, (const char **)argv);
    // A face that speaks on the standard streams (the help, a refusal, the
    // terminal, a one-shot, a server) needs a console: a Windows GUI image
    // (chthonia's) starts without one, so it attaches to its parent's. The
    // window face never does — closing that console would end the window.
    if ( !parsed || !ide_face_is_window(a) )
	madc::console_attach();
    if ( !parsed )
    {
	println(stderr, "{}: {}", product.name, why);
	var usage;
	ide_usage(usage);
	print(stderr, "{}", usage);
	return 2;
    }
    if ( a.help )
    {
	var usage;
	ide_usage(usage);
	print("{}", usage);
	return 0;
    }
    // `--sessions` answers before anything else: it needs no file, because it
    // is the question "what is already running?" (V6c-4).
    if ( a.sessions )
	return run_sessions();
    // `--build-plugin DIR` builds a plugin's library and exits: no session,
    // no file (madcide_plugin_code.inc).
    if ( a.build_plugin != NULL )
	return run_build_plugin(a.build_plugin);
    // No file: the editor opens an untitled buffer (Thonny's, JOE's
    // Unnamed); a headless face has nothing to act on without one.
    if ( a.file == NULL && ide_face_needs_file(a) != NULL )
    {
	println(stderr, "{}: {} needs a file", product.name, ide_face_needs_file(a));
	var usage;
	ide_usage(usage);
	print(stderr, "{}", usage);
	return 2;
    }
    // The UI LEVEL is ONE flag (ui::level, owner 2026-09-09: a program
    // names the rendering model it wants, never a target's spelling): --gui
    // shows the same composed tree in a window through the web level
    // (<ns_ui_web>, a lazy module row — a machine without the library
    // refuses at open with the reason); --line is the ui::LINE ex / edlin
    // mode (stdin lines, the level-0 projection to stdout — over a pipe or a
    // dumb terminal); the grid (ui::TUI) is the default;
    // -c "<command> [arg]" is the ui::NONE level — no surface, one command,
    // the projection printed, the exit status the verdict (run_once). `ro`
    // opens the file read-only. The client loop and the composer are
    // identical for every level (the workbench layout rides layout hints
    // the terminal ignores; the one-shot typesets the same tree). The
    // profile (--profile NAME; "" = settings.json's, else default) and the
    // standard (--std=, a kind; 0 = settings.json's, else each file's) select
    // no face: every face opens its session under them.
    const char *file = a.file == NULL ? "" : a.file;	// "" = untitled
    bool ro = a.ro;
    ui::level lvl = a.level;
    const char *once = a.once;
    const char *serve = a.serve;
    bool mcp = a.mcp;
    bool lsp = a.lsp;
    bool attach = a.attach;		// --attach was asked for
    const char *attach_addr = a.attach_addr;	// ... at this address, or NULL = discover
    bool no_serve = a.no_serve;
    const char *profile = a.profile == NULL ? "" : a.profile;
    if ( attach )
    {
	// The ATTACH RELAY (V6c-3c): become a client of a session that is
	// ALREADY RUNNING instead of opening a private one, so an editor, an
	// agent and a browser share one session's buffers. Pure transport — no
	// session opens here, and <file> is unused (the session has its own
	// documents; the editor adds others with didOpen).
	if ( !lsp && !mcp )
	{
	    println(stderr, "{}: --attach needs --lsp or --mcp", product_in_use().name);
	    return 2;
	}
	// No address given: DISCOVER the session that holds <file> (V6c-4) —
	// which is the whole point of the advertisement, and what the VS Code
	// extension's `madcide.attach: "auto"` passes.
	var found_addr = "";
	if ( attach_addr == NULL )
	{
	    var rec;
	    if ( !session_find(rec, file) )
	    {
		println(stderr,
			"{}: no running session holds {} — start one, or give --attach <host:port>",
			product_in_use().name, file);
		return 2;
	    }
	    found_addr = rec["endpoint"];
	    var line;
	    session_report(line, rec);
	    println(stderr, "{}: discovered {}", product_in_use().name, line);
	    return run_attach(found_addr.c_str(), lsp);
	}
	return run_attach(attach_addr, lsp);
    }
    if ( once != NULL )
    {
	var out;
	long rc = run_once(out, file, ro, once, profile, a.std);
	print("{}", out);
	return rc;
    }
    if ( mcp )
    {
	// The MCP server (V6c): headless, one client over stdio speaking
	// JSON-RPC 2.0 — a tool call is an `api` command by name (run_mcp is an
	// adapter over the SAME command core the --serve api seat drives). The
	// standard MCP deployment: an editor / agent host spawns this process.
	return run_mcp(file, ro, profile, a.std);
    }
    if ( lsp )
    {
	// The LANGUAGE SERVER (V6c-2): headless, one client over stdio
	// speaking JSON-RPC 2.0 framed by Content-Length — the standard LSP
	// deployment, where the editor spawns this process. The second face of
	// the same query layer the MCP seat serves, so every editor that
	// speaks LSP drives madcide without adopting madcide's own frontend.
	// With `--serve` beside it (V6c-3b) this ONE process wears BOTH faces
	// on ONE session: LSP on stdio for the editor's language intelligence,
	// api/ws/http on a loopback port for a window (a VS Code webview, a
	// browser) that joins the very same buffers. Two processes would be
	// two sessions on one file — two carets, two undo histories, last save
	// wins.
	return run_lsp(file, ro, serve, profile, a.std);
    }
    if ( serve != NULL )
    {
	// The headless server (V6a api + V6b ws/http): no local window, one
	// port carrying api JSON-line clients, ws WINDOW clients (a browser's
	// page.js over the socket) and plain HTTP GETs for the page — the SAME
	// session and registry the local windows drive (run_serve classifies
	// each connection with serve_web).
	return run_serve(file, ro, serve, profile, a.std);
    }
    if ( lvl == ui::LINE )
    {
	// The ex / edlin client (V2.5): stdin lines in, the level-0
	// projection out — its own loop, as the one-shot has its own, but
	// the SAME session and composer (run_line).
	run_line(file, ro, profile, a.std);
	return 0;
    }
    // The editor — which LISTENS by default (V6c-4 ruling R2) on a loopback
    // ephemeral port and advertises it, so an LSP editor, an agent's MCP client
    // or a browser joins THIS session instead of opening a second one on the
    // same file. `--no-serve` opts out. ⚠️ NO AUTH, NO TLS: loopback only.
    const char *listen_at = no_serve ? NULL : "127.0.0.1:0";
    return run_tui(file, ro, lvl, listen_at, profile, a.std);
}
