#ifndef __MADC_PROJECT_H
#define __MADC_PROJECT_H 1

#include <string>
#include <vector>

// One translation unit in a project build, plus the flags it compiles with.
struct ProjectTU {
	std::string file;			// resolved (absolute or manifest-relative) path
	std::string working_dir;		// the entry's "directory" (for -I resolution)
	std::vector<std::string> defines;	// "NAME" or "NAME=VALUE"
	std::vector<std::string> include_dirs;	// -I paths
	std::string std_option;			// e.g. "c89"; empty = madc default
};

// A whole project: the TU list + how to link/run it.
struct ProjectManifest {
	std::vector<ProjectTU> tus;
	std::string entry = "main";	// entry symbol
	std::string output_name;	// informational in v1 (no object output)
};

// Reader: parse a compile_commands.json at `path` into `out`.
// Returns true on success; on failure returns false and sets `err`.
bool read_compile_commands(const std::string &path,
			   ProjectManifest &out, std::string &err);

// Forward decls to avoid pulling madc.h into the header.
class MadcEngine;

// Engine: build+link+JIT-run the manifest. Returns the program's exit code,
// or -1 on a build/link error. Defined in madc_cir.cpp.
int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv);

#endif
