// madc_guards — the process resource guards and the one knob parser.
// Contract in include/madc_guards.h. Moved out of madc.cpp on 2026-09-07
// (the web-provider engine plan, Task 8) when the defaults turned off and
// the CIR project driver gained a caller (the GUI lift after parse).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <new>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "datadef.h"		// madc_verbose + the DBG(x) macro
#include "madc_guards.h"
#include "madc_crash.h"		// guard-handler writers (no allocation)

bool madc_guard_knob_parse(const std::string &text, MadcGuardKnob &out)
{
	std::string t;
	for (size_t i = 0; i < text.size(); i++)
		if (text[i] != ' ' && text[i] != '\t')
			t += (char)tolower((unsigned char)text[i]);
	if (t == "off" || t == "0") {
		out.mode = MadcGuardMode::off;
		out.value = 0;
		return true;
	}
	if (t == "auto") {
		out.mode = MadcGuardMode::auto_;
		out.value = 0;
		return true;
	}
	if (t.empty())
		return false;
	for (size_t i = 0; i < t.size(); i++)
		if (t[i] < '0' || t[i] > '9')
			return false;
	char *end = NULL;
	unsigned long v = strtoul(t.c_str(), &end, 10);
	if (!end || *end)
		return false;
	out.mode = MadcGuardMode::fixed;
	out.value = v;
	return true;
}

#ifndef _WIN32
// Precedence: environment > madc.ini > default (neither knob has a CLI
// flag, so the CLI layer of the rule is vacuous). A misspelled environment
// value is loud and falls back to the ini/default — never a silent zero.
static MadcGuardKnob guard_knob(const char *env_name, bool ini_seen,
				const std::string &ini_text)
{
	MadcGuardKnob knob;	// off
	if (ini_seen && !madc_guard_knob_parse(ini_text, knob))
		knob = MadcGuardKnob();
	if (const char *env = getenv(env_name)) {
		MadcGuardKnob e;
		if (madc_guard_knob_parse(env, e))
			knob = e;
		else
			fprintf(stderr, "madc: %s=%s is not a guard setting"
				" (off, auto or a whole number); ignored\n",
				env_name, env);
	}
	return knob;
}

// Armed with the RLIMIT_AS guard: when operator new first fails, say WHY
// (our own guard, and its knob) before the normal bad_alloc unwind —
// otherwise the failure surfaces as a bare std::bad_alloc with no
// actionable cause. An OOM handler must not allocate, so the message goes
// out via the crash handler's write(2) plumbing.
// Guarded to match the ONE place it is armed (the !__APPLE__ RLIMIT_AS arm
// below): darwin does not enforce RLIMIT_AS, so on Apple targets this handler
// is never installed and a definition here is simply unused.
#ifndef __APPLE__
static rlim_t madc_mem_guard_mb = 0;

static void mem_guard_new_handler(void)
{
	std::set_new_handler(NULL);	// print once; let bad_alloc propagate
	char buf[192];
	int n = snprintf(buf, sizeof(buf),
			 "madc: memory allocation failed with the MADC_MEM_LIMIT=%llu"
			 " MB address-space guard active; raise it or set"
			 " MADC_MEM_LIMIT=off to disable\n",
			 (unsigned long long)madc_mem_guard_mb);
	madc_crash_write_formatted(buf, n, sizeof(buf));
	throw std::bad_alloc();
}
#endif // !__APPLE__

// Armed with the (opt-in) RLIMIT_CPU guard: the default SIGXCPU disposition
// kills silently, which reads as a mystery death instead of the guard doing
// its job — name the knob first, then die with the real signal status.
static rlim_t madc_cpu_guard_secs = 0;

static void cpu_guard_handler(int sig)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf),
			 "madc: CPU time exceeded the MADC_CPU_LIMIT=%llu s guard;"
			 " raise it or set MADC_CPU_LIMIT=off to disable\n",
			 (unsigned long long)madc_cpu_guard_secs);
	madc_crash_write_formatted(buf, n, sizeof(buf));
	struct sigaction dfl;
	memset(&dfl, 0, sizeof(dfl));
	dfl.sa_handler = SIG_DFL;
	sigaction(sig, &dfl, NULL);
	raise(sig);
}
#endif // !_WIN32

void madc_install_resource_guards(size_t project_tus,
				  const madc::config_settings &cfg)
{
#ifdef _WIN32
	// Windows has no setrlimit. The JobObject equivalents
	// (JOB_OBJECT_LIMIT_PROCESS_MEMORY, PerProcessUserTimeLimit) kill the
	// process WITHOUT the nameable-knob message the POSIX guards guarantee,
	// so the guards are documented no-ops here — the same posture as
	// darwin's RLIMIT_AS below. A JobObject-based guard that still names
	// its knob is a W-lane residual.
	(void)project_tus;
	(void)cfg;
#else
	// CPU: `auto` has no safe finite value (madc also RUNS the program, and
	// a legitimate long-running one would die with SIGXCPU), so only a
	// fixed number arms it.
	MadcGuardKnob cpu = guard_knob("MADC_CPU_LIMIT", cfg.has_cpu_limit,
				       cfg.cpu_limit);
	if (cpu.mode == MadcGuardMode::fixed && cpu.value > 0) {
		struct rlimit rl;
		rl.rlim_cur = (rlim_t)cpu.value;
		rl.rlim_max = (rlim_t)cpu.value + 1;
		if (setrlimit(RLIMIT_CPU, &rl) != 0)
			perror("setrlimit(RLIMIT_CPU)");
		else {
			madc_cpu_guard_secs = (rlim_t)cpu.value;
			struct sigaction sa;
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = cpu_guard_handler;
			sigaction(SIGXCPU, &sa, NULL);
		}
	}

	// Memory: `auto` = 4096 MB + 128 MB per --project TU. A --project build
	// holds every TU's parsed state simultaneously (by design: all Programs
	// live until the shared MIR module runs), so its legitimate address-
	// space need scales with the manifest: SMAUG's 51-TU manifest measures
	// ~2.9 GB peak VA (~57 MB/TU), so 128 keeps ~2x headroom while a true
	// runaway still trips. A fixed value is a ceiling — it does not grow
	// with the manifest.
	MadcGuardKnob mem = guard_knob("MADC_MEM_LIMIT", cfg.has_mem_limit,
				       cfg.mem_limit);
	rlim_t mem_mb = 0;
	if (mem.mode == MadcGuardMode::auto_)
		mem_mb = 4096 + (project_tus > 1 ? 128 * (rlim_t)project_tus : 0);
	else if (mem.mode == MadcGuardMode::fixed)
		mem_mb = (rlim_t)mem.value;
#ifdef __APPLE__
	// darwin does not enforce RLIMIT_AS (setrlimit rejects finite values
	// with EINVAL) — the address-space guard is a no-op there. The CPU
	// guard above still applies; a mach-based memory guard is a P3 item.
	(void)mem_mb;
#else
	if (mem_mb > 0) {
		// SOFT limit only: the hard limit stays where the process found
		// it, so a GUI module's lift (madc_lift_memory_guard) can raise
		// it back — a hard limit can only ever go down.
		struct rlimit rl;
		if (getrlimit(RLIMIT_AS, &rl) != 0) {
			perror("getrlimit(RLIMIT_AS)");
			return;
		}
		rlim_t bytes = (rlim_t)mem_mb * 1024 * 1024;
		if (rl.rlim_max != RLIM_INFINITY && bytes > rl.rlim_max)
			bytes = rl.rlim_max;
		rl.rlim_cur = bytes;
		if (setrlimit(RLIMIT_AS, &rl) != 0)
			perror("setrlimit(RLIMIT_AS)");
		else {
			madc_mem_guard_mb = mem_mb;
			std::set_new_handler(mem_guard_new_handler);
		}
	}
#endif // !__APPLE__
#endif // !_WIN32
}

bool madc_lift_memory_guard(const char *reason)
{
#if defined(_WIN32) || defined(__APPLE__)
	(void)reason;
	return false;
#else
	if (madc_mem_guard_mb == 0)
		return false;
	struct rlimit rl;
	if (getrlimit(RLIMIT_AS, &rl) != 0) {
		perror("getrlimit(RLIMIT_AS)");
		return false;
	}
	rl.rlim_cur = rl.rlim_max;
	if (setrlimit(RLIMIT_AS, &rl) != 0) {
		perror("setrlimit(RLIMIT_AS)");
		return false;
	}
	DBG(std::cerr << "madc: memory guard (" << (unsigned long long)madc_mem_guard_mb
		      << " MB) lifted: " << (reason ? reason : "") << std::endl);
	madc_mem_guard_mb = 0;
	std::set_new_handler(NULL);
	return true;
#endif
}
