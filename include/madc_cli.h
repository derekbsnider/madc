#ifndef __MADC_CLI_H
#define __MADC_CLI_H 1

// madc_cli.h — madc's OWN command line as one table over the option reader
// (libmadc/cli_options.h): main() parses with it and switches on cli_option;
// the help, --help=json, the completion scripts and the --capabilities
// introspection list are written from it.
//
// THREAD-SAFETY CONTRACT: the table is constant data — read-only, shared.

#include <cstdint>

#include "libmadc/cli_options.h"

// Every madc option, by code (enum-over-strings: main() switches on these,
// and -Wswitch names an option that has no case).
enum class cli_option : int
{
	none = 0,		// help lines (rf_doc rows)
	// Input / mode
	interactive, lsp, mcp, stdio, history_file, project, preprocess,
	// Language / preprocessor
	language_std, stdlib_flavor, define, include_dir, library, library_dir,
	linker_option, no_auto_load, no_includes, no_embedded_headers, no_posix_compat,
	// Codegen
	opt_level, no_builtin, instrument_functions,
	// Output (no run)
	emit, emit_pch, emit_function, dump_source, dump_cir, dump_nodes, dump_cir_checked,
	// Frozen forest
	freeze, freeze_append, freeze_mir_cache, freeze_ledger, freeze_also_std,
	freeze_sources, no_sysroot_includes, run_frozen, freeze_run, pack_forest,
	dump_forest, forest_bind, forest_touch_log, forest_relayout, no_forest_bind,
	dump_registered, dump_macros,
	// Misc
	show_stats, debug_info, no_warnings, warning_selection, warnings_as_errors,
	pass_through, diagnostics_color, no_diagnostics_color, verbose, help,
	completion, version, capabilities, dump_machine,
	// Configuration file
	config, no_config,
	// AOT output
	compile_object, relocatable, output, static_libmadc, no_eval_shims,
	eval_shims, pie, no_pie, gui_subsystem, console_subsystem, shared,
	emit_object, emit_executable, pic
};

// madc's own row tags (row::tags; the reader never reads them).
enum madc_cli_tag : uint32_t
{
	mt_stage = 1u << 0,		// a stage option: the line builds, no program
					// runs (gcc's -c -shared -r -o -E -dM; madc's aliases)
	mt_introspection = 1u << 1	// a dump the --capabilities manifest lists
};

// THE table.
const madc::cli::table &madc_cli_table();

// The standard a `--std=NAME` (or gcc's `-std=NAME`) word names, read as the
// command line reads it: a script's shebang line and a session's standard
// option say it the same way. False for any other word.
bool madc_cli_standard_of(const std::string &word, madc::dis::istring &name);

#endif
