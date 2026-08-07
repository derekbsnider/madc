#include "madcdis/process.h"
#include "madc_posix_io.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <map>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace madc {
namespace {

void set_process_error(error *err, const std::string &message)
{
	if ( err )
		*err = error(error::severity::error, error::phase::runtime, message);
}

void set_process_errno(error *err, const std::string &operation, int number)
{
	set_process_error(err, operation + ": " + std::string(std::strerror(number)));
}

void close_fd(int &fd)
{
	if ( fd >= 0 )
		::close(fd);
	fd = -1;
}

bool make_cloexec_pipe(int fds[2], error *err)
{
	if ( ::pipe(fds) != 0 )
	{
		set_process_errno(err, "process pipe creation failed", errno);
		return false;
	}
	for ( int i = 0; i < 2; ++i )
	{
		if ( !detail::set_fd_close_on_exec(fds[i]) )
		{
			int number = errno;
			close_fd(fds[0]);
			close_fd(fds[1]);
			set_process_errno(err, "process pipe close-on-exec setup failed", number);
			return false;
		}
	}
	return true;
}

class ProcessPipeChannel : public DataChannel
{
public:
	ProcessPipeChannel(const std::string &channel_name, bool readable, bool writable)
		: name_(channel_name), fd_(-1), readable_(readable), writable_(writable)
	{}

	~ProcessPipeChannel() override { close(); }

	void assign(int fd)
	{
		close();
		fd_ = fd;
	}

	const char *name() const override { return name_.c_str(); }

	ChannelCapabilities capabilities() const override
	{
		ChannelCapabilities capabilities;
		capabilities.read = readable_ && fd_ >= 0;
		capabilities.write = writable_ && fd_ >= 0;
		return capabilities;
	}

	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override
	{
		bytes_read = 0;
		if ( !readable_ || fd_ < 0 )
		{
			set_process_error(err, name_ + " is not readable");
			return false;
		}
		ssize_t result;
		do
			result = ::read(fd_, buffer, capacity);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			set_process_errno(err, name_ + " read failed", errno);
			return false;
		}
		bytes_read = static_cast<std::size_t>(result);
		return true;
	}

	bool write(const void *buffer, std::size_t size, std::size_t &bytes_written,
		   error *err = nullptr) override
	{
		bytes_written = 0;
		if ( !writable_ || fd_ < 0 )
		{
			set_process_error(err, name_ + " is not writable");
			return false;
		}
		ssize_t result = detail::write_fd_without_sigpipe(fd_, buffer, size);
		if ( result < 0 )
		{
			set_process_errno(err, name_ + " write failed", errno);
			return false;
		}
		bytes_written = static_cast<std::size_t>(result);
		return true;
	}

	void close_read() override
	{
		if ( readable_ )
			close();
	}

	void close_write() override
	{
		if ( writable_ )
			close();
	}

	void close() override { close_fd(fd_); }

private:
	std::string name_;
	int fd_;
	bool readable_;
	bool writable_;
};

void close_pipe(int fds[2])
{
	close_fd(fds[0]);
	close_fd(fds[1]);
}

bool read_exec_error(int fd, int &number)
{
	unsigned char *next = reinterpret_cast<unsigned char *>(&number);
	std::size_t remaining = sizeof(number);
	while ( remaining )
	{
		ssize_t count = ::read(fd, next, remaining);
		if ( count == 0 )
			return false;
		if ( count < 0 )
		{
			if ( errno == EINTR )
				continue;
			return false;
		}
		next += count;
		remaining -= static_cast<std::size_t>(count);
	}
	return true;
}

void report_child_error(int fd, int number)
{
	const unsigned char *next = reinterpret_cast<const unsigned char *>(&number);
	std::size_t remaining = sizeof(number);
	while ( remaining )
	{
		ssize_t count = ::write(fd, next, remaining); // ASYNC-CHILD-WRITE
		if ( count < 0 && errno == EINTR )
			continue;
		if ( count <= 0 )
			break;
		next += count;
		remaining -= static_cast<std::size_t>(count);
	}
}

bool copy_counted(DataChannel &source, DataChannel *destination,
		  std::size_t &byte_count, error *err)
{
	unsigned char buffer[16384];
	for ( ;; )
	{
		std::size_t count = 0;
		if ( !source.read(buffer, sizeof(buffer), count, err) )
			return false;
		if ( count == 0 )
			return destination ? destination->flush(err) : true;
		byte_count += count;
		if ( destination && !write_all(*destination, buffer, count, err) )
			return false;
	}
}

} // namespace

struct Process::impl
{
	impl(const DataSource &process_source, const ProcessOptions &process_options)
		: source(process_source), options(process_options), child(-1),
		  stdin_pipe("process stdin", false, true),
		  stdout_pipe("process stdout", true, false),
		  stderr_pipe("process stderr", true, false),
		  has_started(false), has_exited(false), status(-1)
	{}

	DataSource source;
	ProcessOptions options;
	pid_t child;
	ProcessPipeChannel stdin_pipe;
	ProcessPipeChannel stdout_pipe;
	ProcessPipeChannel stderr_pipe;
	bool has_started;
	bool has_exited;
	int status;
};

Process::Process(const DataSource &source, const ProcessOptions &options)
	: _(new impl(source, options))
{}

Process::~Process()
{
	_->stdin_pipe.close();
	_->stdout_pipe.close();
	_->stderr_pipe.close();
	if ( _->has_started && !_->has_exited && _->child > 0 )
	{
		::kill(_->child, SIGKILL);
		int status = 0;
		while ( ::waitpid(_->child, &status, 0) < 0 && errno == EINTR ) {}
	}
}

bool Process::start(error *err)
{
	if ( _->has_started )
	{
		set_process_error(err, "process has already been started");
		return false;
	}
	if ( !_->source.is_process() || _->source.path().empty() )
	{
		set_process_error(err, "process source must be a non-empty exec:// path");
		return false;
	}
	for ( std::map<std::string, std::string>::const_iterator it =
		  _->options.environment.begin(); it != _->options.environment.end(); ++it )
	{
		if ( it->first.empty() || it->first.find('=') != std::string::npos )
		{
			set_process_error(err, "process environment contains an invalid name");
			return false;
		}
	}

	std::vector<std::string> argv_storage;
	argv_storage.push_back(_->source.path());
	argv_storage.insert(argv_storage.end(), _->options.args.begin(), _->options.args.end());
	std::vector<char *> argv;
	for ( std::size_t i = 0; i < argv_storage.size(); ++i )
		argv.push_back(const_cast<char *>(argv_storage[i].c_str()));
	argv.push_back(nullptr);

	std::map<std::string, std::string> environment;
	for ( char **item = environ; item && *item; ++item )
	{
		std::string entry(*item);
		std::size_t equals = entry.find('=');
		if ( equals != std::string::npos )
			environment[entry.substr(0, equals)] = entry.substr(equals + 1);
	}
	for ( std::map<std::string, std::string>::const_iterator it =
		  _->options.environment.begin(); it != _->options.environment.end(); ++it )
		environment[it->first] = it->second;
	std::vector<std::string> environment_storage;
	for ( std::map<std::string, std::string>::const_iterator it =
		  environment.begin(); it != environment.end(); ++it )
		environment_storage.push_back(it->first + "=" + it->second);
	std::vector<char *> environment_vector;
	for ( std::size_t i = 0; i < environment_storage.size(); ++i )
		environment_vector.push_back(const_cast<char *>(environment_storage[i].c_str()));
	environment_vector.push_back(nullptr);

	int input_fds[2] = { -1, -1 };
	int output_fds[2] = { -1, -1 };
	int error_fds[2] = { -1, -1 };
	int exec_fds[2] = { -1, -1 };
	if ( !make_cloexec_pipe(input_fds, err)
	  || !make_cloexec_pipe(output_fds, err)
	  || !make_cloexec_pipe(error_fds, err)
	  || !make_cloexec_pipe(exec_fds, err) )
	{
		close_pipe(input_fds);
		close_pipe(output_fds);
		close_pipe(error_fds);
		close_pipe(exec_fds);
		return false;
	}

	pid_t child = ::fork();
	if ( child < 0 )
	{
		int number = errno;
		close_pipe(input_fds);
		close_pipe(output_fds);
		close_pipe(error_fds);
		close_pipe(exec_fds);
		set_process_errno(err, "process fork failed", number);
		return false;
	}
	if ( child == 0 )
	{
		close_fd(exec_fds[0]);
		if ( ::dup2(input_fds[0], STDIN_FILENO) < 0
		  || ::dup2(output_fds[1], STDOUT_FILENO) < 0
		  || ::dup2(error_fds[1], STDERR_FILENO) < 0 )
		{
			int number = errno;
			report_child_error(exec_fds[1], number);
			::_exit(127);
		}
		close_pipe(input_fds);
		close_pipe(output_fds);
		close_pipe(error_fds);
		if ( !_->options.working_directory.empty()
		  && ::chdir(_->options.working_directory.c_str()) != 0 )
		{
			int number = errno;
			report_child_error(exec_fds[1], number);
			::_exit(127);
		}
		::execve(_->source.path().c_str(), &argv[0], &environment_vector[0]);
		int number = errno;
		report_child_error(exec_fds[1], number);
		::_exit(127);
	}

	close_fd(input_fds[0]);
	close_fd(output_fds[1]);
	close_fd(error_fds[1]);
	close_fd(exec_fds[1]);
	int exec_error = 0;
	bool exec_failed = read_exec_error(exec_fds[0], exec_error);
	close_fd(exec_fds[0]);
	if ( exec_failed )
	{
		close_fd(input_fds[1]);
		close_fd(output_fds[0]);
		close_fd(error_fds[0]);
		int status = 0;
		while ( ::waitpid(child, &status, 0) < 0 && errno == EINTR ) {}
		set_process_errno(err, "process exec failed for " + _->source.path(), exec_error);
		return false;
	}

	_->child = child;
	_->stdin_pipe.assign(input_fds[1]);
	_->stdout_pipe.assign(output_fds[0]);
	_->stderr_pipe.assign(error_fds[0]);
	_->has_started = true;
	return true;
}

DataChannel &Process::stdin_channel() { return _->stdin_pipe; }
DataChannel &Process::stdout_channel() { return _->stdout_pipe; }
DataChannel &Process::stderr_channel() { return _->stderr_pipe; }

bool Process::close_stdin(error *err)
{
	if ( !_->stdin_pipe.flush(err) )
		return false;
	_->stdin_pipe.close_write();
	return true;
}

bool Process::wait(error *err)
{
	if ( !_->has_started )
	{
		set_process_error(err, "cannot wait for a process that was not started");
		return false;
	}
	if ( _->has_exited )
		return true;
	int child_status = 0;
	pid_t result;
	do
		result = ::waitpid(_->child, &child_status, 0);
	while ( result < 0 && errno == EINTR );
	if ( result < 0 )
	{
		set_process_errno(err, "process wait failed", errno);
		return false;
	}
	_->has_exited = true;
	if ( WIFEXITED(child_status) )
		_->status = WEXITSTATUS(child_status);
	else if ( WIFSIGNALED(child_status) )
		_->status = 128 + WTERMSIG(child_status);
	else
		_->status = -1;
	return true;
}

bool Process::started() const { return _->has_started; }
bool Process::exited() const { return _->has_exited; }
int Process::exit_status() const { return _->status; }

void Process::terminate()
{
	if ( _->has_started && !_->has_exited && _->child > 0 )
		::kill(_->child, SIGTERM);
}

bool pump_process(DataChannel &input,
		  Process &process,
		  DataChannel &output,
		  DataChannel *stderr_output,
		  ProcessPumpResult &result,
		  error *err)
{
	result = ProcessPumpResult();
	if ( !process.started() )
	{
		set_process_error(err, "process pump requires a started process");
		return false;
	}

	bool input_ok = false;
	bool output_ok = false;
	bool stderr_ok = false;
	error input_error;
	error output_error;
	error stderr_error;

	std::thread input_thread([&]() {
		input_ok = copy_counted(input, &process.stdin_channel(),
					result.input_bytes, &input_error);
		if ( input_ok )
			input_ok = process.close_stdin(&input_error);
		if ( !input_ok )
			process.terminate();
	});
	std::thread output_thread([&]() {
		output_ok = copy_counted(process.stdout_channel(), &output,
					 result.output_bytes, &output_error);
		if ( !output_ok )
		{
			input.close_read();
			process.terminate();
		}
	});
	std::thread stderr_thread([&]() {
		stderr_ok = copy_counted(process.stderr_channel(), stderr_output,
					 result.stderr_bytes, &stderr_error);
		if ( !stderr_ok )
		{
			input.close_read();
			process.terminate();
		}
	});

	input_thread.join();
	output_thread.join();
	stderr_thread.join();
	if ( !process.wait(err) )
		return false;
	result.exit_status = process.exit_status();

	if ( !input_ok )
	{
		if ( err ) *err = input_error;
		return false;
	}
	if ( !output_ok )
	{
		if ( err ) *err = output_error;
		return false;
	}
	if ( !stderr_ok )
	{
		if ( err ) *err = stderr_error;
		return false;
	}
	if ( result.exit_status != 0 )
	{
		set_process_error(err, "process exited with status "
				       + std::to_string(result.exit_status));
		return false;
	}
	return true;
}

} // namespace madc
