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
    bool emit_pch = false;

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
        } else if (strcmp(argv[i], "--finstrument-functions") == 0) {
            prog->instrument_functions = true;
            filearg = i + 1;
        } else if (strncmp(argv[i], "-fno-builtin-", strlen("-fno-builtin-")) == 0) {
            const char *name = argv[i] + strlen("-fno-builtin-");
            if ( *name )
                prog->disabled_builtin_names.insert(name);
            filearg = i + 1;
        } else {
            filearg = i;
            break;
        }
    }

    if ( emit_object_path || emit_executable_path )
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

    if ( argc >= 2 && filearg < argc )
    {
	if ( !(tp=prog->tokenize(argv[filearg])) )
	    return 0;
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
