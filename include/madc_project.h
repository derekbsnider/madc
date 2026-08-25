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
	std::string stdlib_option;		// e.g. "libc++"; empty = the build's default flavor
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
class Program;

// Apply one TU's language options (-I / -D / -stdlib / the std selection,
// with the .c -> gnu17 default) to its Program — THE one rule; the
// --project build lane and the project parse handles both apply it.
// False + err = unknown -stdlib flavor. Defined in madc_cir.cpp.
bool apply_project_tu_options(Program &prog, const ProjectTU &tu,
			      std::string &err);

// Engine: build+link+JIT-run the manifest. Returns the program's exit code,
// or -1 on a build/link error. Defined in madc_cir.cpp.
// forest_bind: each TU binds grove-backed system #includes from the frozen
// container (forest_bind_path, or the blob appended to this executable when
// empty) — the compile-mode default, with silent live fall-through when no
// container is present. false = force live parse (--no-forest-bind, the A/B
// measurement lever).
// class_pattern_live_capture: per-TU lazy live ClassPattern capture (the
// MADC_CLASS_PATTERN_LIVE opt-in, resolved by the CLI onto its Program and
// forwarded here so the lever reaches every TU).
// show_stats: print the lane's phase report to stderr (--show-stats) —
// per-TU front-end walls plus the shared link/gen/execution phases.
int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv,
			 bool forest_bind = true,
			 const std::string &forest_bind_path = std::string(),
			 bool class_pattern_live_capture = false,
			 bool show_stats = false);

#endif
