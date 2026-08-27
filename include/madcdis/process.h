#ifndef __MADCDIS_PROCESS_H
#define __MADCDIS_PROCESS_H 1

#include "libmadc/datasource.h"
#include "madcdis/datachannel.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace madc {

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
