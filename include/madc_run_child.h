/* madc_run_child.h — a Run in a child OF SELF where there is no fork.
 *
 * POSIX runs a live parse (madcrun://, madc::parse_run) or a project
 * (madcproj://, madc::project_run) in a fork child that inherits the tree.
 * Windows has no fork: the parent freezes the tree to a snapshot (or names
 * the manifest) and spawns ITS OWN executable as the child (design (b),
 * owner ruling 2026-08-28). That executable is the madc CLI, an IDE built on
 * the engine (madcide.exe, chthonia), or any program that calls the run
 * verbs, so the request cannot ride the command line, which belongs to the
 * program: it rides the child's environment, MADC_RUN_CHILD, and every
 * image that can make the request serves it before its own main runs (the
 * CLI's main calls madc_serve_run_child first; <ns_madc> calls it from a
 * namespace-scope initializer, the <iostream> ios_base::Init idiom).
 * Python's multiprocessing spawn of a frozen executable is the precedent.
 *
 * Thread contract: madc_run_child_process builds the caller's child (no
 * shared state); madc_serve_run_child runs at process start, before the
 * program's threads, and consumes the variable it reads.
 */

#ifndef __MADC_RUN_CHILD_H
#define __MADC_RUN_CHILD_H 1

#include <cstdint>
#include <memory>
#include <string>

namespace madc { struct ProcessOptions; class Process; }

// What the child runs: a frozen snapshot of a live parse (madc_cir_freeze's
// container) or a --project manifest.
enum MadcRunChildKind { rckFrozen, rckProject };

// Parent: the run child for `path` (the snapshot or the manifest) — THIS
// executable, with the request in its environment and the caller's
// options otherwise (stdio, cleanup_paths). Not started.
std::unique_ptr<madc::Process> madc_run_child_process(
    MadcRunChildKind kind, const std::string &path,
    madc::ProcessOptions options);

// Child: -1 when this process is not a run child. Otherwise it runs the
// request and exits the process with the guest's status (1 when the guest
// never ran) — it does not return.
int64_t madc_serve_run_child();

#endif // __MADC_RUN_CHILD_H
