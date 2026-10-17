// greet — the test module (tests/testmodule_import.mad): a madc module built
// into the object tree by src/madc_modules.mk. Its face (greet.madi) states
// what a program may call; these are the definitions, and the generated table
// (greet.inc, `madc --emit-module-table`) is what the import registers. The
// module binds libmadc's own symbols (madc::value, the registration API its
// builders call) at load, from the image that imported it.
//
// THREAD CONTRACT: hello's text is per thread; squares writes only its `out`.

#include "libmadc/value.h"
#include "madc_module.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace greet {

enum class tone : int { plain, loud };

const char *hello(const char *who, tone t)
{
	static thread_local std::string text;
	text = std::string("hello, ") + (who ? who : "");
	if ( t == tone::loud )
		for ( size_t i = 0; i < text.size(); ++i )
			if ( text[i] >= 'a' && text[i] <= 'z' )
				text[i] = (char)(text[i] - 'a' + 'A');
	return text.c_str();
}

madc::value &squares(madc::value &out, int64_t n)
{
	std::vector<madc::value> v;
	for ( int64_t i = 1; i <= n; ++i )
		v.push_back(madc::value(i * i));
	out = madc::value::make_array(v);
	return out;
}

} // namespace greet

#include "greet.inc"

MADC_MODULE_EXPORT const MadcModuleDescription *madc_module_greet(void)
{
	static const MadcModuleDescription d = {
		MADC_MODULE_ABI, "greet", 0u,
		madc_module_declarations, madc_module_ndeclarations,
		NULL, NULL
	};
	return &d;
}
