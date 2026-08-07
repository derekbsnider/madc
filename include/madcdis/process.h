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

	bool started() const;
	bool exited() const;
	int exit_status() const;
	void terminate();

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

bool pump_process(DataChannel &input,
		  Process &process,
		  DataChannel &output,
		  DataChannel *stderr_output,
		  ProcessPumpResult &result,
		  error *err = nullptr);

} // namespace madc

#endif // __MADCDIS_PROCESS_H
