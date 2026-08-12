// The crash surface — platform fault reporters over one shared JIT-aware
// backtrace printer. Contract in madc_crash.h; deliberately free of madc
// headers so the Win32 arm can include <windows.h> (winnt.h's TokenType
// enumerator collides with madc's core TokenType).
#include "madc_crash.h"
#include "madc_dl.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>	// SetUnhandledExceptionFilter, CaptureStackBackTrace
#include <io.h>
#else
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

// Resolve a JIT (MIR-generated) code address to "func+0xoff [JIT]".
// Defined in madc_cir.cpp where the live MIR module is in scope. Returns 1
// if the address falls inside a generated function's code range.
extern "C" int madc_jit_symbolize(void *addr, char *out, unsigned long n);

void madc_crash_write(const char *data, std::size_t size)
{
    while ( size > 0 )
    {
#ifdef _WIN32
	int written = ::_write(2, data, (unsigned int)size);
#else
	ssize_t written = write(STDERR_FILENO, data, size);
#endif
	if ( written <= 0 )
	    return;
	data += written;
	size -= (std::size_t)written;
    }
}

void madc_crash_write_formatted(const char *data, int length,
				std::size_t capacity)
{
    if ( length <= 0 || capacity == 0 )
	return;
    std::size_t size = (std::size_t)length;
    if ( size >= capacity )
	size = capacity - 1;
    madc_crash_write(data, size);
}

// Symbolize each frame. JIT (MIR-generated) frames are invisible to
// backtrace_symbols/dladdr — resolve those against the live MIR module so
// a crash inside transpiled code reads as `func+0xoff [JIT]` instead of a
// bare address. Falls back to madcdl_addr for native (libc/madc) frames.
// Shared by the POSIX signal handler and the Win32 exception filter.
static void crash_print_backtrace(void *const *frames, int nf)
{
    const char *btheader = "Backtrace:\n";
    madc_crash_write(btheader, strlen(btheader));
    for (int i = 0; i < nf; i++)
    {
	char line[320], sym[200];
	if ( madc_jit_symbolize(frames[i], sym, sizeof(sym)) )
	{
	    int n = snprintf(line, sizeof(line), "  [%p] %s\n", frames[i], sym);
	    madc_crash_write_formatted(line, n, sizeof(line));
	    continue;
	}
	MadcDlInfo di;
	if ( madcdl_addr(frames[i], di) && di.sname )
	{
	    int n = snprintf(line, sizeof(line), "  [%p] %s+0x%lx\n", frames[i],
			     di.sname,
			     (unsigned long)((char *)frames[i] - (char *)di.saddr));
	    madc_crash_write_formatted(line, n, sizeof(line));
	}
	else
	{
	    int n = snprintf(line, sizeof(line), "  [%p] ??\n", frames[i]);
	    madc_crash_write_formatted(line, n, sizeof(line));
	}
    }
}

#ifndef _WIN32
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
    madc_crash_write(prefix, strlen(prefix));
    madc_crash_write(name, strlen(name));
    if ( info && (sig == SIGSEGV || sig == SIGBUS) )
    {
	char addrbuf[64];
	int n = snprintf(addrbuf, sizeof(addrbuf), " at address %p", info->si_addr);
	madc_crash_write_formatted(addrbuf, n, sizeof(addrbuf));
    }
    madc_crash_write("\n", 1);

    void *frames[64];
    int nf = backtrace(frames, 64);
    crash_print_backtrace(frames, nf);

    // Restore default handler and re-raise so the shell sees the real exit
    // status (and optionally produces a core dump).
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

void madc_install_crash_handler(void)
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
#else
// Win32 crash surface: faults arrive as SEH exceptions, not signals.
// SetUnhandledExceptionFilter runs after every frame declined; the filter
// prints the same name + address + backtrace shape as the POSIX handler,
// then returns CONTINUE_SEARCH so WER / a debugger / the NTSTATUS exit
// code still happen (the analogue of re-raising with SIG_DFL).
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *xp)
{
    DWORD code = (xp && xp->ExceptionRecord)
	       ? xp->ExceptionRecord->ExceptionCode : 0;
    const char *name = "exception";
    switch ( code )
    {
	case EXCEPTION_ACCESS_VIOLATION:
	    name = "EXCEPTION_ACCESS_VIOLATION (access violation)";	break;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	    name = "EXCEPTION_ILLEGAL_INSTRUCTION (illegal instruction)"; break;
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	    name = "EXCEPTION_INT_DIVIDE_BY_ZERO (integer divide by zero)"; break;
	case EXCEPTION_STACK_OVERFLOW:
	    name = "EXCEPTION_STACK_OVERFLOW (stack overflow)";		break;
	case EXCEPTION_IN_PAGE_ERROR:
	    name = "EXCEPTION_IN_PAGE_ERROR (page-in failure)";		break;
    }
    const char *prefix = "\nmadc: caught ";
    madc_crash_write(prefix, strlen(prefix));
    madc_crash_write(name, strlen(name));
    if ( xp && xp->ExceptionRecord
      && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) )
    {
	// ExceptionInformation[1] holds the address the faulting access touched.
	char addrbuf[64];
	int n = snprintf(addrbuf, sizeof(addrbuf), " at address %p",
			 (void *)xp->ExceptionRecord->ExceptionInformation[1]);
	madc_crash_write_formatted(addrbuf, n, sizeof(addrbuf));
    }
    madc_crash_write("\n", 1);

    void *frames[64];
    int nf = (int)CaptureStackBackTrace(0, 64, frames, NULL);
    crash_print_backtrace(frames, nf);
    return EXCEPTION_CONTINUE_SEARCH;
}

void madc_install_crash_handler(void)
{
    // The filter runs on the faulting thread's own stack; a reserved
    // guarantee keeps a stack overflow printable (the sigaltstack analogue,
    // per-thread — this covers the main thread, where JIT code runs).
    ULONG guarantee = 65536;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(crash_filter);
}
#endif
