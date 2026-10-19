/* madc_history.cpp — the REPL history store (split arc 1B.7; design
 * docs/plans/2026-10-10-history-std-reset-design.md, decision 1). The
 * records' format is madcdis/line_edit.h's; this file reads and writes it.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <cstdlib>
#include <ctime>
#include <stdint.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_history.h"
#include "madc_posix_io.h"
#include "madcdis/line_edit.h"

std::string madc::history_default_path()
{
    const char *over = getenv("MADC_HISTORY_FILE");
    if ( over && *over )
	return std::string(over);
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if ( local && *local )
	return std::string(local) + "\\madc\\history";
    return std::string();
#else
    const char *state = getenv("XDG_STATE_HOME");
    if ( state && *state )
	return std::string(state) + "/madc/history";
    const char *home = getenv("HOME");
    if ( home && *home )
	return std::string(home) + "/.local/state/madc/history";
    return std::string();
#endif
}

// The language a standard names: a reader sees its session's own (C, C++
// or madc). The Program's is_c_mode / is_cpp_mode ask the same of the
// standard in force.
enum class entry_language : unsigned char { none, c, cpp, madc };
static entry_language language_named(const std::string &mode)
{
    Program::LanguageStd s;
    if ( !Program::standard_of_canonical_name(mode.c_str(), s) )
	return entry_language::none;
    if ( s == Program::STD_MADC )
	return entry_language::madc;
    if ( s >= Program::STD_CPP98 && s <= Program::STD_CPP26 )
	return entry_language::cpp;
    return entry_language::c;
}

std::vector<std::string> madc::history_entries(const std::string &path,
					       const std::string &mode)
{
    std::vector<std::string> out;
    const entry_language want = language_named(mode);
    if ( path.empty() || want == entry_language::none )
	return out;
    std::ifstream f(path.c_str(), std::ios::binary);
    if ( !f )
	return out;
    std::ostringstream text;
    text << f.rdbuf();
    std::vector<std::pair<std::string, std::string> > records =
	madc::hub::history_records(text.str());
    for ( size_t i = 0; i < records.size(); ++i )
	if ( language_named(records[i].first) == want )
	    out.push_back(records[i].second);
    return out;
}

static std::string utc_now()
{
    struct tm parts = {};
    madc::detail::utc_time(time(NULL), parts);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%SZ", &parts);
    return buf;
}

// One open for appending, one write: two sessions' records interleave whole.
static bool append_once(const std::string &path, const std::string &record)
{
#ifdef _WIN32
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if ( !f )
	return false;
    f << record;
    return (bool)f;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if ( fd < 0 )
	return false;
    ssize_t n = madc::detail::write_fd_without_sigpipe(fd, record.data(), record.size());
    ::close(fd);
    return n == (ssize_t)record.size();
#endif
}

bool madc::history_append(const std::string &path, const std::string &mode,
			  const std::string &entry)
{
    if ( path.empty() )
	return false;
    const std::string record = madc::hub::history_record(utc_now(), mode, entry);
    if ( append_once(path, record) )
	return true;
    madc::detail::host_path_make_parents(path);
    return append_once(path, record);
}

madc::value &madc::history_path(value &out)
{
    out = value(history_default_path());
    return out;
}

madc::value &madc::history_entries(value &out, const char *standard)
{
    std::vector<value> rows;
    for ( const std::string &e : history_entries(history_default_path(),
						   standard ? standard : "") )
	rows.push_back(value(e));
    out = value::make_array(rows);
    return out;
}

bool madc::history_append(const char *standard, const char *entry)
{
    return history_append(history_default_path(), standard ? standard : "",
			  entry ? entry : "");
}
