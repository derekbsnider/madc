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

using namespace std;

bool madc_verbose = false;

throwstream throwit;

// Globals exposed by the compiler so the crash handler can map a
// faulting JIT'd RIP back to the .mad source location that emitted it.
extern const Program::JitSourceEntry *g_madc_jit_map;
extern size_t g_madc_jit_map_size;
extern const void *g_madc_jit_code_base;
extern size_t g_madc_jit_code_size;

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
    if ( g_madc_jit_map && g_madc_jit_map_size
      && g_madc_jit_code_base && g_madc_jit_code_size )
    {
	uintptr_t base = (uintptr_t)g_madc_jit_code_base;
	uintptr_t end  = base + g_madc_jit_code_size;
	auto print_at = [&](uintptr_t pc, const char *header) {
	    if ( pc < base || pc >= end ) return false;
	    uint32_t offset = (uint32_t)(pc - base);
	    size_t lo = 0, hi = g_madc_jit_map_size, best = SIZE_MAX;
	    while ( lo < hi )
	    {
		size_t mid = lo + (hi - lo) / 2;
		if ( g_madc_jit_map[mid].byte_offset <= offset )
		{ best = mid; lo = mid + 1; }
		else hi = mid;
	    }
	    if ( best == SIZE_MAX ) return false;
	    const Program::JitSourceEntry &e = g_madc_jit_map[best];
	    char buf[512];
	    int n = snprintf(buf, sizeof(buf),
		"%s at +0x%x — last anchor +0x%x: %s:%u:%u (%s)\n",
		header, offset, e.byte_offset,
		e.file ? e.file : "(null)",
		(unsigned)e.line, (unsigned)e.col,
		e.kind ? e.kind : "?");
	    write(2, buf, n);
	    if ( best + 1 < g_madc_jit_map_size )
	    {
		const Program::JitSourceEntry &n2 = g_madc_jit_map[best + 1];
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

int main(int argc, char **argv)
{
    install_crash_handler();

    stringstream ss;
    Program prog;
    TokenProgram *tp;

    prog.colors = true;

    int filearg = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            madc_verbose = true;
            filearg = i + 1;
        } else {
            filearg = i;
            break;
        }
    }

    if ( argc >= 2 && filearg < argc )
    {
	if ( !(tp=prog.tokenize(argv[filearg])) )
	    return 0;
	if ( !prog.parse(tp) )
	    return 0;
	if ( !prog.compile() )
	    return 0;

	// set script argc/argv after tokenize/parse/compile (tokenizer_init resets members)
	prog.script_argc = argc - filearg;
	prog.script_argv = argv + filearg;

	struct timeval before, after;

	gettimeofday(&before, NULL);
	prog.execute();
	gettimeofday(&after, NULL);

	DBG(std::cout << "Elapsed time: " << time_diff(before, after) << std::endl);

	return 0;
    }
    std::cout << "Usage: madc [-v|--verbose] <file.mad>" << std::endl;

    return 0;
}
