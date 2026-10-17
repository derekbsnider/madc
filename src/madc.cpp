#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <new>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_builtin_registry.h"
#include "madc_dl.h"
#include "madc_modules.h"	// -l<name> -> the target library spelling (the one owner)
#include "madc_guards.h"	// the resource guards (install + the GUI lift)
#include "madc_crash.h"	// fault reporter + guard-handler writers (own TU: windows.h vs tokens.h)
#include "madc_console.h"	// -fdiagnostics-color: colour only when the stream shows it
#include "madc_posix_io.h"	// cross-platform temporary file owner (--freeze-run)
#include "madcdis/process.h"	// Process::run_and_wait (--freeze-run re-exec)
#include "madc_pch.h"
#include "madc_config.h"  // madc.ini reader (forest-carriers S6)
#include "madc_file_kinds.h"  // file_kind_of_path: the source's standard by extension
#include "cir_emit_c.h"   // CirEmitLang
#include "madc_capabilities.h" // --capabilities=json (source-free manifest)
#include "madc_cli.h"	// madc's own command-line table
#include "madc_sys_includes.h" // -dumpmachine: the toolchain target triple
#include "madc_project.h" // --project: compile_commands.json multi-TU driver
#include "cir_freeze.h"	// --freeze-role=: the forest role table (CirForestRole)

#include "madc_cir.h"     // madc_cir_execute/emit/freeze/emit_native + MadcNativeKind
#include "madc_session.h" // InteractiveSession: the REPL's session (D20)
#include "madc_session_client.h" // BackendSession: the session in a backend (D2)
#include "madc_repl.h"    // madc_repl_run: the REPL's loop (D20)
#include "madc_history.h" // the REPL history store's default place (1B.7)
#include "madc_theme.h"   // the REPL's colours: madc.ini's theme (1B.9)
#include "madc_lsp.h"     // madc --lsp: the LSP server (split arc 1C.12)
#include "madc_mcp.h"     // madc --mcp: the MCP server (split arc 1C)
#include "madc_memory.h"  // madc --mcp --project: the memory's file
#include "madc_tool_host.h"	// madc --mcp: the tools a madc process serves
#include "madc_discovery.h"	// madc --mcp --project: one owner per project
#include "madc_jsonrpc.h"	// madc --mcp --project: the owner's record
#include "madc_lang.h"	// madc --mcp --project: a TU's document parses as its TU
#include "madcdis/channel.h" // madc --lsp: stdin/stdout, Content-Length framed
#include "madc_ex.h"      // colon mode: the host's ex session (D24)
#include "madc_documents.h" // the host's buffer: madc -i FILE's file
#include "madcdis/hub.h"     // the host's world of buffers
#include "madc_run_child.h" // a Run child of self (Windows: no fork)
#include "madcdis/tui_provider.h"	// create_line_target: the editor's terminal (D23)

// Supplied by the build as -DMADC_VERSION_STR='"x.y.z"' from ../VERSION (the
// version-consuming objects depend on that file — src/Makefile). The fallback
// matches lexer.cpp's and cir_freeze.cpp's so an ad-hoc compile still builds.
#ifndef MADC_VERSION_STR
#define MADC_VERSION_STR "0.0.0"
#endif

using namespace std;

// CLI-only active program pointer used by the crash handler to map a
// faulting JIT RIP back to source. Library consumers should provide
// their own crash/error plumbing instead of relying on process globals.
static Program *g_active_program = NULL;

double time_diff(struct timeval x , struct timeval y)
{
	double x_ms , y_ms , diff;
	
	x_ms = (double)x.tv_sec*1000000 + (double)x.tv_usec;
	y_ms = (double)y.tv_sec*1000000 + (double)y.tv_usec;
	
	diff = (double)y_ms - (double)x_ms;
	
	return diff;
}

// Resource guards: src/madc_guards.cpp (owner ruling 2026-09-07 — no guard
// arms by default; knobs off|auto|N; an armed memory guard is a soft limit a
// GUI module lifts at run start).

// Walk backwards from a line to include preceding comment block.
static int find_comment_start(const std::vector<std::string> &lines, int func_line)
{
    int start = func_line;
    while ( start > 0 )
    {
	const std::string &prev = lines[start - 1];
	size_t first = prev.find_first_not_of(" \t");
	if ( first != std::string::npos && prev.compare(first, 2, "//") == 0 )
	    --start;
	else if ( prev.empty() || first == std::string::npos )
	{
	    if ( start > 1 )
	    {
		const std::string &prev2 = lines[start - 2];
		size_t f2 = prev2.find_first_not_of(" \t");
		if ( f2 != std::string::npos && prev2.compare(f2, 2, "//") == 0 )
		    --start;
		else
		    break;
	    }
	    else
		break;
	}
	else
	    break;
    }
    return start;
}

// Walk forward from a line tracking brace depth (with comment/string
// awareness) to find the closing brace of a function body.  Returns
// the 0-based line index of the closing brace, or -1 on failure.
static int find_closing_brace(const std::vector<std::string> &lines, int func_line)
{
    int depth = 0;
    bool in_body = false;
    enum { NORMAL, IN_STR, IN_CHR, IN_BCOMMENT } state = NORMAL;

    for ( size_t j = (size_t)func_line; j < lines.size(); ++j )
    {
	const std::string &ln = lines[j];
	for ( size_t k = 0; k < ln.size(); ++k )
	{
	    char c = ln[k];
	    char next = (k + 1 < ln.size()) ? ln[k + 1] : '\0';

	    switch ( state )
	    {
	    case NORMAL:
		if ( c == '/' && next == '/' )
		    goto next_line;
		if ( c == '/' && next == '*' )
		    { state = IN_BCOMMENT; ++k; break; }
		if ( c == '"' )  { state = IN_STR; break; }
		if ( c == '\'' ) { state = IN_CHR; break; }
		if ( c == '{' )  { ++depth; in_body = true; }
		else if ( c == '}' )
		{
		    --depth;
		    if ( in_body && depth == 0 )
			return (int)j;
		}
		break;
	    case IN_STR:
		if ( c == '\\' ) { ++k; break; }
		if ( c == '"' ) state = NORMAL;
		break;
	    case IN_CHR:
		if ( c == '\\' ) { ++k; break; }
		if ( c == '\'' ) state = NORMAL;
		break;
	    case IN_BCOMMENT:
		if ( c == '*' && next == '/' )
		    { state = NORMAL; ++k; }
		break;
	    }
	}
	next_line:;
    }
    return -1;
}

// Text-based function extraction: find the function signature by name
// in the raw source text, then use brace-counting to find the body.
// Works on any C/C++ source without needing the madc parser.
static int emit_function_text(const std::vector<std::string> &lines, const char *funcname)
{
    std::string target(funcname);

    for ( size_t i = 0; i < lines.size(); ++i )
    {
	size_t pos = lines[i].find(target);
	if ( pos == std::string::npos )
	    continue;
	// Must be followed by ( (possibly with whitespace)
	size_t after = pos + target.size();
	while ( after < lines[i].size() && lines[i][after] == ' ' )
	    ++after;
	if ( after >= lines[i].size() || lines[i][after] != '(' )
	    continue;
	// Must be a definition (at column 0 or after a type), not indented code
	if ( pos > 0 && (lines[i][0] == ' ' || lines[i][0] == '\t') )
	    continue;
	// Skip lines where the match is inside a comment
	size_t comment_pos = lines[i].find("//");
	if ( comment_pos != std::string::npos && comment_pos < pos )
	    continue;

	int end = find_closing_brace(lines, (int)i);
	if ( end < 0 )
	{
	    std::cerr << "Found '" << funcname << "' at line " << (i + 1)
		      << " but could not find closing brace" << std::endl;
	    return 1;
	}

	int start = find_comment_start(lines, (int)i);
	for ( int j = start; j <= end; ++j )
	    std::cout << lines[j] << '\n';
	return 0;
    }

    std::cerr << "Function '" << funcname << "' not found" << std::endl;
    return 1;
}

// --emit-function: extract a complete function by name from a source file.
// For .mad files: tokenizes and parses using the madc parser, then uses
// the token's line position to locate the function in the original source.
// For other files (C/C++): falls back to text-based brace-matching.
// Both paths emit the function verbatim, including preceding comment block.
static int emit_function(Program &prog, const char *filepath, const char *funcname)
{
    // Read original source lines for verbatim output
    std::ifstream in(filepath);
    if ( !in )
    {
	std::cerr << "Cannot open " << filepath << std::endl;
	return 1;
    }
    std::vector<std::string> lines;
    std::string line;
    while ( std::getline(in, line) )
	lines.push_back(line);
    in.close();

    // Try the parser path first (works for .mad files).
    // Tokenize and parse to find functions.
    TokenProgram *tp = prog.tokenize(filepath);
    if ( tp && prog.parse(tp) )
    {
	// Search pending_funcs for a match
	std::string target(funcname);
	for ( TokenBase *tb : prog.pending_funcs )
	{
	    TokenFunc *tf = dynamic_cast<TokenFunc *>(tb);
	    if ( !tf ) continue;
	    if ( tf->var.name != target ) continue;

	    int func_line = tf->line - 1;
	    int end = tf->end_line - 1;
	    if ( func_line >= 0 && (size_t)func_line < lines.size()
	      && end >= func_line && (size_t)end < lines.size() )
	    {
		int start = find_comment_start(lines, func_line);
		for ( int j = start; j <= end; ++j )
		    std::cout << lines[j] << '\n';
		return 0;
	    }
	}
    }

    // Parser path didn't find it — fall back to text-based extraction.
    // This handles C/C++ source and cases where parse failed.
    return emit_function_text(lines, funcname);
}

#ifdef MADC_CROSS_TARGET
// Cross build (gcc cross model): this binary EMITS native artifacts for
// MADC_CROSS_TARGET and can never execute target code on this host. Every
// run lane refuses loudly through here.
static int cross_refuse_run(const char *lane)
{
    std::cerr << "madc: cross build for " MADC_CROSS_TARGET
              << " is emit-only: cannot " << lane << " on this host."
                 " Use -c / -o / -shared / -r / --emit=..., then run the"
                 " artifact on the target." << std::endl;
    return 1;
}
#endif

// gcc's -Wl,<list>: the comma-split options the linker would see. madc is
// its own linker, so it reads what it implements — `-rpath <dir>` /
// `-rpath=<dir>`, appended to `runpath` — and false with `refused` naming the
// first option it does not, so an option is never dropped silently.
static bool read_linker_options(const char *list, std::vector<std::string> &runpath,
                                std::string &refused)
{
    std::vector<std::string> words;
    std::string word;
    for ( const char *p = list; ; p++ )
    {
        if ( *p == ',' || *p == '\0' )
        {
            words.push_back(word);
            word.clear();
            if ( *p == '\0' )
                break;
        }
        else
            word += *p;
    }
    for ( size_t i = 0; i < words.size(); i++ )
    {
        if ( words[i] == "-rpath" && i + 1 < words.size() && !words[i + 1].empty() )
            runpath.push_back(words[++i]);
        else if ( words[i].compare(0, 7, "-rpath=") == 0 && words[i].size() > 7 )
            runpath.push_back(words[i].substr(7));
        else
        {
            refused = words[i];
            return false;
        }
    }
    return true;
}

// The preprocess-only stop (-E / --dump-source, -dM): its text goes to the
// file -o names, else stdout (gcc). -dM wins over -E, as in `gcc -dM -E`.
static int write_preprocessed_output(Program &prog, bool macros, const char *path)
{
    FILE *out = path ? fopen(path, "w") : stdout;
    if ( !out )
    {
        std::cerr << "madc: cannot open output file " << path << ": "
                  << strerror(errno) << std::endl;
        return 1;
    }
    if ( macros )
        prog.dump_macros(out);
    else
    {
        auto text = prog.reconstruct_source();
        fwrite(text.data(), 1, text.size(), out);
    }
    if ( out != stdout && fclose(out) != 0 )
    {
        std::cerr << "madc: cannot write output file " << path << ": "
                  << strerror(errno) << std::endl;
        return 1;
    }
    return 0;
}

// The file lane's one read of its input (plan §41.5a): `-` names stdin, as
// in gcc and python, read whole into the buffer lane; any other argument
// names a file.
static TokenProgram *tokenize_input(Program &prog, const char *path)
{
    if ( strcmp(path, "-") != 0 )
	return prog.tokenize(path);
    std::string text((std::istreambuf_iterator<char>(std::cin)),
		     std::istreambuf_iterator<char>());
    return prog.tokenize_buffer(text, "<stdin>");
}

// The REPL (D20): the session adopts the Program the command line and
// madc.ini configured, and the loop runs over stdin and stdout, prompting
// when stdin is a terminal. With a program file (`-i file`, python -i; plan
// §41.5a, slice 2) the file loads into the session first and its main, when
// it has one, runs with the file lane's argv (the path, then the program's
// arguments). The prompt follows whatever they did, as python's does: a
// refused file leaves nothing, a stopped main leaves the file's names.
// `history` is the line editor's history file (D23; empty: none); `theme`
// colours each entry as it is typed (split arc 1B.9).
static int repl_loops(ReplSession &session, bool terminal,
		      const std::string &history, const madc::theme *theme,
		      int file_argc, char **file_argv)
{
    // Colon mode (D24; ex core design decision 11) is this host's: an ex
    // session over the host's own world, holding ONE buffer as vim starts
    // with one — FILE for madc -i FILE, else an untitled buffer.
    madc::hub::world buffers;
    madc::ex::session ex(buffers);
    madc::hub::entity_id buffer =
	file_argc > 0 ? madc::document_open(buffers, file_argv[0]) : 0;
    if ( !buffer )
	buffer = madc::document_new(buffers, file_argc > 0 ? file_argv[0] : "");
    ex.use(buffer);
    repl_colon colon;
    colon.ex = &ex;
    colon.err = &std::cerr;
    // The session's commands on a buffer (%check; split arc 1B.8, design
    // decision 1) read these: a FILE's buffer, "" the current one, by its
    // live text; a FILE no buffer holds is the disk's.
    session.set_buffers([&buffers, &ex](const madc::dis::istring &name,
					madc::dis::istring &path, std::string &text,
					bool &has_text, std::string &why) -> bool {
	return madc::document_buffer(buffers, ex.current(), name, path, text,
				     has_text, why);
    });
    int status = 0;
    if ( file_argc > 0 )
    {
	session.run_file(file_argc, file_argv);
	if ( session.ended(status) )
	    return status;		// the file's exit(n) ends madc
    }
    else if ( terminal )
	std::cout << "madc " << MADC_VERSION_STR << ". Ctrl-D exits."
		  << std::endl;
    // A VT terminal edits each entry (D23); anything else reads cooked
    // lines (a pipe, a dumb terminal, a console without VT mode).
    if ( terminal )
    {
	std::unique_ptr<madc::hub::line_target> term(
	    madc::hub::create_line_target());
	if ( term )
	{
	    int rc = madc_repl_edit(session, *term, std::cout, history, colon,
				    theme);
	    if ( rc >= 0 )
		return rc;
	}
    }
    // Cooked lines come byte by byte from the OS, so the backend's entry
    // reads the stdin bytes after them (one stdin for both).
    madc::shared_stdin_input shared;
    std::istream in(&shared);
    return madc_repl_run(session, in, std::cout, terminal, colon, theme);
}

// madc --lsp [FILE] (split arc 1C.12): the Language Server Protocol on
// stdin/stdout, Content-Length framed. The configured Program is the active
// runtime Program for the server's life, so every document's parse is its
// child and searches its include directories (-I, madc.ini's `include`) as a
// compile does; a document's standard is its kind's. workspace/executeCommand
// runs in a backend session as -i's does — the configured Program again on
// each restart (POSIX: the fork's copy; Windows: `options` configure the
// backend's) — its program's output piped, never the protocol's stdout.
static int run_lsp(std::unique_ptr<Program> prog, const char *file,
		   const std::vector<std::string> &options)
{
    madc::channel io("stdio://");
    if ( !io.frame_headers() )
    {
	std::cerr << "madc: --lsp: stdin/stdout cannot carry framed messages"
		  << std::endl;
	return 1;
    }
#ifndef _WIN32
    Program *configured = prog.get();
    madc::lsp::backend_commands::ProgramFactory copy = [configured]() {
	return std::unique_ptr<Program>(configured);	// the child's copy
    };
#else
    madc::lsp::backend_commands::ProgramFactory copy;	// the child configures its own
#endif
    madc::lsp::backend_commands commands(options, copy);
    madc::lsp::server server;
    server.set_command_host(&commands);
    prog->push_runtime_scope();
    if ( file && !server.open_file(file) )
	std::cerr << "madc: --lsp: cannot read " << file << std::endl;
    int rc = server.serve(io);
    prog->pop_runtime_scope();
    return rc;
}

// madc --mcp FILE (split arc 1C): the Model Context Protocol on stdin/stdout,
// one message per line — the tools over FILE's document, for the lone local
// client, the operator (the owner tier), the session's project memory
// (memory-only: its events, proposals, records and test runs) and the tools
// its imported modules bring (--import=git: the version history). The
// configured Program is the active runtime Program for the server's life, as
// for --lsp.
// The registry a madc --mcp server serves: the code-graph tools, the
// session's project memory, and the tools the Program's imported modules
// bring (--import=git: the version history, 1C.14).
static int run_mcp(std::unique_ptr<Program> prog, const char *file)
{
    madc::channel io("stdio://");
    madc::hub::world w;
    madc::tool_host host(w, prog.get());	// before the open: the memory's baseline
    prog->push_runtime_scope();
    madc::hub::entity_id doc = madc::document_open(w, file);
    if ( !doc )
    {
	std::cerr << "madc: --mcp: cannot read " << file << std::endl;
	prog->pop_runtime_scope();
	return 1;
    }
    madc::mcp::server server(host.registry(), w, doc, madc::tools::tier::owner);
    int rc = server.serve(io);
    prog->pop_runtime_scope();
    return rc;
}

// madc --mcp --project X (split arc 1C, item 16): the headless MCP server for
// a project — X a manifest, or a single file (the one-TU case). The served
// documents are the project's TUs, each parsing as its manifest entry (as the
// --project build compiles it); a call's `file` names one, the first TU
// otherwise. The project memory lives beside the project, in <X less its
// extension>.events, so a headless agent's proposal waits there for an
// editor. ONE OWNER PER PROJECT: the first process to claim the project
// advertises it (madc::discovery); a second refuses, naming the owner.
static int run_mcp_project(std::unique_ptr<Program> prog, const char *path,
			   const ProjectManifest &manifest)
{
    if ( manifest.tus.empty() )
    {
	std::cerr << "madc: --mcp --project: " << path << " names no translation unit"
		  << std::endl;
	return 1;
    }
    const std::string project = madc::detail::canonical_path_for_compare(path);
    std::map<std::string, madc::value> advert;
    advert["madc"] = madc::value((int64_t)1);
    advert["project"] = madc::value(project);
    advert["serves"] = madc::value::make_array(std::vector<madc::value>(1, madc::value("mcp")));
    // discovery::find's fields: the project's documents and its directory.
    std::vector<madc::value> tus;
    for ( const ProjectTU &tu : manifest.tus )
	tus.push_back(madc::value(madc::detail::canonical_path_for_compare(tu.file)));
    advert["documents"] = madc::value::make_array(tus);
    advert["root"] = madc::value(madc::detail::host_path_dirname(project));
    madc::discovery::claim claim;
    madc::value holder;
    if ( claim.take(project, madc::value::make_object(advert), holder)
	 == madc::discovery::claim_result::held_elsewhere )
    {
	const madc::value *pid = madc::jsonrpc::field(holder, "pid");
	std::cerr << "madc: --mcp --project: " << path << " is served by madc pid "
		  << (pid && pid->is_integer() ? std::to_string(pid->as_integer())
					       : std::string("(starting)"))
		  << " (one owner per project)" << std::endl;
	return 1;
    }
    madc::channel io("stdio://");
    madc::hub::world w;
    madc::tool_host host(w, prog.get());	// before any open: the memory's baseline
    std::string why;
    const std::string events = project_events_path(path);
    if ( !events.empty() && !host.memory().bind(events, why) )
	std::cerr << "madc: --mcp --project: " << why << std::endl;
    if ( project_path_is_manifest(path) )
	host.memory().set_manifest(path);
    prog->push_runtime_scope();
    // A TU's document, opened on first use and parsing as its TU.
    auto open_tu = [&w, &manifest](size_t i) {
	const ProjectTU &tu = manifest.tus[i];
	madc::hub::entity_id doc = madc::document_find(w, tu.file);
	if ( !doc && (doc = madc::document_open(w, tu.file)) )
	    madc::lang::document_set_tu(w, doc, tu);
	return doc;
    };
    madc::hub::entity_id first = open_tu(0);
    if ( !first )
    {
	std::cerr << "madc: --mcp --project: cannot read " << manifest.tus[0].file
		  << std::endl;
	prog->pop_runtime_scope();
	return 1;
    }
    madc::mcp::server server(host.registry(), w, first, madc::tools::tier::owner);
    // A call's `file`: as the caller spells it (the working directory's) or
    // beside the project, compared canonically with each TU's.
    const std::string base = madc::detail::host_path_dirname(path);
    server.set_documents([&](const std::string &file, std::string &refusal) {
	const std::string want = madc::detail::canonical_path_for_compare(file);
	const std::string beside = madc::detail::canonical_path_for_compare(
	    madc::detail::host_path_join(base.empty() ? "." : base, file));
	for ( size_t i = 0; i < manifest.tus.size(); ++i )
	{
	    const std::string have = madc::detail::canonical_path_for_compare(manifest.tus[i].file);
	    if ( have == want || have == beside )
	    {
		madc::hub::entity_id doc = open_tu(i);
		if ( !doc )
		    refusal = "cannot read " + manifest.tus[i].file;
		return doc;
	    }
	}
	refusal = file + " is not a file of the project";
	return (madc::hub::entity_id)0;
    });
    int rc = server.serve(io);
    prog->pop_runtime_scope();
    return rc;
}

static int run_repl(std::unique_ptr<Program> prog, bool terminal,
		    const std::string &history, const madc::theme *theme,
		    const std::vector<std::string> &options,
		    int file_argc = 0, char **file_argv = NULL)
{
#ifdef MADC_CROSS_TARGET
    return cross_refuse_run("run the REPL");
#endif
    // The session runs in a backend process on this terminal (D2, plan
    // §41.9a slice 4), so a crash in an entry starts a fresh session instead
    // of ending madc. The backend's Program is this configured one, again on
    // each restart (the command line and madc.ini hold there): on POSIX the
    // fork's copy; on Windows, which has no fork, this executable run on the
    // same `options`, whose main configures its Program exactly as this one
    // was (split arc 1A.5). This one never begins. One stdin for both: the
    // backend reads it unbuffered, this host through shared_stdin_input.
    // The echo under a diagnostic in the theme's colours (1B.9), in the
    // backend's copy of this Program.
    prog->echo_theme = theme;
    SessionClient client;
    client.set_inherit_stdio(true);
    client.set_backend_arguments(options);
    client.set_history_file(history);	// %history's store, on every start
    // The backend renders diagnostics into a string this host prints on
    // std::cerr: coloured as std::cerr shows colour (Phase 0.5's gate).
    client.set_diagnostic_colour(madc::colour_reaches(std::cerr));
#ifndef _WIN32
    Program *configured = prog.get();
    SessionClient::ProgramFactory copy = [configured]() {
	return std::unique_ptr<Program>(configured);	// the child's copy
    };
#else
    SessionClient::ProgramFactory copy;	// the child configures its own
#endif
    if ( !client.start(std::string(), copy) )
    {
	std::cerr << client.last_error() << std::endl;
	return 1;
    }
    BackendSession session(client, std::cerr, NULL);
    return repl_loops(session, terminal, history, theme, file_argc, file_argv);
}

int main(int argc, char **argv)
{
    // --show-stats: earliest in-process timestamp, so the phase breakdown can
    // reconcile to a measured in-process total (the only part main() can see;
    // pre-main dynamic-load/static-init and post-exit teardown are outside it).
    struct timeval _t_main;
    gettimeofday(&_t_main, NULL);

    madc_install_crash_handler();
    // A Run child of self (madc_run_child.h): the parent named the run in
    // the environment; serve it and exit before this command line is read —
    // except the REPL's configured session backend (split arc 1A.5), served
    // below once this command line has configured the Program.
    std::string configured_session;
    madc_serve_run_child(&configured_session);

    stringstream ss;
    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    TokenProgram *tp;

#if defined(MADC_FOREST_EXPECT_EMBEDDED) || defined(MADC_FOREST_EXPECT_SIDECAR)
    // --with-forest= product expectation (forest-carriers S3): this binary
    // was built to ship WITH a forest (embedded carrier or sidecar), so a
    // discovery chain that finds nothing is degradation worth one loud
    // stderr notice — the CLI liberal default (live parse still runs).
    // Dev/debug/cross builds bake neither define and stay silent_fallback.
    prog->registration_policy.forest_missing_policy =
	Program::RegistrationPolicy::ForestPolicy::loud_fallback;
    engine.registration_policy.forest_missing_policy =
	Program::RegistrationPolicy::ForestPolicy::loud_fallback;
    // OWNER LAW: the standard core system headers come from libmadc, never a
    // system include dir (.claude/rules/embedded-headers.md, top section).
    prog->registration_policy.system_headers_from_libmadc = true;
    engine.registration_policy.system_headers_from_libmadc = true;
#endif

    const char *emit_object_path = NULL;
    const char *emit_executable_path = NULL;
    const char *generic_output_path = NULL;
    const char *emit_function_name = NULL;
    bool emit_pch = false;
    bool dump_cir = false;        // --dump-cir: dump CIR tree before checking
    bool dump_nodes = false;      // --dump-nodes: dump cir_node tree via madc walker
    bool dump_source = false;     // --dump-source / -E: full-fidelity source reconstruction
    bool dump_checked = false;    // --dump-cir-checked: post-check tree dump (c2m -d stage)
    bool do_emit = false;         // --emit=c11|mc11: render cir_node tree as C, no run
    CirEmitLang emit_lang = celC11;
    const char *project_manifest = NULL;  // --project <compile_commands.json>
    std::vector<std::string> cli_include_dirs; // -I as given: a --project build adds them to every TU
    std::vector<std::string> cli_define_args;  // -D as given: likewise
    std::vector<std::string> link_libs;   // -l<name>: TARGET spellings (madc_modules) opened RTLD_GLOBAL
    std::vector<std::string> cc_link_args; // -l<name> → DT_NEEDED in AOT mode
    MadcLinkPaths link_paths;             // -L<dir> (the -l search) and -Wl,-rpath,<dir> (DT_RUNPATH)
    bool compile_object = false;          // -c: emit a relocatable .o, no run
    bool emit_shared = false;             // -shared: emit an ET_DYN .so, no run
    bool no_pie = false;                  // -no-pie: fixed-base ET_EXEC (default = PIE, gcc parity)
    bool emit_relocatable = false;        // -r: relocatable link output — ONE .o (gcc/ld -r), no run
    bool show_help = false;               // --help / -h / -?
    bool help_json = false;               // --help=json
    bool show_completion = false;         // --completion=SHELL
    madc::cli::shell completion_shell = madc::cli::shell::bash;
    bool show_version = false;            // --version / -V
    bool show_machine = false;            // -dumpmachine
    bool show_capabilities = false;       // --capabilities=json
    bool show_dialect_roots = false;      // --print-dialect-roots: the dialect image's roots
    const char *builtin_registry_out = NULL; // --emit-builtin-registry=: the generated registration
    const char *module_table_out = NULL;     // --emit-module-table=: a module's generated declarations
    bool show_stats = false;               // --show-stats: print input/token traffic counters
    const char *freeze_path = NULL;       // --freeze= / --freeze-append=: forest container out
    bool freeze_append = false;           // --freeze-append=: placement 2 (append to binary)
    bool freeze_mir_cache = false;        // --freeze-mir-cache: pack the compiled MIR module blob
    std::vector<std::string> freeze_ledger_srcs;  // --freeze-ledger=: AOT ledger sources (S5)
    std::vector<madc::dis::istring> freeze_also_std;  // --freeze-also-std=: more standards the profile serves
    CirForestRole freeze_role = CIR_FOREST_ROLE_SYSTEM;  // --freeze-role=: the consumer the profile serves (B1)
    std::vector<std::string> freeze_source_lists;  // --freeze-sources=: lists of files for the source arm
    bool run_frozen = false;              // --run-frozen[=path]: thaw + run, no parse
    const char *run_frozen_path = NULL;   // NULL = the blob appended to this executable
    bool freeze_run = false;              // --freeze-run: freeze, then re-exec fresh to run
    bool dump_forest = false;             // --dump-forest[=path]: print container surfaces
    const char *forest_relayout_path = NULL;  // --forest-relayout=<image>: the pack layout pass
    const char *dump_forest_path = NULL;  // NULL = the blob appended to this executable
    bool dump_registered = false;         // --dump-registered: post-parse name maps (oracle side B)
    bool dump_macro_table = false;        // -dM: effective macro table after lex (gcc -dM -E analogue)
    bool no_forest_bind = false;          // --no-forest-bind: force live parse (overrides the default and --forest-bind)
    bool forest_bind_asked = false;       // --forest-bind[=PATH]: bind even while freezing (a test-harness request)
    const char *config_path = NULL;       // --config=<file>: this madc.ini instead of the lookup chain
    bool no_config = false;               // --no-config: skip the madc.ini lookup entirely
    bool cli_set_std = false;             // --std= came from the COMMAND LINE (so a madc.ini `std` key must not override it)
    madc::dis::istring cli_std_option;    // that --std='s standard name: a command-line project's units compile under it
    bool interactive = false;             // -i / --interactive: the REPL, whatever stdin is (D20)
    bool lsp = false;                     // --lsp: the Language Server Protocol on stdio (split arc 1C.12)
    bool mcp = false;                     // --mcp: the Model Context Protocol on stdio (split arc 1C)
    bool stdio_transport = false;         // --stdio: an LSP client names --lsp's transport
    std::string history_file;             // --history-file=PATH|no: the REPL's history (D23)
    bool history_file_given = false;

    // AOT (-c / -shared / -r / -o / --emit-*): compile through gen
    // object-capture mode and write a native artifact instead of running.
    // -o without -c or -shared means a linked executable (gcc semantics);
    // --emit-pch keeps its own -o meaning (.madh output path).
    // -E / -dM stop after preprocessing: -o names that text's file, not a
    // native artifact.
    auto preprocess_only = [&]() { return dump_source || dump_macro_table; };
    auto native_artifact_requested = [&]() {
        return compile_object || emit_shared || emit_relocatable
            || emit_object_path || emit_executable_path
            || (generic_output_path && !emit_pch && !preprocess_only());
    };

    // madc's command line, read by THE reader against madc's table
    // (src/madc_cli.cpp: the spellings, the lane rule, @file); each option
    // applies here, in command-line order. The values point into `parsed`,
    // which lives until main() returns.
    const madc::cli::result parsed =
        madc::cli::parse(madc_cli_table(), std::vector<std::string>(argv + 1, argv + argc));
    if ( !parsed.ok() )
    {
        std::cerr << "madc: error: " << parsed.message << std::endl;
        return 1;
    }
    for ( size_t k = 0; k < parsed.options.size(); k++ )
    {
        const madc::cli::occurrence &o = parsed.options[k];
        const char *value = o.value.c_str();
        // An optional value's bare word (--run-frozen) is not its empty
        // value (--run-frozen=).
        const bool bare = o.spelled && o.word == o.spelled;
        switch ( (cli_option)o.id )
        {
        case cli_option::none:
            break;
        case cli_option::verbose:
            madc_verbose = true;
            break;
        case cli_option::opt_level:
            // Bare -O == -O1; the row's choices admit 0..3.
            madc_opt_level = o.value.empty() ? 1 : o.value[0] - '0';
            break;
        case cli_option::debug_info:
            madc_debug_info = true;
            break;
        case cli_option::no_warnings:
            madc_no_warnings = true;	// gcc -w: c2mir's warnings included
            break;
        case cli_option::compile_object:
            compile_object = true;
            break;
        case cli_option::shared:
            emit_shared = true;
            break;
        case cli_option::relocatable:
            emit_relocatable = true;
            break;
        case cli_option::no_pie:
            no_pie = true;
            break;
        case cli_option::pie:
            no_pie = false;
            break;
        case cli_option::gui_subsystem:
            // Emit-lane state like -static-libmadc; ELF / Mach-O emits ignore
            // it; the --project lane takes the manifest's "kind" instead.
            madc_gui_subsystem = true;
            break;
        case cli_option::console_subsystem:
            madc_gui_subsystem = false;
            break;
        case cli_option::emit_object:
            emit_object_path = value;
            break;
        case cli_option::emit_executable:
            emit_executable_path = value;
            break;
        case cli_option::output:
            generic_output_path = value;
            break;
        case cli_option::include_dir:
            prog->add_include_dir(value);	// guards empty + normalizes trailing '/'
            if ( *value )
                cli_include_dirs.push_back(value);
            break;
        case cli_option::define:
            // NAME, NAME=VALUE (a bare NAME is "1", gcc). Object-like only;
            // applied after the builtin defines so a -D can override one.
            prog->add_cli_define(value);	// guards empty + splits NAME[=VALUE]
            if ( *value )
                cli_define_args.push_back(value);
            break;
        case cli_option::emit_pch:
            emit_pch = true;
            break;
        case cli_option::emit_function:
            emit_function_name = value;
            break;
        case cli_option::no_includes:
            prog->skip_includes = true;
            break;
        case cli_option::no_embedded_headers:
            // Header partition (madc-header-partition-handoff.md): bypass the
            // embedded SYSTEM-library shims so <...> reads the real headers
            // on disk, keeping madc-own and compiler-owned headers
            // (embedded_header_is_system_library_shim). Rides the engine so
            // --project units inherit it. The ONE switch that sends the core
            // system headers to disk.
            engine.registration_policy.bypass_system_library_headers = true;
            prog->registration_policy.bypass_system_library_headers = true;
            engine.registration_policy.system_headers_from_libmadc = false;
            prog->registration_policy.system_headers_from_libmadc = false;
            break;
        case cli_option::no_posix_compat:
            // Rides the engine so --project units inherit the target surface.
            engine.registration_policy.enable_posix_compat = false;
            prog->registration_policy.enable_posix_compat = false;
            break;
        case cli_option::no_auto_load:
            // `import` / #load bind to the program's own symbol scope; the
            // library is linked, never opened (link explicitly via -l).
            // Rides the engine so --project units inherit it.
            engine.registration_policy.enable_auto_library_loading = false;
            prog->registration_policy.enable_auto_library_loading = false;
            break;
        case cli_option::instrument_functions:
            prog->instrument_functions = true;
            break;
        case cli_option::no_builtin:
            prog->disabled_builtin_names.insert(value);	// a joined value: never empty
            break;
        case cli_option::no_eval_shims:
            // Whether this artifact can be host-called through the value ABI
            // is a property of the BUILD (for a .o only the build knows), as
            // -fPIC is: emit-lane state every lane sees.
            madc_no_eval_shims = true;
            break;
        case cli_option::eval_shims:
            madc_no_eval_shims = false;
            break;
        case cli_option::pic:
            break;	// madc's code is position-independent already
        case cli_option::diagnostics_color:
        {
            // gcc's -fdiagnostics-color[=WHEN]: the bare flag is always.
            const char *when = bare ? "always" : value;
            madc::colour_when code;
            if ( !madc::parse_colour_when(when, code) )
            {
                std::cerr << "Unknown -fdiagnostics-color= value: " << when
                          << " (auto, always, never)" << std::endl;
                return 1;
            }
            madc::set_colour_when(code);
            break;
        }
        case cli_option::no_diagnostics_color:
            madc::set_colour_when(madc::colour_when::never);
            break;
        case cli_option::dump_cir:
            dump_cir = true;
            break;
        case cli_option::dump_nodes:
            dump_nodes = true;
            break;
        case cli_option::dump_cir_checked:
            dump_checked = true;
            break;
        case cli_option::dump_source:
        case cli_option::preprocess:
            // -E: madc preprocesses during tokenize(), so the token stream IS
            // the preprocessed unit; reconstruct_source prints it,
            // content-only (no `# line` markers), diffable against gcc -E.
            dump_source = true;
            break;
        case cli_option::show_stats:
            show_stats = true;
            break;
        case cli_option::freeze:
            freeze_path = value;
            break;
        case cli_option::freeze_append:
            freeze_path = value;
            freeze_append = true;
            break;
        case cli_option::freeze_mir_cache:
            freeze_mir_cache = true;
            break;
        case cli_option::freeze_ledger:
            // Repeatable; the canonical list is scripts/ledger_sources.txt,
            // handed over by scripts/forest_pack.sh — never spelled out here.
            freeze_ledger_srcs.push_back(o.value);
            break;
        case cli_option::freeze_also_std:
            freeze_also_std.push_back(o.value);
            break;
        case cli_option::print_dialect_roots:
            show_dialect_roots = true;
            break;
        case cli_option::emit_builtin_registry:
            builtin_registry_out = value;
            break;
        case cli_option::emit_module_table:
            module_table_out = value;
            break;
        case cli_option::freeze_role:
            // The role's spelling -> its code, once, here (the role table in
            // cir_freeze.cpp owns both).
            if ( !cir_forest_role_named(value, freeze_role) )
            {
                std::cerr << "madc: --freeze-role=" << value
                          << ": unknown role (system, dialect)" << std::endl;
                return 1;
            }
            break;
        case cli_option::freeze_sources:
            freeze_source_lists.push_back(o.value);
            break;
        case cli_option::freeze_run:
            freeze_run = true;
            break;
        case cli_option::no_sysroot_includes:
            // The toolchain-only include view, so a freeze on a host with a
            // real SDK makes the header-less consumer's corpus. Rides the
            // engine too.
            engine.registration_policy.enable_sysroot_includes = false;
            prog->registration_policy.enable_sysroot_includes = false;
            break;
        case cli_option::pack_forest:
            madc_pack_forest_path = value;
            break;
        case cli_option::static_libmadc:
            madc_static_libmadc = true;
            break;
        case cli_option::run_frozen:
            run_frozen = true;
            if ( !bare )
                run_frozen_path = value;
            break;
        case cli_option::dump_forest:
            dump_forest = true;
            if ( !bare )
                dump_forest_path = value;
            break;
        case cli_option::forest_bind:
            prog->registration_policy.enable_forest_bind = true;
            forest_bind_asked = true;
            if ( !bare )
                prog->forest_bind_path = value;
            break;
        case cli_option::forest_touch_log:
            prog->forest_touch_log = value;
            break;
        case cli_option::forest_relayout:
            forest_relayout_path = value;	// the remainder is its logs
            break;
        case cli_option::no_forest_bind:
            no_forest_bind = true;
            break;
        case cli_option::config:
            config_path = value;
            break;
        case cli_option::no_config:
            no_config = true;
            break;
        case cli_option::dump_registered:
            dump_registered = true;
            break;
        case cli_option::dump_macros:
            dump_macro_table = true;
            break;
        case cli_option::project:
            project_manifest = value;
            break;
        case cli_option::interactive:
            interactive = true;
            break;
        case cli_option::mcp:
            mcp = true;
            break;
        case cli_option::lsp:
            lsp = true;
            break;
        case cli_option::stdio:
            stdio_transport = true;
            break;
        case cli_option::history_file:
            history_file = o.value;	// `no`: history for the session alone
            if ( history_file == "no" )
                history_file.clear();
            history_file_given = true;
            break;
        case cli_option::help:
            show_help = true;
            help_json = o.value == "json";
            break;
        case cli_option::completion:
            show_completion = madc::cli::parse_shell(o.value, completion_shell);
            break;
        case cli_option::version:
            show_version = true;
            break;
        case cli_option::dump_machine:
            show_machine = true;
            break;
        case cli_option::capabilities:
            // json is the only format this build implements; any other (the
            // bare word included) is refused loudly rather than answered
            // with a schema the consumer did not ask for.
            if ( o.value != "json" )
            {
                std::cerr << "Unknown capabilities format: " << o.word
                          << " (json)" << std::endl;
                return 1;
            }
            show_capabilities = true;
            break;
        case cli_option::library:
        {
            // The NAME's TARGET spelling comes from the ONE owner
            // (madc_modules); JIT: opened RTLD_GLOBAL below; AOT: the
            // spelling joins the link closure verbatim.
            std::string lib = madc_module_library_spelling(value);
            cc_link_args.push_back(lib);
            link_libs.push_back(lib);
            break;
        }
        case cli_option::library_dir:
            if ( *value )
                link_paths.lib_dirs.push_back(value);
            break;
        case cli_option::module_path:
            if ( *value )
                prog->module_search_dirs.push_back(value);
            break;
        case cli_option::import_module:
            if ( *value )
                prog->command_line_imports.push_back(madc::dis::istring(value));
            break;
        case cli_option::linker_option:
        {
            // gcc's pass-through to the linker; madc is its own linker.
            std::string refused;
            if ( !read_linker_options(value, link_paths.runpath, refused) )
            {
                std::cerr << "madc: " << o.word << ": linker option '"
                          << refused << "' is not supported (-rpath is)"
                          << std::endl;
                return 1;
            }
            break;
        }
        case cli_option::warning_selection:
            break;	// madc's diagnostics are one fixed set
        case cli_option::warnings_as_errors:
            std::cerr << "madc: " << o.word << ": not supported (madc's warnings"
                      << " are never errors)" << std::endl;
            return 1;
        case cli_option::pass_through:
            std::cerr << "madc: " << o.word << ": not supported (madc has no separate"
                      << " assembler or preprocessor to pass it to)" << std::endl;
            return 1;
        case cli_option::emit:
            // Render the cir_node tree (MC11-IR) as C source; do not run.
            if ( !cir_emit_lang_of(value, emit_lang) )
            {
                std::cerr << "Unknown --emit target: " << value
                          << " (" << CIR_EMIT_TARGETS << ")" << std::endl;
                return 1;
            }
            do_emit = true;
            break;
        case cli_option::language_std:
            if ( !prog->set_language_standard(o.value) )
            {
                std::cerr << "Unknown --std target: " << o.value << std::endl;
                return 1;
            }
            cli_set_std = true;
            cli_std_option = o.value;
            break;
        case cli_option::stdlib_flavor:
            // Which flavors exist is a property of the BUILD host, so name the
            // ones this binary has rather than the ones the flag could spell.
            if ( !prog->set_stdlib_flavor(o.value) )
            {
                std::cerr << "Unknown -stdlib flavor: " << o.value
                          << " (this madc was built with: " << prog->stdlib_flavor_names()
                          << ")" << std::endl;
                return 1;
            }
            break;
        }
    }

    // The program's own words: the operands a line that builds took (gcc's
    // argument permutation), then the remainder; argv[filearg..argc) as before.
    std::vector<std::string> program_words(parsed.operands);
    program_words.insert(program_words.end(), parsed.remainder.begin(),
                         parsed.remainder.end());
    std::vector<char *> program_argv(1, argv[0]);
    for ( size_t k = 0; k < program_words.size(); k++ )
        program_argv.push_back(&program_words[k][0]);
    program_argv.push_back(NULL);
    argv = program_argv.data();
    argc = (int)program_words.size() + 1;
    int filearg = 1;

    prog->class_parse_observability = show_stats;
    {
	const char *live_capture = getenv("MADC_CLASS_PATTERN_LIVE");
	if ( live_capture && *live_capture && strcmp(live_capture, "0") != 0 )
	    prog->class_pattern_live_capture = true;
    }

    if ( show_version )
    {
        // Same string the built-in MADC_VERSION macro and madc::sys.version
        // carry: the build threads ../VERSION in as -DMADC_VERSION_STR, and
        // the version-consuming objects depend on that file, so all three
        // spellings move together on a release bump.
        std::cout << "madc " << MADC_VERSION_STR << std::endl;
#ifdef MADC_CROSS_TARGET
        // A cross artifact is a DIFFERENT compiler from the native one of the
        // same version; say so, because the version alone cannot tell them
        // apart and the macOS/Windows tarballs ship exactly that.
        std::cout << "Target: " << MADC_CROSS_TARGET << std::endl;
#endif
        return 0;
    }

    if ( show_machine )
    {
        // gcc's -dumpmachine: the target triple (build systems ask it —
        // dpkg-architecture, autoconf's multiarch probe). The toolchain madc
        // was built against answered it at build time.
        std::cout << (*madc_toolchain_target ? madc_toolchain_target : "unknown")
                  << std::endl;
        return 0;
    }

    if ( show_capabilities )
    {
        // Source-free like --version: emit the manifest before any madc.ini
        // lookup or Program construction, so tooling can query the compiler
        // with nothing but the binary.
        madc_print_capabilities_json();
        return 0;
    }

    // The help, --help=json and the completion scripts are madc's table
    // (src/madc_cli.cpp) through the reader's writers: one source for all.
    if ( show_help )
    {
        if ( help_json )
            std::cout << madc::cli::describe_json(madc_cli_table()) << std::endl;
        else
            madc::cli::write_help(std::cout, madc_cli_table(), madc::terminal_columns());
        return 0;
    }

    if ( show_completion )
    {
        madc::cli::write_completion(std::cout, madc_cli_table(), completion_shell);
        return 0;
    }

    // --print-dialect-roots (dialect per-name plan B1): the dialect image's
    // roots, as the auto-include orders them (Program::dialect_image_roots) —
    // forest_pack.sh --dialect-image writes the image's TU from them. Only
    // the madc dialect has the surface; any other standard is refused.
    if ( show_dialect_roots )
    {
        std::vector<madc::dis::istring> roots;
        if ( !prog->dialect_image_roots(roots) )
        {
            std::cerr << "madc: --print-dialect-roots needs --std=madc" << std::endl;
            return 1;
        }
        for ( size_t i = 0; i < roots.size(); ++i )
            std::cout << roots[i].c_str() << "\n";
        std::cout.flush();
        return 0;
    }

    // --emit-builtin-registry=<out.cpp> (the dialect is builtin, R1): the
    // build's generator — the dialect files read once, as text, into one
    // builder per name (madc_builtin_registry.cpp). No source file.
    if ( builtin_registry_out )
    {
        std::ostringstream gen;
        if ( !emit_builtin_registry(gen, std::cerr) )
            return 1;
        std::ofstream out(builtin_registry_out);
        out << gen.str();
        out.close();
        if ( !out )
        {
            std::cerr << "madc: --emit-builtin-registry: cannot write "
                      << builtin_registry_out << std::endl;
            return 1;
        }
        return 0;
    }

    // --emit-module-table=<out.inc> <face>: a module's build step — its face
    // read once, as text, into one builder per name and the static table its
    // description returns (madc_module.h). The face is the source argument.
    if ( module_table_out )
    {
        if ( filearg >= argc )
        {
            std::cerr << "madc: --emit-module-table: name the module's face file" << std::endl;
            return 1;
        }
        std::ostringstream gen;
        if ( !emit_module_table(gen, std::cerr, argv[filearg]) )
            return 1;
        std::ofstream out(module_table_out);
        out << gen.str();
        out.close();
        if ( !out )
        {
            std::cerr << "madc: --emit-module-table: cannot write " << module_table_out
                      << std::endl;
            return 1;
        }
        return 0;
    }

    // --forest-relayout=<image> <log>...: lay the image's last container out
    // by the reads the recorded root binds made (format 60; the pack step,
    // scripts/forest_pack_layout.sh). No source file, no parse.
    if ( forest_relayout_path )
    {
        std::vector<std::string> logs(argv + filearg, argv + argc);
        return madc_cir_forest_relayout(forest_relayout_path, logs) ? 0 : 1;
    }

    // madc.ini (forest-carriers S6). The precedence rule is
    // CLI > environment > madc.ini > baked defaults, and it is enforced HERE,
    // in one visible place:
    //   - the command line has already been parsed, so a --std= on it wins
    //     (cli_set_std) and its -I dirs are already in front of these;
    //   - the environment beats these because each consumer reads its env var
    //     first (MADC_FOREST is discovery arm 4, the forest key is arm 5;
    //     install_resource_guards checks MADC_*_LIMIT before the ini value);
    //   - what is left is the baked default, which these replace.
    madc::config_settings config;
    if ( config_path && !*config_path )
    {
        std::cerr << "madc: --config= needs a file path" << std::endl;
        return 1;
    }
    // Both together is a contradiction, and resolving it either way would mean
    // silently ignoring one of two explicit instructions.
    if ( config_path && no_config )
    {
        std::cerr << "madc: --config= and --no-config may not be used together"
                  << std::endl;
        return 1;
    }
    if ( !no_config && !madc::config_load(config_path, config, std::cerr) )
        return 1;
    if ( config.has_std && !cli_set_std
      && !prog->set_language_standard(config.std_option) )
    {
        std::cerr << "madc: " << config.source_path << ": unknown std '"
                  << config.std_option << "'" << std::endl;
        return 1;
    }
    // The source's extension outranks madc.ini (split arc 1A.4): `--std` >
    // extension > madc.ini `std` > madc. The rule is the one a --project TU
    // and the editor's parse take (Program::adopt_file_standard); a source
    // with no recognised extension — a shebang script, stdin — keeps the ini's
    // standard. Under --run-frozen the positionals are the program's argv.
    if ( !cli_set_std && !run_frozen && filearg < argc )
        prog->adopt_file_standard(madc::file_kind_of_path(argv[filearg]));
    // A -stdlib= on the command line already set prog->stdlib_flavor, and CLI
    // outranks madc.ini — same precedence as `std` above.
    if ( config.has_stdlib && !prog->stdlib_flavor
      && !prog->set_stdlib_flavor(config.stdlib_option) )
    {
        std::cerr << "madc: " << config.source_path << ": unknown stdlib '"
                  << config.stdlib_option << "' (this madc was built with: "
                  << prog->stdlib_flavor_names() << ")" << std::endl;
        return 1;
    }
    // `theme` (split arc 1B.9): the REPL's colours, madc's built-in theme
    // unless madc.ini names a `.theme` file. One that does not read keeps the
    // built-in, and says why.
    madc::theme repl_theme = madc::theme_default();
    if ( config.has_theme )
    {
        std::string text, why;
        if ( !madc::document_read_file(config.theme, text) )
            why = "cannot be read";
        else if ( !madc::theme_read(text, repl_theme, why) )
            repl_theme = madc::theme_default();
        if ( !why.empty() && configured_session.empty() )   // the host said it
            std::cerr << "madc: " << config.source_path << ": theme "
                      << config.theme << ": " << why
                      << "; the built-in theme stays" << std::endl;
    }
    // Appended AFTER the command line's -I dirs (gcc searches configured
    // directories last).
    for ( const std::string &dir : config.include_dirs )
        prog->add_include_dir(dir);
    // `modules` (split arc 1A.4) — php.ini's `extension =`: each a module
    // imported as --import=<name> imports it, after the command line's.
    // Additive, not an override (a module is a capability, not a setting); a
    // module the command line already named is not named twice.
    for ( const std::string &name : config.modules )
    {
        madc::dis::istring module(name.c_str());
        if ( std::find(prog->command_line_imports.begin(), prog->command_line_imports.end(),
                       module) == prog->command_line_imports.end() )
            prog->command_line_imports.push_back(module);
    }
    // Discovery arm 5, on both the program and the engine — the engine's copy
    // is what --project TU programs and runtime-eval children inherit (the
    // MADC_FOREST_EXPECT_* block above sets the pair the same way).
    prog->registration_policy.forest_config_path = config.forest;
    engine.registration_policy.forest_config_path = config.forest;

    // A .json input file with no explicit --project defaults to project mode:
    // `madc compile_commands.json [args...]` == `madc --project compile_commands.json
    // [args...]`. gcc/clang select by extension; we mirror that for the build driver.
    // Not under --run-frozen: there the positionals are the PROGRAM's argv,
    // and a program argument ending in .json must pass through untouched.
    if ( !project_manifest && !run_frozen && filearg < argc )
    {
        const char *f = argv[filearg];
        size_t flen = strlen(f);
        if ( flen >= 5 && strcmp(f + flen - 5, ".json") == 0 )
        {
            project_manifest = f;
            ++filearg;   // remaining positionals become the program's argv
        }
    }

    // .o inputs: a precompiled native cache to run (AOT R4b), or — several
    // of them / with an output flag — a multi-object link (this rung). The
    // LEADING RUN of .o positionals are all objects; the rest become the
    // program's argv (argv[0] stays the first object path). Freshness is
    // the build system's concern (make semantics), exactly as with any
    // compiler's .o. Same extension convention and --run-frozen exclusion
    // as the .json arm above.
    std::vector<std::string> run_object_paths;
    if ( !project_manifest && !run_frozen )
    {
        while ( filearg < argc )
        {
            const char *f = argv[filearg];
            size_t flen = strlen(f);
            if ( flen < 2 || strcmp(f + flen - 2, ".o") != 0 )
                break;
            run_object_paths.push_back(f);
            ++filearg;   // remaining positionals become the program's argv
        }
    }

    // Resource guards install AFTER argument parsing so the memory guard can
    // scale with the workload (see install_resource_guards). The manifest is
    // read here, once, for its TU count; the --project branch below reuses
    // it. Order matters: RLIMIT hard limits can only be lowered, never
    // raised, so the guard must know the workload before it arms.
    ProjectManifest manifest;
    // Several sources on an artifact lane (gcc's `cc -o prog a.c b.c`,
    // `cc -c a.c b.c`) are a project of those translation units, built by
    // the --project lane below: one .o per unit for -c, one image for -o /
    // -shared, every unit under the command line's --std, -I and -D. Only
    // when EVERY operand is a unit the compiler reads; anything else keeps
    // the single-source lane's loud refusal.
    bool cli_sources_project = !project_manifest && !run_frozen
        && run_object_paths.empty() && argc - filearg > 1
        && native_artifact_requested();
    for ( int i = filearg; cli_sources_project && i < argc; i++ )
        if ( madc::file_kind_family(madc::file_kind_of_path(argv[i]))
             == madc::fkUNKNOWN )
            cli_sources_project = false;
    if ( cli_sources_project )
    {
        for ( int i = filearg; i < argc; i++ )
        {
            ProjectTU tu;
            tu.file = argv[i];
            tu.std_option = cli_std_option.c_str();
            manifest.tus.push_back(tu);
        }
    }
    if ( project_manifest || cli_sources_project )
    {
        std::string err;
        if ( project_manifest
          && !(mcp ? project_of_path(project_manifest, manifest, err)
                   : read_project_manifest(project_manifest, manifest, err)) )
        {
            std::cerr << "madc --project: " << err << std::endl;
            return 1;
        }
        // The command line's -I and -D apply to every TU, after the TU's
        // own (gcc's `-I dir a.c b.c`; a compilation database's extra
        // arguments, as clang's tools append them): a TU's own directories
        // are searched first, and a -D here overrides the manifest's.
        for ( ProjectTU &tu : manifest.tus )
        {
            tu.include_dirs.insert(tu.include_dirs.end(),
                                   cli_include_dirs.begin(), cli_include_dirs.end());
            tu.defines.insert(tu.defines.end(),
                              cli_define_args.begin(), cli_define_args.end());
        }
    }
    madc_install_resource_guards(manifest.tus.size(), config);

    // Embedded-forest default (Phase 4): with no explicit forest flag, bind
    // system #includes from the blob appended to this executable (the
    // Program default) — ensure_bind_forest() falls through silently to live
    // parse when no blob is appended or the context pin mismatches. Freeze
    // modes are excluded: a freeze PRODUCES forest state from a live parse;
    // binding while freezing stays an explicit test-harness request
    // (--forest-bind), never the default.
    if ( no_forest_bind || ((freeze_path || freeze_run) && !forest_bind_asked) )
    {
        prog->registration_policy.enable_forest_bind = false;
        prog->forest_bind_path.clear();
    }

    bool emit_native = native_artifact_requested();

    // gcc parity: ld rejects "-r and -shared may not be used together".
    if ( emit_relocatable && emit_shared )
    {
        std::cerr << "madc: -r and -shared may not be used together"
                  << std::endl;
        return 1;
    }

    // -g on a Mach-O target: neither the executable writer nor the .o writer
    // emits __DWARF sections yet. Say so ONCE, here, where the user's -g is
    // visible, and continue without debug info — the alternative was the
    // executable writer silently dropping an attached debug builder while the
    // .o writer refused the emit outright.
#if MADC_TARGET_APPLE_P
    if ( madc_debug_info )
    {
        std::cerr << "madc: -g: DWARF is not emitted for Mach-O targets yet;"
                     " continuing without debug info" << std::endl;
        madc_debug_info = false;
    }
#endif

    // --pack-forest rides a LINKED image (executable or shared object): a
    // relocatable .o has no self-image carrier, and a JIT run produces no
    // image at all. One chokepoint for every lane (source, .o link,
    // --project).
    if ( madc_pack_forest_path
         && (!emit_native || compile_object || emit_object_path
             || emit_relocatable) )
    {
        std::cerr << "madc: --pack-forest requires a linked native output"
                     " (-o executable or -shared)" << std::endl;
        return 1;
    }

    // -static-libmadc rides a LINKED image too: the runtime pieces are merged
    // at the link that produces the final image, exactly as gcc resolves
    // -static-libgcc there. Putting them in a .o would duplicate them into
    // every object of a multi-TU program.
    if ( madc_static_libmadc
         && (!emit_native || compile_object || emit_object_path
             || emit_relocatable) )
    {
        std::cerr << "madc: -static-libmadc requires a linked native output"
                     " (-o executable or -shared); the runtime is merged at"
                     " the final link" << std::endl;
        return 1;
    }

    // .o inputs + an output flag = the multi-object LINK lane: combine the
    // precompiled captures into one artifact (-r one .o, -o an executable,
    // -shared a .so) — MIR is the linker, no external toolchain. -c stays
    // a loud error (gcc: the input is already compiled).
    if ( !run_object_paths.empty() && emit_native )
    {
        if ( compile_object || emit_object_path )
        {
            std::cerr << "madc: -c with .o inputs: the input is already"
                         " compiled (use -r to combine objects)"
                      << std::endl;
            return 1;
        }
        MadcNativeKind kind = emit_relocatable ? mnkRelocatable
                            : emit_shared ? mnkShared
                            : no_pie ? mnkExecutable
                            : mnkPieExecutable;
        const char *outp = emit_executable_path ? emit_executable_path
                         : generic_output_path ? generic_output_path
                         : "a.out";
        return madc_cir_link_objects(run_object_paths, kind, outp,
                                     cc_link_args, prog.get(),
                                     link_paths) == 0 ? 0 : 1;
    }

    // -l<name>: dlopen each requested library (RTLD_GLOBAL) so the import
    // resolver (dlsym(RTLD_DEFAULT, ...)) finds its symbols at link time. Done
    // before any compile/run so it applies to both the single-file and
    // --project paths. In AOT mode nothing reads import addresses (they become
    // undefined ELF symbols) — the libs go to the host link line instead.
    // The opener is import's (madc_module_open): madc's own lib directory
    // first, then the loader, so an installed madc's -lmadcide finds the
    // libmadcide beside it.
    for ( const std::string &lib : link_libs )
    {
        if ( emit_native )
            break;
        std::string lerr;
        std::string target = madc_module_locate(lib, link_paths.lib_dirs);
        if ( !madc_module_open(target, lerr, /*bind_now=*/true) )
        {
            std::cerr << "madc: -l: failed to load " << lib << ": "
                      << lerr << std::endl;
            return 1;
        }
        prog->loaded_lib_paths.push_back(target);   // the frozen-forest link closure
    }

    // The REPL host's backend (Windows, split arc 1A.5): this process is its
    // configured-session run child, the command line is the host's options,
    // and the Program is now configured exactly as the host's was.
    if ( !configured_session.empty() )
    {
        prog->echo_theme = &repl_theme;     // the host's run_repl sets it there
        return madc_session_serve_child(configured_session, std::move(prog));
    }

    // .o inputs (AOT R4b + multi-object): execute the madc-emitted
    // relocatable object(s) as a precompiled cache — no parse, no
    // translate, no gen; several objects merge in memory first. Placed
    // after the -l loop so `madc -lcrypt foo.o` resolves the same way the
    // JIT lane would. argv[0] is the first object path (the JIT lane's
    // source-path convention); remaining positionals are the program's
    // argv.
    if ( !run_object_paths.empty() )
    {
#ifdef MADC_CROSS_TARGET
        return cross_refuse_run("execute .o objects in-process");
#endif
        std::vector<char *> oargv;
        oargv.push_back((char *)run_object_paths[0].c_str());
        for ( int i = filearg; i < argc; ++i )
            oargv.push_back(argv[i]);
        return madc_cir_run_objects(run_object_paths,
                                    (int)oargv.size(), oargv.data(),
                                    prog->active_stdlib_flavor());
    }

    // --run-frozen: thaw + compile + run a frozen forest container — no
    // source file, no parse. Remaining positionals become the program's
    // argv (argv[0] is the container / executable path).
#ifdef MADC_CROSS_TARGET
    if ( run_frozen || freeze_run )
        return cross_refuse_run("run a frozen program");
#endif
    if ( run_frozen )
    {
        std::string ra0 = run_frozen_path ? std::string(run_frozen_path)
                                          : madc_self_exe_path();
        std::vector<char *> rargv;
        rargv.push_back((char *)ra0.c_str());
        for ( int i = filearg; i < argc; ++i )
            rargv.push_back(argv[i]);
        int rc = madc_cir_execute_frozen(run_frozen_path,
                                         (int)rargv.size(), rargv.data(),
                                         show_stats);
        // --show-stats: the lane printed its phase walls; append the process
        // total only main() can see (same closing line as the parse lanes).
        if ( show_stats )
        {
            struct timeval _t_now;
            gettimeofday(&_t_now, NULL);
            fprintf(stderr, "[stats] total (in-process) .. %.3f s"
                    "  (excl. pre-main load + teardown)\n",
                    (_t_now.tv_sec - _t_main.tv_sec)
                    + (_t_now.tv_usec - _t_main.tv_usec) / 1e6);
        }
        return rc;
    }

    // --dump-forest: print a container's directory + grove payload v2
    // surfaces (decl index, PP exports, edges, branch macros, canonical
    // order) — the B4a oracle data source. No source file, no parse.
    if ( dump_forest )
    {
        int rc = madc_cir_dump_forest(dump_forest_path);
        return (rc < 0) ? 1 : 0;
    }

    if ( generic_output_path && !emit_pch && !compile_object && !emit_shared
	 && !emit_relocatable && !preprocess_only() )
	emit_executable_path = generic_output_path;

    if ( emit_native )
	prog->aot_tracking = true;

    // --emit-pch: lex the input file and write a .madh pre-compiled header
    if ( emit_pch && filearg < argc )
    {
	const char *input = argv[filearg];
	tp = prog->tokenize(input);
	if ( !tp )
	{
	    std::cerr << "Failed to tokenize " << input << std::endl;
	    return 1;
	}

	// Compute source hash from file content
	std::ifstream hf(input, std::ios::binary | std::ios::ate);
	uint64_t src_hash = 0;
	if ( hf )
	{
	    size_t fsize = hf.tellg();
	    hf.seekg(0);
	    std::vector<char> fbuf(fsize);
	    hf.read(fbuf.data(), fsize);
	    src_hash = madc_pch::hash_content(fbuf.data(), fsize);
	}

	// Determine output path (-o flag or default from input name)
	std::string outpath;
	if ( generic_output_path )
	    outpath = generic_output_path;
	else
	{
	    outpath = madc::detail::host_path_drop_extension(input) + ".madh";
	}

	// Choose compression: prefer zstd if available
	PchCompression method = PchCompression::Zlib;
#ifdef HAVE_ZSTD
	method = PchCompression::Zstd;
#endif

	if ( madc_pch::write_madh(outpath.c_str(), prog->tokens, src_hash, method) )
	{
	    std::cout << "Wrote " << outpath << " (" << prog->tokens.size()
		      << " tokens)" << std::endl;
	    return 0;
	}
	else
	{
	    std::cerr << "Failed to write " << outpath << std::endl;
	    return 1;
	}
    }

    // --emit-function: tokenize+parse, find function, emit source lines
    if ( emit_function_name && filearg < argc )
	return emit_function(*prog, argv[filearg], emit_function_name);

    if ( (project_manifest || cli_sources_project) && !mcp )
    {
	if ( emit_native )
	{
	    // AOT over the whole project: -c → one .o per TU (object capture
	    // is context-wide, so each TU compiles in its own context; gcc
	    // semantics for names and for rejecting -c -o with many TUs);
	    // -o / -shared → ONE MIR-assembled native image of every TU
	    // (whole-program capture in one shared context — the same
	    // granularity the JIT project lane uses).
	    MadcNativeKind kind = (compile_object || emit_object_path) ? mnkObject
				: emit_relocatable ? mnkRelocatable
				: emit_shared ? mnkShared
				: no_pie ? mnkExecutable
				: mnkPieExecutable;
	    const char *explicit_out =
		  kind == mnkObject ? (emit_object_path ? emit_object_path
						        : generic_output_path)
		: (kind == mnkShared || kind == mnkRelocatable)
			? generic_output_path
		: emit_executable_path;
	    if ( kind == mnkObject && explicit_out && manifest.tus.size() > 1 )
	    {
		std::cerr << "madc: cannot specify -o with -c and multiple"
			     " translation units" << std::endl;
		return 1;
	    }
	    const char *outpath = explicit_out ? explicit_out
			        : kind == mnkObject ? NULL   // per-TU naming
			        : "a.out";   // -r too: gcc emits a relocatable a.out
	    int erc = madc_project_emit_native(engine, manifest, kind, outpath,
					       cc_link_args,
					       prog->registration_policy.enable_forest_bind,
					       prog->forest_bind_path,
					       link_paths);
	    return erc == 0 ? 0 : 1;
	}
	// Remaining positionals (filearg..argc) become the program's argv.
#ifdef MADC_CROSS_TARGET
	return cross_refuse_run("execute a --project program");
#endif
	int run_argc = argc - filearg;
	char **run_argv = argv + filearg;
	int rc = madc_project_execute(engine, manifest, run_argc, run_argv,
				      prog->registration_policy.enable_forest_bind,
				      prog->forest_bind_path,
				      prog->class_pattern_live_capture,
				      show_stats);
	// --show-stats: the lane printed its phase report; append the process
	// total only main() can see (same closing line as the single-TU lane).
	if ( show_stats )
	{
	    struct timeval _t_now;
	    gettimeofday(&_t_now, NULL);
	    fprintf(stderr, "[stats] total (in-process) .. %.3f s"
		    "  (excl. pre-main load + teardown)\n",
		    (_t_now.tv_sec - _t_main.tv_sec)
		    + (_t_now.tv_usec - _t_main.tv_usec) / 1e6);
	}
	return rc;
    }

    // No program file (D20, plan §41.5a). An artifact or dump request names
    // what to do with a translation unit, so it has nothing to work on
    // (gcc); otherwise -i or a terminal stdin is the REPL, and a piped stdin
    // is the program, as if named `-`.
    // The REPL's options — madc's own option words, as the reader consumed
    // them — are its Windows backend's command line (run_repl).
    const std::vector<std::string> repl_options(parsed.option_words);
    std::vector<char *> stdin_argv;
    if ( interactive && filearg < argc && strcmp(argv[filearg], "-") == 0 )
    {
	// `-i -` is `-i`: `-` names stdin, which -i reads as entries.
	if ( filearg + 1 < argc )
	{
	    std::cerr << "madc: -i -: the REPL takes no program arguments"
		      << std::endl;
	    return 1;
	}
	++filearg;
    }
    bool terminal = isatty(0) != 0;
    bool artifact_request = emit_native || do_emit || emit_pch
	|| emit_function_name || dump_source || dump_macro_table
	|| dump_cir || dump_nodes || dump_checked || dump_registered
	|| freeze_path || freeze_run;
    if ( stdio_transport && !lsp && !mcp )
    {
	std::cerr << "madc: --stdio names the transport of --lsp or --mcp" << std::endl;
	return 1;
    }
    if ( lsp )
    {
	if ( interactive || artifact_request || filearg + 1 < argc )
	{
	    std::cerr << "madc: --lsp serves a client on stdin/stdout; it takes"
			 " at most one file and no other mode" << std::endl;
	    return 1;
	}
	return run_lsp(std::move(prog), filearg < argc ? argv[filearg] : NULL,
		       repl_options);
    }
    // A tool server's tools come from its Program's imports, read before any
    // document parses: --import's modules enter it now.
    if ( mcp && !prog->import_command_line_modules() )
	return 1;
    if ( mcp && project_manifest )
    {
	if ( interactive || artifact_request || filearg != argc )
	{
	    std::cerr << "madc: --mcp --project serves a client on stdin/stdout;"
			 " it takes the project and no other mode" << std::endl;
	    return 1;
	}
	return run_mcp_project(std::move(prog), project_manifest, manifest);
    }
    if ( mcp )
    {
	if ( interactive || artifact_request || filearg + 1 != argc )
	{
	    std::cerr << "madc: --mcp serves a client on stdin/stdout; it takes"
			 " one file and no other mode" << std::endl;
	    return 1;
	}
	return run_mcp(std::move(prog), argv[filearg]);
    }
    if ( interactive && artifact_request )
    {
	std::cerr << "madc: -i runs the REPL; it cannot also write an artifact"
		     " or a dump" << std::endl;
	return 1;
    }
    if ( filearg >= argc )
    {
	if ( artifact_request )
	{
	    std::cerr << "madc: fatal error: no input files" << std::endl
		      << "compilation terminated." << std::endl;
	    return 1;
	}
	// --import with no source only tries the imports (python -c "import
	// NAME"), so a launcher tests before it starts: 0, or the import
	// error and a status of its own.
	if ( !interactive && !prog->command_line_imports.empty() )
	    return prog->import_command_line_modules() ? 0 : madc_import_unavailable_status;
	if ( interactive || terminal )
	    return run_repl(std::move(prog), terminal,
			    history_file_given ? history_file
					       : madc::history_default_path(),
			    &repl_theme, repl_options);
	static char stdin_path[] = "-";
	stdin_argv.assign(argv, argv + argc);
	stdin_argv.push_back(stdin_path);
	stdin_argv.push_back(NULL);
	argv = stdin_argv.data();
	filearg = argc++;
    }
    else if ( interactive )
	return run_repl(std::move(prog), terminal,
			history_file_given ? history_file
					   : madc::history_default_path(),
			&repl_theme, repl_options, argc - filearg, argv + filearg);

    if ( argc >= 2 && filearg < argc )
    {
	if ( dump_source )
	    prog->keep_trivia = true;   // preserve whitespace/comments for round-trip
	if ( do_emit && emit_lang == celCxx )
	    prog->keep_trivia = true;   // the C++ reverse-render echoes TU
					// statements from the retained tokens
	if ( freeze_path )
	    prog->begin_pack_recording();   // B4a: record grove payload v2 during lex+parse
	if ( freeze_path || freeze_run )
	    prog->forest_arena_enabled = true; // B3: populate forest_arena during parse — the arena IS the type-graph dump (v18)
	struct timeval _tk0, _tk1;     // --show-stats: tokenize (read+lex) wall time
	gettimeofday(&_tk0, NULL);
	// slice D: sample the forest-work clock at the same boundaries so the
	// lex/parse buckets can report NET compute (the clock's advance inside
	// each window is the forest share, carved into its own stats lines).
	double _fw_tk0 = prog->_forest_work_seconds;
	// The CLI compile is one thread: hold the name table for tokenize + parse
	// so every intern skips its per-call lock. Released before the program
	// runs (its threads may compile through eval).
	std::unique_ptr<madc::dis::istring_table::hold> intern_hold(
	    new madc::dis::istring_table::hold());
	// A NULL TokenProgram means a lexer-phase diagnostic already printed
	// (failed #include, unterminated literal, bad PP directive) and
	// compilation aborted — exit nonzero like every later phase does.
	if ( !(tp=tokenize_input(*prog, argv[filearg])) )
	    return 1;
	gettimeofday(&_tk1, NULL);
	double _fw_tk1 = prog->_forest_work_seconds;

	// -E / --dump-source: the preprocessed text; -dM: the effective macro
	// table after preprocessing (madc's PP runs during tokenize), sorted —
	// diffable against `gcc -dM -E`.
	if ( preprocess_only() )
	{
	    std::cout.flush();
	    return write_preprocessed_output(*prog, dump_macro_table,
					     generic_output_path);
	}

	// CIR pipeline: madc parse → CIR translate → c2mir compile → MIR execute.
	// This is the sole backend; the asmjit JIT codegen path was removed.
	struct timeval _ps0, _ps1;     // --show-stats: parse wall time
	gettimeofday(&_ps0, NULL);
	double _fw_ps0 = prog->_forest_work_seconds;
	bool parse_ok = prog->parse(tp);
	intern_hold.reset();
	gettimeofday(&_ps1, NULL);
	// A GUI module imported at parse (`import ui_web;`) lifts an armed
	// memory guard before the program runs: WebKit's address-space
	// reservations dwarf any sane program budget (owner ruling 2026-09-07).
	if ( parse_ok && prog->bound_gui_module )
	    madc_lift_memory_guard("a GUI module was imported");
	double _fw_ps1 = prog->_forest_work_seconds;
	// import (module form): the modules' TARGET spellings join the native
	// link closure exactly as -l spellings do (verbatim entries — the same
	// image the JIT binder opened).
	for ( const std::string &l : prog->module_link_libs )
	    cc_link_args.push_back(l);

	// --show-stats: report input volume, token-stream traffic, and phase timing
	// (read / lex / parse / c2mir-compile / execution, with tokens-per-second).
	// Defined as a lambda and called at every exit (failed parse, --emit, after
	// execute) so the c2mir + execution timers — populated only by the run path
	// — are included when available and 0 otherwise. A stuck/expensive
	// instantiation is exactly when these numbers matter, so print on failure too.
	auto print_stats = [&]() {
	    if ( !show_stats )
		return;
	    auto _secs = [](const timeval &a, const timeval &b) {
		return (b.tv_sec - a.tv_sec) + (b.tv_usec - a.tv_usec) / 1e6;
	    };
	    double bytes      = (double)prog->input_bytes();
	    double tok_wall   = _secs(_tk0, _tk1);
	    double read_secs  = prog->_read_seconds;
	    // slice D: the forest-work clock's advance inside each phase window
	    // — carved OUT of that phase's bucket so lex/parse/cir report NET
	    // compute (the forest lines below own the carved share).
	    double fw_lex     = _fw_tk1 - _fw_tk0;
	    double fw_parse   = _fw_ps1 - _fw_ps0;
	    double fw_cir     = prog->_cir_forest_seconds;
	    double lex_secs   = tok_wall - read_secs - fw_lex;
	    if ( lex_secs < 0 ) lex_secs = 0;
	    double parse_secs = _secs(_ps0, _ps1) - fw_parse;
	    if ( parse_secs < 0 ) parse_secs = 0;
	    double inst_secs  = prog->_inst_seconds;
	    double decl_secs  = parse_secs - inst_secs;
	    if ( decl_secs < 0 ) decl_secs = 0;
	    double cir_secs   = prog->_cir_build_seconds - fw_cir;
	    if ( cir_secs < 0 ) cir_secs = 0;
	    double c2mir_secs = prog->_c2mir_seconds;
	    double exec_secs  = prog->_exec_seconds;
	    // Reconcile to a measured in-process total. The named phases are the
	    // instrumented ones; "other" absorbs the un-instrumented in-process work
	    // (arg parse, Program/engine setup, MIR module link, between-phase gaps).
	    // accounted + other == total (in-process) by construction. The remaining
	    // gap to `time(1)`'s `real` is pre-main load + post-exit teardown, which
	    // main() cannot self-measure.
	    struct timeval _t_now;
	    gettimeofday(&_t_now, NULL);
	    double total_secs = _secs(_t_main, _t_now);
	    double accounted  = read_secs + lex_secs + parse_secs + cir_secs
			      + c2mir_secs + exec_secs
			      + fw_lex + fw_parse + fw_cir;
	    double other_secs = total_secs - accounted;
	    if ( other_secs < 0 ) other_secs = 0;
	    fprintf(stderr,
		"[stats] input read .......... %.1f KiB (%llu bytes)\n"
		"[stats] tokens produced ..... %llu (lexer)\n"
		"[stats] tokens consumed ..... %llu (parser; %.2fx produced)\n"
		"[stats] tokens re-read (>1x) . %llu   max reads/token: %u\n"
		"[stats] avg bytes/token ..... %.2f\n"
		"[stats] read time ........... %.3f s\n"
		"[stats] lex time ............ %.3f s  (%.0f tok/s)\n"
		"[stats] parse time .......... %.3f s  (%.0f tok/s)\n"
		"[stats]   instantiate ....... %.3f s  (%.0f%% of parse; %llu calls)\n"
		"[stats]     call sites ...... class=%llu opaque=%llu alias=%llu fn=%llu member-fn=%llu member-ctor=%llu capture=%llu\n"
		"[stats]   tsubst bodies ..... %llu hit / %llu fallback\n"
		"[stats]   class instantiate . %llu pattern / %llu parse / %llu cache / %llu opaque\n"
		"[stats]   class patterns .... %llu materialized / %llu deferred\n"
		"[stats]   resolver memo ...... %llu hit / %llu miss / %llu published\n"
		"[stats]   decl-parse ........ %.3f s  (PCH-cacheable share)\n"
		"[stats] cir build ........... %.3f s  (AST -> cir_node)\n"
		"[stats] c2mir compile ....... %.3f s\n"
		"[stats] execution .......... %.3f s\n"
		"[stats] other (setup/link) .. %.3f s  (un-instrumented in-process)\n"
		"[stats] total (in-process) .. %.3f s  (excl. pre-main load + teardown)\n",
		bytes / 1024.0, (unsigned long long)prog->input_bytes(),
		prog->_tok_produced,
		prog->_tok_consumed,
		prog->_tok_produced ? (double)prog->_tok_consumed / (double)prog->_tok_produced : 0.0,
		prog->_tok_reread, prog->_tok_max_reads,
		prog->_tok_produced ? bytes / (double)prog->_tok_produced : 0.0,
		read_secs,
		lex_secs,   lex_secs   > 0 ? (double)prog->_tok_produced / lex_secs   : 0.0,
		parse_secs, parse_secs > 0 ? (double)prog->_tok_consumed / parse_secs : 0.0,
		inst_secs,  parse_secs > 0 ? 100.0 * inst_secs / parse_secs : 0.0,
		prog->_inst_count,
		prog->_inst_class_count,
		prog->_inst_opaque_count,
		prog->_inst_alias_count,
		prog->_inst_fn_count,
		prog->_inst_member_fn_count,
		prog->_inst_member_ctor_count,
		prog->_inst_capture_count,
		prog->_tsubst_body_hits,
		prog->_tsubst_body_fallbacks,
		prog->_class_inst_pattern,
		prog->_class_inst_parse,
		prog->_class_inst_cache,
		prog->_class_inst_opaque,
		prog->_class_pattern_restore_materialized,
		prog->_class_pattern_restore_deferred,
		prog->_class_pattern_resolver_memo_hits,
		prog->_class_pattern_resolver_memo_misses,
		prog->_class_pattern_resolver_memo_published,
		decl_secs,
		cir_secs,
		c2mir_secs,
		exec_secs,
		other_secs,
		total_secs);
	    if ( !prog->_tsubst_body_fallback_profile.empty() )
	    {
		std::vector<std::pair<madc::dis::istring, Program::TsubstBodyProfile> > rows;
		for (std::map<madc::dis::istring, Program::TsubstBodyProfile>::const_iterator it =
			 prog->_tsubst_body_fallback_profile.begin();
		     it != prog->_tsubst_body_fallback_profile.end(); ++it)
		    rows.push_back(*it);
		std::sort(rows.begin(), rows.end(),
			  [](const std::pair<madc::dis::istring, Program::TsubstBodyProfile> &a,
			     const std::pair<madc::dis::istring, Program::TsubstBodyProfile> &b) {
			      if (a.second.count != b.second.count)
				  return a.second.count > b.second.count;
			      return a.first < b.first;
			  });
		fprintf(stderr, "[stats]   tsubst fallback profile (ranked):\n");
		for (size_t i = 0; i < rows.size(); ++i)
		    fprintf(stderr, "[stats]     %zu. %llu x %s  [why: %s]  sample=%s\n",
			    i + 1,
			    rows[i].second.count,
			    rows[i].first.c_str(),
			    rows[i].second.reason.empty()
				? "?" : rows[i].second.reason.c_str(),
			    rows[i].second.sample.c_str());
	    }
	    if ( !prog->_class_parse_profile.empty() )
	    {
		typedef std::pair<Program::ClassParseProfileKey,
			Program::ClassParseProfile> ClassProfileRow;
		std::vector<ClassProfileRow> rows;
		for (std::map<Program::ClassParseProfileKey,
			Program::ClassParseProfile>::const_iterator it =
			 prog->_class_parse_profile.begin();
		     it != prog->_class_parse_profile.end(); ++it)
		    rows.push_back(*it);
		std::sort(rows.begin(), rows.end(),
			  [](const ClassProfileRow &a, const ClassProfileRow &b) {
			      if (a.second.count != b.second.count)
				  return a.second.count > b.second.count;
			      return a.first < b.first;
			  });
		fprintf(stderr, "[stats]   class parse profile (ranked):\n");
		unsigned long long body_calls = 0;
		unsigned long long base_specs = 0;
		std::map<Program::ClassDeclKind, unsigned long long> decls;
		for (size_t i = 0; i < rows.size(); ++i)
		{
		    fprintf(stderr,
			"[stats]     %zu. %llu x %s [why: %s] sample=%s\n",
			i + 1, rows[i].second.count,
			rows[i].first.identity.c_str(),
			Program::class_parse_reason_name(rows[i].first.reason),
			rows[i].second.sample.c_str());
		    body_calls += rows[i].second.body_calls;
		    base_specs += rows[i].second.base_specs;
		    for (std::map<Program::ClassDeclKind,
			    unsigned long long>::const_iterator di =
			    rows[i].second.decls.begin();
			 di != rows[i].second.decls.end(); ++di)
			decls[di->first] += di->second;
		}
		fprintf(stderr,
			"[stats]   class parse census . %llu body / %llu base-spec\n",
			body_calls, base_specs);
		fprintf(stderr, "[stats]   class decl KINDs ....");
		for (std::map<Program::ClassDeclKind,
			unsigned long long>::const_iterator it = decls.begin();
		     it != decls.end(); ++it)
		    fprintf(stderr, " %s=%llu",
			Program::class_decl_kind_name(it->first), it->second);
		fprintf(stderr, "\n");
	    }
	    // Forest-bind startup breakdown (startup-latency R0): map + open
	    // (once per process), the per-#include bind walks, the one-shot
	    // decl restore (decl-index sweep / arena materialize / the
	    // registration remainder), node-segment loads, and the reader's
	    // decode traffic. Printed whenever the bind was attempted — a
	    // blob-less binary shows just the probe cost.
	    {
		Program::ForestBindStats fs = prog->forest_bind_stats();
		if ( prog->forest_system.directory_tried )
		    fprintf(stderr,
			"[stats] forest map+open ..... %.3f + %.3f s  (%s)\n",
			fs.map_secs, fs.open_secs,
			fs.opened ? "container bound" : "no container — live parse");
		// Every profile whose forest directory a walk opened, by role:
		// a strict compile opens no dialect image (dialect per-name plan
		// B2; forest_pack_gate's split legs read it).
		if ( fs.profiles_system || fs.profiles_dialect )
		    fprintf(stderr,
			"[stats] forest profiles ..... %llu system + %llu dialect opened\n",
			fs.profiles_system, fs.profiles_dialect);
		// The dialect image (B2): bound in place, beside the system pack.
		if ( fs.image_probed )
		{
		    if ( fs.image_bound )
			fprintf(stderr,
			    "[stats] dialect image ....... image bound (%llu units bound / %u packed;"
			    " %llu zstd frames)\n",
			    fs.image_units_bound, fs.image_units_total, fs.image_zstd_frames);
		    else
			fprintf(stderr, "[stats] dialect image ....... %s\n",
			    fs.image_found ? "found, not bound"
					   : "no image — the dialect binds as the system pack holds it");
		}
		// The dialect's units, on whichever forest holds them (the image,
		// or the system pack's fragment units where no image binds).
		if ( fs.dialect_units )
		    fprintf(stderr,
			"[stats] forest dialect ...... %llu units (%s); %llu names demanded"
			" (%llu hit), %llu records, %llu functions + %llu other"
			" declarations registered\n",
			fs.dialect_units, fs.dialect_on_image ? "dialect image" : "system pack",
			fs.dialect_names, fs.dialect_hits,
			fs.dialect_records, fs.dialect_funcs, fs.dialect_objects);
		if ( fs.opened )
		{
		    double reg_secs = fs.restore_secs - fs.declidx_secs - fs.mat_secs;
		    if ( reg_secs < 0 ) reg_secs = 0;
		    fprintf(stderr,
			"[stats] forest bind ......... %.3f s  (%llu units bound / %u packed)\n"
			"[stats] forest restore ...... %.3f s  (decl-index sweep %.3f + materialize %.3f + register %.3f)\n"
			"[stats] forest unit loads ... %.3f s  (%llu node-record segments; %llu of %llu records materialized)\n"
			"[stats] forest decode ....... %.3f s  (%llu zstd frames -> %.1f KiB; %llu copies, %.1f KiB)\n"
			"[stats] forest in phases .... lex %.3f + parse %.3f + cir %.3f s  (carved OUT of those buckets)\n",
			fs.bind_secs, fs.units_bound, fs.units_total,
			fs.restore_secs, fs.declidx_secs, fs.mat_secs, reg_secs,
			fs.unitload_secs, fs.unitload_count,
			fs.unit_records_materialized, fs.unit_records,
			fs.zstd_secs, fs.zstd_frames, fs.zstd_bytes / 1024.0,
			fs.copy_calls, fs.copy_bytes / 1024.0,
			fw_lex, fw_parse, fw_cir);
		    if ( fs.blocks_total )
			fprintf(stderr,
			    "[stats] forest blocks ....... %llu of %llu decoded  (%s)\n",
			    fs.blocks_decoded, fs.blocks_total, fs.block_rows.c_str());
		    if ( !fs.unit_rows.empty() )
			fprintf(stderr,
			    "[stats] forest node units ... %s  (unit:materialized/records[blocks decoded/stored])\n",
			    fs.unit_rows.c_str());
		    fprintf(stderr,
			"[stats] forest functions .... %llu eager + %llu derived deferred; %llu activated / %llu remain\n",
			fs.funcs_eager, fs.funcs_deferred,
			fs.funcs_activated, fs.funcs_remaining);
		    if ( !prog->_forest_unit_bind_costs.empty() )
		    {
			std::vector<std::pair<madc::dis::istring, double> > rows =
			    prog->_forest_unit_bind_costs;
			std::sort(rows.begin(), rows.end(),
				  [](const std::pair<madc::dis::istring, double> &a,
				     const std::pair<madc::dis::istring, double> &b) {
				      if (a.second != b.second)
					  return a.second > b.second;
				      return a.first < b.first;
				  });
			fprintf(stderr, "[stats]   bind units (self cost, ranked):\n");
			size_t shown = 0;
			double rest = 0.0;
			for (size_t i = 0; i < rows.size(); ++i)
			{
			    if (rows[i].second >= 0.0005)
			    {
				fprintf(stderr, "[stats]     %.3f s  %s\n",
					rows[i].second, rows[i].first.c_str());
				++shown;
			    }
			    else
				rest += rows[i].second;
			}
			if (shown < rows.size())
			    fprintf(stderr,
				"[stats]     %.3f s  (+%zu units under 0.5 ms)\n",
				rest, rows.size() - shown);
		    }
		}
	    }
	};

	if ( !parse_ok )
	{
	    print_stats();
	    return 1;
	}

	prog->script_argc = argc - filearg;
	prog->script_argv = argv + filearg;
	g_active_program = prog.get();

	// --dump-registered: the post-parse name-registration maps, sorted —
	// side B of the forest index-parity oracle; do not run.
	if ( dump_registered )
	{
	    prog->dump_registered_names(stdout);
	    print_stats();
	    return 0;
	}

	// --emit=c11|mc11: render the cir_node tree as C source; do not run.
	// Takes precedence over the freeze modes — an explicit render request
	// (e.g. the emit-C corpus harness over a .flags test) means "show me
	// the lowering", never "run it".
	if (do_emit)
	{
	    int erc = madc_cir_emit(prog.get(), argv[filearg], stdout, emit_lang);
	    print_stats();
	    return erc;
	}

	// --freeze / --freeze-append: freeze the translated module tree into
	// a forest snapshot container; do not run.
	if ( freeze_path )
	{
	    // --freeze-sources=: the source arm joins AFTER the parse, so a
	    // header the frozen TU tokenized keeps that record. An unreadable
	    // list or file fails the freeze.
	    for ( const std::string &list : freeze_source_lists )
		if ( !prog->pack_record_source_list(list) )
		    return 1;
	    // --freeze-ledger=: compile the C-lane runtime sources into ledger
	    // modules FIRST (each an independent compile with its own Program),
	    // then pack them alongside the groves. A ledger failure fails the
	    // freeze — a container that silently lacks the ledger it was asked
	    // for is worse than no container at all.
	    std::vector<cir_ledger_module> ledger;
	    if ( !freeze_ledger_srcs.empty()
		 && !madc_cir_ledger_compile(engine, freeze_ledger_srcs, ledger,
						 prog.get()) )
		return 1;
	    int frc = madc_cir_freeze(prog.get(), argv[filearg], freeze_path,
				      freeze_append, freeze_mir_cache,
				      ledger.empty() ? NULL : &ledger,
				      /*progress=*/true, &freeze_also_std,
				      freeze_role);
	    print_stats();
	    return frc == 0 ? 0 : 1;
	}

	// --freeze-run: prove the frozen round-trip in one invocation —
	// freeze to a temp container, then re-exec this madc binary in a
	// FRESH process with --run-frozen (a genuinely cross-process thaw:
	// no parser state, token arena, or live pool carries over).
	if ( freeze_run )
	{
	    std::string snapshot_path;
	    int tfd = madc::detail::make_temp_file("madc_frozen", snapshot_path);
	    if ( tfd < 0 )
	    {
		perror("madc: --freeze-run: temporary file");
		return 1;
	    }
	    close(tfd);
	    if ( madc_cir_freeze(prog.get(), argv[filearg], snapshot_path.c_str(), false,
				 freeze_mir_cache) != 0 )
	    {
		unlink(snapshot_path.c_str());
		return 1;
	    }
	    std::string opt = std::string("--run-frozen=") + snapshot_path;
	    std::string selfexe = madc_self_exe_path();   // resolved pre-spawn
	    std::vector<std::string> cargv;
	    cargv.push_back(argv[0]);
	    // Verbosity crosses the re-exec: the thaw side's diagnostics
	    // (trap-bind list, link trace) are DBG-gated in the CHILD.
	    if ( madc_verbose )
		cargv.push_back("-v");
	    cargv.push_back(opt);
	    for ( int i = filearg + 1; i < argc; ++i )   // program args after the source
		cargv.push_back(argv[i]);
	    madc::error rerr;
	    int rc = madc::Process::run_and_wait(selfexe, cargv, &rerr);
	    unlink(snapshot_path.c_str());
	    if ( rc < 0 )
	    {
		std::cerr << "madc: --freeze-run: " << rerr.message << std::endl;
		return 1;
	    }
	    return rc;
	}

	// AOT: -c → .o, -shared → .so, -o → linked executable; do not run.
	// (gcc CLI vocabulary; --emit-object/--emit-executable are aliases.)
	if ( emit_native )
	{
	    // Options after the source were consumed where they stood (the
	    // permutation after the option loop), so what remains here is a
	    // second operand this lane has no use for. Dropping it silently
	    // would build the artifact from part of the command line — a
	    // silent wrong answer. Refuse loudly instead.
	    if ( filearg + 1 < argc )
	    {
		std::cerr << "madc: unconsumed arguments after '" << argv[filearg]
			  << "' in AOT mode:";
		for ( int i = filearg + 1; i < argc; i++ )
		    std::cerr << " " << argv[i];
		std::cerr << std::endl;
		return 1;
	    }
	    MadcNativeKind kind;
	    const char *explicit_out = NULL;
	    const char *dflt_suffix = NULL;
	    if ( compile_object || emit_object_path )
	    {
		kind = mnkObject;
		explicit_out = emit_object_path ? emit_object_path
						: generic_output_path;
		dflt_suffix = ".o";
	    }
	    else if ( emit_relocatable )
	    {
		// gcc -r on a single source: compile + relocatable link —
		// one .o, default name a.out (gcc parity).
		kind = mnkRelocatable;
		explicit_out = generic_output_path;
	    }
	    else if ( emit_shared )
	    {
		kind = mnkShared;
		explicit_out = generic_output_path;
		// The artifact is the TARGET's: its default suffix comes from
		// the one library-suffix owner (a Windows-hosted or cross
		// madc names a shared output .dll / .dylib, never .so).
		dflt_suffix = madc_target_dso_suffix(madc_target_os);
	    }
	    else
	    {
		kind = no_pie ? mnkExecutable : mnkPieExecutable;
		explicit_out = emit_executable_path;   // -o / --emit-executable
	    }
	    std::string outpath;
	    if ( explicit_out )
		outpath = explicit_out;
	    else if ( dflt_suffix )
	    {
		// gcc semantics: strip the directory and extension, land the
		// artifact in the current directory (foo.mad -> foo.o / foo.so).
		outpath = madc::detail::host_path_stem(argv[filearg]) + dflt_suffix;
	    }
	    else
		outpath = "a.out";
	    int erc = madc_cir_emit_native(prog.get(), argv[filearg], kind,
					   outpath.c_str(), cc_link_args,
					   link_paths);
	    print_stats();
	    return erc == 0 ? 0 : 1;
	}

	struct timeval before, after;
#ifdef MADC_CROSS_TARGET
	// The default lane compiles AND RUNS (JIT). --dump-* still need the
	// compile side, but execution of target code on this host is
	// impossible -- refuse before c2mir/MIR_gen would produce it.
	return cross_refuse_run("JIT-run a program");
#endif
	gettimeofday(&before, NULL);
	int result = madc_cir_execute(prog.get(), argv[filearg],
				      argc - filearg, argv + filearg,
				      dump_cir, dump_nodes, dump_checked);
	gettimeofday(&after, NULL);
	DBG(std::cout << "CIR elapsed time: " << time_diff(before, after) << std::endl);
	print_stats();
	// main()'s return value IS the process exit status (gcc parity:
	// `./prog; echo $?`), a negative one included; a run that never
	// reached main() comes back as 1.
	return result;
    }
    // Not reached: with no program file the D20 dispatch above ran the REPL
    // or named stdin as the program.
    return 0;
}
