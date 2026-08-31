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

// Resolve a (possibly relative) path against a base directory. A "."
// base adds nothing — a manifest in the cwd (madcide's default
// <base>.prj.json beside the launch file) must yield the TU's own
// spelling, not a "./"-prefixed twin of it (buffer paths compare by
// spelling; "./x" and "x" would open twice).
std::string resolve(const std::string &base, const std::string &p) {
	if (p.empty() || p[0] == '/') return p;
	if (base.empty() || base == ".") return p;
	std::string b = base;
	if (b.back() != '/') b += '/';
	return b + p;
}

// Scan argv tokens for -D/-I/-std=/-stdlib=, filling the TU. Ignores the rest.
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
		} else if (a.rfind("-stdlib=", 0) == 0) {
			// A libc++ project's manifest carries this, and ignoring
			// it would silently compile against the WRONG standard
			// library's headers.
			tu.stdlib_option = a.substr(8);
		}
	}
}
} // namespace

namespace {
// The compile_commands.json import (READ-ONLY interop — a generated file;
// madcide never writes this shape): a top-level ARRAY of
// {directory, file, command|arguments} entries.
bool read_cc_array(const json &root, const std::string &manifest_dir,
		   ProjectManifest &out, std::string &err) {
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

// The NATIVE manifest (owner ruling 2026-08-31): a top-level OBJECT
// mirroring ProjectManifest — the shape madcide reads AND writes. A TU
// entry is a bare string (just the file) or an object carrying the
// ProjectTU fields directly ("file" required; "directory", "defines",
// "include_dirs", "std", "stdlib" optional). Unknown keys — top-level
// (a future "commands" section) and per-TU alike — pass through
// untouched: forward compatibility is the writer-symmetry contract.
bool read_native_object(const json &root, const std::string &manifest_dir,
			ProjectManifest &out, std::string &err) {
	// "tus" is the shape's one REQUIRED key (possibly empty): an object
	// without it is not a manifest — reject it rather than silently
	// building nothing.
	if (!root.contains("tus")) {
		err = "manifest object has no \"tus\"";
		return false;
	}
	{
		const json &tus = root["tus"];
		if (!tus.is_array()) { err = "\"tus\" is not an array"; return false; }
		for (const auto &entry : tus) {
			ProjectTU tu;
			if (entry.is_string()) {
				tu.working_dir = manifest_dir;
				tu.file = resolve(manifest_dir,
						  entry.get<std::string>());
			} else if (entry.is_object()) {
				tu.working_dir = entry.value("directory",
							     std::string());
				if (tu.working_dir.empty())
					tu.working_dir = manifest_dir;
				tu.file = entry.value("file", std::string());
				if (tu.file.empty()) {
					err = "TU entry missing \"file\"";
					return false;
				}
				tu.file = resolve(tu.working_dir, tu.file);
				if (entry.contains("defines") && entry["defines"].is_array())
					for (const auto &d : entry["defines"])
						if (d.is_string())
							tu.defines.push_back(d.get<std::string>());
				if (entry.contains("include_dirs") && entry["include_dirs"].is_array())
					for (const auto &i : entry["include_dirs"])
						if (i.is_string())
							tu.include_dirs.push_back(
							    resolve(tu.working_dir,
								    i.get<std::string>()));
				tu.std_option = entry.value("std", std::string());
				tu.stdlib_option = entry.value("stdlib", std::string());
			} else {
				err = "TU entry is neither a string nor an object";
				return false;
			}
			out.tus.push_back(tu);
		}
	}
	if (root.contains("entry") && root["entry"].is_string())
		out.entry = root["entry"].get<std::string>();
	if (root.contains("output") && root["output"].is_string())
		out.output_name = root["output"].get<std::string>();
	return true;
}
} // namespace

bool read_project_manifest(const std::string &path,
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

	// The shape routes: OBJECT = the native madc manifest, ARRAY = a
	// compile_commands.json import (owner ruling 2026-08-31).
	if (root.is_object())
		return read_native_object(root, manifest_dir, out, err);
	if (root.is_array())
		return read_cc_array(root, manifest_dir, out, err);
	err = "top-level JSON is neither an object (madc manifest) nor an "
	      "array (compile_commands.json)";
	return false;
}
