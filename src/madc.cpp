#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/resource.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <asmjit/x86.h>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_pch.h"
#include "madc_sema.h"

extern "C" {
struct gp_tree_node;
}

// Transpiler pipeline (Gecko + MIR)
extern struct gp_tree_node *madc_gecko_parse(std::deque<TokenBase *> *tokens,
                                              int *out_ambiguity);
extern void madc_gecko_free_tree(struct gp_tree_node *root);
extern std::string madc_emit_c(struct gp_tree_node *root, SemaInfo *sema);
extern int madc_mir_execute(const std::string &c_source,
                             const std::string &source_name,
                             int user_argc, char **user_argv);
extern int madc_cir_execute(Program *prog, const char *source_name,
                             int user_argc, char **user_argv,
                             bool dump_tree = false, bool dump_nodes = false);

using namespace std;

// CLI-only active program pointer used by the crash handler to map a
// faulting JIT RIP back to source. Library consumers should provide
// their own crash/error plumbing instead of relying on process globals.
static Program *g_active_program = NULL;

// Async-signal-safe crash handler: writes signal name + backtrace to fd 2
// (stderr) using only async-signal-safe libc calls. Re-raises the signal
// with the default handler so core files still drop if enabled.
static void crash_handler(int sig, siginfo_t *info, void *uctx)
{
    const char *name = "signal";
    switch ( sig )
    {
	case SIGSEGV: name = "SIGSEGV (segmentation fault)";	break;
	case SIGABRT: name = "SIGABRT (abort)";			break;
	case SIGFPE:  name = "SIGFPE (arithmetic error)";	break;
	case SIGBUS:  name = "SIGBUS (bus error)";		break;
	case SIGILL:  name = "SIGILL (illegal instruction)";	break;
    }
    const char *prefix = "\nmadc: caught ";
    write(2, prefix, strlen(prefix));
    write(2, name, strlen(name));
    if ( info && (sig == SIGSEGV || sig == SIGBUS) )
    {
	char addrbuf[64];
	int n = snprintf(addrbuf, sizeof(addrbuf), " at address %p", info->si_addr);
	write(2, addrbuf, n);
    }
    write(2, "\n", 1);

    // JIT crash → source-line lookup. The faulting RIP usually lands
    // inside JIT'd code, but for crashes that bottom out in libc
    // (memcpy with NULL dst, strlen on bad pointer, etc.) RIP is in
    // glibc — walk the backtrace and report the first frame whose
    // address falls in the JIT'd region.
    if ( g_active_program
      && !g_active_program->jit_source_map.empty()
      && g_active_program->root_fn )
    {
	const std::vector<Program::JitSourceEntry> &jit_map = g_active_program->jit_source_map;
	size_t jit_map_size = jit_map.size();
	uintptr_t base = (uintptr_t)g_active_program->root_fn;
	uintptr_t end  = base + g_active_program->code.codeSize();
	auto print_at = [&](uintptr_t pc, const char *header) {
	    if ( pc < base || pc >= end ) return false;
	    uint32_t offset = (uint32_t)(pc - base);
	    size_t lo = 0, hi = jit_map_size, best = SIZE_MAX;
	    while ( lo < hi )
	    {
		size_t mid = lo + (hi - lo) / 2;
		if ( jit_map[mid].byte_offset <= offset )
		{ best = mid; lo = mid + 1; }
		else hi = mid;
	    }
	    if ( best == SIZE_MAX ) return false;
	    const Program::JitSourceEntry &e = jit_map[best];
	    char buf[512];
	    int n = snprintf(buf, sizeof(buf),
		"%s at +0x%x — last anchor +0x%x: %s:%u:%u (%s)\n",
		header, offset, e.byte_offset,
		e.file ? e.file : "(null)",
		(unsigned)e.line, (unsigned)e.col,
		e.kind ? e.kind : "?");
	    write(2, buf, n);
	    if ( best + 1 < jit_map_size )
	    {
		const Program::JitSourceEntry &n2 = jit_map[best + 1];
		int n3 = snprintf(buf, sizeof(buf),
		    "               next anchor +0x%x: %s:%u:%u (%s)\n",
		    n2.byte_offset,
		    n2.file ? n2.file : "(null)",
		    (unsigned)n2.line, (unsigned)n2.col,
		    n2.kind ? n2.kind : "?");
		write(2, buf, n3);
	    }
	    return true;
	};
	bool found = false;
	if ( uctx )
	{
	    ucontext_t *uc = (ucontext_t *)uctx;
	    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
	    found = print_at(rip, "JIT'd code");
	}
	if ( !found )
	{
	    void *frames[64];
	    int nf = backtrace(frames, 64);
	    for ( int i = 0; i < nf; ++i )
	    {
		if ( print_at((uintptr_t)frames[i], "JIT frame on stack") )
		    break;
	    }
	}
    }

    const char *btheader = "Backtrace:\n";
    write(2, btheader, strlen(btheader));

    void *frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, 2);

    // Restore default handler and re-raise so the shell sees the real exit
    // status (and optionally produces a core dump).
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void install_crash_handler()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

double time_diff(struct timeval x , struct timeval y)
{
	double x_ms , y_ms , diff;
	
	x_ms = (double)x.tv_sec*1000000 + (double)x.tv_usec;
	y_ms = (double)y.tv_sec*1000000 + (double)y.tv_usec;
	
	diff = (double)y_ms - (double)x_ms;
	
	return diff;
}

// Resource guards. Defaults are generous enough for normal compile +
// execute, strict enough that a runaway JIT loop or pathological alloc
// trips well before the host gets noticeable. All overridable via env:
//   MADC_CPU_LIMIT=<secs>   (default 60, 0 disables)
//   MADC_MEM_LIMIT=<MB>     (default 2048, 0 disables) — virtual address
//                            space (RLIMIT_AS), so includes JIT mappings
//                            and dlopen()'d shared libs.
// Soft = limit, hard = limit+slop so the process can't extend itself.
// Hitting RLIMIT_CPU sends SIGXCPU; hitting RLIMIT_AS makes the next
// mmap/brk return ENOMEM (allocators usually abort()).
static rlim_t env_rlim(const char *env_name, rlim_t fallback)
{
    if ( const char *env = getenv(env_name) ) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if ( end != env && v >= 0 ) return (rlim_t)v;
    }
    return fallback;
}

static void install_resource_guards(void)
{
    rlim_t cpu_secs = env_rlim("MADC_CPU_LIMIT", 60);
    if ( cpu_secs > 0 ) {
        struct rlimit rl;
        rl.rlim_cur = cpu_secs;
        rl.rlim_max = cpu_secs + 1;
        if ( setrlimit(RLIMIT_CPU, &rl) != 0 )
            perror("setrlimit(RLIMIT_CPU)");
    }

    rlim_t mem_mb = env_rlim("MADC_MEM_LIMIT", 2048);
    if ( mem_mb > 0 ) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)mem_mb * 1024 * 1024;
        rl.rlim_max = rl.rlim_cur;
        if ( setrlimit(RLIMIT_AS, &rl) != 0 )
            perror("setrlimit(RLIMIT_AS)");
    }
}

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

int main(int argc, char **argv)
{
    install_crash_handler();
    install_resource_guards();

    stringstream ss;
    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    TokenProgram *tp;

    prog->colors = true;

    int filearg = 1;
    const char *emit_object_path = NULL;
    const char *emit_executable_path = NULL;
    const char *emit_function_name = NULL;
    bool emit_pch = false;
    bool emit_c = false;
    bool use_mir_backend = true;  // MIR is the default backend
    bool use_cir_backend = false; // CIR: direct AST → c2mir (libc2mir)
    bool dump_cir = false;        // --dump-cir: dump CIR tree before checking
    bool dump_nodes = false;      // --dump-nodes: dump cir_node tree via madc walker
    bool dump_source = false;     // --dump-source: full-fidelity source reconstruction (trivia round-trip)

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            madc_verbose = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-object") == 0 && i + 1 < argc) {
            emit_object_path = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-executable") == 0 && i + 1 < argc) {
            emit_executable_path = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            emit_executable_path = argv[++i];
            filearg = i + 1;
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            // -Ipath or -I path
            const char *path = argv[i] + 2;
            if ( *path == '\0' && i + 1 < argc )
                path = argv[++i];
            if ( *path )
            {
                std::string p = path;
                if ( p.back() != '/' ) p += '/';
                prog->include_paths.push_back(p);
            }
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-pch") == 0) {
            emit_pch = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-function") == 0 && i + 1 < argc) {
            emit_function_name = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "--no-includes") == 0) {
            prog->skip_includes = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--finstrument-functions") == 0) {
            prog->instrument_functions = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "-fno-builtin-", strlen("-fno-builtin-")) == 0) {
            const char *name = argv[i] + strlen("-fno-builtin-");
            if ( *name )
                prog->disabled_builtin_names.insert(name);
            filearg = i + 1;
        } else if (strncmp(argv[i], "--backend=", 10) == 0) {
            const char *backend = argv[i] + 10;
            if (strcmp(backend, "mir") == 0)
                use_mir_backend = true;
            else if (strcmp(backend, "cir") == 0) {
                use_cir_backend = true;
                use_mir_backend = false;
            } else if (strcmp(backend, "jit") == 0 || strcmp(backend, "asmjit") == 0)
                use_mir_backend = false;
            else {
                std::cerr << "Unknown backend: " << backend
                          << " (use 'mir', 'cir', 'jit', or 'asmjit')" << std::endl;
                return 1;
            }
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-cir") == 0) {
            dump_cir = true;
            use_cir_backend = true;
            use_mir_backend = false;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-nodes") == 0) {
            dump_nodes = true;
            use_cir_backend = true;
            use_mir_backend = false;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-source") == 0) {
            dump_source = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = true;
            use_mir_backend = true;  // --emit-c implies MIR pipeline
            filearg = i + 1;
        } else if (strcmp(argv[i], "--std=c") == 0 || strcmp(argv[i], "--std=c11") == 0) {
            prog->language_std = Program::STD_C11;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--std=c89") == 0 || strcmp(argv[i], "--std=c90") == 0) {
            prog->language_std = Program::STD_C89;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--std=c99") == 0) {
            prog->language_std = Program::STD_C99;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--std=c17") == 0) {
            prog->language_std = Program::STD_C17;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--std=c23") == 0) {
            prog->language_std = Program::STD_C23;
            filearg = i + 1;
        } else {
            filearg = i;
            break;
        }
    }

    if ( emit_object_path || emit_executable_path )
	prog->aot_tracking = true;

    if (use_mir_backend && (emit_object_path || emit_executable_path)) {
	std::cerr << "--backend=mir does not support --emit-object or "
	          << "--emit-executable yet" << std::endl;
	return 1;
    }

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
	if ( emit_executable_path )
	    outpath = emit_executable_path;
	else
	{
	    outpath = input;
	    size_t dot = outpath.rfind('.');
	    if ( dot != std::string::npos )
		outpath = outpath.substr(0, dot);
	    outpath += ".madh";
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

    if ( argc >= 2 && filearg < argc )
    {
	if ( dump_source )
	    prog->keep_trivia = true;   // preserve whitespace/comments for round-trip
	if ( !(tp=prog->tokenize(argv[filearg])) )
	    return 0;

	if ( dump_source )
	{
	    std::cout << prog->reconstruct_source();
	    return 0;
	}

	if ( use_cir_backend )
	{
	    // CIR pipeline: madc parse → CIR translate → c2mir compile → MIR execute
	    if ( !prog->parse(tp) )
		return 1;

	    struct timeval before, after;
	    gettimeofday(&before, NULL);
	    int result = madc_cir_execute(prog.get(), argv[filearg],
					  argc - filearg, argv + filearg,
					  dump_cir, dump_nodes);
	    gettimeofday(&after, NULL);
	    DBG(std::cout << "CIR elapsed time: " << time_diff(before, after) << std::endl);
	    return (result < 0) ? 1 : 0;
	}

	if ( use_mir_backend )
	{
	    // Transpiler pipeline: Gecko parse → sema → emit C → MIR execute
	    int ambiguity = 0;
	    struct gp_tree_node *ast = madc_gecko_parse(&prog->tokens, &ambiguity);
	    if ( !ast )
	    {
		std::cerr << "Gecko parse failed for " << argv[filearg] << std::endl;
		return 1;
	    }

	    SemaInfo *sema = madc_sema_collect(ast);
	    std::string c_source = madc_emit_c(ast, sema);

	    if ( emit_c )
	    {
		std::cout << c_source;
		madc_sema_free(sema);
		madc_gecko_free_tree(ast);
		return 0;
	    }

	    struct timeval before, after;
	    gettimeofday(&before, NULL);
	    int result = madc_mir_execute(c_source, argv[filearg],
					  argc - filearg, argv + filearg);
	    gettimeofday(&after, NULL);

	    DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

	    madc_sema_free(sema);
	    madc_gecko_free_tree(ast);
	    return (result < 0) ? 1 : 0;
	}

	// Legacy asmjit pipeline
	if ( !prog->parse(tp) )
	    return 0;
	if ( !prog->compile() )
	    return 0;

	if ( emit_object_path )
	{
	    if ( prog->save_object(emit_object_path) )
		cerr << "wrote " << emit_object_path << endl;
	    else
		cerr << "failed to write " << emit_object_path << endl;
	    return 0;
	}

	if ( emit_executable_path )
	{
	    if ( prog->save_executable(emit_executable_path) )
		cerr << "wrote " << emit_executable_path << endl;
	    else
		cerr << "failed to write " << emit_executable_path << endl;
	    return 0;
	}

	// set script argc/argv after tokenize/parse/compile (tokenizer_init resets members)
	prog->script_argc = argc - filearg;
	prog->script_argv = argv + filearg;
	g_active_program = prog.get();

	struct timeval before, after;

	gettimeofday(&before, NULL);
	prog->execute();
	gettimeofday(&after, NULL);

	DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

	return 0;
    }
    std::cout << "Usage: madc [-v|--verbose] [--finstrument-functions] [-fno-builtin-name] <file.mad>" << std::endl;

    return 0;
}
