///////////////////////////////////////////////////////////////////////////
//                                                                       //
// config_file — the schema-blind ini reader. Contract and the reason it   //
// is schema-blind (and private): include/madc_config_file.h.              //
//                                                                       //
// Deliberately hand-written and small: the grammar is `key = value`, and  //
// consumers may read a file an attacker can influence in a sandboxed      //
// deployment, so this is sized to be auditable line by line. A general    //
// TOML/YAML dependency would also raise the self-hosting bar (madc must   //
// eventually compile every file under src/) for a handful of flat scalars. //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <fstream>
#include <ostream>
#include <string>
#include <vector>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "madc_config_file.h"

namespace madc {
namespace cfg {

namespace {

std::string trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string lowercased(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return out;
}

// An ini value may be quoted, which is what users expect of the format; the
// quotes are syntax, not content. Only a matching pair is stripped, so a value
// that genuinely contains a quote survives.
std::string unquote(const std::string &s)
{
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		return s.substr(1, s.size() - 2);
	return s;
}

// Resolve a path written in a config file: `~/x` against $HOME, a RELATIVE path
// against the config file's own directory, an absolute path untouched.
// Relative-to-the-file is the only defensible rule — a system-wide
// /etc/<app>.ini naming `store = data.db` means the file beside it, not
// something in whatever directory the process happened to start in.
std::string resolve_path(const std::string &value, const std::string &file)
{
	if (value.empty() || value[0] == '/')
		return value;
	if (value[0] == '~' && (value.size() == 1 || value[1] == '/')) {
		const char *home = getenv("HOME");
		if (home && *home)
			return std::string(home) + value.substr(1);
		return value;
	}
	size_t slash = file.rfind('/');
	if (slash == std::string::npos)
		return value;
	return file.substr(0, slash + 1) + value;
}

// Whole-string unsigned decimal. Trailing junk is an error, not a silent
// truncation: `8G` must say so rather than mean 8.
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

bool parse_bool(const std::string &v, bool &out)
{
	std::string s = lowercased(v);
	if (s == "yes" || s == "true" || s == "on" || s == "1") {
		out = true;
		return true;
	}
	if (s == "no" || s == "false" || s == "off" || s == "0") {
		out = false;
		return true;
	}
	return false;
}

bool file_exists(const std::string &p)
{
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

// Every diagnostic names the file, the line, and what to do about it.
std::ostream &at_line(std::ostream &err, const std::string &path, int lineno)
{
	err << "madc: " << path << ":" << lineno << ": ";
	return err;
}

}   // namespace

config_file::config_file(const std::string &app)
	: _app(app)
{
}

void config_file::add_key(const char *key, value_kind kind, std::string *text,
			  std::vector<std::string> *list, unsigned long *count,
			  bool *flag, bool *seen, const char *units)
{
	key_spec spec;
	spec.name = lowercased(key);
	spec.kind = kind;
	spec.text = text;
	spec.list = list;
	spec.count = count;
	spec.flag = flag;
	spec.seen = seen;
	spec.units = units;
	_keys.push_back(spec);
}

void config_file::accept_text(const char *key, std::string &out, bool *seen)
{
	add_key(key, vkText, &out, NULL, NULL, NULL, seen);
}

void config_file::accept_text_list(const char *key, std::vector<std::string> &out)
{
	add_key(key, vkTextList, NULL, &out, NULL, NULL, NULL);
}

void config_file::accept_path(const char *key, std::string &out, bool *seen)
{
	add_key(key, vkPath, &out, NULL, NULL, NULL, seen);
}

void config_file::accept_path_list(const char *key, std::vector<std::string> &out)
{
	add_key(key, vkPathList, NULL, &out, NULL, NULL, NULL);
}

void config_file::accept_count(const char *key, unsigned long &out, bool *seen,
			       const char *units)
{
	add_key(key, vkCount, NULL, NULL, &out, NULL, seen, units);
}

void config_file::accept_flag(const char *key, bool &out, bool *seen)
{
	add_key(key, vkFlag, NULL, NULL, NULL, &out, seen);
}

const config_file::key_spec *config_file::find_key(const std::string &name) const
{
	for (const key_spec &spec : _keys)
		if (spec.name == name)
			return &spec;
	return NULL;
}

// The accepted-key list in the unknown-key diagnostic is generated from the
// registered schema, so it can never drift from what the reader takes.
std::string config_file::accepted_keys() const
{
	std::string list;
	for (const key_spec &spec : _keys) {
		if (!list.empty())
			list += ", ";
		list += spec.name;
	}
	return list;
}

bool config_file::assign(const key_spec &spec, const std::string &value,
			 const std::string &path, int lineno, std::ostream &err)
{
	unsigned long n = 0;
	bool b = false;
	switch (spec.kind) {
	case vkText:
		*spec.text = value;
		break;
	case vkTextList:
		spec.list->push_back(value);
		break;
	case vkPath:
		*spec.text = resolve_path(value, path);
		break;
	case vkPathList:
		// Repeatable and ORDERED: the consumer decides where the list
		// sits relative to its own command-line inputs.
		spec.list->push_back(resolve_path(value, path));
		break;
	case vkCount:
		if (!parse_ulong(value, n)) {
			at_line(err, path, lineno)
				<< spec.name << " needs a whole number";
			if (spec.units)
				err << " of " << spec.units;
			err << " (0 disables), got '" << value << "'"
			    << std::endl;
			return false;
		}
		*spec.count = n;
		break;
	case vkFlag:
		if (!parse_bool(value, b)) {
			at_line(err, path, lineno)
				<< spec.name << " needs yes/no (also true/false,"
				   " on/off, 1/0), got '" << value << "'"
				<< std::endl;
			return false;
		}
		*spec.flag = b;
		break;
	}
	if (spec.seen)
		*spec.seen = true;
	return true;
}

std::vector<std::string> config_file::search_paths() const
{
	std::vector<std::string> paths;
	// 1. The current directory — a per-project config, the common case.
	paths.push_back(_app + ".ini");
	// 2. The user config directory (XDG; honoured on macOS too when set).
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		paths.push_back(std::string(xdg) + "/" + _app + "/" + _app + ".ini");
	else {
		const char *home = getenv("HOME");
		if (home && *home)
			paths.push_back(std::string(home) + "/.config/" + _app
					+ "/" + _app + ".ini");
	}
	// 3. The system config directory, baked at build time from $(sysconfdir).
#ifdef MADC_SYSCONFDIR
	paths.push_back(std::string(MADC_SYSCONFDIR) + "/" + _app + ".ini");
#endif
	return paths;
}

bool config_file::parse_file(const std::string &path, std::ostream &err)
{
	std::ifstream in(path.c_str());
	if (!in.is_open()) {
		err << "madc: cannot read config file '" << path << "'" << std::endl;
		return false;
	}
	_source_path = path;
	std::string raw;
	int lineno = 0;
	while (std::getline(in, raw)) {
		++lineno;
		std::string line = trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		// An optional [<app>] section header, so a file can grow sections
		// later without breaking today's flat ones. Any other section is a
		// typo or a file meant for a different program.
		if (line[0] == '[') {
			if (line.back() != ']') {
				at_line(err, path, lineno)
					<< "unterminated section header: " << line
					<< std::endl;
				return false;
			}
			std::string section = trim(line.substr(1, line.size() - 2));
			if (lowercased(section) != lowercased(_app)) {
				at_line(err, path, lineno)
					<< "unknown section [" << section
					<< "] (only [" << _app << "] is accepted)"
					<< std::endl;
				return false;
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			at_line(err, path, lineno)
				<< "expected 'key = value': " << line << std::endl;
			return false;
		}
		std::string key = lowercased(trim(line.substr(0, eq)));
		std::string value = unquote(trim(line.substr(eq + 1)));
		if (key.empty()) {
			at_line(err, path, lineno) << "missing key before '='"
						  << std::endl;
			return false;
		}
		if (value.empty()) {
			at_line(err, path, lineno)
				<< "key '" << key << "' has an empty value"
				<< std::endl;
			return false;
		}
		const key_spec *spec = find_key(key);
		if (!spec) {
			at_line(err, path, lineno)
				<< "unknown key '" << key << "' (accepted: "
				<< accepted_keys() << ")" << std::endl;
			return false;
		}
		if (!assign(*spec, value, path, lineno, err))
			return false;
	}
	return true;
}

bool config_file::load(const char *explicit_path, std::ostream &err)
{
	if (explicit_path && *explicit_path)
		return parse_file(explicit_path, err);
	for (const std::string &p : search_paths())
		if (file_exists(p))
			return parse_file(p, err);
	return true;	// no config file anywhere: the normal case
}

}   // namespace cfg
}   // namespace madc
