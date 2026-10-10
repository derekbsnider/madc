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

// What kind of program the project builds (the manifest's "kind"; absent =
// console). It decides the executable's SUBSYSTEM on Windows (a gui program
// gets no console at start — gcc's -mwindows) and where an IDE sends the
// program's output when it runs it; ELF and Mach-O links ignore it.
enum class ProjectKind { console, gui };

// A whole project: the TU list + how to link/run it.
struct ProjectManifest {
	std::vector<ProjectTU> tus;
	std::string entry = "main";	// entry symbol
	std::string output_name;	// informational in v1 (no object output)
	ProjectKind kind = ProjectKind::console;
	// "icon": the program's Windows icon file (.ico), resolved against the
	// manifest's directory; "" = none. A PE image carries it as its icon
	// resources (Explorer, the window); ELF and Mach-O images carry no
	// icon, but the file is read and checked on every target.
	std::string icon;
	// "libs": the libraries the program links, as `-l` names them on the
	// command line — a name (`madcide`: madc_module_library_spelling makes
	// it libmadcide.so / libmadcide.dylib / madcide.dll) or a path, resolved
	// against the manifest's directory. Each binds through the binder
	// `import` uses (Program::bind_module_namespace, link form): a run opens
	// it (madc's own lib directory first), a native build records it as
	// needed.
	std::vector<std::string> libs;
	// "madc": the oldest madc release that builds the project
	// ("major.minor.patch", VERSION's form); "" = any. The reader refuses
	// the manifest when the running madc is older, naming both versions.
	std::string madc_min;
};

// The manifest spelling of a kind and back (the one table): "console" /
// "gui". False = not a known kind.
bool project_kind_from_name(const std::string &name, ProjectKind &out);
const char *project_kind_name(ProjectKind kind);

// Reader: parse a project manifest at `path` into `out`. Two shapes route
// by the top-level JSON kind (owner ruling 2026-08-31): an OBJECT is the
// native madc manifest (mirrors this struct: tus + entry + output; the
// shape madcide writes), an ARRAY is a compile_commands.json import
// (READ-ONLY interop). Returns true on success; on failure returns false
// and sets `err`.
bool read_project_manifest(const std::string &path,
			   ProjectManifest &out, std::string &err);
// A project at `path`: a manifest (".json": read_project_manifest's two
// shapes), or a single source file — the one-TU project, its directory the
// TU's (madc --mcp --project FILE). False + err when neither reads.
bool project_of_path(const std::string &path, ProjectManifest &out,
		     std::string &err);

// The manifest's one WRITER, beside its reader (split arc 1B.8): each call
// is a read-modify-write of the native manifest's JSON object that keeps
// every key it does not change (an IDE's "commands", a TU entry's own
// fields), written as nlohmann's dump(2) and a newline — the bytes madcide
// writes (js::stringify(proj, 2)). A comment the reader skips is not kept
// (JSON has none to write). A compile_commands.json import (an array) is
// never written. False + err = refused, the file as it was.
// Thread contract: the C++ standard library's convention; two writers of
// one manifest race as two editors saving one file do (the last write wins).
//
// create: {"entry": "main", "tus": []} at `path`; refused when it exists.
bool project_manifest_create(const std::string &path, std::string &err);
// add: `file` (absolute, or relative to the cwd) joins "tus", named as the
// manifest names its TUs — relative to the manifest's directory when it
// lies below it, else by its canonical path. Refused when it is no file,
// or a TU already.
bool project_manifest_add_tu(const std::string &path, const std::string &file,
			     std::string &err);
// remove: the TU entry naming `file` leaves whole, its own fields with it —
// the entry whose path is `file`'s, or which spells `file` as given.
// Refused when none does. The file itself is untouched.
bool project_manifest_remove_tu(const std::string &path,
				const std::string &file, std::string &err);

// The executable a project builds when no OUT is named (%project build):
// its "output" against the manifest's directory, else the manifest's path
// less ".prj.json" (madcide's <base>.prj.json), else less its extension.
std::string project_default_output(const std::string &manifest_path,
				   const ProjectManifest &manifest);

// Forward decls to avoid pulling madc.h into the header.
class MadcEngine;
class Program;

// Apply one TU's language options (-I / -D / -stdlib / the std selection,
// with the .c -> gnu17 default) to its Program — THE one rule; the
// --project build lane and the project parse handles both apply it.
// False + err = unknown -stdlib flavor. Defined in madc_cir.cpp.
bool apply_project_tu_options(Program &prog, const ProjectTU &tu,
			      std::string &err);

// Engine: build+link+JIT-run the manifest. Returns the run's exit status:
// main()'s return value, or 1 on a build/link error (madc_cir_execute's
// contract). Defined in madc_cir.cpp.
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
