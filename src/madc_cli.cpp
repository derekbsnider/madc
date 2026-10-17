// src/madc_cli.cpp — madc's own command line: every option as one row of one
// table (include/madc_cli.h), and the rule for where the program's own argv
// begins. The reader is libmadc/cli_options.h; main() (src/madc.cpp) applies
// what it reads. The madc.ini key lines document register_madc_keys
// (src/madc_config.cpp), the owner of those spellings.

#include "madc_cli.h"

#include "madc_file_kinds.h"	// the compiler-line rule reads a source's family

using madc::cli::row;
using madc::cli::value_form;
using madc::cli::value_type;

namespace {

const value_form flag = value_form::none;
const value_form sep = value_form::separate;
const value_form glued = value_form::joined;
const value_form pfx = value_form::prefix;
const value_form both = value_form::joined_or_separate;
const value_form eq = value_form::equals;
const value_form opt_eq = value_form::optional_equals;

const uint32_t doc = madc::cli::rf_doc;
const uint32_t hidden = madc::cli::rf_hidden;

constexpr int id(cli_option o)
{
	return (int)o;
}

enum : unsigned
{
	s_input, s_language, s_codegen, s_output, s_forest, s_misc, s_config, s_env, s_aot
};

const madc::cli::section sections[] = {
	{ "Input / mode", NULL },
	{ "Language / preprocessor (gcc/clang-style)", NULL },
	{ "Codegen", NULL },
	{ "Output (no run)", NULL },
	{ "Frozen forest (compile once, run without parsing)", NULL },
	{ "Misc", NULL },
	{ "Configuration file (optional; CLI > environment > madc.ini > defaults)",
	  "Searched in order; the first existing file wins (files are never merged): "
	  "./madc.ini, $XDG_CONFIG_HOME/madc/madc.ini (else %APPDATA%\\madc\\madc.ini on "
	  "Windows, else ~/.config/madc/madc.ini), then the system config dir. Keys (an unknown "
	  "one is an error, not a warning) are listed below; relative paths resolve against the "
	  "config file's own directory, and ~/ works." },
	{ "Environment", NULL },
	{ "AOT output (gcc vocabulary; do not run)", NULL },
};

const char *const opt_levels[] = { "0", "1", "2", "3", NULL };
const char *const json_only[] = { "json", NULL };
const char *const shells[] = { "bash", "zsh", "fish", "powershell", NULL };

// Field order: id, name, spellings, metavar, section, help, usage, flags,
// tags, type, choices (cli_options.h).
const row rows[] = {
	// ---- Input / mode
	{ 0, NULL, {}, NULL, s_input, "compile and JIT-run a single source file", "<file>", doc },
	{ 0, NULL, {}, NULL, s_input,
	  "the REPL when stdin is a terminal; otherwise compile and run stdin, as if named -",
	  "(no file)", doc },
	{ 0, NULL, {}, NULL, s_input, "read the program from stdin (gcc, python)", "-", doc },
	{ id(cli_option::interactive), "interactive", { { "-i", flag }, { "--interactive", flag } },
	  NULL, s_input,
	  "the REPL, even when stdin is not a terminal; with a <file>, run it first and keep its "
	  "names (python -i). A line starting with : is an ex command on <file>'s buffer (vim's "
	  "colon mode: :p :s :g :w :e ...; :q quits)" },
	{ id(cli_option::lsp), "lsp", { { "--lsp", flag } }, NULL, s_input,
	  "a Language Server Protocol server on stdin/stdout (Content-Length framed): "
	  "diagnostics, symbols, definition, references, hover, semantic tokens for the "
	  "documents a client opens; <file> is parsed before the client asks",
	  "--lsp [<file>]" },
	{ id(cli_option::mcp), "mcp", { { "--mcp", flag } }, NULL, s_input,
	  "a Model Context Protocol server on stdin/stdout (one JSON-RPC message per line): the "
	  "code-graph tools over <file>, for the operator",
	  "--mcp <file>" },
	{ id(cli_option::stdio), "stdio", { { "--stdio", flag } }, NULL, s_input,
	  "with --lsp or --mcp: the transport is stdin/stdout, the only one madc's servers "
	  "speak (LSP clients pass it)" },
	{ id(cli_option::history_file), "history_file", { { "--history-file", eq } }, "PATH|no",
	  s_input,
	  "where the REPL keeps its history (default $MADC_HISTORY_FILE, else "
	  "$XDG_STATE_HOME/madc/history); no keeps none" },
	{ id(cli_option::project), "project", { { "--project", sep } }, "<prj.json>", s_input,
	  "build from a project manifest: compile each translation unit, link the modules, run "
	  "the entry. Two shapes by top-level JSON kind: an OBJECT is the native manifest "
	  "(tus/entry/output — what madcide writes), an ARRAY is a compile_commands.json import" },
	{ 0, NULL, {}, NULL, s_input,
	  "a .json source is treated as a project manifest (implicit --project; gcc/clang-style "
	  "by extension)",
	  "<file.json>", doc },
	{ 0, NULL, {}, NULL, s_input,
	  "execute madc-compiled relocatable objects (-c/-r output) as a precompiled cache: MIR's "
	  "in-process loader maps and relocates them — no recompilation; several .o inputs merge "
	  "first (multi-object link); with -o/-shared/-r they LINK into one artifact",
	  "<file.o>...", doc },
	{ id(cli_option::preprocess), "preprocess", { { "-E", flag } }, NULL, s_input,
	  "preprocess only: print the expanded source, or write it to the file -o names",
	  NULL, 0, mt_stage },

	// ---- Language / preprocessor
	{ id(cli_option::language_std), "std", { { "--std", eq }, { "-std", eq } }, "<std>",
	  s_language,
	  "c89/c90/c99/c11/c17/c23 (c = c11), gnuNN, c++NN, gnu++NN, or madc (C++ keywords "
	  "reserved). Without it the extension decides, as gcc/g++ do: .c gnu17, "
	  ".cpp/.cc/.cxx gnu++17, .mad madc; else madc.ini's std, else madc. A --project TU alike." },
	{ id(cli_option::stdlib_flavor), "stdlib", { { "-stdlib", eq } }, "<lib>", s_language,
	  "C++ standard library flavor (clang's spelling): libstdc++ / libc++, whichever this "
	  "build could probe. REPLACES the C++ include search list, as clang does — it is not "
	  "the same as putting the library first with -I." },
	{ id(cli_option::define), "define", { { "-D", both } }, "<name>[=value]", s_language,
	  "define a preprocessor macro" },
	{ id(cli_option::include_dir), "include", { { "-I", both } }, "<dir>", s_language,
	  "add an include search directory" },
	{ id(cli_option::library), "library", { { "-l", glued } }, "<name>", s_language,
	  "bind a library so its symbols resolve at link time (e.g. -lcrypt): a module or bare "
	  "name spelled for the target by the module map (-lm = libm.so.6 / libSystem.B.dylib / "
	  "ucrtbase.dll; lib<name>.<dso> otherwise; a path verbatim). The build-line form of the "
	  "source's `import`. Works with or without --project. In AOT mode it becomes a "
	  "DT_NEEDED / load command / PE import; otherwise the library is opened." },
	{ id(cli_option::library_dir), "library_dir", { { "-L", both } }, "<dir>", s_language,
	  "search <dir> for the -l libraries ahead of the toolchain's directories (ld's -L "
	  "order): a run opens the library found, an image records it as ld does (its SONAME; "
	  "nothing for an empty archive)" },
	{ id(cli_option::module_path), "module_path", { { "--module-path", eq } }, "<dir>",
	  s_language,
	  "search <dir> for the modules `import NAME;` names, after the program's own directory "
	  "and ahead of madc's own module directory (repeatable; a module is <dir>/<NAME>.<dso>)" },
	{ id(cli_option::import_module), "import_module", { { "--import", eq } }, "<name>",
	  s_language,
	  "import the module <name> ahead of the program, as if its source began with "
	  "`import <name>;` (repeatable); with no source, only try the import: exit 0, or "
	  "print the import error and exit 69" },
	{ id(cli_option::linker_option), "linker_option", { { "-Wl,", glued } }, "<option>",
	  s_language,
	  "add <dir> to the produced image's run-time library path (DT_RUNPATH / LC_RPATH), "
	  "after madc's own; any other -Wl, linker option is refused",
	  "-Wl,-rpath,<dir>" },
	{ id(cli_option::no_auto_load), "no_auto_load", { { "--no-auto-load", flag } }, NULL,
	  s_language,
	  "do not act on `import` / #load library bindings (the library is linked, not loaded); "
	  "link explicitly via -l. The namespace binds to the program's own scope." },
	{ id(cli_option::no_includes), "no_includes", { { "--no-includes", flag } }, NULL,
	  s_language, "do not process #include directives" },
	{ id(cli_option::no_embedded_headers), "no_embedded_headers",
	  { { "--no-embedded-headers", flag } }, NULL, s_language,
	  "disable baked-in headers; use real system headers" },
	{ id(cli_option::no_posix_compat), "no_posix_compat", { { "--no-posix-compat", flag } },
	  NULL, s_language, "disable Win64's additive POSIX header supplements" },

	// ---- Codegen
	{ id(cli_option::opt_level), "opt_level", { { "-O", pfx } }, "<level>", s_codegen,
	  "JIT optimization level (bare -O = -O1)", "-O, -O0 .. -O3", 0, 0, value_type::text,
	  opt_levels },
	{ id(cli_option::no_builtin), "no_builtin", { { "-fno-builtin-", glued } }, "<name>",
	  s_codegen, "disable a specific builtin" },
	{ id(cli_option::instrument_functions), "instrument_functions",
	  { { "--finstrument-functions", flag } }, NULL, s_codegen,
	  "emit __cyg_profile instrumentation hooks" },

	// ---- Output (no run)
	{ id(cli_option::emit), "emit", { { "--emit", eq } }, "c11|mc11", s_output,
	  "render the cir_node tree as C / MC11 source" },
	{ id(cli_option::emit_pch), "emit_pch", { { "--emit-pch", flag } }, NULL, s_output,
	  "write a .madh precompiled header (-o names it)" },
	{ id(cli_option::emit_function), "emit_function", { { "--emit-function", sep } }, "<name>",
	  s_output, "print one function's source" },
	{ id(cli_option::dump_source), "dump_source", { { "--dump-source", flag } }, NULL, s_output,
	  "reconstruct full-fidelity source", NULL, 0, mt_stage | mt_introspection },
	{ id(cli_option::dump_cir), "dump_cir", { { "--dump-cir", flag } }, NULL, s_output,
	  "dump the cir_node tree before checking", NULL, 0, mt_introspection },
	{ id(cli_option::dump_nodes), "dump_nodes", { { "--dump-nodes", flag } }, NULL, s_output,
	  "dump the cir_node tree through madc's walker", NULL, 0, mt_introspection },
	{ id(cli_option::dump_cir_checked), "dump_cir_checked", { { "--dump-cir-checked", flag } },
	  NULL, s_output, "dump the cir_node tree after c2mir's check (the c2m -d stage)",
	  NULL, 0, mt_introspection },

	// ---- Frozen forest
	{ id(cli_option::freeze), "freeze", { { "--freeze", eq } }, "<file>", s_forest,
	  "parse + translate, then freeze the module tree into a forest snapshot container (no run)" },
	{ id(cli_option::freeze_append), "freeze_append", { { "--freeze-append", eq } }, "<bin>",
	  s_forest,
	  "same, but append the container to an existing binary (found later from its EOF footer)" },
	{ id(cli_option::freeze_mir_cache), "freeze_mir_cache", { { "--freeze-mir-cache", flag } },
	  NULL, s_forest,
	  "also compile the frozen module and pack its MIR binary form as a cache segment "
	  "(consumers skip node rebuild + c2mir; absent = normal fallback)" },
	{ id(cli_option::freeze_ledger), "freeze_ledger", { { "--freeze-ledger", eq } }, "<src>",
	  s_forest,
	  "also compile this C-lane runtime source into the container's AOT ledger segment, so "
	  "-static-libmadc programs carry it (repeatable; the build's list is "
	  "scripts/ledger_sources.txt)" },
	{ id(cli_option::freeze_also_std), "freeze_also_std", { { "--freeze-also-std", eq } },
	  "<std>", s_forest,
	  "label the frozen profile to serve compiles under this --std= too (repeatable; the "
	  "release pack names the C++ modes that share madc's corpus)" },
	{ id(cli_option::freeze_role), "freeze_role", { { "--freeze-role", eq } },
	  "<role>", s_forest,
	  "stamp the frozen profile with the consumer it serves (system, dialect); a dialect "
	  "image is stored raw, read in place (the release pack's --dialect-image)" },
	{ id(cli_option::print_dialect_roots), "print_dialect_roots",
	  { { "--print-dialect-roots", flag } }, NULL, s_forest,
	  "(pack step, --std=madc) print the dialect image's roots — every dialect fragment and "
	  "the headers its auto-include pulls, in auto-include order, one per line — and exit" },
	{ id(cli_option::emit_builtin_registry), "emit_builtin_registry",
	  { { "--emit-builtin-registry", eq } }, "<out.cpp>", s_forest,
	  "(build step, --std=madc) write the dialect's generated registration — one builder "
	  "per namespace member read from the dialect files, and its table — to <out.cpp>, "
	  "and exit" },
	{ id(cli_option::emit_module_table), "emit_module_table",
	  { { "--emit-module-table", eq } }, "<out.inc>", s_forest,
	  "(module build step, --std=madc) read the source file as a module's face and write "
	  "its declarations — one builder per name and the static table the module's "
	  "description returns — to <out.inc>, and exit" },
	{ id(cli_option::freeze_sources), "freeze_sources", { { "--freeze-sources", eq } },
	  "<list>", s_forest,
	  "also pack the exact text of every file <list> names (one path per line) as "
	  "source-only units, served to a compile with no headers on disk (repeatable; the "
	  "release pack lists every system header)" },
	{ id(cli_option::no_sysroot_includes), "no_sysroot_includes",
	  { { "--no-sysroot-includes", flag } }, NULL, s_forest,
	  "resolve <...> only from the toolchain's own roots (the C++ stdlib dirs + the "
	  "compiler's builtin dir) and the embedded headers — never the host's C library / SDK "
	  "dirs. The freezer's posture: a frozen forest is the header-less consumer's corpus" },
	{ id(cli_option::run_frozen), "run_frozen", { { "--run-frozen", opt_eq } }, "<file>",
	  s_forest,
	  "thaw + compile + run a frozen container; with no value, load the blob appended to "
	  "this executable. Remaining arguments become the program's argv" },
	{ id(cli_option::freeze_run), "freeze_run", { { "--freeze-run", flag } }, NULL, s_forest,
	  "freeze to a temp container, then re-exec this madc in a FRESH process to run it "
	  "(round-trip)" },
	{ id(cli_option::pack_forest), "pack_forest", { { "--pack-forest", eq } }, "<file>",
	  s_forest,
	  "with -o/-shared: the emitted native image also carries this frozen container in its "
	  "self-image carrier (ELF: appended trailer; Mach-O: a __MADC,__forest section signed "
	  "at emit)" },
	{ id(cli_option::dump_forest), "dump_forest", { { "--dump-forest", opt_eq } }, "<file>",
	  s_forest,
	  "print a container's directory + grove payloads (decl index, PP exports, edges, branch "
	  "macros, canonical order); no value = this executable's blob",
	  NULL, 0, mt_introspection },
	{ id(cli_option::forest_bind), "forest_bind", { { "--forest-bind", opt_eq } }, "<file>",
	  s_forest,
	  "bind grove-backed system #includes from a frozen container instead of live-parsing; "
	  "no value = the discovery chain below. This is the DEFAULT for compiles (live "
	  "fall-through when nothing is found); freeze modes live-parse unless it is passed "
	  "explicitly.\n"
	  "Discovery order (first usable container wins):\n"
	  "1. this binary's own image (ELF trailer / Mach-O __MADC,__forest section),\n"
	  "2. the libmadc image, when linked shared,\n"
	  "3. <exe>.forest, then <lib>.forest (sidecars),\n"
	  "4. the $MADC_FOREST path,\n"
	  "5. the madc.ini `forest' key" },
	{ id(cli_option::forest_touch_log), "forest_touch_log", { { "--forest-touch-log", eq } },
	  "<file>", s_forest,
	  "(pack step) record the frozen-table reads this compile's bind makes, for "
	  "--forest-relayout" },
	{ id(cli_option::forest_relayout), "forest_relayout", { { "--forest-relayout", eq } },
	  "<image>", s_forest,
	  "(pack step) lay the image's last forest container out by the recorded reads; every "
	  "argument after it is a log",
	  "--forest-relayout=<image> <log>...", madc::cli::rf_ends_options },
	{ id(cli_option::no_forest_bind), "no_forest_bind", { { "--no-forest-bind", flag } }, NULL,
	  s_forest,
	  "force live parse (overrides the default and an explicit --forest-bind; the A/B "
	  "measurement lever)" },
	{ id(cli_option::dump_registered), "dump_registered", { { "--dump-registered", flag } },
	  NULL, s_forest,
	  "parse, then print the registered name maps (forest index-parity oracle input; no run)",
	  NULL, 0, mt_introspection },
	{ id(cli_option::dump_macros), "dump_macros", { { "-dM", flag } }, NULL, s_forest,
	  "preprocess, then print the effective macro table (gcc -dM -E analogue; no parse, no run)",
	  NULL, 0, mt_stage },

	// ---- Misc
	{ id(cli_option::show_stats), "show_stats", { { "--show-stats", flag } }, NULL, s_misc,
	  "print input/token/timing stats (read, lex, parse, c2mir, execute) to stderr after the run",
	  NULL, 0, mt_introspection },
	{ id(cli_option::debug_info), "debug_info", { { "-g", flag } }, NULL, s_misc,
	  "debug info: gdb can break/step/inspect the JIT'd program (forces -O0, no inlining, "
	  "spill-all)" },
	{ id(cli_option::no_warnings), "no_warnings", { { "-w", flag } }, NULL, s_misc,
	  "inhibit all warning messages (gcc -w)" },
	{ id(cli_option::warning_selection), "warning_selection", { { "-W", pfx } }, "<name>",
	  s_misc,
	  "gcc's warning selection (-Wall, -Wextra, ...): accepted; madc reports one fixed set "
	  "(-Werror is refused)",
	  "-W<name>, -Wno-<name>" },
	{ id(cli_option::warnings_as_errors), "warnings_as_errors", { { "-Werror", pfx } }, NULL,
	  s_misc, "refused: madc's warnings are never errors", NULL, hidden },
	{ id(cli_option::pass_through), "pass_through", { { "-Wa,", glued }, { "-Wp,", glued } },
	  "<option>", s_misc,
	  "refused: madc has no separate assembler or preprocessor to pass it to", NULL, hidden },
	{ id(cli_option::diagnostics_color), "diagnostics_color",
	  { { "-fdiagnostics-color", opt_eq } }, "WHEN", s_misc,
	  "colour diagnostics (gcc's spelling): WHEN is auto (the default: a terminal only, never "
	  "with NO_COLOR set), always, or never; bare = always" },
	{ id(cli_option::no_diagnostics_color), "no_diagnostics_color",
	  { { "-fno-diagnostics-color", flag } }, NULL, s_misc, "never colour diagnostics" },
	{ id(cli_option::verbose), "verbose", { { "-v", flag }, { "--verbose", flag } }, NULL,
	  s_misc, "verbose / debug output" },
	{ id(cli_option::help), "help", { { "-h", flag }, { "-?", flag }, { "--help", opt_eq } },
	  "json", s_misc, "show this help; --help=json describes every option as JSON",
	  "-h, -?, --help[=json]", 0, 0, value_type::text, json_only },
	{ id(cli_option::completion), "completion", { { "--completion", eq } }, "<shell>", s_misc,
	  "print a completion script for bash, zsh, fish or powershell and exit",
	  NULL, 0, 0, value_type::text, shells },
	{ id(cli_option::version), "version", { { "-V", flag }, { "--version", flag } }, NULL,
	  s_misc,
	  "print the madc version (and the cross target, if this artifact has one) and exit" },
	{ id(cli_option::capabilities), "capabilities", { { "--capabilities", opt_eq } }, "json",
	  s_misc,
	  "print the build's machine-readable capability manifest (no source file required) "
	  "and exit",
	  "--capabilities=json" },
	{ id(cli_option::dump_machine), "dump_machine", { { "-dumpmachine", flag } }, NULL, s_misc,
	  "print the target triple the toolchain names (gcc -dumpmachine) and exit" },

	// ---- Configuration file
	{ id(cli_option::config), "config", { { "--config", eq } }, "<file>", s_config,
	  "read this madc.ini instead of searching; a file that fails to load is an error, "
	  "never ignored" },
	{ id(cli_option::no_config), "no_config", { { "--no-config", flag } }, NULL, s_config,
	  "skip the search entirely (hermetic builds)" },
	{ 0, NULL, {}, NULL, s_config, "default dialect (a --std= on the CLI wins)",
	  "std = c17", doc },
	{ 0, NULL, {}, NULL, s_config, "default C++ stdlib flavor (a -stdlib= on the CLI wins)",
	  "stdlib = libc++", doc },
	{ 0, NULL, {}, NULL, s_config, "frozen forest container (discovery arm 5)",
	  "forest = <file>", doc },
	{ 0, NULL, {}, NULL, s_config, "extra include dir, repeatable, searched after -I",
	  "include = <dir>", doc },
	{ 0, NULL, {}, NULL, s_config, "a module to import, as --import=<name> (repeatable)",
	  "modules = <name>", doc },
	{ 0, NULL, {}, NULL, s_config,
	  "the REPL's colours: a .theme file in place of the built-in theme",
	  "theme = <file>", doc },
	{ 0, NULL, {}, NULL, s_config, "MADC_CPU_LIMIT default (auto = off)",
	  "cpu-limit = off|auto|<secs>", doc },
	{ 0, NULL, {}, NULL, s_config, "MADC_MEM_LIMIT default",
	  "mem-limit = off|auto|<MB>", doc },

	// ---- Environment
	{ 0, NULL, {}, NULL, s_env,
	  "RLIMIT_CPU guard (default off; auto is off too — madc also runs the program, so no "
	  "finite default is safe; for embedding hosts and sandboxes)",
	  "MADC_CPU_LIMIT=off|auto|<secs>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "address-space guard (RLIMIT_AS, a SOFT limit covering JIT mappings); default off; "
	  "auto = 4096 MB + 128 MB per --project TU (the test runner asks for auto). A program "
	  "that imports a GUI module lifts it at run start. Trips name the knob.",
	  "MADC_MEM_LIMIT=off|auto|<MB>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "frozen forest container to bind when no earlier discovery arm (binary image, libmadc "
	  "image, <exe>.forest / <lib>.forest sidecars) carries one; beats the madc.ini `forest' "
	  "key, which is last",
	  "MADC_FOREST=<file>", doc },
	{ 0, NULL, {}, NULL, s_env,
	  "where the madc.ini search looks for the user config (<dir>/madc/madc.ini; default "
	  "%APPDATA% on Windows, ~/.config elsewhere)",
	  "XDG_CONFIG_HOME=<dir>", doc },

	// ---- AOT output
	{ id(cli_option::compile_object), "compile_object", { { "-c", flag } }, NULL, s_aot,
	  "compile to a relocatable native object (default: <source-base>.o in the current dir; "
	  "--project: one .o per TU, gcc semantics)",
	  "-c [-o file.o]", 0, mt_stage },
	{ id(cli_option::relocatable), "relocatable", { { "-r", flag } }, NULL, s_aot,
	  "relocatable link output (gcc/ld -r): ONE .o carrying the whole input — with --project "
	  "the whole-program capture (default name: a.out)",
	  "-r [-o file.o]", 0, mt_stage },
	{ id(cli_option::output), "output", { { "-o", sep } }, "<file>", s_aot,
	  "compile to a native executable — MIR assembles the ELF directly (no external "
	  "toolchain); needs libmadc.so at run time (DT_RUNPATH is set); position-independent "
	  "(PIE) by default, gcc parity. With -c / -r / -shared it names that artifact; with "
	  "-E / -dM, the preprocessed text's file; with --emit-pch, the .madh",
	  NULL, 0, mt_stage },
	{ id(cli_option::static_libmadc), "static_libmadc",
	  { { "-static-libmadc", flag }, { "-static", flag } }, NULL, s_aot,
	  "carry the madc runtime pieces the program needs INSIDE the emitted image (from this "
	  "binary's AOT ledger), so it runs with no madc library at all; libc/libstdc++ stay "
	  "dynamic. Covers the C lane (try/catch, VLA scope exit); a program needing the C++ "
	  "script-lane runtime refuses, naming the symbols. -static is an alias (gcc "
	  "-static-libgcc precedent)" },
	{ id(cli_option::no_eval_shims), "no_eval_shims", { { "-fno-eval-shims", flag } }, NULL,
	  s_aot,
	  "leave the __madc_shim_* host-call adapters out of the artifact: it will not be called "
	  "through the value ABI by a libmadc host. An executable emit implies this; pass it when "
	  "compiling .o files you will link with -static-libmadc (the adapters need value-ABI "
	  "accessors, which are not on the ledger)" },
	{ id(cli_option::eval_shims), "eval_shims", { { "-feval-shims", flag } }, NULL, s_aot,
	  "keep the host-call adapters (undoes -fno-eval-shims)" },
	{ id(cli_option::pie), "pie", { { "-pie", flag } }, NULL, s_aot,
	  "keep the PIE layout (the default)" },
	{ id(cli_option::no_pie), "no_pie", { { "-no-pie", flag } }, NULL, s_aot,
	  "drop the PIE layout: a fixed-base ET_EXEC instead of the ET_DYN PIE" },
	{ id(cli_option::gui_subsystem), "gui_subsystem", { { "-mwindows", flag } }, NULL, s_aot,
	  "Windows: a windowed program (no console at start); a --project build reads the "
	  "manifest's \"kind\" (console | gui) instead" },
	{ id(cli_option::console_subsystem), "console_subsystem", { { "-mconsole", flag } }, NULL,
	  s_aot, "Windows: the console subsystem (the default)" },
	{ id(cli_option::shared), "shared", { { "-shared", flag } }, NULL, s_aot,
	  "compile to a shared object (ET_DYN, MIR-assembled; dlopen/import-consumable; PIC, no "
	  "TEXTREL)",
	  "-shared [-o file.so]", 0, mt_stage },
	{ id(cli_option::emit_object), "emit_object", { { "--emit-object", sep } }, "<path>", s_aot,
	  "an alias of -c -o <path>", NULL, 0, mt_stage },
	{ id(cli_option::emit_executable), "emit_executable", { { "--emit-executable", sep } },
	  "<path>", s_aot, "an alias of -o <path>", NULL, 0, mt_stage },
	{ id(cli_option::pic), "pic",
	  { { "-fPIC", flag }, { "-fpic", flag }, { "-fPIE", flag }, { "-fpie", flag } }, NULL,
	  s_aot,
	  "accepted: madc's code is position-independent already (a -c object links into a gcc "
	  "-shared library with no text relocations; an executable is PIE unless -no-pie)" },
};

// A C or C++ source (by extension) with a stage option AFTER it is a compiler
// line, permuted as gcc's (configure's `cc -O2 x.c -o x -lm`). A madc script
// keeps its argv: `madc tool.mad -o out` runs tool.mad (owner, 2026-10-16).
bool compiler_line_source(const madc::cli::table &t, const std::vector<std::string> &words,
			  size_t at)
{
	const int64_t family = madc::file_kind_family(madc::file_kind_of_path(words[at].c_str()));
	if ( family != madc::fkC && family != madc::fkCPP )
		return false;
	for ( size_t j = at + 1; j < words.size(); j++ )
	{
		const madc::cli::match m = madc::cli::match_word(t, words[j]);
		if ( m.option && (m.option->tags & mt_stage) )
			return true;
	}
	return false;
}

// Whether madc's options continue past the operand words[at]. A line that
// builds runs no program, so nothing after its source is a script's argv:
// options and operands interleave as gcc's do (`-c x.c -fPIC -o x.o`;
// ccache's `-E -o x.i x.c`). That lane is on once a stage option was read
// (-o beside --emit-pch names the .madh, not an artifact), once the lane took
// an operand, or for a compiler line. Otherwise this operand is the program
// and the rest of the line its argv.
bool madc_operand_continues(const madc::cli::table &t, const madc::cli::result &so_far,
			    const std::vector<std::string> &words, size_t at)
{
	if ( !so_far.operands.empty() )
		return true;
	const bool pch = so_far.has(id(cli_option::emit_pch));
	for ( size_t k = 0; k < so_far.options.size(); k++ )
	{
		const madc::cli::occurrence &o = so_far.options[k];
		const row *rw = madc::cli::find_row(t, o.id);
		if ( rw && (rw->tags & mt_stage) && !(pch && o.id == id(cli_option::output)) )
			return true;
	}
	return compiler_line_source(t, words, at);
}

const madc::cli::table madc_table = {
	"madc",
	"[options] <source> [program-args...]",
	"madc — My Advanced Dialect of C. Parses a C/C++ dialect into a cir_node tree, lowers "
	"it through c2mir -> MIR, and JIT-executes it (or emits C / a PCH).",
	NULL,
	sections, sizeof(sections) / sizeof(sections[0]),
	rows, sizeof(rows) / sizeof(rows[0]),
	NULL, 0,
	false,				// gcc: no bundling (-dM is a word, not -d -M)
	true,				// @file response files (gcc)
	madc::cli::operand_rule::caller,
	madc_operand_continues,
	NULL
};

} // namespace

const madc::cli::table &madc_cli_table()
{
	return madc_table;
}

bool madc_cli_standard_of(const std::string &word, madc::dis::istring &name)
{
	const madc::cli::match m = madc::cli::match_word(madc_table, word);
	if ( !m.option || m.option->id != id(cli_option::language_std) || !m.has_value )
		return false;
	name = m.value;
	return true;
}
