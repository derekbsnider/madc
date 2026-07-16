#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/resource.h>
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
#include "madc_pch.h"
#include "cir_emit_c.h"   // CirEmitLang
#include "madc_project.h" // --project: compile_commands.json multi-TU driver

extern int madc_cir_execute(Program *prog, const char *source_name,
                             int user_argc, char **user_argv,
                             bool dump_tree = false, bool dump_nodes = false,
                             bool dump_checked = false);
extern int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
                         CirEmitLang lang);
extern int madc_cir_freeze(Program *prog, const char *source_name,
                           const char *out_path, bool append);
extern int madc_cir_execute_frozen(const char *container_path,
                                   int user_argc, char **user_argv);
extern int madc_cir_dump_forest(const char *container_path);

using namespace std;

// CLI-only active program pointer used by the crash handler to map a
// faulting JIT RIP back to source. Library consumers should provide
// their own crash/error plumbing instead of relying on process globals.
static Program *g_active_program = NULL;

// Resolve a JIT (MIR-generated) code address to "func+0xoff [JIT]".
// Defined in madc_cir.cpp where the live MIR module is in scope. Returns 1
// if the address falls inside a generated function's code range.
extern "C" int madc_jit_symbolize(void *addr, char *out, unsigned long n);

static void crash_write(const char *data, size_t size)
{
    while ( size > 0 )
    {
	ssize_t written = write(STDERR_FILENO, data, size);
	if ( written <= 0 )
	    return;
	data += written;
	size -= (size_t)written;
    }
}

static void crash_write_formatted(const char *data, int length, size_t capacity)
{
    if ( length <= 0 || capacity == 0 )
	return;
    size_t size = (size_t)length;
    if ( size >= capacity )
	size = capacity - 1;
    crash_write(data, size);
}

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
    crash_write(prefix, strlen(prefix));
    crash_write(name, strlen(name));
    if ( info && (sig == SIGSEGV || sig == SIGBUS) )
    {
	char addrbuf[64];
	int n = snprintf(addrbuf, sizeof(addrbuf), " at address %p", info->si_addr);
	crash_write_formatted(addrbuf, n, sizeof(addrbuf));
    }
    crash_write("\n", 1);

    const char *btheader = "Backtrace:\n";
    crash_write(btheader, strlen(btheader));

    void *frames[64];
    int nf = backtrace(frames, 64);
    // Symbolize each frame. JIT (MIR-generated) frames are invisible to
    // backtrace_symbols/dladdr — resolve those against the live MIR module so
    // a crash inside transpiled code reads as `func+0xoff [JIT]` instead of a
    // bare address. Falls back to dladdr for native (libc/madc) frames.
    for (int i = 0; i < nf; i++)
    {
	char line[320], sym[200];
	if ( madc_jit_symbolize(frames[i], sym, sizeof(sym)) )
	{
	    int n = snprintf(line, sizeof(line), "  [%p] %s\n", frames[i], sym);
	    crash_write_formatted(line, n, sizeof(line));
	    continue;
	}
	Dl_info di;
	if ( dladdr(frames[i], &di) && di.dli_sname )
	{
	    int n = snprintf(line, sizeof(line), "  [%p] %s+0x%lx\n", frames[i],
			     di.dli_sname,
			     (unsigned long)((char *)frames[i] - (char *)di.dli_saddr));
	    crash_write_formatted(line, n, sizeof(line));
	}
	else
	{
	    int n = snprintf(line, sizeof(line), "  [%p] ??\n", frames[i]);
	    crash_write_formatted(line, n, sizeof(line));
	}
    }

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
    // Run the handler on a dedicated alternate stack so a STACK OVERFLOW
    // (e.g. runaway recursion in JIT'd code) is still catchable+symbolizable —
    // without SA_ONSTACK the handler would re-fault on the exhausted stack and
    // the kernel would kill us silently with no backtrace.
    static char altstack[65536];	// fixed: SIGSTKSZ is non-constant on modern glibc
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
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

// Print the command-line help. madc has accreted gcc/clang-style options over
// time; keep this in sync with the argument parser in main().
static void print_usage(const char *prog)
{
    std::cout <<
"Usage: " << (prog ? prog : "madc") << " [options] <source> [program-args...]\n"
"\n"
"madc — My Advanced Dialect of C. Parses a C/C++ dialect into a cir_node tree,\n"
"lowers it through c2mir -> MIR, and JIT-executes it (or emits C / a PCH).\n"
"\n"
"Input / mode:\n"
"  <file>                  compile and JIT-run a single source file\n"
"  --project <db.json>     build from a compile_commands.json: compile each\n"
"                          translation unit, link the modules, run the entry\n"
"  <file.json>             a .json source is treated as a project manifest\n"
"                          (implicit --project; gcc/clang-style by extension)\n"
"  -E                      preprocess only (print the expanded source)\n"
"\n"
"Language / preprocessor (gcc/clang-style):\n"
"  --std=<std>             c89/c90/c99/c11/c17/c23 (c = c11), c++NN, or madc\n"
"                          (default; C++ keywords reserved). A .c TU under\n"
"                          --project defaults to C mode.\n"
"  -D<name>[=value]        define a preprocessor macro\n"
"  -I<dir>                 add an include search directory\n"
"  -l<name>                dlopen lib<name>.so (RTLD_GLOBAL) so its symbols\n"
"                          resolve at link time (e.g. -lcrypt). Works with or\n"
"                          without --project.\n"
"  --no-auto-load          do not act on #load directives (e.g. an embedded\n"
"                          header auto-loading libm/libcrypt); link explicitly\n"
"                          via -l instead. The namespace binds to global scope.\n"
"  --no-includes           do not process #include directives\n"
"  --no-embedded-headers   disable baked-in headers; use real system headers\n"
"\n"
"Codegen:\n"
"  -O, -O0 .. -O3          JIT optimization level (bare -O = -O1)\n"
"  -fno-builtin-<name>     disable a specific builtin\n"
"  --finstrument-functions emit __cyg_profile instrumentation hooks\n"
"\n"
"Output (no run):\n"
"  --emit=c11|mc11         render the cir_node tree as C / MC11 source\n"
"  --emit-pch              write a .madh precompiled header\n"
"  --emit-function <name>  print one function's source\n"
"  --dump-source           reconstruct full-fidelity source\n"
"  --dump-cir | --dump-nodes | --dump-cir-checked   dump the cir_node tree\n"
"\n"
"Frozen forest (compile once, run without parsing):\n"
"  --freeze=<file>         parse + translate, then freeze the module tree\n"
"                          into a forest snapshot container (no run)\n"
"  --freeze-append=<bin>   same, but append the container to an existing\n"
"                          binary (found later from its EOF footer)\n"
"  --run-frozen[=<file>]   thaw + compile + run a frozen container; with no\n"
"                          value, load the blob appended to this executable.\n"
"                          Remaining arguments become the program's argv\n"
"  --freeze-run            freeze to a temp container, then re-exec this\n"
"                          madc in a FRESH process to run it (round-trip)\n"
"  --dump-forest[=<file>]  print a container's directory + grove payloads\n"
"                          (decl index, PP exports, edges, branch macros,\n"
"                          canonical order); no value = this executable's blob\n"
"  --forest-bind[=<file>]  bind grove-backed system #includes from a frozen\n"
"                          container instead of live-parsing; no value = the\n"
"                          blob appended to this executable. This is the\n"
"                          DEFAULT for compiles (silent live fall-through\n"
"                          when no blob is appended); freeze modes live-parse\n"
"                          unless it is passed explicitly\n"
"  --no-forest-bind        force live parse (overrides the default and an\n"
"                          explicit --forest-bind; the A/B measurement lever)\n"
"  --dump-registered       parse, then print the registered name maps\n"
"                          (forest index-parity oracle input; no run)\n"
"  -dM                     preprocess, then print the effective macro table\n"
"                          (gcc -dM -E analogue; no parse, no run)\n"
"\n"
"Misc:\n"
"  --show-stats            print input/token/timing stats (read, lex, parse,\n"
"                          c2mir, execute) to stderr after the run\n"
"  -v, --verbose           verbose / debug output\n"
"  -h, -?, --help          show this help\n"
"\n"
"Note: AOT object/executable output (--emit-object/--emit-executable/-o) is not\n"
"available on the CIR backend; emit C with --emit=c11 and compile with gcc/clang.\n";
}

int main(int argc, char **argv)
{
    // --show-stats: earliest in-process timestamp, so the phase breakdown can
    // reconcile to a measured in-process total (the only part main() can see;
    // pre-main dynamic-load/static-init and post-exit teardown are outside it).
    struct timeval _t_main;
    gettimeofday(&_t_main, NULL);

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
    const char *generic_output_path = NULL;
    const char *emit_function_name = NULL;
    bool emit_pch = false;
    bool dump_cir = false;        // --dump-cir: dump CIR tree before checking
    bool dump_nodes = false;      // --dump-nodes: dump cir_node tree via madc walker
    bool dump_source = false;     // --dump-source: full-fidelity source reconstruction (trivia round-trip)
    bool dump_checked = false;    // --dump-cir-checked: post-check tree dump (c2m -d stage)
    bool do_emit = false;         // --emit=c11|mc11: render cir_node tree as C, no run
    CirEmitLang emit_lang = celC11;
    const char *project_manifest = NULL;  // --project <compile_commands.json>
    std::vector<std::string> link_libs;   // -l<name>: dlopen lib<name>.so (RTLD_GLOBAL)
    bool show_help = false;               // --help / -h / -?
    bool show_stats = false;               // --show-stats: print input/token traffic counters
    const char *freeze_path = NULL;       // --freeze= / --freeze-append=: forest container out
    bool freeze_append = false;           // --freeze-append=: placement 2 (append to binary)
    bool run_frozen = false;              // --run-frozen[=path]: thaw + run, no parse
    const char *run_frozen_path = NULL;   // NULL = the blob appended to this executable
    bool freeze_run = false;              // --freeze-run: freeze, then re-exec fresh to run
    bool dump_forest = false;             // --dump-forest[=path]: print container surfaces
    const char *dump_forest_path = NULL;  // NULL = the blob appended to this executable
    bool dump_registered = false;         // --dump-registered: post-parse name maps (oracle side B)
    bool dump_macro_table = false;        // -dM: effective macro table after lex (gcc -dM -E analogue)
    bool no_forest_bind = false;          // --no-forest-bind: force live parse (overrides the default and --forest-bind)

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            madc_verbose = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "-O", 2) == 0 && (argv[i][2] == '\0' ||
                   (argv[i][2] >= '0' && argv[i][2] <= '3' && argv[i][3] == '\0'))) {
            // -O / -O0..-O3 : codegen optimization level (bare -O == -O1).
            madc_opt_level = (argv[i][2] == '\0') ? 1 : (argv[i][2] - '0');
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-object") == 0 && i + 1 < argc) {
            emit_object_path = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "--emit-executable") == 0 && i + 1 < argc) {
            emit_executable_path = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            generic_output_path = argv[++i];
            filearg = i + 1;
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            // -Ipath or -I path
            const char *path = argv[i] + 2;
            if ( *path == '\0' && i + 1 < argc )
                path = argv[++i];
            prog->add_include_dir(path);	// guards empty + normalizes trailing '/'
            filearg = i + 1;
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            // -DNAME, -DNAME=VALUE, or -D NAME (repeatable). A bare NAME defines
            // it as "1" (gcc behavior). Object-like only; applied after the
            // builtin defines so a -D can override one. (Used to supply the
            // predefined-macro environment real system headers branch on.)
            const char *def = argv[i] + 2;
            if ( *def == '\0' && i + 1 < argc )
                def = argv[++i];
            prog->add_cli_define(def);	// guards empty + splits NAME[=VALUE]
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
        } else if (strcmp(argv[i], "--no-embedded-headers") == 0) {
            // Header partition (madc-header-partition-handoff.md): bypass embedded
            // SYSTEM-library shims (glibc/libstdc++ twins) so `#include <...>` uses
            // the REAL headers on disk, while KEEPING madc-own headers (ns_*/__madc__,
            // which have no real twin) and compiler-owned freestanding headers
            // (stddef/limits/float). The classifier is data-driven
            // (embedded_header_is_system_library_shim). This is the incremental
            // shim-retirement lever; it replaces the old disallow-everything gate,
            // which wrongly also dropped ns_php etc.
            prog->registration_policy.bypass_system_library_headers = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--no-auto-load") == 0) {
            // Do not act on #load directives: the named library is not loaded
            // and the namespace binds to the global symbol scope, so linking
            // is explicit (e.g. via -l). Set on the engine too so --project
            // translation-unit Programs (created from the engine) inherit it.
            engine.registration_policy.enable_auto_library_loading = false;
            prog->registration_policy.enable_auto_library_loading = false;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--finstrument-functions") == 0) {
            prog->instrument_functions = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "-fno-builtin-", strlen("-fno-builtin-")) == 0) {
            const char *name = argv[i] + strlen("-fno-builtin-");
            if ( *name )
                prog->disabled_builtin_names.insert(name);
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-cir") == 0) {
            dump_cir = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-nodes") == 0) {
            dump_nodes = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-cir-checked") == 0) {
            dump_checked = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-source") == 0) {
            dump_source = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--show-stats") == 0) {
            show_stats = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "-E") == 0) {
            // -E: preprocess only — expand #include/#define/macros and print the
            // resulting token stream as text, then stop (no parse, no codegen).
            // madc's lexer preprocesses during tokenize(), so the token stream IS
            // the preprocessed translation unit; this reuses the dump_source path
            // (reconstruct_source over the post-PP tokens). Content-only (no
            // `# line` markers) so it diffs cleanly against `gcc -E | grep -v '^#'`.
            dump_source = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "--freeze=", 9) == 0) {
            freeze_path = argv[i] + 9;
            filearg = i + 1;
        } else if (strncmp(argv[i], "--freeze-append=", 16) == 0) {
            freeze_path = argv[i] + 16;
            freeze_append = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--freeze-run") == 0) {
            freeze_run = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--run-frozen") == 0) {
            run_frozen = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "--run-frozen=", 13) == 0) {
            run_frozen = true;
            run_frozen_path = argv[i] + 13;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-forest") == 0) {
            dump_forest = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "--dump-forest=", 14) == 0) {
            dump_forest = true;
            dump_forest_path = argv[i] + 14;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--forest-bind") == 0) {
            // Phase 6 (opt-in): bind grove-backed system #includes from the
            // blob appended to this executable instead of live-parsing them.
            prog->forest_bind_enabled = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "--forest-bind=", 14) == 0) {
            // --forest-bind=PATH: bind from a standalone --freeze container
            // (dev/testing without appending to the binary).
            prog->forest_bind_enabled = true;
            prog->forest_bind_path = argv[i] + 14;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--no-forest-bind") == 0) {
            // Force live parse: overrides both the packed-binary default and
            // an explicit --forest-bind (the A/B lever for measurement).
            no_forest_bind = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--dump-registered") == 0) {
            dump_registered = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "-dM") == 0) {
            dump_macro_table = true;
            filearg = i + 1;
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_manifest = argv[++i];
            filearg = i + 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0
                || strcmp(argv[i], "-?") == 0) {
            show_help = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
            // -l<name>: dlopen a shared library so its symbols are resolvable by
            // the import resolver at link time (e.g. -lcrypt). Like a linker's
            // -l, but it dlopen()s lib<name>.so (RTLD_GLOBAL). A name containing
            // '/' or ending in .so is used verbatim.
            std::string lib(argv[i] + 2);
            if ( lib.find('/') == std::string::npos
              && (lib.size() < 3 || lib.compare(lib.size() - 3, 3, ".so") != 0) )
                lib = "lib" + lib + ".so";
            link_libs.push_back(lib);
            filearg = i + 1;
        } else if (strncmp(argv[i], "--emit=", 7) == 0) {
            // Render the cir_node tree (MC11-IR) as C source; do not run.
            const char *lang = argv[i] + 7;
            if (strcmp(lang, "c11") == 0)       emit_lang = celC11;
            else if (strcmp(lang, "mc11") == 0) emit_lang = celMC11;
            else {
                std::cerr << "Unknown --emit target: " << lang
                          << " (c11|mc11)" << std::endl;
                return 1;
            }
            do_emit = true;
            filearg = i + 1;
        } else if (prog->set_language_standard_option(argv[i])) {
            filearg = i + 1;
        } else if (strncmp(argv[i], "--std=", 6) == 0) {
            std::cerr << "Unknown --std target: " << (argv[i] + 6) << std::endl;
            return 1;
        } else {
            filearg = i;
            break;
        }
    }

    prog->class_parse_observability = show_stats;
    {
	const char *live_capture = getenv("MADC_CLASS_PATTERN_LIVE");
	if ( live_capture && *live_capture && strcmp(live_capture, "0") != 0 )
	    prog->class_pattern_live_capture = true;
    }

    if ( show_help )
    {
        print_usage(argv[0]);
        return 0;
    }

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

    // Embedded-forest default (Phase 4): with no explicit forest flag, bind
    // system #includes from the blob appended to this executable —
    // ensure_bind_forest() falls through silently to live parse when no blob
    // is appended or the context pin mismatches. Freeze modes are excluded:
    // a freeze PRODUCES forest state from a live parse; binding while
    // freezing stays an explicit test-harness request, never the default.
    if ( no_forest_bind )
    {
        prog->forest_bind_enabled = false;
        prog->forest_bind_path.clear();
    }
    else if ( !prog->forest_bind_enabled && !freeze_path && !freeze_run )
        prog->forest_bind_enabled = true;

    // -l<name>: dlopen each requested library (RTLD_GLOBAL) so the import
    // resolver (dlsym(RTLD_DEFAULT, ...)) finds its symbols at link time. Done
    // before any compile/run so it applies to both the single-file and
    // --project paths.
    for ( const std::string &lib : link_libs )
    {
        if ( !dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL) )
        {
            std::cerr << "madc: -l: failed to load " << lib << ": "
                      << dlerror() << std::endl;
            return 1;
        }
        prog->loaded_lib_paths.push_back(lib);   // the frozen-forest link closure
    }

    // --run-frozen: thaw + compile + run a frozen forest container — no
    // source file, no parse. Remaining positionals become the program's
    // argv (argv[0] is the container / executable path).
    if ( run_frozen )
    {
        std::string ra0 = run_frozen_path ? run_frozen_path : "/proc/self/exe";
        std::vector<char *> rargv;
        rargv.push_back((char *)ra0.c_str());
        for ( int i = filearg; i < argc; ++i )
            rargv.push_back(argv[i]);
        int rc = madc_cir_execute_frozen(run_frozen_path,
                                         (int)rargv.size(), rargv.data());
        return (rc < 0) ? 1 : rc;
    }

    // --dump-forest: print a container's directory + grove payload v2
    // surfaces (decl index, PP exports, edges, branch macros, canonical
    // order) — the B4a oracle data source. No source file, no parse.
    if ( dump_forest )
    {
        int rc = madc_cir_dump_forest(dump_forest_path);
        return (rc < 0) ? 1 : 0;
    }

    if ( generic_output_path && !emit_pch )
	emit_executable_path = generic_output_path;

    if ( emit_object_path || emit_executable_path )
	prog->aot_tracking = true;

    if (emit_object_path || emit_executable_path) {
	std::cerr << "AOT object/executable output is not available on the CIR "
	             "backend; emit C with --emit-c and compile with gcc/clang"
	          << std::endl;
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
	if ( generic_output_path )
	    outpath = generic_output_path;
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

    if ( project_manifest )
    {
	ProjectManifest manifest;
	std::string err;
	if ( !read_compile_commands(project_manifest, manifest, err) )
	{
	    std::cerr << "madc --project: " << err << std::endl;
	    return 1;
	}
	// Remaining positionals (filearg..argc) become the program's argv.
	int run_argc = argc - filearg;
	char **run_argv = argv + filearg;
	int rc = madc_project_execute(engine, manifest, run_argc, run_argv,
				      prog->forest_bind_enabled,
				      prog->forest_bind_path);
	return (rc < 0) ? 1 : rc;
    }

    if ( argc >= 2 && filearg < argc )
    {
	if ( dump_source )
	    prog->keep_trivia = true;   // preserve whitespace/comments for round-trip
	if ( freeze_path )
	    prog->pack_recording = true;   // B4a: record grove payload v2 during lex+parse
	if ( freeze_path || freeze_run )
	    prog->forest_arena_enabled = true; // B3: populate forest_arena during parse — the arena IS the type-graph dump (v18)
	struct timeval _tk0, _tk1;     // --show-stats: tokenize (read+lex) wall time
	gettimeofday(&_tk0, NULL);
	if ( !(tp=prog->tokenize(argv[filearg])) )
	    return 0;
	gettimeofday(&_tk1, NULL);

	if ( dump_source )
	{
	    std::cout << prog->reconstruct_source();
	    return 0;
	}

	// -dM: the effective macro table after preprocessing (madc's PP runs
	// during tokenize), sorted — diffable against `gcc -dM -E`.
	if ( dump_macro_table )
	{
	    prog->dump_macros(stdout);
	    return 0;
	}

	// CIR pipeline: madc parse → CIR translate → c2mir compile → MIR execute.
	// This is the sole backend; the asmjit JIT codegen path was removed.
	struct timeval _ps0, _ps1;     // --show-stats: parse wall time
	gettimeofday(&_ps0, NULL);
	bool parse_ok = prog->parse(tp);
	gettimeofday(&_ps1, NULL);

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
	    double lex_secs   = tok_wall - read_secs;
	    if ( lex_secs < 0 ) lex_secs = 0;
	    double parse_secs = _secs(_ps0, _ps1);
	    double inst_secs  = prog->_inst_seconds;
	    double decl_secs  = parse_secs - inst_secs;
	    if ( decl_secs < 0 ) decl_secs = 0;
	    double cir_secs   = prog->_cir_build_seconds;
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
			      + c2mir_secs + exec_secs;
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
		std::vector<std::pair<std::string, Program::TsubstBodyProfile> > rows;
		for (std::map<std::string, Program::TsubstBodyProfile>::const_iterator it =
			 prog->_tsubst_body_fallback_profile.begin();
		     it != prog->_tsubst_body_fallback_profile.end(); ++it)
		    rows.push_back(*it);
		std::sort(rows.begin(), rows.end(),
			  [](const std::pair<std::string, Program::TsubstBodyProfile> &a,
			     const std::pair<std::string, Program::TsubstBodyProfile> &b) {
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
	    int frc = madc_cir_freeze(prog.get(), argv[filearg], freeze_path,
				      freeze_append);
	    print_stats();
	    return frc == 0 ? 0 : 1;
	}

	// --freeze-run: prove the frozen round-trip in one invocation —
	// freeze to a temp container, then re-exec this madc binary in a
	// FRESH process with --run-frozen (a genuinely cross-process thaw:
	// no parser state, token arena, or live pool carries over).
	if ( freeze_run )
	{
	    char tmpl[] = "/tmp/madc_frozen_XXXXXX";
	    int tfd = mkstemp(tmpl);
	    if ( tfd < 0 )
	    {
		perror("madc: --freeze-run: mkstemp");
		return 1;
	    }
	    close(tfd);
	    if ( madc_cir_freeze(prog.get(), argv[filearg], tmpl, false) != 0 )
	    {
		unlink(tmpl);
		return 1;
	    }
	    std::string opt = std::string("--run-frozen=") + tmpl;
	    std::vector<char *> cargv;
	    cargv.push_back(argv[0]);
	    cargv.push_back((char *)opt.c_str());
	    for ( int i = filearg + 1; i < argc; ++i )   // program args after the source
		cargv.push_back(argv[i]);
	    cargv.push_back(NULL);
	    pid_t pid = fork();
	    if ( pid == 0 )
	    {
		execv("/proc/self/exe", cargv.data());
		perror("madc: --freeze-run: execv");
		_exit(127);
	    }
	    int status = 0;
	    if ( pid > 0 )
		waitpid(pid, &status, 0);
	    unlink(tmpl);
	    if ( pid < 0 )
	    {
		perror("madc: --freeze-run: fork");
		return 1;
	    }
	    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	}

	struct timeval before, after;
	gettimeofday(&before, NULL);
	int result = madc_cir_execute(prog.get(), argv[filearg],
				      argc - filearg, argv + filearg,
				      dump_cir, dump_nodes, dump_checked);
	gettimeofday(&after, NULL);
	DBG(std::cout << "CIR elapsed time: " << time_diff(before, after) << std::endl);
	print_stats();
	return (result < 0) ? 1 : 0;
    }
    std::cout << "Usage: madc [-v|--verbose] [-E] [--finstrument-functions] [-fno-builtin-name] <file.mad>" << std::endl;

    return 0;
}
