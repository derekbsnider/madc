///////////////////////////////////////////////////////////////////////////
//                                                                       //
// madc's OWN configuration schema (forest-carriers S6): the five keys a   //
// madc.ini may set, registered against the schema-blind reader in         //
// madc_config_file.h. Everything about the FORMAT — the lookup chain, the //
// grammar, path resolution, the strict diagnostics — lives there and is   //
// shared with every other in-tree consumer; this file is only madc's half //
// of that contract.                                                      //
//                                                                       //
// Applying the values, and therefore the precedence rule                  //
// CLI > environment > madc.ini > baked defaults, belongs to the consumer  //
// too: see src/madc.cpp, where it is enforced in one visible place.       //
//                                                                       //
// See include/madc_config.h for why reading a config file is a madc(1)     //
// feature and not a libmadc one.                                         //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <ostream>
#include <string>
#include <vector>

#include "madc_config.h"
#include "madc_config_file.h"

namespace madc {

namespace {

// THE madc schema — the single owner of madc's key spellings. A new setting is
// one line here plus a field in config_settings; the reader learns key names
// only through these calls, and generates its own accepted-key diagnostic from
// them.
void register_madc_keys(cfg::config_file &cf, config_settings &out)
{
	cf.accept_text("std", out.std_option, &out.has_std);
	cf.accept_text("stdlib", out.stdlib_option, &out.has_stdlib);
	cf.accept_path("forest", out.forest);
	cf.accept_path_list("include", out.include_dirs);
	cf.accept_count("cpu-limit", out.cpu_limit_secs, &out.has_cpu_limit,
			"seconds");
	cf.accept_count("mem-limit", out.mem_limit_mb, &out.has_mem_limit,
			"megabytes");
}

}   // namespace

std::vector<std::string> config_search_paths()
{
	return cfg::config_file("madc").search_paths();
}

bool config_parse_file(const std::string &path, config_settings &out,
		       std::ostream &err)
{
	cfg::config_file cf("madc");
	register_madc_keys(cf, out);
	bool ok = cf.parse_file(path, err);
	out.source_path = cf.source_path();
	return ok;
}

bool config_file_supported()
{
#ifdef MADC_ENABLE_CONFIG_FILE
	return true;
#else
	return false;
#endif
}

bool config_load(const char *explicit_path, config_settings &out,
		 std::ostream &err)
{
#ifndef MADC_ENABLE_CONFIG_FILE
	// The configure axis is off: madc performs no config lookup at all — it
	// never searches for, nor opens, a madc.ini. An explicitly named one
	// still refuses loudly rather than being quietly ignored. (The reader
	// itself is ordinary in-tree code either way; a parser nobody calls is
	// not a call path, and the sandbox contract lives on the policy knobs.)
	if (explicit_path && *explicit_path) {
		err << "madc: --config=: this madc was built without config-file"
		       " support (configure --enable-config-file)" << std::endl;
		return false;
	}
	(void)out;
	return true;
#else
	cfg::config_file cf("madc");
	register_madc_keys(cf, out);
	bool ok = cf.load(explicit_path, err);
	out.source_path = cf.source_path();
	return ok;
#endif
}

}   // namespace madc
