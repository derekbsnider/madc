///////////////////////////////////////////////////////////////////////////
//                                                                       //
// madc.ini reader (forest-carriers S6). See include/madc_config.h for    //
// the contract and why this is a CLI-shape feature rather than a         //
// libmadc default.                                                      //
//                                                                       //
// Deliberately hand-written and small: the grammar is `key = value`, and //
// this code reads a file an attacker may be able to influence in a       //
// sandboxed deployment, so it is sized to be auditable line by line. A   //
// general TOML/YAML dependency would also raise the self-hosting bar     //
// (madc must eventually compile every file under src/) for five flat     //
// scalar keys.                                                          //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "madc_config.h"

namespace madc {

namespace {

// The ACCEPTED KEYS — the single owner of the config surface. A new setting is
// a row here plus a field in config_settings; nothing else in madc learns key
// spellings, and the "accepted keys" list in the error message is generated
// from this table so it can never drift from what the parser takes.
// The string is converted to the enum once, at the boundary, and every
// decision downstream is made on the enum (enum-over-strings rule).
enum config_key_id { ckStd, ckForest, ckInclude, ckCpuLimit, ckMemLimit, ckUnknown };

struct config_key_row { const char *name; config_key_id id; };

const config_key_row config_key_table[] = {
	{ "std",	ckStd },
	{ "forest",	ckForest },
	{ "include",	ckInclude },
	{ "cpu-limit",	ckCpuLimit },
	{ "mem-limit",	ckMemLimit },
};

config_key_id config_key_lookup(const std::string &name)
{
	for (const config_key_row &row : config_key_table)
		if (name == row.name)
			return row.id;
	return ckUnknown;
}

std::string config_accepted_keys()
{
	std::string list;
	for (const config_key_row &row : config_key_table) {
		if (!list.empty())
			list += ", ";
		list += row.name;
	}
	return list;
}

std::string trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// An ini value may be quoted, which is what users expect of the format; the
// quotes are syntax, not content. Only a matching pair is stripped, so a path
// that genuinely contains a quote survives.
std::string unquote(const std::string &s)
{
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		return s.substr(1, s.size() - 2);
	return s;
}

// Resolve a path written in a config file: `~/x` against $HOME, a RELATIVE
// path against the config file's own directory, an absolute path untouched.
// Relative-to-the-file is the only defensible rule — a system-wide
// /etc/madc.ini naming `forest = groves.msnap` means the file beside it, not
// something in whatever directory madc happened to be run from.
std::string resolve_config_path(const std::string &value, const std::string &ini_path)
{
	if (value.empty())
		return value;
	if (value[0] == '/')
		return value;
	if (value[0] == '~' && (value.size() == 1 || value[1] == '/')) {
		const char *home = getenv("HOME");
		if (home && *home)
			return std::string(home) + value.substr(1);
		return value;
	}
	size_t slash = ini_path.rfind('/');
	if (slash == std::string::npos)
		return value;
	return ini_path.substr(0, slash + 1) + value;
}

// Whole-string unsigned decimal. Trailing junk is an error, not a silent
// truncation: `mem-limit = 8G` must say so, not arm an 8 MB guard.
bool parse_ulong(const std::string &v, unsigned long &out)
{
	if (v.empty())
		return false;
	for (char c : v)
		if (c < '0' || c > '9')
			return false;
	errno = 0;
	char *end = NULL;
	unsigned long n = strtoul(v.c_str(), &end, 10);
	if (errno != 0 || !end || *end != '\0')
		return false;
	out = n;
	return true;
}

#ifdef MADC_ENABLE_CONFIG_FILE
// Only the lookup chain needs this; with the configure axis off nothing in this
// TU probes the filesystem (and an unused static would warn).
bool path_exists(const std::string &p)
{
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}
#endif

// Every diagnostic names the file, the line, and what to do about it — a
// config file that half-applies is worse than one that refuses.
std::ostream &config_error(std::ostream &err, const std::string &path, int line)
{
	err << "madc: " << path << ":" << line << ": ";
	return err;
}

}   // namespace

std::vector<std::string> config_search_paths()
{
	std::vector<std::string> paths;
	// 1. The current directory — a per-project config, the common case.
	paths.push_back("madc.ini");
	// 2. The user config directory (XDG; honoured on macOS too when set).
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		paths.push_back(std::string(xdg) + "/madc/madc.ini");
	else {
		const char *home = getenv("HOME");
		if (home && *home)
			paths.push_back(std::string(home) + "/.config/madc/madc.ini");
	}
	// 3. The system config directory, baked at build time from $(sysconfdir).
#ifdef MADC_SYSCONFDIR
	paths.push_back(std::string(MADC_SYSCONFDIR) + "/madc.ini");
#endif
	return paths;
}

bool config_parse_file(const std::string &path, config_settings &out,
		       std::ostream &err)
{
	std::ifstream in(path.c_str());
	if (!in.is_open()) {
		err << "madc: cannot read config file '" << path << "'" << std::endl;
		return false;
	}
	out.source_path = path;
	std::string raw;
	int lineno = 0;
	while (std::getline(in, raw)) {
		++lineno;
		std::string line = trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		// An optional [madc] section header, so the file can grow
		// sections later without breaking today's flat files. Any other
		// section is a typo or a file meant for a different program.
		if (line[0] == '[') {
			if (line.back() != ']') {
				config_error(err, path, lineno)
					<< "unterminated section header: " << line
					<< std::endl;
				return false;
			}
			std::string section = trim(line.substr(1, line.size() - 2));
			if (section != "madc") {
				config_error(err, path, lineno)
					<< "unknown section [" << section
					<< "] (only [madc] is accepted)" << std::endl;
				return false;
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			config_error(err, path, lineno)
				<< "expected 'key = value': " << line << std::endl;
			return false;
		}
		std::string key = trim(line.substr(0, eq));
		std::string value = unquote(trim(line.substr(eq + 1)));
		// Keys are case-insensitive (ini convention); values are not.
		for (char &c : key)
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
		if (key.empty()) {
			config_error(err, path, lineno)
				<< "missing key before '='" << std::endl;
			return false;
		}
		if (value.empty()) {
			config_error(err, path, lineno)
				<< "key '" << key << "' has an empty value"
				<< std::endl;
			return false;
		}
		config_key_id id = config_key_lookup(key);
		unsigned long n = 0;
		switch (id) {
		case ckStd:
			// Accepted spellings are the --std= vocabulary itself; the
			// caller validates through Program::set_language_standard so
			// there is ONE owner of the dialect list.
			out.std_option = value;
			out.has_std = true;
			break;
		case ckForest:
			out.forest = resolve_config_path(value, path);
			break;
		case ckInclude:
			// Repeatable and ORDERED: the caller appends these after the
			// command line's -I dirs (gcc puts configured dirs last).
			out.include_dirs.push_back(resolve_config_path(value, path));
			break;
		case ckCpuLimit:
			if (!parse_ulong(value, n)) {
				config_error(err, path, lineno)
					<< "cpu-limit needs a whole number of seconds"
					   " (0 disables), got '" << value << "'"
					<< std::endl;
				return false;
			}
			out.cpu_limit_secs = n;
			out.has_cpu_limit = true;
			break;
		case ckMemLimit:
			if (!parse_ulong(value, n)) {
				config_error(err, path, lineno)
					<< "mem-limit needs a whole number of megabytes"
					   " (0 disables), got '" << value << "'"
					<< std::endl;
				return false;
			}
			out.mem_limit_mb = n;
			out.has_mem_limit = true;
			break;
		case ckUnknown:
			config_error(err, path, lineno)
				<< "unknown key '" << key << "' (accepted: "
				<< config_accepted_keys() << ")" << std::endl;
			return false;
		}
	}
	return true;
}

bool config_file_supported()
{
#ifdef MADC_ENABLE_CONFIG_FILE
	return true;
#else
	return false;
#endif
}

bool config_load(const char *explicit_path, config_settings &out,
		 std::ostream &err)
{
#ifndef MADC_ENABLE_CONFIG_FILE
	// The configure axis is off: there is no path from this build to a config
	// file at all. An explicitly named one still refuses loudly rather than
	// being quietly ignored.
	if (explicit_path && *explicit_path) {
		err << "madc: --config=: this madc was built without config-file"
		       " support (configure --enable-config-file)" << std::endl;
		return false;
	}
	(void)out;
	return true;
#else
	if (explicit_path && *explicit_path)
		return config_parse_file(explicit_path, out, err);
	for (const std::string &p : config_search_paths())
		if (path_exists(p))
			return config_parse_file(p, out, err);
	return true;	// no config file anywhere: the normal case
#endif
}

}   // namespace madc
