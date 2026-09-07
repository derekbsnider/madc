/* madc_capabilities.cpp — machine-readable CLI capability manifest.
 *
 * The manifest describes surfaces owned by the madc CLI itself. It is kept
 * separate from parser/runtime state so tooling can query a compiler without
 * providing a source file or loading a configuration file.
 */

#include <iostream>
#include <iomanip>
#include <stddef.h>

#include "madc_capabilities.h"

#ifndef MADC_VERSION_STR
#define MADC_VERSION_STR "0.0.0"
#endif

static void print_json_string(const char *value)
{
    std::cout << '"';
    if ( value )
    {
	for ( const unsigned char *p =
	      reinterpret_cast<const unsigned char *>(value); *p; ++p )
	{
	    switch ( *p )
	    {
	    case '"': std::cout << "\\\""; break;
	    case '\\': std::cout << "\\\\"; break;
	    case '\b': std::cout << "\\b"; break;
	    case '\f': std::cout << "\\f"; break;
	    case '\n': std::cout << "\\n"; break;
	    case '\r': std::cout << "\\r"; break;
	    case '\t': std::cout << "\\t"; break;
	    default:
		if ( *p < 0x20 )
		    std::cout << "\\u00" << std::hex << std::setw(2)
			      << std::setfill('0') << static_cast<unsigned int>(*p)
			      << std::dec << std::setfill(' ');
		else
		    std::cout << static_cast<char>(*p);
	    }
	}
    }
    std::cout << '"';
}

static void print_json_array(const char *const *values, size_t count)
{
    std::cout << '[';
    for ( size_t i = 0; i < count; ++i )
    {
	if ( i ) std::cout << ',';
	print_json_string(values[i]);
    }
    std::cout << ']';
}

void madc_print_capabilities_json()
{
#ifdef MADC_CROSS_TARGET
    const char *target = MADC_CROSS_TARGET;
    const char *execution = "emit-only";
    const char *jit = "false";
#else
    const char *target = "native";
    const char *execution = "jit-and-aot";
    const char *jit = "true";
#endif

    static const char *const c_standards[] = {
	"c78", "c86", "c88", "c89", "c94", "c99", "c11", "c17", "c23"
    };
    static const char *const cpp_standards[] = {
	"c++98", "c++03", "c++11", "c++14", "c++17", "c++20", "c++23", "c++26"
    };
    static const char *const emit_targets[] = { "c11", "mc11", "c++" };
    static const char *const native_outputs[] = {
	"object", "executable", "shared", "relocatable"
    };
    static const char *const introspection[] = {
	"dump-source", "dump-cir", "dump-nodes", "dump-cir-checked",
	"dump-forest", "dump-registered", "show-stats"
    };

    std::cout << "{\n"
	      << "  \"schema\":1,\n"
	      << "  \"compiler\":{\"name\":\"madc\",\"version\":";
    print_json_string(MADC_VERSION_STR);
    std::cout << ",\"target\":";
    print_json_string(target);
    std::cout << "},\n"
	      << "  \"input\":{\"dialects\":[\"madc\",\"c\",\"c++\"],\n"
	      << "    \"c_standards\":";
    print_json_array(c_standards, sizeof(c_standards) / sizeof(c_standards[0]));
    std::cout << ",\n    \"cpp_standards\":";
    print_json_array(cpp_standards, sizeof(cpp_standards) / sizeof(cpp_standards[0]));
    std::cout << ",\n    \"project_manifest\":true},\n"
	      << "  \"execution\":{\"mode\":";
    print_json_string(execution);
    std::cout << ",\"jit\":" << jit << ",\"aot\":true},\n"
	      << "  \"native_outputs\":";
    print_json_array(native_outputs, sizeof(native_outputs) / sizeof(native_outputs[0]));
    std::cout << ",\n"
	      << "  \"emit_targets\":";
    print_json_array(emit_targets, sizeof(emit_targets) / sizeof(emit_targets[0]));
    std::cout << ",\n"
	      << "  \"introspection\":";
    print_json_array(introspection, sizeof(introspection) / sizeof(introspection[0]));
    std::cout << ",\n"
	      << "  \"embedding\":{\"libmadc\":true,\"c_api\":true},\n"
	      << "  \"ir\":{\"frontend\":\"cir_node\",\"lowering\":\"c2mir\",\"backend\":\"MIR\"}\n"
	      << "}\n";
}
