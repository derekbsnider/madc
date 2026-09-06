#include "madcdis/process.h"
#include "madc_datachannel_internal.h"
#include "madc_posix_io.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/types.h>
#include <thread>
#ifdef _WIN32
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <time.h>	/* nanosleep: wait_or_kill's grace slices (MT-3b) */
#include <unistd.h>

extern char **environ;
#endif

namespace madc {
namespace {

void set_process_error(error *err, const std::string &message)
{
	detail::set_channel_error(err, message);
}

void set_process_errno(error *err, const std::string &operation, int number)
{
	set_process_error(err, operation + ": " + std::string(std::strerror(number)));
}

void close_fd(int &fd)
{
	if ( fd >= 0 )
#ifdef _WIN32
		::_close(fd);
#else
		::close(fd);
#endif
	fd = -1;
}

char **host_environ()
{
	// mingw's CRT spells the POSIX environ pointer _environ.
#ifdef _WIN32
	return _environ;
#else
	return environ;
#endif
}

#ifndef _WIN32
bool make_cloexec_pipe(int fds[2], error *err)
{
#ifdef __linux__
	// Atomic close-on-exec at creation: no window for a concurrent
	// fork+exec in a threaded embedding host to leak the pipe ends.
	if ( ::pipe2(fds, O_CLOEXEC) != 0 )
	{
		set_process_errno(err, "process pipe creation failed", errno);
		return false;
	}
	return true;
#else
	// Portable fallback (darwin has no pipe2): post-hoc owner.
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
#endif
}
#endif // !_WIN32

class ProcessPipeChannel : public DataChannel, public PollableDataChannel
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
		ssize_t result = detail::read_fd(fd_, buffer, capacity);
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

	intptr_t read_poll_handle() const override
	{
		return (readable_ && fd_ >= 0) ? (intptr_t)fd_ : (intptr_t)-1;
	}

private:
	std::string name_;
	int fd_;
	bool readable_;
	bool writable_;
};

#ifndef _WIN32
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
		ssize_t count = detail::read_fd(fd, next, remaining);
		if ( count <= 0 )
			return false;
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
#endif // !_WIN32

#ifdef _WIN32
void set_process_last_error(error *err, const std::string &operation)
{
	unsigned long code = GetLastError();
	set_process_error(err, operation + ": " + detail::win_error_text(code));
}

void close_handle(HANDLE &h)
{
	if ( h )
		CloseHandle(h);
	h = NULL;
}

// An inheritable duplicate of h (caller closes it), or NULL when h is not a
// real handle (detached stdio). Duplicating instead of flipping the
// original's inherit flag: a redirected std handle is not guaranteed
// inheritable, and mutating it would race concurrent spawns in an embedding
// host.
HANDLE duplicate_inheritable(HANDLE h)
{
	if ( !h || h == INVALID_HANDLE_VALUE )
		return NULL;
	HANDLE dup = NULL;
	if ( !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
			      0, TRUE, DUPLICATE_SAME_ACCESS) )
		return NULL;
	return dup;
}

// Both ends are born non-inheritable; the CHILD end alone is flipped
// inheritable, and spawn_windows_process's attribute list passes exactly
// those ends — the Win32 shape of the POSIX arm's O_CLOEXEC pipes.
bool make_process_pipe(HANDLE &parent_end, HANDLE &child_end, bool child_reads,
		       error *err)
{
	HANDLE read_end = NULL;
	HANDLE write_end = NULL;
	if ( !CreatePipe(&read_end, &write_end, NULL, 0) )
	{
		set_process_last_error(err, "process pipe creation failed");
		return false;
	}
	child_end = child_reads ? read_end : write_end;
	parent_end = child_reads ? write_end : read_end;
	if ( !SetHandleInformation(child_end, HANDLE_FLAG_INHERIT,
				   HANDLE_FLAG_INHERIT) )
	{
		set_process_last_error(err, "process pipe inherit setup failed");
		CloseHandle(read_end);
		CloseHandle(write_end);
		parent_end = NULL;
		child_end = NULL;
		return false;
	}
	return true;
}

// Windows has ONE command-line string; the child's CRT re-splits it under
// the MS quoting rules (backslash-doubling before quotes). This encodes the
// CRT argv contract only — a cmd.exe/batch target would need cmd's ^ rules
// on top, which no madc surface spawns.
void append_windows_argument(std::string &command_line, const std::string &argument)
{
	if ( !command_line.empty() )
		command_line += ' ';
	if ( !argument.empty()
	  && argument.find_first_of(" \t\n\v\"") == std::string::npos )
	{
		command_line += argument;
		return;
	}
	command_line += '"';
	std::size_t backslashes = 0;
	for ( std::size_t i = 0; i < argument.size(); ++i )
	{
		char c = argument[i];
		if ( c == '\\' )
		{
			++backslashes;
			continue;
		}
		// A quote needs every pending backslash doubled plus its own
		// escape; any other character leaves them literal.
		command_line.append(c == '"' ? backslashes * 2 + 1 : backslashes,
				    '\\');
		backslashes = 0;
		command_line += c;
	}
	command_line.append(backslashes * 2, '\\');	// they precede the closing quote
	command_line += '"';
}

bool environment_name_before(const std::pair<const std::string, std::string> *left,
			     const std::pair<const std::string, std::string> *right)
{
	return _stricmp(left->first.c_str(), right->first.c_str()) < 0;
}

// CreateProcess environment block: NAME=value\0...\0\0, sorted
// case-insensitively by name (the loader binary-searches the child's block).
std::string build_environment_block(const std::map<std::string, std::string> &environment)
{
	std::vector<const std::pair<const std::string, std::string> *> entries;
	entries.reserve(environment.size());
	for ( std::map<std::string, std::string>::const_iterator it =
		  environment.begin(); it != environment.end(); ++it )
		entries.push_back(&*it);
	std::sort(entries.begin(), entries.end(), environment_name_before);
	std::string block;
	for ( std::size_t i = 0; i < entries.size(); ++i )
	{
		block += entries[i]->first;
		block += '=';
		block += entries[i]->second;
		block += '\0';
	}
	block += '\0';
	return block;
}

// The one CreateProcess site: explicit std-handle triple + a
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST restricting inheritance to exactly those
// handles — bInheritHandles=TRUE alone would leak every inheritable handle
// in a threaded embedding host. A NULL application searches like the shell
// (app dir, cwd, system dirs, PATH; .exe appended); a NULL environment
// block inherits the parent's.
bool spawn_windows_process(const char *application, std::vector<char> &command_line,
			   char *environment_block, const char *working_directory,
			   HANDLE std_in, HANDLE std_out, HANDLE std_err,
			   HANDLE &process, error *err)
{
	process = NULL;
	HANDLE inherited[3];
	DWORD inherited_count = 0;
	if ( std_in )
		inherited[inherited_count++] = std_in;
	if ( std_out )
		inherited[inherited_count++] = std_out;
	if ( std_err )
		inherited[inherited_count++] = std_err;

	SIZE_T attribute_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
	std::vector<unsigned char> attribute_storage(attribute_size);
	if ( attribute_storage.empty() )
	{
		set_process_error(err, "process attribute list sizing failed");
		return false;
	}
	STARTUPINFOEXA startup;
	std::memset(&startup, 0, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = std_in;
	startup.StartupInfo.hStdOutput = std_out;
	startup.StartupInfo.hStdError = std_err;
	startup.lpAttributeList =
		reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attribute_storage[0]);
	if ( !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
						&attribute_size) )
	{
		set_process_last_error(err, "process attribute list setup failed");
		return false;
	}
	if ( inherited_count
	  && !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
					PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
					inherited_count * sizeof(HANDLE), NULL, NULL) )
	{
		set_process_last_error(err, "process handle-inheritance setup failed");
		DeleteProcThreadAttributeList(startup.lpAttributeList);
		return false;
	}
	PROCESS_INFORMATION info;
	std::memset(&info, 0, sizeof(info));
	BOOL created = CreateProcessA(application, &command_line[0], NULL, NULL,
				      TRUE, EXTENDED_STARTUPINFO_PRESENT,
				      environment_block, working_directory,
				      &startup.StartupInfo, &info);
	if ( !created )
		set_process_last_error(err, "process creation failed");
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	if ( !created )
		return false;
	CloseHandle(info.hThread);
	process = info.hProcess;
	return true;
}
#endif // _WIN32

} // namespace

struct Process::impl
{
	impl(const DataSource &process_source, const ProcessOptions &process_options)
		: source(process_source), options(process_options),
		  stdin_pipe("process stdin", false, true),
		  stdout_pipe("process stdout", true, false),
		  stderr_pipe("process stderr", true, false),
		  has_started(false), has_exited(false), status(-1)
	{
#ifdef _WIN32
		child = NULL;
#else
		child = -1;
#endif
	}

	DataSource source;
	ProcessOptions options;
#ifdef _WIN32
	HANDLE child;	// process handle; the destructor owns closing it
#else
	pid_t child;
#endif
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
#ifdef _WIN32
	if ( _->has_started && !_->has_exited && _->child )
	{
		// The exit code is never observed on this path (nobody waits
		// after the dtor); 1 just marks the stop as abnormal.
		TerminateProcess(_->child, 1);
		WaitForSingleObject(_->child, INFINITE);
	}
	close_handle(_->child);
#else
	if ( _->has_started && !_->has_exited && _->child > 0 )
	{
		::kill(_->child, SIGKILL);
		int status = 0;
		while ( ::waitpid(_->child, &status, 0) < 0 && errno == EINTR ) {}
	}
#endif
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

	std::map<std::string, std::string> environment;
	for ( char **item = host_environ(); item && *item; ++item )
	{
		std::string entry(*item);
		std::size_t equals = entry.find('=');
		if ( equals != std::string::npos )
			environment[entry.substr(0, equals)] = entry.substr(equals + 1);
	}
	for ( std::map<std::string, std::string>::const_iterator it =
		  _->options.environment.begin(); it != _->options.environment.end(); ++it )
		environment[it->first] = it->second;

#ifdef _WIN32
	// One command-line string, re-split by the child's CRT under the MS
	// quoting rules. CreateProcess owns the executable search for the
	// first token (app dir, cwd, system dirs, PATH; .exe appended), so
	// the POSIX arm's PATH walk has no Win32 twin.
	std::string command_line;
	for ( std::size_t i = 0; i < argv_storage.size(); ++i )
		append_windows_argument(command_line, argv_storage[i]);
	std::vector<char> command_buffer(command_line.begin(), command_line.end());
	command_buffer.push_back('\0');	// CreateProcess may edit it in place
	std::string environment_block = build_environment_block(environment);

	HANDLE parent_stdin = NULL, child_stdin = NULL;
	HANDLE parent_stdout = NULL, child_stdout = NULL;
	HANDLE parent_stderr = NULL, child_stderr = NULL;
	bool piped = make_process_pipe(parent_stdin, child_stdin, true, err)
		  && make_process_pipe(parent_stdout, child_stdout, false, err)
		  && (_->options.inherit_stderr
		   || make_process_pipe(parent_stderr, child_stderr, false, err));
	if ( !piped )
	{
		close_handle(parent_stdin);
		close_handle(child_stdin);
		close_handle(parent_stdout);
		close_handle(child_stdout);
		return false;
	}
	if ( _->options.inherit_stderr )
	{
		// The POSIX arm leaves the child on the parent's fd 2; a NULL
		// duplicate (detached stderr) leaves the child slot empty, the
		// same tolerance as a closed fd 2 there.
		child_stderr = duplicate_inheritable(GetStdHandle(STD_ERROR_HANDLE));
	}

	// Parent ends become CRT fds so the pipe channels stay fd-shaped; a
	// converted handle belongs to its fd from here on.
	int stdin_fd = ::_open_osfhandle((intptr_t)parent_stdin, _O_BINARY);
	if ( stdin_fd >= 0 )
		parent_stdin = NULL;
	int stdout_fd = ::_open_osfhandle((intptr_t)parent_stdout,
					  _O_RDONLY | _O_BINARY);
	if ( stdout_fd >= 0 )
		parent_stdout = NULL;
	int stderr_fd = -1;
	if ( parent_stderr )
	{
		stderr_fd = ::_open_osfhandle((intptr_t)parent_stderr,
					      _O_RDONLY | _O_BINARY);
		if ( stderr_fd >= 0 )
			parent_stderr = NULL;
	}
	if ( stdin_fd < 0 || stdout_fd < 0
	  || (!_->options.inherit_stderr && stderr_fd < 0) )
	{
		set_process_error(err, "process pipe fd conversion failed");
		close_fd(stdin_fd);
		close_fd(stdout_fd);
		close_fd(stderr_fd);
		close_handle(parent_stdin);
		close_handle(parent_stdout);
		close_handle(parent_stderr);
		close_handle(child_stdin);
		close_handle(child_stdout);
		close_handle(child_stderr);
		return false;
	}

	HANDLE child = NULL;
	bool spawned = spawn_windows_process(
		NULL, command_buffer,
		const_cast<char *>(environment_block.c_str()),
		_->options.working_directory.empty()
			? NULL : _->options.working_directory.c_str(),
		child_stdin, child_stdout, child_stderr, child, err);
	close_handle(child_stdin);
	close_handle(child_stdout);
	close_handle(child_stderr);
	if ( !spawned )
	{
		close_fd(stdin_fd);
		close_fd(stdout_fd);
		close_fd(stderr_fd);
		return false;
	}

	_->child = child;
	_->stdin_pipe.assign(stdin_fd);
	_->stdout_pipe.assign(stdout_fd);
	_->stderr_pipe.assign(stderr_fd);
	_->has_started = true;
	return true;
#else
	std::vector<char *> argv;
	for ( std::size_t i = 0; i < argv_storage.size(); ++i )
		argv.push_back(const_cast<char *>(argv_storage[i].c_str()));
	argv.push_back(nullptr);

	std::vector<std::string> environment_storage;
	for ( std::map<std::string, std::string>::const_iterator it =
		  environment.begin(); it != environment.end(); ++it )
		environment_storage.push_back(it->first + "=" + it->second);
	std::vector<char *> environment_vector;
	for ( std::size_t i = 0; i < environment_storage.size(); ++i )
		environment_vector.push_back(const_cast<char *>(environment_storage[i].c_str()));
	environment_vector.push_back(nullptr);

	// posix_spawnp shape: a command with no '/' resolves against the
	// SPAWN environment's PATH (options.environment can override it);
	// execve itself never searches. argv[0] keeps the caller's spelling,
	// execvp-style. An unresolved command falls through to execve and
	// reports ENOENT through the exec-errno pipe.
	std::string executable = _->source.path();
	if ( executable.find('/') == std::string::npos )
	{
		std::map<std::string, std::string>::const_iterator path_it =
			environment.find("PATH");
		const std::string search =
			path_it != environment.end() ? path_it->second : "";
		std::size_t begin = 0;
		while ( begin <= search.size() && !search.empty() )
		{
			std::size_t end = search.find(':', begin);
			if ( end == std::string::npos )
				end = search.size();
			std::string dir = search.substr(begin, end - begin);
			if ( dir.empty() )
				dir = ".";
			std::string candidate = dir + "/" + executable;
			if ( ::access(candidate.c_str(), X_OK) == 0 )
			{
				executable = candidate;
				break;
			}
			begin = end + 1;
		}
	}

	int input_fds[2] = { -1, -1 };
	int output_fds[2] = { -1, -1 };
	int error_fds[2] = { -1, -1 };
	int exec_fds[2] = { -1, -1 };
	if ( !make_cloexec_pipe(input_fds, err)
	  || !make_cloexec_pipe(output_fds, err)
	  || (!_->options.inherit_stderr && !make_cloexec_pipe(error_fds, err))
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
		  || (error_fds[1] >= 0 && ::dup2(error_fds[1], STDERR_FILENO) < 0) )
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
		::execve(executable.c_str(), &argv[0], &environment_vector[0]);
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
#endif // !_WIN32
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

#ifndef _WIN32
// Map a reaped waitpid status to the process-facing exit shape — 128+signal
// for a killed child — ONE owner (dupaudit family child_status_exit_mapping;
// gate: check-child-status-map-owner.sh). Shared (madcdis/process.h): the
// fork-Run reap (parse_run) adopts it too. run_and_wait deliberately stays
// apart: its contract turns abnormal termination into -1 + err, not a code.
int map_child_status(int child_status)
{
	if ( WIFEXITED(child_status) )
		return WEXITSTATUS(child_status);
	if ( WIFSIGNALED(child_status) )
		return 128 + WTERMSIG(child_status);
	return -1;
}
#endif

bool Process::wait(error *err)
{
	if ( !_->has_started )
	{
		set_process_error(err, "cannot wait for a process that was not started");
		return false;
	}
	if ( _->has_exited )
		return true;
#ifdef _WIN32
	DWORD code = 0;
	if ( WaitForSingleObject(_->child, INFINITE) != WAIT_OBJECT_0
	  || !GetExitCodeProcess(_->child, &code) )
	{
		set_process_last_error(err, "process wait failed");
		return false;
	}
	_->has_exited = true;
	// No exited/killed split on Windows: a crashed child's NTSTATUS exit
	// code arrives as a (huge) nonzero int every caller already fails on.
	_->status = (int)code;
#else
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
	_->status = map_child_status(child_status);
#endif
	return true;
}

bool Process::wait_or_kill(int grace_ms, error *err)
{
	if ( !_->has_started )
	{
		set_process_error(err, "cannot wait for a process that was not started");
		return false;
	}
	if ( _->has_exited )
		return true;
	if ( grace_ms < 0 )
		grace_ms = 0;
#ifdef _WIN32
	// No SIGKILL on Windows (the CRT declares only the six ANSI signals)
	// and no graceful/forced split — TerminateProcess IS the hard stop, so
	// the escalation reports the same 128+SIGTERM shape terminate() uses.
	if ( WaitForSingleObject(_->child, (DWORD)grace_ms) != WAIT_OBJECT_0 )
		TerminateProcess(_->child, 128 + SIGTERM);
	return wait(err);
#else
	// Poll for a voluntary exit through the grace window (SIGTERM was
	// already sent by terminate() on this path), then hard-kill. 20ms
	// slices: prompt for the common quick exit, cheap for the rest.
	int waited_ms = 0;
	for ( ;; )
	{
		int child_status = 0;
		pid_t result;
		do
			result = ::waitpid(_->child, &child_status, WNOHANG);
		while ( result < 0 && errno == EINTR );
		if ( result < 0 )
		{
			set_process_errno(err, "process wait failed", errno);
			return false;
		}
		if ( result > 0 )
		{
			_->has_exited = true;
			_->status = map_child_status(child_status);
			return true;
		}
		if ( waited_ms >= grace_ms )
			break;
		struct timespec ts = { 0, 20 * 1000 * 1000 };
		::nanosleep(&ts, NULL);
		waited_ms += 20;
	}
	::kill(_->child, SIGKILL);
	return wait(err);
#endif
}

bool Process::started() const { return _->has_started; }
bool Process::exited() const { return _->has_exited; }
int Process::exit_status() const { return _->status; }

void Process::terminate()
{
#ifdef _WIN32
	// Windows has no cross-process SIGTERM; TerminateProcess is the only
	// general-purpose stop. 128+SIGTERM keeps the reported exit status
	// shaped like the POSIX arm's signal mapping.
	if ( _->has_started && !_->has_exited && _->child )
		TerminateProcess(_->child, 128 + SIGTERM);
#else
	if ( _->has_started && !_->has_exited && _->child > 0 )
		::kill(_->child, SIGTERM);
#endif
}

int Process::run_and_wait(const std::string &executable,
			  const std::vector<std::string> &argv,
			  error *err)
{
#ifdef _WIN32
	std::string command_line;
	for ( std::size_t i = 0; i < argv.size(); ++i )
		append_windows_argument(command_line, argv[i]);
	std::vector<char> command_buffer(command_line.begin(), command_line.end());
	command_buffer.push_back('\0');

	// Inherited stdio: the parent's std triple travels as inheritable
	// duplicates so harness redirections (files, pipes) reach the child.
	HANDLE std_in = duplicate_inheritable(GetStdHandle(STD_INPUT_HANDLE));
	HANDLE std_out = duplicate_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
	HANDLE std_err = duplicate_inheritable(GetStdHandle(STD_ERROR_HANDLE));
	HANDLE child = NULL;
	// Explicit application path + the parent's environment — the execv
	// contract (no PATH search, no .exe appending).
	bool spawned = spawn_windows_process(executable.c_str(), command_buffer,
					     NULL, NULL, std_in, std_out, std_err,
					     child, err);
	close_handle(std_in);
	close_handle(std_out);
	close_handle(std_err);
	if ( !spawned )
		return -1;
	DWORD code = 0;
	bool reaped = WaitForSingleObject(child, INFINITE) == WAIT_OBJECT_0
		   && GetExitCodeProcess(child, &code) != 0;
	if ( !reaped )
		set_process_last_error(err, "process wait failed");
	CloseHandle(child);
	return reaped ? (int)code : -1;
#else
	std::vector<char *> cargv;
	cargv.reserve(argv.size() + 1);
	for ( const std::string &a : argv )
		cargv.push_back(const_cast<char *>(a.c_str()));
	cargv.push_back(NULL);

	pid_t child = ::fork();
	if ( child < 0 )
	{
		set_process_errno(err, "process fork failed", errno);
		return -1;
	}
	if ( child == 0 )
	{
		::execv(executable.c_str(), &cargv[0]);
		// No exec-errno pipe on this arm: stdio is the caller's, so
		// the child reports where the user is already looking.
		perror("madc: exec failed");
		::_exit(127);
	}
	int status = 0;
	pid_t result;
	do
		result = ::waitpid(child, &status, 0);
	while ( result < 0 && errno == EINTR );
	if ( result < 0 )
	{
		set_process_errno(err, "process wait failed", errno);
		return -1;
	}
	if ( WIFEXITED(status) )
		return WEXITSTATUS(status);
	set_process_error(err, "process terminated abnormally");
	return -1;
#endif // !_WIN32
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

	// inherit_stderr leaves the process with no stderr pipe; a pump leg on
	// that unreadable channel would fail instantly and terminate the child.
	bool pump_stderr = process.stderr_channel().capabilities().read;
	bool input_ok = false;
	bool output_ok = false;
	bool stderr_ok = !pump_stderr;
	error input_error;
	error output_error;
	error stderr_error;

	std::thread input_thread([&]() {
		input_ok = copy_channel(input, &process.stdin_channel(),
					result.input_bytes, &input_error);
		if ( input_ok )
			input_ok = process.close_stdin(&input_error);
		if ( !input_ok )
			process.terminate();
	});
	std::thread output_thread([&]() {
		output_ok = copy_channel(process.stdout_channel(), &output,
					 result.output_bytes, &output_error);
		if ( !output_ok )
		{
			input.close_read();
			process.terminate();
		}
	});
	std::thread stderr_thread;
	if ( pump_stderr )
		stderr_thread = std::thread([&]() {
			stderr_ok = copy_channel(process.stderr_channel(), stderr_output,
						 result.stderr_bytes, &stderr_error);
			if ( !stderr_ok )
			{
				input.close_read();
				process.terminate();
			}
		});

	input_thread.join();
	output_thread.join();
	if ( stderr_thread.joinable() )
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

namespace {

// exec:// as a registry channel: write -> child stdin, read -> child stdout.
// The child's stderr stays on the parent's (inherit_stderr) so an undrained
// stderr pipe can never block a chatty child. Never seekable.
class ExecDataChannel : public DataChannel, public PollableDataChannel
{
public:
	explicit ExecDataChannel(std::unique_ptr<Process> process)
		: process_(std::move(process))
	{}

	~ExecDataChannel() override { close(); }

	const char *name() const override { return "exec"; }

	ChannelCapabilities capabilities() const override
	{
		ChannelCapabilities capabilities;
		if ( !process_ )
			return capabilities;
		capabilities.read = process_->stdout_channel().capabilities().read;
		capabilities.write = process_->stdin_channel().capabilities().write;
		capabilities.half_close = true;
		return capabilities;
	}

	bool read(void *buffer, std::size_t capacity, std::size_t &bytes_read,
		  error *err = nullptr) override
	{
		bytes_read = 0;
		if ( !process_ )
		{
			set_process_error(err, "exec channel is closed");
			return false;
		}
		return process_->stdout_channel().read(buffer, capacity, bytes_read, err);
	}

	bool write(const void *buffer, std::size_t size, std::size_t &bytes_written,
		   error *err = nullptr) override
	{
		bytes_written = 0;
		if ( !process_ )
		{
			set_process_error(err, "exec channel is closed");
			return false;
		}
		return process_->stdin_channel().write(buffer, size, bytes_written, err);
	}

	void close_read() override
	{
		if ( process_ )
			process_->stdout_channel().close_read();
	}

	void close_write() override
	{
		if ( process_ )
			process_->close_stdin();
	}

	void close() override
	{
		if ( !process_ )
			return;
		// Stdin EOF + a closed stdout let the child run out; wait()
		// reaps it. A CANCELLED channel escalates (MT-3b): the child
		// already got terminate()'s SIGTERM — after a 2s grace for
		// its handlers to flush, SIGKILL reaps it, so a
		// SIGTERM-ignoring child can no longer hang this close (the
		// old shape waited forever; the dtor's SIGKILL never ran).
		process_->close_stdin();
		process_->stdout_channel().close_read();
		if ( cancelled_ )
			process_->wait_or_kill(2000);
		else
			process_->wait();
		process_.reset();
	}

	// The waitable READ side is the child's stdout pipe.
	intptr_t read_poll_handle() const override
	{
		if ( !process_ )
			return (intptr_t)-1;
		PollableDataChannel *pollable =
			pollable_surface(&process_->stdout_channel());
		return pollable ? pollable->read_poll_handle() : (intptr_t)-1;
	}

	// Stop-this-build (IDE-10b): SIGTERM the child so the following
	// close() reaps promptly instead of waiting for it to run out.
	// MT-3b closed the residue: close() on a cancelled channel
	// escalates to SIGKILL after a 2s grace, so a SIGTERM-ignoring
	// child cannot hang it.
	void cancel() override
	{
		cancelled_ = true;
		if ( process_ )
			process_->terminate();
	}

private:
	std::unique_ptr<Process> process_;
	bool cancelled_ = false;
};

class ExecChannelFactory : public DataChannelRegistry::Factory
{
public:
	std::unique_ptr<DataChannel> open(const DataSource &source,
					  ChannelOpenMode mode,
					  error *err = nullptr) const override
	{
		// An exec channel is inherently bidirectional (the pipes are the
		// truth); mode does not narrow it. argv splits on single spaces —
		// this is NOT a shell: no quoting, globbing, variables, or
		// redirection. An argument containing a space needs the
		// ProcessOptions.args API.
		(void)mode;
		const std::string &spec = source.path();
		std::vector<std::string> words;
		std::size_t begin = 0;
		while ( begin <= spec.size() )
		{
			std::size_t end = spec.find(' ', begin);
			if ( end == std::string::npos )
				end = spec.size();
			if ( end > begin )
				words.push_back(spec.substr(begin, end - begin));
			begin = end + 1;
		}
		if ( words.empty() )
		{
			set_process_error(err, "exec channel requires a command");
			return std::unique_ptr<DataChannel>();
		}
		ProcessOptions options;
		options.args.assign(words.begin() + 1, words.end());
		options.inherit_stderr = true;
		std::unique_ptr<Process> process(
			new Process(DataSource("exec://" + words[0]), options));
		if ( !process->start(err) )
			return std::unique_ptr<DataChannel>();
		return std::unique_ptr<DataChannel>(
			new ExecDataChannel(std::move(process)));
	}
};

} // namespace

namespace detail {

void register_exec_channel_factory(DataChannelRegistry &registry)
{
	registry.register_factory(
		"exec", std::unique_ptr<DataChannelRegistry::Factory>(
				new ExecChannelFactory()));
}

} // namespace detail

} // namespace madc
