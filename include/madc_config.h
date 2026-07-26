///////////////////////////////////////////////////////////////////////////
//                                                                       //
// madc.ini — the optional configuration file (forest-carriers S6).       //
//                                                                       //
// A READER, not a policy: it locates one file, parses it, and hands back //
// a value object. Applying the values — and therefore the precedence     //
// rule CLI > environment > madc.ini > baked defaults — belongs to the    //
// consumer, so the precedence is visible where the settings land         //
// (src/madc.cpp for the CLI) instead of hidden in here.                  //
//                                                                       //
// The config file is a CLI-SHAPE feature: the standalone `madc` reads    //
// one, and an embedding host does not, because a file that can redirect  //
// where the compiler loads frozen state from is an attack surface for a  //
// sandboxed madc (fork+rlimit+seccomp hosts). A host that WANTS ini      //
// semantics calls this reader itself and applies what it likes to its    //
// own compile_options — nothing here reaches into libmadc's defaults.    //
// The --enable-config-file configure axis removes the file-reading path  //
// entirely for builds that want the surface absent, not merely unused.   //
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
    std::string forest;				// forest = <file>   -> discovery arm 5
    std::vector<std::string> include_dirs;	// include = <dir>   (repeatable)
    unsigned long cpu_limit_secs = 0;		// cpu-limit = <secs>
    bool has_cpu_limit = false;
    unsigned long mem_limit_mb = 0;		// mem-limit = <MB>
    bool has_mem_limit = false;
};

// The lookup chain when no file was named explicitly, in order:
//   ./madc.ini, $XDG_CONFIG_HOME/madc/madc.ini (or ~/.config/madc/madc.ini),
//   <sysconfdir>/madc.ini
// The first EXISTING file wins outright — configs are never merged, because a
// merged chain makes "why is this setting on?" unanswerable.
std::vector<std::string> config_search_paths();

// Parse one file into `out`. false = unreadable or malformed, with the reason
// already written to `err` (file:line plus the offending text). STRICT by
// design: an unknown key is an error, not a warning — a config file is the
// user's own file, and silently ignoring half of it is precisely the silent
// degradation this project refuses. Paths in the file resolve against the
// FILE's directory (a system config naming relative paths against whatever
// directory madc happens to run in is meaningless), and a leading ~/ expands.
bool config_parse_file(const std::string &path, config_settings &out,
		       std::ostream &err);

// The CLI's whole config step. A non-NULL explicit_path (--config=FILE) is the
// entire search and MUST load — a named file that is ignored is the same
// failure as a named forest container that is ignored. Otherwise the chain
// above is walked and a complete miss is normal and silent. false = a hard
// error was reported on `err`.
bool config_load(const char *explicit_path, config_settings &out,
		 std::ostream &err);

// Was config-file support compiled in (--enable-config-file)? The CLI uses it
// to refuse --config= loudly rather than accept a flag it will not honour.
bool config_file_supported();

}   // namespace madc

#endif
