#ifndef __MADCDIS_PROCESS_H
#define __MADCDIS_PROCESS_H 1

#include "libmadc/datasource.h"
#include "madcdis/datachannel.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace madc {

#ifndef _WIN32
// Map a reaped waitpid status to the process-facing exit shape (128+signal
// for a killed child) — THE one owner (dupaudit family
// child_status_exit_mapping; gate: check-child-status-map-owner.sh).
// Defined in madc_process.cpp; Process::wait / wait_or_kill and the
// fork-Run reap (parse_run) all route through it. -1 = neither exited nor
// signaled.
int map_child_status(int child_status);
#endif

struct ProcessOptions
{
	std::vector<std::string> args;
	std::map<std::string, std::string> environment;
	std::string working_directory;
	// Leave the child's stderr on the parent's (shell-pipe semantics)
	// instead of piping it. A piped stderr that nobody drains blocks a
	// chatty child; inherit when no consumer will read stderr_channel().
	// When set, stderr_channel() reports itself unreadable and
	// pump_process skips its stderr leg.
	bool inherit_stderr = false;
	// Fork-as-isolation THROUGH the one spawn owner (madcide polish P3b):
	// when set, the forked child runs this body instead of exec'ing the
	// source — the pipes, the reap and the cancel are the owner's — and
	// _exit()s with its return (the low byte). The body runs in the child
	// after the stdio dup: it owns the task runtime's atfork reset and its
	// signal dispositions. POSIX only: start() refuses it on Windows (no
	// fork — a caller spawns a child of self there).
	std::function<int()> child_body;
	// Files the owner removes once the child is reaped (a snapshot the
	// child ran from); removed in the destructor, after the reap.
	std::vector<std::string> cleanup_paths;
	// The child on a PSEUDO-TERMINAL (madcide polish P3b-2, the embedded
	// Terminal): its stdin/stdout/stderr are the pty slave — its
	// controlling terminal (prompts flush, isatty holds, the tty echoes
	// what is written to it) — and the master is ONE fd that is both the
	// stdin channel (write) and the stdout channel (read); the stderr
	// channel is unreadable (the child's stderr is the pty). POSIX only:
	// start() refuses it on Windows (ConPTY is the named residue).
	bool pty = false;
};

class Process
{
public:
	Process(const DataSource &source, const ProcessOptions &options = ProcessOptions());
	~Process();

	bool start(error *err = nullptr);

	DataChannel &stdin_channel();
	DataChannel &stdout_channel();
	DataChannel &stderr_channel();

	bool close_stdin(error *err = nullptr);
	bool wait(error *err = nullptr);

	// Reap with an escalation deadline (MT-3b): poll for exit up to
	// grace_ms, then hard-kill (SIGKILL / TerminateProcess) and reap.
	// The cancelled-channel close path — a child that ignored
	// terminate()'s SIGTERM must not hang the caller forever. Prefer
	// wait() everywhere a child is EXPECTED to exit on its own.
	bool wait_or_kill(int grace_ms, error *err = nullptr);

	bool started() const;
	bool exited() const;
	int exit_status() const;
	// The child runs on a pseudo-terminal (the pty option honoured by
	// start()); false on pipes — and always on Windows for now.
	bool is_pty() const;
	void terminate();

	// Spawn `executable` with the caller's full argv (argv[0] included)
	// and ALL stdio inherited — no pipes, no channels — then wait.
	// Returns the child's exit code, or -1 with err set when the spawn
	// or wait fails or the child terminates abnormally. This is the
	// run-and-wait arm of the ONE process-spawn owner (madc --freeze-run
	// re-exec rides it); the Win32 backend replaces the platform
	// primitives here and in start() together.
	static int run_and_wait(const std::string &executable,
				const std::vector<std::string> &argv,
				error *err = nullptr);

private:
	Process(const Process &);
	Process &operator=(const Process &);

	struct impl;
	std::unique_ptr<impl> _;
};

namespace detail {
// The exec-style channel over a started Process: read = the child's stdout,
// write = its stdin, cancel = SIGTERM, close = reap (madc_process.cpp's
// ExecDataChannel) — for factories that spawn through the owner and hand the
// result to the registry (the madcrun:// / madcproj:// schemes).
std::unique_ptr<DataChannel> exec_channel_over(std::unique_ptr<Process> process);
}

struct ProcessPumpResult
{
	std::size_t input_bytes = 0;
	std::size_t output_bytes = 0;
	std::size_t stderr_bytes = 0;
	int exit_status = -1;
};

// Drives input -> child stdin, child stdout -> output, and child stderr ->
// stderr_output concurrently (three bounded pump threads), then reaps the
// child. input, output, and stderr_output must be DISTINCT channels — two
// pumps writing one channel would race (DataChannel is single-threaded; see
// the contract note in madcdis/datachannel.h). Failure paths use
// close_read()/terminate() as cross-thread wake-ups.
bool pump_process(DataChannel &input,
		  Process &process,
		  DataChannel &output,
		  DataChannel *stderr_output,
		  ProcessPumpResult &result,
		  error *err = nullptr);

} // namespace madc

#endif // __MADCDIS_PROCESS_H
