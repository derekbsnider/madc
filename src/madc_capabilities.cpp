/* madc_capabilities.cpp — machine-readable CLI capability manifest.
 *
 * `madc --capabilities=json` prints a versioned JSON description of what THIS
 * build accepts and produces, so tooling can query the compiler without a
 * source file, a Program, or a madc.ini. The manifest is built with the
 * in-tree JSON machinery (nlohmann::json, include/json.hpp) — the same one
 * madc_project.cpp and include/madcdis/web_model.h use — never hand-rolled
 * string escaping.
 *
 * The two lists most prone to drift are DERIVED, not re-typed here:
 *   - accepted C/C++ standards come from Program's one --std= table
 *     (Program::supported_c_standard_names / supported_cpp_standard_names),
 *     the single owner of the recognizer, so this manifest cannot advertise a
 *     standard the compiler rejects, or omit one it accepts (a hand-kept copy
 *     once dropped c95).
 *   - CIR emission targets come from CIR_EMIT_TARGETS — the same string the
 *     `--emit=` "unknown target" error cites — split on '|', never re-listed.
 *
 * native_outputs and introspection have NO shared registry to derive from (the
 * MadcNativeKind enum names its kinds, not their CLI -c/-o/-shared/-r
 * spellings; the --dump-* set is scattered arg handling), so they are
 * CLI-surface literals here by design: this file owns the CLI's own surface.
 */

#include "madc_capabilities.h"
#include "json.hpp"
#include "cir_emit_c.h"		// CIR_EMIT_TARGETS

#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"		// Program::supported_*_standard_names()

#include <iostream>
#include <string>
#include <vector>

// Same fallback the other version-consuming TUs carry: the build threads
// ../VERSION in as -DMADC_VERSION_STR, and this default only applies to an
// out-of-build compile.
#ifndef MADC_VERSION_STR
#define MADC_VERSION_STR "0.0.0"
#endif

using nlohmann::json;

// Split a '|'-separated list (CIR_EMIT_TARGETS) into its members, in order.
static std::vector<std::string> split_pipe(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for ( char c : s )
    {
	if ( c == '|' )
	{
	    out.push_back(cur);
	    cur.clear();
	}
	else
	    cur += c;
    }
    if ( !cur.empty() )
	out.push_back(cur);
    return out;
}

void madc_print_capabilities_json()
{
    json m;
    m["schema"] = 1;
    m["compiler"]["name"] = "madc";
    m["compiler"]["version"] = MADC_VERSION_STR;
#ifdef MADC_CROSS_TARGET
    // A cross artifact emits for its target and cannot run target code on this
    // host; the manifest describes THAT artifact, not the querying host.
    m["compiler"]["target"] = MADC_CROSS_TARGET;
    m["execution"]["mode"] = "emit-only";
    m["execution"]["jit"] = false;
#else
    m["compiler"]["target"] = "native";
    m["execution"]["mode"] = "jit-and-aot";
    m["execution"]["jit"] = true;
#endif
    m["execution"]["aot"] = true;

    m["input"]["dialects"] = { "madc", "c", "c++" };
    m["input"]["c_standards"] = Program::supported_c_standard_names();
    m["input"]["cpp_standards"] = Program::supported_cpp_standard_names();
    m["input"]["project_manifest"] = true;

    m["emit_targets"] = split_pipe(CIR_EMIT_TARGETS);

    // CLI-owned surfaces (no shared registry — see the file header).
    m["native_outputs"] = { "object", "executable", "shared", "relocatable" };
    m["introspection"] = { "dump-source", "dump-cir", "dump-nodes",
			   "dump-cir-checked", "dump-forest", "dump-registered",
			   "show-stats" };

    m["embedding"]["libmadc"] = true;
    m["embedding"]["c_api"] = true;
    m["ir"]["frontend"] = "cir_node";
    m["ir"]["lowering"] = "c2mir";
    m["ir"]["backend"] = "MIR";

    // nlohmann's default object keeps keys sorted; consumers (and the gate)
    // read by key, and every array above stays in the order built.
    std::cout << m.dump(2) << std::endl;
}
