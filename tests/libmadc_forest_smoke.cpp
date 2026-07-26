// Embedding-host frozen-forest policy smoke (forest-carriers S4).
//
// A real host: it sees ONLY the public C++ API (libmadc/api.h + engine.h) and
// links libmadc dynamically, so its frozen state can come from the LIBRARY
// image (discovery arm 2) — the shape a packaged install ships. The gate
// (scripts/forest_library_gate.sh) runs it against a packed and an unpacked
// staged library, with and without MADC_FOREST, once per leg below.
//
//   argv[1] = leg
//     default      library defaults: silent fallback (a host that shipped no
//                  container must compile, and must not be nagged)
//     strict       forest_policy::strict_require — no silent degradation
//     strict-noext strict_require + enable_external_forest = false: the
//                  sandboxed host. Image carriers still bind; the sidecar and
//                  MADC_FOREST arms are refused.
//
// Output contract (the gate reads these):
//   exit 0 + "forest-bound"   the compile succeeded — under a strict leg that
//                             PROVES a container bound (strict hard-errors
//                             when the chain ends empty)
//   exit 3 + "forest-refused" the strict policy refused; the message madc
//                             printed on stderr says which case it was
//   other exits               harness failure (unexpected result / no engine)

#include "libmadc/api.h"
#include "libmadc/engine.h"

#include <cstdio>
#include <exception>
#include <string>

// A system #include is what drives the forest lane: the discovery chain opens
// on the FIRST grove-backed include, so a source without one never probes.
static const char *kSource =
    "#include <stdio.h>\n"
    "int __madc_eval() { return 7; }\n";

int main(int argc, char **argv)
{
	std::string leg = argc > 1 ? argv[1] : "default";

	madc::engine eng;
	madc::compile_options opts = eng.get_compile_options();
	if ( leg == "strict" || leg == "strict-noext" )
		opts.forest_missing = madc::forest_policy::strict_require;
	if ( leg == "strict-noext" )
		opts.enable_external_forest = false;
	else if ( leg != "default" && leg != "strict" )
	{
		std::fprintf(stderr, "unknown leg '%s'\n", leg.c_str());
		return 2;
	}
	eng.set_compile_options(opts);

	madc::program prog = eng.create_program();
	int64_t result = 0;
	bool ok = false;
	std::string diag;
	try
	{
		ok = prog.eval_unit(kSource, result, "forest_smoke.mad");
		if ( !ok )
		{
			const madc::error *e = prog.last_error();
			diag = e ? e->message : std::string("(no diagnostic)");
		}
	}
	catch ( const std::exception &e )
	{
		// A strict refusal raises through madc's Throw path, which has
		// already printed the real message to stderr.
		diag = e.what();
	}
	// Report through stdio, not iostreams: a live engine owns std::cout's
	// streambuf (host output capture), so a cout write here would vanish
	// into the engine's buffer instead of reaching the gate.
	if ( !ok )
	{
		std::printf("forest-refused: %s\n", diag.c_str());
		std::fflush(stdout);
		return 3;
	}
	if ( result != 7 )
	{
		std::fprintf(stderr, "unexpected result %lld\n", (long long)result);
		return 4;
	}
	std::printf("forest-bound\n");
	std::fflush(stdout);
	return 0;
}
