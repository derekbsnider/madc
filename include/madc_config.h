///////////////////////////////////////////////////////////////////////////
//                                                                       //
// madc.ini — madc's own configuration SCHEMA (forest-carriers S6).       //
//                                                                       //
// The five keys a madc.ini may set, and nothing else: the format itself   //
// (lookup chain, grammar, path semantics, strict diagnostics) belongs to  //
// the schema-blind reader in madc_config_file.h, which madcdat and any    //
// madcdis-based tool share. Applying the values — and therefore the       //
// precedence rule CLI > environment > madc.ini > baked defaults —         //
// belongs to the consumer, so the precedence is visible where the         //
// settings land (src/madc.cpp for the CLI) instead of hidden in here.     //
//                                                                       //
// The config file is a CLI-SHAPE feature: the standalone `madc` reads    //
// one, and an embedding host does not, because a file that can redirect  //
// where the compiler loads frozen state from is an attack surface for a  //
// sandboxed madc (fork+rlimit+seccomp hosts). A host that WANTS ini      //
// semantics registers its own keys against the shared reader and applies //
// what it likes to its own compile_options — nothing here reaches into    //
// libmadc's defaults. The --enable-config-file configure axis removes     //
// madc's config LOOKUP: such a build never searches for, nor opens, a    //
// madc.ini.                                                             //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_CONFIG_H
#define __MADC_CONFIG_H 1

#include <iosfwd>
#include <string>
#include <vector>

namespace madc {

// Everything a madc.ini may set — ONE struct, the whole surface. The has_*
// flags are what the precedence rule needs: "the file said 0" and "the file
// said nothing" are different answers when an environment variable or a baked
// default sits underneath.
struct config_settings
{
    std::string source_path;			// file these came from; empty = none loaded
    std::string std_option;			// std = c17         -> --std=c17
    bool has_std = false;
    std::string stdlib_option;			// stdlib = libc++   -> -stdlib=libc++
    bool has_stdlib = false;
    std::string forest;				// forest = <file>   -> discovery arm 5
    std::vector<std::string> include_dirs;	// include = <dir>   (repeatable)
    unsigned long cpu_limit_secs = 0;		// cpu-limit = <secs>
    bool has_cpu_limit = false;
    unsigned long mem_limit_mb = 0;		// mem-limit = <MB>
    bool has_mem_limit = false;
};

// madc's lookup chain (./madc.ini → the user config dir → the system config
// dir). The ORDER and the first-file-wins rule are the reader's contract —
// see madc::cfg::config_file::search_paths().
std::vector<std::string> config_search_paths();

// Parse one named file against the madc schema. false = unreadable or
// malformed, reason already on `err`.
bool config_parse_file(const std::string &path, config_settings &out,
		       std::ostream &err);

// The CLI's whole config step: --config=FILE (that file is the entire search
// and MUST load — a named file that gets ignored is the same failure as a named
// forest container that gets ignored), else the chain above, where a complete
// miss is normal and silent. false = a hard error was reported on `err`.
// With --enable-config-file OFF this performs no lookup at all.
bool config_load(const char *explicit_path, config_settings &out,
		 std::ostream &err);

// Was madc's config lookup compiled in (--enable-config-file)? The CLI uses it
// to refuse --config= loudly rather than accept a flag it will not honour.
bool config_file_supported();

}   // namespace madc

#endif
