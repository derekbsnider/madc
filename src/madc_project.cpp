#include "madc_project.h"
#include "json.hpp"
#include <fstream>
#include <cctype>

using nlohmann::json;

namespace {
// Shell-split a `command` string into argv-like tokens. Honors simple
// single/double quotes (compile_commands.json rarely needs more).
void shell_split(const std::string &cmd, std::vector<std::string> &out) {
	size_t i = 0; std::string cur; bool in = false;
	char q = 0;
	while (i < cmd.size()) {
		char c = cmd[i++];
		if (q) { if (c == q) q = 0; else cur += c; continue; }
		if (c == '"' || c == '\'') { q = c; in = true; continue; }
		if (std::isspace((unsigned char)c)) {
			if (in) { out.push_back(cur); cur.clear(); in = false; }
			continue;
		}
		cur += c; in = true;
	}
	if (in) out.push_back(cur);
}

// Resolve a (possibly relative) path against a base directory.
std::string resolve(const std::string &base, const std::string &p) {
	if (p.empty() || p[0] == '/') return p;
	if (base.empty()) return p;
	std::string b = base;
	if (b.back() != '/') b += '/';
	return b + p;
}

// Scan argv tokens for -D/-I/-std=, filling the TU. Ignores everything else.
void apply_args(const std::vector<std::string> &args,
		const std::string &dir, ProjectTU &tu) {
	for (size_t k = 0; k < args.size(); k++) {
		const std::string &a = args[k];
		if (a.rfind("-D", 0) == 0) {
			std::string v = a.substr(2);
			if (v.empty() && k + 1 < args.size() && !args[k + 1].empty() && args[k + 1][0] != '-')
				v = args[++k];
			if (!v.empty()) tu.defines.push_back(v);
		} else if (a.rfind("-I", 0) == 0) {
			std::string v = a.substr(2);
			if (v.empty() && k + 1 < args.size() && !args[k + 1].empty() && args[k + 1][0] != '-')
				v = args[++k];
			if (!v.empty()) tu.include_dirs.push_back(resolve(dir, v));
		} else if (a.rfind("-std=", 0) == 0) {
			tu.std_option = a.substr(5);
		}
	}
}
} // namespace

bool read_compile_commands(const std::string &path,
			   ProjectManifest &out, std::string &err) {
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f) { err = "cannot open " + path; return false; }

	json root;
	try {
		root = json::parse(f, nullptr, true, /*ignore_comments=*/true);
	} catch (const std::exception &e) {
		err = std::string("JSON parse error: ") + e.what();
		return false;
	}
	if (!root.is_array()) { err = "top-level JSON is not an array"; return false; }

	// Base for relative paths when an entry omits "directory": the manifest's
	// own directory. This lets a manifest with relative paths be committed and
	// stay portable, resolving the same regardless of the process cwd (which a
	// project's program may need to set elsewhere — e.g. SMAUG runs from its
	// data directory). An explicit "directory" is honored unchanged.
	std::string manifest_dir;
	{
		size_t s = path.find_last_of('/');
		manifest_dir = (s == std::string::npos) ? std::string(".")
						       : path.substr(0, s);
	}

	for (const auto &entry : root) {
		if (!entry.is_object()) { err = "entry is not an object"; return false; }
		ProjectTU tu;
		tu.working_dir = entry.value("directory", std::string());
		if (tu.working_dir.empty()) tu.working_dir = manifest_dir;
		tu.file = entry.value("file", std::string());
		if (tu.file.empty()) { err = "entry missing \"file\""; return false; }

		std::vector<std::string> args;
		if (entry.contains("arguments") && entry["arguments"].is_array()) {
			for (const auto &a : entry["arguments"])
				if (a.is_string()) args.push_back(a.get<std::string>());
		} else if (entry.contains("command") && entry["command"].is_string()) {
			shell_split(entry["command"].get<std::string>(), args);
		}

		tu.file = resolve(tu.working_dir, tu.file);
		apply_args(args, tu.working_dir, tu);
		out.tus.push_back(tu);
	}
	return true;
}
