// The low-level IO owner — one definition per function, per-platform BODIES
// (the definition-counting gates key on single signatures:
// check-datachannel-write-owner.sh). Win32 bridges CRT fds to HANDLEs via
// _get_osfhandle where the Win32 API is the only expression.
#include "madc_posix_io.h"

#include <cerrno>
#include <cstdlib>
#ifdef _WIN32
#include <cstdio>
#include <cstring>
#include <fcntl.h>	// _O_CREAT/_O_EXCL — make_temp_file's atomic claim
#include <io.h>
#include <limits.h>
#include <share.h>	// _SH_DENYNO for _sopen_s
#include <sys/stat.h>	// _fstat64 — the 64-bit stat the 32-bit CRT default hides
#include <windows.h>
// Version 2 = the K32* kernel32 inlines — no psapi.dll import needed.
#define PSAPI_VERSION 2
#include <psapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <fstream>	// process_resident_bytes: /proc/self/statm
#include <glob.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>	// process_cpu_microseconds: getrusage
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>	// process_resident_bytes: task_info resident size
#endif
#endif

namespace madc {
namespace detail {

bool set_fd_close_on_exec(int fd)
{
#ifdef _WIN32
	// Close-on-exec == not inherited by CreateProcess children.
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return false;
	return SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0) != 0;
#else
	int flags = ::fcntl(fd, F_GETFD);
	return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
}

ssize_t write_fd_without_sigpipe(int fd, const void *buffer, std::size_t size)
{
#ifdef _WIN32
	// Windows has no SIGPIPE: a plain write already has the "report
	// EPIPE, don't kill the process" contract the POSIX body builds.
	unsigned int chunk = size > (std::size_t)INT_MAX ? (unsigned int)INT_MAX
							 : (unsigned int)size;
	return ::_write(fd, buffer, chunk);
#else
	sigset_t blocked;
	sigset_t previous;
	sigset_t pending;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	int mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if ( mask_error != 0 )
	{
		errno = mask_error;
		return -1;
	}
	if ( sigpending(&pending) != 0 )
	{
		int number = errno;
		pthread_sigmask(SIG_SETMASK, &previous, nullptr);
		errno = number;
		return -1;
	}
	bool already_pending = sigismember(&pending, SIGPIPE) == 1;

	ssize_t result;
	do
		result = ::write(fd, buffer, size); // SIGPIPE-OWNER
	while ( result < 0 && errno == EINTR );
	int number = errno;

	if ( result < 0 && number == EPIPE && !already_pending )
	{
		int signal_number = 0;
		sigwait(&blocked, &signal_number);
	}
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	errno = number;
	return result;
#endif
}

ssize_t read_fd(int fd, void *buffer, std::size_t size)
{
#ifdef _WIN32
	unsigned int chunk = size > (std::size_t)INT_MAX ? (unsigned int)INT_MAX
							 : (unsigned int)size;
	return ::_read(fd, buffer, chunk);
#else
	ssize_t result;
	do
		result = ::read(fd, buffer, size);
	while ( result < 0 && errno == EINTR );
	return result;
#endif
}

#ifdef _WIN32
std::string win_error_text(unsigned long code)
{
	char msg[256];
	msg[0] = '\0';
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		       NULL, (DWORD)code, 0, msg, sizeof(msg), NULL);
	// FormatMessage terminates with "\r\n" — trim it.
	std::size_t n = std::strlen(msg);
	while ( n && (msg[n - 1] == '\n' || msg[n - 1] == '\r') )
		msg[--n] = '\0';
	if ( !n )
		return "Windows error " + std::to_string(code);
	return std::string(msg, n);
}

void debug_log_line(const std::string &line)
{
	OutputDebugStringA(line.c_str());
}
#endif

std::string resolve_real_path(const char *path)
{
#ifdef _WIN32
	// GetFullPathName normalizes (absolute, `.`/`..` collapsed) without
	// touching the filesystem — no existence check and no link
	// resolution, both permitted by the header contract (consistent
	// spellings, not link identity).
	char buf[MAX_PATH];
	DWORD n = GetFullPathNameA(path, sizeof(buf), buf, NULL);
	if ( n == 0 || n >= sizeof(buf) )
		return std::string();
	return std::string(buf, n);
#else
	char *rp = ::realpath(path, NULL);
	if ( !rp )
		return std::string();
	std::string out(rp);
	::free(rp);
	return out;
#endif
}

std::string get_host_name()
{
#ifdef _WIN32
	// GetComputerNameEx, deliberately not winsock gethostname — callers
	// run at static-init time, before any WSAStartup (header contract).
	char buf[256];
	DWORD n = sizeof(buf);
	if ( !GetComputerNameExA(ComputerNameDnsHostname, buf, &n) )
		return std::string();
	return std::string(buf, n);
#else
	char buf[256];
	if ( ::gethostname(buf, sizeof(buf) - 1) != 0 )
		return std::string();
	buf[sizeof(buf) - 1] = '\0';
	return std::string(buf);
#endif
}

bool local_time(time_t t, struct tm &out)
{
#ifdef _WIN32
	return ::localtime_s(&out, &t) == 0;
#else
	return ::localtime_r(&t, &out) != NULL;
#endif
}

long long seek_fd(int fd, long long offset)
{
#ifdef _WIN32
	return ::_lseeki64(fd, offset, SEEK_SET);
#else
	return (long long)::lseek(fd, (off_t)offset, SEEK_SET);
#endif
}

long long fd_size(int fd)
{
#ifdef _WIN32
	struct _stat64 st;
	if ( ::_fstat64(fd, &st) != 0 )
		return -1;
	return (long long)st.st_size;
#else
	struct stat st;
	if ( ::fstat(fd, &st) != 0 )
		return -1;
	return (long long)st.st_size;
#endif
}

bool fd_is_regular_file(int fd)
{
#ifdef _WIN32
	struct _stat64 st;
	if ( ::_fstat64(fd, &st) != 0 )
		return false;
	return (st.st_mode & _S_IFMT) == _S_IFREG;
#else
	struct stat st;
	if ( ::fstat(fd, &st) != 0 )
		return false;
	return S_ISREG(st.st_mode);
#endif
}

ssize_t pread_fd(int fd, void *buffer, std::size_t size, long long offset)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return -1;
	OVERLAPPED ov;
	std::memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffff);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	DWORD got = 0;
	if ( !ReadFile(h, buffer, (DWORD)size, &got, &ov) )
	{
		if ( GetLastError() == ERROR_HANDLE_EOF )
			return 0;
		errno = EIO;
		return -1;
	}
	return (ssize_t)got;
#else
	ssize_t result;
	do
		result = ::pread(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
#endif
}

ssize_t pwrite_fd(int fd, const void *buffer, std::size_t size,
		  long long offset)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ( h == INVALID_HANDLE_VALUE )
		return -1;
	OVERLAPPED ov;
	std::memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffff);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	DWORD put = 0;
	if ( !WriteFile(h, buffer, (DWORD)size, &put, &ov) )
	{
		errno = EIO;
		return -1;
	}
	return (ssize_t)put;
#else
	ssize_t result;
	do
		result = ::pwrite(fd, buffer, size, (off_t)offset);
	while ( result < 0 && errno == EINTR );
	return result;
#endif
}

bool open_string_capture(StringCapture &cap)
{
#ifdef _WIN32
	// No memory-backed FILE* exists on Windows; capture through a
	// delete-on-close temp file. NOT tmpfile(): the MS CRT's tmpfile
	// creates in the drive root and fails for non-elevated users. The
	// "D" fopen flag deletes on close even on abnormal exit.
	cap.buf = NULL;
	cap.len = 0;
	cap.f = NULL;
	char *name = ::_tempnam(NULL, "madc");
	if ( !name )
		return false;
	cap.f = ::fopen(name, "w+bTD");
	::free(name);
	return cap.f != NULL;
#else
	cap.buf = NULL;
	cap.len = 0;
	cap.f = ::open_memstream(&cap.buf, &cap.len);
	return cap.f != NULL;
#endif
}

std::string finish_string_capture(StringCapture &cap)
{
#ifdef _WIN32
	if ( !cap.f )
		return std::string();
	std::string s;
	long end = ( ::fflush(cap.f) == 0 && ::fseek(cap.f, 0, SEEK_END) == 0 )
		 ? ::ftell(cap.f) : -1;
	if ( end > 0 && ::fseek(cap.f, 0, SEEK_SET) == 0 )
	{
		s.resize((std::size_t)end);
		std::size_t got = ::fread(&s[0], 1, (std::size_t)end, cap.f);
		s.resize(got);
	}
	::fclose(cap.f);	// "D" flag: the backing file deletes here
	cap.f = NULL;
	return s;
#else
	if ( !cap.f )
		return std::string();
	::fclose(cap.f);
	std::string s = cap.buf ? std::string(cap.buf, cap.len) : std::string();
	::free(cap.buf);
	cap.f = NULL;
	cap.buf = NULL;
	cap.len = 0;
	return s;
#endif
}

#ifdef _WIN32
namespace {

// Join a walk prefix and a component without doubling separators.
std::string glob_join(const std::string &prefix, const std::string &name)
{
	if ( prefix.empty() )
		return name;
	char last = prefix[prefix.size() - 1];
	if ( last == '/' || last == '\\' )
		return prefix + name;
	return prefix + "/" + name;
}

// Component-wise FindFirstFile expansion: each path component containing
// a wildcard is enumerated at its level (Windows-native rules: * and ?);
// literal components pass through, with only the LEAF existence-checked
// (the glob contract: a literal pattern matches iff the path exists).
void glob_walk(const std::string &prefix, const std::string &rest,
	       std::vector<std::string> &out)
{
	std::size_t sep = rest.find_first_of("/\\");
	std::string head = sep == std::string::npos ? rest : rest.substr(0, sep);
	std::string tail = sep == std::string::npos ? std::string()
						    : rest.substr(sep + 1);
	if ( head.empty() )
	{
		// Leading or doubled separator: descend with the separator kept.
		if ( !tail.empty() )
			glob_walk(prefix.empty() ? "/" : prefix, tail, out);
		return;
	}
	if ( head.find_first_of("*?") == std::string::npos )
	{
		std::string next = glob_join(prefix, head);
		if ( tail.empty() )
		{
			if ( GetFileAttributesA(next.c_str()) != INVALID_FILE_ATTRIBUTES )
				out.push_back(next);
		}
		else
			glob_walk(next, tail, out);
		return;
	}
	WIN32_FIND_DATAA data;
	HANDLE h = FindFirstFileA(glob_join(prefix, head).c_str(), &data);
	if ( h == INVALID_HANDLE_VALUE )
		return;
	do
	{
		if ( !std::strcmp(data.cFileName, ".")
		  || !std::strcmp(data.cFileName, "..") )
			continue;
		std::string next = glob_join(prefix, data.cFileName);
		if ( tail.empty() )
			out.push_back(next);
		else if ( (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 )
			glob_walk(next, tail, out);
	} while ( FindNextFileA(h, &data) );
	FindClose(h);
}

} // namespace
#endif

void glob_paths(const std::string &pattern, std::vector<std::string> &out)
{
#ifdef _WIN32
	glob_walk(std::string(), pattern, out);
#else
	glob_t globbuf;
	if ( ::glob(pattern.c_str(), GLOB_NOSORT, NULL, &globbuf) != 0 )
		return;
	for ( size_t i = 0; i < globbuf.gl_pathc; ++i )
		out.push_back(std::string(globbuf.gl_pathv[i]));
	globfree(&globbuf);
#endif
}

void *map_file_readonly(const char *path, std::size_t &length)
{
#ifdef _WIN32
	length = 0;
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
			       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if ( f == INVALID_HANDLE_VALUE )
		return NULL;
	LARGE_INTEGER sz;
	if ( !GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 )
	{
		CloseHandle(f);
		return NULL;
	}
	HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
	CloseHandle(f);	// the mapping holds its own reference
	if ( !m )
		return NULL;
	void *view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(m);	// the view holds its own reference
	if ( !view )
		return NULL;
	length = (std::size_t)sz.QuadPart;
	return view;
#else
	length = 0;
	int fd = ::open(path, O_RDONLY);
	if ( fd < 0 )
		return NULL;
	struct stat st;
	if ( ::fstat(fd, &st) != 0 || st.st_size <= 0 )
	{
		::close(fd);
		return NULL;
	}
	void *m = ::mmap(NULL, (std::size_t)st.st_size, PROT_READ, MAP_PRIVATE,
			 fd, 0);
	::close(fd);
	if ( m == MAP_FAILED )
		return NULL;
	length = (std::size_t)st.st_size;
	return m;
#endif
}

int make_temp_file(const char *prefix, std::string &path_out)
{
	path_out.clear();
#ifdef _WIN32
	// _tempnam names a fresh candidate in %TMP%; _O_CREAT|_O_EXCL makes
	// the claim atomic — retry when a concurrent claimer wins the name.
	// NOT delete-on-close (_O_TEMPORARY): callers close the fd and
	// re-read the file by path.
	for ( int attempt = 0; attempt < 16; ++attempt )
	{
		char *name = ::_tempnam(NULL, prefix);
		if ( !name )
			break;
		int fd = -1;
		errno_t err = ::_sopen_s(&fd, name,
					 _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
					 _SH_DENYNO, _S_IREAD | _S_IWRITE);
		if ( err == 0 && fd >= 0 )
		{
			path_out.assign(name);
			::free(name);
			return fd;
		}
		::free(name);
		if ( err != EEXIST )
			break;
	}
	return -1;
#else
	std::string templ = std::string("/tmp/") + prefix + "_XXXXXX";
	std::vector<char> writable(templ.begin(), templ.end());
	writable.push_back('\0');
	int fd = ::mkstemp(&writable[0]);
	if ( fd < 0 )
		return -1;
	path_out.assign(&writable[0]);
	return fd;
#endif
}

unsigned long long process_cpu_microseconds()
{
#ifdef _WIN32
	FILETIME creation, exited, kernel, user;
	if ( !GetProcessTimes(GetCurrentProcess(), &creation, &exited,
			      &kernel, &user) )
		return 0;
	ULARGE_INTEGER k, u;
	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;
	// FILETIME ticks are 100ns.
	return (unsigned long long)((k.QuadPart + u.QuadPart) / 10);
#else
	struct rusage usage;
	if ( ::getrusage(RUSAGE_SELF, &usage) != 0 )
		return 0;
	unsigned long long user_us =
		(unsigned long long)usage.ru_utime.tv_sec * 1000000ULL
		+ (unsigned long long)usage.ru_utime.tv_usec;
	unsigned long long sys_us =
		(unsigned long long)usage.ru_stime.tv_sec * 1000000ULL
		+ (unsigned long long)usage.ru_stime.tv_usec;
	return user_us + sys_us;
#endif
}

unsigned long long process_resident_bytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS pmc;
	if ( !GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) )
		return 0;
	return (unsigned long long)pmc.WorkingSetSize;
#elif defined(__APPLE__)
	mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if ( task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
		       reinterpret_cast<task_info_t>(&info), &count)
	     != KERN_SUCCESS )
		return 0;
	return info.resident_size;
#else
	std::ifstream statm("/proc/self/statm");
	unsigned long long pages_total = 0;
	unsigned long long pages_resident = 0;
	statm >> pages_total >> pages_resident;
	if ( !statm )
		return 0;
	long page_size = ::sysconf(_SC_PAGESIZE);
	if ( page_size <= 0 )
		return 0;
	return pages_resident * (unsigned long long)page_size;
#endif
}

} // namespace detail
} // namespace madc
