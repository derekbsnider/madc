#ifndef __MADC_GUARDS_H
#define __MADC_GUARDS_H 1

// madc_guards — the process resource guards (RLIMIT_CPU / RLIMIT_AS) and
// the ONE parser of their knobs. OWNER RULING (2026-09-07, KG Decision
// resource_guards_default_off): madc arms NO guard by default — it is a
// developer CLI that also runs the program, and gcc/clang-style tools impose
// no self-limits; machine protection is the container's (a cgroup cap), and
// suites ask for their caps through the runner. Every knob reads
//   off      — no guard (the default; `0` is a synonym)
//   auto     — the computed default: memory 4096 MB + 128 MB per --project
//              TU (SMAUG's 51-TU manifest measures ~2.9 GB peak); CPU has
//              no safe finite default, so `auto` is off
//   <N>      — a fixed value (MB / seconds)
// from, in precedence order, the environment (MADC_MEM_LIMIT /
// MADC_CPU_LIMIT), madc.ini (mem-limit / cpu-limit), the default.
//
// An armed memory guard sets the SOFT limit only, so a program that binds a
// GUI-flagged module row (MADC_MODULE_GUI — WebKit reserves address space
// far beyond any sane program budget) lifts it at run start
// (madc_lift_memory_guard). Every trip names its knob (never silent).
//
// Thread contract: process-wide state set once before the program runs, on
// the main thread; the lift runs on the same thread before execution.

#include <string>
#include "madc_config.h"

enum class MadcGuardMode { off, auto_, fixed };

struct MadcGuardKnob {
	MadcGuardMode mode;
	unsigned long value;	// fixed: MB or seconds
	MadcGuardKnob() : mode(MadcGuardMode::off), value(0) {}
};

// `off` | `0` | `auto` | a whole number; false = not a knob spelling (the
// caller names the key in its diagnostic).
bool madc_guard_knob_parse(const std::string &text, MadcGuardKnob &out);

// Arm the guards per the knobs (env > madc.ini > default). Call after
// argument parsing (the memory default scales with the workload) and before
// anything allocates in earnest.
void madc_install_resource_guards(size_t project_tus,
				  const madc::config_settings &cfg);

// Lift an armed memory guard (soft limit back to the hard limit) because
// the program bound a GUI module row; `reason` names it for the DBG trace.
// False when no memory guard was armed.
bool madc_lift_memory_guard(const char *reason);

#endif
