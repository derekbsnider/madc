// Unit tests for madc_crash — the fault reporter (src/madc_crash.cpp): every
// signal a program can die of by a fault carries the reporter, so the death is
// never silent. A trap (__builtin_trap, a pointer-authentication failure) is
// SIGILL on x86-64 and SIGTRAP on AArch64 (`brk`).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;	// the prologue every tests/unit file uses
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "../../src/madc_crash.h"

#ifndef _WIN32
#include <signal.h>
#include <string.h>

TEST_CASE("the crash reporter covers every fault signal, a trap's included")
{
	static const int fault_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
	const size_t n = sizeof(fault_signals) / sizeof(fault_signals[0]);
	struct sigaction saved[sizeof(fault_signals) / sizeof(fault_signals[0])];
	for (size_t i = 0; i < n; i++)
		sigaction(fault_signals[i], NULL, &saved[i]);

	madc_install_crash_handler();
	struct sigaction segv;
	sigaction(SIGSEGV, NULL, &segv);
	REQUIRE((segv.sa_flags & SA_SIGINFO) != 0);
	for (size_t i = 0; i < n; i++) {
		struct sigaction sa;
		sigaction(fault_signals[i], NULL, &sa);
		INFO("signal " << fault_signals[i]);
		CHECK((sa.sa_flags & SA_SIGINFO) != 0);
		CHECK(sa.sa_sigaction == segv.sa_sigaction);
	}

	for (size_t i = 0; i < n; i++)
		sigaction(fault_signals[i], &saved[i], NULL);
}
#endif
